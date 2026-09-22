/****************************************************************************

    UI.H

    - Platform UI layer for the SDL2 port: the menu bar and file dialogs.

    Three interchangeable backends implement the "menu backend" and "file
    dialog backend" sections below:

        ui_sdl.c     menu bar drawn inside the SDL window with SDL_ttf, file
                     browser drawn with SDL (sdl_filebrowser.c). Any platform.
        ui_win32.c   native Win32 menu bar (SetMenu on the SDL window's HWND)
                     and the common file dialogs.
        ui_cocoa.m   native macOS menu bar (NSMenu) and NSOpenPanel/NSSavePanel.

    The menu *definition* (labels, shortcuts, command ids), the live
    enabled/checked state and command dispatch are shared by all three and
    live in ui_common.c, so the backends only translate the table into their
    widget set.

    This header is included from Objective-C, so it must stay free of
    gemtypes.h / Win32 types: SDL and standard C only.

****************************************************************************/

#ifndef XF_UI_H
#define XF_UI_H

#ifdef SDL2_ENABLED

#include <SDL2/SDL.h>

/* ======================================================================
   Menu definition (ui_common.c)
   ====================================================================== */

/* idm values for submenu parents; real commands are positive IDM_* ids */
#define UI_IDM_SUBMENU_D1   (-10)
#define UI_IDM_SUBMENU_D2   (-11)

typedef struct {
    const char *label;      /* NULL = separator */
    const char *shortcut;   /* accelerator as shown to the user ("Alt+Enter",
                               "Ctrl+F10", "F5"); NULL if none */
    int         idm;        /* IDM_* command, 0 = separator,
                               UI_IDM_SUBMENU_* = parent of a submenu */
    int         needsVM;    /* disabled while no VM is selected */
} UIMenuItem;

typedef struct {
    const char       *title;
    const UIMenuItem *items;
    int               count;
} UIMenuDef;

#define UI_NUM_MENUS     5      /* File, VM, Window, Disk/Cartridge, Help */
#define UI_NUM_SUBMENUS  2      /* D1:, D2: */

extern const UIMenuDef kUIMenus[UI_NUM_MENUS];
extern const UIMenuDef kUISubmenus[UI_NUM_SUBMENUS];

/* Submenu definition for a parent item's idm, or NULL if it is not one. */
const UIMenuDef *UISubmenuFor(int idm);

/* Live state: whether an item is currently selectable / shows a checkmark. */
int  UIMenuItemEnabled(const UIMenuItem *it);
int  UIMenuItemChecked(const UIMenuItem *it);

/* Run a menu command. */
void UIMenuCommand(int idm);

/* Command bound to a keyboard accelerator (the "shortcut" column of the
   menu table), or 0 if the key is not an accelerator. */
int  UIAcceleratorCommand(SDL_Scancode sc, SDL_Keymod mod);

/* Show/hide the mouse cursor: visible while a menu is open, the window is
   unfocused, the pointer is over the (SDL) menu bar, or in the tiled
   overview; hidden over the play area. Called once per rendered frame. */
void UICursorSync(int menuOpen);

/* ======================================================================
   Menu backend (ui_sdl.c / ui_win32.c / ui_cocoa.m)
   ====================================================================== */

/* Height in pixels the backend reserves at the top of the SDL window. The
   SDL backend draws its bar there; native backends reserve nothing. Set by
   InitDrawing() before the window is created; every layout site reads it
   through MENU_H. */
#define MENU_H_BASE 22          /* unscaled bar height of the SDL backend */
extern int gMenuBarH;
#define MENU_H gMenuBarH

int  UIMenuBarHeight(void);                            /* value for gMenuBarH */
void UIMenuInit(SDL_Window *win, SDL_Renderer *ren);   /* after window creation */
void UIMenuQuit(void);
void UIMenuRender(SDL_Renderer *ren);                  /* once per frame */
int  UIMenuHandleEvent(const SDL_Event *e);            /* 1 = event consumed */

/* 1 if the backend's own menu fires accelerators (Cocoa key equivalents),
   in which case the SDL event loop must not dispatch them a second time. */
int  UIMenuOwnsAccelerators(void);

/* Bracket SDL_SetWindowFullscreen(): the Win32 backend detaches its menu
   bar before entering fullscreen and re-attaches it after leaving. */
void UIMenuFullscreenChanging(int entering);
void UIMenuFullscreenChanged(int entering);

/* ======================================================================
   File dialogs
   ====================================================================== */

enum {
    UI_FILE_OPEN   = 0,     /* pick an existing file */
    UI_FILE_FOLDER = 1,     /* pick a directory */
    UI_FILE_SAVE   = 2,     /* choose a new or existing file name */
};

typedef struct {
    int         mode;        /* UI_FILE_* */
    const char *title;       /* dialog title / prompt, or NULL */
    const char *filterName;  /* "Atari Disk Images" (shown by Win32), or NULL */
    const char *exts;        /* ".atr,.atx,.xfd"; NULL or "" = any file */
    const char *start;       /* initial file or directory, or NULL */
} UIFileDialogArgs;

/* Run a modal dialog. Returns 1 and the chosen path in out[0..sz-1], or 0 if
   cancelled. ui_common.c wraps the backend to show the cursor meanwhile. */
int  UIFileDialog(const UIFileDialogArgs *args, char *out, int sz);
int  UIPlatformFileDialog(const UIFileDialogArgs *args, char *out, int sz);

/* Append ext (".gem") to path unless it already ends with it. */
void UIEnsureExtension(char *path, int sz, const char *ext);

#endif /* SDL2_ENABLED */
#endif /* XF_UI_H */
