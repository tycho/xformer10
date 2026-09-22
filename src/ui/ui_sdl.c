/****************************************************************************

    UI_SDL.C

    - SDL backend of the UI layer (see ui.h): a menu bar drawn inside the
      SDL window with SDL_ttf, and file dialogs via the SDL file browser.
      Used on Linux, and on any platform with -DXFORMER_UI=sdl.

    Copyright (C) 1991-2021 by Darek Mihocka. All Rights Reserved.
    Branch Always Software. http://www.emulators.com/

    This file is part of the Xformer project and subject to the MIT license terms
    in the LICENSE file found in the top-level directory of this distribution.
    No part of Xformer, including this file, may be copied, modified, propagated,
    or distributed except according to the terms contained in the LICENSE file.

****************************************************************************/

#ifdef SDL2_ENABLED

#include <SDL2/SDL.h>
#include <SDL_ttf.h>
#include <stdio.h>
#include "gemtypes.h"
#include "ui.h"
#include "ddlib_sdl.h"
#include "font_sdl.h"
#include "sdl_filebrowser.h"

#define MAX_ITEMS      24
#define MAX_SUB_ITEMS  8

/* Layout metrics, in pixels. Initialized to 1.0x base values and multiplied by
   the UI scale in UIMenuInit(). The metric macros below expand to these
   variables so the layout code reads the scaled values directly. */
static int s_fontPx    = 14;
static int s_itemH     = 20;
static int s_sepH      = 8;
static int s_pad       = 8;
static int s_shortGap  = 16;
static int s_dropMinW  = 160;
static int s_checkArea = 16;   /* reserved width for checkmark left of label */

#define FONT_SIZE         s_fontPx
#define MENU_ITEM_H       s_itemH
#define MENU_SEP_H        s_sepH
#define MENU_PAD          s_pad
#define MENU_SHORTCUT_GAP s_shortGap
#define MENU_DROP_MIN_W   s_dropMinW
#define MENU_CHECK_AREA   s_checkArea

typedef struct {
    SDL_Texture *labelTex;
    int          labelW, labelH;
    SDL_Texture *shortTex;
    int          shortW, shortH;
} ItemTex;

static TTF_Font    *gFont         = NULL;
static float        gBacking      = 1.0f;   /* device pixels per window unit */
static SDL_Texture *gTopTex[UI_NUM_MENUS];
static int          gTopW[UI_NUM_MENUS];
static int          gTopH[UI_NUM_MENUS];
static int          gTopX[UI_NUM_MENUS];
static int          gDropW[UI_NUM_MENUS];
static ItemTex      gItemTex[UI_NUM_MENUS][MAX_ITEMS];
static ItemTex      gSubTex[UI_NUM_SUBMENUS][MAX_SUB_ITEMS];
static int          gSubDropW[UI_NUM_SUBMENUS];
static int          gSubParentMenu = -1;              /* menu holding the submenu parents */
static int          gSubParentRow[UI_NUM_SUBMENUS];   /* row of each parent in that menu */
static SDL_Texture *gCheckTex     = NULL;
static int          gCheckW       = 0;
static int          gCheckH       = 0;
static SDL_Texture *gArrowTex     = NULL;
static int          gArrowW       = 0;
static int          gArrowH       = 0;
static int          gMenuReady    = 0;
static int          gMenuOpen     = -1;
static int          gMenuHover    = -1;
static int          gSubmenuOpen  = -1;
static int          gSubmenuHover = -1;

static const UIMenuItem *Item(int m, int j)    { return &kUIMenus[m].items[j]; }
static const UIMenuItem *SubItem(int s, int j) { return &kUISubmenus[s].items[j]; }

/* Submenu index for a parent item, or -1 */
static int SubIndex(const UIMenuItem *it)
{
    if (it->idm == UI_IDM_SUBMENU_D1) return 0;
    if (it->idm == UI_IDM_SUBMENU_D2) return 1;
    return -1;
}

static int RowHeight(const UIMenuItem *it)
{
    return (it->idm == 0) ? MENU_SEP_H : MENU_ITEM_H;
}

/* Returns y offset of item j within dropdown m (relative to MENU_H) */
static int ItemYOffset(int m, int j)
{
    int yoff = 0;
    for (int k = 0; k < j; k++)
        yoff += RowHeight(Item(m, k));
    return yoff;
}

/* Returns bounding rect of open dropdown for menu m */
static SDL_Rect DropRect(int m)
{
    return (SDL_Rect){ gTopX[m], MENU_H, gDropW[m], ItemYOffset(m, kUIMenus[m].count) };
}

/* Returns total pixel height of submenu s */
static int SubDropHeight(int s)
{
    int h = 0;
    for (int j = 0; j < kUISubmenus[s].count; j++)
        h += RowHeight(SubItem(s, j));
    return h;
}

/* Returns bounding rect of submenu s flyout panel */
static SDL_Rect SubRect(int s)
{
    int m = gSubParentMenu;
    return (SDL_Rect){
        gTopX[m] + gDropW[m],
        MENU_H + ItemYOffset(m, gSubParentRow[s]),
        gSubDropW[s],
        SubDropHeight(s)
    };
}

static int InRect(const SDL_Rect *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* Returns index of the top-level label hit at x, or -1 */
static int HitTopLabel(int x)
{
    for (int i = 0; i < UI_NUM_MENUS; i++) {
        if (x >= gTopX[i] - 2 && x < gTopX[i] + gTopW[i] + 2)
            return i;
    }
    return -1;
}

/* Returns item index within dropdown m at pixel (x,y), or -1 */
static int HitItem(int m, int x, int y)
{
    SDL_Rect r = DropRect(m);
    if (!InRect(&r, x, y))
        return -1;
    int yrel = y - MENU_H;
    int yacc = 0;
    for (int j = 0; j < kUIMenus[m].count; j++) {
        int h = RowHeight(Item(m, j));
        if (yrel < yacc + h)
            return j;
        yacc += h;
    }
    return -1;
}

/* Returns item index within submenu s at pixel (x,y), or -1 */
static int HitSubItem(int s, int x, int y)
{
    SDL_Rect sr = SubRect(s);
    if (!InRect(&sr, x, y))
        return -1;
    int yrel = y - sr.y;
    int yacc = 0;
    for (int j = 0; j < kUISubmenus[s].count; j++) {
        int h = RowHeight(SubItem(s, j));
        if (yrel < yacc + h) return j;
        yacc += h;
    }
    return -1;
}

static void CloseMenus(void)
{
    gMenuOpen = -1; gMenuHover = -1;
    gSubmenuOpen = -1; gSubmenuHover = -1;
}

/* Rasterize text at device resolution (the font is opened at gBacking times
   the point size); w/h come back in window units so the caller lays out in
   points and the render scale maps the texture 1:1 onto device pixels. */
static SDL_Texture *RenderText(SDL_Renderer *ren, const char *text,
                               SDL_Color color, int *w, int *h)
{
    SDL_Surface *surf = TTF_RenderUTF8_Blended(gFont, text, color);
    if (!surf) { *w = *h = 0; return NULL; }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    *w = (int)(surf->w / gBacking + 0.5f);
    *h = (int)(surf->h / gBacking + 0.5f);
    SDL_FreeSurface(surf);
    return tex;
}

/* Rasterize an item's label/shortcut; returns the row width it needs. */
static int PrepareItem(SDL_Renderer *ren, const UIMenuItem *it, ItemTex *t)
{
    SDL_Color white = {255, 255, 255, 255};   /* white so color mod can dim it */
    if (it->idm == 0 || !it->label)
        return 0;
    t->labelTex = RenderText(ren, it->label, white, &t->labelW, &t->labelH);
    if (it->shortcut)
        t->shortTex = RenderText(ren, it->shortcut, white, &t->shortW, &t->shortH);
    int needed = MENU_PAD + MENU_CHECK_AREA + t->labelW
               + (it->shortcut ? MENU_SHORTCUT_GAP + t->shortW : 0)
               + MENU_PAD;
    if (SubIndex(it) >= 0)                     /* room for the flyout arrow */
        needed += gArrowW + MENU_PAD;
    return needed;
}

static void FreeItem(ItemTex *t)
{
    if (t->labelTex) { SDL_DestroyTexture(t->labelTex); t->labelTex = NULL; }
    if (t->shortTex) { SDL_DestroyTexture(t->shortTex); t->shortTex = NULL; }
}

int UIMenuBarHeight(void)
{
    return SDLUIScaled(MENU_H_BASE);
}

int UIMenuOwnsAccelerators(void)
{
    return 0;
}

void UIMenuFullscreenChanging(int entering) { (void)entering; }
void UIMenuFullscreenChanged(int entering)  { (void)entering; }

void UIMenuInit(SDL_Window *win, SDL_Renderer *ren)
{
    (void)win;

    /* scale all layout metrics for the display DPI (gMenuBarH was already set
       before the window was created, using the same scale) */
    s_fontPx    = SDLUIScaled(14);
    s_itemH     = SDLUIScaled(20);
    s_sepH      = SDLUIScaled(8);
    s_pad       = SDLUIScaled(8);
    s_shortGap  = SDLUIScaled(16);
    s_dropMinW  = SDLUIScaled(160);
    s_checkArea = SDLUIScaled(16);

    if (TTF_Init() < 0) {
        fprintf(stderr, "UIMenuInit: TTF_Init failed: %s\n", TTF_GetError());
        return;
    }
    const char *fontPath = SDLUIFontPath();
    if (!fontPath) {
        fprintf(stderr, "UIMenuInit: no usable UI font found "
                        "(install DejaVu/any sans font or set $XFORMER_FONT)\n");
        TTF_Quit();
        return;
    }
    gBacking = GetSDLBackingScale();
    gFont = TTF_OpenFont(fontPath, (int)(FONT_SIZE * gBacking + 0.5f));
    if (!gFont) {
        fprintf(stderr, "UIMenuInit: TTF_OpenFont(%s) failed: %s\n",
                fontPath, TTF_GetError());
        TTF_Quit();
        return;
    }

    SDL_Color fg    = {230, 230, 230, 255};
    SDL_Color white = {255, 255, 255, 255};

    /* Top-level labels, laid out left to right from their rendered widths */
    int xpos = 8;
    for (int i = 0; i < UI_NUM_MENUS; i++) {
        gTopTex[i] = RenderText(ren, kUIMenus[i].title, fg, &gTopW[i], &gTopH[i]);
        gTopX[i] = xpos;
        xpos += gTopW[i] + 20;
    }

    gCheckTex = RenderText(ren, "\xe2\x9c\x93", white, &gCheckW, &gCheckH); /* U+2713 */
    gArrowTex = RenderText(ren, "\xe2\x96\xb6", white, &gArrowW, &gArrowH); /* U+25B6 */

    /* Dropdown items */
    for (int m = 0; m < UI_NUM_MENUS; m++) {
        int maxW = 0;
        int count = kUIMenus[m].count;
        if (count > MAX_ITEMS) count = MAX_ITEMS;
        for (int j = 0; j < count; j++) {
            const UIMenuItem *it = Item(m, j);
            int needed = PrepareItem(ren, it, &gItemTex[m][j]);
            if (needed > maxW) maxW = needed;
            int s = SubIndex(it);
            if (s >= 0) {
                gSubParentMenu = m;
                gSubParentRow[s] = j;
            }
        }
        gDropW[m] = (maxW > MENU_DROP_MIN_W) ? maxW : MENU_DROP_MIN_W;
    }

    /* Submenu items */
    for (int s = 0; s < UI_NUM_SUBMENUS; s++) {
        int maxW = 0;
        int count = kUISubmenus[s].count;
        if (count > MAX_SUB_ITEMS) count = MAX_SUB_ITEMS;
        for (int j = 0; j < count; j++) {
            int needed = PrepareItem(ren, SubItem(s, j), &gSubTex[s][j]);
            if (needed > maxW) maxW = needed;
        }
        gSubDropW[s] = (maxW > MENU_DROP_MIN_W) ? maxW : MENU_DROP_MIN_W;
    }

    gMenuReady = 1;
}

void UIMenuQuit(void)
{
    if (!gMenuReady) return;
    for (int i = 0; i < UI_NUM_MENUS; i++) {
        if (gTopTex[i]) { SDL_DestroyTexture(gTopTex[i]); gTopTex[i] = NULL; }
        for (int j = 0; j < MAX_ITEMS; j++)
            FreeItem(&gItemTex[i][j]);
    }
    for (int s = 0; s < UI_NUM_SUBMENUS; s++)
        for (int j = 0; j < MAX_SUB_ITEMS; j++)
            FreeItem(&gSubTex[s][j]);
    if (gCheckTex) { SDL_DestroyTexture(gCheckTex); gCheckTex = NULL; }
    if (gArrowTex) { SDL_DestroyTexture(gArrowTex); gArrowTex = NULL; }
    if (gFont) { TTF_CloseFont(gFont); gFont = NULL; }
    TTF_Quit();
    gMenuReady = 0;
}

static void DrawSeparator(SDL_Renderer *ren, const SDL_Rect *panel, int y)
{
    SDL_SetRenderDrawColor(ren, 90, 90, 90, 255);
    SDL_RenderDrawLine(ren,
        panel->x + MENU_PAD,            y + MENU_SEP_H / 2,
        panel->x + panel->w - MENU_PAD, y + MENU_SEP_H / 2);
}

/* Draw one menu row: highlight, checkmark, label, flyout arrow, shortcut */
static void DrawRow(SDL_Renderer *ren, const SDL_Rect *panel, int y,
                    const UIMenuItem *it, const ItemTex *t,
                    int hovered)
{
    int itemH = MENU_ITEM_H;
    int enabled = UIMenuItemEnabled(it);
    int checked = UIMenuItemChecked(it);

    if (hovered && enabled) {
        SDL_SetRenderDrawColor(ren, 80, 110, 160, 255);
        SDL_Rect hlr = { panel->x + 1, y + 1, panel->w - 2, itemH - 2 };
        SDL_RenderFillRect(ren, &hlr);
    }

    Uint8 c = enabled ? 230 : 100;

    if (checked && gCheckTex) {
        SDL_SetTextureColorMod(gCheckTex, c, c, c);
        SDL_Rect dst = { panel->x + MENU_PAD, y + (itemH - gCheckH) / 2, gCheckW, gCheckH };
        SDL_RenderCopy(ren, gCheckTex, NULL, &dst);
    }
    if (t->labelTex) {
        SDL_SetTextureColorMod(t->labelTex, c, c, c);
        SDL_Rect dst = { panel->x + MENU_PAD + MENU_CHECK_AREA,
                         y + (itemH - t->labelH) / 2, t->labelW, t->labelH };
        SDL_RenderCopy(ren, t->labelTex, NULL, &dst);
    }
    if (SubIndex(it) >= 0 && gArrowTex) {
        SDL_SetTextureColorMod(gArrowTex, c, c, c);
        SDL_Rect dst = { panel->x + panel->w - gArrowW - MENU_PAD,
                         y + (itemH - gArrowH) / 2, gArrowW, gArrowH };
        SDL_RenderCopy(ren, gArrowTex, NULL, &dst);
    }
    if (t->shortTex) {
        SDL_SetTextureColorMod(t->shortTex, c, c, c);
        SDL_Rect dst = { panel->x + panel->w - t->shortW - MENU_PAD,
                         y + (itemH - t->shortH) / 2, t->shortW, t->shortH };
        SDL_RenderCopy(ren, t->shortTex, NULL, &dst);
    }
}

static void DrawPanelFrame(SDL_Renderer *ren, const SDL_Rect *r)
{
    SDL_SetRenderDrawColor(ren, 55, 55, 55, 255);
    SDL_RenderFillRect(ren, r);
    SDL_SetRenderDrawColor(ren, 100, 100, 100, 255);
    SDL_RenderDrawRect(ren, r);
}

void UIMenuRender(SDL_Renderer *ren)
{
    UICursorSync(gMenuOpen >= 0);
    if (!gMenuReady) return;

    int winW, winH;
    SDL_GetWindowSize(GetSDLWindow(), &winW, &winH);   /* window units */
    (void)winH;

    /* bar background */
    SDL_SetRenderDrawColor(ren, 50, 50, 50, 255);
    SDL_Rect bar = {0, 0, winW, MENU_H};
    SDL_RenderFillRect(ren, &bar);

    /* top-level labels */
    for (int i = 0; i < UI_NUM_MENUS; i++) {
        if (!gTopTex[i]) continue;
        SDL_Rect dst = { gTopX[i], (MENU_H - gTopH[i]) / 2, gTopW[i], gTopH[i] };
        SDL_RenderCopy(ren, gTopTex[i], NULL, &dst);
    }

    /* separator line at bottom of bar */
    SDL_SetRenderDrawColor(ren, 80, 80, 80, 255);
    SDL_RenderDrawLine(ren, 0, MENU_H - 1, winW - 1, MENU_H - 1);

    if (gMenuOpen < 0) return;

    /* dropdown panel */
    int m = gMenuOpen;
    SDL_Rect dr = DropRect(m);
    DrawPanelFrame(ren, &dr);

    int y = MENU_H;
    for (int j = 0; j < kUIMenus[m].count && j < MAX_ITEMS; j++) {
        const UIMenuItem *it = Item(m, j);
        if (it->idm == 0) {
            DrawSeparator(ren, &dr, y);
        } else {
            /* submenu parent rows stay highlighted while their flyout is open */
            int s = SubIndex(it);
            int hovered = (gMenuHover == j) || (s >= 0 && gSubmenuOpen == s);
            DrawRow(ren, &dr, y, it, &gItemTex[m][j], hovered);
        }
        y += RowHeight(it);
    }

    /* submenu flyout panel */
    if (m == gSubParentMenu && gSubmenuOpen >= 0) {
        int s = gSubmenuOpen;
        SDL_Rect sr = SubRect(s);
        DrawPanelFrame(ren, &sr);

        int sy = sr.y;
        for (int j = 0; j < kUISubmenus[s].count && j < MAX_SUB_ITEMS; j++) {
            const UIMenuItem *it = SubItem(s, j);
            if (it->idm == 0)
                DrawSeparator(ren, &sr, sy);
            else
                DrawRow(ren, &sr, sy, it, &gSubTex[s][j], gSubmenuHover == j);
            sy += RowHeight(it);
        }
    }
}

int UIMenuHandleEvent(const SDL_Event *e)
{
    if (!gMenuReady) return 0;

    switch (e->type) {
    case SDL_KEYDOWN:
        if (e->key.keysym.sym == SDLK_ESCAPE && gMenuOpen >= 0) {
            CloseMenus();
            return 1;
        }
        return 0;

    case SDL_MOUSEMOTION:
        if (gMenuOpen >= 0) {
            int x = e->motion.x, y = e->motion.y;
            /* if a submenu is open, check its area before the main dropdown */
            if (gMenuOpen == gSubParentMenu && gSubmenuOpen >= 0) {
                SDL_Rect sr = SubRect(gSubmenuOpen);
                if (InRect(&sr, x, y)) {
                    gSubmenuHover = HitSubItem(gSubmenuOpen, x, y);
                    /* keep gMenuHover on the parent row so it stays highlighted */
                    return 0;
                }
            }
            int j = HitItem(gMenuOpen, x, y);
            gMenuHover = j;
            if (gMenuOpen == gSubParentMenu) {
                int s = (j >= 0) ? SubIndex(Item(gMenuOpen, j)) : -1;
                gSubmenuOpen  = s;
                gSubmenuHover = -1;
            }
        }
        return 0;

    case SDL_MOUSEBUTTONDOWN:
        if (e->button.button != SDL_BUTTON_LEFT) return 0;
        {
            int x = e->button.x, y = e->button.y;

            if (y < MENU_H) {
                int hit = HitTopLabel(x);
                gSubmenuOpen  = -1;
                gSubmenuHover = -1;
                if (hit < 0) {
                    CloseMenus();
                    return 0;
                }
                gMenuOpen  = (gMenuOpen == hit) ? -1 : hit;
                gMenuHover = -1;
                return 1;
            }

            if (gMenuOpen >= 0) {
                /* check submenu flyout first */
                if (gMenuOpen == gSubParentMenu && gSubmenuOpen >= 0) {
                    SDL_Rect sr = SubRect(gSubmenuOpen);
                    if (InRect(&sr, x, y)) {
                        int j = HitSubItem(gSubmenuOpen, x, y);
                        int s = gSubmenuOpen;
                        CloseMenus();
                        if (j >= 0 && UIMenuItemEnabled(SubItem(s, j)))
                            UIMenuCommand(SubItem(s, j)->idm);
                        return 1;
                    }
                }

                SDL_Rect dr = DropRect(gMenuOpen);
                if (InRect(&dr, x, y)) {
                    int j = HitItem(gMenuOpen, x, y);
                    int m = gMenuOpen;

                    /* clicking a submenu parent opens its flyout without closing the menu */
                    if (j >= 0 && SubIndex(Item(m, j)) >= 0) {
                        gSubmenuOpen  = SubIndex(Item(m, j));
                        gMenuHover    = j;
                        gSubmenuHover = -1;
                        return 1;
                    }

                    CloseMenus();
                    if (j >= 0) {
                        const UIMenuItem *it = Item(m, j);
                        if (it->idm > 0 && UIMenuItemEnabled(it))
                            UIMenuCommand(it->idm);
                    }
                    return 1;
                }
                CloseMenus();
                return 0;
            }
        }
        return 0;

    default:
        return 0;
    }
}

/* ----------------------------------------------------------------------
   File dialogs: the SDL-drawn browser
   ---------------------------------------------------------------------- */

int UIPlatformFileDialog(const UIFileDialogArgs *a, char *out, int sz)
{
    /* the browser's modes match UI_FILE_OPEN/FOLDER/SAVE by value */
    return SDL_FileBrowserRunEx(GetSDLRenderer(), GetSDLWindow(),
                                a->start, out, sz, a->exts, a->mode);
}

#endif /* SDL2_ENABLED */
