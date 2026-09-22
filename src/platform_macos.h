/****************************************************************************

    PLATFORM_MACOS.H

    - macOS-only process setup that has no SDL equivalent (platform_macos.m).
      Compiled with either UI backend.

****************************************************************************/

#ifndef PLATFORM_MACOS_H
#define PLATFORM_MACOS_H

#ifdef __APPLE__

/* Call before SDL_Init(): enables trackpad momentum scrolling for the
   process, which SDL's own startup turns off. */
void MacOSPlatformPreInit(void);

/* Scroll handler: dy is in window points for a trackpad or Magic Mouse
   (precise = 1; momentum-phase events included), or in wheel notches for a
   notched mouse wheel (precise = 0). Same sign convention as SDL's wheel.y. */
typedef void (*MacOSScrollFn)(float dy, int precise);

/* Call after SDL_Init(): route the application's scroll events to fn as
   AppKit delivers them. SDL_MOUSEWHEEL must then be ignored on macOS: SDL
   forwards a trackpad's deltas rescaled to line units and drops the
   zero-delta gesture-phase events, so it can't be matched up reliably. */
void MacOSPlatformInit(MacOSScrollFn fn);

/* XF_RENDER_PROF diagnostic: log NSWindow / CAMetalLayer state to stderr. */
struct SDL_Window;
void MacOSDebugWindowState(struct SDL_Window *w);

#endif /* __APPLE__ */
#endif /* PLATFORM_MACOS_H */
