/****************************************************************************

    UI_COMMON.C

    - Menu definition, live menu state and command dispatch shared by every
      UI backend (ui_sdl.c, ui_win32.c, ui_cocoa.m), plus the file dialog
      wrapper. See ui.h.

    Copyright (C) 1991-2021 by Darek Mihocka. All Rights Reserved.
    Branch Always Software. http://www.emulators.com/

    This file is part of the Xformer project and subject to the MIT license terms
    in the LICENSE file found in the top-level directory of this distribution.
    No part of Xformer, including this file, may be copied, modified, propagated,
    or distributed except according to the terms contained in the LICENSE file.

****************************************************************************/

#ifdef SDL2_ENABLED

#include <string.h>
#include <SDL2/SDL.h>
#include "gemtypes.h"
#ifdef _WIN32
#define strcasecmp _stricmp
#endif
#include "ddlib_sdl.h"
#include "ui.h"

extern void LinuxDoCommand(int idm);

int gMenuBarH = MENU_H_BASE;

/* ----------------------------------------------------------------------
   Menu table
   ---------------------------------------------------------------------- */

#define SEP  { NULL, NULL, 0, 0 }

static const UIMenuItem kFileItems[] = {
    {"Open Folder...",         NULL, IDM_OPENFOLDER, 0},
    {"Add Atari 800",          NULL, IDM_ADDVM1,     0},
    {"Add Atari 800XL",        NULL, IDM_ADDVM1 + 1, 0},
    {"Add Atari 130XE",        NULL, IDM_ADDVM1 + 2, 0},
    {"Delete VM",              NULL, IDM_DELVM,      1},
    SEP,
    {"New Session",            NULL, IDM_NEW,        0},
    {"Load Session...",        NULL, IDM_LOAD,       0},
    {"Save Session As...",     NULL, IDM_SAVEAS,     0},
    SEP,
    {"Restore Last Session",   NULL, IDM_AUTOLOAD,   0},
    SEP,
    {"Next VM",                NULL, IDM_NEXTVM,     1},
    {"Previous VM",            NULL, IDM_PREVVM,     1},
    SEP,
    {"Save Settings",          NULL, IDM_SAVEINI,    0},
    SEP,
    {"Exit",                   NULL, IDM_EXIT,       0},
};

static const UIMenuItem kVMItems[] = {
    {"Warm Start",       "F10",       IDM_WARMSTART,             1},
    {"Cold Start",       "Ctrl+F10",  IDM_COLDSTART,             1},
    {"Toggle BASIC",     "Shift+F10", IDM_TOGGLEBASIC,           1},
    {"Change Type",      "Alt+F10",   IDM_CHANGEVM,              1},
    SEP,
    {"Emulate PAL",      "Alt+F12",   IDM_NTSCPAL,               1},
    {"Switch Monitor",   "Shift+F12", IDM_COLORMONO,             1},
    SEP,
    {"Paste as ASCII",   NULL,        IDM_PASTEASCII,            1},
    {"Paste as ATASCII", NULL,        IDM_PASTEATASCII,          1},
    SEP,
    {"Time Travel",      NULL,        IDM_TIMETRAVEL,            1},
    {"Set Fix Point",    NULL,        IDM_TIMETRAVELFIXPOINT,    1},
    {"Use Fix Point",    NULL,        IDM_USETIMETRAVELFIXPOINT, 1},
};

static const UIMenuItem kWindowItems[] = {
    {"Full Screen",                  "Alt+Enter", IDM_FULLSCREEN,       0},
    {"Stretch Mode",                 "F12",       IDM_STRETCH,          0},
    {"Tile All VMs",                 "F5",        IDM_TILE,             0},
    SEP,
    {"Turbo Mode",                   "Alt+F1",    IDM_TURBO,            1},
    SEP,
    {"Auto-detect VM Type",          NULL,        IDM_AUTOKILL,         0},
    SEP,
    {"Disable L-CTRL as FIRE",       NULL,        IDM_LCTRLFIRE,        0},
    SEP,
    {"Mouse Wheel Sensitivity High", NULL,        IDM_WHEELSENS,        0},
    SEP,
    {"My Video Cards Sucks",         NULL,        IDM_MYVIDEOCARDSUCKS, 0},
    SEP,
    {"Enable Sound",                 "Alt+S",     IDM_TOGGLESOUND,      0},
};

static const UIMenuItem kDiskItems[] = {
    {"D1:",              NULL, UI_IDM_SUBMENU_D1, 1},
    {"D2:",              NULL, UI_IDM_SUBMENU_D2, 1},
    SEP,
    {"Cartridge...",     NULL, IDM_CART,          1},
    {"Remove Cartridge", NULL, IDM_NOCART,        1},
};

static const UIMenuItem kHelpItems[] = {
    {"Emulators.com Home Page", NULL, IDM_WEB_EMULATORS, 0},
    {"SoftMac/XFormer10 Repo",  NULL, IDM_WEB_FREE,      0},
    {"Online Documentation",    NULL, IDM_WEB_HELP,      0},
    SEP,
    {"About",                   NULL, IDM_ABOUT,         0},
};

static const UIMenuItem kD1Items[] = {
    {"Mount D1",          NULL, IDM_D1,         1},
    {"Unmount D1",        NULL, IDM_D1U,        1},
    SEP,
    {"Write Protect",     NULL, IDM_WP1,        1},
    {"Create Blank",      NULL, IDM_D1BLANKSD,  1},
    {"Extract DOS Files", NULL, IDM_IMPORTDOS1, 1},
};

static const UIMenuItem kD2Items[] = {
    {"Mount D2",          NULL, IDM_D2,         1},
    {"Unmount D2",        NULL, IDM_D2U,        1},
    SEP,
    {"Write Protect",     NULL, IDM_WP2,        1},
    {"Create Blank",      NULL, IDM_D2BLANKSD,  1},
    {"Extract DOS Files", NULL, IDM_IMPORTDOS2, 1},
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

const UIMenuDef kUIMenus[UI_NUM_MENUS] = {
    { "File",           kFileItems,   COUNT(kFileItems)   },
    { "VM",             kVMItems,     COUNT(kVMItems)     },
    { "Window",         kWindowItems, COUNT(kWindowItems) },
    { "Disk/Cartridge", kDiskItems,   COUNT(kDiskItems)   },
    { "Help",           kHelpItems,   COUNT(kHelpItems)   },
};

const UIMenuDef kUISubmenus[UI_NUM_SUBMENUS] = {
    { "D1:", kD1Items, COUNT(kD1Items) },
    { "D2:", kD2Items, COUNT(kD2Items) },
};

const UIMenuDef *UISubmenuFor(int idm)
{
    if (idm == UI_IDM_SUBMENU_D1) return &kUISubmenus[0];
    if (idm == UI_IDM_SUBMENU_D2) return &kUISubmenus[1];
    return NULL;
}

/* ----------------------------------------------------------------------
   Live state
   ---------------------------------------------------------------------- */

static int DiskMounted(int drive)
{
    return v.iVM >= 0 && rgpvm[v.iVM]->rgvd[drive].sz[0] != '\0';
}

int UIMenuItemEnabled(const UIMenuItem *it)
{
    if (it->idm == 0)
        return 0;                               /* separator */
    if (it->needsVM && v.iVM < 0)
        return 0;
    if (it->idm < 0)
        return 1;                               /* submenu parent */

    switch (it->idm) {
    case IDM_NOCART:
        return v.iVM >= 0 && rgpvm[v.iVM]->rgcart.fCartIn;
    case IDM_DELVM:
    case IDM_NEXTVM:
    case IDM_PREVVM:
        return v.cVM > 1;
    case IDM_STRETCH:
        return !v.fTiling;
    case IDM_TIMETRAVEL:
        return v.iVM >= 0 && rgpvm[v.iVM]->fTimeTravelEnabled;
    case IDM_D1U:
    case IDM_WP1:
    case IDM_IMPORTDOS1:
        return DiskMounted(0);
    case IDM_D2U:
    case IDM_WP2:
    case IDM_IMPORTDOS2:
        return DiskMounted(1);
    default:
        return 1;
    }
}

int UIMenuItemChecked(const UIMenuItem *it)
{
    switch (it->idm) {
    case IDM_TURBO:            return !fBrakes;
    case IDM_NTSCPAL:          return v.iVM >= 0 && rgpvm[v.iVM]->fEmuPAL;
    case IDM_AUTOLOAD:         return v.fSaveOnExit;
    case IDM_FULLSCREEN:       return v.fFullScreen;
    case IDM_STRETCH:          return v.fZoomColor;
    case IDM_TILE:             return v.fTiling;
    case IDM_AUTOKILL:         return v.fAutoKill;
    case IDM_LCTRLFIRE:        return v.fDisableLCTRLFire;
    case IDM_WHEELSENS:        return v.fWheelSensitive;
    case IDM_MYVIDEOCARDSUCKS: return v.fMyVideoCardSucks;
    case IDM_TOGGLESOUND:      return !v.fSilentMode;
    case IDM_USETIMETRAVELFIXPOINT:
        return v.iVM >= 0 && rgpvm[v.iVM]->fTimeTravelFixed;
    case IDM_WP1:
        return DiskMounted(0) && FWriteProtectDiskVM(v.iVM, 0, FALSE, FALSE);
    case IDM_WP2:
        return DiskMounted(1) && FWriteProtectDiskVM(v.iVM, 1, FALSE, FALSE);
    default:
        return 0;
    }
}

/* ----------------------------------------------------------------------
   Commands and accelerators
   ---------------------------------------------------------------------- */

void UIMenuCommand(int idm)
{
    switch (idm) {
    case IDM_EXIT:      vi.fQuitting = TRUE;    break;
    case IDM_WARMSTART: FWarmbootVM(v.iVM);     break;
    case IDM_COLDSTART: ColdStart(v.iVM);       break;
    default:            LinuxDoCommand(idm);    break;
    }
}

int UIAcceleratorCommand(SDL_Scancode sc, SDL_Keymod mod)
{
    int alt   = (mod & KMOD_ALT)   != 0;
    int shift = (mod & KMOD_SHIFT) != 0;
    int ctrl  = (mod & KMOD_CTRL)  != 0;

    switch (sc) {
    case SDL_SCANCODE_F5:
        return IDM_TILE;
    case SDL_SCANCODE_F1:
        return alt ? IDM_TURBO : 0;
    case SDL_SCANCODE_F10:
        if (alt)   return IDM_CHANGEVM;
        if (shift) return IDM_TOGGLEBASIC;
        if (ctrl)  return IDM_COLDSTART;
        return IDM_WARMSTART;
    case SDL_SCANCODE_F12:
        if (alt)   return IDM_NTSCPAL;
        if (shift) return IDM_COLORMONO;
        return IDM_STRETCH;
    case SDL_SCANCODE_RETURN:
        return alt ? IDM_FULLSCREEN : 0;
    case SDL_SCANCODE_S:
        return alt ? IDM_TOGGLESOUND : 0;
    default:
        return 0;
    }
}

void UICursorSync(int menuOpen)
{
    /* Show the cursor when the user needs it: a menu is open, the window
       isn't focused, the pointer is over the SDL menu bar, or we're in the
       tiled overview (where the mouse picks tiles). Only auto-hide it over
       the single-VM play area, so it stays out of the way during gameplay. */
    int show;
    SDL_Window *win = GetSDLWindow();
    if (menuOpen || !win || !(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) {
        show = 1;
    } else {
        int y;
        SDL_GetMouseState(NULL, &y);
        show = (y < MENU_H) || v.fTiling;
    }
    SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
}

/* ----------------------------------------------------------------------
   File dialogs
   ---------------------------------------------------------------------- */

int UIFileDialog(const UIFileDialogArgs *args, char *out, int sz)
{
    UIFileDialogArgs a = *args;
    if (a.start && !a.start[0])
        a.start = NULL;
    if (a.exts && !a.exts[0])
        a.exts = NULL;

    int wasShown = SDL_ShowCursor(SDL_QUERY);
    SDL_ShowCursor(SDL_ENABLE);

    int ok = UIPlatformFileDialog(&a, out, sz);

    SDL_ShowCursor(wasShown);
    if (ok)
        out[sz - 1] = '\0';
    return ok;
}

void UIEnsureExtension(char *path, int sz, const char *ext)
{
    size_t n = strlen(path), e = strlen(ext);
    if (n >= e && strcasecmp(path + n - e, ext) == 0)
        return;
    if ((int)(n + e) < sz)
        strcat(path, ext);
}

#endif /* SDL2_ENABLED */
