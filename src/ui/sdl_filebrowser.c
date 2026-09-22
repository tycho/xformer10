#ifdef SDL2_ENABLED
#include <SDL2/SDL.h>
#include <SDL_ttf.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#ifndef _WIN32
#include <dirent.h>
#include <libgen.h>
#else
#include <windows.h>
#endif
#include "sdl_filebrowser.h"
#include "font_sdl.h"

#ifdef _WIN32
/* Just enough POSIX for the directory-walking code below: a dirent shim over
   FindFirstFile, dirname() for Windows paths, and the usual CRT renames. */

#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#define strcasecmp _stricmp
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

struct dirent { char d_name[MAX_PATH]; };

typedef struct {
    HANDLE           h;
    WIN32_FIND_DATAA fd;
    int              pending;   /* fd holds an entry not yet returned */
    struct dirent    de;
} DIR;

static DIR *opendir(const char *path)
{
    char pat[PATH_MAX + 4];
    DIR *dp = calloc(1, sizeof(DIR));
    if (!dp) return NULL;
    snprintf(pat, sizeof pat, "%s\\*", path);
    dp->h = FindFirstFileA(pat, &dp->fd);
    if (dp->h == INVALID_HANDLE_VALUE) { free(dp); return NULL; }
    dp->pending = 1;
    return dp;
}

static struct dirent *readdir(DIR *dp)
{
    if (!dp->pending && !FindNextFileA(dp->h, &dp->fd))
        return NULL;
    dp->pending = 0;
    strncpy(dp->de.d_name, dp->fd.cFileName, MAX_PATH - 1);
    dp->de.d_name[MAX_PATH - 1] = '\0';
    return &dp->de;
}

static void closedir(DIR *dp)
{
    FindClose(dp->h);
    free(dp);
}

/* dirname(3) that understands both separators and drive roots:
   "C:\a\b" -> "C:\a", "C:\a" -> "C:\", "C:\" -> "C:\" */
static char *dirname(char *p)
{
    size_t n = strlen(p);
    while (n > 1 && (p[n-1] == '\\' || p[n-1] == '/')
           && !(n == 3 && p[1] == ':'))
        p[--n] = '\0';
    char *s1 = strrchr(p, '\\');
    char *s2 = strrchr(p, '/');
    char *s  = s1 > s2 ? s1 : s2;
    if (!s)               { strcpy(p, "."); return p; }
    if (s == p)           { s[1] = '\0';    return p; }   /* "\foo" -> "\" */
    if (s - p == 2 && p[1] == ':') { s[1] = '\0'; return p; } /* "C:\foo" -> "C:\" */
    *s = '\0';
    return p;
}
#endif /* _WIN32 */

/* Layout metrics hold unscaled (1.0x) base values and are multiplied by the UI
   scale at the top of SDL_FileBrowserRunEx(); the macro names redirect to them
   so the drawing/hit-test code below is unchanged. FB_MAX_ENTRIES sizes a
   static array and must stay a compile-time constant. */
static int s_fbFont   = 14;
static int s_fbOvlW   = 600;
static int s_fbOvlH   = 440;
static int s_fbPathH  = 28;
static int s_fbItemH  = 22;
static int s_fbPad    = 8;
static int s_fbInputH = 30;
static float s_fbBacking = 1.0f;   /* device pixels per window unit */

#define FB_FONT_SIZE  s_fbFont
#define FB_OVL_W      s_fbOvlW
#define FB_OVL_H      s_fbOvlH
#define FB_PATH_H     s_fbPathH
#define FB_ITEM_H     s_fbItemH
#define FB_PAD        s_fbPad
#define FB_INPUT_H    s_fbInputH
#define FB_MAX_ENTRIES 512

typedef struct {
    char name[256];
    int  is_dir;
} FbEntry;

#ifdef _WIN32
/* Returns the user's profile directory, or the system drive root. */
static const char *fb_default_dir(void)
{
    const char *home = getenv("USERPROFILE");
    if (home && home[0]) {
        struct stat st;
        if (stat(home, &st) == 0 && S_ISDIR(st.st_mode))
            return home;
    }
    return "C:\\";
}
#else
/* Returns the best available home directory, skipping /root when a real
   user home under /home/ can be found. */
static const char *fb_default_dir(void)
{
    const char *home = getenv("HOME");

    /* Prefer HOME if it looks like a real user home (not /root) */
    if (home && home[0] && strcmp(home, "/root") != 0) {
        struct stat st;
        if (stat(home, &st) == 0 && S_ISDIR(st.st_mode))
            return home;
    }

    /* sudo session: try the real user's home */
    const char *sudo_user = getenv("SUDO_USER");
    if (sudo_user && sudo_user[0]) {
        static char sudo_home[PATH_MAX];
        snprintf(sudo_home, sizeof(sudo_home), "/home/%s", sudo_user);
        struct stat st;
        if (stat(sudo_home, &st) == 0 && S_ISDIR(st.st_mode))
            return sudo_home;
    }

    /* Scan /home for first accessible user directory */
    DIR *dp = opendir("/home");
    if (dp) {
        struct dirent *de;
        static char hbuf[PATH_MAX];
        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] == '.') continue;
            snprintf(hbuf, sizeof(hbuf), "/home/%s", de->d_name);
            struct stat st;
            if (stat(hbuf, &st) == 0 && S_ISDIR(st.st_mode)) {
                closedir(dp);
                return hbuf;
            }
        }
        closedir(dp);
    }

    return home ? home : "/";
}
#endif /* _WIN32 */

static int fb_ext_ok_ex(const char *name, const char *exts)
{
    if (!exts || exts[0] == '\0')
        return 1;
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    char buf[256];
    strncpy(buf, exts, 255);
    buf[255] = '\0';
    char *tok = strtok(buf, ",");
    while (tok) {
        if (strcasecmp(dot, tok) == 0)
            return 1;
        tok = strtok(NULL, ",");
    }
    return 0;
}

static int fb_cmp(const void *a, const void *b)
{
    const FbEntry *ea = (const FbEntry *)a;
    const FbEntry *eb = (const FbEntry *)b;
    if (ea->is_dir != eb->is_dir)
        return eb->is_dir - ea->is_dir;
    return strcasecmp(ea->name, eb->name);
}

static int fb_load_dir_ex(const char *path, FbEntry *entries, int max,
                           const char *exts, int mode)
{
    DIR *dp = opendir(path);
    if (!dp) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL && n < max) {
        if (strcmp(de->d_name, ".") == 0) continue;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        int is_dir = S_ISDIR(st.st_mode);
        if (mode == 1) {
            if (!is_dir) continue;
        } else {
            if (!is_dir && !fb_ext_ok_ex(de->d_name, exts)) continue;
        }
        strncpy(entries[n].name, de->d_name, 255);
        entries[n].name[255] = '\0';
        entries[n].is_dir    = is_dir;
        n++;
    }
    closedir(dp);
    qsort(entries, n, sizeof(FbEntry), fb_cmp);
    if (mode == 1) {
        memmove(&entries[1], &entries[0], n * sizeof(FbEntry));
        strcpy(entries[0].name, "[ This Folder ]");
        entries[0].is_dir = 0;
        return n + 1;
    }
    return n;
}

/* Rasterize text at device resolution (the font is opened at s_fbBacking
   times the point size); w/h come back in window units so layout stays in
   points and the render scale maps the texture 1:1 onto device pixels. */
static SDL_Texture *fb_text(SDL_Renderer *ren, TTF_Font *font, const char *text,
                            SDL_Color color, int *w, int *h)
{
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) { *w = *h = 0; return NULL; }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    *w = (int)(surf->w / s_fbBacking + 0.5f);
    *h = (int)(surf->h / s_fbBacking + 0.5f);
    SDL_FreeSurface(surf);
    return tex;
}

static void fb_render(SDL_Renderer *ren, SDL_Window *win, TTF_Font *font,
                      const char *cwd, FbEntry *entries, int nEntries,
                      int selected, int scroll, int mode, const char *inputbuf)
{
    int winW, winH;
    SDL_GetWindowSize(win, &winW, &winH);

    /* opaque backdrop over the whole window. The browser runs a nested modal
       loop and never redraws the emulator, so without a full clear each frame
       the stale contents of alternating back-buffers flicker through on present. */
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);

    int ox = (winW - FB_OVL_W) / 2;
    int oy = (winH - FB_OVL_H) / 2;

    /* overlay background */
    SDL_SetRenderDrawColor(ren, 35, 35, 35, 255);
    SDL_Rect ovl = {ox, oy, FB_OVL_W, FB_OVL_H};
    SDL_RenderFillRect(ren, &ovl);

    /* outline */
    SDL_SetRenderDrawColor(ren, 110, 110, 110, 255);
    SDL_RenderDrawRect(ren, &ovl);

    /* path bar background */
    SDL_SetRenderDrawColor(ren, 25, 35, 60, 255);
    SDL_Rect pathBar = {ox, oy, FB_OVL_W, FB_PATH_H};
    SDL_RenderFillRect(ren, &pathBar);

    /* path text */
    SDL_Color white = {230, 230, 230, 255};
    int tw, th;
    SDL_Texture *pt = fb_text(ren, font, cwd, white, &tw, &th);
    if (pt) {
        int tx = ox + FB_OVL_W - tw - FB_PAD;
        if (tx < ox + FB_PAD) tx = ox + FB_PAD;
        SDL_Rect dst = {tx, oy + (FB_PATH_H - th) / 2, tw, th};
        SDL_RenderCopy(ren, pt, NULL, &dst);
        SDL_DestroyTexture(pt);
    }

    /* separator */
    SDL_SetRenderDrawColor(ren, 80, 80, 80, 255);
    SDL_RenderDrawLine(ren, ox, oy + FB_PATH_H, ox + FB_OVL_W - 1, oy + FB_PATH_H);

    /* list area height (reduced for mode=2 input bar) */
    int listH = (mode == 2) ? (FB_OVL_H - FB_PATH_H - FB_INPUT_H)
                             : (FB_OVL_H - FB_PATH_H);
    int visRows = listH / FB_ITEM_H;

    SDL_Rect listClip = {ox + 1, oy + FB_PATH_H + 1, FB_OVL_W - 2, listH - 2};
    SDL_RenderSetClipRect(ren, &listClip);

    for (int i = scroll; i < nEntries && i < scroll + visRows; i++) {
        int rowY = oy + FB_PATH_H + (i - scroll) * FB_ITEM_H;

        if (i == selected) {
            if (mode == 1 && i == 0)
                SDL_SetRenderDrawColor(ren, 160, 140, 40, 255);
            else
                SDL_SetRenderDrawColor(ren, 60, 100, 170, 255);
            SDL_Rect hr = {ox + 1, rowY, FB_OVL_W - 2, FB_ITEM_H};
            SDL_RenderFillRect(ren, &hr);
        }

        SDL_Color fc = entries[i].is_dir
                       ? (SDL_Color){140, 190, 255, 255}
                       : (SDL_Color){220, 220, 220, 255};

        char label[258];
        if (entries[i].is_dir)
            snprintf(label, sizeof(label), "%s/", entries[i].name);
        else
            snprintf(label, sizeof(label), "%s",  entries[i].name);

        int lw, lh;
        SDL_Texture *lt = fb_text(ren, font, label, fc, &lw, &lh);
        if (lt) {
            SDL_Rect dst = {ox + FB_PAD, rowY + (FB_ITEM_H - lh) / 2, lw, lh};
            SDL_RenderCopy(ren, lt, NULL, &dst);
            SDL_DestroyTexture(lt);
        }
    }

    SDL_RenderSetClipRect(ren, NULL);

    /* input bar for mode=2 */
    if (mode == 2) {
        SDL_SetRenderDrawColor(ren, 25, 35, 60, 255);
        SDL_Rect inputBar = {ox, oy + FB_OVL_H - FB_INPUT_H, FB_OVL_W, FB_INPUT_H};
        SDL_RenderFillRect(ren, &inputBar);

        SDL_SetRenderDrawColor(ren, 80, 80, 80, 255);
        SDL_RenderDrawLine(ren, ox, oy + FB_OVL_H - FB_INPUT_H,
                           ox + FB_OVL_W - 1, oy + FB_OVL_H - FB_INPUT_H);

        char prompt[260];
        snprintf(prompt, sizeof(prompt), "> %s_", inputbuf ? inputbuf : "");
        int iw, ih;
        SDL_Texture *it_tex = fb_text(ren, font, prompt, white, &iw, &ih);
        if (it_tex) {
            SDL_Rect dst = {ox + FB_PAD,
                            oy + FB_OVL_H - FB_INPUT_H + (FB_INPUT_H - ih) / 2,
                            iw, ih};
            SDL_RenderCopy(ren, it_tex, NULL, &dst);
            SDL_DestroyTexture(it_tex);
        }
    }
}

int SDL_FileBrowserRunEx(SDL_Renderer *ren, SDL_Window *win,
                         const char *start_path, char *out, int sz,
                         const char *exts, int mode)
{
    /* scale the dialog and its font for the display DPI */
    s_fbFont   = SDLUIScaled(14);
    s_fbOvlW   = SDLUIScaled(600);
    s_fbOvlH   = SDLUIScaled(440);
    s_fbPathH  = SDLUIScaled(28);
    s_fbItemH  = SDLUIScaled(22);
    s_fbPad    = SDLUIScaled(8);
    s_fbInputH = SDLUIScaled(30);

    s_fbBacking = GetSDLBackingScale();
    const char *fontPath = SDLUIFontPath();
    TTF_Font *font = fontPath
        ? TTF_OpenFont(fontPath, (int)(FB_FONT_SIZE * s_fbBacking + 0.5f)) : NULL;
    if (!font) return 0;

    char cwd[PATH_MAX];
    if (start_path && start_path[0] != '\0') {
        struct stat st0;
        if (stat(start_path, &st0) == 0 && S_ISDIR(st0.st_mode)) {
            strncpy(cwd, start_path, PATH_MAX - 1);
            cwd[PATH_MAX - 1] = '\0';
        } else {
            char tmp[PATH_MAX];
            strncpy(tmp, start_path, PATH_MAX - 1);
            tmp[PATH_MAX - 1] = '\0';
            char *dir = dirname(tmp);
            struct stat st;
            if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
                strncpy(cwd, dir, PATH_MAX - 1);
                cwd[PATH_MAX - 1] = '\0';
            } else {
                strncpy(cwd, fb_default_dir(), PATH_MAX - 1);
                cwd[PATH_MAX - 1] = '\0';
            }
        }
    } else {
        strncpy(cwd, fb_default_dir(), PATH_MAX - 1);
        cwd[PATH_MAX - 1] = '\0';
    }

    static FbEntry entries[FB_MAX_ENTRIES];
    int nEntries = fb_load_dir_ex(cwd, entries, FB_MAX_ENTRIES, exts, mode);
    int selected = 0, scroll = 0;
    int listH = (mode == 2) ? (FB_OVL_H - FB_PATH_H - FB_INPUT_H)
                             : (FB_OVL_H - FB_PATH_H);
    int visRows = listH / FB_ITEM_H;

    char inputbuf[256] = "";

    if (mode == 2)
        SDL_StartTextInput();

    Uint32 last_click_time  = 0;
    int    last_click_index = -1;

    fb_render(ren, win, font, cwd, entries, nEntries, selected, scroll, mode, inputbuf);
    SDL_RenderPresent(ren);

    int running = 1;
    SDL_Event ev;

    while (running && SDL_WaitEvent(&ev)) {
        int redraw = 0;

        switch (ev.type) {

        case SDL_QUIT:
            running = 0;
            break;

        case SDL_TEXTINPUT:
            if (mode == 2) {
                /* filter newlines — Enter generates SDL_TEXTINPUT "\n" on some SDL2/X11 builds */
                for (const char *p = ev.text.text; *p; p++) {
                    if (*p == '\n' || *p == '\r') continue;
                    int len = strlen(inputbuf);
                    if (len < 255) { inputbuf[len] = *p; inputbuf[len+1] = '\0'; }
                }
                redraw = 1;
            }
            break;

        case SDL_KEYDOWN:
            switch (ev.key.keysym.sym) {
            case SDLK_UP:
                if (selected > 0) selected--;
                if (selected < scroll) scroll = selected;
                redraw = 1;
                break;

            case SDLK_DOWN:
                if (selected < nEntries - 1) selected++;
                if (selected >= scroll + visRows) scroll = selected - visRows + 1;
                redraw = 1;
                break;

            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (mode == 2 && inputbuf[0] != '\0') {
                    /* strip any trailing newline that Enter may have added via SDL_TEXTINPUT */
                    { int n = strlen(inputbuf);
                      while (n > 0 && (inputbuf[n-1]=='\n'||inputbuf[n-1]=='\r')) inputbuf[--n]='\0'; }
                    if (inputbuf[0] == '\0') break; /* nothing left after strip */
                    /* save-as with typed name: commit regardless of entry list */
                    snprintf(out, sz, "%s/%s", cwd, inputbuf);
                    TTF_CloseFont(font);
                    SDL_StopTextInput();
                    return 1;
                } else if (nEntries > 0) {
                    if (entries[selected].is_dir) {
                        char newpath[PATH_MAX];
                        snprintf(newpath, sizeof(newpath), "%s/%s",
                                 cwd, entries[selected].name);
                        strncpy(cwd, newpath, PATH_MAX - 1);
                        cwd[PATH_MAX - 1] = '\0';
                        nEntries = fb_load_dir_ex(cwd, entries, FB_MAX_ENTRIES, exts, mode);
                        selected = 0; scroll = 0;
                        last_click_time = 0; last_click_index = -1;
                        redraw = 1;
                    } else if (mode == 1 && selected == 0) {
                        snprintf(out, sz, "%s", cwd);
                        TTF_CloseFont(font);
                        return 1;
                    } else if (mode == 2) {
                        /* empty inputbuf: populate from selected file */
                        strncpy(inputbuf, entries[selected].name, 255);
                        inputbuf[255] = '\0';
                        redraw = 1;
                    } else {
                        snprintf(out, sz, "%s/%s", cwd, entries[selected].name);
                        TTF_CloseFont(font);
                        return 1;
                    }
                }
                break;

            case SDLK_BACKSPACE:
                if (mode == 2) {
                    int len = strlen(inputbuf);
                    if (len > 0) {
                        inputbuf[len - 1] = '\0';
                        redraw = 1;
                    }
                } else {
                    if (strcmp(cwd, "/") != 0) {
                        char tmp[PATH_MAX];
                        strncpy(tmp, cwd, PATH_MAX - 1);
                        tmp[PATH_MAX - 1] = '\0';
                        char *par = dirname(tmp);
                        strncpy(cwd, par, PATH_MAX - 1);
                        cwd[PATH_MAX - 1] = '\0';
                        nEntries = fb_load_dir_ex(cwd, entries, FB_MAX_ENTRIES, exts, mode);
                        selected = 0; scroll = 0;
                        last_click_time = 0; last_click_index = -1;
                        redraw = 1;
                    }
                }
                break;

            case SDLK_ESCAPE:
                running = 0;
                break;

            default:
                break;
            }
            /* clamp scroll */
            {
                int maxScroll = nEntries > visRows ? nEntries - visRows : 0;
                if (scroll < 0) scroll = 0;
                if (scroll > maxScroll) scroll = maxScroll;
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                int winW, winH;
                SDL_GetWindowSize(win, &winW, &winH);
                int ox = (winW - FB_OVL_W) / 2;
                int oy = (winH - FB_OVL_H) / 2;
                int mx = ev.button.x;
                int my = ev.button.y;
                int listBottom = oy + FB_PATH_H + listH;
                if (mx >= ox && mx < ox + FB_OVL_W &&
                    my >= oy + FB_PATH_H && my < listBottom) {
                    int idx = scroll + (my - oy - FB_PATH_H) / FB_ITEM_H;
                    if (idx >= 0 && idx < nEntries) {
                        Uint32 now = SDL_GetTicks();
                        if (idx == last_click_index &&
                            (now - last_click_time) < 400) {
                            /* double-click */
                            if (entries[idx].is_dir) {
                                char newpath[PATH_MAX];
                                snprintf(newpath, sizeof(newpath), "%s/%s",
                                         cwd, entries[idx].name);
                                strncpy(cwd, newpath, PATH_MAX - 1);
                                cwd[PATH_MAX - 1] = '\0';
                                nEntries = fb_load_dir_ex(cwd, entries, FB_MAX_ENTRIES, exts, mode);
                                selected = 0; scroll = 0;
                                last_click_time = 0; last_click_index = -1;
                                redraw = 1;
                            } else if (mode == 1 && idx == 0) {
                                snprintf(out, sz, "%s", cwd);
                                TTF_CloseFont(font);
                                return 1;
                            } else if (mode == 2) {
                                if (inputbuf[0] != '\0') {
                                    snprintf(out, sz, "%s/%s", cwd, inputbuf);
                                    TTF_CloseFont(font);
                                    SDL_StopTextInput();
                                    return 1;
                                } else {
                                    strncpy(inputbuf, entries[idx].name, 255);
                                    inputbuf[255] = '\0';
                                    redraw = 1;
                                }
                            } else {
                                snprintf(out, sz, "%s/%s", cwd, entries[idx].name);
                                TTF_CloseFont(font);
                                return 1;
                            }
                        } else {
                            /* single click */
                            selected = idx;
                            if (mode == 2 && !entries[idx].is_dir) {
                                strncpy(inputbuf, entries[idx].name, 255);
                                inputbuf[255] = '\0';
                            }
                            last_click_time = now;
                            last_click_index = idx;
                            redraw = 1;
                        }
                    }
                }
            }
            break;

        case SDL_MOUSEWHEEL:
            {
                int maxScroll = nEntries > visRows ? nEntries - visRows : 0;
                if (ev.wheel.y > 0) {
                    scroll -= 3;
                    if (scroll < 0) scroll = 0;
                } else if (ev.wheel.y < 0) {
                    scroll += 3;
                    if (scroll > maxScroll) scroll = maxScroll;
                }
                redraw = 1;
            }
            break;
        }

        if (redraw) {
            fb_render(ren, win, font, cwd, entries, nEntries, selected, scroll, mode, inputbuf);
            SDL_RenderPresent(ren);
        }
    }

    TTF_CloseFont(font);
    if (mode == 2)
        SDL_StopTextInput();
    return 0;
}

#endif /* SDL2_ENABLED */
