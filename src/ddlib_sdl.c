
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

/* Tiled-view compositing. Instead of converting + uploading + drawing each tile
   separately (N palette->ARGB conversions on the main thread, plus N texture
   uploads and N draw calls -- crippling at hundreds of tiles, especially over
   WSLg's virtualized GPU), we composite every visible tile into one client-area
   ARGB framebuffer and present it with a single UpdateTexture + RenderCopy --
   the SDL equivalent of the Windows build's single BitBlt. The per-tile
   conversion is split across a worker pool so it scales with cores. */
static Uint32      *gCompose;        /* client-area ARGB framebuffer (CPU side) */
static SDL_Texture *gComposeTex;     /* matching streaming texture (GPU side) */
static int          gComposeW, gComposeH;

/* 6-bit palette index -> 0xAARRGGBB, expanding each 6-bit channel to 8 bits. */
static inline Uint32 pal_to_argb(BYTE p)
{
    BYTE r = rgbRainbow[p * 3], g = rgbRainbow[p * 3 + 1], b = rgbRainbow[p * 3 + 2];
    return (Uint32)0xFF000000
         | (Uint32)((r << 2) | (r >> 4)) << 16
         | (Uint32)((g << 2) | (g >> 4)) <<  8
         | (Uint32)((b << 2) | (b >> 4));
}

/* Convert one gTexW x gTexH tile from 8-bit palette (src) into the compose
   framebuffer at client-area pixel (dx,dy), clipped to [0,gComposeW)x[0,gComposeH).
   Tiles never overlap, so this is safe to run concurrently across tiles. */
static void compose_tile(const BYTE *src, int dx, int dy)
{
    int ry0 = dy < 0 ? -dy : 0;
    int ry1 = (dy + gTexH > gComposeH) ? (gComposeH - dy) : gTexH;
    int cx0 = dx < 0 ? -dx : 0;
    int cx1 = (dx + gTexW > gComposeW) ? (gComposeW - dx) : gTexW;
    for (int ry = ry0; ry < ry1; ry++) {
        const BYTE *s = src + (size_t)ry * gTexW + cx0;
        Uint32 *d = gCompose + (size_t)(dy + ry) * gComposeW + (dx + cx0);
        for (int cx = cx0; cx < cx1; cx++)
            *d++ = pal_to_argb(*s++);
    }
}

/* (Re)allocate the compose framebuffer + texture for a client area of w x h. */
static BOOL compose_ensure(int w, int h)
{
    if (w < 1 || h < 1) return FALSE;
    if (gCompose && gComposeTex && w == gComposeW && h == gComposeH) return TRUE;

    Uint32 *buf = (Uint32 *)realloc(gCompose, (size_t)w * h * sizeof(Uint32));
    if (!buf) return FALSE;
    gCompose = buf;

    if (gComposeTex) { SDL_DestroyTexture(gComposeTex); gComposeTex = NULL; }
    gComposeTex = SDL_CreateTexture(gSDLRen, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!gComposeTex) return FALSE;

    gComposeW = w;
    gComposeH = h;
    return TRUE;
}

/* ---- compose worker pool ----------------------------------------------------
   The palette->ARGB conversion is the dominant CPU cost of the tiled view, and
   it parallelizes perfectly: each tile writes a disjoint region of the compose
   buffer. A small persistent pool drains a shared job queue (work-stealing via
   an atomic index) while the main thread joins in, so the conversion scales with
   cores. (The emulation worker threads are blocked waiting for the next frame
   during render, so the cores are free.) */
#define MAX_RENDER_WORKERS 32
typedef struct { const BYTE *src; int dx, dy; } ComposeJob;
static ComposeJob   *gJobs;
static int           gJobCap, gJobCount;
static SDL_atomic_t  gJobNext;
static SDL_sem      *gGoSem, *gDoneSem;
static SDL_Thread   *gWorkers[MAX_RENDER_WORKERS];
static int           gNumWorkers;
static volatile int  gWorkersQuit;

static void compose_drain(void)   /* run jobs until the queue is empty */
{
    int i;
    while ((i = SDL_AtomicAdd(&gJobNext, 1)) < gJobCount)
        compose_tile(gJobs[i].src, gJobs[i].dx, gJobs[i].dy);
}

static int compose_worker(void *arg)
{
    (void)arg;
    for (;;) {
        SDL_SemWait(gGoSem);
        if (gWorkersQuit) return 0;
        compose_drain();
        SDL_SemPost(gDoneSem);
    }
}

static void compose_pool_init(void)
{
    if (gGoSem) return;                       /* once */
    gGoSem   = SDL_CreateSemaphore(0);
    gDoneSem = SDL_CreateSemaphore(0);
    if (!gGoSem || !gDoneSem) return;         /* fall back to serial dispatch */
    int n = SDL_GetCPUCount() - 1;            /* + the main thread = all cores */
    if (n < 1) n = 1;
    if (n > MAX_RENDER_WORKERS) n = MAX_RENDER_WORKERS;
    for (int i = 0; i < n; i++) {
        gWorkers[i] = SDL_CreateThread(compose_worker, "compose", NULL);
        if (gWorkers[i]) gNumWorkers++;
    }
}

static void compose_pool_quit(void)
{
    if (gNumWorkers) {
        gWorkersQuit = 1;
        for (int i = 0; i < gNumWorkers; i++) SDL_SemPost(gGoSem);
        for (int i = 0; i < gNumWorkers; i++) SDL_WaitThread(gWorkers[i], NULL);
        gNumWorkers = 0;
    }
    if (gGoSem)   { SDL_DestroySemaphore(gGoSem);   gGoSem = NULL; }
    if (gDoneSem) { SDL_DestroySemaphore(gDoneSem); gDoneSem = NULL; }
    free(gJobs); gJobs = NULL; gJobCap = 0;
}

/* Convert all queued tiles into the compose buffer, in parallel when it pays. */
static void compose_dispatch(void)
{
    if (gJobCount <= 0) return;

    Uint64 t0 = SDL_GetPerformanceCounter();

    if (gNumWorkers == 0 || gJobCount < 4) {  /* not worth the sync overhead */
        for (int i = 0; i < gJobCount; i++)
            compose_tile(gJobs[i].src, gJobs[i].dx, gJobs[i].dy);
    } else {
        SDL_AtomicSet(&gJobNext, 0);
        for (int i = 0; i < gNumWorkers; i++) SDL_SemPost(gGoSem);
        compose_drain();                      /* the main thread pulls its share */
        for (int i = 0; i < gNumWorkers; i++) SDL_SemWait(gDoneSem);
    }

    /* set XF_RENDER_PROF=1 to log the tiled-view conversion cost once a second */
    static int prof = -1;
    if (prof < 0) prof = getenv("XF_RENDER_PROF") ? 1 : 0;
    if (prof) {
        static Uint64 last;
        Uint64 now = SDL_GetTicks64();
        if (now - last >= 1000) {
            last = now;
            double ms = (double)(SDL_GetPerformanceCounter() - t0)
                      * 1e3 / (double)SDL_GetPerformanceFrequency();
            fprintf(stderr, "[render] compose %d tiles in %.2f ms (%d workers)\n",
                    gJobCount, ms, gNumWorkers);
        }
    }
}

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
        compose_pool_quit();
        if (gComposeTex) { SDL_DestroyTexture(gComposeTex); gComposeTex = NULL; }
        free(gCompose); gCompose = NULL; gComposeW = gComposeH = 0;
        if (gSDLTex) { SDL_DestroyTexture(gSDLTex);   gSDLTex = NULL; }
        if (gSDLRen) { SDL_DestroyRenderer(gSDLRen);  gSDLRen = NULL; }
        if (gSDLWin) { SDL_DestroyWindow(gSDLWin);    gSDLWin = NULL; }
    }
}

void RenderBitmap_SDL(void)
{
    if (gSDLTex == NULL || gSDLRen == NULL) return;

    static Uint32 argbBuf[X8 * Y8];

    /* clear to black explicitly — the menu code leaves the draw color set to
       its bar/highlight color, which would otherwise tint the whole window */
    SDL_SetRenderDrawColor(gSDLRen, 0, 0, 0, 255);
    SDL_RenderClear(gSDLRen);

    if (v.fTiling) {
        /* Composite every visible tile into one client-area framebuffer, then
           present it with a single UpdateTexture + RenderCopy. */
        int winW, winH;
        SDL_GetWindowSize(gSDLWin, &winW, &winH);
        int clientH = (winH > MENU_H) ? winH - MENU_H : 0;

        if (compose_ensure(winW, clientH)) {
            memset(gCompose, 0, (size_t)gComposeW * gComposeH * sizeof(Uint32));

            /* Screen position of the first visible tile, in tile-slots. Threads are
               packed consecutively from this slot (matching GetTileFromPos), so this
               is correct whether or not a type-to-search filter is active — unlike
               nFirstVisibleTile, which holds a (scattered) VM index when searching. */
            int tileH = (int)sTileSize.y > 0 ? (int)sTileSize.y : gTexH;
            int firstSpot = (sTilesPerRow > 0)
                            ? (abs(v.sWheelOffset) / tileH) * sTilesPerRow : 0;

            int focusX = -1, focusY = 0;   /* hovered tile's client-area top-left */

            compose_pool_init();
            if (gJobCap < cThreads) {        /* grow the job list to fit */
                ComposeJob *nj = (ComposeJob *)realloc(gJobs, (size_t)cThreads * sizeof(ComposeJob));
                if (nj) { gJobs = nj; gJobCap = cThreads; }
            }
            gJobCount = 0;

            for (int t = 0; t < cThreads; t++) {
                if (t >= vvmhw.numTiles) break;
                BYTE *src = (BYTE *)vvmhw.pbmTile[t].pvBits;
                if (!src) continue;

                int slot = firstSpot + t;
                int col  = (sTilesPerRow > 0) ? slot % sTilesPerRow : 0;
                int row  = (sTilesPerRow > 0) ? slot / sTilesPerRow : t;
                int dx   = col * gTexW;
                int dy   = v.sWheelOffset + row * gTexH;   /* client coords, 0 = below menu */

                if (gJobCount < gJobCap) {
                    gJobs[gJobCount].src = src;
                    gJobs[gJobCount].dx  = dx;
                    gJobs[gJobCount].dy  = dy;
                    gJobCount++;
                } else {
                    compose_tile(src, dx, dy);   /* alloc shortfall: do it inline */
                }

                if (sVM >= 0 && ThreadStuff[t].iThreadVM == sVM) { focusX = dx; focusY = dy; }
            }

            compose_dispatch();                  /* parallel palette->ARGB conversion */

            SDL_UpdateTexture(gComposeTex, NULL, gCompose, gComposeW * (int)sizeof(Uint32));
            SDL_Rect dst = {0, MENU_H, gComposeW, gComposeH};
            SDL_RenderCopy(gSDLRen, gComposeTex, NULL, &dst);

            /* highlight the hovered tile (window coords) */
            if (focusX >= 0) {
                SDL_Rect fr = {focusX, MENU_H + focusY, gTexW, gTexH};
                SDL_SetRenderDrawColor(gSDLRen, 255, 255, 255, 255);
                SDL_RenderDrawRect(gSDLRen, &fr);
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

/* Toggle vsync on the existing renderer without recreating it (SDL >= 2.0.18).
   Used to let turbo mode out-run the display refresh; see IDM_TURBO. */
void SetSDLVSync(BOOL on)
{
    if (gSDLRen)
        SDL_RenderSetVSync(gSDLRen, on ? 1 : 0);
}

#endif /* !_WIN32 */
