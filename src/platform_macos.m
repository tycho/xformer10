/****************************************************************************

    PLATFORM_MACOS.M

    - macOS process setup outside SDL's reach. See platform_macos.h.

****************************************************************************/

#if defined(SDL2_ENABLED) && defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#include "platform_macos.h"

static MacOSScrollFn sScrollFn;

void MacOSPlatformPreInit(void)
{
    @autoreleasepool {
        /* SDL_Init registers AppleMomentumScrollSupported = NO, which makes
           AppKit stop a scroll dead when the fingers leave the trackpad.
           The argument domain outranks the registration domain and is not
           persisted, so YES there wins whenever AppKit reads the key. AppKit
           then keeps delivering scroll events with decaying deltas during
           the momentum phase. */
        NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
        NSMutableDictionary *args = [[ud volatileDomainForName:NSArgumentDomain] mutableCopy];
        if (!args)
            args = [NSMutableDictionary new];
        args[@"AppleMomentumScrollSupported"] = @YES;
        [ud setVolatileDomain:args forName:NSArgumentDomain];
    }
}

void MacOSPlatformInit(MacOSScrollFn fn)
{
    sScrollFn = fn;

    @autoreleasepool {
        [[NSUserDefaults standardUserDefaults] registerDefaults:@{
            @"AppleMomentumScrollSupported": @YES
        }];

        /* Local monitors run on the main thread as each event is dequeued,
           i.e. from inside SDL's event pump between frames, so the handler
           may touch emulator state. Nothing is delivered while a modal
           panel (file dialog) owns the run loop. */
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
                                              handler:^NSEvent *(NSEvent *ev) {
            if (sScrollFn && ev.window && [NSApp modalWindow] == nil) {
                if (ev.hasPreciseScrollingDeltas) {
                    if (ev.scrollingDeltaY != 0.0)
                        sScrollFn((float)ev.scrollingDeltaY, 1);
                } else if (ev.deltaY != 0.0) {
                    /* whole notches, as SDL reports them for a discrete wheel */
                    float d = (float)ev.deltaY;
                    sScrollFn(d > 0 ? ceilf(d) : floorf(d), 0);
                }
            }
            return ev;
        }];
    }
}

#endif /* SDL2_ENABLED && __APPLE__ */
