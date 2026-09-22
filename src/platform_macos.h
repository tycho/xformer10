/****************************************************************************

    PLATFORM_MACOS.H

    - macOS-only process setup that has no SDL equivalent (platform_macos.m).
      Compiled with either UI backend.

****************************************************************************/

#ifndef PLATFORM_MACOS_H
#define PLATFORM_MACOS_H

#ifdef __APPLE__

/* Call once after SDL_Init(). Re-enables trackpad momentum scrolling, which
   SDL turns off for the process, and starts recording scroll events so
   MacOSPopScroll() can report them as AppKit delivered them. */
void MacOSPlatformInit(void);

/* Dequeue the AppKit scroll event behind the SDL_MOUSEWHEEL being handled
   (one per wheel event, in order). Returns 0 if none is recorded. precise
   is 1 for a trackpad or Magic Mouse, whose dy is in window points (momentum
   events included), 0 for a notched wheel, whose dy is in notches. */
int  MacOSPopScroll(int *precise, float *dy);

#endif /* __APPLE__ */
#endif /* PLATFORM_MACOS_H */
