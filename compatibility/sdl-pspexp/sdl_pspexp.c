/*
 * sdl-pspexp: run an unmodified SDL2 program with a PSP as its display,
 * speaker and controller (section 12, "SDL -> PSPExp"; v0.4).
 *
 * Loaded into the program with DYLD_INSERT_LIBRARIES (macOS) or LD_PRELOAD
 * (Linux) by scripts/pspexp-run. It interposes the SDL_Render* API, the
 * audio device and lets the game keep its own window; every draw is
 * mirrored into PSPExp commands, the mixed audio is streamed, and the PSP's
 * controls arrive as a virtual game controller and/or keyboard events.
 *
 * Programs without a renderer (SDL_GetWindowSurface + SDL_UpdateWindowSurface)
 * are handled by streaming the window surface as a texture every present
 * (~270 KB per frame at 480 x 272, fine over USB).
 *
 * Coordinates: the renderer's logical size (or window size) is fitted into
 * 480 x 272 with letterboxing. Textures are shadowed in RGBA8888 so that
 * SDL_UpdateTexture / SDL_LockTexture changes can be re-uploaded (4444, in
 * pages of <= 512 x 512; 5650 when the texture's blend mode is NONE, 4444
 * otherwise). Drawing into a render target is recorded and replayed, scaled,
 * when the target is copied to the screen (the integer-upscaling pattern of
 * pixel-art ports). SDL_RenderGeometry and rotation in SDL_RenderCopyEx are
 * ignored (drawn unrotated).
 *
 * Environment:
 *   PSPEXP=usb | tcp[:host[:port]] | sdl | null   (default usb; sdl = the
 *          PSPExp SDL executor in a second window, for checking the stream)
 *   PSPEXP_KEYS=cross=z,circle=x,...          keyboard events for PSP buttons
 *   PSPEXP_NUB=joystick | mouse | keys        (default joystick; keys = arrows)
 *   PSPEXP_WINDOW=hide                        hide the program's own window
 *   PSPEXP_FOCUS=host                         let the game see host focus loss (default: hidden)
 *   PSPEXP_LOCAL_AUDIO=0                      silence the host's audio device
 *   PSPEXP_AUDIO=psp | host                   pace the game's audio callback ourselves (default when
 *                                             the host driver is SDL's dummy: headless hosts)
 *   PSPEXP_SHOT_EVERY=N                       PSP framebuffer dumps
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <SDL.h>
#include "../../host/libpspexp/pspexp.h"
#include "../../host/executor-null/pspexp_null.h"
#include "../../host/transport-tcp/pspexp_tcp.h"
#include "../../host/executor-sdl/pspexp_sdl.h"
#ifdef PSPEXP_HAVE_USB
#include "../../host/transport-usb/pspexp_usb.h"
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef __APPLE__
#include <dlfcn.h>
#endif

/* ---- interposition ------------------------------------------------------- */

#ifdef __APPLE__
#define HOOK(ret, fn, params) \
    static ret shim_##fn params; \
    __attribute__((used)) static const struct { const void *replacement, *original; } \
        interpose_##fn __attribute__((section("__DATA,__interpose"))) = { (const void *)&shim_##fn, (const void *)&fn }; \
    static ret shim_##fn params
#define REAL(fn) fn
#else
#define HOOK(ret, fn, params) ret fn params
#define REAL(fn) ((__typeof__(&fn))real_symbol(#fn))
static void *real_symbol(const char *name)
{
    void *p = dlsym(RTLD_NEXT, name);
    if (!p) { fprintf(stderr, "[sdl-pspexp] cannot find %s\n", name); abort(); }
    return p;
}
#endif

/* ---- state ------------------------------------------------------------------ */

typedef struct Page { uint16_t tex; int x0, y0, w, h, texw, texh; } Page;

/* A draw recorded into a render target, in the target's pixel space; replayed
 * (scaled into the destination) when the target is copied to the screen. */
typedef struct TargetOp {
    SDL_Texture *tex;          /* NULL: a fill with `color` */
    SDL_Rect src, dst;
    int flip;
    Uint8 r, g, b, a;
} TargetOp;
#define TARGET_OPS_MAX 256

typedef struct Tex {
    SDL_Texture *sdl;
    int w, h;
    uint32_t *rgba;            /* shadow, RGBA8888 (R in the low byte) */
    int dirty, unsupported, streaming;   /* streaming: re-uploaded often, small pages */
    int is_target;             /* SDL_TEXTUREACCESS_TARGET: holds recorded draws, not pixels */
    TargetOp *ops; int nops;
    SDL_BlendMode blend;       /* NONE: uploaded as 5650 (full colour), else 4444 */
    Page pages[16];
    int npages;
    Uint8 r, g, b, a;          /* colour / alpha mod */
    void *lock_pixels; int lock_pitch; SDL_Rect lock_rect; int locked;
    struct Tex *next;
} Tex;

static struct {
    int inited, active;
    PspExpContext *ctx;
    PspExpTransport *transport;
    PspExpNullExecutor *nul;
    PspExpSdlExecutor *sdlx;
    int mode;                          /* 0 usb, 1 tcp, 2 null */
    PspExpTcpOptions tcp;
#ifdef PSPEXP_HAVE_USB
    PspExpUsbOptions usb;
#endif
    SDL_Renderer *ren;
    SDL_Window *win;
    int win_w, win_h, log_w, log_h;
    float sx, sy; int ox, oy;          /* logical -> PSP */
    SDL_Rect viewport; int has_viewport;
    SDL_Rect clip; int has_clip;
    float rsx, rsy;                    /* SDL_RenderSetScale */
    struct { SDL_Rect viewport; int has_viewport; SDL_Rect clip; int has_clip; float rsx, rsy; } backup;   /* around render targets */
    int frame_open, clip_sent; SDL_Rect clip_last;
    Uint8 dr, dg, db, da; SDL_BlendMode dblend;
    SDL_Texture *target;
    Tex *textures;
    uint16_t next_handle;
    /* pending quads */
    PspExpQuadExRec quads[512]; int nquads; uint16_t qtex; uint16_t qfmt;
    /* audio */
    SDL_mutex *amu; int16_t *aring; size_t acap, ahead, atail, abuf; int local_audio;
    SDL_AudioSpec dev_spec[8]; SDL_AudioStream *dev_stream[8]; SDL_AudioCallback dev_cb[8]; void *dev_ud[8];
    /* self-paced: the game's callback is driven by our own thread on a precise
     * clock (headless hosts: SDL's dummy driver runs slow and jittery) */
    int dev_selfpaced[8]; SDL_AudioDeviceID dev_id[8]; SDL_Thread *audio_thread; int audio_thread_stop;
    /* input */
    int nub_mouse, nub_keys, hide_window; uint32_t prev_buttons, nub_dirs; float px, py; Uint32 ptick;
    SDL_Keycode keys[16];
    int vjoy;                          /* virtual joystick device index, -1 none */
    SDL_Joystick *vjoy_handle;
    /* stats */
    Uint32 last_report; unsigned frames, uploads, shot_every, shot_index;
    uint32_t last_shot_frame;
    int skip_clear, skip_rects, skip_lines, skip_quads, skip_audio;   /* PSPEXP_DEBUG_SKIP */
    /* window-surface streaming (no renderer) */
    Tex *surface_tex; int surface_w, surface_h;
    unsigned long total_frames;
} S;

enum { B_CROSS, B_CIRCLE, B_SQUARE, B_TRIANGLE, B_L, B_R, B_START, B_SELECT, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_COUNT };
static const char *BUTTON_NAMES[B_COUNT] = { "cross", "circle", "square", "triangle", "l", "r", "start", "select", "up", "down", "left", "right" };
static const uint32_t BUTTON_BITS[B_COUNT] = {
    PSPEXP_BUTTON_CROSS, PSPEXP_BUTTON_CIRCLE, PSPEXP_BUTTON_SQUARE, PSPEXP_BUTTON_TRIANGLE,
    PSPEXP_BUTTON_LTRIGGER, PSPEXP_BUTTON_RTRIGGER, PSPEXP_BUTTON_START, PSPEXP_BUTTON_SELECT,
    PSPEXP_BUTTON_UP, PSPEXP_BUTTON_DOWN, PSPEXP_BUTTON_LEFT, PSPEXP_BUTTON_RIGHT };

static void logf_(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[sdl-pspexp] "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap);
}
static void lib_log(void *u, const char *msg) { (void)u; logf_("%s", msg); }

static uint16_t alloc_handle(void) { return S.next_handle++; }
static void shim_exit(void);

/* ---- connection ------------------------------------------------------------ */

/* SDL's key names are "Left Ctrl", "Return", ","...; accept the short forms
 * people type, then SDL's own names (case-insensitive). */
static SDL_Keycode key_from_name(const char *name)
{
    static const struct { const char *alias; SDL_Keycode key; } aliases[] = {
        { "lctrl", SDLK_LCTRL }, { "rctrl", SDLK_RCTRL }, { "ctrl", SDLK_LCTRL },
        { "lshift", SDLK_LSHIFT }, { "rshift", SDLK_RSHIFT }, { "shift", SDLK_LSHIFT },
        { "lalt", SDLK_LALT }, { "ralt", SDLK_RALT }, { "alt", SDLK_LALT },
        { "lgui", SDLK_LGUI }, { "rgui", SDLK_RGUI }, { "cmd", SDLK_LGUI },
        { "comma", SDLK_COMMA }, { "period", SDLK_PERIOD }, { "slash", SDLK_SLASH },
        { "minus", SDLK_MINUS }, { "equals", SDLK_EQUALS }, { "plus", SDLK_PLUS },
        { "enter", SDLK_RETURN }, { "esc", SDLK_ESCAPE }, { "backspace", SDLK_BACKSPACE },
        { "pgup", SDLK_PAGEUP }, { "pgdn", SDLK_PAGEDOWN }, { "ins", SDLK_INSERT }, { "del", SDLK_DELETE },
        { "none", SDLK_UNKNOWN },
    };
    size_t i;
    SDL_Keycode k;
    for (i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
        if (!SDL_strcasecmp(name, aliases[i].alias)) return aliases[i].key;
    k = SDL_GetKeyFromName(name);
    if (k == SDLK_UNKNOWN) logf_("PSPEXP_KEYS: unknown key name \"%s\"", name);
    return k;
}

static void parse_keys(void)
{
    const char *def = "cross=z,circle=x,square=a,triangle=s,l=q,r=w,start=return,select=escape,up=up,down=down,left=left,right=right";
    const char *env = getenv("PSPEXP_KEYS");
    char buf[512], *p, *tok;
    int i;
    for (i = 0; i < B_COUNT; i++) S.keys[i] = SDLK_UNKNOWN;
    strncpy(buf, env && *env ? env : def, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_r(buf, ",", &p); tok; tok = strtok_r(NULL, ",", &p)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        for (i = 0; i < B_COUNT; i++)
            if (!strcmp(tok, BUTTON_NAMES[i])) S.keys[i] = key_from_name(eq + 1);
    }
}

static void connect_once(void)
{
    const char *env = getenv("PSPEXP");
    char err[200] = "";
    if (S.inited) return;
    S.inited = 1;
    S.next_handle = 1;
    S.local_audio = !(getenv("PSPEXP_LOCAL_AUDIO") && !strcmp(getenv("PSPEXP_LOCAL_AUDIO"), "0"));
    S.nub_mouse = getenv("PSPEXP_NUB") && !strcmp(getenv("PSPEXP_NUB"), "mouse");
    S.nub_keys = getenv("PSPEXP_NUB") && !strcmp(getenv("PSPEXP_NUB"), "keys");
    S.hide_window = getenv("PSPEXP_WINDOW") && !strcmp(getenv("PSPEXP_WINDOW"), "hide");
    S.shot_every = getenv("PSPEXP_SHOT_EVERY") ? (unsigned)atoi(getenv("PSPEXP_SHOT_EVERY")) : 0;
    S.vjoy = -1;
    S.rsx = S.rsy = 1.0f;
    parse_keys();
    {
        const char *skip = getenv("PSPEXP_DEBUG_SKIP");
        if (skip) {
            S.skip_clear = strstr(skip, "clear") != NULL; S.skip_rects = strstr(skip, "rects") != NULL;
            S.skip_lines = strstr(skip, "lines") != NULL; S.skip_quads = strstr(skip, "quads") != NULL;
            S.skip_audio = strstr(skip, "audio") != NULL;
        }
    }
    if (!env || !*env) env = "usb";
    if (!strcmp(env, "null")) {
        S.nul = pspexp_null_create(NULL);
        S.transport = pspexp_null_transport(S.nul);
        S.mode = 2;
    } else if (!strcmp(env, "sdl")) {
        /* In-process executor: its own renderer, so the hooks pass its draws through. */
        PspExpSdlOptions o;
        pspexp_sdl_options_default(&o);
        o.pump_events = 0;
        o.title = "PSPExp (sdl-pspexp)";
        S.sdlx = pspexp_sdl_create(&o, err, sizeof(err));
        if (S.sdlx) S.transport = pspexp_sdl_transport(S.sdlx);
        S.mode = 2;
    } else if (!strncmp(env, "tcp", 3)) {
        static char host[128];
        pspexp_tcp_options_default(&S.tcp);
        if (env[3] == ':') {
            char *colon;
            strncpy(host, env + 4, sizeof(host) - 1);
            colon = strrchr(host, ':');
            if (colon) { *colon = 0; S.tcp.port = (uint16_t)atoi(colon + 1); }
            if (host[0]) S.tcp.host = host;
        }
        logf_("connecting to %s:%u over TCP...", S.tcp.host, S.tcp.port);
        S.transport = pspexp_tcp_wait(&S.tcp, 30000, err, sizeof(err));
        S.mode = 1;
    } else {
#ifdef PSPEXP_HAVE_USB
        pspexp_usb_options_default(&S.usb);
        logf_("waiting up to 15 s for a PSP over USB...");
        S.transport = pspexp_usb_wait(&S.usb, 15000, err, sizeof(err));
        S.mode = 0;
#else
        logf_("built without USB support");
#endif
    }
    if (!S.transport) { logf_("%s", err[0] ? err : "no transport"); return; }
    S.ctx = pspexp_create(S.transport, NULL);
    pspexp_set_log(S.ctx, lib_log, NULL);
    if (pspexp_connect(S.ctx, 5000) < 0) { logf_("no HELLO from the runtime"); pspexp_destroy(S.ctx); S.ctx = NULL; return; }
    S.active = 1;
    atexit(shim_exit);
    S.amu = SDL_CreateMutex();
    S.acap = 44100 * 2;
    S.aring = (int16_t *)calloc(S.acap * 2, sizeof(int16_t));
    S.px = 240; S.py = 136;
    S.last_report = SDL_GetTicks();
    logf_("active (%s)", env);
}

static int link_up(void) { return S.active && S.ctx && pspexp_link_state(S.ctx) == PSPEXP_LINK_CONNECTED; }

static void try_reconnect(void)
{
    static Uint32 last;
    char err[200];
    PspExpTransport *t = NULL;
    Uint32 now = SDL_GetTicks();
    if (!S.active || S.mode == 2 || now - last < 1000) return;
    last = now;
    if (S.transport) { pspexp_set_transport(S.ctx, NULL); S.transport = NULL; logf_("link lost; waiting for the PSP"); }
    if (S.mode == 1) t = pspexp_tcp_open(&S.tcp, err, sizeof(err));
#ifdef PSPEXP_HAVE_USB
    else t = pspexp_usb_open(&S.usb, err, sizeof(err));
#endif
    if (!t) return;
    pspexp_set_transport(S.ctx, t);
    S.transport = t;
    if (pspexp_connect(S.ctx, 3000) == 0) {
        Tex *tx;
        logf_("reconnected");
        for (tx = S.textures; tx; tx = tx->next) tx->dirty = 1;   /* the library replays, but re-upload cheaply anyway */
    }
}

/* ---- geometry -------------------------------------------------------------- */

static void update_scale(void)
{
    int lw = S.log_w > 0 ? S.log_w : S.win_w, lh = S.log_h > 0 ? S.log_h : S.win_h;
    float s;
    if (lw <= 0 || lh <= 0) { lw = 480; lh = 272; }
    s = fminf(480.0f / lw, 272.0f / lh);
    S.sx = S.sy = s;
    S.ox = (int)((480 - lw * s) / 2);
    S.oy = (int)((272 - lh * s) / 2);
    logf_("geometry: window %dx%d, logical %dx%d -> scale %.3f, offset %d,%d", S.win_w, S.win_h, S.log_w, S.log_h, s, S.ox, S.oy);
}

static int16_t X(float x) { return (int16_t)lroundf(S.ox + (x + (S.has_viewport ? S.viewport.x : 0)) * S.sx); }
static int16_t Y(float y) { return (int16_t)lroundf(S.oy + (y + (S.has_viewport ? S.viewport.y : 0)) * S.sy); }
static int16_t W(float w) { int v = (int)lroundf(w * S.sx); return (int16_t)(v < 1 ? 1 : v); }
static int16_t H(float h) { int v = (int)lroundf(h * S.sy); return (int16_t)(v < 1 ? 1 : v); }

static void flush_quads(void)
{
    if (S.nquads) {
        pspexp_draw_quad_ex_batch(S.ctx, S.qtex, S.quads, (uint32_t)S.nquads, PSPEXP_PALETTE_DEFAULT, S.qfmt);
        S.nquads = 0;
    }
}

static void begin_frame(void)
{
    if (!S.frame_open) { pspexp_begin_frame(S.ctx); S.frame_open = 1; S.clip_sent = 0; }
}

static void apply_clip(void)
{
    SDL_Rect want;
    if (S.has_clip) want = (SDL_Rect){ X((float)S.clip.x), Y((float)S.clip.y), W((float)S.clip.w), H((float)S.clip.h) };
    else want = (SDL_Rect){ 0, 0, 480, 272 };
    if (S.clip_sent && !memcmp(&want, &S.clip_last, sizeof(want))) return;
    flush_quads();
    pspexp_set_clip(S.ctx, (int16_t)want.x, (int16_t)want.y, (int16_t)want.w, (int16_t)want.h);
    S.clip_last = want;
    S.clip_sent = 1;
}

static Tex *find_tex(SDL_Texture *t);
static void mirror_copy(SDL_Texture *t, const SDL_Rect *src, float dx, float dy, float dw, float dh, int flip);
static Tex *target_tex(void) { return S.target ? find_tex(S.target) : NULL; }

static void record_op(SDL_Texture *tex, const SDL_Rect *src, const SDL_Rect *dst, int flip, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    Tex *t = target_tex();
    TargetOp *op;
    if (!t || !t->is_target) return;
    if (!t->ops) t->ops = (TargetOp *)calloc(TARGET_OPS_MAX, sizeof(TargetOp));
    if (!t->ops || t->nops == TARGET_OPS_MAX) return;
    op = &t->ops[t->nops++];
    op->tex = tex;
    op->src = src ? *src : (SDL_Rect){ 0, 0, 0, 0 };
    op->dst = dst ? *dst : (SDL_Rect){ 0, 0, t->w, t->h };
    op->flip = flip; op->r = r; op->g = g; op->b = b; op->a = a;
}

static int drawing(void)
{
    if (!link_up() || S.target) return 0;
    begin_frame();
    apply_clip();
    return 1;
}

static uint16_t draw_color5650(void) { return pspexp_color_5650(S.dr, S.dg, S.db); }
static uint8_t draw_alpha(void) { return S.dblend == SDL_BLENDMODE_NONE ? 255 : S.da; }

/* ---- textures -------------------------------------------------------------- */

static Tex *find_tex(SDL_Texture *t)
{
    Tex *x;
    for (x = S.textures; x; x = x->next) if (x->sdl == t) return x;
    return NULL;
}

static Tex *add_tex(SDL_Texture *t, int w, int h)
{
    Tex *x = (Tex *)calloc(1, sizeof(*x));
    if (!x) return NULL;
    x->sdl = t; x->w = w; x->h = h;
    x->rgba = (uint32_t *)calloc((size_t)w * h, 4);
    x->r = x->g = x->b = x->a = 255;
    x->dirty = 1;
    x->next = S.textures;
    S.textures = x;
    return x;
}

static void release_pages(Tex *x)
{
    int i;
    if (S.ctx) for (i = 0; i < x->npages; i++) pspexp_release(S.ctx, x->pages[i].tex);
    x->npages = 0;
}

static int pow2ceil(int v, int minimum) { int p = minimum; while (p < v) p <<= 1; return p; }

static int upload_tex(Tex *x)
{
    int y0, x0;
    /* streamed surfaces use 256 x 128 pages so the padding stays small */
    int pw = x->streaming ? 256 : 512, ph = x->streaming ? 128 : 512;   /* small pages: less padding per frame */
    release_pages(x);
    for (y0 = 0; y0 < x->h; y0 += ph) {
        for (x0 = 0; x0 < x->w; x0 += pw) {
            Page *p;
            PspExpTextureDesc d;
            PspExpFrame f;
            uint16_t *buf;
            int y, opaque = x->blend == SDL_BLENDMODE_NONE;
            if (x->npages == 16) { x->unsupported = 1; return -1; }
            p = &x->pages[x->npages];
            p->x0 = x0; p->y0 = y0;
            p->w = x->w - x0 < pw ? x->w - x0 : pw;
            p->h = x->h - y0 < ph ? x->h - y0 : ph;
            p->texw = pow2ceil(p->w, 16);
            p->texh = pow2ceil(p->h, 8);
            buf = (uint16_t *)calloc((size_t)p->texw * p->texh, 2);
            if (!buf) return -1;
            for (y = 0; y < p->h; y++) {
                const uint32_t *src = x->rgba + (size_t)(y0 + y) * x->w + x0;
                uint16_t *dst = buf + (size_t)y * p->texw;
                int xx;
                for (xx = 0; xx < p->w; xx++) {
                    uint32_t c = src[xx];
                    dst[xx] = opaque ? pspexp_color_5650((uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16))
                                     : pspexp_color_4444((uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24));
                }
            }
            /* Swizzled: the GE fetches 16-byte x 8-row blocks, several times
             * faster than a linear texture (Doom's frame: 10.5 ms -> ~3 ms). */
            {
                uint16_t *sw = (uint16_t *)malloc((size_t)p->texw * p->texh * 2);
                if (sw) { pspexp_swizzle(sw, buf, (uint32_t)p->texw * 2u, (uint32_t)p->texh); free(buf); buf = sw; }
            }
            memset(&d, 0, sizeof(d));
            d.width = (uint16_t)p->texw; d.height = (uint16_t)p->texh; d.format = opaque ? PSPEXP_TEX_5650 : PSPEXP_TEX_4444;
            d.layout = PSPEXP_LAYOUT_SWIZZLED; d.priority = PSPEXP_PRIORITY_NORMAL;   /* VRAM: the GE reads it far faster than RAM */
            f.u0 = 0; f.v0 = 0; f.w = (uint16_t)(p->texw < 64 ? p->texw : 64); f.h = (uint16_t)p->texh;
            p->tex = alloc_handle();
            if (pspexp_upload_texture(S.ctx, p->tex, &d, buf, (uint32_t)((size_t)p->texw * p->texh * 2), &f, 1) < 0) {
                free(buf);
                x->unsupported = 1;
                logf_("texture %dx%d rejected", x->w, x->h);
                return -1;
            }
            free(buf);
            S.uploads++;
            x->npages++;
        }
    }
    x->dirty = 0;
    return 0;
}

/* Copy pixels of any SDL format into the RGBA8888 shadow. */
static void shadow_update(Tex *x, const SDL_Rect *rect, Uint32 format, const void *pixels, int pitch)
{
    SDL_Rect r = rect ? *rect : (SDL_Rect){ 0, 0, x->w, x->h };
    if (r.x < 0 || r.y < 0 || r.x + r.w > x->w || r.y + r.h > x->h || r.w <= 0 || r.h <= 0) return;
    SDL_ConvertPixels(r.w, r.h, format, pixels, pitch, SDL_PIXELFORMAT_ABGR8888,
                      x->rgba + (size_t)r.y * x->w + r.x, x->w * 4);
    x->dirty = 1;
}

static uint16_t tint_for(const Tex *x, uint16_t *fmt)
{
    if (x->a < 255) { *fmt = PSPEXP_VTX_SPRITE_2D_4444; return pspexp_color_4444(x->r, x->g, x->b, x->a); }
    *fmt = PSPEXP_VTX_SPRITE_2D_5650;
    return pspexp_color_5650(x->r, x->g, x->b);
}

static void mirror_fill(float x, float y, float w, float h);

/* Copy of a render target: replay what was drawn into it, mapped through
 * src (target space) -> (dx, dy, dw, dh) (logical space), clipped to it. */
static void replay_target(Tex *t, const SDL_Rect *src, float dx, float dy, float dw, float dh)
{
    SDL_Rect s = src ? *src : (SDL_Rect){ 0, 0, t->w, t->h };
    SDL_Rect saved_clip = S.clip; int saved_has = S.has_clip;
    SDL_Rect want = { (int)dx, (int)dy, (int)dw, (int)dh };
    Uint8 sr = S.dr, sg = S.dg, sb = S.db, sa = S.da; SDL_BlendMode sblend = S.dblend;
    float kx, ky;
    int i;
    if (s.w <= 0 || s.h <= 0 || !t->ops) return;
    kx = dw / (float)s.w; ky = dh / (float)s.h;
    if (S.has_clip) SDL_IntersectRect(&S.clip, &want, &want);
    S.clip = want; S.has_clip = 1;
    for (i = 0; i < t->nops; i++) {
        const TargetOp *op = &t->ops[i];
        /* mirror_copy / mirror_fill multiply by the render scale again: pre-divide */
        float ox = (dx + (op->dst.x - s.x) * kx) / S.rsx, oy = (dy + (op->dst.y - s.y) * ky) / S.rsy;
        float ow = op->dst.w * kx / S.rsx, oh = op->dst.h * ky / S.rsy;
        if (!op->tex) {
            S.dr = op->r; S.dg = op->g; S.db = op->b; S.da = op->a; S.dblend = op->a < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE;
            mirror_fill(ox, oy, ow, oh);
        } else {
            Tex *x = find_tex(op->tex);
            if (!x) continue;
            mirror_copy(op->tex, op->src.w ? &op->src : NULL, ox, oy, ow, oh, op->flip);
        }
    }
    S.dr = sr; S.dg = sg; S.db = sb; S.da = sa; S.dblend = sblend;
    S.clip = saved_clip; S.has_clip = saved_has;
}

static void mirror_copy(SDL_Texture *t, const SDL_Rect *src, float dx, float dy, float dw, float dh, int flip)
{
    Tex *x = find_tex(t);
    uint16_t fmt, color;
    int i;
    SDL_Rect s;
    if (!x || x->unsupported || S.skip_quads) return;
    dx *= S.rsx; dy *= S.rsy; dw *= S.rsx; dh *= S.rsy;
    if (S.target) {   /* drawing into a render target: remember it */
        SDL_Rect d = { (int)dx, (int)dy, (int)dw, (int)dh };
        record_op(t, src, &d, flip, 255, 255, 255, 255);
        return;
    }
    if (x->is_target) { replay_target(x, src, dx, dy, dw, dh); return; }
    if (!drawing()) return;
    if (x->dirty && upload_tex(x) < 0) return;
    s = src ? *src : (SDL_Rect){ 0, 0, x->w, x->h };
    color = tint_for(x, &fmt);
    /* the source may span pages: split it */
    for (i = 0; i < x->npages; i++) {
        const Page *p = &x->pages[i];
        int ix0 = s.x > p->x0 ? s.x : p->x0, ix1 = (s.x + s.w < p->x0 + p->w) ? s.x + s.w : p->x0 + p->w;
        int iy0 = s.y > p->y0 ? s.y : p->y0, iy1 = (s.y + s.h < p->y0 + p->h) ? s.y + s.h : p->y0 + p->h;
        PspExpQuadExRec q;
        float fx, fy, fw, fh;
        if (ix0 >= ix1 || iy0 >= iy1) continue;
        /* destination sub-rectangle in logical units, proportional to the source split */
        fw = dw * (float)(ix1 - ix0) / (float)s.w;
        fh = dh * (float)(iy1 - iy0) / (float)s.h;
        fx = dx + ((flip & SDL_FLIP_HORIZONTAL) ? dw * (float)(s.x + s.w - ix1) / (float)s.w : dw * (float)(ix0 - s.x) / (float)s.w);
        fy = dy + ((flip & SDL_FLIP_VERTICAL) ? dh * (float)(s.y + s.h - iy1) / (float)s.h : dh * (float)(iy0 - s.y) / (float)s.h);
        memset(&q, 0, sizeof(q));
        q.x = X(fx); q.y = Y(fy); q.w = W(fw); q.h = H(fh);
        q.u0 = (uint16_t)(ix0 - p->x0); q.u1 = (uint16_t)(ix1 - p->x0);
        q.v0 = (uint16_t)(iy0 - p->y0); q.v1 = (uint16_t)(iy1 - p->y0);
        if (flip & SDL_FLIP_HORIZONTAL) { uint16_t tmp = q.u0; q.u0 = q.u1; q.u1 = tmp; }
        if (flip & SDL_FLIP_VERTICAL) { uint16_t tmp = q.v0; q.v0 = q.v1; q.v1 = tmp; }
        q.color = color;
        if (S.nquads && (S.qtex != p->tex || S.qfmt != fmt)) flush_quads();
        if (S.nquads == 512) flush_quads();
        S.qtex = p->tex; S.qfmt = fmt;
        S.quads[S.nquads++] = q;
    }
}

static void mirror_fill(float x, float y, float w, float h)
{
    if (S.skip_rects) return;
    x *= S.rsx; y *= S.rsy; w *= S.rsx; h *= S.rsy;
    if (S.target) { SDL_Rect d = { (int)x, (int)y, (int)w, (int)h }; record_op(NULL, NULL, &d, 0, S.dr, S.dg, S.db, draw_alpha()); return; }
    if (!drawing()) return;
    flush_quads();
    pspexp_fill_rect(S.ctx, X(x), Y(y), W(w), H(h), draw_color5650(), draw_alpha());
}

static void mirror_line(float x0, float y0, float x1, float y1)
{
    if (S.skip_lines || !drawing()) return;
    x0 *= S.rsx; y0 *= S.rsy; x1 *= S.rsx; y1 *= S.rsy;
    flush_quads();
    pspexp_draw_line(S.ctx, X(x0), Y(y0), X(x1), Y(y1), draw_color5650(), draw_alpha());
}

/* ---- audio ----------------------------------------------------------------- */

static void audio_push(const int16_t *stereo, size_t frames)
{
    size_t f;
    SDL_LockMutex(S.amu);
    for (f = 0; f < frames && S.abuf < S.acap; f++) {
        S.aring[S.atail * 2] = stereo[f * 2];
        S.aring[S.atail * 2 + 1] = stereo[f * 2 + 1];
        S.atail = (S.atail + 1) % S.acap;
        S.abuf++;
    }
    SDL_UnlockMutex(S.amu);
}

static void audio_tap(int slot, const Uint8 *data, int len)
{
    SDL_AudioStream *st = S.dev_stream[slot];
    int16_t tmp[8192];
    int got;
    if (!st) return;
    SDL_AudioStreamPut(st, data, len);
    while ((got = SDL_AudioStreamGet(st, tmp, (int)sizeof(tmp))) > 0) audio_push(tmp, (size_t)got / 4);
}

static void audio_callback_wrapper(void *userdata, Uint8 *stream, int len)
{
    int slot = (int)(intptr_t)userdata;
    if (S.dev_selfpaced[slot]) { memset(stream, 0, (size_t)len); return; }   /* our thread feeds the game's callback */
    if (S.dev_cb[slot]) S.dev_cb[slot](S.dev_ud[slot], stream, len);
    else memset(stream, 0, (size_t)len);
    audio_tap(slot, stream, len);
    if (!S.local_audio) memset(stream, 0, (size_t)len);
}

static void audio_drain(void)
{
    static int16_t chunk[4096 * 2];
    size_t n, f;
    if (!link_up() || !S.amu) return;
    if (S.skip_audio) { SDL_LockMutex(S.amu); S.abuf = 0; S.ahead = S.atail; SDL_UnlockMutex(S.amu); return; }
    for (;;) {
        SDL_LockMutex(S.amu);
        n = S.abuf < 4096 ? S.abuf : 4096;
        for (f = 0; f < n; f++) {
            chunk[f * 2] = S.aring[S.ahead * 2];
            chunk[f * 2 + 1] = S.aring[S.ahead * 2 + 1];
            S.ahead = (S.ahead + 1) % S.acap;
        }
        S.abuf -= n;
        SDL_UnlockMutex(S.amu);
        if (n == 0) break;
        pspexp_audio_stream(S.ctx, chunk, (uint32_t)n);
    }
}

/* Calls the game's audio callback at exactly the device rate, under the
 * device lock the game may also take (SDL_LockAudioDevice), while the real
 * (dummy) device only outputs silence. Started for the first self-paced slot. */
static int audio_pacer(void *arg)
{
    Uint64 f = SDL_GetPerformanceFrequency(), t0 = SDL_GetPerformanceCounter();
    Uint64 produced[8] = { 0 };
    Uint8 *buf = NULL; int buflen = 0;
    (void)arg;
    while (!S.audio_thread_stop) {
        int slot;
        double elapsed = (double)(SDL_GetPerformanceCounter() - t0) / (double)f;
        for (slot = 1; slot < 8; slot++) {
            const SDL_AudioSpec *sp = &S.dev_spec[slot];
            Uint64 target;
            int bytes;
            if (!S.dev_selfpaced[slot] || !S.dev_cb[slot] || !sp->freq) continue;
            if (SDL_GetAudioDeviceStatus(S.dev_id[slot]) != SDL_AUDIO_PLAYING) { produced[slot] = (Uint64)(elapsed * sp->freq); continue; }
            target = (Uint64)(elapsed * sp->freq);
            bytes = sp->samples * sp->channels * (int)(SDL_AUDIO_BITSIZE(sp->format) / 8);
            if (bytes > buflen) { free(buf); buf = (Uint8 *)malloc((size_t)bytes); buflen = bytes; }
            if (!buf) break;
            /* never more than ~120 ms ahead (after a stall), never behind */
            if (produced[slot] + (Uint64)sp->freq / 8 < target) produced[slot] = target - (Uint64)sp->freq / 8;
            while (produced[slot] < target) {
                SDL_LockAudioDevice(S.dev_id[slot]);
                S.dev_cb[slot](S.dev_ud[slot], buf, bytes);
                SDL_UnlockAudioDevice(S.dev_id[slot]);
                audio_tap(slot, buf, bytes);
                produced[slot] += (Uint64)sp->samples;
            }
        }
        SDL_Delay(2);
    }
    free(buf);
    return 0;
}

static int want_selfpaced(void)
{
    const char *drv = SDL_GetCurrentAudioDriver(), *env = getenv("PSPEXP_AUDIO");
    if (env) return !strcmp(env, "psp");
    return drv && !strcmp(drv, "dummy");
}

static void start_selfpaced(int slot, SDL_AudioDeviceID id)
{
    S.dev_selfpaced[slot] = 1;
    S.dev_id[slot] = id;
    if (!S.audio_thread) S.audio_thread = SDL_CreateThread(audio_pacer, "pspexp-audio", NULL);
    logf_("audio device %u: callback paced by the shim (host driver '%s')", (unsigned)id, SDL_GetCurrentAudioDriver());
}

static int setup_stream(int slot, const SDL_AudioSpec *have)
{
    if (slot < 0 || slot >= 8) return -1;
    if (S.dev_stream[slot]) SDL_FreeAudioStream(S.dev_stream[slot]);
    S.dev_spec[slot] = *have;
    S.dev_stream[slot] = SDL_NewAudioStream(have->format, have->channels, have->freq, AUDIO_S16SYS, 2, 44100);
    return S.dev_stream[slot] ? 0 : -1;
}

/* ---- input ------------------------------------------------------------------ */

static void push_key(SDL_Keycode sym, int down)
{
    SDL_Event ev;
    if (sym == SDLK_UNKNOWN) return;
    SDL_zero(ev);
    ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    ev.key.keysym.sym = sym;
    ev.key.keysym.scancode = SDL_GetScancodeFromKey(sym);
    ev.key.windowID = S.win ? SDL_GetWindowID(S.win) : 0;
    SDL_PushEvent(&ev);
}

static void push_mouse_button(Uint8 button, int down)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    ev.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    ev.button.button = button;
    ev.button.clicks = 1;
    ev.button.x = (int)S.px; ev.button.y = (int)S.py;
    ev.button.windowID = S.win ? SDL_GetWindowID(S.win) : 0;
    SDL_PushEvent(&ev);
}

static void setup_vjoy(void)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    char guid[64], mapping[256];
    if (S.vjoy >= 0) return;
    if (!SDL_WasInit(SDL_INIT_JOYSTICK)) return;   /* the game did not ask for joysticks */
    S.vjoy = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 2, 12, 1);
    if (S.vjoy < 0) { logf_("virtual joystick: %s", SDL_GetError()); return; }
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(S.vjoy), guid, sizeof(guid));
    snprintf(mapping, sizeof(mapping),
             "%s,PSP (PSPExp),a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,start:b6,back:b7,"
             "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1", guid);
    SDL_GameControllerAddMapping(mapping);
    S.vjoy_handle = SDL_JoystickOpen(S.vjoy);
    logf_("virtual game controller attached (%s)", guid);
#endif
}

static void feed_input(void)
{
    const PspExpInput *in = pspexp_input(S.ctx);
    uint32_t pressed, released;
    int i;
    if (in->sequence == 0 && in->timestamp_us == 0) return;
    pressed = in->buttons & ~S.prev_buttons;
    released = S.prev_buttons & ~in->buttons;
    S.prev_buttons = in->buttons;

    setup_vjoy();
    if (S.vjoy_handle) {
        Uint8 hat = 0;
        SDL_JoystickSetVirtualAxis(S.vjoy_handle, 0, (Sint16)(((int)in->analog_x - 128) * 258));
        SDL_JoystickSetVirtualAxis(S.vjoy_handle, 1, (Sint16)(((int)in->analog_y - 128) * 258));
        for (i = 0; i < 8; i++) SDL_JoystickSetVirtualButton(S.vjoy_handle, i, (in->buttons & BUTTON_BITS[i]) ? 1 : 0);
        if (in->buttons & PSPEXP_BUTTON_UP) hat |= SDL_HAT_UP;
        if (in->buttons & PSPEXP_BUTTON_DOWN) hat |= SDL_HAT_DOWN;
        if (in->buttons & PSPEXP_BUTTON_LEFT) hat |= SDL_HAT_LEFT;
        if (in->buttons & PSPEXP_BUTTON_RIGHT) hat |= SDL_HAT_RIGHT;
        SDL_JoystickSetVirtualHat(S.vjoy_handle, 0, hat);
    }
    for (i = 0; i < B_COUNT; i++) {
        if (S.nub_mouse && (i == B_CROSS || i == B_CIRCLE)) continue;
        if (pressed & BUTTON_BITS[i]) push_key(S.keys[i], 1);
        if (released & BUTTON_BITS[i]) push_key(S.keys[i], 0);
    }
    if (S.nub_keys) {
        /* The nub as the four arrow keys, with hysteresis so a held diagonal stays held. */
        int ax = (int)in->analog_x - 128, ay = (int)in->analog_y - 128;
        uint32_t dirs = 0, changed;
        if (ax < -50 || (ax < -30 && (S.nub_dirs & PSPEXP_BUTTON_LEFT))) dirs |= PSPEXP_BUTTON_LEFT;
        if (ax > 50 || (ax > 30 && (S.nub_dirs & PSPEXP_BUTTON_RIGHT))) dirs |= PSPEXP_BUTTON_RIGHT;
        if (ay < -50 || (ay < -30 && (S.nub_dirs & PSPEXP_BUTTON_UP))) dirs |= PSPEXP_BUTTON_UP;
        if (ay > 50 || (ay > 30 && (S.nub_dirs & PSPEXP_BUTTON_DOWN))) dirs |= PSPEXP_BUTTON_DOWN;
        changed = dirs ^ S.nub_dirs;
        S.nub_dirs = dirs;
        if (changed & PSPEXP_BUTTON_LEFT) push_key(S.keys[B_LEFT], (dirs & PSPEXP_BUTTON_LEFT) != 0);
        if (changed & PSPEXP_BUTTON_RIGHT) push_key(S.keys[B_RIGHT], (dirs & PSPEXP_BUTTON_RIGHT) != 0);
        if (changed & PSPEXP_BUTTON_UP) push_key(S.keys[B_UP], (dirs & PSPEXP_BUTTON_UP) != 0);
        if (changed & PSPEXP_BUTTON_DOWN) push_key(S.keys[B_DOWN], (dirs & PSPEXP_BUTTON_DOWN) != 0);
    }
    if (S.nub_mouse) {
        Uint32 now = SDL_GetTicks();
        float dt = (now - S.ptick > 100 ? 100 : now - S.ptick) / 1000.0f;
        int ax = (int)in->analog_x - 128, ay = (int)in->analog_y - 128;
        S.ptick = now;
        if (abs(ax) > 35 || abs(ay) > 35) {
            float fx = ax / 127.0f, fy = ay / 127.0f, speed = 300.0f * dt;
            SDL_Event ev;
            S.px = fminf(fmaxf(S.px + fx * fabsf(fx) * speed, 0), (float)(S.win_w - 1));
            S.py = fminf(fmaxf(S.py + fy * fabsf(fy) * speed, 0), (float)(S.win_h - 1));
            SDL_zero(ev);
            ev.type = SDL_MOUSEMOTION; ev.motion.x = (int)S.px; ev.motion.y = (int)S.py;
            ev.motion.windowID = S.win ? SDL_GetWindowID(S.win) : 0;
            SDL_PushEvent(&ev);
        }
        if (pressed & PSPEXP_BUTTON_CROSS) push_mouse_button(SDL_BUTTON_LEFT, 1);
        if (released & PSPEXP_BUTTON_CROSS) push_mouse_button(SDL_BUTTON_LEFT, 0);
        if (pressed & PSPEXP_BUTTON_CIRCLE) push_mouse_button(SDL_BUTTON_RIGHT, 1);
        if (released & PSPEXP_BUTTON_CIRCLE) push_mouse_button(SDL_BUTTON_RIGHT, 0);
    }
}

/* ---- hooks: window / renderer ------------------------------------------------ */

HOOK(SDL_Window *, SDL_CreateWindow, (const char *title, int x, int y, int w, int h, Uint32 flags))
{
    SDL_Window *win = REAL(SDL_CreateWindow)(title, x, y, w, h, flags);
    if (win && !S.win) { S.win = win; S.win_w = w; S.win_h = h; }
    return win;
}

HOOK(SDL_Renderer *, SDL_CreateRenderer, (SDL_Window *win, int index, Uint32 flags))
{
    SDL_Renderer *r = REAL(SDL_CreateRenderer)(win, index, flags);
    if (r && !S.ren) {
        S.ren = r;
        if (!S.win) { S.win = win; SDL_GetWindowSize(win, &S.win_w, &S.win_h); }
        connect_once();
        update_scale();
        if (S.hide_window && S.active) { SDL_SetWindowFullscreen(win, 0); SDL_HideWindow(win); }
    }
    return r;
}

HOOK(int, SDL_CreateWindowAndRenderer, (int w, int h, Uint32 flags, SDL_Window **win, SDL_Renderer **ren))
{
    int rc = REAL(SDL_CreateWindowAndRenderer)(w, h, flags, win, ren);
    if (rc == 0 && !S.ren && win && ren) {
        S.win = *win; S.ren = *ren; S.win_w = w; S.win_h = h;
        connect_once();
        update_scale();
        if (S.hide_window && S.active) { SDL_SetWindowFullscreen(*win, 0); SDL_HideWindow(*win); }
    }
    return rc;
}

HOOK(int, SDL_RenderSetLogicalSize, (SDL_Renderer *r, int w, int h))
{
    int rc = REAL(SDL_RenderSetLogicalSize)(r, w, h);
    if (r == S.ren) { S.log_w = w; S.log_h = h; update_scale(); }
    return rc;
}

HOOK(int, SDL_RenderSetViewport, (SDL_Renderer *r, const SDL_Rect *rect))
{
    int rc = REAL(SDL_RenderSetViewport)(r, rect);
    if (r == S.ren) { S.has_viewport = rect != NULL; if (rect) S.viewport = *rect; }
    return rc;
}

HOOK(int, SDL_RenderSetScale, (SDL_Renderer *r, float sx, float sy))
{
    int rc = REAL(SDL_RenderSetScale)(r, sx, sy);
    if (r == S.ren) { S.rsx = sx > 0 ? sx : 1.0f; S.rsy = sy > 0 ? sy : 1.0f; }
    return rc;
}

HOOK(int, SDL_RenderSetClipRect, (SDL_Renderer *r, const SDL_Rect *rect))
{
    int rc = REAL(SDL_RenderSetClipRect)(r, rect);
    if (r == S.ren) { S.has_clip = rect != NULL; if (rect) S.clip = *rect; }
    return rc;
}

HOOK(int, SDL_SetRenderTarget, (SDL_Renderer *r, SDL_Texture *t))
{
    int rc = REAL(SDL_SetRenderTarget)(r, t);
    if (r == S.ren && t != S.target) {
        flush_quads();
        if (t && !S.target) {   /* SDL resets viewport, clip and scale for the target and restores them after */
            S.backup.viewport = S.viewport; S.backup.has_viewport = S.has_viewport;
            S.backup.clip = S.clip; S.backup.has_clip = S.has_clip;
            S.backup.rsx = S.rsx; S.backup.rsy = S.rsy;
            S.has_viewport = 0; S.has_clip = 0; S.rsx = S.rsy = 1.0f;
        } else if (!t && S.target) {
            S.viewport = S.backup.viewport; S.has_viewport = S.backup.has_viewport;
            S.clip = S.backup.clip; S.has_clip = S.backup.has_clip;
            S.rsx = S.backup.rsx; S.rsy = S.backup.rsy;
        }
        S.target = t;
        if (t) { Tex *x = find_tex(t); if (x && x->is_target) x->nops = 0; }   /* a new pass replaces the last */
    }
    return rc;
}

HOOK(int, SDL_SetRenderDrawColor, (SDL_Renderer *r, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca))
{
    if (r == S.ren) { S.dr = cr; S.dg = cg; S.db = cb; S.da = ca; }
    return REAL(SDL_SetRenderDrawColor)(r, cr, cg, cb, ca);
}

HOOK(int, SDL_SetRenderDrawBlendMode, (SDL_Renderer *r, SDL_BlendMode m))
{
    if (r == S.ren) S.dblend = m;
    return REAL(SDL_SetRenderDrawBlendMode)(r, m);
}

/* ---- hooks: textures --------------------------------------------------------- */

HOOK(SDL_Texture *, SDL_CreateTexture, (SDL_Renderer *r, Uint32 format, int access, int w, int h))
{
    SDL_Texture *t = REAL(SDL_CreateTexture)(r, format, access, w, h);
    if (t && r == S.ren) {
        Tex *x = add_tex(t, w, h);
        if (x && access == SDL_TEXTUREACCESS_TARGET) x->is_target = 1;
        if (x && access == SDL_TEXTUREACCESS_STREAMING) x->streaming = 1;
        if (x) x->blend = SDL_BLENDMODE_NONE;   /* SDL's default for SDL_CreateTexture */
    }
    return t;
}

HOOK(SDL_Texture *, SDL_CreateTextureFromSurface, (SDL_Renderer *r, SDL_Surface *surf))
{
    SDL_Texture *t = REAL(SDL_CreateTextureFromSurface)(r, surf);
    if (t && r == S.ren && surf) {
        Tex *x = add_tex(t, surf->w, surf->h);
        if (x) {
            SDL_Surface *conv = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ABGR8888, 0);
            if (conv) {
                int y;
                for (y = 0; y < surf->h; y++)
                    memcpy(x->rgba + (size_t)y * surf->w, (const Uint8 *)conv->pixels + (size_t)y * conv->pitch, (size_t)surf->w * 4);
                SDL_FreeSurface(conv);
            }
            x->dirty = 1;
            SDL_GetTextureBlendMode(t, &x->blend);   /* BLEND when the surface had alpha or a colour key */
        }
    }
    return t;
}

HOOK(int, SDL_SetTextureBlendMode, (SDL_Texture *t, SDL_BlendMode mode))
{
    Tex *x = find_tex(t);
    if (x && x->blend != mode) { x->blend = mode; x->dirty = 1; }   /* format changes: re-upload */
    return REAL(SDL_SetTextureBlendMode)(t, mode);
}

HOOK(void, SDL_DestroyTexture, (SDL_Texture *t))
{
    Tex **pp = &S.textures;
    while (*pp) {
        if ((*pp)->sdl == t) {
            Tex *x = *pp;
            *pp = x->next;
            if (link_up()) release_pages(x);
            free(x->rgba);
            free(x->ops);
            free(x);
            break;
        }
        pp = &(*pp)->next;
    }
    REAL(SDL_DestroyTexture)(t);
}

HOOK(int, SDL_UpdateTexture, (SDL_Texture *t, const SDL_Rect *rect, const void *pixels, int pitch))
{
    Tex *x = find_tex(t);
    if (x && !x->unsupported) {
        Uint32 format = 0;
        SDL_QueryTexture(t, &format, NULL, NULL, NULL);
        shadow_update(x, rect, format, pixels, pitch);
    }
    return REAL(SDL_UpdateTexture)(t, rect, pixels, pitch);
}

HOOK(int, SDL_LockTexture, (SDL_Texture *t, const SDL_Rect *rect, void **pixels, int *pitch))
{
    int rc = REAL(SDL_LockTexture)(t, rect, pixels, pitch);
    Tex *x = find_tex(t);
    if (rc == 0 && x) {
        x->locked = 1;
        x->lock_pixels = *pixels; x->lock_pitch = *pitch;
        x->lock_rect = rect ? *rect : (SDL_Rect){ 0, 0, x->w, x->h };
    }
    return rc;
}

HOOK(void, SDL_UnlockTexture, (SDL_Texture *t))
{
    Tex *x = find_tex(t);
    if (x && x->locked && !x->unsupported) {
        Uint32 format = 0;
        SDL_QueryTexture(t, &format, NULL, NULL, NULL);
        shadow_update(x, &x->lock_rect, format, x->lock_pixels, x->lock_pitch);
        x->locked = 0;
    }
    REAL(SDL_UnlockTexture)(t);
}

HOOK(int, SDL_SetTextureColorMod, (SDL_Texture *t, Uint8 cr, Uint8 cg, Uint8 cb))
{
    Tex *x = find_tex(t);
    if (x) { x->r = cr; x->g = cg; x->b = cb; }
    return REAL(SDL_SetTextureColorMod)(t, cr, cg, cb);
}

HOOK(int, SDL_SetTextureAlphaMod, (SDL_Texture *t, Uint8 alpha))
{
    Tex *x = find_tex(t);
    if (x) x->a = alpha;
    return REAL(SDL_SetTextureAlphaMod)(t, alpha);
}

/* ---- hooks: drawing ---------------------------------------------------------- */

HOOK(int, SDL_RenderClear, (SDL_Renderer *r))
{
    if (r == S.ren && S.target) {
        Tex *x = target_tex();
        if (x && x->is_target) { x->nops = 0; record_op(NULL, NULL, NULL, 0, S.dr, S.dg, S.db, 255); }
    } else if (r == S.ren && link_up() && !S.target && !S.skip_clear) {
        begin_frame();
        flush_quads();
        /* clear covers the whole PSP screen including the letterbox bars */
        S.clip_sent = 0;
        pspexp_set_clip(S.ctx, 0, 0, 480, 272);
        pspexp_fill_rect(S.ctx, 0, 0, 480, 272, draw_color5650(), 255);
    }
    return REAL(SDL_RenderClear)(r);
}

static SDL_Rect full_dst(void)
{
    Tex *t = target_tex();
    if (t) return (SDL_Rect){ 0, 0, t->w, t->h };
    return (SDL_Rect){ 0, 0, S.log_w > 0 ? S.log_w : S.win_w, S.log_h > 0 ? S.log_h : S.win_h };
}

HOOK(int, SDL_RenderCopy, (SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_Rect *dst))
{
    if (r == S.ren) {
        Tex *x = find_tex(t);
        SDL_Rect d = dst ? *dst : full_dst();
        if (x) mirror_copy(t, src, (float)d.x, (float)d.y, (float)d.w, (float)d.h, 0);
    }
    return REAL(SDL_RenderCopy)(r, t, src, dst);
}

HOOK(int, SDL_RenderCopyEx, (SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_Rect *dst, double angle, const SDL_Point *center, SDL_RendererFlip flip))
{
    if (r == S.ren) {
        Tex *x = find_tex(t);
        SDL_Rect d = dst ? *dst : full_dst();
        if (x) mirror_copy(t, src, (float)d.x, (float)d.y, (float)d.w, (float)d.h, (int)flip);
    }
    return REAL(SDL_RenderCopyEx)(r, t, src, dst, angle, center, flip);
}

HOOK(int, SDL_RenderCopyF, (SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_FRect *dst))
{
    if (r == S.ren && dst) mirror_copy(t, src, dst->x, dst->y, dst->w, dst->h, 0);
    else if (r == S.ren) { SDL_Rect d = full_dst(); mirror_copy(t, src, (float)d.x, (float)d.y, (float)d.w, (float)d.h, 0); }
    return REAL(SDL_RenderCopyF)(r, t, src, dst);
}

HOOK(int, SDL_RenderCopyExF, (SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_FRect *dst, double angle, const SDL_FPoint *center, SDL_RendererFlip flip))
{
    if (r == S.ren && dst) mirror_copy(t, src, dst->x, dst->y, dst->w, dst->h, (int)flip);
    return REAL(SDL_RenderCopyExF)(r, t, src, dst, angle, center, flip);
}

HOOK(int, SDL_RenderFillRect, (SDL_Renderer *r, const SDL_Rect *rect))
{
    if (r == S.ren) {
        if (rect) mirror_fill((float)rect->x, (float)rect->y, (float)rect->w, (float)rect->h);
        else { SDL_Rect d = full_dst(); mirror_fill((float)d.x, (float)d.y, (float)d.w, (float)d.h); }
    }
    return REAL(SDL_RenderFillRect)(r, rect);
}

HOOK(int, SDL_RenderFillRects, (SDL_Renderer *r, const SDL_Rect *rects, int count))
{
    int i;
    if (r == S.ren && rects) for (i = 0; i < count; i++) mirror_fill((float)rects[i].x, (float)rects[i].y, (float)rects[i].w, (float)rects[i].h);
    return REAL(SDL_RenderFillRects)(r, rects, count);
}

HOOK(int, SDL_RenderFillRectF, (SDL_Renderer *r, const SDL_FRect *rect))
{
    if (r == S.ren && rect) mirror_fill(rect->x, rect->y, rect->w, rect->h);
    return REAL(SDL_RenderFillRectF)(r, rect);
}

HOOK(int, SDL_RenderDrawRect, (SDL_Renderer *r, const SDL_Rect *rect))
{
    if (r == S.ren && rect) {
        mirror_fill((float)rect->x, (float)rect->y, (float)rect->w, 1);
        mirror_fill((float)rect->x, (float)(rect->y + rect->h - 1), (float)rect->w, 1);
        mirror_fill((float)rect->x, (float)rect->y, 1, (float)rect->h);
        mirror_fill((float)(rect->x + rect->w - 1), (float)rect->y, 1, (float)rect->h);
    }
    return REAL(SDL_RenderDrawRect)(r, rect);
}

HOOK(int, SDL_RenderDrawLine, (SDL_Renderer *r, int x1, int y1, int x2, int y2))
{
    if (r == S.ren) mirror_line((float)x1, (float)y1, (float)x2, (float)y2);
    return REAL(SDL_RenderDrawLine)(r, x1, y1, x2, y2);
}

HOOK(int, SDL_RenderDrawLines, (SDL_Renderer *r, const SDL_Point *points, int count))
{
    int i;
    if (r == S.ren && points) for (i = 1; i < count; i++) mirror_line((float)points[i - 1].x, (float)points[i - 1].y, (float)points[i].x, (float)points[i].y);
    return REAL(SDL_RenderDrawLines)(r, points, count);
}

HOOK(int, SDL_RenderDrawPoint, (SDL_Renderer *r, int x, int y))
{
    if (r == S.ren) mirror_fill((float)x, (float)y, 1, 1);
    return REAL(SDL_RenderDrawPoint)(r, x, y);
}

HOOK(int, SDL_RenderDrawPoints, (SDL_Renderer *r, const SDL_Point *points, int count))
{
    int i;
    if (r == S.ren && points) for (i = 0; i < count; i++) mirror_fill((float)points[i].x, (float)points[i].y, 1, 1);
    return REAL(SDL_RenderDrawPoints)(r, points, count);
}

/* Input and audio must not wait for the next present: a menu that redraws
 * once a second would lose every press and let the audio pile up. Games
 * poll events every loop iteration, so the link is serviced from there. */
static void service_link(void)
{
    static Uint32 last;
    Uint32 now = SDL_GetTicks();
    if (!S.active || !link_up() || now == last) return;
    last = now;
    pspexp_pump(S.ctx, 0);
    feed_input();
    audio_drain();
}

static void end_frame(void)
{
    if (!S.active) return;
    if (S.win) {   /* programs resize their window after creating it (scalers, settings) */
        int w = 0, h = 0;
        SDL_GetWindowSize(S.win, &w, &h);
        if (w > 0 && h > 0 && (w != S.win_w || h != S.win_h)) { S.win_w = w; S.win_h = h; update_scale(); }
    }
    if (!link_up()) { S.frame_open = 0; S.nquads = 0; try_reconnect(); return; }
    begin_frame();
    flush_quads();
    audio_drain();
    pspexp_wait_present(S.ctx, 40);
    pspexp_present(S.ctx);
    S.frame_open = 0;
    pspexp_pump(S.ctx, 0);
    feed_input();
    S.frames++;
    S.total_frames++;
    if (S.shot_every && S.total_frames % S.shot_every == 0) pspexp_request_screenshot(S.ctx, (uint16_t)S.shot_index++);
    {
        Uint32 now = SDL_GetTicks();
        if (now - S.last_report >= 2000) {
            const PspExpStats *st = pspexp_stats(S.ctx);
            const PspExpAudioStatus *au = pspexp_audio_status(S.ctx);
            logf_("%.1f fps  %u B/frame  %u batches  %u sprites  psp cpu %u us  ge %u us  uploads %u  errors %u  audio buffered %u underruns %u",
                  S.frames * 1000.0 / (double)(now - S.last_report), st->last_frame_bytes, st->last_frame_batches,
                  st->last_frame_sprites, st->last_execute_us, st->last_ge_us, S.uploads,
                  st->resource_errors + st->protocol_errors, au->buffered_frames, au->underruns);
            S.last_report = now; S.frames = 0; S.uploads = 0;
        }
    }
}

HOOK(void, SDL_RenderPresent, (SDL_Renderer *r))
{
    REAL(SDL_RenderPresent)(r);
    if (r == S.ren) end_frame();
}

/* ---- hooks: event polling ------------------------------------------------------ */

/* With the PSP as the screen, the host window is usually not the focused
 * one; games that pause or mute on focus loss would stay paused. Hide focus
 * loss from them (PSPEXP_FOCUS=host keeps SDL's real focus events). */
static int hide_focus_loss(void)
{
    static int checked, hide;
    if (!checked) { checked = 1; hide = S.active && !(getenv("PSPEXP_FOCUS") && !strcmp(getenv("PSPEXP_FOCUS"), "host")); }
    return hide;
}

static int drop_event(const SDL_Event *ev)
{
    if (!hide_focus_loss()) return 0;
    if (ev->type == SDL_WINDOWEVENT &&
        (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST || ev->window.event == SDL_WINDOWEVENT_MINIMIZED ||
         ev->window.event == SDL_WINDOWEVENT_HIDDEN || ev->window.event == SDL_WINDOWEVENT_LEAVE))
        return 1;
    return 0;
}

static void fake_focus_once(void)
{
    static int done;
    SDL_Event ev;
    if (done || !hide_focus_loss() || !S.win) return;
    done = 1;
    SDL_zero(ev);
    ev.type = SDL_WINDOWEVENT; ev.window.event = SDL_WINDOWEVENT_FOCUS_GAINED; ev.window.windowID = SDL_GetWindowID(S.win);
    SDL_PushEvent(&ev);
}

HOOK(int, SDL_PollEvent, (SDL_Event *ev))
{
    int rc;
    service_link();
    fake_focus_once();
    while ((rc = REAL(SDL_PollEvent)(ev)) == 1 && ev && drop_event(ev)) ;
    return rc;
}

static int wait_event_impl(SDL_Event *ev, int timeout)
{
    /* wait in short steps so the PSP's input still arrives while the game blocks */
    Uint32 start = SDL_GetTicks();
    for (;;) {
        int step, rc;
        service_link();
        step = timeout < 0 ? 20 : (int)(timeout - (SDL_GetTicks() - start));
        if (timeout >= 0 && step <= 0) return REAL(SDL_PollEvent)(ev);
        if (step > 20) step = 20;
        rc = REAL(SDL_WaitEventTimeout)(ev, step);
        if (rc && ev && drop_event(ev)) continue;
        if (rc) return rc;
    }
}

HOOK(int, SDL_WaitEventTimeout, (SDL_Event *ev, int timeout)) { return wait_event_impl(ev, timeout); }
HOOK(int, SDL_WaitEvent, (SDL_Event *ev)) { return wait_event_impl(ev, -1); }

/* ---- hooks: window surface (programs without a renderer) --------------------- */

static void stream_surface(SDL_Window *win)
{
    SDL_Surface *surf = SDL_GetWindowSurface(win);
    Tex *x;
    if (!surf) return;
    if (!S.inited) { S.win = win; S.win_w = surf->w; S.win_h = surf->h; connect_once(); update_scale(); }
    if (!link_up()) { end_frame(); return; }
    if (S.surface_tex && (S.win_w != surf->w || S.win_h != surf->h)) {
        release_pages(S.surface_tex); free(S.surface_tex->rgba); free(S.surface_tex); S.surface_tex = NULL;
    }
    if (!S.surface_tex) {
        /* not on the texture list: it never comes through the SDL_Texture hooks */
        S.surface_tex = (Tex *)calloc(1, sizeof(Tex));
        if (!S.surface_tex) return;
        S.surface_tex->w = surf->w; S.surface_tex->h = surf->h;
        S.surface_tex->rgba = (uint32_t *)calloc((size_t)surf->w * surf->h, 4);
        S.surface_tex->r = S.surface_tex->g = S.surface_tex->b = S.surface_tex->a = 255;
        S.surface_tex->streaming = 1;
        S.win_w = surf->w; S.win_h = surf->h; S.log_w = S.log_h = 0;
        update_scale();
    }
    x = S.surface_tex;
    /* Convert, then shrink to the PSP size with nearest-neighbour sampling so
     * the upload is at most 480 x 272 (~270 KB), whatever the window size. */
    if (SDL_MUSTLOCK(surf)) SDL_LockSurface(surf);
    SDL_ConvertPixels(surf->w, surf->h, surf->format->format, surf->pixels, surf->pitch, SDL_PIXELFORMAT_ABGR8888, x->rgba, surf->w * 4);
    if (SDL_MUSTLOCK(surf)) SDL_UnlockSurface(surf);
    if (S.sx < 1.0f) {
        int dw = (int)(surf->w * S.sx + 0.5f), dh = (int)(surf->h * S.sy + 0.5f), yy, xx;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        for (yy = 0; yy < dh; yy++) {
            const uint32_t *src = x->rgba + (size_t)(yy * surf->h / dh) * surf->w;
            uint32_t *dst = x->rgba + (size_t)yy * dw;
            for (xx = 0; xx < dw; xx++) dst[xx] = src[xx * surf->w / dw];
        }
        x->w = dw; x->h = dh;
    }
    x->dirty = 1;
    begin_frame();
    S.has_clip = 0;
    apply_clip();
    /* Black letterbox bars (and, under PPSSPP, a frame made only of textured
     * quads never reaches its framebuffer: the flat rect first makes it show). */
    pspexp_fill_rect(S.ctx, 0, 0, 480, 272, 0, 255);
    if (upload_tex(x) == 0) {
        int i;
        uint16_t fmt, color = tint_for(x, &fmt);
        int shrunk = S.sx < 1.0f;
        for (i = 0; i < x->npages; i++) {
            const Page *p = &x->pages[i];
            PspExpQuadExRec q;
            memset(&q, 0, sizeof(q));
            if (shrunk) { q.x = (int16_t)(S.ox + p->x0); q.y = (int16_t)(S.oy + p->y0); q.w = (int16_t)p->w; q.h = (int16_t)p->h; }
            else { q.x = X((float)p->x0); q.y = Y((float)p->y0); q.w = W((float)p->w); q.h = H((float)p->h); }
            q.u0 = 0; q.v0 = 0; q.u1 = (uint16_t)p->w; q.v1 = (uint16_t)p->h; q.color = color;
            if (S.nquads && S.qtex != p->tex) flush_quads();
            S.qtex = p->tex; S.qfmt = fmt;
            S.quads[S.nquads++] = q;
        }
    }
    end_frame();
}

HOOK(int, SDL_UpdateWindowSurface, (SDL_Window *win))
{
    int rc = REAL(SDL_UpdateWindowSurface)(win);
    if (!S.ren) stream_surface(win);
    return rc;
}

HOOK(int, SDL_UpdateWindowSurfaceRects, (SDL_Window *win, const SDL_Rect *rects, int numrects))
{
    int rc = REAL(SDL_UpdateWindowSurfaceRects)(win, rects, numrects);
    if (!S.ren) stream_surface(win);
    return rc;
}

/* ---- hooks: audio ------------------------------------------------------------ */

HOOK(SDL_AudioDeviceID, SDL_OpenAudioDevice, (const char *device, int iscapture, const SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int allowed))
{
    SDL_AudioSpec want, have;
    SDL_AudioDeviceID id;
    int slot;
    if (iscapture || !desired) return REAL(SDL_OpenAudioDevice)(device, iscapture, desired, obtained, allowed);
    connect_once();
    for (slot = 1; slot < 8; slot++) if (!S.dev_stream[slot]) break;
    if (slot == 8) return REAL(SDL_OpenAudioDevice)(device, iscapture, desired, obtained, allowed);
    want = *desired;
    S.dev_cb[slot] = desired->callback;
    S.dev_ud[slot] = desired->userdata;
    if (desired->callback) { want.callback = audio_callback_wrapper; want.userdata = (void *)(intptr_t)slot; }
    id = REAL(SDL_OpenAudioDevice)(device, iscapture, &want, &have, allowed);
    if (id) {
        setup_stream(slot, &have);
        if (obtained) { *obtained = have; obtained->callback = desired->callback; obtained->userdata = desired->userdata; }
        logf_("audio device %u tapped: %d Hz x%d fmt %#x", (unsigned)id, have.freq, have.channels, have.format);
        if (desired->callback && want_selfpaced()) start_selfpaced(slot, id);
    }
    return id;
}

HOOK(int, SDL_OpenAudio, (SDL_AudioSpec *desired, SDL_AudioSpec *obtained))
{
    /* SDL_OpenAudio is a wrapper around device 1 */
    SDL_AudioSpec have;
    int rc;
    connect_once();
    if (!desired) return REAL(SDL_OpenAudio)(desired, obtained);
    S.dev_cb[1] = desired->callback;
    S.dev_ud[1] = desired->userdata;
    if (desired->callback) { desired->callback = audio_callback_wrapper; desired->userdata = (void *)(intptr_t)1; }
    rc = REAL(SDL_OpenAudio)(desired, obtained ? obtained : &have);
    if (rc == 0) setup_stream(1, obtained ? obtained : &have);
    if (rc == 0 && S.dev_cb[1] && want_selfpaced()) start_selfpaced(1, 1);
    desired->callback = S.dev_cb[1]; desired->userdata = S.dev_ud[1];
    if (obtained) { obtained->callback = S.dev_cb[1]; obtained->userdata = S.dev_ud[1]; }
    return rc;
}

HOOK(int, SDL_QueueAudio, (SDL_AudioDeviceID dev, const void *data, Uint32 len))
{
    int slot = -1, i;
    for (i = 1; i < 8; i++) if (S.dev_stream[i]) { slot = i; break; }   /* single queued device is the common case */
    if (slot > 0) audio_tap(slot, (const Uint8 *)data, (int)len);
    if (!S.local_audio) return 0;
    return REAL(SDL_QueueAudio)(dev, data, len);
}

/* ---- shutdown --------------------------------------------------------------- */

/* Registered with atexit() once connected, so it runs before the USB
 * transport's own atexit handler (libusb_exit) and after the game's. */
static void shim_exit(void)
{
    logf_("exit: %lu frames mirrored", S.total_frames);
    if (S.audio_thread) { S.audio_thread_stop = 1; SDL_WaitThread(S.audio_thread, NULL); S.audio_thread = NULL; }
    if (S.ctx) {
        pspexp_send_raw(S.ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_DISCONNECT, NULL, 0);
        pspexp_destroy(S.ctx);
        S.ctx = NULL;
    }
    if (S.nul) pspexp_null_destroy(S.nul);
    if (S.sdlx) pspexp_sdl_destroy(S.sdlx);
}
