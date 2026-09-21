/****************************************************************************

    FONT_SDL.C

    - UI font resolution for the SDL2 menu bar and file browser.

    Font file locations differ by platform (and by distro on Linux), so a
    hardcoded path makes TTF_OpenFont fail wherever that file is absent — which
    would silently remove the menu bar and every file dialog. On Windows the
    font is resolved from the system Fonts folder; on Linux, through
    fontconfig, which is present on every Linux desktop and always yields an
    installed face.

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

    float hdpi = 0.0f;
    if (SDL_GetDisplayDPI(0, NULL, &hdpi, NULL) == 0 && hdpi > 1.0f)
        scale = hdpi / 96.0f;

    if (scale < 1.0f) scale = 1.0f;
    if (scale > 4.0f) scale = 4.0f;
    return scale;
}


int SDLUIScaled(int base)
{
    return (int)(base * SDLUIScale() + 0.5f);
}
