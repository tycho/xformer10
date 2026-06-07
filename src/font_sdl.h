/****************************************************************************

    FONT_SDL.H

    - UI font resolution for the SDL2 menu bar and file browser on Linux.

****************************************************************************/

#ifndef FONT_SDL_H
#define FONT_SDL_H

/* Resolve a TrueType UI font path for SDL_ttf.

   Honors $XFORMER_FONT if set and readable; otherwise asks fontconfig, which
   always resolves "DejaVu Sans" to an installed font (falling back to the
   system default sans face if DejaVu is absent). The result is cached for the
   process lifetime. Returns NULL only if no font could be found at all. */
const char *SDLUIFontPath(void);

/* UI scale factor (>= 1.0) for HiDPI displays. Honors $XFORMER_UI_SCALE if set
   (0.5..6.0); otherwise derives from the display DPI (hdpi/96, clamped to
   1.0..4.0). Cached; requires SDL video to be initialized. Used to size the
   menu bar, menu/file-browser fonts, and the default window. */
float SDLUIScale(void);

/* Round base * SDLUIScale() to the nearest pixel. */
int SDLUIScaled(int base);

#endif /* FONT_SDL_H */
