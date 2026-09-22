/****************************************************************************

    FONT_SDL.H

    - UI font resolution for the SDL2 menu bar and file browser.

****************************************************************************/

#ifndef FONT_SDL_H
#define FONT_SDL_H

/* Resolve a TrueType UI font path for SDL_ttf.

   Honors $XFORMER_FONT if set and readable; otherwise asks the platform's
   font machinery for an installed face: the Windows Fonts folder (Segoe UI,
   Arial, Tahoma), CoreText on macOS (the system UI font, then Helvetica Neue,
   Helvetica, Lucida Grande, Arial, Geneva), or fontconfig on Linux ("DejaVu
   Sans", falling back to the default sans face). The result is cached for
   the process lifetime. Returns NULL only if no font could be found at all. */
const char *SDLUIFontPath(void);

/* UI scale factor (>= 1.0) for HiDPI displays. Honors $XFORMER_UI_SCALE if set
   (0.5..6.0); otherwise derives from the display DPI (hdpi/96, clamped to
   1.0..4.0), except on macOS where the window is in points and the OS does
   the Retina scaling, so 1.0 is returned. Cached; requires SDL video to be
   initialized. Used to size the menu bar, menu/file-browser fonts, and the
   default window. */
float SDLUIScale(void);

/* Round base * SDLUIScale() to the nearest pixel. */
int SDLUIScaled(int base);

#endif /* FONT_SDL_H */
