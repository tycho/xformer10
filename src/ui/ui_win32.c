/****************************************************************************

    UI_WIN32.C

    - Native Windows backend of the UI layer (see ui.h): a real menu bar
      attached to the SDL window's HWND, and the common file dialogs.

      The menu is built from the shared table in ui_common.c. SDL owns the
      window procedure, so menu messages are observed through
      SDL_SetWindowsMessageHook(): WM_INITMENU refreshes enabled/checked
      state from the live emulator state, WM_COMMAND dispatches the command.

    Copyright (C) 1991-2021 by Darek Mihocka. All Rights Reserved.
    Branch Always Software. http://www.emulators.com/

    This file is part of the Xformer project and subject to the MIT license terms
    in the LICENSE file found in the top-level directory of this distribution.
    No part of Xformer, including this file, may be copied, modified, propagated,
    or distributed except according to the terms contained in the LICENSE file.

****************************************************************************/

#if defined(SDL2_ENABLED) && defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <initguid.h>       /* define CLSID_FileOpenDialog etc. in this TU */
#include <commdlg.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include "ui.h"

#define MAX_TRACKED 96

/* Every non-separator item, remembered by (parent popup, position) so its
   state can be refreshed by position -- submenu parents have no command id. */
typedef struct {
    HMENU             parent;
    UINT              pos;
    const UIMenuItem *it;
} TrackedItem;

static SDL_Window  *sWin;
static HWND         sHwnd;
static HMENU        sMenu;
static TrackedItem  sTracked[MAX_TRACKED];
static int          sTrackedCount;

static void Track(HMENU parent, UINT pos, const UIMenuItem *it)
{
    if (sTrackedCount < MAX_TRACKED) {
        sTracked[sTrackedCount].parent = parent;
        sTracked[sTrackedCount].pos    = pos;
        sTracked[sTrackedCount].it     = it;
        sTrackedCount++;
    }
}

static HMENU BuildPopup(const UIMenuDef *def)
{
    HMENU h = CreatePopupMenu();
    for (int j = 0; j < def->count; j++) {
        const UIMenuItem *it = &def->items[j];
        if (!it->label) {
            AppendMenuA(h, MF_SEPARATOR, 0, NULL);
            continue;
        }
        const UIMenuDef *sub = UISubmenuFor(it->idm);
        if (sub) {
            AppendMenuA(h, MF_STRING | MF_POPUP, (UINT_PTR)BuildPopup(sub), it->label);
        } else {
            char text[160];
            if (it->shortcut)
                snprintf(text, sizeof text, "%s\t%s", it->label, it->shortcut);
            else
                snprintf(text, sizeof text, "%s", it->label);
            AppendMenuA(h, MF_STRING, (UINT_PTR)it->idm, text);
        }
        Track(h, (UINT)j, it);
    }
    return h;
}

static void SyncState(void)
{
    for (int i = 0; i < sTrackedCount; i++) {
        const TrackedItem *t = &sTracked[i];
        int enabled = UIMenuItemEnabled(t->it);
        EnableMenuItem(t->parent, t->pos,
                       MF_BYPOSITION | (enabled ? MF_ENABLED : MF_GRAYED));
        if (t->it->idm > 0)
            CheckMenuItem(t->parent, t->pos,
                          MF_BYPOSITION | (UIMenuItemChecked(t->it) ? MF_CHECKED : MF_UNCHECKED));
    }
}

static void SDLCALL MessageHook(void *userdata, void *hwnd, unsigned int msg,
                                Uint64 wParam, Sint64 lParam)
{
    (void)userdata;
    if ((HWND)hwnd != sHwnd)
        return;
    if (msg == WM_INITMENU) {
        SyncState();
    } else if (msg == WM_COMMAND && HIWORD(wParam) == 0 && lParam == 0) {
        /* menu item (not an accelerator or control notification) */
        UIMenuCommand((int)LOWORD(wParam));
    }
}

int UIMenuBarHeight(void)
{
    return 0;       /* the menu bar lives in the non-client area */
}

int UIMenuOwnsAccelerators(void)
{
    return 0;       /* keys are handled by the SDL event loop */
}

void UIMenuInit(SDL_Window *win, SDL_Renderer *ren)
{
    (void)ren;

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) {
        fprintf(stderr, "UIMenuInit: no Win32 window handle: %s\n", SDL_GetError());
        return;
    }
    sWin  = win;
    sHwnd = info.info.win.window;

    sMenu = CreateMenu();
    for (int m = 0; m < UI_NUM_MENUS; m++)
        AppendMenuA(sMenu, MF_STRING | MF_POPUP,
                    (UINT_PTR)BuildPopup(&kUIMenus[m]), kUIMenus[m].title);
    SyncState();

#ifdef SDL_HINT_WINDOWS_ENABLE_MENU_MNEMONICS
    /* let a bare Alt press activate the menu bar, as on any Windows app */
    SDL_SetHint(SDL_HINT_WINDOWS_ENABLE_MENU_MNEMONICS, "1");
#endif
    SDL_SetWindowsMessageHook(MessageHook, NULL);

    /* Attaching the menu takes its height out of the client area; put the
       client area back to the size SDL created (SDL accounts for the menu
       through GetMenu() when it converts client to window size). */
    int w, h;
    SDL_GetWindowSize(win, &w, &h);
    SetMenu(sHwnd, sMenu);
    DrawMenuBar(sHwnd);
    SDL_SetWindowSize(win, w, h);
}

void UIMenuQuit(void)
{
    SDL_SetWindowsMessageHook(NULL, NULL);
    if (sHwnd && sMenu && GetMenu(sHwnd) == sMenu)
        SetMenu(sHwnd, NULL);
    if (sMenu) { DestroyMenu(sMenu); sMenu = NULL; }
    sTrackedCount = 0;
    sHwnd = NULL;
    sWin  = NULL;
}

void UIMenuRender(SDL_Renderer *ren)
{
    (void)ren;
    UICursorSync(0);
}

int UIMenuHandleEvent(const SDL_Event *e)
{
    (void)e;
    return 0;
}

/* A borderless fullscreen window would still show the menu bar and push the
   picture down by its height, so detach it for the duration. Detaching
   before SDL_SetWindowFullscreen() grows the client area by the bar height,
   which SDL records as the windowed size; re-attaching after leaving takes
   the same height back, so the window returns to its original size. */
void UIMenuFullscreenChanging(int entering)
{
    if (entering && sHwnd && sMenu)
        SetMenu(sHwnd, NULL);
}

void UIMenuFullscreenChanged(int entering)
{
    if (!entering && sHwnd && sMenu) {
        SetMenu(sHwnd, sMenu);
        DrawMenuBar(sHwnd);
    }
}

/* ----------------------------------------------------------------------
   File dialogs
   ---------------------------------------------------------------------- */

static int IsDirectory(const char *path)
{
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* "Name\0*.atr;*.xfd\0All Files\0*.*\0\0" from ".atr,.xfd" */
static void BuildFilter(const UIFileDialogArgs *a, char *buf, int cb)
{
    int n = 0;
    if (a->exts) {
        n += snprintf(buf + n, cb - n, "%s", a->filterName ? a->filterName : "Supported Files");
        buf[n++] = '\0';
        const char *p = a->exts;
        int first = 1;
        while (*p && n < cb - 8) {
            while (*p == ',' || *p == ' ') p++;
            if (!*p) break;
            if (!first) buf[n++] = ';';
            first = 0;
            buf[n++] = '*';
            if (*p != '.') buf[n++] = '.';
            while (*p && *p != ',' && n < cb - 8) buf[n++] = *p++;
        }
        buf[n++] = '\0';
    }
    n += snprintf(buf + n, cb - n, "All Files");
    buf[n++] = '\0';
    n += snprintf(buf + n, cb - n, "*.*");
    buf[n++] = '\0';
    buf[n++] = '\0';
}

/* first extension without its dot, for lpstrDefExt */
static void DefaultExt(const char *exts, char *buf, int cb)
{
    buf[0] = '\0';
    if (!exts) return;
    const char *p = exts;
    while (*p == '.' || *p == ' ') p++;
    int n = 0;
    while (*p && *p != ',' && n < cb - 1) buf[n++] = *p++;
    buf[n] = '\0';
}

static int PickFolder(const UIFileDialogArgs *a, char *out, int sz)
{
    int ok = 0;
    HRESULT hrInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog *pfd = NULL;

    if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IFileOpenDialog, (void **)&pfd))) {
        FILEOPENDIALOGOPTIONS opts = 0;
        IFileOpenDialog_GetOptions(pfd, &opts);
        IFileOpenDialog_SetOptions(pfd, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

        if (a->title) {
            WCHAR wtitle[256];
            if (MultiByteToWideChar(CP_ACP, 0, a->title, -1, wtitle, 256))
                IFileOpenDialog_SetTitle(pfd, wtitle);
        }
        if (a->start) {
            WCHAR wpath[MAX_PATH];
            IShellItem *folder = NULL;
            if (MultiByteToWideChar(CP_ACP, 0, a->start, -1, wpath, MAX_PATH) &&
                SUCCEEDED(SHCreateItemFromParsingName(wpath, NULL, &IID_IShellItem, (void **)&folder))) {
                IFileOpenDialog_SetFolder(pfd, folder);
                IShellItem_Release(folder);
            }
        }

        if (SUCCEEDED(IFileOpenDialog_Show(pfd, sHwnd))) {
            IShellItem *item = NULL;
            if (SUCCEEDED(IFileOpenDialog_GetResult(pfd, &item))) {
                PWSTR wsz = NULL;
                if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &wsz))) {
                    if (WideCharToMultiByte(CP_ACP, 0, wsz, -1, out, sz, NULL, NULL))
                        ok = 1;
                    CoTaskMemFree(wsz);
                }
                IShellItem_Release(item);
            }
        }
        IFileOpenDialog_Release(pfd);
    }

    if (SUCCEEDED(hrInit))
        CoUninitialize();
    return ok;
}

int UIPlatformFileDialog(const UIFileDialogArgs *a, char *out, int sz)
{
    if (a->mode == UI_FILE_FOLDER)
        return PickFolder(a, out, sz);

    char file[MAX_PATH] = "";
    const char *initialDir = NULL;
    if (a->start) {
        if (IsDirectory(a->start))
            initialDir = a->start;
        else
            snprintf(file, sizeof file, "%s", a->start);
    }

    char filter[512];
    char defExt[16];
    BuildFilter(a, filter, sizeof filter);
    DefaultExt(a->exts, defExt, sizeof defExt);

    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize     = sizeof ofn;
    ofn.hwndOwner       = sHwnd;
    ofn.lpstrFilter     = filter;
    ofn.nFilterIndex    = 1;
    ofn.lpstrFile       = file;
    ofn.nMaxFile        = sizeof file;
    ofn.lpstrInitialDir = initialDir;
    ofn.lpstrTitle      = a->title;
    ofn.lpstrDefExt     = defExt[0] ? defExt : NULL;
    ofn.Flags           = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST |
                          (a->mode == UI_FILE_SAVE ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);

    BOOL ok = (a->mode == UI_FILE_SAVE) ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn);
    if (!ok)
        return 0;

    snprintf(out, sz, "%s", file);
    return 1;
}

#endif /* SDL2_ENABLED && _WIN32 */
