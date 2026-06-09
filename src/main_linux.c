#ifndef _WIN32

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glob.h>
#include <limits.h>
#include <time.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>
#include "gemtypes.h"
#include "atari800.h"
#include "res/resource.h"
#include "menu_sdl.h"

void UninitThreads(void);
void LinuxDoCommand(int idm);
extern const int sdl_to_vk[];
void linux_get_client_rect(RECT *r);
extern int GetTileFromPos(int xPos, int yPos, void *ppt);
void ScrollTiles(void);
extern void OpenFolders(char *lpCmdLine, int *piFirstVM);
extern char cGemKeys[];            /* type-to-search filter string (gemul8r.c) */
void DisplayStatus(int iVM);

#define CPUAVG 60ull   /* jiffies to average the speed % over; must match gemul8r.c */

static void sigint_handler(int s) { (void)s; vi.fQuitting = TRUE; }

/* Clock-sanity watchdog. The whole emulator's timing -- frame pacing and audio
   production rate -- rides GetCycles(), i.e. CLOCK_MONOTONIC_RAW. If the host's
   timebase is badly broken (e.g. WSL2's CLOCK_MONOTONIC running ~10% off RAW,
   which silently breaks game speed and makes the audio device overrun), there is
   nothing the emulator can do to stay accurate. This thread quietly compares
   CLOCK_MONOTONIC against CLOCK_MONOTONIC_RAW for a few seconds; if they diverge
   far beyond any sane jitter/NTP slew, it flags the main thread to warn and quit.
   On a healthy host the two agree and the thread just exits after ~15s. */
static volatile int   gClockInsane;    /* set by the watchdog when drift is gross */
static double         gClockDriftPct;  /* measured MONOTONIC vs RAW drift, for the message */

static double ts_sec(clockid_t clk)
{
    struct timespec t;
    clock_gettime(clk, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int clock_sanity_thread(void *unused)
{
    (void)unused;
    double mono0 = ts_sec(CLOCK_MONOTONIC);
    double raw0  = ts_sec(CLOCK_MONOTONIC_RAW);
    for (int i = 0; i < 15 && !vi.fQuitting; i++) {
        SDL_Delay(1000);
        double mono = ts_sec(CLOCK_MONOTONIC) - mono0;
        double raw  = ts_sec(CLOCK_MONOTONIC_RAW) - raw0;
        if (raw < 2.0)                       /* let a couple seconds average out jitter */
            continue;
        double drift = (mono - raw) / raw;   /* fractional rate difference */
        if (drift < -0.01 || drift > 0.01) { /* >1%: far beyond NTP's 500ppm cap */
            gClockDriftPct = drift * 100.0;
            gClockInsane = 1;
            return 0;
        }
    }
    return 0;
}

static LPARAM make_key_lparam(int sdl_sc, int is_up)
{
    extern const unsigned char sdl_to_ps2[];
    DWORD oem = (sdl_sc >= 0 && sdl_sc < 512) ? sdl_to_ps2[sdl_sc] : 0;
    if (is_up)
        return (LPARAM)((oem << 16) | 0xC0000001u);
    return (LPARAM)((oem << 16) | 1u);
}

static SDL_Joystick *gJoy;
static SDL_JoystickID gJoyID;
static int gJoyDir;   /* bits: 0=UP 1=DOWN 2=LEFT 3=RIGHT */

static void joy_key(int sc, int is_down)
{
    int vk = (sc >= 0 && sc < 512) ? sdl_to_vk[sc] : 0;
    if (!vk || v.iVM < 0) return;
    LPARAM lp = make_key_lparam(sc, !is_down) | 0x01000000;
    FWinMsgVM(v.iVM, vi.hWnd, is_down ? WM_KEYDOWN : WM_KEYUP, (WPARAM)vk, lp);
}

static void joy_update_dir(int new_dir)
{
    int changed = gJoyDir ^ new_dir;
    if (changed & 1) joy_key(SDL_SCANCODE_UP,    new_dir & 1);
    if (changed & 2) joy_key(SDL_SCANCODE_DOWN,  (new_dir >> 1) & 1);
    if (changed & 4) joy_key(SDL_SCANCODE_LEFT,  (new_dir >> 2) & 1);
    if (changed & 8) joy_key(SDL_SCANCODE_RIGHT, (new_dir >> 3) & 1);
    gJoyDir = new_dir;
}

int main(int argc, char **argv)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    if (SDL_NumJoysticks() > 0) {
        gJoy = SDL_JoystickOpen(0);
        if (gJoy) gJoyID = SDL_JoystickInstanceID(gJoy);
    }

    {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(vi.szWindowsDir, sizeof(vi.szWindowsDir),
                 "%s/.config/xformer", home);
        mkdir(vi.szWindowsDir, 0755);
    }

    // initialize our clock
    LARGE_INTEGER qpc;
    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);
    vi.qpfCold = qpf.QuadPart;
    QueryPerformanceCounter(&qpc);
    vi.qpcCold = qpc.QuadPart;

    rgpvm = malloc(128 * (sizeof(VM) + sizeof(VMINST)));
    cpvm = 128;

    InitProperties();
    LoadProperties(NULL, FALSE);

    fBrakes = TRUE;     /* emulated (real) speed, not turbo; set before the first
                           DisplayStatus so the title shows the right speed */

    vi.szAppName = "Xformer";
    vi.szTitle   = "Xformer";

    sMaxTiles = 2;

    InitDrawing(X8, Y8, 8, NULL, FALSE);

    {   /* query display refresh rate; fallback 60 Hz */
        SDL_DisplayMode dm;
        v.vRefresh = (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.refresh_rate > 1)
                     ? dm.refresh_rate : 60;
    }

    {   /* compute tile capacity from full display size so resize never needs CreateNewBitmaps */
        RECT rc; linux_get_client_rect(&rc);
        int cols = rc.right  > 0 ? rc.right  / (int)X8 : 1;
        int rows = rc.bottom > 0 ? rc.bottom / (int)Y8 : 1;
        sTilesPerRow = cols + 1;           /* +1: partial tiles on right */
        SDL_DisplayMode dm;
        int maxCols = sTilesPerRow, maxRows = rows + 2;
        if (SDL_GetCurrentDisplayMode(0, &dm) == 0) {
            maxCols = dm.w / (int)X8 + 3;
            maxRows = dm.h / (int)Y8 + 4;
        }
        sMaxTiles = maxCols * maxRows;
        if (sMaxTiles < 2) sMaxTiles = 2;
    }

    if (!CreateNewBitmaps()) {
        fprintf(stderr, "CreateNewBitmaps failed\n");
        SDL_Quit();
        return 1;
    }

    /* Command-line ROM/disk/cartridge/folder loading: OpenFolders() recursively
       walks any directory arguments and loads every valid Atari image it finds,
       exactly like dragging them onto the window. The args are joined into one
       quoted, space-separated string so GetNextFilename()'s tokenizer handles
       paths that contain spaces. */
    if (argc > 1) {
        /* Command-line files replace the restored session: drop any VMs that
           LoadProperties() restored (keeping global settings), then load only
           what was named on the command line. */
        int prev = v.cVM;
        for (int z = 0; z < prev; z++)
            DeleteVM(v.cVM - 1, FALSE);
        DeleteVM(-1, TRUE);

        size_t total = 1;
        for (int i = 1; i < argc; i++)
            total += strlen(argv[i]) + 3;   /* opening + closing quote + space */
        char *cmd = malloc(total);
        if (cmd) {
            cmd[0] = '\0';
            for (int i = 1; i < argc; i++) {
                if (i > 1) strcat(cmd, " ");
                strcat(cmd, "\"");
                strcat(cmd, argv[i]);
                strcat(cmd, "\"");
            }
            int iVMcmd = -1;
            OpenFolders(cmd, &iVMcmd);
            free(cmd);
            if (iVMcmd >= 0) {
                if (v.cVM > 1) {
                    /* multiple images come up tiled; on Linux the tile renderer
                       reads each VM's own pvBits, which fMyVideoCardSucks enables
                       (otherwise the tiles draw with no pixel data) */
                    v.fTiling = 1;
                    v.fMyVideoCardSucks = TRUE;
                    sVM = -1;
                } else {
                    v.fTiling = 0;   /* a single file comes up windowed */
                }
                SelectInstance(iVMcmd);
                v.sWheelOffset = 0;
            }
        }
    }

    if (v.cVM == 0) {
        int iVM = AddVM(1, FALSE, FALSE);
        if (iVM >= 0) {
            FInitVM(iVM);
            ColdStart(iVM);
            SelectInstance(iVM);
        }
    }

    setenv("SDL_AUDIODRIVER", "pulseaudio", 1);
    {
        glob_t gl;
        if (glob("/run/user/*/pulse/native", GLOB_NOSORT, NULL, &gl) == 0
                && gl.gl_pathc > 0) {
            char buf[PATH_MAX];
            snprintf(buf, sizeof buf, "unix:%s", gl.gl_pathv[0]);
            setenv("PULSE_SERVER", buf, 1);
        }
        globfree(&gl);
    }

    InitSound();
    InitThreads();

    vi.fExecuting = TRUE;
    vi.fHaveFocus = TRUE;
    SDL_ShowCursor(SDL_DISABLE);

    signal(SIGINT, sigint_handler);

    /* watch the host timebase for gross drift (see clock_sanity_thread) */
    SDL_CreateThread(clock_sanity_thread, "clocksanity", NULL);

    /* wall-clock anchor for frame pacing (see throttle at the end of the loop) */
    ULONGLONG cLastJif = GetCycles();

    SDL_Event e;
    while (!vi.fQuitting) {
        while (SDL_PollEvent(&e)) {
            if (MenuHandleEvent(&e)) continue;
            if (e.type == SDL_QUIT) {
                vi.fQuitting = TRUE;
            } else if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    vi.fHaveFocus = TRUE;
                    SDL_ShowCursor(SDL_DISABLE);
                } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    vi.fHaveFocus = FALSE;
                    SDL_ShowCursor(SDL_ENABLE);
                }
                else if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED
                         && v.fTiling != 0 && v.cVM > 0
                         && (int)sTileSize.x > 0 && (int)sTileSize.y > 0) {
                    RECT rc; linux_get_client_rect(&rc);
                    int cols = rc.right > 0
                               ? (rc.right * 10 / (int)sTileSize.x + 5) / 10 : 1;
                    if (cols != sTilesPerRow) {
                        sTilesPerRow = cols;
                        InitThreads();
                    }
                }
            } else if (e.type == SDL_JOYDEVICEADDED) {
                if (!gJoy) {
                    gJoy = SDL_JoystickOpen(e.jdevice.which);
                    if (gJoy) gJoyID = SDL_JoystickInstanceID(gJoy);
                }
            } else if (e.type == SDL_JOYDEVICEREMOVED) {
                if (gJoy && e.jdevice.which == gJoyID) {
                    SDL_JoystickClose(gJoy);
                    gJoy = NULL;
                    joy_update_dir(0);   /* release held directions */
                    if (v.iVM >= 0)
                        FWinMsgVM(v.iVM, vi.hWnd, MM_JOY1BUTTONUP, 0, 0);
                }
            } else if (e.type == SDL_JOYBUTTONDOWN) {
                if (gJoy && (e.jbutton.button == 0 || e.jbutton.button == 1) && v.iVM >= 0)
                    FWinMsgVM(v.iVM, vi.hWnd, MM_JOY1BUTTONDOWN, JOY_BUTTON1, 0);
            } else if (e.type == SDL_JOYBUTTONUP) {
                if (gJoy && (e.jbutton.button == 0 || e.jbutton.button == 1) && v.iVM >= 0)
                    FWinMsgVM(v.iVM, vi.hWnd, MM_JOY1BUTTONUP, 0, 0);
            } else if (e.type == SDL_JOYAXISMOTION && gJoy) {
                int new_dir = gJoyDir;
                if (e.jaxis.axis == 0) {         /* X axis → LEFT/RIGHT */
                    new_dir &= ~(4 | 8);
                    if      (e.jaxis.value < -8192) new_dir |= 4;
                    else if (e.jaxis.value >  8192) new_dir |= 8;
                } else if (e.jaxis.axis == 1) {  /* Y axis → UP/DOWN */
                    new_dir &= ~(1 | 2);
                    if      (e.jaxis.value < -8192) new_dir |= 1;
                    else if (e.jaxis.value >  8192) new_dir |= 2;
                }
                joy_update_dir(new_dir);
            } else if (e.type == SDL_JOYHATMOTION && gJoy && e.jhat.hat == 0) {
                int hv = e.jhat.value;
                int new_dir = 0;
                if (hv & SDL_HAT_UP)    new_dir |= 1;
                if (hv & SDL_HAT_DOWN)  new_dir |= 2;
                if (hv & SDL_HAT_LEFT)  new_dir |= 4;
                if (hv & SDL_HAT_RIGHT) new_dir |= 8;
                joy_update_dir(new_dir);
            } else if (e.type == SDL_MOUSEMOTION && v.fTiling) {
                int hit = GetTileFromPos(e.motion.x, e.motion.y - MENU_H, NULL);
                if (hit != sVM) { sVM = hit; }
            } else if (e.type == SDL_MOUSEBUTTONDOWN
                       && e.button.button == SDL_BUTTON_LEFT
                       && v.fTiling && v.cVM > 0) {
                int hit = GetTileFromPos(e.button.x, e.button.y - MENU_H, NULL);
                if (hit >= 0) {
                    v.iVM = hit;
                    LinuxDoCommand(IDM_TILE);
                }
            } else if (e.type == SDL_MOUSEWHEEL && v.fTiling && v.cVM > 0
                       && sTilesPerRow > 0 && (int)sTileSize.y > 0) {
                /* smooth pixel scrolling: ~8px per notch, 64 with "Mouse Wheel
                   Sensitivity High". ScrollTiles() clamps the offset and re-inits
                   threads only when the visible row set changes */
                v.sWheelOffset += e.wheel.y * (v.fWheelSensitive ? 64 : 8);
                ScrollTiles();
            } else if (e.type == SDL_DROPFILE) {
                char *path = e.drop.file;
                if (path) {
                    size_t n = strlen(path);
                    int isGem = (n >= 4 && strcasecmp(path + n - 4, ".gem") == 0);
                    if (isGem) {
                        LoadProperties(path, TRUE);
                        LoadProperties(path, FALSE);
                        v.sWheelOffset = 0;
                        sVM = -1;
                        FixAllMenus(TRUE);
                        InitThreads();
                    } else {
                        int iVMx = -1;
                        OpenFolders(path, &iVMx);
                        if (iVMx >= 0)
                            SelectInstance(iVMx);
                        FixAllMenus(TRUE);
                        InitThreads();
                    }
                    SDL_free(path);
                }
            } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                /* Tiled overview: type the disk/cart name to filter visible VMs
                   (the cGemKeys match lives in InitThreads). Only plain keys —
                   leave Ctrl/Alt/Super combos for the menu shortcuts, and let
                   function/arrow keys (sym has the scancode bit set, > 0x7f)
                   fall through to the F5-untile etc. handlers below. */
                if (v.fTiling && e.type == SDL_KEYDOWN
                    && !(SDL_GetModState() & (KMOD_CTRL | KMOD_ALT | KMOD_GUI))) {
                    SDL_Keycode k = e.key.keysym.sym;
                    int l = (int)strlen(cGemKeys);
                    if (k == SDLK_BACKSPACE) {
                        if (l) cGemKeys[l - 1] = 0;
                        v.sWheelOffset = 0; InitThreads(); DisplayStatus(-1);
                        continue;
                    } else if (k == SDLK_ESCAPE) {
                        if (l) { cGemKeys[0] = 0; v.sWheelOffset = 0;
                                 InitThreads(); DisplayStatus(-1); }
                        continue;
                    } else if (k >= 0x20 && k < 0x7f) {
                        if (l < MAX_PATH - 1) { cGemKeys[l] = (char)k;
                                                cGemKeys[l + 1] = 0; }
                        v.sWheelOffset = 0; InitThreads(); DisplayStatus(-1);
                        continue;
                    }
                }
                int sc = (int)e.key.keysym.scancode;
                int vk = (sc >= 0 && sc < 512) ? sdl_to_vk[sc] : 0;
                if (vk && v.cVM > 0) {
                    int is_down = (e.type == SDL_KEYDOWN);
                    SDL_Keymod mod = SDL_GetModState();
                    LPARAM lp = make_key_lparam(sc, !is_down);
                    switch (sc) {
                    case SDL_SCANCODE_UP:    case SDL_SCANCODE_DOWN:
                    case SDL_SCANCODE_LEFT:  case SDL_SCANCODE_RIGHT:
                    case SDL_SCANCODE_HOME:  case SDL_SCANCODE_END:
                    case SDL_SCANCODE_INSERT: case SDL_SCANCODE_DELETE:
                    case SDL_SCANCODE_PAGEUP: case SDL_SCANCODE_PAGEDOWN:
                        lp |= 0x01000000;
                        break;
                    default:
                        break;
                    }
                    if (sc == SDL_SCANCODE_F4 && (mod & KMOD_ALT) && is_down) {
                        /* Alt+F4: close emulator */
                        vi.fQuitting = TRUE;
                    } else if (sc == SDL_SCANCODE_F4) {
                        /* F4 without Alt: xkey.c case 0x3e swallows bare F4,
                           so set the Win32 extended-key bit (bit 24) to make
                           the switch see 0x13E instead of 0x3E, falling to
                           default which passes scan 0x3E to CheckKey. */
                        FWinMsgVM(v.iVM, vi.hWnd,
                                  is_down ? WM_KEYDOWN : WM_KEYUP,
                                  (WPARAM)vk, lp | (LPARAM)0x01000000);
                    } else if (sc == SDL_SCANCODE_F5 && is_down) {
                        LinuxDoCommand(IDM_TILE);
                    } else if (sc == SDL_SCANCODE_F1 && (mod & KMOD_ALT) && is_down) {
                        LinuxDoCommand(IDM_TURBO);
                    } else if (sc == SDL_SCANCODE_F10 && is_down) {
                        if (mod & KMOD_ALT)
                            LinuxDoCommand(IDM_CHANGEVM);
                        else if (mod & KMOD_SHIFT)
                            LinuxDoCommand(IDM_TOGGLEBASIC);
                        else if (mod & KMOD_CTRL)
                            ColdStart(v.iVM);
                        else
                            FWarmbootVM(v.iVM);
                    } else if (sc == SDL_SCANCODE_F12 && !(mod & (KMOD_ALT|KMOD_SHIFT)) && is_down) {
                        LinuxDoCommand(IDM_STRETCH);
                    } else if (sc == SDL_SCANCODE_F12 && (mod & KMOD_ALT) && is_down) {
                        LinuxDoCommand(IDM_NTSCPAL);
                    } else if (sc == SDL_SCANCODE_F12 && (mod & KMOD_SHIFT) && is_down) {
                        LinuxDoCommand(IDM_COLORMONO);
                    } else if (sc == SDL_SCANCODE_RETURN && (mod & KMOD_ALT) && is_down) {
                        LinuxDoCommand(IDM_FULLSCREEN);
                    } else if (sc == SDL_SCANCODE_S && (mod & KMOD_ALT) && is_down) {
                        LinuxDoCommand(IDM_TOGGLESOUND);
                    } else {
                        FWinMsgVM(v.iVM, vi.hWnd,
                                  is_down ? WM_KEYDOWN : WM_KEYUP,
                                  (WPARAM)vk, lp);
                    }
                }
            }
        }
        /* Throttle rendering to <=70 Hz like the Windows loop: emulating one
           guest frame per iteration is cheap, but presenting every one just
           draws duplicate frames. Gating the present is also what makes turbo
           effective — with vsync dropped in turbo (see IDM_TURBO), the
           non-render iterations carry no wait at all, so the guest free-runs
           well past the display refresh rate. */
        {
            static Uint64 lastRenderMs;
            Uint64 nowMs = SDL_GetTicks64();
            int hz = v.vRefresh + 1;
            if (hz > 70) hz = 70;
            if (hz < 1)  hz = 1;
            if (nowMs - lastRenderMs >= (Uint64)(1000 / hz)) {
                lastRenderMs = nowMs;
                fRenderThisTime = TRUE;
            } else {
                fRenderThisTime = FALSE;
            }
        }

        ULONGLONG FrameBegin = GetCycles();
        BOOL fRanFrame = FALSE;

        static int rprof = -1;
        if (rprof < 0) rprof = getenv("XF_RENDER_PROF") ? 1 : 0;
        Uint64 _e0 = 0, _e1 = 0, _r0 = 0, _r1 = 0;   /* emul / render timestamps */

        if (v.cVM > 0 && cThreads > 0 && !vi.fQuitting) {
            fRanFrame = TRUE;
            if (rprof) _e0 = SDL_GetPerformanceCounter();
            for (int t = 0; t < cThreads; t++)
                SetEvent(ThreadStuff[t].hGoEvent);
            WaitForMultipleObjects(cThreads, hDoneEvent, TRUE, INFINITE);
            if (rprof) _e1 = SDL_GetPerformanceCounter();

            /* A VM stopped executing this frame (vi.fExecuting got reset): it ran
               a KIL — the auto-detect path's signal that this app needs a
               different machine type — or it wants the debugger. Recover the same
               way the Windows message loop does: reinstall such a VM as a
               different type and cold start it (which also clears the breakpoint
               KillMePlease left behind), or delete it if no type works. Without
               this the VM re-KILs every frame and the guest crawls. */
            if (!vi.fExecuting) {
                BOOL fOK = TRUE, fDeleted = FALSE;
                for (int i = 0; i < v.cVM; i++) {
                    int why = (int)rgpvmi(i)->fKillMePlease;
                    if (why && why != 2 && why != 4) {
                        int type = rgpvm[i]->bfHW, otype = 0;
                        while (type >>= 1) otype++;
                        BOOL fXOK = FALSE;
                        FUnInitVM(i);
                        FUnInstallVM(i);
                        rgpvmi(i)->pPrivate = NULL;
                        rgpvmi(i)->iPrivateSize = 0;
                        if (FInstallVM(&rgpvmi(i)->pPrivate, &rgpvmi(i)->iPrivateSize,
                                       rgpvm[i], (PVMINFO)VM_CRASHED, otype))
                            if (FInitVM(i))
                                if (ColdStart(i))
                                    fXOK = TRUE;
                        if (!fXOK) {
                            DeleteVM(v.iVM, TRUE);
                            fDeleted = TRUE;
                        }
                        rgpvmi(i)->fKillMePlease = FALSE;
                    } else if (why == 2 || why == 4) {       /* binary loader / BASIC */
                        if (!ColdStart(i)) {
                            DeleteVM(v.iVM, TRUE);
                            fDeleted = TRUE;
                        }
                        rgpvmi(i)->fKillMePlease = FALSE;
                    } else if (rgpvmi(i)->fWantDebugger) {
                        fOK = FALSE;                          /* no debugger on Linux; leave stopped */
                    }
                }
                if (fOK)
                    vi.fExecuting = TRUE;
                /* A reinstalled VM keeps its tile/thread slot, so its thread just
                   picks up the new private next frame — only a delete shifts the
                   indices and needs the thread pool rebuilt. The throttle below
                   still paces these recovery frames, so we don't spin. */
                if (fDeleted) {
                    FixAllMenus(TRUE);
                    InitThreads();
                    continue;
                }
            }

            if (fRenderThisTime) {
                if (rprof) _r0 = SDL_GetPerformanceCounter();
                RenderBitmap_SDL();
                if (rprof) _r1 = SDL_GetPerformanceCounter();
            }
        } else if (v.fTiling && !vi.fQuitting) {
            /* tiled overview with no visible tiles (e.g. a search that matches
               nothing): still repaint so the view doesn't freeze on a stale frame */
            if (fRenderThisTime)
                RenderBitmap_SDL();
        }

        /* XF_RENDER_PROF: emulation (90-VM fork-join) vs render (incl. the vsync
           present) split, once a second -- pairs with ddlib's clear/compose/upload. */
        if (rprof && _e1 > _e0) {
            static Uint64 last;
            Uint64 now = SDL_GetTicks64();
            if (now - last >= 1000) {
                last = now;
                double f = 1e3 / (double)SDL_GetPerformanceFrequency();
                fprintf(stderr, "[frame] emul %.2f + render %.2f ms (render incl. present)\n",
                        (double)(_e1 - _e0) * f, _r1 > _r0 ? (double)(_r1 - _r0) * f : 0.0);
            }
        }

        /* Maintain the decaying-average execution time exactly like the Windows
           loop (gemul8r.c) so the title-bar speed % is meaningful instead of
           stuck at 0. FrameEnd is taken after the optional render so the sample
           covers the same work the Windows build measures. */
        if (fRanFrame) {
            ULONGLONG FrameEnd = GetCycles();
            if (uExecSpeed) {
                uExecSpeed = (uExecSpeed * (CPUAVG - 1)) / CPUAVG;
                uExecSpeed += (FrameEnd - FrameBegin);
            } else {
                uExecSpeed = (FrameEnd - FrameBegin) * CPUAVG;
            }
        }

        /* Throttle to the guest frame rate. The loop emulates one guest frame
           per iteration; GetCycles() is pure wall-clock (CLOCK_MONOTONIC_RAW),
           so this sleeps until one guest jiffy of real time has elapsed. Audio
           is decoupled -- it's pushed into SDL's queue and paced by this same
           loop -- so nothing else gates speed. Turbo (fBrakes==0) skips the wait
           and free-runs; the IDM_TURBO handler drops vsync so the present can't
           re-cap it. */
        if (!vi.fQuitting) {
            int pal = (!v.fTiling && v.iVM >= 0 && rgpvm[v.iVM]->fEmuPAL);
            ULONGLONG ulljif = pal ? (PAL_CLK / PAL_FPS) : (NTSC_CLK / NTSC_FPS);
            ULONGLONG ullsec = pal ?  PAL_CLK            :  NTSC_CLK;
            ULONGLONG cCur   = GetCycles() - cLastJif;

            /* only brake at emulated speed (or when idle); cap catch-up to 1s */
            if ((fBrakes || !cThreads) && cCur < ullsec) {
                while (cCur < ulljif) {
                    Sleep((cCur < ulljif / 2) ? 8 : 1);   /* sleep, never spin */
                    cCur = GetCycles() - cLastJif;
                }
                cLastJif += ulljif;     /* fixed cadence; absorbs jitter */
            } else {
                cLastJif = GetCycles(); /* turbo or fell behind: resync */
            }
        }

        /* refresh the title bar once a second so the speed, current VM name,
           and search prompt stay current */
        {
            static Uint64 lastStatusMs;
            Uint64 nowMs = SDL_GetTicks64();
            if (nowMs - lastStatusMs >= 1000) {
                lastStatusMs = nowMs;
                int ids = (v.fTiling && sVM >= 0) ? sVM : (v.fTiling ? -1 : v.iVM);
                DisplayStatus(ids);
            }
        }

        /* clock-sanity watchdog tripped: the host timebase is badly broken, so
           neither speed nor audio can be kept correct -- warn and quit */
        if (gClockInsane) {
            char msg[640];
            snprintf(msg, sizeof msg,
                "The system clock is broken: CLOCK_MONOTONIC is drifting %.1f%% "
                "from CLOCK_MONOTONIC_RAW.\n\n"
                "Emulation speed and audio cannot be kept accurate on this host. "
                "This usually means a time-sync daemon is fighting the hardware "
                "clock; on WSL2, 'sudo systemctl mask systemd-timesyncd' and a "
                "restart typically fixes it.\n\nExiting.",
                gClockDriftPct);
            fprintf(stderr, "xformer10: %s\n", msg);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                "xformer10: system clock fault", msg, NULL);
            vi.fQuitting = TRUE;
        }
    }

    UninitThreads();
    if (v.fSaveOnExit)
        SaveProperties(NULL);
    UninitDrawing(TRUE);
    UninitSound();
    SDL_Quit();
    return 0;
}

#endif /* !_WIN32 */
