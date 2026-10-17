/* libsdl-pspexp-sdl12.so: the SDL layer for SDL 1.2 programs (pspexp-run preloads it instead of the
 * layer when the program uses SDL 1.2).
 *
 * Debian runs SDL 1.2 programs on SDL2 through sdl12-compat (libSDL-1.2.so.0), which opens SDL2 and
 * looks each of its functions up by name (dlsym). The layer cannot be preloaded as for an SDL2
 * program: SDL 1.2 and SDL2 share many names (SDL_Init, SDL_PollEvent, ...), and the program's own
 * SDL 1.2 calls then reached the layer's SDL2 hooks (an SDL2 event written into SDL 1.2's smaller
 * one: Kobo Deluxe's stack smashed). So this preloads only dlsym: the layer is loaded privately
 * (PSPEXP_SDL12_LAYER, RTLD_LOCAL), and a lookup that SDL2 answers with a function the layer hooks
 * gets the hook (pspexp_sdl12_redirect). The program's calls go to sdl12-compat as they should,
 * and sdl12-compat's to SDL2 through the layer.
 *
 * A dlsym(RTLD_NEXT, ...) is passed on as a tail call, so glibc still sees its real caller (RTLD_NEXT
 * is relative to the caller). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void *(*g_real)(void *, const char *);
static void *(*g_redirect)(void *, const char *);
static int g_tried;

static void find_real(void)
{
    static const char *const v[] = { "GLIBC_2.34", "GLIBC_2.17", "GLIBC_2.4", "GLIBC_2.2.5", "GLIBC_2.0" };
    size_t i;
    for (i = 0; i < sizeof(v) / sizeof(v[0]) && !g_real; i++)
        g_real = (void *(*)(void *, const char *))(uintptr_t)dlvsym(RTLD_NEXT, "dlsym", v[i]);
    if (!g_real) abort();
}

static void load_layer(void)
{
    const char *path = getenv("PSPEXP_SDL12_LAYER");
    void *h;
    g_tried = 1;
    if (!path || !*path) { fprintf(stderr, "[sdl-pspexp] SDL 1.2 bridge: PSPEXP_SDL12_LAYER not set\n"); return; }
    if (!(h = dlopen(path, RTLD_NOW | RTLD_LOCAL))) { fprintf(stderr, "[sdl-pspexp] SDL 1.2 bridge: %s\n", dlerror()); return; }
    g_redirect = (void *(*)(void *, const char *))(uintptr_t)g_real(h, "pspexp_sdl12_redirect");
    if (!g_redirect) fprintf(stderr, "[sdl-pspexp] SDL 1.2 bridge: %s has no pspexp_sdl12_redirect\n", path);
}

void *dlsym(void *handle, const char *name)
{
    void *p;
    if (!g_real) find_real();
    if (handle == RTLD_NEXT || handle == RTLD_DEFAULT || !(name[0] == 'S' && name[1] == 'D' && name[2] == 'L' && name[3] == '_'))
        return g_real(handle, name);   /* (a tail call: the caller's RTLD_NEXT stays its own) */
    p = g_real(handle, name);
    if (!g_tried) load_layer();
    return g_redirect && p ? g_redirect(p, name) : p;
}
