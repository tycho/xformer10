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

#endif /* FONT_SDL_H */
