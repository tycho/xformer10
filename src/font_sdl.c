/****************************************************************************

    FONT_SDL.C

    - UI font resolution for the SDL2 menu bar and file browser on Linux.

    Font file locations differ by distro, so a hardcoded path makes TTF_OpenFont
    fail wherever that file is absent — which would silently remove the menu bar
    and every file dialog. Resolve the font through fontconfig instead, which is
    present on every Linux desktop and always yields an installed face.

****************************************************************************/

#ifndef _WIN32

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <fontconfig/fontconfig.h>
#include <SDL2/SDL.h>

#include "font_sdl.h"

const char *SDLUIFontPath(void)
{
    static char cached[PATH_MAX];
    static int  resolved = 0;

    if (resolved)
        return cached[0] ? cached : NULL;
    resolved = 1;

    /* 1. explicit override wins (any distro, any custom font) */
    const char *env = getenv("XFORMER_FONT");
    if (env && env[0] && access(env, R_OK) == 0) {
        strncpy(cached, env, sizeof cached - 1);
        cached[sizeof cached - 1] = '\0';
        return cached;
    }

    /* 2. ask fontconfig. Prefer DejaVu Sans (what the UI was tuned for); if it
       is not installed, fontconfig substitutes the system default sans face,
       so this effectively never comes back empty on a real desktop. */
    if (FcInit()) {
        FcPattern *pat = FcNameParse((const FcChar8 *)"DejaVu Sans");
        if (pat) {
            FcConfigSubstitute(NULL, pat, FcMatchPattern);
            FcDefaultSubstitute(pat);

            FcResult   res;
            FcPattern *match = FcFontMatch(NULL, pat, &res);
            if (match) {
                FcChar8 *file = NULL;
                if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch
                        && file && file[0]
                        && access((const char *)file, R_OK) == 0) {
                    strncpy(cached, (const char *)file, sizeof cached - 1);
                    cached[sizeof cached - 1] = '\0';
                }
                FcPatternDestroy(match);
            }
            FcPatternDestroy(pat);
        }
    }

    return cached[0] ? cached : NULL;
}

float SDLUIScale(void)
{
    static int   computed = 0;
    static float scale    = 1.0f;

    if (computed)
        return scale;
    computed = 1;

    /* explicit override (e.g. XFORMER_UI_SCALE=2 for a fractional-scale desktop
       where the reported DPI doesn't match the compositor's scaling) */
    const char *env = getenv("XFORMER_UI_SCALE");
    if (env && env[0]) {
        float v = (float)atof(env);
        if (v >= 0.5f && v <= 6.0f) {
            scale = v;
            return scale;
        }
    }

    /* derive from the panel DPI; 96 DPI == 1.0x */
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

#endif /* !_WIN32 */
