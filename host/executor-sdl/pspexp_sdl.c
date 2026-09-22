#include "pspexp_sdl.h"
#include "../libpspexp/pspexp_time.h"
#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct PspExpSdlExecutor {
    PspExpRuntime rt;
    PspExpTransport transport;
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_GameController *pad;
    size_t out_off;
    int quit;
    int vsync;
    int pump_events;
    Uint32 subsystems;
    uint64_t next_input_us;
    SDL_AudioDeviceID audio;
    PspExpMixer *mixer;
};

static uint32_t op_now(void *u) { (void)u; return (uint32_t)pspexp_time_us(); }

/* Indexed textures are expanded through their palette when first drawn, and
 * again whenever the palette resource changes (a palette swap on the PSP is
 * a draw-time binding, so it must be here too). */
typedef struct SdlPalette { uint32_t colors[256]; uint32_t generation; } SdlPalette;
#define SDL_TEX_VARIANTS 8
typedef struct SdlTexVariant { SDL_Texture *tex; const SdlPalette *palette; uint32_t generation; uint32_t last_use; } SdlTexVariant;
typedef struct SdlTexture {
    SDL_Texture *tex;            /* direct-format texture, or NULL for indexed */
    uint8_t *indices;            /* linear index data for T4/T8, else NULL */
    SdlTexVariant variants[SDL_TEX_VARIANTS];   /* indexed: one expansion per palette in use */
} SdlTexture;
static uint32_t g_palette_generation;
static uint32_t g_use_counter;

static uint32_t expand_pixel(uint8_t format, const uint8_t *p)
{
    uint32_t r, g, b, a;
    switch (format) {
    case PSPEXP_TEX_5650: {
        uint16_t c = (uint16_t)(p[0] | (p[1] << 8));
        r = c & 0x1F; g = (c >> 5) & 0x3F; b = (c >> 11) & 0x1F;
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2); a = 255;
        break;
    }
    case PSPEXP_TEX_5551: {
        uint16_t c = (uint16_t)(p[0] | (p[1] << 8));
        r = c & 0x1F; g = (c >> 5) & 0x1F; b = (c >> 10) & 0x1F; a = (c >> 15) ? 255 : 0;
        r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
        break;
    }
    case PSPEXP_TEX_4444: {
        uint16_t c = (uint16_t)(p[0] | (p[1] << 8));
        r = c & 0xF; g = (c >> 4) & 0xF; b = (c >> 8) & 0xF; a = (c >> 12) & 0xF;
        r |= r << 4; g |= g << 4; b |= b << 4; a |= a << 4;
        break;
    }
    default: /* 8888: bytes R G B A */
        r = p[0]; g = p[1]; b = p[2]; a = p[3];
        break;
    }
    return r | (g << 8) | (b << 16) | (a << 24);   /* SDL_PIXELFORMAT_ABGR8888 */
}

static SDL_Texture *make_texture(PspExpSdlExecutor *x, const PspExpCacheEntry *e, const uint32_t *argb)
{
    SDL_Texture *tex = SDL_CreateTexture(x->ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, e->width, e->height);
    if (!tex) return NULL;
    SDL_UpdateTexture(tex, NULL, argb, e->width * 4);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

static int op_texture_create(void *u, PspExpCacheEntry *e, const void *pixels)
{
    PspExpSdlExecutor *x = (PspExpSdlExecutor *)u;
    const uint32_t bits = pspexp_texture_bits_per_pixel(e->format);
    const uint32_t row = e->width * bits / 8;
    uint8_t *linear = NULL;
    const uint8_t *src = (const uint8_t *)pixels;
    SdlTexture *st;
    uint32_t i, n = (uint32_t)e->width * e->height;

    if (bits == 0) return PSPEXP_RERR_BAD_FORMAT;
    st = (SdlTexture *)calloc(1, sizeof(*st));
    if (!st) return PSPEXP_RERR_OUT_OF_MEMORY;
    if (e->layout == PSPEXP_LAYOUT_SWIZZLED) {
        linear = (uint8_t *)malloc(e->bytes);
        if (!linear) { free(st); return PSPEXP_RERR_OUT_OF_MEMORY; }
        pspexp_unswizzle(linear, pixels, row, e->height);
        src = linear;
    }
    if (PSPEXP_TEX_IS_INDEXED(e->format)) {
        /* Keep one index per texel; the palette is applied at draw time. */
        st->indices = (uint8_t *)malloc(n);
        if (!st->indices) { free(linear); free(st); return PSPEXP_RERR_OUT_OF_MEMORY; }
        if (e->format == PSPEXP_TEX_T8) memcpy(st->indices, src, n);
        else for (i = 0; i < n; i++) st->indices[i] = (uint8_t)((src[i / 2] >> ((i & 1) * 4)) & 0xF);  /* T4: low nibble first */
    } else {
        uint32_t *argb = (uint32_t *)malloc((size_t)n * 4);
        if (!argb) { free(linear); free(st); return PSPEXP_RERR_OUT_OF_MEMORY; }
        for (i = 0; i < n; i++) argb[i] = expand_pixel(e->format, src + (size_t)i * (bits / 8));
        st->tex = make_texture(x, e, argb);
        free(argb);
        if (!st->tex) { free(linear); free(st); return PSPEXP_RERR_OUT_OF_MEMORY; }
    }
    free(linear);
    e->user = st;
    return PSPEXP_RERR_NONE;
}

static void op_texture_destroy(void *u, PspExpCacheEntry *e)
{
    SdlTexture *st = (SdlTexture *)e->user;
    int i;
    (void)u;
    if (!st) return;
    if (st->tex) SDL_DestroyTexture(st->tex);
    for (i = 0; i < SDL_TEX_VARIANTS; i++) if (st->variants[i].tex) SDL_DestroyTexture(st->variants[i].tex);
    free(st->indices);
    free(st);
    e->user = NULL;
}

static int op_palette_create(void *u, PspExpCacheEntry *e, const void *entries)
{
    SdlPalette *p = (SdlPalette *)calloc(1, sizeof(*p));
    const uint8_t *src = (const uint8_t *)entries;
    uint32_t i, bpp = pspexp_texture_bits_per_pixel(e->format) / 8;
    (void)u;
    if (!p) return PSPEXP_RERR_OUT_OF_MEMORY;
    for (i = 0; i < e->width && i < 256; i++) p->colors[i] = expand_pixel(e->format, src + (size_t)i * bpp);
    p->generation = ++g_palette_generation;
    e->user = p;
    return PSPEXP_RERR_NONE;
}

static void op_palette_destroy(void *u, PspExpCacheEntry *e) { (void)u; free(e->user); e->user = NULL; }

/* The SDL texture for an indexed atlas expanded through `pal`: one cached
 * expansion per palette variant in use (player colours, font colours), the
 * least recently used one recycled when all slots are taken. */
static SDL_Texture *indexed_texture(PspExpSdlExecutor *x, PspExpCacheEntry *e, const SdlPalette *pal)
{
    SdlTexture *st = (SdlTexture *)e->user;
    uint32_t *argb, i, n = (uint32_t)e->width * e->height;
    SdlTexVariant *slot = NULL;
    if (!st || !st->indices || !pal) return NULL;
    g_use_counter++;
    for (i = 0; i < SDL_TEX_VARIANTS; i++) {
        SdlTexVariant *v = &st->variants[i];
        if (v->tex && v->palette == pal && v->generation == pal->generation) { v->last_use = g_use_counter; return v->tex; }
    }
    for (i = 0; i < SDL_TEX_VARIANTS; i++) {
        SdlTexVariant *v = &st->variants[i];
        if (!v->tex) { slot = v; break; }
        if (!slot || v->last_use < slot->last_use) slot = v;
    }
    argb = (uint32_t *)malloc((size_t)n * 4);
    if (!argb) return NULL;
    for (i = 0; i < n; i++) argb[i] = pal->colors[st->indices[i]];
    if (slot->tex) SDL_DestroyTexture(slot->tex);
    slot->tex = make_texture(x, e, argb);
    free(argb);
    slot->palette = pal;
    slot->generation = pal->generation;
    slot->last_use = g_use_counter;
    return slot->tex;
}

static void op_frame_begin(void *u, uint32_t seq)
{
    PspExpSdlExecutor *x = (PspExpSdlExecutor *)u;
    (void)seq;
    SDL_RenderSetClipRect(x->ren, NULL);
}

static void op_set_clip(void *u, int16_t cx, int16_t cy, int16_t w, int16_t h)
{
    PspExpSdlExecutor *x = (PspExpSdlExecutor *)u;
    SDL_Rect r = { cx, cy, w, h };
    if (w <= 0 || h <= 0) { r.x = r.y = 0; r.w = r.h = 1; }
    SDL_RenderSetClipRect(x->ren, &r);
}

static void op_draw_sprites(void *u, PspExpCacheEntry *atlas, PspExpCacheEntry *palette, uint16_t fmt, const PspExpVertex2D *v, uint32_t count)
{
    PspExpSdlExecutor *x = (PspExpSdlExecutor *)u;
    SdlTexture *st = (SdlTexture *)atlas->user;
    SDL_Texture *tex;
    uint32_t i;
    uint16_t last_color = 0xFFFF;
    if (!st) return;
    tex = palette ? indexed_texture(x, atlas, (const SdlPalette *)palette->user) : st->tex;
    if (!tex) return;
    SDL_SetTextureColorMod(tex, 255, 255, 255);
    SDL_SetTextureAlphaMod(tex, 255);
    for (i = 0; i < count; i++) {
        const PspExpVertex2D *a = &v[2 * i], *b = &v[2 * i + 1];
        const int flipped = a->u > b->u, vflipped = a->v > b->v;
        SDL_Rect src = { flipped ? b->u : a->u, vflipped ? b->v : a->v,
                         flipped ? a->u - b->u : b->u - a->u, vflipped ? a->v - b->v : b->v - a->v };
        SDL_Rect dst = { a->x, a->y, b->x - a->x, b->y - a->y };
        const SDL_RendererFlip flip = (SDL_RendererFlip)((flipped ? SDL_FLIP_HORIZONTAL : 0) | (vflipped ? SDL_FLIP_VERTICAL : 0));
        if (a->color != last_color) {
            if (fmt == PSPEXP_VTX_SPRITE_2D_4444) {
                uint8_t r = (uint8_t)((a->color & 0xF) * 17), g = (uint8_t)(((a->color >> 4) & 0xF) * 17);
                uint8_t bb = (uint8_t)(((a->color >> 8) & 0xF) * 17), al = (uint8_t)(((a->color >> 12) & 0xF) * 17);
                SDL_SetTextureColorMod(tex, r, g, bb);
                SDL_SetTextureAlphaMod(tex, al);
            } else {
                uint8_t r, g, bb;
                pspexp_color_unpack_5650(a->color, &r, &g, &bb);
                SDL_SetTextureColorMod(tex, r, g, bb);
            }
            last_color = a->color;
        }
        if (flip) SDL_RenderCopyEx(x->ren, tex, &src, &dst, 0.0, NULL, flip);
        else SDL_RenderCopy(x->ren, tex, &src, &dst);
    }
}

static void op_draw_rect(void *u, int16_t rx, int16_t ry, int16_t w, int16_t h, uint16_t color, uint8_t alpha)
{
    PspExpSdlExecutor *x = (PspExpSdlExecutor *)u;
    SDL_Rect r = { rx, ry, w, h };
    uint8_t cr, cg, cb;
    pspexp_color_unpack_5650(color, &cr, &cg, &cb);
    SDL_SetRenderDrawBlendMode(x->ren, alpha == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(x->ren, cr, cg, cb, alpha);
    SDL_RenderFillRect(x->ren, &r);
}

static void op_draw_line(void *u, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color, uint8_t alpha)
{
    PspExpSdlExecutor *x = (PspExpSdlExecutor *)u;
    uint8_t cr, cg, cb;
    pspexp_color_unpack_5650(color, &cr, &cg, &cb);
    SDL_SetRenderDrawBlendMode(x->ren, alpha == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(x->ren, cr, cg, cb, alpha);
    SDL_RenderDrawLine(x->ren, x0, y0, x1, y1);
}

static uint32_t op_present(void *u)
{
    PspExpSdlExecutor *x = (PspExpSdlExecutor *)u;
    /* PSPEXP_SDL_SHOT=<file.bmp>: save every 30th frame (for headless checks). */
    static const char *shot = NULL;
    static int shot_checked = 0, frames = 0;
    if (!shot_checked) { shot = getenv("PSPEXP_SDL_SHOT"); shot_checked = 1; }
    if (shot && (frames++ % 30) == 0) {
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(x->ren, &w, &h);
        if (w > 0 && h > 0) {
            SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
            if (surf && SDL_RenderReadPixels(x->ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch) == 0)
                SDL_SaveBMP(surf, shot);
            if (surf) SDL_FreeSurface(surf);
        }
    }
    SDL_RenderPresent(x->ren);
    if (!x->vsync) pspexp_sleep_us(16000);
    return (uint32_t)pspexp_time_us();
}

static void op_log(void *u, const char *msg) { (void)u; fprintf(stderr, "sdl-executor: %s\n", msg); }

static void op_screenshot(void *u, uint16_t index)
{
    PspExpSdlExecutor *x = (PspExpSdlExecutor *)u;
    char name[64];
    int w = 0, h = 0;
    SDL_Surface *surf;
    SDL_GetRendererOutputSize(x->ren, &w, &h);
    if (w <= 0 || h <= 0) return;
    surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return;
    snprintf(name, sizeof(name), "pspexp-shot-%u.bmp", (unsigned)index);
    if (SDL_RenderReadPixels(x->ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch) == 0) SDL_SaveBMP(surf, name);
    SDL_FreeSurface(surf);
}

static void audio_callback(void *u, Uint8 *stream, int len)
{
    PspExpSdlExecutor *x = (PspExpSdlExecutor *)u;
    if (x->mixer) pspexp_mixer_render(x->mixer, (int16_t *)stream, (uint32_t)len / 4u);
    else memset(stream, 0, (size_t)len);
}

static int op_audio_start(void *u, PspExpMixer *m, uint32_t rate)
{
    PspExpSdlExecutor *x = (PspExpSdlExecutor *)u;
    SDL_AudioSpec want, have;
    if (!(SDL_WasInit(SDL_INIT_AUDIO))) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return -1;
        x->subsystems |= SDL_INIT_AUDIO;
    }
    SDL_zero(want);
    want.freq = (int)rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    want.userdata = x;
    x->audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!x->audio) { fprintf(stderr, "sdl-executor: audio: %s\n", SDL_GetError()); return -1; }
    x->mixer = m;
    SDL_PauseAudioDevice(x->audio, 0);
    return 0;
}

static void op_audio_stop(void *u)
{
    PspExpSdlExecutor *x = (PspExpSdlExecutor *)u;
    if (x->audio) { SDL_CloseAudioDevice(x->audio); x->audio = 0; }
    x->mixer = NULL;
}

static void op_audio_lock(void *u) { PspExpSdlExecutor *x = (PspExpSdlExecutor *)u; if (x->audio) SDL_LockAudioDevice(x->audio); }
static void op_audio_unlock(void *u) { PspExpSdlExecutor *x = (PspExpSdlExecutor *)u; if (x->audio) SDL_UnlockAudioDevice(x->audio); }

/* ---- input ---------------------------------------------------------------- */

static void sample_input(PspExpSdlExecutor *x)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint32_t b = 0;
    int ax = 128, ay = 128;
    if (k[SDL_SCANCODE_UP]) b |= PSPEXP_BUTTON_UP;
    if (k[SDL_SCANCODE_DOWN]) b |= PSPEXP_BUTTON_DOWN;
    if (k[SDL_SCANCODE_LEFT]) b |= PSPEXP_BUTTON_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) b |= PSPEXP_BUTTON_RIGHT;
    if (k[SDL_SCANCODE_Z]) b |= PSPEXP_BUTTON_CROSS;
    if (k[SDL_SCANCODE_X]) b |= PSPEXP_BUTTON_CIRCLE;
    if (k[SDL_SCANCODE_A]) b |= PSPEXP_BUTTON_SQUARE;
    if (k[SDL_SCANCODE_S]) b |= PSPEXP_BUTTON_TRIANGLE;
    if (k[SDL_SCANCODE_Q]) b |= PSPEXP_BUTTON_LTRIGGER;
    if (k[SDL_SCANCODE_W]) b |= PSPEXP_BUTTON_RTRIGGER;
    if (k[SDL_SCANCODE_RETURN]) b |= PSPEXP_BUTTON_START;
    if (k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_BACKSPACE]) b |= PSPEXP_BUTTON_SELECT;
    if (k[SDL_SCANCODE_J]) ax = 0;
    if (k[SDL_SCANCODE_L]) ax = 255;
    if (k[SDL_SCANCODE_I]) ay = 0;
    if (k[SDL_SCANCODE_K]) ay = 255;
    if (x->pad) {
        int lx, ly;
        if (SDL_GameControllerGetButton(x->pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) b |= PSPEXP_BUTTON_UP;
        if (SDL_GameControllerGetButton(x->pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b |= PSPEXP_BUTTON_DOWN;
        if (SDL_GameControllerGetButton(x->pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b |= PSPEXP_BUTTON_LEFT;
        if (SDL_GameControllerGetButton(x->pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= PSPEXP_BUTTON_RIGHT;
        if (SDL_GameControllerGetButton(x->pad, SDL_CONTROLLER_BUTTON_A)) b |= PSPEXP_BUTTON_CROSS;
        if (SDL_GameControllerGetButton(x->pad, SDL_CONTROLLER_BUTTON_B)) b |= PSPEXP_BUTTON_CIRCLE;
        if (SDL_GameControllerGetButton(x->pad, SDL_CONTROLLER_BUTTON_X)) b |= PSPEXP_BUTTON_SQUARE;
        if (SDL_GameControllerGetButton(x->pad, SDL_CONTROLLER_BUTTON_Y)) b |= PSPEXP_BUTTON_TRIANGLE;
        if (SDL_GameControllerGetButton(x->pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) b |= PSPEXP_BUTTON_LTRIGGER;
        if (SDL_GameControllerGetButton(x->pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) b |= PSPEXP_BUTTON_RTRIGGER;
        if (SDL_GameControllerGetButton(x->pad, SDL_CONTROLLER_BUTTON_START)) b |= PSPEXP_BUTTON_START;
        if (SDL_GameControllerGetButton(x->pad, SDL_CONTROLLER_BUTTON_BACK)) b |= PSPEXP_BUTTON_SELECT;
        lx = SDL_GameControllerGetAxis(x->pad, SDL_CONTROLLER_AXIS_LEFTX);
        ly = SDL_GameControllerGetAxis(x->pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (abs(lx) > 4000) ax = (lx + 32768) / 257;
        if (abs(ly) > 4000) ay = (ly + 32768) / 257;
    }
    pspexp_runtime_push_input(&x->rt, b, (uint8_t)ax, (uint8_t)ay);
}

static void pump_events(PspExpSdlExecutor *x)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) x->quit = 1;
        else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) x->quit = 1;
        else if (ev.type == SDL_CONTROLLERDEVICEADDED && !x->pad) x->pad = SDL_GameControllerOpen(ev.cdevice.which);
        else if (ev.type == SDL_CONTROLLERDEVICEREMOVED && x->pad) { SDL_GameControllerClose(x->pad); x->pad = NULL; }
    }
}

/* ---- transport ------------------------------------------------------------ */

static int tr_send(PspExpTransport *t, const void *data, size_t len)
{
    PspExpSdlExecutor *x = (PspExpSdlExecutor *)t->user;
    if (x->quit) return -1;
    return pspexp_runtime_feed(&x->rt, data, len);
}

static long tr_recv(PspExpTransport *t, void *buf, size_t cap, int timeout_ms)
{
    PspExpSdlExecutor *x = (PspExpSdlExecutor *)t->user;
    size_t len, n;
    const uint8_t *out;
    uint64_t now;
    if (x->pump_events) pump_events(x);
    if (x->quit) return -1;
    now = pspexp_time_us();
    if (now >= x->next_input_us) {
        uint32_t rate = x->rt.connected && x->rt.ack.input_rate ? x->rt.ack.input_rate : 60;
        sample_input(x);
        x->next_input_us = now + 1000000u / rate;
    }
    out = (const uint8_t *)pspexp_runtime_out_peek(&x->rt, &len);
    if (len <= x->out_off) {
        x->out_off = 0;
        pspexp_runtime_out_consume(&x->rt);
        if (timeout_ms > 0) pspexp_sleep_us(1000);
        return 0;
    }
    n = len - x->out_off;
    if (n > cap) n = cap;
    memcpy(buf, out + x->out_off, n);
    x->out_off += n;
    if (x->out_off >= len) { x->out_off = 0; pspexp_runtime_out_consume(&x->rt); }
    return (long)n;
}

static void tr_close(PspExpTransport *t) { (void)t; }

void pspexp_sdl_options_default(PspExpSdlOptions *o)
{
    memset(o, 0, sizeof(*o));
    o->scale = 2;
    o->vsync = 1;
    o->title = "PSPExp SDL executor";
    o->pump_events = 1;
}

PspExpSdlExecutor *pspexp_sdl_create(const PspExpSdlOptions *o, char *err, size_t errlen)
{
    PspExpSdlExecutor *x = (PspExpSdlExecutor *)calloc(1, sizeof(*x));
    PspExpSdlOptions opt;
    PspExpRuntimeOps ops;
    PspExpRuntimeConfig cfg;
    Uint32 flags;
    if (!x) { snprintf(err, errlen, "out of memory"); return NULL; }
    if (o) opt = *o; else pspexp_sdl_options_default(&opt);
    if (opt.scale < 1) opt.scale = 1;
    x->vsync = opt.vsync;
    x->pump_events = opt.pump_events;
    /* Initialise only what is not already up, and later quit only that. */
    x->subsystems = (SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) & ~SDL_WasInit(0);
    if (x->subsystems && SDL_InitSubSystem(x->subsystems) != 0) {
        snprintf(err, errlen, "SDL_Init: %s", SDL_GetError()); free(x); return NULL;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    x->win = SDL_CreateWindow(opt.title ? opt.title : "PSPExp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              PSPEXP_DISPLAY_WIDTH * opt.scale, PSPEXP_DISPLAY_HEIGHT * opt.scale, SDL_WINDOW_ALLOW_HIGHDPI);
    if (!x->win) { snprintf(err, errlen, "SDL_CreateWindow: %s", SDL_GetError()); if (x->subsystems) SDL_QuitSubSystem(x->subsystems); free(x); return NULL; }
    flags = SDL_RENDERER_ACCELERATED | (opt.vsync ? SDL_RENDERER_PRESENTVSYNC : 0);
    x->ren = SDL_CreateRenderer(x->win, -1, flags);
    if (!x->ren) x->ren = SDL_CreateRenderer(x->win, -1, 0);
    if (!x->ren) { snprintf(err, errlen, "SDL_CreateRenderer: %s", SDL_GetError()); SDL_DestroyWindow(x->win); if (x->subsystems) SDL_QuitSubSystem(x->subsystems); free(x); return NULL; }
    SDL_RenderSetLogicalSize(x->ren, PSPEXP_DISPLAY_WIDTH, PSPEXP_DISPLAY_HEIGHT);
    if (SDL_NumJoysticks() > 0 && SDL_IsGameController(0)) x->pad = SDL_GameControllerOpen(0);

    memset(&ops, 0, sizeof(ops));
    ops.user = x;
    ops.now_us = op_now;
    ops.texture_create = op_texture_create;
    ops.texture_destroy = op_texture_destroy;
    ops.palette_create = op_palette_create;
    ops.palette_destroy = op_palette_destroy;
    ops.frame_begin = op_frame_begin;
    ops.set_clip = op_set_clip;
    ops.draw_sprites = op_draw_sprites;
    ops.draw_rect = op_draw_rect;
    ops.draw_line = op_draw_line;
    ops.present = op_present;
    ops.audio_start = op_audio_start;
    ops.audio_stop = op_audio_stop;
    ops.audio_lock = op_audio_lock;
    ops.audio_unlock = op_audio_unlock;
    ops.log = op_log;
    ops.screenshot = op_screenshot;
    pspexp_runtime_default_config(&cfg, PSPEXP_EXECUTOR_SDL);
    if (pspexp_runtime_init(&x->rt, &ops, &cfg) < 0) { snprintf(err, errlen, "runtime init failed"); pspexp_sdl_destroy(x); return NULL; }
    x->transport.name = "sdl";
    x->transport.user = x;
    x->transport.send = tr_send;
    x->transport.recv = tr_recv;
    x->transport.close = tr_close;
    pspexp_runtime_connect(&x->rt);
    return x;
}

void pspexp_sdl_destroy(PspExpSdlExecutor *x)
{
    if (!x) return;
    pspexp_runtime_free(&x->rt);
    if (x->pad) SDL_GameControllerClose(x->pad);
    if (x->ren) SDL_DestroyRenderer(x->ren);
    if (x->win) SDL_DestroyWindow(x->win);
    if (x->subsystems) SDL_QuitSubSystem(x->subsystems);
    free(x);
}

PspExpTransport *pspexp_sdl_transport(PspExpSdlExecutor *x) { return &x->transport; }
PspExpRuntime *pspexp_sdl_runtime(PspExpSdlExecutor *x) { return &x->rt; }
int pspexp_sdl_quit_requested(const PspExpSdlExecutor *x) { return x->quit; }
