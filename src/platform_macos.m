/****************************************************************************

    PLATFORM_MACOS.M

    - macOS process setup outside SDL's reach. See platform_macos.h.

****************************************************************************/

#if defined(SDL2_ENABLED) && defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#include "platform_macos.h"

/* Scroll deltas as AppKit reported them, in the order SDL will translate the
   same events. SDL exposes only deltaY, which for a trackpad is scrollingDeltaY
   rescaled to line units (about a tenth), so the pixel value is kept here.
   Everything runs on the main thread: no locking. */
#define SCROLL_RING 64
static struct { float dy; int precise; } sScroll[SCROLL_RING];
static unsigned sScrollHead, sScrollTail;

void MacOSPlatformInit(void)
{
    @autoreleasepool {
        /* SDL_Init registers AppleMomentumScrollSupported = NO, which makes
           AppKit stop the scroll dead when the fingers leave the trackpad.
           A later registration for the same key wins, so put it back: AppKit
           then keeps delivering scroll events with decaying deltas during the
           momentum phase, which SDL passes through as ordinary wheel events. */
        [[NSUserDefaults standardUserDefaults] registerDefaults:@{
            @"AppleMomentumScrollSupported": @YES
        }];

        /* Monitors run when an event is dequeued, before SDL translates it,
           so the queue here fills in the same order as SDL's wheel events. */
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
                                              handler:^NSEvent *(NSEvent *ev) {
            if (sScrollHead - sScrollTail < SCROLL_RING) {
                sScroll[sScrollHead % SCROLL_RING].dy      = (float)ev.scrollingDeltaY;
                sScroll[sScrollHead % SCROLL_RING].precise = ev.hasPreciseScrollingDeltas ? 1 : 0;
                sScrollHead++;
            }
            return ev;
        }];
    }
}

int MacOSPopScroll(int *precise, float *dy)
{
    if (sScrollTail == sScrollHead)
        return 0;
    *dy      = sScroll[sScrollTail % SCROLL_RING].dy;
    *precise = sScroll[sScrollTail % SCROLL_RING].precise;
    sScrollTail++;
    return 1;
}

#endif /* SDL2_ENABLED && __APPLE__ */
