/****************************************************************************

    PLATFORM_MACOS.M

    - macOS process setup outside SDL's reach. See platform_macos.h.

****************************************************************************/

#if defined(SDL2_ENABLED) && defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include "platform_macos.h"

static CAMetalLayer *find_metal_layer(NSView *root)
{
    NSMutableArray *stack = [NSMutableArray arrayWithObject:root];
    while (stack.count) {
        NSView *vw = stack.lastObject; [stack removeLastObject];
        if ([vw.layer isKindOfClass:[CAMetalLayer class]]) return (CAMetalLayer *)vw.layer;
        [stack addObjectsFromArray:vw.subviews];
    }
    return nil;
}

/* XF_RENDER_PROF diagnostic (see main_linux.c): dump the NSWindow and
   CAMetalLayer state once a second. Added while chasing a macOS 27 / M5
   report of "1000% speed, nothing drawn": SDL_RenderPresent stops blocking
   on vsync whenever the window is not being composited (launched behind
   another window and never activated, fully covered, or mostly off-screen),
   which makes the title-bar speed % -- measured across the present -- read
   ~1000% while the emulation itself stays correctly paced. */
void MacOSDebugWindowState(SDL_Window *w)
{
    SDL_SysWMinfo info; SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(w, &info)) return;
    NSWindow *nw = info.info.cocoa.window;
    CAMetalLayer *ml = find_metal_layer(nw.contentView);
    CALayer *layer = ml ? (CALayer *)ml : nw.contentView.layer;
    NSRect fr = nw.frame;
    fprintf(stderr, "[win] appActive=%d appOccl=%lu key=%d main=%d visible=%d onActiveSpace=%d winOccl=%lu "
                    "num=%ld frame=(%.0f,%.0f %.0fx%.0f) screen=%d layer=%s drawable=%.0fx%.0f sync=%d dev=%s\n",
            [NSApp isActive], (unsigned long)[NSApp occlusionState], nw.isKeyWindow, nw.isMainWindow,
            nw.isVisible, nw.isOnActiveSpace, (unsigned long)nw.occlusionState, (long)nw.windowNumber,
            fr.origin.x, fr.origin.y, fr.size.width, fr.size.height, nw.screen != nil,
            layer ? [NSStringFromClass([layer class]) UTF8String] : "none",
            ml ? ml.drawableSize.width : 0, ml ? ml.drawableSize.height : 0,
            ml ? (int)ml.displaySyncEnabled : -1, ml && ml.device ? [ml.device.name UTF8String] : "nil");
    if (ml) fprintf(stderr, "[mtl] maxDrawables=%lu allowsTimeout=%d framebufferOnly=%d hidden=%d opaque=%d bounds=%.0fx%.0f scale=%.2f super=%s\n",
            (unsigned long)ml.maximumDrawableCount, (int)ml.allowsNextDrawableTimeout, (int)ml.framebufferOnly,
            (int)ml.hidden, (int)ml.opaque, ml.bounds.size.width, ml.bounds.size.height, ml.contentsScale,
            ml.superlayer ? "yes" : "NO");
}

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
