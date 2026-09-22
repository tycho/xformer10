/****************************************************************************

    UI_COCOA.M

    - Native macOS backend of the UI layer (see ui.h): the application's
      menu bar (NSMenu) and NSOpenPanel / NSSavePanel file dialogs.

      The menus are built from the shared table in ui_common.c and inserted
      into the menu bar SDL creates at SDL_Init, between the application
      menu and SDL's "Window" menu. Enabled/checked state is refreshed from
      the live emulator state each time a menu opens (menuNeedsUpdate:).

      Menu items carry real key equivalents, so the SDL event loop must not
      dispatch those accelerators itself (UIMenuOwnsAccelerators).

    Copyright (C) 1991-2021 by Darek Mihocka. All Rights Reserved.
    Branch Always Software. http://www.emulators.com/

    This file is part of the Xformer project and subject to the MIT license terms
    in the LICENSE file found in the top-level directory of this distribution.
    No part of Xformer, including this file, may be copied, modified, propagated,
    or distributed except according to the terms contained in the LICENSE file.

****************************************************************************/

#if defined(SDL2_ENABLED) && defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "ui.h"

/* Target of every menu item, and delegate of every menu. */
@interface XFMenuController : NSObject <NSMenuDelegate>
@end

@implementation XFMenuController

- (void)menuAction:(NSMenuItem *)sender
{
    UIMenuCommand((int)sender.tag);
}

/* Refresh the whole menu just before it is shown. */
- (void)menuNeedsUpdate:(NSMenu *)menu
{
    for (NSMenuItem *mi in menu.itemArray) {
        const UIMenuItem *it = (const UIMenuItem *)[(NSValue *)mi.representedObject pointerValue];
        if (!it)
            continue;
        mi.enabled = UIMenuItemEnabled(it) ? YES : NO;
        if (it->idm > 0)
            mi.state = UIMenuItemChecked(it) ? NSControlStateValueOn : NSControlStateValueOff;
    }
}

@end

static XFMenuController *sController;
static NSMutableArray<NSMenuItem *> *sTopItems;

/* "Ctrl+F10" / "Alt+Enter" / "F5" / "Alt+S" -> Cocoa key equivalent */
static void ParseShortcut(const char *shortcut, NSString **key, NSEventModifierFlags *mods)
{
    *key  = @"";
    *mods = 0;
    if (!shortcut)
        return;

    char buf[64];
    snprintf(buf, sizeof buf, "%s", shortcut);
    char *save = NULL;
    for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        if      (!strcmp(tok, "Ctrl"))  *mods |= NSEventModifierFlagControl;
        else if (!strcmp(tok, "Alt"))   *mods |= NSEventModifierFlagOption;
        else if (!strcmp(tok, "Shift")) *mods |= NSEventModifierFlagShift;
        else if (!strcmp(tok, "Cmd"))   *mods |= NSEventModifierFlagCommand;
        else if (!strcmp(tok, "Enter")) *key = @"\r";
        else if (tok[0] == 'F' && tok[1] >= '1' && tok[1] <= '9') {
            int n = atoi(tok + 1);
            *key = [NSString stringWithFormat:@"%C", (unichar)(NSF1FunctionKey + n - 1)];
        } else {
            *key = [@(tok) lowercaseString];
        }
    }
}

static NSMenu *BuildMenu(const UIMenuDef *def, NSString *title)
{
    NSMenu *menu = [[NSMenu alloc] initWithTitle:title];
    menu.autoenablesItems = NO;
    menu.delegate = sController;

    for (int j = 0; j < def->count; j++) {
        const UIMenuItem *it = &def->items[j];
        if (!it->label) {
            [menu addItem:[NSMenuItem separatorItem]];
            continue;
        }
        const UIMenuDef *sub = UISubmenuFor(it->idm);
        NSString *key; NSEventModifierFlags mods;
        ParseShortcut(sub ? NULL : it->shortcut, &key, &mods);

        NSMenuItem *mi = [[NSMenuItem alloc] initWithTitle:@(it->label)
                                                    action:(sub ? nil : @selector(menuAction:))
                                             keyEquivalent:key];
        mi.keyEquivalentModifierMask = mods;
        mi.target = sController;
        mi.tag = it->idm;
        mi.representedObject = [NSValue valueWithPointer:it];
        if (sub)
            mi.submenu = BuildMenu(sub, @(sub->title));
        [menu addItem:mi];
    }
    return menu;
}

int UIMenuBarHeight(void)
{
    return 0;       /* the menu bar is the system one */
}

int UIMenuOwnsAccelerators(void)
{
    return 1;
}

void UIMenuFullscreenChanging(int entering) { (void)entering; }
void UIMenuFullscreenChanged(int entering)  { (void)entering; }

void UIMenuInit(SDL_Window *win, SDL_Renderer *ren)
{
    (void)win; (void)ren;

    @autoreleasepool {
        sController = [XFMenuController new];
        sTopItems   = [NSMutableArray new];

        NSMenu *bar = [NSApp mainMenu];
        if (!bar) {
            bar = [NSMenu new];
            [NSApp setMainMenu:bar];
        }

        /* after the application menu, before SDL's "Window" menu */
        NSInteger at = bar.numberOfItems > 0 ? 1 : 0;
        NSMenu *windowsMenu = [NSApp windowsMenu];
        for (NSInteger i = 0; i < bar.numberOfItems; i++) {
            if (windowsMenu && [bar itemAtIndex:i].submenu == windowsMenu) {
                at = i;
                break;
            }
        }

        for (int m = 0; m < UI_NUM_MENUS; m++) {
            const UIMenuDef *def = &kUIMenus[m];
            /* SDL already provides the standard "Window" menu */
            NSString *title = !strcmp(def->title, "Window") ? @"View" : @(def->title);
            NSMenuItem *top = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
            top.submenu = BuildMenu(def, title);
            [bar insertItem:top atIndex:at++];
            [sTopItems addObject:top];
            if (!strcmp(def->title, "Help"))
                [NSApp setHelpMenu:top.submenu];
        }
    }
}

void UIMenuQuit(void)
{
    @autoreleasepool {
        NSMenu *bar = [NSApp mainMenu];
        for (NSMenuItem *top in sTopItems)
            [bar removeItem:top];
        sTopItems   = nil;
        sController = nil;
    }
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

/* ----------------------------------------------------------------------
   File dialogs
   ---------------------------------------------------------------------- */

static NSArray<UTType *> *ContentTypes(const char *exts) API_AVAILABLE(macos(11.0))
{
    NSMutableArray<UTType *> *types = [NSMutableArray new];
    if (!exts)
        return types;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", exts);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
        if (*tok == '.') tok++;
        UTType *t = [UTType typeWithFilenameExtension:@(tok)];
        if (t) [types addObject:t];
    }
    return types;
}

int UIPlatformFileDialog(const UIFileDialogArgs *a, char *out, int sz)
{
    @autoreleasepool {
        NSSavePanel *panel;
        if (a->mode == UI_FILE_SAVE) {
            NSSavePanel *sp = [NSSavePanel savePanel];
            sp.canCreateDirectories = YES;
            sp.extensionHidden = NO;
            panel = sp;
        } else {
            NSOpenPanel *op = [NSOpenPanel openPanel];
            op.canChooseFiles         = (a->mode == UI_FILE_OPEN);
            op.canChooseDirectories   = (a->mode == UI_FILE_FOLDER);
            op.allowsMultipleSelection = NO;
            op.resolvesAliases        = YES;
            panel = op;
        }

        if (a->title) {
            panel.title   = @(a->title);
            panel.message = @(a->title);
        }

        if (a->exts && a->mode != UI_FILE_FOLDER) {
            if (@available(macOS 11.0, *)) {
                NSArray<UTType *> *types = ContentTypes(a->exts);
                if (types.count)
                    panel.allowedContentTypes = types;
            }
            panel.allowsOtherFileTypes = (a->mode == UI_FILE_OPEN);
        }

        if (a->start) {
            NSString *path = @(a->start);
            BOOL isDir = NO;
            if ([[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDir] && isDir) {
                panel.directoryURL = [NSURL fileURLWithPath:path isDirectory:YES];
            } else {
                panel.directoryURL = [NSURL fileURLWithPath:[path stringByDeletingLastPathComponent]
                                                isDirectory:YES];
                if (a->mode == UI_FILE_SAVE)
                    panel.nameFieldStringValue = [path lastPathComponent];
            }
        }

        if ([panel runModal] != NSModalResponseOK || !panel.URL)
            return 0;

        snprintf(out, sz, "%s", panel.URL.fileSystemRepresentation);
        return 1;
    }
}

#endif /* SDL2_ENABLED && __APPLE__ */
