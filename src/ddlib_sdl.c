
/****************************************************************************

    DDLIB_SDL.C

    - SDL2 window + streaming texture replacing DirectDraw on Linux/Pi

    Copyright (C) 1991-2021 by Darek Mihocka. All Rights Reserved.
    Branch Always Software. http://www.emulators.com/

    This file is part of the Xformer project and subject to the MIT license terms
    in the LICENSE file found in the top-level directory of this distribution.
    No part of Xformer, including this file, may be copied, modified, propagated,
    or distributed except according to the terms contained in the LICENSE file.

****************************************************************************/

#ifndef _WIN32

/* SDL2/SDL.h must come before gemtypes.h to avoid __inline redefinition conflict with arm_neon.h */
#include <SDL2/SDL.h>
#include "gemtypes.h"
#include "atari800.h"
#include "menu_sdl.h"
#include "font_sdl.h"

static SDL_Window   *gSDLWin;
static SDL_Renderer *gSDLRen;
static SDL_Texture  *gSDLTex;
static int gTexW, gTexH;
static int gWinZoom = 3;   /* DPI-scaled integer zoom for the default window */

/* Per-tile textures for tiling mode — avoids single-texture update race */
#define MAX_TILE_TEX 64
static SDL_Texture *gTileTex[MAX_TILE_TEX];

extern BYTE rgbRainbow[];  /* atari800.c: interleaved [R,G,B] * 256, 6-bit values (0-63) */

void linux_set_window_title(const char *s)
{
    if (gSDLWin) SDL_SetWindowTitle(gSDLWin, s);
}

void linux_get_client_rect(RECT *r)
{
    int w = 0, h = 0;
    if (gSDLWin) SDL_GetWindowSize(gSDLWin, &w, &h);
    r->left = 0; r->top = 0;
    r->right = w; r->bottom = (h > MENU_H) ? h - MENU_H : 0;
}

BOOL InitDrawing(int dx, int dy, int bpp, HANDLE hwndApp, BOOL fReInit)
{
    (void)bpp; (void)hwndApp; (void)fReInit;

    /* HiDPI: scale the menu bar and default window zoom for the panel DPI.
       gMenuBarH must be set before the window is created — the height below and
       all later layout read it through the MENU_H macro. */
    float uiScale = SDLUIScale();
    gMenuBarH = (int)(MENU_H_BASE * uiScale + 0.5f);

    int zoom = (int)(3 * uiScale + 0.5f);
    if (zoom < 1) zoom = 1;
    /* keep the initial window within ~90% of the desktop */
    SDL_DisplayMode dmDesk;
    if (SDL_GetDesktopDisplayMode(0, &dmDesk) == 0) {
        while (zoom > 1 &&
               (dx * zoom > dmDesk.w * 9 / 10 ||
                dy * zoom + gMenuBarH > dmDesk.h * 9 / 10))
            zoom--;
    }
    gWinZoom = zoom;

    gSDLWin = SDL_CreateWindow("Xformer 10",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               dx * gWinZoom, dy * gWinZoom + MENU_H,
                               SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!gSDLWin) return FALSE;
    gSDLRen = SDL_CreateRenderer(gSDLWin, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!gSDLRen) return FALSE;
    gSDLTex = SDL_CreateTexture(gSDLRen,
                                SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING,
                                dx, dy);
    if (!gSDLTex) return FALSE;
    gTexW = dx;
    gTexH = dy;
    MenuInit(gSDLRen);
    return TRUE;
}

BYTE *LockSurface(int *pStride)
{
    void *pixels;
    int pitch;
    if (SDL_LockTexture(gSDLTex, NULL, &pixels, &pitch) != 0)
        return NULL;
    *pStride = pitch / 4;
    return (BYTE *)pixels;
}

void UnlockSurface(void)
{
    SDL_UnlockTexture(gSDLTex);
}

void ClearSurface(void)
{
    if (gSDLRen)
        SDL_RenderClear(gSDLRen);
}

void UninitDrawing(BOOL fFinal)
{
    if (fFinal)
    {
        MenuQuit();
        for (int i = 0; i < MAX_TILE_TEX; i++) {
            if (gTileTex[i]) { SDL_DestroyTexture(gTileTex[i]); gTileTex[i] = NULL; }
        }
        if (gSDLTex) { SDL_DestroyTexture(gSDLTex);   gSDLTex = NULL; }
        if (gSDLRen) { SDL_DestroyRenderer(gSDLRen);  gSDLRen = NULL; }
        if (gSDLWin) { SDL_DestroyWindow(gSDLWin);    gSDLWin = NULL; }
    }
}

void RenderBitmap_SDL(void)
{
    if (gSDLTex == NULL || gSDLRen == NULL) return;

    static Uint32 argbBuf[X8 * Y8];

    SDL_RenderClear(gSDLRen);

    if (v.fTiling && cThreads > 0) {
        for (int t = 0; t < cThreads; t++) {
            if (t >= vvmhw.numTiles) break;
            BYTE *src = (BYTE *)vvmhw.pbmTile[t].pvBits;
            if (!src) continue;

            /* Ensure a per-tile texture exists */
            if (t < MAX_TILE_TEX && !gTileTex[t]) {
                gTileTex[t] = SDL_CreateTexture(gSDLRen, SDL_PIXELFORMAT_ARGB8888,
                                                SDL_TEXTUREACCESS_STREAMING, gTexW, gTexH);
            }
            SDL_Texture *tex = (t < MAX_TILE_TEX && gTileTex[t]) ? gTileTex[t] : gSDLTex;

            for (int i = 0; i < gTexW * gTexH; i++) {
                BYTE p = src[i];
                BYTE r = rgbRainbow[p * 3    ];
                BYTE g = rgbRainbow[p * 3 + 1];
                BYTE b = rgbRainbow[p * 3 + 2];
                argbBuf[i] = (Uint32)0xFF000000
                    | (Uint32)((r << 2) | (r >> 4)) << 16
                    | (Uint32)((g << 2) | (g >> 4)) <<  8
                    | (Uint32)((b << 2) | (b >> 4));
            }
            SDL_UpdateTexture(tex, NULL, argbBuf, gTexW * 4);

            int slot = nFirstVisibleTile + t;
            int col  = (sTilesPerRow > 0) ? slot % sTilesPerRow : 0;
            int row  = (sTilesPerRow > 0) ? slot / sTilesPerRow : t;
            SDL_Rect dest = {col * gTexW,
                             MENU_H + v.sWheelOffset + row * gTexH,
                             gTexW, gTexH};
            SDL_RenderCopy(gSDLRen, tex, NULL, &dest);

            if (sVM >= 0 && slot == sVM) {
                SDL_SetRenderDrawColor(gSDLRen, 255, 255, 255, 255);
                SDL_RenderDrawRect(gSDLRen, &dest);
                SDL_SetRenderDrawColor(gSDLRen, 0, 0, 0, 255);
            }
        }
    } else {
        void *src = vvmhw.pbmTile[0].pvBits;

        if (src == NULL) goto done;

        for (int i = 0; i < gTexW * gTexH; i++) {
            BYTE p = ((BYTE *)src)[i];
            BYTE r = rgbRainbow[p * 3    ];
            BYTE g = rgbRainbow[p * 3 + 1];
            BYTE b = rgbRainbow[p * 3 + 2];
            argbBuf[i] = (Uint32)0xFF000000
                | (Uint32)((r << 2) | (r >> 4)) << 16
                | (Uint32)((g << 2) | (g >> 4)) <<  8
                | (Uint32)((b << 2) | (b >> 4));
        }
        SDL_UpdateTexture(gSDLTex, NULL, argbBuf, gTexW * 4);

        /* Fit the frame to the actual client area below the menu bar. This is
           used for windowed, fullscreen, and stretch alike, so the picture
           always tracks the real window size (gWinZoom only sets the initial
           size at creation) and is never clipped to the upper-left on a HiDPI
           or resized window. */
        SDL_Rect dest;
        {
            int winW, winH;
            SDL_GetWindowSize(gSDLWin, &winW, &winH);
            int availW = winW;
            int availH = winH - MENU_H;
            if (availH < 1) availH = 1;
            if (v.fZoomColor) {
                dest = (SDL_Rect){0, MENU_H, availW, availH};   /* stretch */
            } else {                                            /* aspect-fit */
                int scaledW = availH * gTexW / gTexH;
                if (scaledW <= availW)
                    dest = (SDL_Rect){(availW - scaledW) / 2, MENU_H, scaledW, availH};
                else {
                    int scaledH = availW * gTexH / gTexW;
                    dest = (SDL_Rect){0, MENU_H + (availH - scaledH) / 2, availW, scaledH};
                }
            }
        }
        SDL_RenderCopy(gSDLRen, gSDLTex, NULL, &dest);
    }
done:
    MenuRender(gSDLRen);
    SDL_RenderPresent(gSDLRen);
}

SDL_Renderer *GetSDLRenderer(void) { return gSDLRen; }
SDL_Window   *GetSDLWindow(void)   { return gSDLWin; }

#endif /* !_WIN32 */
