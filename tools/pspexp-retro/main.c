/*
 * pspexp-retro: a libretro player that is a plain SDL2 program. It loads one
 * emulator core, plays one ROM and has no menus: the PSPExp launcher is the
 * menu (systems, ROM lists, box art), and the SDL layer (sdl-pspexp, preloaded
 * by pspexp-run or linked in on the Omega2+) turns its SDL calls into PSPExp
 * commands, so the PSP is its screen, speaker and controller. It never uses
 * PSPExp itself; on the Mac it plays in a window.
 *
 *   pspexp-retro CORE ROM
 *
 * CORE is a core file (a path) or a core name ("fceumm", "nestopia"), looked
 * for as NAME_libretro.so/.dylib in $PSPEXP_RETRO_CORES (':'-separated), the
 * player's own directory, /opt/pspexp/cores, and the distribution's libretro
 * directories (Debian: /usr/lib/<arch>/libretro). Built with
 * PSPEXP_RETRO_STATIC the core is linked in and CORE is only its name.
 *
 * What the PSP gets from it, and why it is cheap:
 *   - the core's frame at its own size (256x240 for the NES), one streaming
 *     texture; the PSP's GE scales it to the screen at the core's aspect
 *     (SDL_RenderSetLogicalSize), the board scales nothing;
 *   - frames the core repeats (dupes) are not updated at all, and the SDL
 *     layer sends only the texture pages that changed, as CLUT8 when a page
 *     has at most 256 colours (8- and 16-bit machines nearly always do);
 *   - sound resampled here to 44.1 kHz stereo, the PSP's rate (integer
 *     linear interpolation: no floating point per sample for FPU-less
 *     boards), through an SDL audio callback paced by the SDL layer's clock;
 *     the ratio follows the buffer by up to +-0.5 %, so the core's clock
 *     (60.1 fps NES, 32040 Hz SNES) and the PSP's 60 Hz display never drift.
 *
 * Controls: SDL game controller (the PSP's, which the SDL layer provides:
 * cross = B, circle = A, square = Y, triangle = X, as on a Nintendo pad; the
 * nub is the d-pad) or the keyboard (arrows, x = A, z = B, s = X, a = Y,
 * q / w = L / R, Return = Start, right Shift = Select, Escape quits). Holding
 * START + SELECT on the PSP leaves (the SDL layer's gesture).
 *
 * Battery saves (the cartridge's SRAM) go to $PSPEXP_RETRO_SAVES or
 * ~/.local/share/pspexp-retro/saves/<rom name>.srm, written every 10 s when
 * they changed and at exit. Core options: defaults, overridden by KEY=value
 * lines in ~/.config/pspexp-retro/<core>.opt or $PSPEXP_RETRO_OPT
 * ("key=value;key=value"). Environment: PSPEXP_RETRO_ASPECT=pixel for square
 * pixels instead of the core's aspect.
 */
#include <SDL.h>
#include "libretro.h"
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef PSPEXP_RETRO_STATIC
#include <dlfcn.h>
#endif

#ifdef __APPLE__
#define CORE_SUFFIX "_libretro.dylib"
#else
#define CORE_SUFFIX "_libretro.so"
#endif

/* ---- the core's entry points ---------------------------------------------- */

static struct {
    void (*init)(void);
    void (*deinit)(void);
    unsigned (*api_version)(void);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*run)(void);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    void *(*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);
} C;

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "pspexp-retro: ");
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

#ifdef PSPEXP_RETRO_STATIC
static void core_bind(const char *name)
{
    (void)name;
    C.init = retro_init; C.deinit = retro_deinit; C.api_version = retro_api_version;
    C.get_system_info = retro_get_system_info; C.get_system_av_info = retro_get_system_av_info;
    C.set_environment = retro_set_environment; C.set_video_refresh = retro_set_video_refresh;
    C.set_audio_sample = retro_set_audio_sample; C.set_audio_sample_batch = retro_set_audio_sample_batch;
    C.set_input_poll = retro_set_input_poll; C.set_input_state = retro_set_input_state;
    C.set_controller_port_device = retro_set_controller_port_device;
    C.run = retro_run; C.load_game = retro_load_game; C.unload_game = retro_unload_game;
    C.get_memory_data = retro_get_memory_data; C.get_memory_size = retro_get_memory_size;
}
#else
static int try_core(char *out, size_t n, const char *dir, const char *name)
{
    struct stat st;
    snprintf(out, n, "%s/%s%s", dir, name, CORE_SUFFIX);
    return stat(out, &st) == 0;
}

static void core_find(const char *name, const char *argv0, char *out, size_t n)
{
    static const char *dirs[] = {
        "/opt/pspexp/cores",
        "/usr/lib/aarch64-linux-gnu/libretro", "/usr/lib/arm-linux-gnueabihf/libretro",
        "/usr/lib/x86_64-linux-gnu/libretro", "/usr/lib/libretro", "/usr/local/lib/libretro",
    };
    char buf[PATH_MAX], *p, *save;
    size_t i;
    if (strchr(name, '/')) { snprintf(out, n, "%s", name); return; }
    if (getenv("PSPEXP_RETRO_CORES")) {
        snprintf(buf, sizeof(buf), "%s", getenv("PSPEXP_RETRO_CORES"));
        for (p = strtok_r(buf, ":", &save); p; p = strtok_r(NULL, ":", &save))
            if (try_core(out, n, p, name)) return;
    }
    snprintf(buf, sizeof(buf), "%s", argv0);
    if ((p = strrchr(buf, '/'))) { *p = 0; if (try_core(out, n, buf, name)) return; }
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        if (try_core(out, n, dirs[i], name)) return;
    die("core \"%s\" not found (%s%s in $PSPEXP_RETRO_CORES, next to the player, /opt/pspexp/cores, the libretro directories)",
        name, name, CORE_SUFFIX);
}

static void core_bind(const char *path)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) die("cannot load %s: %s", path, dlerror());
#define SYM(field, sym) do { *(void **)&C.field = dlsym(h, sym); if (!C.field) die("%s: no %s", path, sym); } while (0)
    SYM(init, "retro_init"); SYM(deinit, "retro_deinit"); SYM(api_version, "retro_api_version");
    SYM(get_system_info, "retro_get_system_info"); SYM(get_system_av_info, "retro_get_system_av_info");
    SYM(set_environment, "retro_set_environment"); SYM(set_video_refresh, "retro_set_video_refresh");
    SYM(set_audio_sample, "retro_set_audio_sample"); SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
    SYM(set_input_poll, "retro_set_input_poll"); SYM(set_input_state, "retro_set_input_state");
    SYM(set_controller_port_device, "retro_set_controller_port_device");
    SYM(run, "retro_run"); SYM(load_game, "retro_load_game"); SYM(unload_game, "retro_unload_game");
    SYM(get_memory_data, "retro_get_memory_data"); SYM(get_memory_size, "retro_get_memory_size");
#undef SYM
}
#endif

/* ---- state ------------------------------------------------------------------- */

static struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    int tex_w, tex_h;                 /* texture size (the core's maximum geometry) */
    int fb_w, fb_h;                   /* the last frame's size */
    Uint32 sdl_format;
    enum retro_pixel_format pixfmt;
    float aspect;
    int square_pixels;
    struct retro_system_av_info av;
    double fps;
    /* audio: a ring of stereo frames, filled by the core, drained by SDL's callback */
    SDL_AudioDeviceID adev;
    int16_t *ring; unsigned ring_cap, ring_head, ring_tail, ring_fill;
    unsigned underruns;
    uint32_t step;                    /* core frames per output frame, 16.16 */
    uint32_t frac;                    /* position between last and the next input frame, 16.16 */
    int16_t last[2];
    /* input */
    SDL_GameController *pad;
    uint16_t buttons;                 /* RETRO_DEVICE_ID_JOYPAD_* bits */
    /* paths */
    char system_dir[PATH_MAX], save_dir[PATH_MAX], srm_path[PATH_MAX], core_name[64];
    /* options: key, default (first value), override */
    struct { char key[96], def[96], val[96]; } opt[256]; int nopt;
    int running;
} P;

/* ---- options ---------------------------------------------------------------- */

static void opt_override(const char *key, const char *val)
{
    int i;
    for (i = 0; i < P.nopt; i++) if (!strcmp(P.opt[i].key, key)) { snprintf(P.opt[i].val, sizeof(P.opt[i].val), "%s", val); return; }
    if (P.nopt < (int)(sizeof(P.opt) / sizeof(P.opt[0]))) {
        snprintf(P.opt[P.nopt].key, sizeof(P.opt[0].key), "%s", key);
        snprintf(P.opt[P.nopt].val, sizeof(P.opt[0].val), "%s", val);
        P.nopt++;
    }
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t' || *s == '"') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r' || e[-1] == '"')) *--e = 0;
    return s;
}

static void opt_load_overrides(void)
{
    char path[PATH_MAX], line[256], *eq;
    const char *home = getenv("HOME");
    FILE *f;
    if (home) {
        snprintf(path, sizeof(path), "%s/.config/pspexp-retro/%s.opt", home, P.core_name);
        if ((f = fopen(path, "r"))) {
            while (fgets(line, sizeof(line), f))
                if (line[0] != '#' && (eq = strchr(line, '='))) { *eq = 0; opt_override(trim(line), trim(eq + 1)); }
            fclose(f);
        }
    }
    if (getenv("PSPEXP_RETRO_OPT")) {
        char buf[1024], *p, *save;
        snprintf(buf, sizeof(buf), "%s", getenv("PSPEXP_RETRO_OPT"));
        for (p = strtok_r(buf, ";", &save); p; p = strtok_r(NULL, ";", &save))
            if ((eq = strchr(p, '='))) { *eq = 0; opt_override(trim(p), trim(eq + 1)); }
    }
}

/* RETRO_ENVIRONMENT_SET_VARIABLES: "Description; first|second|..." */
static void opt_defaults(const struct retro_variable *v)
{
    for (; v && v->key; v++) {
        const char *semi = v->value ? strchr(v->value, ';') : NULL;
        char def[96];
        int i, found = -1;
        if (!semi) continue;
        semi++;
        while (*semi == ' ') semi++;
        snprintf(def, sizeof(def), "%.*s", (int)strcspn(semi, "|"), semi);
        for (i = 0; i < P.nopt; i++) if (!strcmp(P.opt[i].key, v->key)) found = i;
        if (found < 0 && P.nopt < (int)(sizeof(P.opt) / sizeof(P.opt[0]))) {
            found = P.nopt++;
            snprintf(P.opt[found].key, sizeof(P.opt[0].key), "%s", v->key);
            P.opt[found].val[0] = 0;
        }
        if (found >= 0) snprintf(P.opt[found].def, sizeof(P.opt[0].def), "%s", def);
    }
}

static const char *opt_get(const char *key)
{
    int i;
    for (i = 0; i < P.nopt; i++)
        if (!strcmp(P.opt[i].key, key)) return P.opt[i].val[0] ? P.opt[i].val : (P.opt[i].def[0] ? P.opt[i].def : NULL);
    return NULL;
}

/* ---- environment ------------------------------------------------------------ */

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
    static const char *names[] = { "debug", "info", "warn", "error" };
    va_list ap;
    if (level < RETRO_LOG_INFO && !getenv("PSPEXP_RETRO_DEBUG")) return;
    fprintf(stderr, "[core %s] ", names[level <= RETRO_LOG_ERROR ? level : RETRO_LOG_ERROR]);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

static void set_geometry(const struct retro_game_geometry *g)
{
    P.aspect = g->aspect_ratio > 0.0f ? g->aspect_ratio : (float)g->base_width / (float)g->base_height;
    if (P.ren && g->base_width && g->base_height) {
        int lh = (int)g->base_height;
        int lw = P.square_pixels ? (int)g->base_width : (int)(lh * P.aspect + 0.5f);
        SDL_RenderSetLogicalSize(P.ren, lw, lh);
    }
}

static bool environment(unsigned cmd, void *data)
{
    switch (cmd & ~(unsigned)RETRO_ENVIRONMENT_EXPERIMENTAL) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
        if (f != RETRO_PIXEL_FORMAT_0RGB1555 && f != RETRO_PIXEL_FORMAT_RGB565 && f != RETRO_PIXEL_FORMAT_XRGB8888) return false;
        P.pixfmt = f;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char **)data = P.system_dir; return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = P.save_dir; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = core_log; return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES: opt_defaults((const struct retro_variable *)data); return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = (struct retro_variable *)data;
        v->value = v->key ? opt_get(v->key) : NULL;
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 0; return true;   /* cores fall back to SET_VARIABLES */
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE: *(unsigned *)data = RETRO_LANGUAGE_ENGLISH; return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: if (data) *(int *)data = 3; return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING: if (data) *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: set_geometry((const struct retro_game_geometry *)data); return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        P.av = *(const struct retro_system_av_info *)data;
        set_geometry(&P.av.geometry);
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        const struct retro_message *m = (const struct retro_message *)data;
        if (m && m->msg) fprintf(stderr, "[core] %s\n", m->msg);
        return true;
    }
    /* accepted and not needed: the launcher is the menu, there is one pad */
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
        return true;
    default:
        return false;   /* including SET_HW_RENDER: software cores only */
    }
}

/* ---- video ------------------------------------------------------------------ */

static void make_texture(int w, int h)
{
    switch (P.pixfmt) {
    case RETRO_PIXEL_FORMAT_RGB565:   P.sdl_format = SDL_PIXELFORMAT_RGB565; break;
    case RETRO_PIXEL_FORMAT_XRGB8888: P.sdl_format = SDL_PIXELFORMAT_ARGB8888; break;   /* opaque below: X is ignored */
    default:                          P.sdl_format = SDL_PIXELFORMAT_RGB555; break;
    }
    if (P.tex) SDL_DestroyTexture(P.tex);
    P.tex = SDL_CreateTexture(P.ren, P.sdl_format, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!P.tex) die("texture %dx%d: %s", w, h, SDL_GetError());
    SDL_SetTextureBlendMode(P.tex, SDL_BLENDMODE_NONE);
    P.tex_w = w; P.tex_h = h;
}

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    SDL_Rect r;
    if (!data || data == RETRO_HW_FRAME_BUFFER_VALID) return;   /* a dupe: the texture already holds it */
    if ((int)width > P.tex_w || (int)height > P.tex_h) make_texture((int)width, (int)height);
    r.x = 0; r.y = 0; r.w = (int)width; r.h = (int)height;
    SDL_UpdateTexture(P.tex, &r, data, (int)pitch);
    P.fb_w = (int)width; P.fb_h = (int)height;
}

static void present(void)
{
    int lw, lh;
    SDL_Rect src = { 0, 0, P.fb_w, P.fb_h };
    SDL_SetRenderDrawColor(P.ren, 0, 0, 0, 255);
    SDL_RenderClear(P.ren);
    SDL_RenderGetLogicalSize(P.ren, &lw, &lh);
    if (P.fb_w && P.fb_h) {
        SDL_Rect dst = { 0, 0, lw ? lw : P.fb_w, lh ? lh : P.fb_h };
        SDL_RenderCopy(P.ren, P.tex, &src, &dst);
    }
    SDL_RenderPresent(P.ren);
}

/* ---- audio ------------------------------------------------------------------ */

static void audio_callback(void *u, Uint8 *stream, int len)
{
    int16_t *out = (int16_t *)stream;
    unsigned frames = (unsigned)len / 4, i;
    (void)u;
    for (i = 0; i < frames; i++) {
        if (P.ring_fill == 0) { memset(out + i * 2, 0, (size_t)(frames - i) * 4); P.underruns++; break; }
        out[i * 2] = P.ring[P.ring_tail * 2];
        out[i * 2 + 1] = P.ring[P.ring_tail * 2 + 1];
        P.ring_tail = (P.ring_tail + 1) % P.ring_cap;
        P.ring_fill--;
    }
}

#define OUT_RATE 44100

/* the core's frames, resampled to OUT_RATE, into the ring */
static size_t audio_batch(const int16_t *data, size_t frames)
{
    size_t i;
    if (!P.adev) return frames;
    SDL_LockAudioDevice(P.adev);
    for (i = 0; i < frames; i++) {
        int32_t l = data[i * 2], r = data[i * 2 + 1];
        while (P.frac < 65536u) {
            int32_t f = (int32_t)P.frac;
            if (P.ring_fill == P.ring_cap) break;   /* full: drop (the rate control brings it back) */
            P.ring[P.ring_head * 2] = (int16_t)(P.last[0] + (((l - P.last[0]) * f) >> 16));
            P.ring[P.ring_head * 2 + 1] = (int16_t)(P.last[1] + (((r - P.last[1]) * f) >> 16));
            P.ring_head = (P.ring_head + 1) % P.ring_cap;
            P.ring_fill++;
            P.frac += P.step;
        }
        P.frac -= P.frac >= 65536u ? 65536u : P.frac;
        P.last[0] = (int16_t)l; P.last[1] = (int16_t)r;
    }
    SDL_UnlockAudioDevice(P.adev);
    return frames;
}

static void audio_sample(int16_t l, int16_t r)
{
    int16_t s[2] = { l, r };
    audio_batch(s, 1);
}

static void audio_open(double rate)
{
    SDL_AudioSpec want, got;
    SDL_zero(want);
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = audio_callback;
    P.adev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!P.adev) { fprintf(stderr, "pspexp-retro: no audio: %s\n", SDL_GetError()); return; }
    P.ring_cap = OUT_RATE / 4;   /* 250 ms */
    P.step = (uint32_t)(rate / OUT_RATE * 65536.0 + 0.5);
    P.ring = (int16_t *)calloc(P.ring_cap, 4);
    if (!P.ring) { SDL_CloseAudioDevice(P.adev); P.adev = 0; return; }
    SDL_PauseAudioDevice(P.adev, 0);
}

/* audio frames buffered, in video frames */
static double audio_backlog(void)
{
    unsigned fill;
    if (!P.adev) return 3.0;
    SDL_LockAudioDevice(P.adev);
    fill = P.ring_fill;
    SDL_UnlockAudioDevice(P.adev);
    return (double)fill / (OUT_RATE / P.fps);
}

/* nudge the resampling ratio towards a steady backlog of 3 video frames
 * (about 50 ms): up to +-0.5 %, far below what the ear notices */
static void audio_rate_control(double backlog)
{
    double adj = (backlog - 3.0) / 3.0 * 0.005, rate = P.av.timing.sample_rate > 0 ? P.av.timing.sample_rate : OUT_RATE;
    if (adj > 0.005) adj = 0.005;
    if (adj < -0.005) adj = -0.005;
    P.step = (uint32_t)(rate * (1.0 + adj) / OUT_RATE * 65536.0 + 0.5);
}

/* ---- input ------------------------------------------------------------------ */

static void input_poll(void)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint16_t b = 0;
    if (k[SDL_SCANCODE_UP]) b |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
    if (k[SDL_SCANCODE_DOWN]) b |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (k[SDL_SCANCODE_LEFT]) b |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) b |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if (k[SDL_SCANCODE_X]) b |= 1u << RETRO_DEVICE_ID_JOYPAD_A;
    if (k[SDL_SCANCODE_Z]) b |= 1u << RETRO_DEVICE_ID_JOYPAD_B;
    if (k[SDL_SCANCODE_S]) b |= 1u << RETRO_DEVICE_ID_JOYPAD_X;
    if (k[SDL_SCANCODE_A]) b |= 1u << RETRO_DEVICE_ID_JOYPAD_Y;
    if (k[SDL_SCANCODE_Q]) b |= 1u << RETRO_DEVICE_ID_JOYPAD_L;
    if (k[SDL_SCANCODE_W]) b |= 1u << RETRO_DEVICE_ID_JOYPAD_R;
    if (k[SDL_SCANCODE_RETURN]) b |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
    if (k[SDL_SCANCODE_RSHIFT]) b |= 1u << RETRO_DEVICE_ID_JOYPAD_SELECT;
    if (P.pad) {
        static const struct { SDL_GameControllerButton sdl; unsigned id; } map[] = {
            /* SDL names positions (A = bottom); the RetroPad names Nintendo's (B = bottom) */
            { SDL_CONTROLLER_BUTTON_A, RETRO_DEVICE_ID_JOYPAD_B }, { SDL_CONTROLLER_BUTTON_B, RETRO_DEVICE_ID_JOYPAD_A },
            { SDL_CONTROLLER_BUTTON_X, RETRO_DEVICE_ID_JOYPAD_Y }, { SDL_CONTROLLER_BUTTON_Y, RETRO_DEVICE_ID_JOYPAD_X },
            { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, RETRO_DEVICE_ID_JOYPAD_L }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, RETRO_DEVICE_ID_JOYPAD_R },
            { SDL_CONTROLLER_BUTTON_START, RETRO_DEVICE_ID_JOYPAD_START }, { SDL_CONTROLLER_BUTTON_BACK, RETRO_DEVICE_ID_JOYPAD_SELECT },
            { SDL_CONTROLLER_BUTTON_DPAD_UP, RETRO_DEVICE_ID_JOYPAD_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN },
            { SDL_CONTROLLER_BUTTON_DPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT },
        };
        size_t i;
        int ax = SDL_GameControllerGetAxis(P.pad, SDL_CONTROLLER_AXIS_LEFTX);
        int ay = SDL_GameControllerGetAxis(P.pad, SDL_CONTROLLER_AXIS_LEFTY);
        for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (SDL_GameControllerGetButton(P.pad, map[i].sdl)) b |= 1u << map[i].id;
        /* the nub is the d-pad (these machines have no analogue stick) */
        if (ax < -16000) b |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
        if (ax > 16000) b |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
        if (ay < -16000) b |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
        if (ay > 16000) b |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
    }
    P.buttons = b;
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port != 0 || (device & RETRO_DEVICE_MASK) != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)P.buttons;
    return id < 16 ? (int16_t)((P.buttons >> id) & 1u) : 0;
}

static void pad_open(void)
{
    int i;
    if (P.pad) return;
    for (i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && (P.pad = SDL_GameControllerOpen(i))) {
            fprintf(stderr, "pspexp-retro: controller \"%s\"\n", SDL_GameControllerName(P.pad));
            return;
        }
}

/* ---- battery saves ------------------------------------------------------------ */

static void mkdirs(const char *path)
{
    char b[PATH_MAX], *s;
    snprintf(b, sizeof(b), "%s", path);
    for (s = b + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(b, 0755); *s = '/'; }
    mkdir(b, 0755);
}

static uint8_t *g_srm_saved; static size_t g_srm_saved_n;

static void srm_load(void)
{
    size_t n = C.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    void *mem = C.get_memory_data(RETRO_MEMORY_SAVE_RAM);
    FILE *f;
    if (!n || !mem) return;
    if ((f = fopen(P.srm_path, "rb"))) {
        size_t got = fread(mem, 1, n, f);
        fclose(f);
        fprintf(stderr, "pspexp-retro: battery save %s (%zu bytes)\n", P.srm_path, got);
    }
    g_srm_saved = (uint8_t *)malloc(n);
    if (g_srm_saved) { memcpy(g_srm_saved, mem, n); g_srm_saved_n = n; }
}

static void srm_save(void)
{
    size_t n = C.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    void *mem = C.get_memory_data(RETRO_MEMORY_SAVE_RAM);
    char tmp[PATH_MAX + 8];
    FILE *f;
    if (!n || !mem || !g_srm_saved || n != g_srm_saved_n || !memcmp(mem, g_srm_saved, n)) return;
    mkdirs(P.save_dir);
    snprintf(tmp, sizeof(tmp), "%s.new", P.srm_path);
    if (!(f = fopen(tmp, "wb"))) { fprintf(stderr, "pspexp-retro: %s: %s\n", tmp, strerror(errno)); return; }
    if (fwrite(mem, 1, n, f) == n && fclose(f) == 0 && rename(tmp, P.srm_path) == 0) memcpy(g_srm_saved, mem, n);
    else remove(tmp);
}

/* ---- main --------------------------------------------------------------------- */

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || !(buf = (uint8_t *)malloc(sz ? (size_t)sz : 1))) { fclose(f); return NULL; }
    *n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    struct retro_system_info si;
    struct retro_game_info gi;
    const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
    const char *rom, *base;
    char core_path[PATH_MAX] = "";
    uint8_t *data = NULL;
    Uint64 freq, next, now;
    Uint32 last_save;
    int w, h;

    if (argc != 3) { fprintf(stderr, "usage: pspexp-retro CORE ROM\n"); return 2; }
    rom = argv[2];
    base = strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1];
    snprintf(P.core_name, sizeof(P.core_name), "%.*s", (int)strcspn(base, "."), base);
    { char *s = strstr(P.core_name, "_libretro"); if (s) *s = 0; }
#ifdef PSPEXP_RETRO_STATIC
    core_bind(argv[1]);
#else
    core_find(argv[1], argv[0], core_path, sizeof(core_path));
    core_bind(core_path);
#endif
    if (C.api_version() != RETRO_API_VERSION) die("%s: libretro API %u, the player speaks %u", argv[1], C.api_version(), RETRO_API_VERSION);

    /* directories: BIOS files in ~/.config/pspexp-retro/system, saves apart */
    snprintf(P.system_dir, sizeof(P.system_dir), "%s", getenv("PSPEXP_RETRO_SYSTEM") ? getenv("PSPEXP_RETRO_SYSTEM") : "");
    if (!P.system_dir[0]) snprintf(P.system_dir, sizeof(P.system_dir), "%s/.config/pspexp-retro/system", home);
    snprintf(P.save_dir, sizeof(P.save_dir), "%s", getenv("PSPEXP_RETRO_SAVES") ? getenv("PSPEXP_RETRO_SAVES") : "");
    if (!P.save_dir[0]) snprintf(P.save_dir, sizeof(P.save_dir), "%s/.local/share/pspexp-retro/saves", home);
    mkdirs(P.system_dir);
    {
        const char *rb = strrchr(rom, '/') ? strrchr(rom, '/') + 1 : rom;
        const char *dot = strrchr(rb, '.');
        snprintf(P.srm_path, sizeof(P.srm_path), "%s/%.*s.srm", P.save_dir, (int)(dot ? dot - rb : (long)strlen(rb)), rb);
    }
    P.square_pixels = getenv("PSPEXP_RETRO_ASPECT") && !strcmp(getenv("PSPEXP_RETRO_ASPECT"), "pixel");
    P.pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;   /* the libretro default */

    opt_load_overrides();
    C.set_environment(environment);
    C.init();
    opt_load_overrides();   /* again: SET_VARIABLES may have come during init */
    C.set_video_refresh(video_refresh);
    C.set_audio_sample(audio_sample);
    C.set_audio_sample_batch(audio_batch);
    C.set_input_poll(input_poll);
    C.set_input_state(input_state);

    memset(&si, 0, sizeof(si));
    C.get_system_info(&si);
    fprintf(stderr, "pspexp-retro: %s %s (%s)\n", si.library_name ? si.library_name : "?", si.library_version ? si.library_version : "",
            core_path[0] ? core_path : "linked in");
    memset(&gi, 0, sizeof(gi));
    gi.path = rom;
    if (!si.need_fullpath) {
        size_t n = 0;
        if (!(data = read_file(rom, &n))) die("%s: %s", rom, strerror(errno));
        gi.data = data; gi.size = n;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) die("SDL: %s", SDL_GetError());
    if (!C.load_game(&gi)) die("%s: the core could not load it", rom);
    C.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    C.get_system_av_info(&P.av);
    P.fps = P.av.timing.fps > 1.0 ? P.av.timing.fps : 60.0;
    w = (int)P.av.geometry.base_width; h = (int)P.av.geometry.base_height;
    fprintf(stderr, "pspexp-retro: %dx%d (max %ux%u), %.3f fps, %.0f Hz audio\n", w, h,
            P.av.geometry.max_width, P.av.geometry.max_height, P.fps, P.av.timing.sample_rate);

    P.win = SDL_CreateWindow("pspexp-retro", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             (int)(h * 2 * (P.av.geometry.aspect_ratio > 0 ? P.av.geometry.aspect_ratio : (float)w / h)), h * 2,
                             SDL_WINDOW_RESIZABLE);
    if (!P.win) die("window: %s", SDL_GetError());
    P.ren = SDL_CreateRenderer(P.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!P.ren) P.ren = SDL_CreateRenderer(P.win, -1, 0);
    if (!P.ren) die("renderer: %s", SDL_GetError());
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    set_geometry(&P.av.geometry);
    make_texture((int)(P.av.geometry.max_width ? P.av.geometry.max_width : (unsigned)w),
                 (int)(P.av.geometry.max_height ? P.av.geometry.max_height : (unsigned)h));
    audio_open(P.av.timing.sample_rate > 0 ? P.av.timing.sample_rate : 44100);
    pad_open();
    srm_load();

    /* One core frame per tick of the core's clock. Presenting may wait (the
     * SDL layer paces on the PSP's display, or vsync); the resampling ratio
     * absorbs the small difference, and only a real stall (the backlog
     * nearly empty) costs an extra, unshown core frame. */
    freq = SDL_GetPerformanceFrequency();
    next = SDL_GetPerformanceCounter();
    last_save = SDL_GetTicks();
    P.running = 1;
    while (P.running) {
        SDL_Event e;
        double backlog;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) P.running = 0;
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) P.running = 0;
            else if (e.type == SDL_CONTROLLERDEVICEADDED) pad_open();
            else if (e.type == SDL_CONTROLLERDEVICEREMOVED && P.pad && !SDL_GameControllerGetAttached(P.pad)) { SDL_GameControllerClose(P.pad); P.pad = NULL; }
        }
        C.run();
        backlog = audio_backlog();
        if (backlog < 0.5) { C.run(); backlog = audio_backlog(); }   /* a stall: catch up one frame (not shown) */
        audio_rate_control(backlog);
        present();
        next += (Uint64)((double)freq / P.fps);
        now = SDL_GetPerformanceCounter();
        if (now < next) SDL_Delay((Uint32)((next - now) * 1000 / freq));
        else if (now - next > freq / 4) next = now;   /* fell far behind (a stall): do not race to catch up */
        if (SDL_GetTicks() - last_save > 10000) { srm_save(); last_save = SDL_GetTicks(); }
    }

    srm_save();
    if (P.adev) SDL_CloseAudioDevice(P.adev);
    C.unload_game();
    C.deinit();
    if (P.underruns) fprintf(stderr, "pspexp-retro: %u audio underruns\n", P.underruns);
    free(data);
    SDL_Quit();
    return 0;
}
