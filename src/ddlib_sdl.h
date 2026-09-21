#ifndef DDLIB_SDL_H
#define DDLIB_SDL_H
#ifdef SDL2_ENABLED
#include <SDL2/SDL.h>
SDL_Renderer *GetSDLRenderer(void);
SDL_Window   *GetSDLWindow(void);
void linux_get_client_rect(RECT *r);
void SetSDLVSync(BOOL on);   /* enable/disable vsync on the live renderer (turbo) */
#endif /* SDL2_ENABLED */
#endif /* DDLIB_SDL_H */
