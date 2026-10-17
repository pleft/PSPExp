/*
 * sdl-pspexp: run an unmodified SDL2 program with a PSP as its display,
 * speaker and controller (section 12, "SDL -> PSPExp"; v0.4).
 *
 * Loaded into the program with DYLD_INSERT_LIBRARIES (macOS) or LD_PRELOAD
 * (Linux) by scripts/dev/pspexp-run. It interposes the SDL_Render* API, the
 * audio device and lets the game keep its own window; every draw is
 * mirrored into PSPExp commands, the mixed audio is streamed, and the PSP's
 * controls arrive as a virtual game controller and/or keyboard events.
 *
 * Programs without a renderer (SDL_GetWindowSurface + SDL_UpdateWindowSurface)
 * are handled by streaming the window surface as a texture every present
 * (~270 KB per frame at 480 x 272, fine over USB).
 *
 * Coordinates: the renderer's logical size (or window size) is fitted into
 * 480 x 272 with letterboxing. Textures are shadowed in 32 bits so that
 * SDL_UpdateTexture / SDL_LockTexture changes can be re-uploaded, in
 * power-of-two pages of <= 512 x 512: only the pages that changed, CLUT8 when
 * a page has at most 256 colours, else 5650 when the texture's blend mode is
 * NONE, 4444 otherwise (see upload_tex). Drawing into a render target is recorded and replayed, scaled,
 * when the target is copied to the screen (the integer-upscaling pattern of
 * pixel-art ports). SDL_RenderGeometry and rotation in SDL_RenderCopyEx are
 * ignored (drawn unrotated).
 *
 * Environment:
 *   PSPEXP=usb | tcp[:host[:port]] | sdl | null   (default usb; sdl = the
 *          PSPExp SDL executor in a second window, for checking the stream)
 *   PSPEXP_KEYS=cross=z,circle=x,...          keyboard events for PSP buttons ("return+y": both)
 *   (holding START + SELECT leaves any game: SDL_QUIT after 1 s, exit after 2 s)
 *   PSPEXP_JOYSTICK=0                         no virtual game controller: the keys only
 *   PSPEXP_SCRIPT="ms:key,ms:+key,ms:-key"    keys at those times (testing with nobody at the PSP)
 *   PSPEXP_NUB=joystick | mouse | keys        (default joystick; keys = arrows; mouse: the nub moves
 *                                             a pointer drawn on the PSP, cross/circle click)
 *   PSPEXP_WINDOW=hide                        hide the program's own window
 *   PSPEXP_FOCUS=host                         let the game see host focus loss (default: hidden)
 *   PSPEXP_LOCAL_AUDIO=0                      silence the host's audio device
 *   PSPEXP_AUDIO=psp | host                   pace the game's audio callback ourselves (default when
 *                                             the host driver is SDL's dummy: headless hosts)
 *   PSPEXP_SHOT_EVERY=N                       PSP framebuffer dumps
 *   PSPEXP_DELTA=0, PSPEXP_CLUT=0             send every texture page, 16-bit; both: also the
 *                                             even page grid (the shim before 2026-09-26, to measure)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <SDL.h>
#include <SDL_syswm.h>
#include "../../host/libpspexp/pspexp.h"
#include "../../host/executor-null/pspexp_null.h"
#include "../../host/transport-tcp/pspexp_tcp.h"
#ifndef PSPEXP_NO_SDL_EXECUTOR
#include "../../host/executor-sdl/pspexp_sdl.h"
#endif
#ifdef PSPEXP_HAVE_USB
#include "../../host/transport-usb/pspexp_usb.h"
#endif
#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#if !defined(__APPLE__) && !defined(PSPEXP_STATIC_RENAME)
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
#elif defined(PSPEXP_STATIC_RENAME)
/* Linked into a static program whose sources (and SDL_mixer's) were compiled
 * with -include pspexp_sdl_rename.h, which renames every hooked SDL call to
 * pspexp_<fn> (hosts where injection is impractical: the Omega2's OpenWrt,
 * whose toolchain has no --wrap). This file is compiled without it, so SDL's
 * real functions keep their names here. */
#define HOOK(ret, fn, params) ret pspexp_##fn params
#define REAL(fn) fn
#else
#ifdef PSPEXP_SDL12_MODE
/* the build for SDL 1.2 programs (libsdl-pspexp-sdl12.so, "SDL 1.2 programs" in the README): its
 * hooks are pspexp_SDL_*, reached only through sdl12-compat's lookups (dlsym below); the program's
 * own SDL 1.2 calls of the same names stay SDL 1.2's. Its OpenGL keeps its names: the program's
 * OpenGL calls are the layer's, as for an SDL2 program */
#define HOOK(ret, fn, params) ret pspexp_##fn params
#else
#define HOOK(ret, fn, params) ret fn params
#endif
#define REAL(fn) ((__typeof__(&fn))(uintptr_t)real_symbol(#fn))   /* via uintptr_t: no pedantic object/function cast warning */
#ifdef __GLIBC__
static void *(*g_real_dlsym)(void *, const char *);
static void *real_dlsym(void *handle, const char *name);
#endif
static void *real_symbol(const char *name)
{
    void *p = NULL;
#ifdef __GLIBC__
    /* an SDL function from SDL2 itself: with sdl12-compat loaded (SDL 1.2 games) the next SDL_Init
     * after the layer was SDL 1.2's, which called the layer's again, round and round to a crash */
    static void *sdl2;
    if (!sdl2) sdl2 = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_NOLOAD);
#ifdef PSPEXP_SDL12_MODE
    if (!sdl2) sdl2 = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);   /* (not linked: privately, as sdl12-compat) */
#endif
    if (sdl2 && !strncmp(name, "SDL_", 4)) p = real_dlsym(sdl2, name);
    if (!p) p = real_dlsym(RTLD_NEXT, name);   /* (RTLD_NEXT: called from here, after the layer) */
#else
    p = dlsym(RTLD_NEXT, name);
#endif
    if (!p) { fprintf(stderr, "[sdl-pspexp] cannot find %s\n", name); abort(); }
    return p;
}

#ifdef __GLIBC__
static Dl_info g_self;
/* glibc's dlsym, called from the layer (RTLD_NEXT then means: after the layer). (The build for SDL 1.2
 * programs overrides dlsym in the program; this one is glibc's own.) */
static void *real_dlsym(void *handle, const char *name)
{
    if (!g_real_dlsym) {
        static const char *const v[] = { "GLIBC_2.34", "GLIBC_2.17", "GLIBC_2.4", "GLIBC_2.2.5", "GLIBC_2.0" };
        size_t i;
        for (i = 0; i < sizeof(v) / sizeof(v[0]) && !g_real_dlsym; i++)
            g_real_dlsym = (void *(*)(void *, const char *))(uintptr_t)dlvsym(RTLD_NEXT, "dlsym", v[i]);
        if (!g_real_dlsym) abort();
        dladdr((void *)(uintptr_t)&real_dlsym, &g_self);
    }
    return g_real_dlsym(handle, name);
}

#ifdef PSPEXP_SDL12_MODE
/* sdl12-compat (Debian's SDL 1.2) opens SDL2 and looks each function up by name: a lookup that SDL2
 * answers with a function the layer hooks gets the hook (pspexp_SDL_*). A dlsym(RTLD_NEXT, ...) is
 * passed on as a tail call, so glibc still sees its real caller (RTLD_NEXT is relative to it) */
void *dlsym(void *handle, const char *name)
{
    void *p, *mine;
    char hook[96];
    Dl_info in;
    if (handle == RTLD_NEXT || handle == RTLD_DEFAULT || strncmp(name, "SDL_", 4) != 0) {
        if (!g_real_dlsym) (void)real_dlsym(RTLD_DEFAULT, "dlsym");
        return g_real_dlsym(handle, name);
    }
    p = real_dlsym(handle, name);
    if (!p || !dladdr(p, &in) || !in.dli_fname || !strstr(in.dli_fname, "libSDL2")) return p;
    if (snprintf(hook, sizeof(hook), "pspexp_%s", name) >= (int)sizeof(hook)) return p;
    mine = real_dlsym(RTLD_DEFAULT, hook);
    if (mine && dladdr(mine, &in) && in.dli_fbase == g_self.dli_fbase) return mine;
    return p;
}
#endif
#endif
/* the layer's direct calls to SDL2 (sdl2_direct.h): the same lookup, from SDL2 itself */
void *pspexp_sdl2_symbol(const char *name) { return real_symbol(name); }
#endif

#if defined(__GLIBC__) && !defined(PSPEXP_STATIC_RENAME)
#include "sdl2_direct.h"
#endif
#ifdef PSPEXP_SDL12_MODE
/* the layer's own calls to the functions it hooks: its hooks (pspexp_SDL_*), as in the SDL2 build;
 * made from this file's HOOK lines when built */
#include "sdl12_rename.h"
#endif

/* ---- crash report (Linux MIPS hosts: no debugger on the Omega2) -------------- */

#if defined(__linux__) && defined(__mips__)
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
static void crash_report(int sig, siginfo_t *si, void *ctx)
{
    const ucontext_t *uc = (const ucontext_t *)ctx;
    char line[160];
    int n = snprintf(line, sizeof(line), "[sdl-pspexp] signal %d at pc %#lx, ra %#lx, fault address %p\n", sig,
                     (unsigned long)uc->uc_mcontext.pc, (unsigned long)uc->uc_mcontext.gregs[31], si->si_addr);
    if (n > 0) (void)write(2, line, (size_t)n);
    signal(sig, SIG_DFL);
    raise(sig);
}
__attribute__((constructor)) static void crash_report_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_report;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}
#endif

/* ---- state ------------------------------------------------------------------ */

typedef struct Page { uint16_t tex, pal; int x0, y0, w, h, texw, texh; } Page;   /* pal: CLUT8 page's palette, 0 none */

/* A draw recorded into a render target, in the target's pixel space; replayed
 * (scaled into the destination) when the target is copied to the screen. */
typedef struct TargetOp {
    SDL_Texture *tex;          /* NULL: a fill with `color` */
    SDL_Rect src, dst;
    int flip;
    Uint8 r, g, b, a;
    Uint8 clear_a;             /* a clear's own alpha (a above is 255 for the replay; bake_target composes with this) */
    Uint8 is_clear;
} TargetOp;
#define TARGET_OPS_MAX 256

typedef struct Tex {
    SDL_Texture *sdl;
    int w, h;
    uint32_t *rgba;            /* shadow: ABGR8888 (R in the low byte), or ARGB8888 when argb */
    int argb;                  /* the shadow is in the game's ARGB8888: no conversion to compare */
    int shadow16;              /* the shadow is the game's RGB565, 2 bytes a pixel (emulators' usual output) */
    int half;                  /* a large streaming texture kept at half size (tex_half) */
    int fit;                   /* a frame larger than the PSP's screen kept at the size it is shown (fit_update) */
    int full_w, full_h;        /* fit: the game's size, its pixels (ARGB8888) in full */
    uint32_t *full;
    float fitf;                /* fit: the shadow's size over the game's (the frame's scale; 1 magnified) */
    int dirty, unsupported, streaming;   /* streaming: re-uploaded often, small pages */
    int is_target;             /* SDL_TEXTUREACCESS_TARGET: holds recorded draws, not pixels */
    int baked;                 /* a target composed into its pixels (bake_target): an ordinary texture since */
    TargetOp *ops; int nops;
    SDL_BlendMode blend;       /* NONE: uploaded as 5650 (full colour), else 4444 */
    Page pages[16];
    int npages;
    uint32_t pdirty;           /* pages whose pixels changed (bit = page index) */
    int grid_w, grid_h, grid_stream;     /* what the grid below was cut for */
    int ncol, nrow, cx[17], ry[17];      /* page columns / rows: starts, then the end */
    int npages_grid_changed;             /* the grid was recut: pages are cut again */
    int opaque_used;
    Uint8 r, g, b, a;          /* colour / alpha mod */
    void *lock_pixels; int lock_pitch; SDL_Rect lock_rect; int locked;
    struct Tex *next;
    struct Tex *hnext;         /* its bucket in g_tex_hash (find_tex) */
} Tex;

static struct {
    int inited, active;
    PspExpContext *ctx;
    PspExpTransport *transport;
    PspExpNullExecutor *nul;
#ifndef PSPEXP_NO_SDL_EXECUTOR
    PspExpSdlExecutor *sdlx;
#endif
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
    int dev_open[8]; SDL_mutex *pace_mu;   /* slots in use; the pacer's lock against an open or a close */
    /* input */
    int nub_mouse, nub_keys, hide_window, cursor_hidden, relative;
    int own_pointer;   /* PSPEXP_POINTER=0: the game draws its own, never the layer's (Unreal Tournament's menus) */
    int sync_corner;   /* such a game left relative mode for its menus: at the nub's next move, both to the top-left first */
    SDL_Cursor *blank_cursor[4]; int cursor_blank;   /* see SDL_CreateCursor */
    float nub_speed;                   /* pointer: window pixels a second, the nub fully over */
    int mouse_left, mouse_right;   /* with the nub as the mouse: the buttons that click (B_*, -1 none) */
    int compose;                   /* PSPEXP_COMPOSE=1: the frame drawn on the board, sent whole (host_render, SDL_RenderPresent) */
    int magnify_btn, magnify;      /* PSPEXP_MAGNIFY: the button (B_*, -1 none) that shows the game's own pixels round the pointer */
    float drawn_px, drawn_py; Uint32 last_present; unsigned replays;   /* the pointer as last shown (replay_pointer) */ uint32_t prev_buttons, nub_dirs; float px, py; Uint32 ptick;
    /* what injected input holds: SDL's own keyboard and mouse state never sees
     * pushed events, and games that poll it (Chromium B.S.U.) would not */
    Uint8 kheld[SDL_NUM_SCANCODES]; Uint32 mbuttons; float rel_x, rel_y, rel_ex, rel_ey;
    Uint32 held_since[16], last_repeat[16];
    SDL_Keycode keys[16][3];           /* up to three keys per PSP button ("return+y") */
    Uint32 quit_hold; int quit_sent;   /* START + SELECT held: leave the game */
    int vjoy;                          /* virtual joystick device index, -1 none */
    SDL_Joystick *vjoy_handle;
    /* stats */
    Uint32 last_report; unsigned frames, uploads, shot_every, shot_index;
    int no_delta, no_clut;             /* PSPEXP_DELTA=0 / PSPEXP_CLUT=0: every page, 16-bit (benchmarks) */
    int quant;                         /* PSPEXP_QUANT=1: a page of more than 256 colours reduced to 256 (median cut), CLUT8 */
    int no_batch;                      /* PSPEXP_BATCH=0: every message its own transfer */
    uint64_t rep_bytes;                /* link bytes at the last report */
    uint64_t up_ticks, tot_up_ticks;
    uint64_t prof_shadow, prof_convert, prof_write;   /* ns: compare, convert (wall), write */
    uint64_t prof_wait, prof_send;    /* ns: waiting for the PSP's last frame, sending this one */
    uint64_t prof_flush, prof_flush_cpu;   /* ns: the batched uploads out (wall, and this thread's CPU) */
    uint64_t prof_game, prof_game_cpu, prof_last_end, prof_last_end_cpu;   /* ns: between frames: the program and its
                                         OpenGL here (wall, and this thread's CPU), from the end of the frame before */   /* ns of this thread's CPU converting textures (waits do not count) */
    unsigned pages_sent, pages_t8, pages_kept;          /* since the last report */
    unsigned long dropped;                              /* frames not sent under PSPEXP_FRAME_DROP */
    unsigned long silent_ms, sound_ms;                   /* a paced device's silence while paused, and its sound */
    unsigned long tot_pages_sent, tot_pages_t8, tot_pages_kept;
    Uint64 first_frame;                /* performance counter at the first frame sent */
    uint32_t last_shot_frame;
    int skip_clear, skip_rects, skip_lines, skip_quads, skip_audio;   /* PSPEXP_DEBUG_SKIP */
    /* window-surface streaming (no renderer) */
    Tex *surface_tex; int surface_w, surface_h;
    unsigned long total_frames;
    /* OpenGL (gl_pspexp.c) */
    int want_depth, gl_context; SDL_Window *gl_window;
    int gl_major, gl_minor, gl_profile;   /* what the program asks of the next context */
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
    for (i = 0; i < B_COUNT; i++) S.keys[i][0] = S.keys[i][1] = S.keys[i][2] = SDLK_UNKNOWN;
    strncpy(buf, env && *env ? env : def, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_r(buf, ",", &p); tok; tok = strtok_r(NULL, ",", &p)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        for (i = 0; i < B_COUNT; i++)
            if (!strcmp(tok, BUTTON_NAMES[i])) {   /* "return+y": both keys */
                char *k, *q;
                int n = 0;
                for (k = strtok_r(eq + 1, "+", &q); k && n < 3; k = strtok_r(NULL, "+", &q)) S.keys[i][n++] = key_from_name(k);
                while (n < 3) S.keys[i][n++] = SDLK_UNKNOWN;
            }
    }
}

#ifndef PSPEXP_STATIC_RENAME
static void start_audio_fifo(void);
#endif

static void connect_once(void)
{
    const char *env = getenv("PSPEXP");
    char err[200] = "";
    if (S.inited) return;
    S.inited = 1;
    S.next_handle = 1;
    S.local_audio = !(getenv("PSPEXP_LOCAL_AUDIO") && !strcmp(getenv("PSPEXP_LOCAL_AUDIO"), "0"));
    S.nub_mouse = getenv("PSPEXP_NUB") && !strcmp(getenv("PSPEXP_NUB"), "mouse");
    S.own_pointer = getenv("PSPEXP_POINTER") && !strcmp(getenv("PSPEXP_POINTER"), "0");
    S.nub_speed = getenv("PSPEXP_NUB_SPEED") && atof(getenv("PSPEXP_NUB_SPEED")) > 0 ? (float)atof(getenv("PSPEXP_NUB_SPEED")) : 300.0f;
    S.mouse_left = B_CROSS; S.mouse_right = B_CIRCLE;
    if (getenv("PSPEXP_MOUSE_BUTTONS")) {   /* "left=r,right=none": which PSP buttons are the mouse's */
        char buf[64], *tok, *save = NULL;
        snprintf(buf, sizeof(buf), "%s", getenv("PSPEXP_MOUSE_BUTTONS"));
        for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            char *eq = strchr(tok, '=');
            int b, which = -1;
            if (!eq) continue;
            *eq = 0;
            for (b = 0; b < B_COUNT; b++) if (!strcmp(eq + 1, BUTTON_NAMES[b])) which = b;
            if (!strcmp(tok, "left")) S.mouse_left = which;
            else if (!strcmp(tok, "right")) S.mouse_right = which;
        }
    }
    S.compose = getenv("PSPEXP_COMPOSE") && !strcmp(getenv("PSPEXP_COMPOSE"), "1");
    S.magnify_btn = -1;
    if (getenv("PSPEXP_MAGNIFY")) {   /* "r": that button switches between the whole frame shrunk and its own pixels */
        int b;
        for (b = 0; b < B_COUNT; b++) if (!strcmp(getenv("PSPEXP_MAGNIFY"), BUTTON_NAMES[b])) S.magnify_btn = b;
    }
    S.nub_keys = getenv("PSPEXP_NUB") && !strcmp(getenv("PSPEXP_NUB"), "keys");
    S.hide_window = getenv("PSPEXP_WINDOW") && !strcmp(getenv("PSPEXP_WINDOW"), "hide");
    S.shot_every = getenv("PSPEXP_SHOT_EVERY") ? (unsigned)atoi(getenv("PSPEXP_SHOT_EVERY")) : 0;
    S.no_delta = getenv("PSPEXP_DELTA") && !strcmp(getenv("PSPEXP_DELTA"), "0");
    S.no_clut = getenv("PSPEXP_CLUT") && !strcmp(getenv("PSPEXP_CLUT"), "0");
    /* on by default for frames from the board's GPU (photo-like, every page changed: SuperTuxKart's
     * race 230 -> 137 KB a frame, 0.4 ms more), off for the rest (pages over 256 colours are rare) */
#ifdef PSPEXP_HOST_GL
    S.quant = !(getenv("PSPEXP_QUANT") && !strcmp(getenv("PSPEXP_QUANT"), "0"));
#else
    S.quant = getenv("PSPEXP_QUANT") && !strcmp(getenv("PSPEXP_QUANT"), "1");
#endif
    S.no_batch = getenv("PSPEXP_BATCH") && !strcmp(getenv("PSPEXP_BATCH"), "0");
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
    }
#ifndef PSPEXP_NO_SDL_EXECUTOR
    else if (!strcmp(env, "sdl")) {
        /* In-process executor: its own renderer, so the hooks pass its draws through. */
        PspExpSdlOptions o;
        pspexp_sdl_options_default(&o);
        o.pump_events = 0;
        o.title = "PSPExp (sdl-pspexp)";
        S.sdlx = pspexp_sdl_create(&o, err, sizeof(err));
        if (S.sdlx) S.transport = pspexp_sdl_transport(S.sdlx);
        S.mode = 2;
    }
#endif
    else if (!strncmp(env, "tcp", 3)) {
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
        S.usb.verbose = getenv("PSPEXP_USB_VERBOSE") != NULL;   /* why a USB transfer failed, in the log */
        logf_("waiting up to 15 s for a PSP over USB...");
        S.transport = pspexp_usb_wait(&S.usb, 15000, err, sizeof(err));
        S.mode = 0;
#else
        logf_("built without USB support");
#endif
    }
    /* No PSP yet (a runtime still busy with the previous client, a Wi-Fi
     * hiccup): stay active without a link; try_reconnect() keeps trying once
     * a second, so the game appears on the PSP as soon as it answers instead
     * of running unseen. (sdl mode: the executor window failed, give up.) */
    if (!S.transport && S.mode == 2) { logf_("%s", err[0] ? err : "no transport"); return; }
    S.ctx = pspexp_create(S.transport, NULL);
    pspexp_set_log(S.ctx, lib_log, NULL);
    if (S.want_depth) pspexp_set_depth_buffer(S.ctx, 1);   /* an OpenGL program: before the handshake asks for it */
    if (!S.transport) logf_("%s: retrying every second", err[0] ? err : "no PSP yet");
    else if (pspexp_connect(S.ctx, 5000) < 0) {
        logf_("no HELLO from the runtime: retrying every second");
        pspexp_set_transport(S.ctx, NULL);
        S.transport = NULL;
    }
    S.active = 1;
    atexit(shim_exit);
    {   /* the ring before its lock: the audio pipe's reader runs from load and pushes once the lock exists */
        SDL_mutex *mu = SDL_CreateMutex();
        S.acap = 44100 * 2;
        S.aring = (int16_t *)calloc(S.acap * 2, sizeof(int16_t));
        __atomic_store_n(&S.amu, mu, __ATOMIC_RELEASE);
    }
#ifndef PSPEXP_STATIC_RENAME
    start_audio_fifo();
#endif
    S.px = 240; S.py = 136;
    S.last_report = SDL_GetTicks();
    logf_("active (%s)", env);
}

static int link_up(void) { return S.active && S.ctx && pspexp_link_state(S.ctx) == PSPEXP_LINK_CONNECTED; }

/* Headless host (SDL's dummy / offscreen video): nobody sees the host's own
 * rendering, and SDL's software renderer scaling every frame to an invisible
 * window is real work on a small host. Skip it once the PSP is the display
 * (PSPEXP_HOST_RENDER=1 keeps it). */
static int host_render(void)
{
    static int decided, render;
    const char *drv;
    if (!S.active || S.compose) return 1;   /* (composed: SDL's own rendering is the frame) */
    if (!decided) {
        decided = 1;
        drv = SDL_GetCurrentVideoDriver();
        render = (getenv("PSPEXP_HOST_RENDER") && atoi(getenv("PSPEXP_HOST_RENDER"))) ||
                 !(drv && (!strcmp(drv, "dummy") || !strcmp(drv, "offscreen")));
        if (!render) logf_("headless host: SDL's own rendering skipped (video driver '%s')", drv);
    }
    return render;
}

static void cursors_forget(void);   /* (a new link: the game's cursors sent again) */

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
        cursors_forget();
    }
}

/* ---- geometry -------------------------------------------------------------- */

/* The pointer (window pixels) in the game's logical pixels, as SDL maps it: with a logical size
 * unlike the window's, letterboxed (fheroes2 in full screen: a 480x272 window holding 640x480). The
 * pointer drawn at the window's position scaled to the logical frame had sat left of where the game
 * took it, 30 pixels at "New Game" (the user). */
static void pointer_logical(float *lx, float *ly)
{
    int ww = S.win_w > 0 ? S.win_w : 480, wh = S.win_h > 0 ? S.win_h : 272;
    int lw = S.log_w > 0 ? S.log_w : ww, lh = S.log_h > 0 ? S.log_h : wh;
    if (S.log_w > 0 && S.log_h > 0 && (lw != ww || lh != wh)) {
        float ls = fminf((float)ww / lw, (float)wh / lh);
        float vx = floorf((ww - lw * ls) / 2), vy = floorf((wh - lh * ls) / 2);
        *lx = (S.px - vx) / ls; *ly = (S.py - vy) / ls;
    } else { *lx = S.px * lw / (float)ww; *ly = S.py * lh / (float)wh; }
}

/* The frame fitted to the PSP's 480x272, or, magnified (PSPEXP_MAGNIFY), at its own size with the
 * pointer's surroundings in view: a 640x480 game's small text, 57% in the fitted frame (Heroes II's
 * 4-5 pixel labels), readable at 1:1. The view follows the pointer, stopped at the frame's edges. */
static void place_frame(int log)
{
    int lw = S.log_w > 0 ? S.log_w : S.win_w, lh = S.log_h > 0 ? S.log_h : S.win_h;
    float s;
    if (lw <= 0 || lh <= 0) { lw = 480; lh = 272; }
    s = fminf(480.0f / lw, 272.0f / lh);
    if (S.magnify && s < 1.0f) {
        float plx, ply;
        int cx, cy;
        pointer_logical(&plx, &ply);
        cx = (int)lroundf(plx); cy = (int)lroundf(ply);
        S.sx = S.sy = 1.0f;
        S.ox = lw > 480 ? 240 - cx : (480 - lw) / 2;
        S.oy = lh > 272 ? 136 - cy : (272 - lh) / 2;
        if (lw > 480) S.ox = S.ox > 0 ? 0 : S.ox < 480 - lw ? 480 - lw : S.ox;
        if (lh > 272) S.oy = S.oy > 0 ? 0 : S.oy < 272 - lh ? 272 - lh : S.oy;
    } else {
        S.sx = S.sy = s;
        S.ox = (int)((480 - lw * s) / 2);
        S.oy = (int)((272 - lh * s) / 2);
    }
    if (log) logf_("geometry: window %dx%d, logical %dx%d -> scale %.3f, offset %d,%d%s", S.win_w, S.win_h, S.log_w, S.log_h, S.sx, S.ox, S.oy, S.magnify ? " (magnified)" : "");
}

static void fit_rescale(Tex *x);
static void update_scale(void)
{
    Tex *tx;
    place_frame(1);
    for (tx = S.textures; tx; tx = tx->next) if (tx->fit) fit_rescale(tx);
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

/* the layer draws its pointer over the frame (the nub as the mouse, the game's cursor not hidden or
 * its own) */
static int pointer_shown(void) { return S.nub_mouse && !S.cursor_hidden && !S.cursor_blank && !S.relative && !S.own_pointer; }
static void draw_pointer(void);

static void begin_frame(void)
{
    if (!S.frame_open) {
        pspexp_begin_frame(S.ctx); S.frame_open = 1; S.clip_sent = 0;
        if (!S.no_batch) pspexp_batch_begin(S.ctx);   /* the frame's uploads and audio: one transfer (end_frame) */
        if (S.magnify) place_frame(0);   /* the view follows the pointer */
        if (S.ren && !S.gl_window && (S.ox > 0 || S.oy > 0)) {
            /* letterboxed: the bars black every frame (a game that draws a full background and never
             * clears, Blobby Volley, left them showing whatever the PSP had there) */
            uint16_t black = pspexp_color_5650(0, 0, 0);
            pspexp_set_clip(S.ctx, 0, 0, 480, 272);
            if (S.ox > 0) { pspexp_fill_rect(S.ctx, 0, 0, (int16_t)S.ox, 272, black, 255); pspexp_fill_rect(S.ctx, (int16_t)(480 - S.ox), 0, (int16_t)S.ox, 272, black, 255); }
            if (S.oy > 0) { pspexp_fill_rect(S.ctx, 0, 0, 480, (int16_t)S.oy, black, 255); pspexp_fill_rect(S.ctx, 0, (int16_t)(272 - S.oy), 480, (int16_t)S.oy, black, 255); }
        }
    }
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

/* ---- targets composed on the board ----------------------------------------------------
 * A target's draws are recorded and replayed where it is drawn (cheap, the PSP does the work). A program
 * that draws a texture into a target and then destroys it (Flare: a line of text into a label's image)
 * leaves nothing to replay: the target is then composed here into pixels, as SDL would have (bake_target),
 * and stays an ordinary texture; what is drawn into it afterwards is composed into it too (compose_op). */
static void bake_target(Tex *t);

static uint32_t tex_pixel(const Tex *s, int x, int y)   /* ABGR (R in the low byte) */
{
    if (s->shadow16) {
        uint16_t c = ((const uint16_t *)s->rgba)[(size_t)y * s->w + x];
        uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        return 0xFF000000u | (b * 255 / 31) << 16 | (g * 255 / 63) << 8 | (r * 255 / 31);
    }
    {
        uint32_t c = s->rgba[(size_t)y * s->w + x];
        if (s->argb) c = (c & 0xFF00FF00u) | (c & 0xFFu) << 16 | ((c >> 16) & 0xFFu);
        return c;
    }
}

static void compose_op(Tex *t, const TargetOp *op)
{
    SDL_Rect d = op->dst;
    int x, y;
    if (d.w <= 0 || d.h <= 0) return;
    if (!op->tex) {   /* a fill (a clear: replaced, its own alpha too) */
        uint32_t c = (uint32_t)(op->is_clear ? op->clear_a : op->a) << 24 | (uint32_t)op->b << 16 | (uint32_t)op->g << 8 | op->r;
        for (y = d.y < 0 ? 0 : d.y; y < d.y + d.h && y < t->h; y++)
            for (x = d.x < 0 ? 0 : d.x; x < d.x + d.w && x < t->w; x++) t->rgba[(size_t)y * t->w + x] = c;
        return;
    }
    {
        Tex *s = find_tex(op->tex);
        SDL_Rect sr = op->src;
        if (!s || !s->rgba) return;
        if (s->is_target && s != t) bake_target(s);
        if (sr.w <= 0 || sr.h <= 0) { sr.x = 0; sr.y = 0; sr.w = s->w; sr.h = s->h; }
        for (y = 0; y < d.h; y++) {
            int ty = d.y + y, sy = sr.y + (op->flip & SDL_FLIP_VERTICAL ? d.h - 1 - y : y) * sr.h / d.h;
            if (ty < 0 || ty >= t->h || sy < 0 || sy >= s->h) continue;
            for (x = 0; x < d.w; x++) {
                int tx = d.x + x, sx = sr.x + (op->flip & SDL_FLIP_HORIZONTAL ? d.w - 1 - x : x) * sr.w / d.w;
                uint32_t c, o, *dp;
                unsigned a, r, g, b;
                if (tx < 0 || tx >= t->w || sx < 0 || sx >= s->w) continue;
                c = tex_pixel(s, sx, sy);
                r = (c & 0xFF) * s->r / 255; g = ((c >> 8) & 0xFF) * s->g / 255; b = ((c >> 16) & 0xFF) * s->b / 255;
                a = (c >> 24) * s->a / 255;
                dp = &t->rgba[(size_t)ty * t->w + tx];
                o = *dp;
                if (s->blend == SDL_BLENDMODE_NONE) { *dp = a << 24 | b << 16 | g << 8 | r; continue; }
                if (s->blend == SDL_BLENDMODE_ADD) {   /* dst + src x alpha, dst alpha kept */
                    unsigned nr = (o & 0xFF) + r * a / 255, ng = ((o >> 8) & 0xFF) + g * a / 255, nb = ((o >> 16) & 0xFF) + b * a / 255;
                    *dp = (o & 0xFF000000u) | (nb > 255 ? 255 : nb) << 16 | (ng > 255 ? 255 : ng) << 8 | (nr > 255 ? 255 : nr);
                    continue;
                }
                {   /* SDL_BLENDMODE_BLEND: src x a + dst x (1 - a); alpha a + dst alpha x (1 - a) */
                    unsigned oa = o >> 24, na = a + oa * (255 - a) / 255;
                    unsigned nr = (r * a + (o & 0xFF) * (255 - a)) / 255, ng = (g * a + ((o >> 8) & 0xFF) * (255 - a)) / 255,
                             nb = (b * a + ((o >> 16) & 0xFF) * (255 - a)) / 255;
                    *dp = na << 24 | nb << 16 | ng << 8 | nr;
                }
            }
        }
    }
}

static void bake_target(Tex *t)
{
    int i;
    if (!t->is_target || !t->rgba) return;
    t->is_target = 0;   /* (first: a target drawing itself is not composed again) */
    t->argb = 0; t->shadow16 = 0;
    memset(t->rgba, 0, (size_t)t->w * t->h * 4);
    for (i = 0; i < t->nops; i++) compose_op(t, &t->ops[i]);
    t->nops = 0;
    t->baked = 1;
    t->dirty = 1;
}

static void record_op(SDL_Texture *tex, const SDL_Rect *src, const SDL_Rect *dst, int flip, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    Tex *t = target_tex();
    TargetOp *op;
    if (!t) return;
    if (t->baked) {   /* composed here from now on */
        TargetOp now;
        now.tex = tex; now.src = src ? *src : (SDL_Rect){ 0, 0, 0, 0 };
        now.dst = dst ? *dst : (SDL_Rect){ 0, 0, t->w, t->h };
        now.flip = flip; now.r = r; now.g = g; now.b = b; now.a = a;
        now.is_clear = !tex && !dst; now.clear_a = a;
        compose_op(t, &now);
        t->dirty = 1;
        return;
    }
    if (!t->is_target) return;
    if (!t->ops) t->ops = (TargetOp *)calloc(TARGET_OPS_MAX, sizeof(TargetOp));
    if (!t->ops || t->nops == TARGET_OPS_MAX) return;
    op = &t->ops[t->nops++];
    op->tex = tex;
    op->src = src ? *src : (SDL_Rect){ 0, 0, 0, 0 };
    op->dst = dst ? *dst : (SDL_Rect){ 0, 0, t->w, t->h };
    op->flip = flip; op->r = r; op->g = g; op->b = b; op->a = a;
    op->is_clear = 0; op->clear_a = a;
}

static int drawing(void)
{
    if (!link_up() || S.target || S.compose) return 0;   /* (composed: nothing mirrored, the frame is sent whole) */
    begin_frame();
    apply_clip();
    return 1;
}

static uint16_t draw_color5650(void) { return pspexp_color_5650(S.dr, S.dg, S.db); }
static uint8_t draw_alpha(void) { return S.dblend == SDL_BLENDMODE_NONE ? 255 : S.da; }

/* ---- textures -------------------------------------------------------------- */

/* Textures by their SDL_Texture, hashed: a game with thousands of small textures (C-Dogs, a texture
 * a sprite) walked the whole list at every draw, 900 draws a frame: 1.5 fps, the board's CPU all in
 * the walk. The last one found is kept (a run of draws from one texture). */
#define TEX_HASH 4096
static Tex *g_tex_hash[TEX_HASH], *g_tex_last;
static unsigned tex_slot(SDL_Texture *t) { return (unsigned)((((uintptr_t)t >> 4) * 2654435761u) >> 8) & (TEX_HASH - 1); }
static Tex *find_tex(SDL_Texture *t)
{
    Tex *x;
    if (g_tex_last && g_tex_last->sdl == t) return g_tex_last;
    for (x = g_tex_hash[tex_slot(t)]; x; x = x->hnext) if (x->sdl == t) return g_tex_last = x;
    return NULL;
}
static void tex_unhash(Tex *x)
{
    Tex **pp = &g_tex_hash[tex_slot(x->sdl)];
    while (*pp && *pp != x) pp = &(*pp)->hnext;
    if (*pp) *pp = x->hnext;
    if (g_tex_last == x) g_tex_last = NULL;
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
    x->hnext = g_tex_hash[tex_slot(t)];
    g_tex_hash[tex_slot(t)] = x;
    return x;
}

static void release_pages(Tex *x)
{
    int i;
    if (S.ctx) for (i = 0; i < x->npages; i++) {
        pspexp_release(S.ctx, x->pages[i].tex);
        if (x->pages[i].pal) pspexp_release(S.ctx, x->pages[i].pal);
    }
    x->npages = 0;
}

static int pow2ceil(int v, int minimum) { int p = minimum; while (p < v) p <<= 1; return p; }

/* Texture pages. A texture is cut into pages of at most 512 x 512 (256 x 128
 * for streaming textures: less padding per frame), each a PSP texture of its
 * own, stored swizzled (16-byte x 8-row blocks: the GE reads these several
 * times faster) directly in the outgoing message.
 *
 * Only what changed is sent: the shadow is compared as it is updated
 * (shadow_update) and marks the pages that differ; an unchanged page keeps
 * its PSP texture. A page with at most 256 distinct colours (after the
 * conversion to the PSP's 16 bits) goes as CLUT8 with its own palette: half
 * the bytes. Most frame-streaming games draw in 256 colours and convert to
 * 32 bits only for SDL (Doom, Tyrian, emulators of 8- and 16-bit machines).
 * PSPEXP_DELTA=0 and PSPEXP_CLUT=0 turn these off, for measurements.
 * On a small host (Omega2: 580 MHz, no FPU) this is most of the shim's cost. */
/* Cut `total` into power-of-two pieces of at most `cap` (the protocol wants
 * power-of-two textures; pieces of those sizes carry no padding: 320 x 200 is
 * 256 + 64 by 128 + 64 + 8), the last one padded up to `minimum`. */
static int cut_pow2(int total, int cap, int minimum, int *starts, int max)
{
    int n = 0, at = 0;
    while (at < total && n < max) {
        int rem = total - at, sz = cap;
        while (sz > rem && sz > minimum) sz >>= 1;
        starts[n++] = at;
        at += sz < rem ? sz : rem;
    }
    starts[n] = total;
    return at < total ? -1 : n;
}

static uint64_t wall_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* CPU time of the calling thread, ns: waits for the link do not count */
static uint64_t cpu_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void page_grid(Tex *x)
{
    int cap_w = x->streaming ? 256 : 512, cap_h = x->streaming ? 128 : 512;
    if (x->grid_w == x->w && x->grid_h == x->h && x->grid_stream == x->streaming && x->ncol) return;
    x->ncol = cut_pow2(x->w, cap_w, 16, x->cx, 16);
    x->nrow = cut_pow2(x->h, cap_h, 8, x->ry, 16);
    if ((S.no_delta && S.no_clut) || x->ncol < 1 || x->nrow < 1 || x->ncol * x->nrow > 16) {
        /* too many pieces (or measuring the old way): an even grid of full pages;
         * a large streaming texture in its small pages would still be too many
         * (Project: Starfighter's 800x600 frame: 20), so it gets the full size */
        int i;
        x->ncol = (x->w + cap_w - 1) / cap_w; x->nrow = (x->h + cap_h - 1) / cap_h;
        if (x->ncol * x->nrow > 16) { cap_w = cap_h = 512; x->ncol = (x->w + 511) / 512; x->nrow = (x->h + 511) / 512; }
        if (x->ncol > 16) x->ncol = 16;
        if (x->nrow > 16) x->nrow = 16;
        for (i = 0; i < x->ncol; i++) x->cx[i] = i * cap_w;
        x->cx[x->ncol] = x->w;
        for (i = 0; i < x->nrow; i++) x->ry[i] = i * cap_h;
        x->ry[x->nrow] = x->h;
    }
    x->grid_w = x->w; x->grid_h = x->h; x->grid_stream = x->streaming;
    x->npages_grid_changed = 1;
}

/* Converting a page: to 16 bits (row-major) and, while it has at most 256
 * colours, to palette indices, in one pass (runs of one colour skip the
 * lookup). A lane holds the colour tables of one thread (a colour's palette
 * index, valid for one generation: no clearing between pages); a Conv the
 * results of one page. Pages are independent: on boards with more cores a
 * frame's pages are converted in parallel (see the pool), then written into
 * the outgoing message in order on the calling thread. */
typedef struct Lane {
    uint16_t stamp[65536]; uint8_t index[65536]; uint16_t gen;
    uint32_t *cnt; uint16_t *col, *tmp;   /* PSPEXP_QUANT: counts by colour, the page's colours, a sort buffer */
} Lane;
typedef struct Conv {
    uint16_t *px16; uint8_t *idx8; size_t cap;
    uint16_t pal[256]; int npal, t8;
} Conv;

static int conv_reserve(Conv *cv, size_t n)
{
    if (n <= cv->cap) return 0;
    free(cv->px16); free(cv->idx8);
    cv->px16 = (uint16_t *)malloc(n * 2); cv->idx8 = (uint8_t *)malloc(n);
    if (!cv->px16 || !cv->idx8) { free(cv->px16); free(cv->idx8); cv->px16 = NULL; cv->idx8 = NULL; cv->cap = 0; return -1; }
    cv->cap = n;
    return 0;
}

/* PSPEXP_QUANT: a page of more than 256 colours (a moving 3D frame from the board's GPU: every page
 * changes, 215 KB a frame at 480x272 in 16 bits) to its 256 best by median cut, then CLUT8: about half
 * the bytes, nothing new on the PSP. The page's 5650 colours are counted; the box of colours whose
 * longest side times its pixel count is largest is cut at its weighted median, until 256 boxes; each
 * box's colours take its weighted mean. Channels have 32 or 64 values, so each cut sorts its box by a
 * counting sort: a few passes over the page's colours, no comparisons. No dithering. */
typedef struct QBox { int lo, hi; uint32_t w; int axis, range; } QBox;

static int q_ch(uint16_t c, int axis) { return axis == 0 ? (c & 31) << 1 : axis == 1 ? (c >> 5) & 63 : (c >> 11) << 1; }   /* on one 0..63 scale */

static void q_measure(const Lane *ln, QBox *b)
{
    int a, i, best = -1;
    b->w = 0;
    for (i = b->lo; i < b->hi; i++) b->w += ln->cnt[ln->col[i]];
    b->range = 0; b->axis = 1;
    for (a = 0; a < 3; a++) {
        int mn = 64, mx = -1;
        for (i = b->lo; i < b->hi; i++) { int v = q_ch(ln->col[i], a); if (v < mn) mn = v; if (v > mx) mx = v; }
        if (mx - mn > best) { best = mx - mn; b->axis = a; }
    }
    b->range = best;
}

static void quantize_page(Lane *ln, const Page *p, Conv *cv)
{
    QBox box[256];
    int nb = 1, n = 0, i, k, total = p->w * p->h;
    if (!ln->cnt) {
        ln->cnt = (uint32_t *)calloc(65536, sizeof(uint32_t));
        ln->col = (uint16_t *)malloc(65536 * sizeof(uint16_t));
        ln->tmp = (uint16_t *)malloc(65536 * sizeof(uint16_t));
        if (!ln->cnt || !ln->col || !ln->tmp) { free(ln->cnt); free(ln->col); free(ln->tmp); ln->cnt = NULL; return; }
    }
    for (i = 0; i < total; i++) { uint16_t c = cv->px16[i]; if (!ln->cnt[c]++) ln->col[n++] = c; }
    box[0].lo = 0; box[0].hi = n; q_measure(ln, &box[0]);
    while (nb < 256) {
        int pick = -1, ax, cut;
        uint64_t score = 0;
        uint32_t half, run = 0, hist[64];
        for (k = 0; k < nb; k++) {
            uint64_t sc = (uint64_t)box[k].range * box[k].w;
            if (box[k].hi - box[k].lo > 1 && box[k].range > 0 && sc > score) { score = sc; pick = k; }
        }
        if (pick < 0) break;
        {   /* counting sort of the box along its longest axis */
            QBox *b = &box[pick];
            uint32_t pos[64];
            ax = b->axis;
            memset(hist, 0, sizeof(hist));
            for (i = b->lo; i < b->hi; i++) hist[q_ch(ln->col[i], ax)]++;
            for (k = 0, run = 0; k < 64; k++) { pos[k] = run; run += hist[k]; }
            for (i = b->lo; i < b->hi; i++) { uint16_t c = ln->col[i]; ln->tmp[pos[q_ch(c, ax)]++] = c; }
            memcpy(ln->col + b->lo, ln->tmp, (size_t)(b->hi - b->lo) * sizeof(uint16_t));
            /* the weighted median, never at an end */
            half = b->w / 2; run = 0;
            for (cut = b->lo; cut < b->hi - 1; cut++) { run += ln->cnt[ln->col[cut]]; if (run >= half) break; }
            cut++;
            if (cut <= b->lo) cut = b->lo + 1;
            if (cut >= b->hi) cut = b->hi - 1;
            box[nb].lo = cut; box[nb].hi = b->hi; b->hi = cut;
            q_measure(ln, b); q_measure(ln, &box[nb]);
            nb++;
        }
    }
    for (k = 0; k < nb; k++) {   /* each box: its weighted mean, and its colours' index */
        uint64_t r = 0, g = 0, bl = 0, w = 0;
        for (i = box[k].lo; i < box[k].hi; i++) {
            uint16_t c = ln->col[i];
            uint32_t m = ln->cnt[c];
            r += (uint64_t)(c & 31) * m; g += (uint64_t)((c >> 5) & 63) * m; bl += (uint64_t)(c >> 11) * m; w += m;
            ln->index[c] = (uint8_t)k;
        }
        if (!w) w = 1;
        cv->pal[k] = (uint16_t)(((r + w / 2) / w) | (((g + w / 2) / w) << 5) | (((bl + w / 2) / w) << 11));
    }
    for (i = 0; i < total; i++) cv->idx8[i] = ln->index[cv->px16[i]];
    for (i = 0; i < n; i++) ln->cnt[ln->col[i]] = 0;   /* clean for the next page */
    ln->gen++;                                          /* the index entries above are not this generation's */
    if (ln->gen == 0) { memset(ln->stamp, 0, sizeof(ln->stamp)); ln->gen = 1; }
    cv->npal = nb;
    cv->t8 = 1;
}

static void convert_page(Lane *ln, const Tex *x, const Page *p, int opaque, Conv *cv)
{
    int y, xx, npal = 0;
    /* CLUT8 only when it pays: the palette costs 512 bytes */
    int try8 = !S.no_clut && p->texw * p->texh >= 2048, over = !try8, last = -1;
    uint8_t lasti = 0;
    uint16_t *pal = cv->pal;
    if (try8 && ++ln->gen == 0) { memset(ln->stamp, 0, sizeof(ln->stamp)); ln->gen = 1; }
#define PAGE_LOOP(T, CONV) \
    for (y = 0; y < p->h; y++) { \
        const T *src = (const T *)x->rgba + (size_t)(p->y0 + y) * x->w + p->x0; \
        uint16_t *o = cv->px16 + (size_t)y * p->w; \
        uint8_t *oi = cv->idx8 + (size_t)y * p->w; \
        for (xx = 0; xx < p->w; xx++) { \
            uint32_t c = src[xx]; \
            uint16_t c16 = (uint16_t)(CONV); \
            o[xx] = c16; \
            if (!over) { \
                if (c16 != last) { \
                    if (ln->stamp[c16] != ln->gen) { \
                        if (npal == 256) { over = 1; continue; } \
                        ln->stamp[c16] = ln->gen; ln->index[c16] = (uint8_t)npal; pal[npal++] = c16; \
                    } \
                    last = c16; lasti = ln->index[c16]; \
                } \
                oi[xx] = lasti; \
            } \
        } \
    }
    if (x->shadow16) {   /* RGB565 (R high): the PSP's 5650 has R low, a swap */
        if (opaque) PAGE_LOOP(uint16_t, ((c & 0x1Fu) << 11) | (c & 0x07E0u) | (c >> 11))
        else PAGE_LOOP(uint16_t, (c >> 12) | (((c >> 7) & 0xFu) << 4) | (((c >> 1) & 0xFu) << 8) | 0xF000u)
    } else if (x->argb) {   /* ARGB8888: B in the low byte */
        if (opaque) PAGE_LOOP(uint32_t, ((c >> 19) & 0x1Fu) | (((c >> 10) & 0x3Fu) << 5) | (((c >> 3) & 0x1Fu) << 11))
        else PAGE_LOOP(uint32_t, ((c >> 20) & 0xFu) | (((c >> 12) & 0xFu) << 4) | (((c >> 4) & 0xFu) << 8) | (((c >> 28) & 0xFu) << 12))
    } else {         /* ABGR8888: R in the low byte */
        if (opaque) PAGE_LOOP(uint32_t, ((c >> 3) & 0x1Fu) | (((c >> 10) & 0x3Fu) << 5) | (((c >> 19) & 0x1Fu) << 11))
        else PAGE_LOOP(uint32_t, ((c >> 4) & 0xFu) | (((c >> 12) & 0xFu) << 4) | (((c >> 20) & 0xFu) << 8) | (((c >> 28) & 0xFu) << 12))
    }
#undef PAGE_LOOP
    cv->npal = npal;
    cv->t8 = try8 && !over;
    if (try8 && over && S.quant && opaque) quantize_page(ln, p, cv);
}

/* the converted page into the outgoing message: palette, texture, swizzled
 * (16-byte x 8-row blocks: the GE reads these several times faster) */
static int write_page(Tex *x, Page *p, int opaque, Conv *cv)
{
    PspExpTextureDesc d;
    PspExpFrame f;
    int y, xx, t8 = cv->t8;
    if (p->pal) { pspexp_release(S.ctx, p->pal); p->pal = 0; }
    if (t8) {
        for (y = cv->npal; y < 256; y++) cv->pal[y] = 0;
        p->pal = alloc_handle();
        if (pspexp_upload_palette(S.ctx, p->pal, opaque ? PSPEXP_TEX_5650 : PSPEXP_TEX_4444, 256, cv->pal, sizeof(cv->pal)) < 0) {
            p->pal = 0; t8 = 0;   /* px16 still holds the colours: 16-bit */
            S.no_clut = 1;
        }
    }
    memset(&d, 0, sizeof(d));
    d.width = (uint16_t)p->texw; d.height = (uint16_t)p->texh;
    d.format = t8 ? PSPEXP_TEX_T8 : opaque ? PSPEXP_TEX_5650 : PSPEXP_TEX_4444;
    d.palette = p->pal;
    d.layout = PSPEXP_LAYOUT_SWIZZLED; d.priority = PSPEXP_PRIORITY_NORMAL;   /* VRAM: the GE reads it far faster than RAM */
    f.u0 = 0; f.v0 = 0; f.w = (uint16_t)(p->texw < 64 ? p->texw : 64); f.h = (uint16_t)p->texh;
    p->tex = alloc_handle();
    if (t8) {
        uint8_t *dst = pspexp_upload_texture_begin(S.ctx, p->tex, &d, (uint32_t)((size_t)p->texw * p->texh), &f, 1);
        if (!dst) return -1;
        for (y = 0; y < p->texh; y++) {   /* 16 pixels per 16-byte block */
            uint8_t *band = dst + (size_t)(y >> 3) * (size_t)p->texw * 8u + (size_t)((y & 7) << 4);
            xx = 0;
            if (y < p->h) {
                const uint8_t *src = cv->idx8 + (size_t)y * p->w;
                for (; xx + 16 <= p->w; xx += 16) memcpy(band + ((xx >> 4) << 7), src + xx, 16);
                for (; xx < p->w; xx++) band[((xx >> 4) << 7) + (xx & 15)] = src[xx];
            }
            for (; xx < p->texw; xx++) band[((xx >> 4) << 7) + (xx & 15)] = 0;
        }
    } else {
        uint16_t *dst = (uint16_t *)pspexp_upload_texture_begin(S.ctx, p->tex, &d, (uint32_t)((size_t)p->texw * p->texh * 2), &f, 1);
        int rowblocks = p->texw >> 3;   /* 8 pixels per 16-byte block */
        if (!dst) return -1;
        for (y = 0; y < p->texh; y++) {
            uint16_t *band = dst + (size_t)(y >> 3) * (size_t)rowblocks * 64u + (size_t)((y & 7) << 3);
            xx = 0;
            if (y < p->h) {
                const uint16_t *src = cv->px16 + (size_t)y * p->w;
                for (; xx + 8 <= p->w; xx += 8) memcpy(band + ((xx >> 3) << 6), src + xx, 16);
                for (; xx < p->w; xx++) band[((xx >> 3) << 6) + (xx & 7)] = src[xx];
            }
            for (; xx < p->texw; xx++) band[((xx >> 3) << 6) + (xx & 7)] = 0;
        }
    }
    if (pspexp_upload_texture_commit(S.ctx, !x->streaming) < 0) return -1;
    S.uploads++;
    S.pages_sent++;
    if (t8) S.pages_t8++;
    return 0;
}

/* ---- the conversion pool: the other cores convert pages ----------------------- */

typedef struct Job { const Tex *x; const Page *p; int opaque; Conv *cv; } Job;
static struct {
    int n;                         /* worker threads (0: none, convert on the caller) */
    int tried;
    SDL_Thread *t[3];
    Lane *lane[4];                 /* [0] the caller's */
    SDL_mutex *mu; SDL_cond *go, *done;
    Job *jobs; int njobs, next, finished, epoch, quit;
} g_pool;
static Conv g_pageconv[16];
static Lane g_lane0;             /* the caller's colour tables */

static int pool_claim(void)
{
    int i;
    SDL_LockMutex(g_pool.mu);
    i = g_pool.next < g_pool.njobs ? g_pool.next++ : -1;
    SDL_UnlockMutex(g_pool.mu);
    return i;
}

static void pool_finish_one(void)
{
    SDL_LockMutex(g_pool.mu);
    if (++g_pool.finished == g_pool.njobs) SDL_CondBroadcast(g_pool.done);
    SDL_UnlockMutex(g_pool.mu);
}

static int pool_worker(void *arg)
{
    Lane *ln = (Lane *)arg;
    int seen = 0;
    for (;;) {
        int i;
        SDL_LockMutex(g_pool.mu);
        while (g_pool.epoch == seen && !g_pool.quit) SDL_CondWait(g_pool.go, g_pool.mu);
        if (g_pool.quit) { SDL_UnlockMutex(g_pool.mu); break; }
        seen = g_pool.epoch;
        SDL_UnlockMutex(g_pool.mu);
        while ((i = pool_claim()) >= 0) {
            const Job *j = &g_pool.jobs[i];
            convert_page(ln, j->x, j->p, j->opaque, j->cv);
            pool_finish_one();
        }
    }
    return 0;
}

/* the pool, once: workers = cores - 1 (at most 3); PSPEXP_THREADS=0 for none */
static void pool_init(void)
{
    int i, want;
    if (g_pool.tried) return;
    g_pool.tried = 1;
    g_pool.lane[0] = &g_lane0;
    want = SDL_GetCPUCount() - 1;
    if (want > 3) want = 3;
    if (getenv("PSPEXP_THREADS") && !strcmp(getenv("PSPEXP_THREADS"), "0")) want = 0;
    if (want <= 0) return;
    g_pool.mu = SDL_CreateMutex(); g_pool.go = SDL_CreateCond(); g_pool.done = SDL_CreateCond();
    if (!g_pool.mu || !g_pool.go || !g_pool.done) return;
    for (i = 0; i < want; i++) {
        g_pool.lane[i + 1] = (Lane *)calloc(1, sizeof(Lane));
        if (!g_pool.lane[i + 1]) break;
        g_pool.t[i] = SDL_CreateThread(pool_worker, "pspexp-pages", g_pool.lane[i + 1]);
        if (!g_pool.t[i]) break;
        g_pool.n++;
    }
    logf_("texture pages converted on %d threads", g_pool.n + 1);
}

/* convert jobs[0..n-1]: on the pool when there is one and more than one page */
static void convert_jobs(Job *jobs, int n)
{
    int i;
    pool_init();
    if (g_pool.n == 0 || n < 2) {
        for (i = 0; i < n; i++) convert_page(&g_lane0, jobs[i].x, jobs[i].p, jobs[i].opaque, jobs[i].cv);
        return;
    }
    SDL_LockMutex(g_pool.mu);
    g_pool.jobs = jobs; g_pool.njobs = n; g_pool.next = 0; g_pool.finished = 0; g_pool.epoch++;
    SDL_CondBroadcast(g_pool.go);
    SDL_UnlockMutex(g_pool.mu);
    while ((i = pool_claim()) >= 0) {   /* the caller works too */
        convert_page(g_pool.lane[0], jobs[i].x, jobs[i].p, jobs[i].opaque, jobs[i].cv);
        pool_finish_one();
    }
    SDL_LockMutex(g_pool.mu);
    while (g_pool.finished < g_pool.njobs) SDL_CondWait(g_pool.done, g_pool.mu);
    g_pool.njobs = 0;
    SDL_UnlockMutex(g_pool.mu);
}

/* convert and write a set of pages of one texture (in order) */
static int upload_pages(Tex *x, Page **pages, int n, int opaque)
{
    Job jobs[16];
    int i;
    for (i = 0; i < n; i++) {
        if (conv_reserve(&g_pageconv[i], (size_t)pages[i]->w * pages[i]->h) < 0) return -1;
        jobs[i].x = x; jobs[i].p = pages[i]; jobs[i].opaque = opaque; jobs[i].cv = &g_pageconv[i];
    }
    {
        uint64_t t0 = wall_ns(), t1;
        convert_jobs(jobs, n);
        t1 = wall_ns();
        S.prof_convert += t1 - t0;
        for (i = 0; i < n; i++) if (write_page(x, pages[i], opaque, &g_pageconv[i]) < 0) return -1;
        S.prof_write += wall_ns() - t1;
    }
    return 0;
}

static int upload_tex(Tex *x)
{
    int i, full, n = 0;
    Page *todo[16];
    int opaque = x->blend == SDL_BLENDMODE_NONE;
    uint64_t t0 = cpu_ns();
    page_grid(x);
    full = x->dirty || S.no_delta || !x->npages || x->npages_grid_changed || opaque != x->opaque_used;
    if (full) {
        int r, c;
        release_pages(x);
        if (x->ncol * x->nrow > 16) { x->unsupported = 1; logf_("texture %dx%d: too large (over 16 pages of 512): not drawn", x->w, x->h); return -1; }
        for (r = 0; r < x->nrow; r++) {
            for (c = 0; c < x->ncol; c++) {
                Page *p = &x->pages[x->npages++];
                memset(p, 0, sizeof(*p));
                p->x0 = x->cx[c]; p->y0 = x->ry[r];
                p->w = x->cx[c + 1] - x->cx[c];
                p->h = x->ry[r + 1] - x->ry[r];
                p->texw = pow2ceil(p->w, 16);
                p->texh = pow2ceil(p->h, 8);
                todo[n++] = p;
            }
        }
        x->opaque_used = opaque;
        x->npages_grid_changed = 0;
    } else {
        for (i = 0; i < x->npages; i++) {
            if (!(x->pdirty & (1u << i))) { S.pages_kept++; continue; }
            pspexp_release(S.ctx, x->pages[i].tex);
            todo[n++] = &x->pages[i];
        }
    }
    if (n && upload_pages(x, todo, n, opaque) < 0) { x->unsupported = 1; logf_("texture %dx%d rejected", x->w, x->h); return -1; }
    x->dirty = 0;
    x->pdirty = 0;
    S.up_ticks += cpu_ns() - t0;
    return 0;
}

/* 1 when n bytes differ. Four bytes at a time: musl's memcmp (the Omega's
 * static builds) goes byte by byte, 2-4 ms a frame there for an NES picture */
static int differs(const void *a, const void *b, size_t n)
{
    const uint8_t *p = (const uint8_t *)a, *q = (const uint8_t *)b;
    if ((((uintptr_t)p | (uintptr_t)q) & 3u) == 0) {
        const uint32_t *wp = (const uint32_t *)p, *wq = (const uint32_t *)q;
        size_t w = n >> 2, i;
        for (i = 0; i + 4 <= w; i += 4)
            if ((wp[i] ^ wq[i]) | (wp[i + 1] ^ wq[i + 1]) | (wp[i + 2] ^ wq[i + 2]) | (wp[i + 3] ^ wq[i + 3])) return 1;
        for (; i < w; i++) if (wp[i] != wq[i]) return 1;
        p += w << 2; q += w << 2; n &= 3u;
    }
    return n ? memcmp(p, q, n) != 0 : 0;
}

/* Store converted pixels (ABGR8888, `spitch` pixels per row) at r in the
 * shadow, marking the pages whose pixels actually changed. */
static void shadow_store(Tex *x, const SDL_Rect *r, const uint32_t *px, int spitch)
{
    int y, c, row = 0;
    page_grid(x);
    if (x->npages_grid_changed) x->dirty = 1;   /* pages are cut again: all of them go */
    for (y = 0; y < r->h; y++) {
        const uint32_t *src = px + (size_t)y * spitch;
        uint32_t *dst = x->rgba + (size_t)(r->y + y) * x->w;
        int yy = r->y + y;
        while (row + 1 < x->nrow && yy >= x->ry[row + 1]) row++;
        for (c = 0; c < x->ncol; c++) {
            int a0 = x->cx[c] > r->x ? x->cx[c] : r->x;
            int a1 = x->cx[c + 1] < r->x + r->w ? x->cx[c + 1] : r->x + r->w;
            if (a0 >= a1) continue;
            if (differs(dst + a0, src + (a0 - r->x), (size_t)(a1 - a0) * 4)) {
                int bit = row * x->ncol + c;
                memcpy(dst + a0, src + (a0 - r->x), (size_t)(a1 - a0) * 4);
                if (bit < 32) x->pdirty |= 1u << bit; else x->dirty = 1;
            }
        }
    }
}

/* shadow_store for a 16-bit shadow */
static void shadow_store16(Tex *x, const SDL_Rect *r, const uint16_t *px, int spitch)
{
    int y, c, row = 0;
    uint16_t *sh = (uint16_t *)x->rgba;
    page_grid(x);
    if (x->npages_grid_changed) x->dirty = 1;
    for (y = 0; y < r->h; y++) {
        const uint16_t *src = px + (size_t)y * spitch;
        uint16_t *dst = sh + (size_t)(r->y + y) * x->w;
        int yy = r->y + y;
        while (row + 1 < x->nrow && yy >= x->ry[row + 1]) row++;
        for (c = 0; c < x->ncol; c++) {
            int a0 = x->cx[c] > r->x ? x->cx[c] : r->x;
            int a1 = x->cx[c + 1] < r->x + r->w ? x->cx[c + 1] : r->x + r->w;
            if (a0 >= a1) continue;
            if (differs(dst + a0, src + (a0 - r->x), (size_t)(a1 - a0) * 2)) {
                int bit = row * x->ncol + c;
                memcpy(dst + a0, src + (a0 - r->x), (size_t)(a1 - a0) * 2);
                if (bit < 32) x->pdirty |= 1u << bit; else x->dirty = 1;
            }
        }
    }
}

static uint32_t *g_conv; static size_t g_conv_n;
static void shadow_update_(Tex *x, const SDL_Rect *rect, Uint32 format, const void *pixels, int pitch);
/* a half-size texture's update halved first: every other pixel of every other row */
static void *g_half; static size_t g_half_n;
static int halve(const Tex *x, const SDL_Rect *rect, Uint32 format, const void *pixels, int pitch, SDL_Rect *out, int *opitch)
{
    SDL_Rect r = rect ? *rect : (SDL_Rect){ 0, 0, x->w * 2, x->h * 2 };
    int bpp = SDL_BYTESPERPIXEL(format), X0 = r.x / 2, Y0 = r.y / 2, X1 = (r.x + r.w + 1) / 2, Y1 = (r.y + r.h + 1) / 2, X, Y;
    size_t need;
    if (bpp < 1 || bpp > 4 || X1 > x->w || Y1 > x->h || X1 <= X0 || Y1 <= Y0) return -1;
    need = (size_t)(X1 - X0) * (size_t)(Y1 - Y0) * (size_t)bpp;
    if (need > g_half_n) { void *b = realloc(g_half, need); if (!b) return -1; g_half = b; g_half_n = need; }
    for (Y = Y0; Y < Y1; Y++) {
        int sy = 2 * Y < r.y ? r.y : 2 * Y;
        const Uint8 *row = (const Uint8 *)pixels + (size_t)(sy - r.y) * (size_t)pitch;
        Uint8 *o = (Uint8 *)g_half + (size_t)(Y - Y0) * (size_t)(X1 - X0) * (size_t)bpp;
        for (X = X0; X < X1; X++) {
            int sx = 2 * X < r.x ? r.x : 2 * X;
            memcpy(o + (size_t)(X - X0) * (size_t)bpp, row + (size_t)(sx - r.x) * (size_t)bpp, (size_t)bpp);
        }
    }
    out->x = X0; out->y = Y0; out->w = X1 - X0; out->h = Y1 - Y0;
    *opitch = (X1 - X0) * bpp;
    return 0;
}

/* A whole frame between the PSP's size and 512 a side (fheroes2's 640x480), shown shrunk: the GE
 * draws a shrunk texture by its nearest texels, every other column and row skipped somewhere, so
 * thin lines and edges sparkled as the map scrolled ("tiny dots", the user). Kept instead at the
 * size it is shown, each pixel the average of the game's pixels under it, and drawn 1:1; the
 * game's pixels kept in full for the magnifier (PSPEXP_MAGNIFY), which shows them at 1:1. */
static uint32_t *g_fit; static size_t g_fit_n;
static void fit_rect(Tex *x, const SDL_Rect *r)   /* the shadow's pixels under the game's rectangle r, from x->full */
{
    float f = x->fitf, inv = 1.0f / f;
    int X0 = (int)floorf(r->x * f), Y0 = (int)floorf(r->y * f), X1 = (int)ceilf((r->x + r->w) * f), Y1 = (int)ceilf((r->y + r->h) * f), X, Y;
    size_t need;
    SDL_Rect o;
    if (X1 > x->w) X1 = x->w;
    if (Y1 > x->h) Y1 = x->h;
    if (X1 <= X0 || Y1 <= Y0) return;
    need = (size_t)(X1 - X0) * (size_t)(Y1 - Y0);
    if (need > g_fit_n) { uint32_t *b = (uint32_t *)realloc(g_fit, need * 4); if (!b) return; g_fit = b; g_fit_n = need; }
    for (Y = Y0; Y < Y1; Y++) {
        int sy0 = (int)(Y * inv), sy1 = (int)((Y + 1) * inv);
        if (sy1 > x->full_h) sy1 = x->full_h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (X = X0; X < X1; X++) {
            int sx0 = (int)(X * inv), sx1 = (int)((X + 1) * inv), xx, yy;
            uint32_t a = 0, rr = 0, gg = 0, bb = 0, n;
            if (sx1 > x->full_w) sx1 = x->full_w;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            n = (uint32_t)((sx1 - sx0) * (sy1 - sy0));
            for (yy = sy0; yy < sy1; yy++) {
                const uint32_t *row = x->full + (size_t)yy * x->full_w;
                for (xx = sx0; xx < sx1; xx++) { uint32_t c = row[xx]; a += c >> 24; rr += (c >> 16) & 255; gg += (c >> 8) & 255; bb += c & 255; }
            }
            g_fit[(size_t)(Y - Y0) * (X1 - X0) + (X - X0)] = ((a / n) << 24) | ((rr / n) << 16) | ((gg / n) << 8) | (bb / n);
        }
    }
    o.x = X0; o.y = Y0; o.w = X1 - X0; o.h = Y1 - Y0;
    shadow_update_(x, &o, SDL_PIXELFORMAT_ARGB8888, g_fit, o.w * 4);
}
static void fit_update(Tex *x, const SDL_Rect *rect, Uint32 format, const void *pixels, int pitch)
{
    SDL_Rect r = rect ? *rect : (SDL_Rect){ 0, 0, x->full_w, x->full_h };
    if (r.x < 0 || r.y < 0 || r.x + r.w > x->full_w || r.y + r.h > x->full_h || r.w <= 0 || r.h <= 0) return;
    if (SDL_ConvertPixels(r.w, r.h, format, pixels, pitch, SDL_PIXELFORMAT_ARGB8888, x->full + (size_t)r.y * x->full_w + r.x, x->full_w * 4) < 0) return;
    if (x->fitf >= 1.0f) shadow_update_(x, &r, SDL_PIXELFORMAT_ARGB8888, x->full + (size_t)r.y * x->full_w + r.x, x->full_w * 4);
    else fit_rect(x, &r);
}
/* the frame's scale changed (the window, the magnifier): the shadow again at the new size */
static void fit_rescale(Tex *x)
{
    float f = S.sx > 0 && S.sx < 1.0f ? S.sx : 1.0f;
    int w = (int)ceilf(x->full_w * f), h = (int)ceilf(x->full_h * f);
    SDL_Rect all = { 0, 0, x->full_w, x->full_h };
    uint32_t *b;
    if (f == x->fitf && x->w == w && x->h == h) return;
    if (!(b = (uint32_t *)realloc(x->rgba, (size_t)w * h * 4))) return;
    x->rgba = b; x->w = w; x->h = h; x->fitf = f;
    x->argb = 0; x->shadow16 = 0; x->dirty = 1;
    if (f >= 1.0f) shadow_update_(x, &all, SDL_PIXELFORMAT_ARGB8888, x->full, x->full_w * 4);
    else fit_rect(x, &all);
}

/* a texture larger than the PSP's screen (a whole frame) made a fitted one: its pixels, if any, from
 * surf. Asked for (PSPEXP_TEX_FIT=1, a recipe's) until the 640x480 games are each looked at again:
 * it changes how they look (averaged, not sampled) */
static int make_fit(Tex *x, int w, int h, SDL_Surface *surf)
{
    if (!(w > 480 || h > 272) || !(getenv("PSPEXP_TEX_FIT") && !strcmp(getenv("PSPEXP_TEX_FIT"), "1"))) return 0;
    if (!(x->full = (uint32_t *)calloc((size_t)w * h, 4))) return 0;
    x->fit = 1; x->full_w = w; x->full_h = h; x->fitf = 0;   /* (0: fit_rescale makes the shadow) */
    if (surf) SDL_ConvertPixels(w, h, surf->format->format, surf->pixels, surf->pitch, SDL_PIXELFORMAT_ARGB8888, x->full, w * 4);
    fit_rescale(x);
    logf_("texture %dx%d kept at the size shown (%dx%d)", w, h, x->w, x->h);
    return 1;
}

/* the game's new pixels into the shadow; its CPU time counts as texture work */
static void shadow_update(Tex *x, const SDL_Rect *rect, Uint32 format, const void *pixels, int pitch)
{
    uint64_t t0 = cpu_ns(), w0 = wall_ns();
    if (x->fit) fit_update(x, rect, format, pixels, pitch);
    else if (x->half) {
        SDL_Rect h;
        int hp;
        if (halve(x, rect, format, pixels, pitch, &h, &hp) == 0) shadow_update_(x, &h, format, g_half, hp);
    } else shadow_update_(x, rect, format, pixels, pitch);
    S.up_ticks += cpu_ns() - t0;
    S.prof_shadow += wall_ns() - w0;
}
static void shadow_update_(Tex *x, const SDL_Rect *rect, Uint32 format, const void *pixels, int pitch)
{
    SDL_Rect r = rect ? *rect : (SDL_Rect){ 0, 0, x->w, x->h };
    size_t n = (size_t)r.w * r.h;
    Uint32 want;
    int y;
    if (r.x < 0 || r.y < 0 || r.x + r.w > x->w || r.y + r.h > x->h || r.w <= 0 || r.h <= 0) return;
    /* keep the shadow in the game's own 32-bit layout when it sends one:
     * then nothing is converted before comparing (a whole-texture update can
     * switch the layout; it re-sends every page anyway) */
    /* RGB565 in, RGB565 kept: half the bytes to compare, no conversion. The
     * format is the texture's own (every update of it has it), so the layout
     * follows it at the first update, whatever that update's size: emulators
     * update part of a texture made at the core's largest size (Sonic's
     * 320x224 in 320x240), and waiting for a whole-texture update kept them
     * on the slow path (5-8 ms a frame on the GameShell) */
    if (format == SDL_PIXELFORMAT_RGB565 && !x->shadow16) { x->shadow16 = 1; x->argb = 0; x->dirty = 1; }
    else if (format != SDL_PIXELFORMAT_RGB565 && x->shadow16) { x->shadow16 = 0; x->dirty = 1; }
    if (x->shadow16) {
        uint16_t *sh = (uint16_t *)x->rgba;
        const uint16_t *src = (const uint16_t *)pixels;
        int sp = pitch / 2;
        if (format != SDL_PIXELFORMAT_RGB565 || (pitch & 1)) {   /* another format into a 565 shadow */
            if (n * 2 > g_conv_n * 4) {
                uint32_t *b = (uint32_t *)realloc(g_conv, n * 2 + 4);
                if (!b) return;
                g_conv = b; g_conv_n = n / 2 + 1;
            }
            SDL_ConvertPixels(r.w, r.h, format, pixels, pitch, SDL_PIXELFORMAT_RGB565, g_conv, r.w * 2);
            src = (const uint16_t *)g_conv; sp = r.w;
        }
        if (S.no_delta || x->dirty) {
            for (y = 0; y < r.h; y++) memcpy(sh + (size_t)(r.y + y) * x->w + r.x, src + (size_t)y * sp, (size_t)r.w * 2);
            x->dirty = 1;
        } else shadow_store16(x, &r, src, sp);
        return;
    }
    want = x->argb ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888;
    if ((format == SDL_PIXELFORMAT_ARGB8888 || format == SDL_PIXELFORMAT_ABGR8888) && format != want) {   /* the same: follow the texture's format */
        want = format;
        x->argb = format == SDL_PIXELFORMAT_ARGB8888;
        x->dirty = 1;
    }
    if (S.no_delta || x->dirty) {   /* all of it goes anyway */
        if (format == want && (pitch & 3) == 0)
            for (y = 0; y < r.h; y++)
                memcpy(x->rgba + (size_t)(r.y + y) * x->w + r.x, (const Uint8 *)pixels + (size_t)y * pitch, (size_t)r.w * 4);
        else
            SDL_ConvertPixels(r.w, r.h, format, pixels, pitch, want, x->rgba + (size_t)r.y * x->w + r.x, x->w * 4);
        x->dirty = 1;
        return;
    }
    if (format == want && (pitch & 3) == 0) {
        shadow_store(x, &r, (const uint32_t *)pixels, pitch / 4);
        return;
    }
    if (n > g_conv_n) {
        uint32_t *b = (uint32_t *)realloc(g_conv, n * 4);
        if (!b) { x->dirty = 1; SDL_ConvertPixels(r.w, r.h, format, pixels, pitch, want, x->rgba + (size_t)r.y * x->w + r.x, x->w * 4); return; }
        g_conv = b; g_conv_n = n;
    }
    SDL_ConvertPixels(r.w, r.h, format, pixels, pitch, want, g_conv, r.w * 4);
    shadow_store(x, &r, g_conv, r.w);
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
    if ((x->dirty || x->pdirty) && upload_tex(x) < 0) return;
    s = src ? *src : (SDL_Rect){ 0, 0, x->w, x->h };
    if (x->half && src) { int x1 = (s.x + s.w + 1) / 2, y1 = (s.y + s.h + 1) / 2; s.x /= 2; s.y /= 2; s.w = x1 - s.x; s.h = y1 - s.y; }   /* (a half-size texture) */
    if (x->fit && src) {   /* (a texture kept at the size it is shown) */
        int x0 = (int)floorf(s.x * x->fitf), y0 = (int)floorf(s.y * x->fitf), x1 = (int)ceilf((s.x + s.w) * x->fitf), y1 = (int)ceilf((s.y + s.h) * x->fitf);
        s.x = x0; s.y = y0; s.w = (x1 > x->w ? x->w : x1) - x0; s.h = (y1 > x->h ? x->h : y1) - y0;
    }
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

/* the frames that fit in the ring (the rest are dropped by the caller, or waited for) */
static size_t audio_push(const int16_t *stereo, size_t frames)
{
    size_t f;
    if (!__atomic_load_n(&S.amu, __ATOMIC_ACQUIRE) || !S.aring) return 0;   /* (the pipe's sound before the link is up: read, so the program does not block, and let go) */
    SDL_LockMutex(S.amu);
    for (f = 0; f < frames && S.abuf < S.acap; f++) {
        S.aring[S.atail * 2] = stereo[f * 2];
        S.aring[S.atail * 2 + 1] = stereo[f * 2 + 1];
        S.atail = (S.atail + 1) % S.acap;
        S.abuf++;
    }
    SDL_UnlockMutex(S.amu);
    return f;
}

static void audio_tap(int slot, const Uint8 *data, int len)
{
    SDL_AudioStream *st = S.dev_stream[slot];
    int16_t tmp[8192];
    int got;
    const SDL_AudioSpec *sp = &S.dev_spec[slot];
    if (sp->format == AUDIO_S16SYS && sp->channels == 2 && sp->freq == 44100) {
        /* already the PSP's format: no conversion (SDL's converter is floating
         * point, emulated in software on FPU-less hosts such as the Omega2) */
        audio_push((const int16_t *)data, (size_t)len / 4);
        return;
    }
    if (sp->format == AUDIO_S16SYS && (sp->channels == 1 || sp->channels == 2) &&
        sp->freq > 0 && sp->freq < 44100 && 44100 % sp->freq == 0) {
        /* integer upsampling by 44100 / freq (2, 4 for 22050, 11025), linear */
        static int16_t prev[8][2];
        const int16_t *in = (const int16_t *)data;
        int k = 44100 / sp->freq, ch = sp->channels, frames = len / (2 * ch), f, j;
        for (f = 0; f < frames; f++) {
            int16_t out[2 * 4];
            int16_t l = in[f * ch], r = ch == 2 ? in[f * ch + 1] : l;
            int n = 0;
            for (j = 1; j <= k; j++) {
                out[2 * n] = (int16_t)(prev[slot][0] + (l - prev[slot][0]) * j / k);
                out[2 * n + 1] = (int16_t)(prev[slot][1] + (r - prev[slot][1]) * j / k);
                if (++n == 4) { audio_push(out, 4); n = 0; }
            }
            if (n) audio_push(out, (size_t)n);
            prev[slot][0] = l; prev[slot][1] = r;
        }
        return;
    }
    if (!st) return;
    SDL_AudioStreamPut(st, data, len);
    while ((got = SDL_AudioStreamGet(st, tmp, (int)sizeof(tmp))) > 0) audio_push(tmp, (size_t)got / 4);
}

#ifndef PSPEXP_STATIC_RENAME
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
/* PSPEXP_AUDIO_FIFO=PATH: sound from a named pipe to the PSP, as a WAV
 * stream (s16, stereo, 44.1 kHz; mono doubled). For games that play through
 * OpenAL or ALSA, not SDL: OpenAL Soft's "wave" output and ALSA's default
 * (a file output, WAV) write into the pipe (scripts/board/pspexp-run sets
 * both up). */
/* read(), again when a signal cut it short: the reader must not end, or the program's next
 * open of the pipe waits for it for ever (Unreal Tournament stood still at its title) */
static ssize_t fifo_read(int fd, void *b, size_t n)
{
    ssize_t r;
    do r = read(fd, b, n); while (r < 0 && errno == EINTR);
    return r;
}

static void *audio_fifo_thread(void *arg)
{
    const char *path = (const char *)arg;
    int fd, channels = 2;
    unsigned char hdr[8];
    static int16_t buf[4096 * 2];
    for (;;) {   /* each opening of the pipe: a new stream (the game may reopen its device) */
        if ((fd = open(path, O_RDONLY)) < 0) {   /* (open waits for a writer: a signal may cut it short) */
            if (errno == EINTR) continue;
            logf_("audio pipe %s: %s; no sound from it", path, strerror(errno));
            return NULL;
        }
        if (fifo_read(fd, hdr, 4) == 4 && !memcmp(hdr, "RIFF", 4) && fifo_read(fd, hdr, 8) == 8) {   /* RIFF size WAVE */
            for (;;) {   /* the chunks up to data */
                uint32_t len;
                if (fifo_read(fd, hdr, 8) != 8) break;
                len = (uint32_t)hdr[4] | (uint32_t)hdr[5] << 8 | (uint32_t)hdr[6] << 16 | (uint32_t)hdr[7] << 24;
                if (!memcmp(hdr, "data", 4)) break;
                {
                    unsigned char fmt[64];
                    uint32_t k = len < sizeof(fmt) ? len : (uint32_t)sizeof(fmt), rest = len - k;
                    if (fifo_read(fd, fmt, k) != (ssize_t)k) break;
                    if (!memcmp(hdr, "fmt ", 4) && k >= 16) {
                        unsigned rate = (unsigned)fmt[4] | (unsigned)fmt[5] << 8 | (unsigned)fmt[6] << 16;
                        channels = fmt[2];
                        if (rate != 44100 || (fmt[14] | fmt[15] << 8) != 16 || channels < 1 || channels > 2)
                            logf_("audio pipe: %u Hz, %d channels, %d bits: 44100 Hz 16-bit stereo or mono wanted", rate, channels, fmt[14] | fmt[15] << 8);
                    }
                    while (rest > 0) { unsigned char skip[256]; ssize_t r = fifo_read(fd, skip, rest < 256 ? rest : 256); if (r <= 0) break; rest -= (uint32_t)r; }
                }
            }
            logf_("audio pipe %s: streaming to the PSP", path);
            {
            /* taken at 44.1 kHz by the clock, at most 100 ms ahead: the pipe then holds the writer to
             * real time. OpenAL's wave output keeps time itself, ALSA's file output (FMOD: Celeste 64)
             * writes as fast as it is let: its sound reached the link many times faster than it
             * plays, and the PSP, swamped, dropped the link */
            struct timespec t0, tn;
            uint64_t taken = 0;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (;;) {
                ssize_t r;
                size_t frames, f;
                uint64_t due;
                clock_gettime(CLOCK_MONOTONIC, &tn);
                due = (uint64_t)((double)(tn.tv_sec - t0.tv_sec) * 44100.0 + (double)(tn.tv_nsec - t0.tv_nsec) * 44100e-9);
                if (taken + 22050 < due) taken = due;   /* the writer stopped a while: no catching up in a burst */
                if (taken > due + 4410) { usleep((useconds_t)((taken - due - 4410) * 1000000 / 44100) + 1000); continue; }
                r = fifo_read(fd, buf, channels == 2 ? sizeof(buf) / 4 : sizeof(buf) / 8);   /* 1024 frames at a time */
                if (r <= 0) break;
                if (channels == 2) frames = (size_t)r / 4;
                else { frames = (size_t)r / 2; for (f = frames; f-- > 0; ) { buf[f * 2 + 1] = buf[f]; buf[f * 2] = buf[f]; } }
                /* all of it, waiting while the ring is full (the program not sending it a while) */
                for (f = 0; f < frames; ) {
                    size_t k = audio_push(buf + f * 2, frames - f);
                    f += k;
                    if (f < frames) { if (!__atomic_load_n(&S.amu, __ATOMIC_ACQUIRE) || !S.aring) break; usleep(5000); }
                }
                taken += frames;
            }
            }
        }
        close(fd);
    }
}

static void start_audio_fifo(void)
{
    static char path[512];
    const char *p = getenv("PSPEXP_AUDIO_FIFO");
    if (!p || !*p || path[0]) return;
    snprintf(path, sizeof(path), "%s", p);
    {   /* pspexp-run makes the pipe before the program starts: a program that opens its OpenAL device
         * before its window (Vanilla Conquer's Red Alert) found no pipe, OpenAL made a plain file of that
         * name, and the pipe made here afterwards was never written: no sound. A pipe already there is
         * kept; anything else is replaced. */
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISFIFO(st.st_mode)) {
            unlink(path);
            if (mkfifo(path, 0600) < 0) { logf_("audio pipe %s: cannot make it", path); return; }
        }
    }
    {   /* read from when the library loads: OpenAL's open of the pipe waits for a reader */
        pthread_t t;
        if (pthread_create(&t, NULL, audio_fifo_thread, path) == 0) pthread_detach(t);
    }
}
__attribute__((constructor)) static void audio_fifo_early(void) { start_audio_fifo(); }
#endif
/* A worn PSP-1000's nub rests well off the middle: one measured -5400 / +2000 in SDL's units, the
 * user's PSP (2026-10-17, untouched) -44 / +31 of the nub's 128, years the same. Within NUB_REST of
 * the middle is the middle, for every use of the nub (joystick, mouse, keys): at 35 the pointer
 * crept left and down by itself (LinCity-NG's, into the corner, the map edge-scrolling) */
#define NUB_REST 56
#define NUB_DEAD (NUB_REST * 258)
/* the nub past its rest as the pointer's speed, -1..1: the square of its tilt as before, less that at
 * the rest's edge (no jump there), so a firm push moves as fast as it did (Quake's aim) */
static float nub_speed_of(int a)
{
    int m = a < 0 ? -a : a;
    float f;
    if (m <= NUB_REST) return 0.0f;
    if (m > 127) m = 127;
    f = (float)(m * m - NUB_REST * NUB_REST) / (float)(127 * 127 - NUB_REST * NUB_REST);
    return a < 0 ? -f : f;
}
static int nub_dead(int v)
{
    int m = v < 0 ? -v : v;
    if (m <= NUB_DEAD) return 0;
    m = (int)((long)(m - NUB_DEAD) * 32767 / (32767 - NUB_DEAD));
    if (m > 32767) m = 32767;
    return v < 0 ? -m : m;
}

static void *quit_watchdog(void *arg)
{
    (void)arg;
    usleep(1500000);
    logf_("still here 1.5 s after leaving: ended");
    _exit(0);
    return NULL;
}

/* after the request to quit: a program that no longer reads its input for 3 s is stuck in its own
 * shutdown (OpenRA's Red Alert: asked, it stopped polling, so the 2 s way out, which runs on its
 * polling, never came, and the PSP stayed with it). One still reading (an "are you sure?") is left */
static volatile Uint32 last_feed;
static void *quit_request_watchdog(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < 300; i++) {   /* 30 s at most */
        usleep(100000);
        if (SDL_GetTicks() - last_feed > 3000) { logf_("no input read for 3 s since asked to quit: ended"); _exit(0); }
    }
    return NULL;
}

/* the PSP is the input whatever has the board's focus: SDL drops joystick events while no window of
 * the program has keyboard focus (a headless board's never does; the focus the layer shows the program
 * is an event, not SDL's own), so Sonic Robo Blast 2 got no button of the PSP's controller at all */
__attribute__((constructor)) static void joystick_background_events(void)
{
    setenv("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 0);
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
static void link_lock(void);
static void link_unlock(void);
static void audio_drain(void);
static int audio_pacer(void *arg)
{
    Uint64 f = SDL_GetPerformanceFrequency(), t0 = SDL_GetPerformanceCounter();
    Uint64 produced[8] = { 0 };
    int started[8] = { 0 };   /* the slot has played: a pause after that is silence */
    Uint8 *buf = NULL; int buflen = 0;
    (void)arg;
    while (!S.audio_thread_stop) {
        int slot;
        double elapsed = (double)(SDL_GetPerformanceCounter() - t0) / (double)f;
        for (slot = 1; slot < 8; slot++) {
            const SDL_AudioSpec *sp = &S.dev_spec[slot];
            Uint64 target;
            int bytes;
            if (S.pace_mu) SDL_LockMutex(S.pace_mu);
            if (!S.dev_selfpaced[slot] || !S.dev_cb[slot] || !sp->freq) { started[slot] = 0; if (S.pace_mu) SDL_UnlockMutex(S.pace_mu); continue; }
            if (SDL_GetAudioDeviceStatus(S.dev_id[slot]) != SDL_AUDIO_PLAYING) {
                /* paused: a sound card plays silence, so silence goes, once sound has gone (Mupen64Plus pauses
                 * its device for a moment whenever its emulation falls behind: sending nothing let the PSP's
                 * buffer run dry, and it then waits to refill 100 ms before playing on: a gap every two seconds
                 * in GoldenEye) */
                if (started[slot]) {
                    int bytes0 = sp->samples * sp->channels * (int)(SDL_AUDIO_BITSIZE(sp->format) / 8);
                    target = (Uint64)(elapsed * sp->freq);
                    if (bytes0 > buflen) { free(buf); buf = (Uint8 *)malloc((size_t)bytes0); buflen = bytes0; }
                    if (buf && produced[slot] + (Uint64)sp->freq / 8 >= target) {
                        memset(buf, sp->silence, (size_t)bytes0);
                        while (produced[slot] < target) { audio_tap(slot, buf, bytes0); produced[slot] += (Uint64)sp->samples; S.silent_ms += (unsigned long)sp->samples * 1000u / (unsigned long)sp->freq; }
                    } else produced[slot] = target;
                } else produced[slot] = (Uint64)(elapsed * sp->freq);
                if (S.pace_mu) SDL_UnlockMutex(S.pace_mu);
                continue;
            }
            started[slot] = 1;
            target = (Uint64)(elapsed * sp->freq);
            bytes = sp->samples * sp->channels * (int)(SDL_AUDIO_BITSIZE(sp->format) / 8);
            if (bytes > buflen) { free(buf); buf = (Uint8 *)malloc((size_t)bytes); buflen = bytes; }
            if (!buf) { if (S.pace_mu) SDL_UnlockMutex(S.pace_mu); break; }
            /* never more than ~120 ms ahead (after a stall), never behind */
            if (produced[slot] + (Uint64)sp->freq / 8 < target) produced[slot] = target - (Uint64)sp->freq / 8;
            while (produced[slot] < target) {
                SDL_LockAudioDevice(S.dev_id[slot]);
                S.dev_cb[slot](S.dev_ud[slot], buf, bytes);
                SDL_UnlockAudioDevice(S.dev_id[slot]);
                audio_tap(slot, buf, bytes);
                produced[slot] += (Uint64)sp->samples;
                S.sound_ms += (unsigned long)sp->samples * 1000u / (unsigned long)sp->freq;
            }
            if (S.pace_mu) SDL_UnlockMutex(S.pace_mu);
        }
#ifdef PSPEXP_HOST_GL
        /* sent from here as soon as made, not only when the program swaps or polls: GoldenEye and
         * Perfect Dark, heavier, left gaps over the 100 ms the PSP keeps, and it ran dry about every
         * two seconds. (The board-GPU layer only: there every use of the link is under its lock,
         * while the PSP-drawn one builds a frame's draws without it.) */
        link_lock();
        audio_drain();
        link_unlock();
#endif
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
    if (!S.pace_mu) S.pace_mu = SDL_CreateMutex();
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

/* SDL's keyboard state with the PSP's keys in: one table, handed out from the
 * first SDL_GetKeyboardState and kept current at every change. Hurrican asks
 * once, before the layer is up, and keeps reading the same pointer: SDL's own
 * table never sees the keys we push, and a copy made only when asked went
 * stale (no key reached its intro or menus). */
static Uint8 kb_merged[SDL_NUM_SCANCODES];

static void kb_refresh(void)
{
    int n = 0, i;
    const Uint8 *real = REAL(SDL_GetKeyboardState)(&n);
    for (i = 0; i < SDL_NUM_SCANCODES; i++) kb_merged[i] = (Uint8)((i < n ? real[i] : 0) | S.kheld[i]);
}

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
    if (ev.key.keysym.scancode < SDL_NUM_SCANCODES) S.kheld[ev.key.keysym.scancode] = (Uint8)(down != 0);
    SDL_PushEvent(&ev);
    kb_refresh();
}

/* a held key's repeats, as a keyboard sends them (games that move a step per KEYDOWN) */
static void push_key_repeat(SDL_Keycode sym)
{
    SDL_Event ev;
    if (sym == SDLK_UNKNOWN) return;
    SDL_zero(ev);
    ev.type = SDL_KEYDOWN;
    ev.key.state = SDL_PRESSED;
    ev.key.repeat = 1;
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
    if (down) S.mbuttons |= SDL_BUTTON(button); else S.mbuttons &= ~(Uint32)SDL_BUTTON(button);
    ev.button.windowID = S.win ? SDL_GetWindowID(S.win) : 0;
    SDL_PushEvent(&ev);
}

static void setup_vjoy(void)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    char guid[64], mapping[256];
    static int failed;   /* once: an SDL built without virtual joysticks (the Omega's) stays so */
    if (S.vjoy_handle || failed) return;   /* not S.vjoy: it is -1 only once connected, and games may count joysticks before */
    if (!SDL_WasInit(SDL_INIT_JOYSTICK)) return;   /* the game did not ask for joysticks */
    if (getenv("PSPEXP_JOYSTICK") && !strcmp(getenv("PSPEXP_JOYSTICK"), "0")) { failed = 1; return; }   /* keys only: games that take both would get each press twice */
    S.vjoy = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 2, 12, 1);
    if (S.vjoy < 0) { failed = 1; logf_("virtual joystick: %s (PSPEXP_KEYS still works)", SDL_GetError()); return; }
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(S.vjoy), guid, sizeof(guid));
    /* the trailing comma matters: SDL appends its own ",crc:..." only after one, else straight on,
     * and "lefty:a1crc:db13" is no binding: the controller had no up and down at all (Diablo's
     * hero and Celeste 64's Madeline went only left and right; the joystick's own axis was right) */
    snprintf(mapping, sizeof(mapping),
             "%s,PSP (PSPExp),a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,start:b6,back:b7,"
             "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,", guid);
    SDL_GameControllerAddMapping(mapping);
    S.vjoy_handle = SDL_JoystickOpen(S.vjoy);
    logf_("virtual game controller attached (%s)", guid);
#endif
}

static uint32_t script_pad;   /* PSPEXP_SCRIPT's "pad:" buttons, held as if pressed on the PSP */
static int script_nub, script_nub_x, script_nub_y;   /* its "nub:XxY": the nub held there (-127..127 each) */

static void feed_input(void)
{
    const PspExpInput *in = pspexp_input(S.ctx);
    PspExpInput with_script;
    uint32_t pressed, released;
    int i;
    last_feed = SDL_GetTicks();
    if (in->sequence == 0 && in->timestamp_us == 0) return;
    if (script_pad || script_nub) {
        with_script = *in;
        with_script.buttons |= script_pad;
        if (script_nub) { with_script.analog_x = (uint8_t)(128 + script_nub_x); with_script.analog_y = (uint8_t)(128 + script_nub_y); }
        in = &with_script;
    }
    pressed = in->buttons & ~S.prev_buttons;
    released = S.prev_buttons & ~in->buttons;
    S.prev_buttons = in->buttons;
    if (S.magnify_btn >= 0) {   /* the magnifier's button is the layer's alone: the game never sees it */
        uint32_t m = BUTTON_BITS[S.magnify_btn];
        if (pressed & m) {
            Tex *tx;
            S.magnify = !S.magnify;
            update_scale();
            for (tx = S.textures; tx; tx = tx->next) tx->dirty = 1;   /* sent again at the new size */
            if (S.surface_tex) S.surface_tex->dirty = 1;
        }
        pressed &= ~m; released &= ~m;
    }

    setup_vjoy();
    if (S.vjoy_handle) {
        Uint8 hat = 0;
        /* the nub (0..255, 128 the middle) as SDL's -32768..32767, clamped: at 0 (fully up or left)
         * 128 * 258 is 33024, which wrapped round in 16 bits to +32512, the opposite way at full tilt
         * (Super Mario 64's Mario, pushed up, ran back at the camera and round in circles) */
        int ax = ((int)in->analog_x - 128) * 258, ay = ((int)in->analog_y - 128) * 258;
        ax = ax < -32768 ? -32768 : ax > 32767 ? 32767 : ax;
        ay = ay < -32768 ? -32768 : ay > 32767 ? 32767 : ay;
        /* a PSP-1000's nub rests off the middle and trembles (one measured: -5400 / +2000, steps of
         * 260): within NUB_DEAD of it is the middle, the rest scaled to the full range */
        ax = nub_dead(ax); ay = nub_dead(ay);
        SDL_JoystickSetVirtualAxis(S.vjoy_handle, 0, (Sint16)ax);
        SDL_JoystickSetVirtualAxis(S.vjoy_handle, 1, (Sint16)ay);
        for (i = 0; i < 8; i++) SDL_JoystickSetVirtualButton(S.vjoy_handle, i, (in->buttons & BUTTON_BITS[i]) ? 1 : 0);
        if (pressed && getenv("PSPEXP_LOG_EVENTS"))
            logf_("input: buttons %#x pressed; joystick events %s, button 0 now %d", (unsigned)pressed,
                  SDL_JoystickEventState(SDL_QUERY) == SDL_ENABLE ? "on" : "OFF", SDL_JoystickGetButton(S.vjoy_handle, 0));
        if (in->buttons & PSPEXP_BUTTON_UP) hat |= SDL_HAT_UP;
        if (in->buttons & PSPEXP_BUTTON_DOWN) hat |= SDL_HAT_DOWN;
        if (in->buttons & PSPEXP_BUTTON_LEFT) hat |= SDL_HAT_LEFT;
        if (in->buttons & PSPEXP_BUTTON_RIGHT) hat |= SDL_HAT_RIGHT;
        SDL_JoystickSetVirtualHat(S.vjoy_handle, 0, hat);
    }
    for (i = 0; i < B_COUNT; i++) {
        int k;
        Uint32 now = SDL_GetTicks();
        if (S.nub_mouse && (i == S.mouse_left || i == S.mouse_right)) continue;
        if (pressed & BUTTON_BITS[i]) { S.held_since[i] = now ? now : 1; S.last_repeat[i] = now; }
        if (released & BUTTON_BITS[i]) S.held_since[i] = 0;
        for (k = 0; k < 3; k++) {
            if (S.keys[i][k] == SDLK_UNKNOWN) continue;
            if (pressed & BUTTON_BITS[i]) push_key(S.keys[i][k], 1);
            if (released & BUTTON_BITS[i]) push_key(S.keys[i][k], 0);
        }
        /* held: repeats after 400 ms, every 40 ms (a keyboard's) */
        if (S.held_since[i] && (in->buttons & BUTTON_BITS[i]) && now - S.held_since[i] >= 400 && now - S.last_repeat[i] >= 40) {
            S.last_repeat[i] = now;
            for (k = 0; k < 3; k++) push_key_repeat(S.keys[i][k]);
        }
    }
    /* START + SELECT held: the way out of any game. After 1 s the game is
     * asked to quit (SDL_QUIT, as when its window is closed); still held at
     * 2 s (a game that asks "are you sure?"), the process ends. */
    if ((in->buttons & (PSPEXP_BUTTON_START | PSPEXP_BUTTON_SELECT)) == (PSPEXP_BUTTON_START | PSPEXP_BUTTON_SELECT)) {
        Uint32 now = SDL_GetTicks();
        if (!S.quit_hold) S.quit_hold = now ? now : 1;
        if (!S.quit_sent && now - S.quit_hold >= 1000) {
            SDL_Event ev;
            SDL_zero(ev);
            ev.type = SDL_QUIT;
            SDL_PushEvent(&ev);
            S.quit_sent = 1;
            logf_("START + SELECT held: asking the game to quit");
            {
                static int watching;
                pthread_t t;
                if (!watching && pthread_create(&t, NULL, quit_request_watchdog, NULL) == 0) { pthread_detach(t); watching = 1; }
            }
        }
        if (now - S.quit_hold >= 2000) {
            /* exit() runs the program's own cleanup, and some hang in it (Luanti, Lugaru HD and Red
             * Alert stayed alive, the PSP never given back): a thread ends the process outright
             * 1.5 s later if it is still there */
            pthread_t t;
            logf_("START + SELECT held 2 s: leaving");
            if (pthread_create(&t, NULL, quit_watchdog, NULL) == 0) pthread_detach(t);
            exit(0);
        }
    } else { S.quit_hold = 0; S.quit_sent = 0; }
    if (S.nub_keys) {
        /* The nub as the four arrow keys, with hysteresis so a held diagonal stays held. */
        int ax = (int)in->analog_x - 128, ay = (int)in->analog_y - 128;
        uint32_t dirs = 0, changed;
        /* (let go only back within NUB_REST: at 30 a nub resting at -44 kept left held once pushed) */
        if (ax < -(NUB_REST + 14) || (ax < -NUB_REST && (S.nub_dirs & PSPEXP_BUTTON_LEFT))) dirs |= PSPEXP_BUTTON_LEFT;
        if (ax > NUB_REST + 14 || (ax > NUB_REST && (S.nub_dirs & PSPEXP_BUTTON_RIGHT))) dirs |= PSPEXP_BUTTON_RIGHT;
        if (ay < -(NUB_REST + 14) || (ay < -NUB_REST && (S.nub_dirs & PSPEXP_BUTTON_UP))) dirs |= PSPEXP_BUTTON_UP;
        if (ay > NUB_REST + 14 || (ay > NUB_REST && (S.nub_dirs & PSPEXP_BUTTON_DOWN))) dirs |= PSPEXP_BUTTON_DOWN;
        changed = dirs ^ S.nub_dirs;
        S.nub_dirs = dirs;
        if (changed & PSPEXP_BUTTON_LEFT) push_key(S.keys[B_LEFT][0], (dirs & PSPEXP_BUTTON_LEFT) != 0);
        if (changed & PSPEXP_BUTTON_RIGHT) push_key(S.keys[B_RIGHT][0], (dirs & PSPEXP_BUTTON_RIGHT) != 0);
        if (changed & PSPEXP_BUTTON_UP) push_key(S.keys[B_UP][0], (dirs & PSPEXP_BUTTON_UP) != 0);
        if (changed & PSPEXP_BUTTON_DOWN) push_key(S.keys[B_DOWN][0], (dirs & PSPEXP_BUTTON_DOWN) != 0);
    }
    if (S.nub_mouse) {
        Uint32 now = SDL_GetTicks();
        float dt = (now - S.ptick > 100 ? 100 : now - S.ptick) / 1000.0f;
        int ax = (int)in->analog_x - 128, ay = (int)in->analog_y - 128;
        S.ptick = now;
        if (abs(ax) > NUB_REST || abs(ay) > NUB_REST) {
            float speed = S.nub_speed * dt, ox = S.px, oy = S.py;
            SDL_Event ev;
            float dx = nub_speed_of(ax) * speed, dy = nub_speed_of(ay) * speed;
            if (S.sync_corner && !S.relative) {
                /* A game that moves a cursor of its own by the positions' differences (Unreal Tournament's
                 * menus) puts it where it likes when its menus come (the middle) and stops it at its edges,
                 * once a frame. At the first move: a jump far to the bottom-right, then, a frame later, to the
                 * top-left corner: its cursor at its corner whatever its start, ours there too, then in step.
                 * Without it the two stood apart and its cursor fell short of an edge (60% across in the
                 * character creation, again after a match) */
                static Uint32 t_far;
                SDL_Event j;
                SDL_zero(j);
                j.type = SDL_MOUSEMOTION; j.motion.state = S.mbuttons; j.motion.windowID = S.win ? SDL_GetWindowID(S.win) : 0;
                if (S.sync_corner == 1) {
                    j.motion.x = S.win_w * 4; j.motion.y = S.win_h * 4;
                    j.motion.xrel = j.motion.x - (int)S.px; j.motion.yrel = j.motion.y - (int)S.py;
                    S.px = (float)j.motion.x; S.py = (float)j.motion.y;
                    SDL_PushEvent(&j);
                    S.sync_corner = 2; t_far = now;
                    return;   /* (nothing else this time: the game takes the far one alone) */
                }
                if (now - t_far < 100) return;
                j.motion.x = 0; j.motion.y = 0;
                j.motion.xrel = -(int)S.px; j.motion.yrel = -(int)S.py;
                S.px = 0; S.py = 0; ox = 0; oy = 0;
                SDL_PushEvent(&j);
                S.sync_corner = 0;
            }
            if (S.relative) {   /* the game draws its own cursor from the motion alone: never stopped at an edge */
                S.rel_x += dx; S.rel_y += dy;
                S.rel_ex += dx; S.rel_ey += dy;
                /* the hidden pointer still moves, stopped at the edges, as SDL's does in relative mode:
                 * kept still at the window's centre (where it starts), every motion came "at the centre",
                 * which AssaultCube drops as the echo of its own warp there (skipmousemotion, in a
                 * window): its nub aimed nothing on the PSP's 480x272 */
                S.px = fminf(fmaxf(S.px + dx, 0), (float)(S.win_w - 1));
                S.py = fminf(fmaxf(S.py + dy, 0), (float)(S.win_h - 1));
            } else {
                S.px = fminf(fmaxf(S.px + dx, 0), (float)(S.win_w - 1));
                S.py = fminf(fmaxf(S.py + dy, 0), (float)(S.win_h - 1));
                if (S.own_pointer) {   /* a cursor of its own moved by the motion: the whole motion */
                    S.rel_x += dx; S.rel_y += dy;
                    S.rel_ex += dx; S.rel_ey += dy;
                } else {
                    S.rel_x += S.px - ox; S.rel_y += S.py - oy;
                    S.rel_ex += S.px - ox; S.rel_ey += S.py - oy;
                }
            }
            SDL_zero(ev);
            ev.type = SDL_MOUSEMOTION; ev.motion.x = (int)S.px; ev.motion.y = (int)S.py;
            ev.motion.xrel = (int)lroundf(S.rel_ex); ev.motion.yrel = (int)lroundf(S.rel_ey);
            S.rel_ex -= (float)ev.motion.xrel; S.rel_ey -= (float)ev.motion.yrel;   /* the fractions carried to the next */
            ev.motion.state = S.mbuttons;
            ev.motion.windowID = S.win ? SDL_GetWindowID(S.win) : 0;
            SDL_PushEvent(&ev);
        }
        if (S.mouse_left >= 0 && (pressed & BUTTON_BITS[S.mouse_left])) push_mouse_button(SDL_BUTTON_LEFT, 1);
        if (S.mouse_left >= 0 && (released & BUTTON_BITS[S.mouse_left])) push_mouse_button(SDL_BUTTON_LEFT, 0);
        if (S.mouse_right >= 0 && (pressed & BUTTON_BITS[S.mouse_right])) push_mouse_button(SDL_BUTTON_RIGHT, 1);
        if (S.mouse_right >= 0 && (released & BUTTON_BITS[S.mouse_right])) push_mouse_button(SDL_BUTTON_RIGHT, 0);
    }
}

/* ---- hooks: the input state games poll ------------------------------------------ */

HOOK(const Uint8 *, SDL_GetKeyboardState, (int *numkeys))
{
    int n = 0;
    (void)REAL(SDL_GetKeyboardState)(&n);
    if (numkeys) *numkeys = n < SDL_NUM_SCANCODES ? n : SDL_NUM_SCANCODES;
    kb_refresh();
    return kb_merged;   /* always ours, even before the layer is up: a game may keep the pointer */
}

/* Games that count joysticks once at start, before any window (Flare), would
 * otherwise find none: the controller is attached when input first arrives. */
HOOK(int, SDL_NumJoysticks, (void))
{
    static int inside;   /* attaching may count joysticks itself */
    if (!inside) { inside = 1; setup_vjoy(); inside = 0; }
    return REAL(SDL_NumJoysticks)();
}

HOOK(Uint32, SDL_GetMouseState, (int *x, int *y))
{
    static unsigned k;
    if (getenv("PSPEXP_LOG_EVENTS") && k++ % 200 == 0) logf_("call: SDL_GetMouseState (%d,%d)", (int)S.px, (int)S.py);
    if (!S.nub_mouse || !S.active) return REAL(SDL_GetMouseState)(x, y);
    if (x) *x = (int)S.px;
    if (y) *y = (int)S.py;
    return S.mbuttons;
}

HOOK(Uint32, SDL_GetRelativeMouseState, (int *x, int *y))
{
    int rx, ry;
    if (!S.nub_mouse || !S.active) return REAL(SDL_GetRelativeMouseState)(x, y);
    rx = (int)lroundf(S.rel_x); ry = (int)lroundf(S.rel_y);
    S.rel_x -= (float)rx; S.rel_y -= (float)ry;
    if (x) *x = rx;
    if (y) *y = ry;
    return S.mbuttons;
}

/* A game that recentres the mouse each frame and reads how far it moved:
 * the nub's pointer is put where the game puts the mouse. */
HOOK(void, SDL_WarpMouseInWindow, (SDL_Window *win, int x, int y))
{
    static unsigned k;
    if (getenv("PSPEXP_LOG_EVENTS") && k++ % 20 == 0) logf_("call: SDL_WarpMouseInWindow %d,%d", x, y);
    if (S.nub_mouse && S.active) { S.px = (float)x; S.py = (float)y; }
    REAL(SDL_WarpMouseInWindow)(win, x, y);
}

/* ---- hooks: window / renderer ------------------------------------------------ */

/* The display a program sees, on a headless board (SDL's dummy video driver,
 * whose display has no real size): the PSP's own screen, 480x272 at 60 Hz. A
 * program that sizes itself to the display (LTris, LBreakoutHD: a full-screen
 * window at the desktop's size) then lays out for the PSP, not for 0x0. */
#define PSP_W 480
#define PSP_H 272
static int headless_display(void)
{
    const char *d = SDL_GetCurrentVideoDriver();
    return d && !strcmp(d, "dummy");
}

/* the display reported as the PSP's screen; not to an SDL 1.2 program (sdl12-compat lists the video
 * modes up to the desktop's size: at 480x272, Powermanga's 640x400 was refused and it quit; its
 * window, whatever its size, is shrunk to the PSP like any other) */
static int psp_display(void)
{
#ifdef PSPEXP_SDL12_MODE
    return 0;
#else
    return headless_display();
#endif
}

static void psp_mode(SDL_DisplayMode *m)
{
    if (!m) return;
    m->w = PSP_W; m->h = PSP_H;
    if (!m->refresh_rate) m->refresh_rate = 60;
    if (!m->format) m->format = SDL_PIXELFORMAT_RGB888;
}

HOOK(int, SDL_GetDesktopDisplayMode, (int index, SDL_DisplayMode *mode))
{
    int r = REAL(SDL_GetDesktopDisplayMode)(index, mode);
    if (r == 0 && psp_display()) psp_mode(mode);
    return r;
}

HOOK(int, SDL_GetCurrentDisplayMode, (int index, SDL_DisplayMode *mode))
{
    int r = REAL(SDL_GetCurrentDisplayMode)(index, mode);
    if (r == 0 && psp_display()) psp_mode(mode);
    return r;
}

HOOK(int, SDL_GetDisplayBounds, (int index, SDL_Rect *rect))
{
    int r = REAL(SDL_GetDisplayBounds)(index, rect);
    if (r == 0 && rect && psp_display()) { rect->x = 0; rect->y = 0; rect->w = PSP_W; rect->h = PSP_H; }
    return r;
}

HOOK(int, SDL_GetDisplayUsableBounds, (int index, SDL_Rect *rect))
{
    int r = REAL(SDL_GetDisplayUsableBounds)(index, rect);
    if (r == 0 && rect && psp_display()) { rect->x = 0; rect->y = 0; rect->w = PSP_W; rect->h = PSP_H; }
    return r;
}

/* PSPEXP_WINDOW=WxH: the window this size and never full screen, whatever the program asks
 * (Celeste 64 goes full screen, which on a headless board is the offscreen driver's 1024x768: a
 * 16:9 picture letterboxed in it, and three times the pixels read back); 0 when not set */
static int forced_window(int *w, int *h)
{
    const char *e = getenv("PSPEXP_WINDOW");
    int fw, fh;
    if (!e || sscanf(e, "%dx%d", &fw, &fh) != 2 || fw < 16 || fh < 16) return 0;
    *w = fw; *h = fh;
    return 1;
}

HOOK(int, SDL_SetWindowFullscreen, (SDL_Window *win, Uint32 flags))
{
    int w, h;
    if (forced_window(&w, &h)) return 0;   /* stays a window of the forced size */
    return REAL(SDL_SetWindowFullscreen)(win, flags);
}

HOOK(SDL_Window *, SDL_CreateWindow, (const char *title, int x, int y, int w, int h, Uint32 flags))
{
    SDL_Window *win;
    if (forced_window(&w, &h)) flags &= ~(Uint32)SDL_WINDOW_FULLSCREEN_DESKTOP;   /* (FULLSCREEN is one of its bits) */
    if (headless_display() && ((flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP || w <= 1 || h <= 1)) {
        w = PSP_W; h = PSP_H;   /* "the desktop's size" or none at all: the PSP's screen */
        flags &= ~(Uint32)SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
#ifndef PSPEXP_STATIC_RENAME
    if (headless_display() && (flags & SDL_WINDOW_OPENGL)) {
        /* SDL's dummy driver has no OpenGL; ours needs no GPU (gl_pspexp.c) */
        flags &= ~(Uint32)SDL_WINDOW_OPENGL;
        win = REAL(SDL_CreateWindow)(title, x, y, w, h, flags);
        if (win) S.gl_window = win;
    } else
#endif
    win = REAL(SDL_CreateWindow)(title, x, y, w, h, flags);
    if (win && !S.win) {
        S.win = win; S.win_w = w; S.win_h = h; S.px = w / 2.0f; S.py = h / 2.0f;   /* the pointer starts centred */
        if (S.inited) update_scale();   /* a window after one destroyed (ScummVM's): its own size */
    }
    return win;
}

/* A minimum window size on a headless board: not applied. Taisei makes its window at the PSP's
 * 480x272 (the display's size) and then asks for at least 640x480, which SDL's offscreen driver
 * (the board's GPU drawing) applied by growing the window: Taisei drew in the 480x272 it had asked for, the bottom-left
 * corner of the 640x480 the layer read back */
HOOK(void, SDL_SetWindowMinimumSize, (SDL_Window *win, int w, int h))
{
    const char *d = SDL_GetCurrentVideoDriver();
    if (headless_display() || (d && !strcmp(d, "offscreen"))) { logf_("window minimum %dx%d not applied (a headless board)", w, h); return; }
    REAL(SDL_SetWindowMinimumSize)(win, w, h);
}

/* a window let go: the next one is the program's. ScummVM makes a 32x32 OpenGL window first to see
 * what OpenGL can do and destroys it; the layer kept that one, so the real window's size never came
 * (its games stayed black, or a corner of them blown up 8.5 times) */
HOOK(void, SDL_DestroyWindow, (SDL_Window *win))
{
    if (win && win == S.win) {
        if (S.ren && REAL(SDL_GetRenderer)(win) == S.ren) S.ren = NULL;
        S.win = NULL;
        logf_("window %p destroyed: the next one is taken", (void *)win);
    }
    if (win && win == S.gl_window) S.gl_window = NULL;
    REAL(SDL_DestroyWindow)(win);
}

HOOK(SDL_Renderer *, SDL_CreateRenderer, (SDL_Window *win, int index, Uint32 flags))
{
    SDL_Renderer *r = REAL(SDL_CreateRenderer)(win, index, flags);
    if (!r) {
        /* a headless board has only the software renderer, and the PSP draws
         * anyway: a program that insists on acceleration (NXEngine), vsync, or
         * a driver by its index (LTris, LBreakoutHD) gets that one */
        fprintf(stderr, "[sdl-pspexp] no renderer as asked (index %d, flags 0x%x: %s): the software one\n", index, (unsigned)flags, SDL_GetError());
        r = REAL(SDL_CreateRenderer)(win, -1, (flags & ~(SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)) | SDL_RENDERER_SOFTWARE);
    }
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
    int rc;
    if ((flags & SDL_WINDOW_OPENGL) && win && ren) {
        /* an OpenGL window from SDL's renderer, its context the renderer's (Emilia Pinball draws with GL
         * from there): a headless board has no GL renderer, so the window and a software renderer, and
         * the layer's OpenGL as if the program had made its context */
        if (!(*win = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, flags))) return -1;
        *ren = SDL_CreateRenderer(*win, -1, SDL_RENDERER_SOFTWARE);
        if (!SDL_GL_CreateContext(*win)) return -1;
        return 0;
    }
    rc = REAL(SDL_CreateWindowAndRenderer)(w, h, flags, win, ren);
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
        if (x && access == SDL_TEXTUREACCESS_STREAMING) {
            x->streaming = 1;
            /* a whole frame larger than 512 on a side (Project: Starfighter's
             * 800x600): the PSP shows it at under half size, so it is kept and
             * sent at half (1.2 MB a frame was 13 fps); PSPEXP_TEX_HALF=0: not */
            /* (asked for, PSPEXP_TEX_FIT first: VCMI's 1066x604 kept at half, 533x302, was shrunk again by
             * the PSP, a dark line where its texture's tiles met; fitted it is the PSP's 480x272, 1:1) */
            if (make_fit(x, w, h, NULL)) ;
            else if ((w > 512 || h > 512) && !(getenv("PSPEXP_TEX_HALF") && !strcmp(getenv("PSPEXP_TEX_HALF"), "0"))) {
                x->half = 1; x->w = (w + 1) / 2; x->h = (h + 1) / 2;
                logf_("streaming texture %dx%d kept at half size (%dx%d)", w, h, x->w, x->h);
            }
        }
        if (x) x->blend = SDL_BLENDMODE_NONE;   /* SDL's default for SDL_CreateTexture */
    }
    return t;
}

HOOK(SDL_Texture *, SDL_CreateTextureFromSurface, (SDL_Renderer *r, SDL_Surface *surf))
{
    SDL_Texture *t = REAL(SDL_CreateTextureFromSurface)(r, surf);
    if (t && r == S.ren && surf) {
        Tex *x = add_tex(t, surf->w, surf->h);
        if (x && make_fit(x, surf->w, surf->h, surf)) {   /* a whole frame (fheroes2's 640x480, updated since by SDL_UpdateTexture) */
            x->dirty = 1;
            SDL_GetTextureBlendMode(t, &x->blend);
        } else if (x) {
            SDL_Surface *conv = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ABGR8888, 0);
            if (conv) {
                int y;
                x->argb = 0; x->shadow16 = 0;
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
    Tex **pp = &S.textures, *y;
    for (y = S.textures; y; y = y->next) {   /* targets that still draw it: composed now, while it is here */
        int i;
        if (!y->is_target || y->sdl == t) continue;
        for (i = 0; i < y->nops; i++) if (y->ops[i].tex == t) { bake_target(y); break; }
    }
    while (*pp) {
        if ((*pp)->sdl == t) {
            Tex *x = *pp;
            *pp = x->next;
            tex_unhash(x);
            if (link_up()) release_pages(x);
            free(x->rgba);
            free(x->full);
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
    if (!host_render()) return 0;
    return REAL(SDL_UpdateTexture)(t, rect, pixels, pitch);
}

HOOK(int, SDL_LockTexture, (SDL_Texture *t, const SDL_Rect *rect, void **pixels, int *pitch))
{
    int rc = REAL(SDL_LockTexture)(t, rect, pixels, pitch);
    Tex *x = find_tex(t);
    if (rc == 0 && x) {
        x->locked = 1;
        x->lock_pixels = *pixels; x->lock_pitch = *pitch;
        if (rect) x->lock_rect = *rect;
        else { int tw = x->w, th = x->h; SDL_QueryTexture(t, NULL, NULL, &tw, &th); x->lock_rect = (SDL_Rect){ 0, 0, tw, th }; }   /* the real size (a half-size texture's is twice ours) */
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
        if (x && x->is_target) {
            x->nops = 0; record_op(NULL, NULL, NULL, 0, S.dr, S.dg, S.db, 255);
            if (x->nops) { x->ops[x->nops - 1].is_clear = 1; x->ops[x->nops - 1].clear_a = S.da; }
        }
        else if (x && x->baked) record_op(NULL, NULL, NULL, 0, S.dr, S.dg, S.db, S.da);   /* (its alpha: a clear to transparent) */
    } else if (r == S.ren && link_up() && !S.target && !S.skip_clear) {
        begin_frame();
        flush_quads();
        /* clear covers the whole PSP screen including the letterbox bars */
        S.clip_sent = 0;
        pspexp_set_clip(S.ctx, 0, 0, 480, 272);
        pspexp_fill_rect(S.ctx, 0, 0, 480, 272, draw_color5650(), 255);
    }
    if (!host_render()) return 0;
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
    if (!host_render()) return 0;
    return REAL(SDL_RenderCopy)(r, t, src, dst);
}

HOOK(int, SDL_RenderCopyEx, (SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_Rect *dst, double angle, const SDL_Point *center, SDL_RendererFlip flip))
{
    if (r == S.ren) {
        Tex *x = find_tex(t);
        SDL_Rect d = dst ? *dst : full_dst();
        if (x) mirror_copy(t, src, (float)d.x, (float)d.y, (float)d.w, (float)d.h, (int)flip);
    }
    if (!host_render()) return 0;
    return REAL(SDL_RenderCopyEx)(r, t, src, dst, angle, center, flip);
}

HOOK(int, SDL_RenderCopyF, (SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_FRect *dst))
{
    if (r == S.ren && dst) mirror_copy(t, src, dst->x, dst->y, dst->w, dst->h, 0);
    else if (r == S.ren) { SDL_Rect d = full_dst(); mirror_copy(t, src, (float)d.x, (float)d.y, (float)d.w, (float)d.h, 0); }
    if (!host_render()) return 0;
    return REAL(SDL_RenderCopyF)(r, t, src, dst);
}

HOOK(int, SDL_RenderCopyExF, (SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_FRect *dst, double angle, const SDL_FPoint *center, SDL_RendererFlip flip))
{
    if (r == S.ren && dst) mirror_copy(t, src, dst->x, dst->y, dst->w, dst->h, (int)flip);
    if (!host_render()) return 0;
    return REAL(SDL_RenderCopyExF)(r, t, src, dst, angle, center, flip);
}

HOOK(int, SDL_RenderFillRect, (SDL_Renderer *r, const SDL_Rect *rect))
{
    if (r == S.ren) {
        if (rect) mirror_fill((float)rect->x, (float)rect->y, (float)rect->w, (float)rect->h);
        else { SDL_Rect d = full_dst(); mirror_fill((float)d.x, (float)d.y, (float)d.w, (float)d.h); }
    }
    if (!host_render()) return 0;
    return REAL(SDL_RenderFillRect)(r, rect);
}

HOOK(int, SDL_RenderFillRects, (SDL_Renderer *r, const SDL_Rect *rects, int count))
{
    int i;
    if (r == S.ren && rects) for (i = 0; i < count; i++) mirror_fill((float)rects[i].x, (float)rects[i].y, (float)rects[i].w, (float)rects[i].h);
    if (!host_render()) return 0;
    return REAL(SDL_RenderFillRects)(r, rects, count);
}

HOOK(int, SDL_RenderFillRectF, (SDL_Renderer *r, const SDL_FRect *rect))
{
    if (r == S.ren && rect) mirror_fill(rect->x, rect->y, rect->w, rect->h);
    if (!host_render()) return 0;
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
    if (!host_render()) return 0;
    return REAL(SDL_RenderDrawRect)(r, rect);
}

HOOK(int, SDL_RenderDrawLine, (SDL_Renderer *r, int x1, int y1, int x2, int y2))
{
    if (r == S.ren) mirror_line((float)x1, (float)y1, (float)x2, (float)y2);
    if (!host_render()) return 0;
    return REAL(SDL_RenderDrawLine)(r, x1, y1, x2, y2);
}

HOOK(int, SDL_RenderDrawLines, (SDL_Renderer *r, const SDL_Point *points, int count))
{
    int i;
    if (r == S.ren && points) for (i = 1; i < count; i++) mirror_line((float)points[i - 1].x, (float)points[i - 1].y, (float)points[i].x, (float)points[i].y);
    if (!host_render()) return 0;
    return REAL(SDL_RenderDrawLines)(r, points, count);
}

HOOK(int, SDL_RenderDrawPoint, (SDL_Renderer *r, int x, int y))
{
    if (r == S.ren) mirror_fill((float)x, (float)y, 1, 1);
    if (!host_render()) return 0;
    return REAL(SDL_RenderDrawPoint)(r, x, y);
}

HOOK(int, SDL_RenderDrawPoints, (SDL_Renderer *r, const SDL_Point *points, int count))
{
    int i;
    if (r == S.ren && points) for (i = 0; i < count; i++) mirror_fill((float)points[i].x, (float)points[i].y, 1, 1);
    if (!host_render()) return 0;
    return REAL(SDL_RenderDrawPoints)(r, points, count);
}

/* Input and audio must not wait for the next present: a menu that redraws
 * once a second would lose every press and let the audio pile up. Games
 * poll events every loop iteration, so the link is serviced from there. */
static void script_tick(void);
/* The link is one conversation: a program that draws on a thread of its own (OpenRA's render
 * thread swaps while its main thread polls events) had the two reading it at once, the frame's
 * present reply taken by the event poll and the swap waiting for it for ever. One lock, recursive
 * (a frame's end pumps the link too), around the event side and the frame side. */
static pthread_mutex_t link_mutex;
static pthread_once_t link_once = PTHREAD_ONCE_INIT;
static void link_mutex_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&link_mutex, &a);
}
static void link_lock(void) { pthread_once(&link_once, link_mutex_init); pthread_mutex_lock(&link_mutex); }
static void link_unlock(void) { pthread_mutex_unlock(&link_mutex); }

/* The pointer moved while the game drew nothing (fheroes2 draws 4-8 frames a second, its pointer the
 * system's): its last frame again, the pointer where it is now, at most every 16 ms and only when
 * no frame is on its way, so the game's own frames never wait. The frame's draws only: its textures
 * are on the PSP already (a few hundred bytes). The pointer had moved in jumps (the user). */
static void replay_pointer(void)
{
    Uint32 now = SDL_GetTicks();
    static Uint32 gap;   /* PSPEXP_POINTER_FPS: at most that many such frames a second (60; the PSP's LCD smears a small
                          * bright cursor moved often, so fewer and larger steps can look cleaner) */
    if (!gap) { int f = getenv("PSPEXP_POINTER_FPS") ? atoi(getenv("PSPEXP_POINTER_FPS")) : 60; gap = f > 0 && f <= 60 ? 1000u / (Uint32)f : 16u; }
    if (S.frame_open || !pointer_shown() || (S.px == S.drawn_px && S.py == S.drawn_py) || now - S.last_present < gap) return;
    if (!pspexp_should_render(S.ctx) || pspexp_replay_frame(S.ctx) < 0) return;
    S.clip_sent = 0;
    draw_pointer();
    S.drawn_px = S.px; S.drawn_py = S.py; S.last_present = now;
    pspexp_present(S.ctx);
    S.replays++;
}

static void service_link(void)
{
    static Uint32 last;
    Uint32 now = SDL_GetTicks();
    if (!S.active || !link_up() || now == last) return;
    link_lock();
    last = now;
    pspexp_pump(S.ctx, 0);
    feed_input();
    audio_drain();
    script_tick();   /* a game waiting for an event (Pinball's menu) gets its scripted keys too */
    replay_pointer();
    link_unlock();
}

/* PSPEXP_NUB=mouse: the PSP shows no host cursor, so the pointer is drawn
 * here on top of the frame (an arrow, black edge, white inside), while the
 * program has not hidden its cursor */
/* SDL's relative mouse mode (Quake 3's engine: the motion only, its own cursor): the dummy driver of a
 * headless board refuses it; the layer keeps it instead, draws no pointer and stops nothing at an edge */
HOOK(int, SDL_SetRelativeMouseMode, (SDL_bool enabled))
{
    if (getenv("PSPEXP_LOG_EVENTS")) logf_("call: SDL_SetRelativeMouseMode %d", (int)enabled);
    if (S.own_pointer && enabled != SDL_TRUE) S.sync_corner = 1;   /* (see feed_input: the cursor's corner) */
    if (S.nub_mouse && S.active) { S.relative = enabled == SDL_TRUE; (void)REAL(SDL_SetRelativeMouseMode)(enabled); return 0; }
    return REAL(SDL_SetRelativeMouseMode)(enabled);
}
HOOK(SDL_bool, SDL_GetRelativeMouseMode, (void))
{
    if (S.nub_mouse && S.active) return S.relative ? SDL_TRUE : SDL_FALSE;
    return REAL(SDL_GetRelativeMouseMode)();
}

HOOK(int, SDL_ShowCursor, (int toggle))
{
    if (toggle == SDL_ENABLE) S.cursor_hidden = 0;
    else if (toggle == SDL_DISABLE) S.cursor_hidden = 1;
    return REAL(SDL_ShowCursor)(toggle);
}

/* Some programs hide the pointer with a transparent cursor instead of
 * SDL_ShowCursor (Stratagus: a 1x1 one, so SDL keeps absolute coordinates
 * in fullscreen) and draw their own; the layer's pointer was then drawn
 * beside theirs. A cursor made with no visible pixel is remembered, and the
 * pointer is not drawn while it is set */
HOOK(SDL_Cursor *, SDL_CreateCursor, (const Uint8 *data, const Uint8 *mask, int w, int h, int hot_x, int hot_y))
{
    SDL_Cursor *c = REAL(SDL_CreateCursor)(data, mask, w, h, hot_x, hot_y);
    int i, n = (w + 7) / 8 * h, seen = 0;
    for (i = 0; i < n && data && mask; i++) seen |= data[i] | mask[i];
    if (c && data && mask && !seen)
        for (i = 0; i < 4; i++) if (!S.blank_cursor[i]) { S.blank_cursor[i] = c; break; }
    return c;
}
/* A game's own cursor pictures (SDL_CreateColorCursor: fheroes2's, which say what a click does: a
 * horse to move, a sword to attack), drawn where the layer's arrow would be, at the PSP's pixels
 * (larger than the shrunk frame: readable), its hot spot on the pointer; sent to the PSP once. */
typedef struct GameCursor { SDL_Cursor *c; int w, h, hx, hy, texw, texh; uint16_t *px; uint16_t tex; } GameCursor;
static GameCursor g_cursors[96], *g_cur;
static void cursors_forget(void)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_cursors) / sizeof(g_cursors[0])); i++) g_cursors[i].tex = 0;
}
static GameCursor *game_cursor(SDL_Cursor *c)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_cursors) / sizeof(g_cursors[0])); i++) if (c && g_cursors[i].c == c) return &g_cursors[i];
    return NULL;
}
HOOK(SDL_Cursor *, SDL_CreateColorCursor, (SDL_Surface *surf, int hot_x, int hot_y))
{
    SDL_Cursor *c = REAL(SDL_CreateColorCursor)(surf, hot_x, hot_y);
    GameCursor *g = game_cursor(NULL);
    int i, y, x;
    for (i = 0; !g && i < (int)(sizeof(g_cursors) / sizeof(g_cursors[0])); i++) if (!g_cursors[i].c) g = &g_cursors[i];
    if (c && g && surf && surf->w > 0 && surf->h > 0 && surf->w <= 64 && surf->h <= 64) {
        SDL_Surface *cv = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ABGR8888, 0);
        int tw = 8, th = 8;
        while (tw < surf->w) tw *= 2;
        while (th < surf->h) th *= 2;
        if (cv && (g->px = (uint16_t *)calloc((size_t)tw * th, 2))) {
            for (y = 0; y < surf->h; y++)
                for (x = 0; x < surf->w; x++) {
                    uint32_t v = ((const uint32_t *)(const void *)((const Uint8 *)cv->pixels + (size_t)y * cv->pitch))[x];
                    g->px[(size_t)y * tw + x] = (uint16_t)(((v >> 4) & 0xF) | (((v >> 12) & 0xF) << 4) | (((v >> 20) & 0xF) << 8) | (((v >> 28) & 0xF) << 12));
                }
            g->c = c; g->w = surf->w; g->h = surf->h; g->hx = hot_x; g->hy = hot_y; g->texw = tw; g->texh = th; g->tex = 0;
        }
        if (cv) SDL_FreeSurface(cv);
    }
    return c;
}
HOOK(void, SDL_SetCursor, (SDL_Cursor *c))
{
    int i;
    if (c) for (S.cursor_blank = 0, i = 0; i < 4; i++) if (S.blank_cursor[i] == c) S.cursor_blank = 1;
    if (c) g_cur = game_cursor(c);
    REAL(SDL_SetCursor)(c);
}
HOOK(void, SDL_FreeCursor, (SDL_Cursor *c))
{
    int i;
    GameCursor *g = game_cursor(c);
    for (i = 0; i < 4; i++) if (c && S.blank_cursor[i] == c) S.blank_cursor[i] = NULL;
    if (g) {
        if (g->tex && link_up()) pspexp_release(S.ctx, g->tex);
        free(g->px);
        if (g_cur == g) g_cur = NULL;
        memset(g, 0, sizeof(*g));
    }
    REAL(SDL_FreeCursor)(c);
}

static void draw_pointer(void)
{
    int r, x, y;
    const uint16_t black = pspexp_color_5650(0, 0, 0), white = pspexp_color_5650(255, 255, 255);
    float plx, ply;
    pointer_logical(&plx, &ply);
    x = S.ox + (int)lroundf(plx * S.sx);   /* (fitted or magnified: the frame's own placing) */
    y = S.oy + (int)lroundf(ply * S.sy);
    if (g_cur && g_cur->px) {   /* the game's own cursor picture */
        PspExpQuadExRec q;
        if (!g_cur->tex) {
            PspExpTextureDesc d;
            PspExpFrame f;
            memset(&d, 0, sizeof(d));
            d.width = (uint16_t)g_cur->texw; d.height = (uint16_t)g_cur->texh; d.format = PSPEXP_TEX_4444;
            d.layout = PSPEXP_LAYOUT_LINEAR; d.priority = PSPEXP_PRIORITY_NORMAL;
            f.u0 = 0; f.v0 = 0; f.w = (uint16_t)g_cur->texw; f.h = (uint16_t)g_cur->texh;
            g_cur->tex = alloc_handle();
            if (pspexp_upload_texture(S.ctx, g_cur->tex, &d, g_cur->px, (uint32_t)((size_t)g_cur->texw * g_cur->texh * 2), &f, 1) < 0) { g_cur->tex = 0; goto arrow; }
        }
        memset(&q, 0, sizeof(q));
        q.x = (int16_t)(x - g_cur->hx); q.y = (int16_t)(y - g_cur->hy); q.w = (int16_t)g_cur->w; q.h = (int16_t)g_cur->h;
        q.u0 = 0; q.v0 = 0; q.u1 = (uint16_t)g_cur->w; q.v1 = (uint16_t)g_cur->h; q.color = 0xFFFF;
        pspexp_set_clip(S.ctx, 0, 0, 480, 272);
        S.clip_sent = 0;
        pspexp_draw_quad_ex_batch(S.ctx, g_cur->tex, &q, 1, PSPEXP_PALETTE_DEFAULT, PSPEXP_VTX_SPRITE_2D_4444);
        return;
    }
arrow:
    pspexp_set_clip(S.ctx, 0, 0, 480, 272);
    S.clip_sent = 0;   /* the program's clip again next frame */
    for (r = 0; r < 12; r++) {
        int len = r < 9 ? r * 2 / 3 + 1 : (11 - r) * 2 + 1;   /* widening, then the notch */
        pspexp_fill_rect(S.ctx, (int16_t)x, (int16_t)(y + r), (int16_t)(len + 1), 1, black, 255);
        if (len > 1 && r < 11) pspexp_fill_rect(S.ctx, (int16_t)(x + 1), (int16_t)(y + r), (int16_t)(len - 1), 1, white, 255);
    }
}

/* PSPEXP_SHOT_TRIGGER=FILE: when FILE appears, the PSP's screen is saved and
 * copied back over this link to FILE-N.bmp (N counting from 1): looking at a
 * game while someone plays it */
static void shot_trigger(void)
{
    static Uint32 last, pending_at;
    static int pending = -1, n;
    const char *trig = getenv("PSPEXP_SHOT_TRIGGER");
    Uint32 now = SDL_GetTicks();
    if (!trig || !link_up() || !pspexp_fs_supported(S.ctx)) return;
    if (pending >= 0 && now - pending_at > 1500) {   /* written by now: fetched */
        char src[80], dst[512];
        PspExpFsEntry st;
        snprintf(src, sizeof(src), "ms0:/PSP/GAME/PSPExp/pspexp-shot-%d.bmp", pending);
        snprintf(dst, sizeof(dst), "%s-%d.bmp", trig, pending - 900);
        if (pspexp_fs_stat(S.ctx, src, &st) >= 0) {
            FILE *f = fopen(dst, "wb");
            static unsigned char buf[PSPEXP_FS_CHUNK];
            uint64_t off = 0;
            int got;
            while (f && off < ((uint64_t)st.size_hi << 32 | st.size_lo) && (got = pspexp_fs_read(S.ctx, src, off, buf, sizeof(buf))) > 0) { fwrite(buf, 1, (size_t)got, f); off += (uint64_t)got; }
            if (f) fclose(f);
            pspexp_fs_remove(S.ctx, src);
            logf_("screenshot: %s", dst);
        }
        pending = -1;
    }
    if (pending >= 0 || now - last < 300) return;
    last = now;
    {
        FILE *f = fopen(trig, "rb");
        if (!f) return;
        fclose(f);
        remove(trig);
        pending = 900 + ++n;   /* its own numbers on the stick */
        pending_at = now;
        pspexp_request_screenshot(S.ctx, (uint16_t)pending);
    }
}

/* PSPEXP_SCRIPT="ms:key,ms:+key,ms:-key,...": keys pressed at those times
 * (ms from the first frame; +: held down, -: let go, else a tap): testing a
 * game past its menus with nobody at the PSP */
static void script_tick(void)
{
    static char *script;
    static const char *next;
    static Uint32 t0, tap_up_at;
    static SDL_Keycode tapped;
    const char *env = getenv("PSPEXP_SCRIPT");
    Uint32 now = SDL_GetTicks();
    if (!env) return;
    if (!script) { script = strdup(env); next = script; t0 = now; }
    static uint32_t pad_tapped;
    static Uint32 pad_up_at;
    if (tapped && now >= tap_up_at) { push_key(tapped, 0); tapped = 0; }
    if (pad_tapped && now >= pad_up_at) { script_pad &= ~pad_tapped; pad_tapped = 0; }
    while (next && *next) {
        char name[32];
        const char *start = next;
        long at = strtol(next, (char **)&next, 10);
        int mode = 0, k = 0;
        SDL_Keycode key;
        if (now - t0 < (Uint32)at) { next = start; return; }   /* not yet */
        if (*next == ':') next++;
        if (*next == '+' || *next == '-') mode = *next++;
        while (*next && *next != ',' && k < 31) name[k++] = *next++;
        name[k] = 0;
        if (*next == ',') next++;
        {   /* the pointer: "@XxY" moves it there (window pixels); "click" taps the left button,
             * "+click" / "-click" hold and let go (menus that want a mouse, tested unattended) */
            int mx, my;
            if ((name[0] == '@' || name[0] == '~') && sscanf(name + 1, "%dx%d", &mx, &my) == 2) {
                /* "~DXxDY": a move by that much (games that make their own cursor from the motion,
                 * Lugaru's starting at its corner) */
                SDL_Event ev;
                int rx = name[0] == '~' ? mx : mx - (int)S.px, ry = name[0] == '~' ? my : my - (int)S.py;
                if (name[0] == '~') { mx = (int)S.px + rx; my = (int)S.py + ry; }
                SDL_zero(ev);
                ev.type = SDL_MOUSEMOTION; ev.motion.x = mx; ev.motion.y = my;
                ev.motion.xrel = rx; ev.motion.yrel = ry;
                ev.motion.state = S.mbuttons; ev.motion.windowID = S.win ? SDL_GetWindowID(S.win) : 0;
                S.rel_x += (float)rx; S.rel_y += (float)ry;   /* for games that poll the motion */
                S.px = (float)mx; S.py = (float)my;
                SDL_PushEvent(&ev);
                logf_("script: pointer to %d,%d at %ld ms", mx, my, at);
                continue;
            }
            if (!strcmp(name, "click")) {
                push_mouse_button(SDL_BUTTON_LEFT, mode != '-');
                if (!mode) push_mouse_button(SDL_BUTTON_LEFT, 0);
                logf_("script: %cclick at %ld ms", mode ? mode : ' ', at);
                continue;
            }
        }
        if (!strncmp(name, "nub:", 4)) {   /* "nub:100x0": the nub held over that far (right, down positive), "nub:0x0" lets go */
            int nx = 0, ny = 0;
            if (sscanf(name + 4, "%dx%d", &nx, &ny) == 2) {
                script_nub_x = nx < -127 ? -127 : nx > 127 ? 127 : nx;
                script_nub_y = ny < -127 ? -127 : ny > 127 ? 127 : ny;
                script_nub = nx || ny;
                logf_("script: %s at %ld ms", name, at);
            }
            continue;
        }
        if (!strncmp(name, "pad:", 4)) {   /* "pad:cross": a PSP button, through the same path as the PSP's own
                                            * (the virtual controller and its keys): games that tell a pad from a keyboard */
            int b;
            for (b = 0; b < B_COUNT; b++) if (!strcmp(name + 4, BUTTON_NAMES[b])) break;
            if (b < B_COUNT) {
                if (getenv("PSPEXP_LOG_EVENTS")) logf_("script: virtual controller %s", S.vjoy_handle ? (SDL_JoystickGetAttached(S.vjoy_handle) ? "attached and open" : "handle no longer attached") : "not open");
                if (mode == '+') script_pad |= BUTTON_BITS[b];
                else if (mode == '-') script_pad &= ~BUTTON_BITS[b];
                else { script_pad |= BUTTON_BITS[b]; pad_tapped |= BUTTON_BITS[b]; pad_up_at = now + 150; }
                logf_("script: %c%s at %ld ms", mode ? mode : ' ', name, at);
            }
            continue;
        }
        key = key_from_name(name);
        if (mode == '+') push_key(key, 1);
        else if (mode == '-') push_key(key, 0);
        else { if (tapped) push_key(tapped, 0); push_key(key, 1); tapped = key; tap_up_at = now + 120; }
        logf_("script: %c%s at %ld ms", mode ? mode : ' ', name, at);
    }
}

static void end_frame_locked(void);
static void end_frame(void)
{
    if (!S.active) return;
    link_lock();
    end_frame_locked();
    link_unlock();
}
static void end_frame_locked(void)
{
    if (S.win) {   /* programs resize their window after creating it (scalers, settings) */
        int w = 0, h = 0;
        SDL_GetWindowSize(S.win, &w, &h);
        /* full screen on a headless board: the PSP's screen, not the dummy driver's 1024x768 (Trigger
         * Rally goes full screen at the desktop's size, 480x272, and drew into a third of it) */
        if (headless_display() && (REAL(SDL_GetWindowFlags)(S.win) & SDL_WINDOW_FULLSCREEN)) { w = PSP_W; h = PSP_H; }
        if (w > 0 && h > 0 && (w != S.win_w || h != S.win_h)) { S.win_w = w; S.win_h = h; update_scale(); }
    }
    if (!link_up()) { S.frame_open = 0; S.nquads = 0; try_reconnect(); return; }
    begin_frame();
    flush_quads();
    pspexp_frame_mark(S.ctx);   /* (the frame kept without the pointer: replay_pointer) */
    if (pointer_shown()) { draw_pointer(); S.drawn_px = S.px; S.drawn_py = S.py; }
    S.last_present = SDL_GetTicks();
    audio_drain();
    {
        uint64_t w0, w1, f0 = wall_ns(), c0 = cpu_ns();
        if (S.prof_last_end) { S.prof_game += f0 - S.prof_last_end; S.prof_game_cpu += c0 - S.prof_last_end_cpu; }
        /* the frame's uploads out in one transfer, before waiting for the
         * PSP's last frame: they travel while it finishes */
        pspexp_batch_flush(S.ctx);
        w0 = wall_ns();
        S.prof_flush += w0 - f0;
        S.prof_flush_cpu += cpu_ns() - c0;
        pspexp_wait_present(S.ctx, 40);
        w1 = wall_ns();
        pspexp_present(S.ctx);
        S.prof_wait += w1 - w0;
        S.prof_send += wall_ns() - w1;
        S.prof_last_end = wall_ns(); S.prof_last_end_cpu = cpu_ns();
    }
    S.frame_open = 0;
    pspexp_pump(S.ctx, 0);
    feed_input();
    shot_trigger();
    script_tick();
    S.frames++;
    if (!S.total_frames) S.first_frame = SDL_GetPerformanceCounter();
    S.total_frames++;
    if (S.shot_every && S.total_frames % S.shot_every == 0) pspexp_request_screenshot(S.ctx, (uint16_t)S.shot_index++);
    {
        Uint32 now = SDL_GetTicks();
        if (now - S.last_report >= 2000) {
            const PspExpStats *st = pspexp_stats(S.ctx);
            const PspExpAudioStatus *au = pspexp_audio_status(S.ctx);
            double fr = S.frames ? (double)S.frames : 1.0;
            logf_("%.1f fps  %u B/frame  %u batches  %u sprites  psp cpu %u us  ge %u us  uploads %u  errors %u  audio buffered %u underruns %u",
                  S.frames * 1000.0 / (double)(now - S.last_report), st->last_frame_bytes, st->last_frame_batches,
                  st->last_frame_sprites, st->last_execute_us, st->last_ge_us, S.uploads,
                  st->resource_errors + st->protocol_errors, au->buffered_frames, au->underruns);
            if (getenv("PSPEXP_PROFILE"))
                logf_("profile: program and its GL %.2f (cpu %.2f)  compare %.2f  convert %.2f  write %.2f  flush %.2f (cpu %.2f)  wait for the PSP %.2f  send %.2f ms/frame",
                      (double)S.prof_game / 1e6 / fr, (double)S.prof_game_cpu / 1e6 / fr,
                      (double)S.prof_shadow / 1e6 / fr, (double)S.prof_convert / 1e6 / fr, (double)S.prof_write / 1e6 / fr,
                      (double)S.prof_flush / 1e6 / fr, (double)S.prof_flush_cpu / 1e6 / fr,
                      (double)S.prof_wait / 1e6 / fr, (double)S.prof_send / 1e6 / fr);
            S.prof_shadow = S.prof_convert = S.prof_write = S.prof_wait = S.prof_send = S.prof_flush = S.prof_flush_cpu = 0;
            S.prof_game = S.prof_game_cpu = 0;
            logf_("link %.1f KB/frame  pages sent %u (clut8 %u) kept %u  texture work %.2f ms/frame (CPU)%s",
                  (double)(st->bytes_sent - S.rep_bytes) / 1024.0 / fr, S.pages_sent, S.pages_t8, S.pages_kept,
                  (double)S.up_ticks / 1e6 / fr, S.replays ? "  +pointer frames" : "");
            if (S.replays) { logf_("pointer frames %u (the last frame again, the pointer moved)", S.replays); S.replays = 0; }
            S.tot_pages_sent += S.pages_sent; S.tot_pages_t8 += S.pages_t8; S.tot_pages_kept += S.pages_kept; S.tot_up_ticks += S.up_ticks;
            S.rep_bytes = st->bytes_sent; S.pages_sent = S.pages_t8 = S.pages_kept = 0; S.up_ticks = 0;
            S.last_report = now; S.frames = 0; S.uploads = 0;
        }
    }
}

/* PSPEXP_COMPOSE=1, for 2D games that draw hundreds of small pictures a frame: each was mirrored to
 * the PSP as a draw of its own (Ri-li 778 a frame, 6 fps on the PSP-1000; LBreakout HD 1500, 33 fps:
 * the PSP's CPU 9 ms a frame on them). Composed, SDL's software renderer draws the frame on the board
 * into the window's surface and it goes to the PSP as one picture, its changed pages only. */
static void stream_surface(SDL_Window *win);
HOOK(void, SDL_RenderPresent, (SDL_Renderer *r))
{
    if (host_render()) REAL(SDL_RenderPresent)(r);
    if (r == S.ren) {
        if (S.compose && S.win && link_up()) stream_surface(S.win);
        else end_frame();
    }
}

/* ---- hooks: OpenGL (gl_pspexp.c draws; here the context and the frames) ---------- */

/* a whole frame of pixels for stream_frame (below) */
typedef struct { int w, h; Uint32 format; void *pixels; int pitch; } Frame;
static void stream_frame(SDL_Window *win, const Frame *surf);

#ifdef PSPEXP_HOST_GL
/* The host's own GPU draws (libsdl-pspexp-hostgl, PSPEXP_HOST_GL): no OpenGL of ours, the program's GL
 * is the real one; at each swap the finished frame is read back and goes to the PSP as a picture, as a
 * window surface does (stream_frame: shrunk to the PSP's screen, the pages that changed). For programs
 * too heavy for the GE (OpenMW: 400,000 vertices a frame); the PSP shows, plays and takes the input */
static void (*hg_ReadPixels)(int, int, int, int, unsigned, unsigned, void *);
static void (*hg_ReadBuffer)(unsigned);
static void (*hg_PixelStorei)(unsigned, int);
static void (*hg_BindFramebuffer)(unsigned, unsigned);
static void (*hg_GetIntegerv)(unsigned, int *);
/* reading back without waiting (pixel buffer objects, GL 2.1 / ES 3): each frame goes into a buffer the
 * GPU fills on its own, and the one before it is sent: a frame later on the PSP, but the program's next
 * frame is not held up while the GPU finishes this one (SuperTuxKart on the Pi's V3D) */
static void (*hg_GenBuffers)(int, unsigned *);
static void (*hg_BindBuffer)(unsigned, unsigned);
static void (*hg_BufferData)(unsigned, long, const void *, unsigned);
static void *(*hg_MapBufferRange)(unsigned, long, long, unsigned);
static unsigned char (*hg_UnmapBuffer)(unsigned);
static unsigned hg_pbo[2];
static int hg_pbo_w, hg_pbo_h, hg_pbo_n, hg_pbo_cur, hg_pbo_off;

HOOK(SDL_GLContext, SDL_GL_CreateContext, (SDL_Window *win))
{
    SDL_GLContext c = REAL(SDL_GL_CreateContext)(win);
    if (c) {
        if (!S.win) S.win = win;
        hg_ReadPixels = (void (*)(int, int, int, int, unsigned, unsigned, void *))SDL_GL_GetProcAddress("glReadPixels");
        hg_ReadBuffer = (void (*)(unsigned))SDL_GL_GetProcAddress("glReadBuffer");
        hg_PixelStorei = (void (*)(unsigned, int))SDL_GL_GetProcAddress("glPixelStorei");
        hg_BindFramebuffer = (void (*)(unsigned, unsigned))SDL_GL_GetProcAddress("glBindFramebuffer");
        hg_GetIntegerv = (void (*)(unsigned, int *))SDL_GL_GetProcAddress("glGetIntegerv");
        hg_GenBuffers = (void (*)(int, unsigned *))SDL_GL_GetProcAddress("glGenBuffers");
        hg_BindBuffer = (void (*)(unsigned, unsigned))SDL_GL_GetProcAddress("glBindBuffer");
        hg_BufferData = (void (*)(unsigned, long, const void *, unsigned))SDL_GL_GetProcAddress("glBufferData");
        hg_MapBufferRange = (void *(*)(unsigned, long, long, unsigned))SDL_GL_GetProcAddress("glMapBufferRange");
        hg_UnmapBuffer = (unsigned char (*)(unsigned))SDL_GL_GetProcAddress("glUnmapBuffer");
        hg_pbo_off = getenv("PSPEXP_HOSTGL_PBO") && !strcmp(getenv("PSPEXP_HOSTGL_PBO"), "0");
        hg_pbo_n = 0; hg_pbo_w = hg_pbo_h = 0;
        connect_once();
        logf_("host GL: the frames drawn here go to the PSP as pictures");
    }
    return c;
}

#ifndef __APPLE__
/* GLEW built for GLX (Debian's) on SDL's offscreen driver: glewInit loads the GL functions, then
 * fails on its GLX part without an X display (GLEW_ERROR_NO_GLX_DISPLAY, 4, "Unknown error"; OpenClonk
 * then had no context). Under offscreen that one failure is success (the GL part is done; nothing
 * of GLX is used), and GLX's function lookups are SDL's, which are EGL's here */
static int hg_offscreen(void) { const char *d = SDL_GetCurrentVideoDriver(); return d && !strcmp(d, "offscreen"); }
__attribute__((visibility("default"))) unsigned glewInit(void)
{
    unsigned (*real)(void) = (unsigned (*)(void))(uintptr_t)dlsym(RTLD_NEXT, "glewInit");
    unsigned r = real ? real() : 1;
    return r == 4 && hg_offscreen() ? 0 : r;
}
__attribute__((visibility("default"))) void *glXGetProcAddressARB(const unsigned char *name)
{
    if (hg_offscreen()) return SDL_GL_GetProcAddress((const char *)name);
    {
        void *(*real)(const unsigned char *) = (void *(*)(const unsigned char *))(uintptr_t)dlsym(RTLD_NEXT, "glXGetProcAddressARB");
        return real ? real(name) : NULL;
    }
}
__attribute__((visibility("default"))) void *glXGetProcAddress(const unsigned char *name) { return glXGetProcAddressARB(name); }
#endif

/* SDL's offscreen driver (a board with no display: the GPU through EGL alone) has no window manager
 * and so no WM info; SuperTuxKart's Irrlicht asks for it and stops without it. Answered here: none in
 * particular, with success */
HOOK(SDL_bool, SDL_GetWindowWMInfo, (SDL_Window *win, SDL_SysWMinfo *info))
{
    if (REAL(SDL_GetWindowWMInfo)(win, info)) return SDL_TRUE;
    if (!info) return SDL_FALSE;
    {
        SDL_version v = info->version;
        memset(info, 0, sizeof(*info));
        info->version = v;
        info->subsystem = SDL_SYSWM_UNKNOWN;
    }
    return SDL_TRUE;
}

HOOK(void, SDL_GL_SwapWindow, (SDL_Window *win))
{
    static uint8_t *buf;
    static size_t cap;
    int w = 0, h = 0;
    static int drop = -1;
    if (drop < 0) drop = getenv("PSPEXP_FRAME_DROP") && atoi(getenv("PSPEXP_FRAME_DROP")) > 0;
    /* PSPEXP_FRAME_DROP=1: while the PSP still has the frame before, this one is not sent (nor read
     * back) rather than waited for: a program that keeps time by its sound (Mupen64Plus, its audio
     * plugin pacing the emulation) then runs at its own speed. Waiting held the N64 to the PSP's
     * 50-57 fps, the emulation 5-10% slow and its sound short: a crackle. */
    if (drop && S.active && link_up()) {
        link_lock();
        pspexp_pump(S.ctx, 0);
        if (!pspexp_should_render(S.ctx)) { S.dropped++; link_unlock(); REAL(SDL_GL_SwapWindow)(win); return; }
        link_unlock();
    }
    SDL_GL_GetDrawableSize(win, &w, &h);
    {   /* why a frame is not read back, once each (Celeste 64 on the PSP once stopped sending its
         * picture after 5 s, at 60 fps, and nothing said why) */
        static int said_size, said_link;
        if ((w <= 0 || h <= 0) && !said_size) { said_size = 1; logf_("host GL: drawable %dx%d: frames not read back", w, h); }
        if (S.active && !link_up() && !said_link) { said_link = 1; logf_("host GL: the link is down: frames not sent"); }
        if (link_up()) said_link = 0;
    }
    if (hg_ReadPixels && w > 0 && h > 0) {
        size_t row = (size_t)w * 4, n = row * (size_t)h;
        int fb = 0, y;
        if (n > cap) { uint8_t *b = (uint8_t *)realloc(buf, n); if (b) { buf = b; cap = n; } }
        if (n <= cap) {
            if (hg_GetIntegerv) hg_GetIntegerv(0x8CAA, &fb);                  /* GL_READ_FRAMEBUFFER_BINDING */
            if (hg_BindFramebuffer) hg_BindFramebuffer(0x8CA8, 0);            /* the window's own framebuffer */
            if (hg_ReadBuffer) hg_ReadBuffer(0x0405);                         /* GL_BACK: the frame just drawn */
            if (hg_PixelStorei) hg_PixelStorei(0x0D05, 4);                    /* GL_PACK_ALIGNMENT */
            if (!hg_pbo_off && hg_GenBuffers && hg_BindBuffer && hg_BufferData && hg_MapBufferRange && hg_UnmapBuffer) {
                const unsigned PACK = 0x88EB;   /* GL_PIXEL_PACK_BUFFER */
                void *m;
                if (w != hg_pbo_w || h != hg_pbo_h) {   /* (re)made at the drawable's size */
                    int k;
                    if (!hg_pbo[0]) hg_GenBuffers(2, hg_pbo);
                    for (k = 0; k < 2; k++) { hg_BindBuffer(PACK, hg_pbo[k]); hg_BufferData(PACK, (long)n, NULL, 0x88E1 /* GL_STREAM_READ */); }
                    hg_pbo_w = w; hg_pbo_h = h; hg_pbo_n = 0;
                }
                hg_BindBuffer(PACK, hg_pbo[hg_pbo_cur]);
                hg_ReadPixels(0, 0, w, h, 0x1908, 0x1401, NULL);              /* into the buffer: no wait */
                hg_pbo_cur ^= 1;
                hg_pbo_n++;
                if (hg_pbo_n < 2) { hg_BindBuffer(PACK, 0); if (hg_BindFramebuffer) hg_BindFramebuffer(0x8CA8, (unsigned)fb); REAL(SDL_GL_SwapWindow)(win); return; }
                hg_BindBuffer(PACK, hg_pbo[hg_pbo_cur]);                     /* the frame before: done by now */
                m = hg_MapBufferRange(PACK, 0, (long)n, 0x0001 /* GL_MAP_READ_BIT */);
                if (m) { memcpy(buf, m, n); hg_UnmapBuffer(PACK); }
                hg_BindBuffer(PACK, 0);
                if (!m) hg_ReadPixels(0, 0, w, h, 0x1908, 0x1401, buf);
            } else hg_ReadPixels(0, 0, w, h, 0x1908, 0x1401, buf);           /* GL_RGBA, GL_UNSIGNED_BYTE */
            if (hg_BindFramebuffer) hg_BindFramebuffer(0x8CA8, (unsigned)fb);
            for (y = 0; y < h / 2; y++) {   /* GL's rows run upwards */
                uint8_t *a = buf + (size_t)y * row, *b = buf + (size_t)(h - 1 - y) * row, t[4096];
                size_t k;
                for (k = 0; k < row; k += sizeof(t)) {
                    size_t m = row - k < sizeof(t) ? row - k : sizeof(t);
                    memcpy(t, a + k, m); memcpy(a + k, b + k, m); memcpy(b + k, t, m);
                }
            }
            {   /* PSPEXP_FRAME_DUMP=PREFIX[:N]: every Nth frame (default 60) to PREFIX-K.ppm on the board:
                 * looking at a game drawn here with no PSP or stand-in to show it (tests) */
                static const char *dump; static int dump_every = -1, dump_k; static unsigned long dump_n;
                if (dump_every < 0) {
                    const char *e = getenv("PSPEXP_FRAME_DUMP"), *c = e ? strrchr(e, ':') : NULL;
                    dump_every = e && *e ? (c && atoi(c + 1) > 0 ? atoi(c + 1) : 60) : 0;
                    dump = e ? strdup(e) : NULL;
                    if (dump && c) ((char *)dump)[c - e] = 0;
                }
                if (dump_every && dump_n++ % (unsigned long)dump_every == 0) {
                    char name[600];
                    FILE *df;
                    static Uint32 dump_t0;   /* (the name carries the ms since the first: matched against PSPEXP_SCRIPT's times) */
                    if (!dump_t0) dump_t0 = SDL_GetTicks() ? SDL_GetTicks() : 1;
                    snprintf(name, sizeof(name), "%s-%d-%ums.ppm", dump, dump_k++, (unsigned)(SDL_GetTicks() - dump_t0));
                    if ((df = fopen(name, "wb"))) {
                        int yy, xx;
                        fprintf(df, "P6\n%d %d\n255\n", w, h);
                        for (yy = 0; yy < h; yy++) for (xx = 0; xx < w; xx++) fwrite(buf + (size_t)yy * row + (size_t)xx * 4, 1, 3, df);
                        fclose(df);
                    }
                }
            }
            {
                Frame f;
                f.w = w; f.h = h; f.format = SDL_PIXELFORMAT_ABGR8888; f.pixels = buf; f.pitch = w * 4;
                stream_frame(win, &f);
            }
        }
    }
    REAL(SDL_GL_SwapWindow)(win);
}
#endif

#if !defined(PSPEXP_STATIC_RENAME) && !defined(PSPEXP_HOST_GL)
#include "gl_pspexp.h"

PspExpContext *shim_gl_frame(void)
{
    if (!link_up() || !pspexp_3d_supported(S.ctx)) return NULL;
    begin_frame();
    return S.ctx;
}

void shim_gl_geometry(int *win_w, int *win_h, float *scale, int *ox, int *oy)
{
    *win_w = S.win_w > 0 ? S.win_w : 480;
    *win_h = S.win_h > 0 ? S.win_h : 272;
    *scale = S.sx > 0 ? S.sx : 1.0f;
    *ox = S.ox; *oy = S.oy;
}

uint16_t shim_gl_handle(void) { return alloc_handle(); }

void shim_gl_flush(void)
{
    if (!S.frame_open || !link_up()) return;
    pspexp_batch_flush(S.ctx);
    pspexp_wait_present(S.ctx, 40);
    pspexp_present(S.ctx);
    S.frame_open = 0;
    pspexp_pump(S.ctx, 0);
}
int shim_gl_reconnects(void) { return S.ctx ? (int)pspexp_stats(S.ctx)->reconnects : 0; }

void shim_gl_log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[sdl-pspexp] "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap);
}

static int gl_context_token;   /* its address is the context handed out */

HOOK(SDL_GLContext, SDL_GL_CreateContext, (SDL_Window *win))
{
    /* ours is OpenGL 2.1 (and ES 2 through it): a program that asks for 3.x
     * is refused, as a real driver without it would, and tries again with
     * less. A core profile without a version of 3.2 or more is not one (core
     * profiles begin there; drivers give a compatibility context): Hurrican
     * asks for "core" with no version for its OpenGL 1.x drawing */
    if (S.gl_major >= 3) {
        logf_("gl: an OpenGL %d.%d%s context asked for: refused (2.1 here)", S.gl_major, S.gl_minor,
              S.gl_profile & SDL_GL_CONTEXT_PROFILE_CORE ? " core" : "");
        SDL_SetError("OpenGL %d.%d is not available (2.1)", S.gl_major, S.gl_minor);
        pspexp_gl_refused();
        return NULL;
    }
    if (!S.win) { S.win = win; SDL_GetWindowSize(win, &S.win_w, &S.win_h); }
    S.want_depth = 1;
    if (S.inited && S.ctx) pspexp_set_depth_buffer(S.ctx, 1);   /* connected already (audio came first) */
    connect_once();
    update_scale();
    pspexp_gl_init();
    S.gl_context = 1;
    if (S.active && S.ctx && link_up() && !pspexp_3d_supported(S.ctx))
        logf_("the PSP's runtime cannot draw OpenGL programs (no 3D): update it");
    if (S.hide_window && S.active) SDL_HideWindow(win);
    return (SDL_GLContext)&gl_context_token;
}
HOOK(int, SDL_GL_MakeCurrent, (SDL_Window *win, SDL_GLContext c)) { (void)win; (void)c; return 0; }
HOOK(SDL_GLContext, SDL_GL_GetCurrentContext, (void)) { return S.gl_context ? (SDL_GLContext)&gl_context_token : NULL; }
HOOK(SDL_Window *, SDL_GL_GetCurrentWindow, (void)) { return S.gl_context ? S.win : NULL; }
HOOK(void, SDL_GL_DeleteContext, (SDL_GLContext c)) { (void)c; }
HOOK(void *, SDL_GL_GetProcAddress, (const char *proc)) { return pspexp_gl_proc(proc); }
HOOK(SDL_bool, SDL_GL_ExtensionSupported, (const char *ext)) { return pspexp_gl_extension(ext) ? SDL_TRUE : SDL_FALSE; }
HOOK(int, SDL_GL_LoadLibrary, (const char *path)) { (void)path; return 0; }
HOOK(void, SDL_GL_UnloadLibrary, (void)) { }
HOOK(int, SDL_GL_SetSwapInterval, (int interval)) { (void)interval; return 0; }
HOOK(int, SDL_GL_GetSwapInterval, (void)) { return 1; }   /* the PSP's vertical blank paces every frame */
HOOK(void, SDL_GL_GetDrawableSize, (SDL_Window *win, int *w, int *h)) { SDL_GetWindowSize(win, w, h); }
HOOK(void, SDL_GL_ResetAttributes, (void)) { S.gl_major = S.gl_minor = S.gl_profile = 0; }
HOOK(int, SDL_GL_SetAttribute, (SDL_GLattr attr, int value))
{
    if (attr == SDL_GL_CONTEXT_MAJOR_VERSION) S.gl_major = value;
    else if (attr == SDL_GL_CONTEXT_MINOR_VERSION) S.gl_minor = value;
    else if (attr == SDL_GL_CONTEXT_PROFILE_MASK) S.gl_profile = value;
    return 0;
}
HOOK(int, SDL_GL_GetAttribute, (SDL_GLattr attr, int *value))
{
    switch (attr) {
    case SDL_GL_RED_SIZE: case SDL_GL_BLUE_SIZE: *value = 5; break;
    case SDL_GL_GREEN_SIZE: *value = 6; break;
    case SDL_GL_BUFFER_SIZE: case SDL_GL_DEPTH_SIZE: *value = 16; break;
    case SDL_GL_DOUBLEBUFFER: case SDL_GL_ACCELERATED_VISUAL: *value = 1; break;
    case SDL_GL_CONTEXT_MAJOR_VERSION: *value = 2; break;
    case SDL_GL_CONTEXT_MINOR_VERSION: *value = 1; break;
    case SDL_GL_CONTEXT_PROFILE_MASK: *value = S.gl_profile == SDL_GL_CONTEXT_PROFILE_ES ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_COMPATIBILITY; break;
    default: *value = 0; break;
    }
    return 0;
}
HOOK(void, SDL_GL_SwapWindow, (SDL_Window *win))
{
    (void)win;
    pspexp_gl_frame_done();
    end_frame();
    if (!link_up()) SDL_Delay(16);   /* no PSP to pace the frames */
}
#endif

/* ---- hooks: event polling ------------------------------------------------------ */

/* With the PSP as the screen, the host window is usually not the focused
 * one; games that pause or mute on focus loss would stay paused. Hide focus
 * loss from them (PSPEXP_FOCUS=host keeps SDL's real focus events). */
static int hide_focus_loss(void)
{
    static int checked, host;
    if (!checked) { checked = 1; host = getenv("PSPEXP_FOCUS") && !strcmp(getenv("PSPEXP_FOCUS"), "host"); }
    return S.active && !host;   /* (asked again each time: a game may ask before the link is up) */
}

/* the same for games that ask the window rather than watch events (NXEngine
 * pauses while its window lacks input focus, which a headless one never has;
 * Quakespasm stops swapping while its window is hidden, which pspexp-run's is) */
HOOK(Uint32, SDL_GetWindowFlags, (SDL_Window *win))
{
    Uint32 f = REAL(SDL_GetWindowFlags)(win);
    if (win && win == S.gl_window) f |= SDL_WINDOW_OPENGL;   /* as asked, though SDL made it without */
    if (hide_focus_loss() && win && (win == S.win || win == S.gl_window))
        f = (f | SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS) & ~(Uint32)(SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN);
    return f;
}

/* PSPEXP_LOG_EVENTS=1: every input event the program takes from SDL, in the log (which input reaches a
 * game, and from which device) */
static void log_event(const SDL_Event *ev)
{
    static int on = -1;
    if (on < 0) on = getenv("PSPEXP_LOG_EVENTS") && atoi(getenv("PSPEXP_LOG_EVENTS")) > 0;
    if (!on) return;
    switch (ev->type) {
    case SDL_KEYDOWN: case SDL_KEYUP: logf_("event: key %s %s", SDL_GetKeyName(ev->key.keysym.sym), ev->type == SDL_KEYDOWN ? "down" : "up"); break;
    /* the button and where the pointer was: with the nub as the mouse, a click shows the nub moved it too */
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: logf_("event: mouse button %d %s at %d,%d", ev->button.button, ev->type == SDL_MOUSEBUTTONDOWN ? "down" : "up", ev->button.x, ev->button.y); break;
    case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP: logf_("event: joystick %d button %d %s", (int)ev->jbutton.which, ev->jbutton.button, ev->type == SDL_JOYBUTTONDOWN ? "down" : "up"); break;
    case SDL_JOYAXISMOTION: if (ev->jaxis.value > 16000 || ev->jaxis.value < -16000) logf_("event: joystick %d axis %d %d", (int)ev->jaxis.which, ev->jaxis.axis, ev->jaxis.value); break;
    case SDL_JOYHATMOTION: logf_("event: joystick %d hat %d", (int)ev->jhat.which, ev->jhat.value); break;
    case SDL_CONTROLLERAXISMOTION: if (ev->caxis.value > 16000 || ev->caxis.value < -16000) logf_("event: controller %d axis %s %d", (int)ev->caxis.which, SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)ev->caxis.axis), ev->caxis.value); break;
    case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: logf_("event: controller %d %s %s", (int)ev->cbutton.which, SDL_GameControllerGetStringForButton((SDL_GameControllerButton)ev->cbutton.button), ev->type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up"); break;
    case SDL_MOUSEMOTION: {   /* (one in 20: they come every frame) */
        static unsigned k;
        if (k++ % 20 == 0) logf_("event: mouse motion to %d,%d by %d,%d", ev->motion.x, ev->motion.y, ev->motion.xrel, ev->motion.yrel);
        break;
    }
    case SDL_JOYDEVICEREMOVED: logf_("event: joystick %d removed", (int)ev->jdevice.which); break;
    case SDL_JOYDEVICEADDED: case SDL_CONTROLLERDEVICEADDED: logf_("event: %s device %d added", ev->type == SDL_JOYDEVICEADDED ? "joystick" : "controller", (int)ev->jdevice.which); break;
    default: break;
    }
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
    static SDL_Window *done;   /* each window once (a game may make a new one: Unreal Tournament does) */
    SDL_Event ev;
    if (done == S.win || !hide_focus_loss() || !S.win) return;
    done = S.win;
    SDL_zero(ev);
    ev.type = SDL_WINDOWEVENT; ev.window.event = SDL_WINDOWEVENT_FOCUS_GAINED; ev.window.windowID = SDL_GetWindowID(S.win);
    SDL_PushEvent(&ev);
}

/* the board's own keyboard (the Pi 400's) into the shared table too, for games
 * that read it after pumping (Hurrican) */
/* the PSP's input too (service_link): a game waiting in a loop of SDL_PumpEvents and
 * SDL_GetKeyboardState, drawing nothing, never saw the PSP's buttons change (Hurrican's menu,
 * waiting for Escape, START, to be let go: no button answered, now and then) */
HOOK(void, SDL_PumpEvents, (void))
{
    service_link();
    REAL(SDL_PumpEvents)();
    kb_refresh();
}

HOOK(int, SDL_PollEvent, (SDL_Event *ev))
{
    int rc;
    service_link();
    fake_focus_once();
    while ((rc = REAL(SDL_PollEvent)(ev)) == 1 && ev && drop_event(ev)) ;
    if (rc == 1 && ev) log_event(ev);
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
        if (timeout >= 0 && step <= 0) {
            rc = REAL(SDL_PollEvent)(ev);
            if (rc == 1 && ev) log_event(ev);
            return rc;
        }
        if (step > 20) step = 20;
        rc = REAL(SDL_WaitEventTimeout)(ev, step);
        if (rc && ev && drop_event(ev)) continue;
        if (rc && ev) log_event(ev);   /* (the games that wait, LinCity-NG, logged too) */
        if (rc) return rc;
    }
}

HOOK(int, SDL_WaitEventTimeout, (SDL_Event *ev, int timeout)) { return wait_event_impl(ev, timeout); }
HOOK(int, SDL_WaitEvent, (SDL_Event *ev)) { return wait_event_impl(ev, -1); }

/* ---- hooks: window surface (programs without a renderer) --------------------- */

/* A whole frame of pixels to the PSP (a window surface; with PSPEXP_HOST_GL, the frame the host's own
 * GPU drew): converted, shrunk to the PSP's screen when larger, the pages that changed sent */
static void stream_frame_locked(SDL_Window *win, const Frame *surf);
static void stream_frame(SDL_Window *win, const Frame *surf)
{
    link_lock();
    stream_frame_locked(win, surf);
    link_unlock();
}
static void stream_frame_locked(SDL_Window *win, const Frame *surf)
{
    Tex *x;
    if (!S.inited) { S.win = win; S.win_w = surf->w; S.win_h = surf->h; connect_once(); update_scale(); }
    if (!link_up()) { end_frame(); return; }
#ifdef PSPEXP_HOST_GL
    {   /* a frame of another size than the window's drops the page cache each time: say so once */
        static int said;
        if (S.surface_tex && (S.win_w != surf->w || S.win_h != surf->h) && !said) { said = 1; logf_("host GL: frame %dx%d, window %dx%d", surf->w, surf->h, S.win_w, S.win_h); }
    }
#endif
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
    if (S.sx >= 1.0f) {   /* full size: only the pages that changed */
        SDL_Rect all = { 0, 0, surf->w, surf->h };
        x->w = surf->w; x->h = surf->h;
        shadow_update(x, &all, surf->format, surf->pixels, surf->pitch);
    } else {
        x->argb = 0; x->shadow16 = 0;
        SDL_ConvertPixels(surf->w, surf->h, surf->format, surf->pixels, surf->pitch, SDL_PIXELFORMAT_ABGR8888, x->rgba, surf->w * 4);
    }
    if (S.sx < 1.0f) {
        int dw = (int)(surf->w * S.sx + 0.5f), dh = (int)(surf->h * S.sy + 0.5f), yy, xx;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        /* each pixel the average of the frame's under it (nearest-pixel shrinking dropped rows and
         * columns, which sparkled as things moved); in place: a row is written over rows already read */
        for (yy = 0; yy < dh; yy++) {
            int sy0 = yy * surf->h / dh, sy1 = (yy + 1) * surf->h / dh;
            uint32_t *dst = x->rgba + (size_t)yy * dw;
            if (sy1 <= sy0) sy1 = sy0 + 1;
            for (xx = 0; xx < dw; xx++) {
                int sx0 = xx * surf->w / dw, sx1 = (xx + 1) * surf->w / dw, a, b;
                uint32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0, n;
                if (sx1 <= sx0) sx1 = sx0 + 1;
                for (a = sy0; a < sy1; a++) {
                    const uint32_t *row = x->rgba + (size_t)a * surf->w;
                    for (b = sx0; b < sx1; b++) { uint32_t c = row[b]; c0 += c & 255; c1 += (c >> 8) & 255; c2 += (c >> 16) & 255; c3 += c >> 24; }
                }
                n = (uint32_t)((sy1 - sy0) * (sx1 - sx0));
                dst[xx] = (c0 / n) | ((c1 / n) << 8) | ((c2 / n) << 16) | ((c3 / n) << 24);
            }
        }
        x->w = dw; x->h = dh;
        {   /* only the pages that changed since the last frame, as at full size: a frame shrunk from a
             * larger window was all sent every time (OpenRA's battlefield, 140 KB a frame however still) */
            static uint32_t *prev;
            static int prev_w, prev_h;
            size_t n = (size_t)dw * dh;
            if (prev && prev_w == dw && prev_h == dh && x->npages && !x->npages_grid_changed && !x->dirty && !S.no_delta) {
                int i, yy2;
                uint32_t bits = 0;
                for (i = 0; i < x->npages; i++) {
                    const Page *p = &x->pages[i];
                    for (yy2 = p->y0; yy2 < p->y0 + p->h; yy2++)
                        if (memcmp(x->rgba + (size_t)yy2 * dw + p->x0, prev + (size_t)yy2 * dw + p->x0, (size_t)p->w * 4)) { bits |= 1u << i; break; }
                }
                x->pdirty |= bits;
            } else x->dirty = 1;
            if (prev_w * prev_h != (int)n) { free(prev); prev = (uint32_t *)malloc(n * 4); }
            if (prev) { memcpy(prev, x->rgba, n * 4); prev_w = dw; prev_h = dh; } else prev_w = prev_h = 0;
        }
    }
    begin_frame();
    S.has_clip = 0;
    apply_clip();
    /* Black letterbox bars (and, under PPSSPP, a frame made only of textured
     * quads never reaches its framebuffer: the flat rect first makes it show). */
    pspexp_fill_rect(S.ctx, 0, 0, 480, 272, 0, 255);
    if (!(x->dirty || x->pdirty) || upload_tex(x) == 0) {
        int i;
        uint16_t fmt, color = tint_for(x, &fmt);
        int shrunk = S.sx < 1.0f;
        for (i = 0; i < x->npages; i++) {
            const Page *p = &x->pages[i];
            PspExpQuadExRec q;
            memset(&q, 0, sizeof(q));
            if (shrunk) { q.x = (int16_t)(S.ox + p->x0); q.y = (int16_t)(S.oy + p->y0); q.w = (int16_t)p->w; q.h = (int16_t)p->h; }
            else {   /* edges from the rounded positions: each page's own rounded width left one-pixel gaps when enlarging (Open Surge's 426 wide on 480: a line) */
                q.x = X((float)p->x0); q.y = Y((float)p->y0);
                q.w = (int16_t)(X((float)(p->x0 + p->w)) - q.x); q.h = (int16_t)(Y((float)(p->y0 + p->h)) - q.y);
            }
            q.u0 = 0; q.v0 = 0; q.u1 = (uint16_t)p->w; q.v1 = (uint16_t)p->h; q.color = color;
            if (S.nquads && S.qtex != p->tex) flush_quads();
            S.qtex = p->tex; S.qfmt = fmt;
            S.quads[S.nquads++] = q;
        }
    }
    end_frame();
}

static void stream_surface(SDL_Window *win)
{
    SDL_Surface *surf = SDL_GetWindowSurface(win);
    Frame f;
    if (!surf) return;
    if (SDL_MUSTLOCK(surf)) SDL_LockSurface(surf);
    f.w = surf->w; f.h = surf->h; f.format = surf->format->format; f.pixels = surf->pixels; f.pitch = surf->pitch;
    stream_frame(win, &f);
    if (SDL_MUSTLOCK(surf)) SDL_UnlockSurface(surf);
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
    for (slot = 2; slot < 8; slot++) if (!S.dev_open[slot]) break;   /* (1: SDL_OpenAudio's) */
    if (slot == 8) return REAL(SDL_OpenAudioDevice)(device, iscapture, desired, obtained, allowed);
    want = *desired;
    {
        /* PSPEXP_AUDIO_RATE=22050 (or 11025): a program that lets the rate change mixes at that rate,
         * upsampled here by integers (audio_tap). Warcraft's MIDI music, synthesised by FluidSynth as it
         * plays, took a whole core of the GameShell's Cortex-A7 at 44.1 kHz and the sound skipped */
        const char *r = getenv("PSPEXP_AUDIO_RATE");
        int rate = r ? atoi(r) : 0;
        if ((allowed & SDL_AUDIO_ALLOW_FREQUENCY_CHANGE) && rate > 0 && rate < want.freq && 44100 % rate == 0) want.freq = rate;
    }
    S.dev_cb[slot] = desired->callback;
    S.dev_ud[slot] = desired->userdata;
    if (desired->callback) { want.callback = audio_callback_wrapper; want.userdata = (void *)(intptr_t)slot; }
    id = REAL(SDL_OpenAudioDevice)(device, iscapture, &want, &have, allowed);
    if (id) {
        if (S.pace_mu) SDL_LockMutex(S.pace_mu);
        setup_stream(slot, &have);
        S.dev_open[slot] = 1; S.dev_id[slot] = id;
        if (S.pace_mu) SDL_UnlockMutex(S.pace_mu);
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
    if (rc == 0) { setup_stream(1, obtained ? obtained : &have); S.dev_open[1] = 1; S.dev_id[1] = 1; }
    if (rc == 0 && S.dev_cb[1] && want_selfpaced()) start_selfpaced(1, 1);
    desired->callback = S.dev_cb[1]; desired->userdata = S.dev_ud[1];
    if (obtained) { obtained->callback = S.dev_cb[1]; obtained->userdata = S.dev_ud[1]; }
    return rc;
}

/* A closed device's slot let go: Mupen64Plus closes its sound device and opens it again when a game
 * starts, SDL gives the new one the same number, and the old slot, still paced, had the plugin's
 * callback called twice as often: twice the sound to the PSP (its buffer full, the rest dropped) and
 * the plugin's own buffer run dry, a crackle. */
static void audio_slot_closed(SDL_AudioDeviceID dev)
{
    int slot;
    if (S.pace_mu) SDL_LockMutex(S.pace_mu);
    for (slot = 1; slot < 8; slot++)
        if (S.dev_open[slot] && S.dev_id[slot] == dev) { S.dev_open[slot] = 0; S.dev_selfpaced[slot] = 0; S.dev_cb[slot] = NULL; }
    if (S.pace_mu) SDL_UnlockMutex(S.pace_mu);
}
HOOK(void, SDL_CloseAudioDevice, (SDL_AudioDeviceID dev))
{
    audio_slot_closed(dev);
    REAL(SDL_CloseAudioDevice)(dev);
}
HOOK(void, SDL_CloseAudio, (void))
{
    audio_slot_closed(1);
    REAL(SDL_CloseAudio)();
}

/* The virtual controller attached as the program starts SDL's joysticks, before it lists them: attached
 * later (at the first input from the PSP), it was missed by programs that look once and do not watch
 * for controllers added (Sonic Robo Blast 2: no PSP button reached it, its intro could not be skipped) */
HOOK(int, SDL_Init, (Uint32 flags))
{
    int rc = REAL(SDL_Init)(flags);
    if (rc == 0 && (flags & (SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER))) setup_vjoy();
    return rc;
}
HOOK(int, SDL_InitSubSystem, (Uint32 flags))
{
    int rc = REAL(SDL_InitSubSystem)(flags);
    if (rc == 0 && (flags & (SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER))) setup_vjoy();
    return rc;
}

/* the joysticks shut down: SDL lets the virtual one go with them, attached again on the next start */
HOOK(void, SDL_QuitSubSystem, (Uint32 flags))
{
    REAL(SDL_QuitSubSystem)(flags);
    if ((flags & (SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER)) && !SDL_WasInit(SDL_INIT_JOYSTICK)) { S.vjoy_handle = NULL; S.vjoy = -1; }
}

HOOK(int, SDL_QueueAudio, (SDL_AudioDeviceID dev, const void *data, Uint32 len))
{
    int slot = -1, i;
    for (i = 1; i < 8; i++) if (S.dev_open[i] && S.dev_id[i] == dev) { slot = i; break; }
    if (slot < 0) for (i = 1; i < 8; i++) if (S.dev_open[i]) { slot = i; break; }   /* single queued device is the common case */
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
    if (S.ctx && S.total_frames) {
        const PspExpStats *st = pspexp_stats(S.ctx);
        double fr = (double)S.total_frames;
        double secs = (double)(SDL_GetPerformanceCounter() - S.first_frame) / (double)SDL_GetPerformanceFrequency();
        logf_("totals: %.1f fps  link %.1f KB/frame  pages sent %lu (clut8 %lu) kept %lu  texture work %.2f ms/frame (delta %s, clut8 %s)",
              secs > 0 ? (fr - 1) / secs : 0.0, (double)st->bytes_sent / 1024.0 / fr, S.tot_pages_sent + S.pages_sent, S.tot_pages_t8 + S.pages_t8,
              S.tot_pages_kept + S.pages_kept, (double)(S.tot_up_ticks + S.up_ticks) / 1e6 / fr,
              S.no_delta ? "off" : "on", S.no_clut ? "off" : "on");
        if (S.silent_ms) logf_("audio: %lu ms of silence sent while the program had its device paused, against %lu ms of its sound", S.silent_ms, S.sound_ms);
        if (S.dropped) logf_("frames not sent while the PSP was busy (PSPEXP_FRAME_DROP): %lu", S.dropped);
    }
    if (S.audio_thread) { S.audio_thread_stop = 1; SDL_WaitThread(S.audio_thread, NULL); S.audio_thread = NULL; }
    if (S.ctx) {
        pspexp_send_raw(S.ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_DISCONNECT, NULL, 0);
        pspexp_destroy(S.ctx);
        S.ctx = NULL;
    }
    if (S.nul) {
        const PspExpNullStats *n = pspexp_null_stats(S.nul);
        if (n->draws3d || n->clears3d)
            logf_("null executor: 3D %u draws (%u of meshes, %u textured) %u vertices, %u clears, %u states, %u matrices, %u lights, %u targets",
                  n->draws3d, n->mesh_draws3d, n->textured3d, n->vertices3d, n->clears3d, n->states3d, n->matrices3d, n->lights3d, n->targets3d);
        pspexp_null_destroy(S.nul);
    }
#ifndef PSPEXP_NO_SDL_EXECUTOR
    if (S.sdlx) pspexp_sdl_destroy(S.sdlx);
#endif
}
