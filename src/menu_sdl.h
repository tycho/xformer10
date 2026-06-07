#ifndef MENU_SDL_H
#define MENU_SDL_H
#ifndef _WIN32
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#define MENU_H_BASE 22      /* unscaled menu bar height (1.0x / 96 DPI) */
extern int gMenuBarH;       /* DPI-scaled bar height; set before window creation */
#define MENU_H gMenuBarH    /* all layout uses the runtime (scaled) height */

void MenuInit(SDL_Renderer *ren);
void MenuQuit(void);
void MenuRender(SDL_Renderer *ren);
int  MenuHandleEvent(SDL_Event *e);  /* returns 1 if event consumed */
#endif /* !_WIN32 */
#endif /* MENU_SDL_H */
