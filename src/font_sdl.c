/****************************************************************************

    FONT_SDL.C

    - UI font resolution for the SDL2 menu bar and file browser.

    Font file locations differ by platform (and by distro on Linux), so a
    hardcoded path makes TTF_OpenFont fail wherever that file is absent — which
    would silently remove the menu bar and every file dialog. Each platform
    therefore asks its native font machinery for an installed face:

      Windows  the system Fonts folder (Segoe UI, then Arial, then Tahoma)
      macOS    CoreText: the system UI font (San Francisco), then Helvetica
               Neue, Helvetica, Lucida Grande, Arial, Geneva
      Linux    fontconfig, which is present on every Linux desktop and always
               yields an installed face

****************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>

#include "font_sdl.h"

#ifdef _WIN32

#include <windows.h>
#include <shlobj.h>

static int
FileReadableUTF8(const char *path)
{
    wchar_t wpath[MAX_PATH];

    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1,
                             wpath, sizeof(wpath) / sizeof(wpath[0])))
        return 0;

    DWORD attr = GetFileAttributesW(wpath);
    return attr != INVALID_FILE_ATTRIBUTES &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

const char *SDLUIFontPath(void)
{
    static char cached[MAX_PATH * 3];
    static int resolved = 0;

    if (resolved)
        return cached[0] ? cached : NULL;
    resolved = 1;

    /*
     * 1. Explicit override.
     */
    const char *env = getenv("XFORMER_FONT");
    if (env && env[0] && FileReadableUTF8(env)) {
        strncpy(cached, env, sizeof cached - 1);
        cached[sizeof cached - 1] = '\0';
        return cached;
    }

    /*
     * 2. Use the native Windows UI font.
     *
     * Segoe UI is the conventional Windows UI face. Fall back to Arial
     * and Tahoma for older/unusual installations.
     */
    PWSTR fonts_dir = NULL;

    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Fonts, 0, NULL, &fonts_dir))) {
        static const wchar_t *names[] = {
            L"segoeui.ttf",
            L"arial.ttf",
            L"tahoma.ttf",
        };
        wchar_t path[2048];

        for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
            if (wsprintfW(path, L"%ls\\%ls",
                         fonts_dir, names[i]) < 0)
                continue;

            DWORD attr = GetFileAttributesW(path);
            if (attr == INVALID_FILE_ATTRIBUTES ||
                (attr & FILE_ATTRIBUTE_DIRECTORY))
                continue;

            if (WideCharToMultiByte(CP_UTF8, 0, path, -1,
                                    cached, sizeof cached,
                                    NULL, NULL)) {
                CoTaskMemFree(fonts_dir);
                return cached;
            }
        }

        CoTaskMemFree(fonts_dir);
    }

    return NULL;
}

#elif defined(__APPLE__)

#include <unistd.h>
#include <limits.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

/*
 * Copy the on-disk file of a CoreText font into out. Returns 1 if the font
 * is backed by a readable file, 0 otherwise (e.g. a font with no URL).
 */
static int
CTFontFilePath(CTFontRef font, char *out, size_t cb)
{
    if (!font)
        return 0;

    CFURLRef url = CTFontCopyAttribute(font, kCTFontURLAttribute);
    if (!url)
        return 0;

    int ok = CFURLGetFileSystemRepresentation(url, true, (UInt8 *)out,
                                              (CFIndex)cb) &&
             out[0] && access(out, R_OK) == 0;

    CFRelease(url);
    return ok;
}

const char *SDLUIFontPath(void)
{
    static char cached[PATH_MAX];
    static int resolved = 0;

    if (resolved)
        return cached[0] ? cached : NULL;
    resolved = 1;

    /*
     * 1. Explicit override.
     */
    const char *env = getenv("XFORMER_FONT");
    if (env && env[0] && access(env, R_OK) == 0) {
        strncpy(cached, env, sizeof cached - 1);
        cached[sizeof cached - 1] = '\0';
        return cached;
    }

    /*
     * 2. The system UI font (San Francisco on every supported macOS).
     *
     * CoreText resolves it to /System/Library/Fonts/SFNS.ttf, a variable font
     * whose default instance is Regular, which is the instance FreeType
     * loads. Asking CoreText rather than hardcoding the path keeps this
     * working if Apple moves or renames the file.
     */
    CTFontRef ui = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 13.0,
                                                 NULL);
    if (ui) {
        int ok = CTFontFilePath(ui, cached, sizeof cached);
        CFRelease(ui);
        if (ok)
            return cached;
    }

    /*
     * 3. Well-known faces that ship with macOS, in preference order.
     *
     * These are all .ttc collections (or plain .ttf) whose Regular face sits
     * at index 0, which is the face TTF_OpenFont picks. CoreText substitutes
     * a default font for any name it cannot find, so every iteration yields
     * *some* installed face; the readability check is what decides.
     */
    static const CFStringRef names[] = {
        CFSTR("Helvetica Neue"),
        CFSTR("Helvetica"),
        CFSTR("Lucida Grande"),
        CFSTR("Arial"),
        CFSTR("Geneva"),
    };

    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        CTFontRef f = CTFontCreateWithName(names[i], 13.0, NULL);
        if (!f)
            continue;

        int ok = CTFontFilePath(f, cached, sizeof cached);
        CFRelease(f);
        if (ok)
            return cached;
    }

    cached[0] = '\0';
    return NULL;
}

#else

#include <unistd.h>
#include <limits.h>
#include <fontconfig/fontconfig.h>

const char *SDLUIFontPath(void)
{
    static char cached[PATH_MAX];
    static int resolved = 0;

    if (resolved)
        return cached[0] ? cached : NULL;
    resolved = 1;

    /*
     * 1. Explicit override.
     */
    const char *env = getenv("XFORMER_FONT");
    if (env && env[0] && access(env, R_OK) == 0) {
        strncpy(cached, env, sizeof cached - 1);
        cached[sizeof cached - 1] = '\0';
        return cached;
    }

    /*
     * 2. Ask fontconfig.
     */
    if (FcInit()) {
        FcPattern *pat = FcNameParse((const FcChar8 *)"DejaVu Sans");
        if (pat) {
            FcConfigSubstitute(NULL, pat, FcMatchPattern);
            FcDefaultSubstitute(pat);

            FcResult res;
            FcPattern *match = FcFontMatch(NULL, pat, &res);

            if (match) {
                FcChar8 *file = NULL;

                if (FcPatternGetString(match, FC_FILE, 0, &file) ==
                        FcResultMatch &&
                    file && file[0] &&
                    access((const char *)file, R_OK) == 0) {
                    strncpy(cached, (const char *)file,
                            sizeof cached - 1);
                    cached[sizeof cached - 1] = '\0';
                }

                FcPatternDestroy(match);
            }

            FcPatternDestroy(pat);
        }
    }

    return cached[0] ? cached : NULL;
}

#endif


float SDLUIScale(void)
{
    static int computed = 0;
    static float scale = 1.0f;

    if (computed)
        return scale;
    computed = 1;

    const char *env = getenv("XFORMER_UI_SCALE");
    if (env && env[0]) {
        float v = (float)atof(env);
        if (v >= 0.5f && v <= 6.0f) {
            scale = v;
            return scale;
        }
    }

#ifndef __APPLE__
    /*
     * macOS is excluded: its windows are laid out in points and the OS applies
     * the Retina backing scale itself, so the DPI SDL reports there (physical
     * pixels per inch, ~227 on a MacBook) would scale the UI a second time.
     * 1.0 is the correct point-space scale.
     */
    float hdpi = 0.0f;
    if (SDL_GetDisplayDPI(0, NULL, &hdpi, NULL) == 0 && hdpi > 1.0f)
        scale = hdpi / 96.0f;
#endif

    if (scale < 1.0f) scale = 1.0f;
    if (scale > 4.0f) scale = 4.0f;
    return scale;
}


int SDLUIScaled(int base)
{
    return (int)(base * SDLUIScale() + 0.5f);
}
