#include "pspexp_null.h"
#include "../libpspexp/pspexp_time.h"
#include "../../runtime/pspexp_fs_posix.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct PspExpNullExecutor {
    PspExpRuntime rt;
    PspExpTransport transport;
    PspExpNullStats stats;
    size_t out_off;          /* bytes of the pending output already handed out */
    int paced;
    uint64_t next_vblank_us;
    PspExpMixer *mixer;
};

static uint32_t op_now(void *u) { (void)u; return (uint32_t)pspexp_time_us(); }
static int op_texture_create(void *u, PspExpCacheEntry *e, const void *pixels)
{
    (void)e; (void)pixels;
    ((PspExpNullExecutor *)u)->stats.textures_created++;
    return PSPEXP_RERR_NONE;
}
static void op_texture_destroy(void *u, PspExpCacheEntry *e) { (void)e; ((PspExpNullExecutor *)u)->stats.textures_destroyed++; }
static void op_frame_begin(void *u, uint32_t seq) { (void)seq; ((PspExpNullExecutor *)u)->stats.frames++; }
static void op_set_clip(void *u, int16_t x, int16_t y, int16_t w, int16_t h) { (void)x; (void)y; (void)w; (void)h; ((PspExpNullExecutor *)u)->stats.clips++; }
static int op_palette_create(void *u, PspExpCacheEntry *e, const void *entries)
{
    (void)e; (void)entries;
    ((PspExpNullExecutor *)u)->stats.palettes_created++;
    return PSPEXP_RERR_NONE;
}
static void op_palette_destroy(void *u, PspExpCacheEntry *e) { (void)u; (void)e; }
static void op_draw_sprites(void *u, PspExpCacheEntry *atlas, PspExpCacheEntry *palette, uint16_t fmt, const PspExpVertex2D *v, uint32_t n)
{
    PspExpNullExecutor *x = (PspExpNullExecutor *)u;
    (void)atlas;
    if (palette) x->stats.indexed_draws++;
    if (fmt == PSPEXP_VTX_SPRITE_2D_4444) x->stats.alpha_draws++;
    if (n && v[0].u > v[1].u) x->stats.flipped_draws++;
    x->stats.sprite_draws++;
    x->stats.sprites += n;
    x->stats.vertices_expanded += 2 * n;
}
static void op_draw_rect(void *u, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c, uint8_t a) { (void)x; (void)y; (void)w; (void)h; (void)c; (void)a; ((PspExpNullExecutor *)u)->stats.rects++; }
static void op_draw_line(void *u, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t c, uint8_t a) { (void)x0; (void)y0; (void)x1; (void)y1; (void)c; (void)a; ((PspExpNullExecutor *)u)->stats.lines++; }
static uint32_t op_present(void *u)
{
    PspExpNullExecutor *x = (PspExpNullExecutor *)u;
    uint64_t now = pspexp_time_us();
    if (x->paced) {
        const uint64_t period = 1000000u / PSPEXP_DISPLAY_REFRESH;
        if (x->next_vblank_us <= now) x->next_vblank_us = now + period;
        else pspexp_sleep_us((uint32_t)(x->next_vblank_us - now));
        now = x->next_vblank_us;
        x->next_vblank_us += period;
    }
    return (uint32_t)now;
}
/* 3D: counted (a check of the messages, not a renderer) */
static void op_depth_buffer(void *u, int on) { ((PspExpNullExecutor *)u)->stats.depth_buffer = on; }
static void op_state3d(void *u, const PspExp3DState *s) { (void)s; ((PspExpNullExecutor *)u)->stats.states3d++; }
static void op_matrices3d(void *u, const PspExp3DMatrices *m) { (void)m; ((PspExpNullExecutor *)u)->stats.matrices3d++; }
static void op_lights3d(void *u, const PspExp3DLights *l) { (void)l; ((PspExpNullExecutor *)u)->stats.lights3d++; }
static void op_clear3d(void *u, const PspExp3DClear *c) { (void)c; ((PspExpNullExecutor *)u)->stats.clears3d++; }
static void op_morph3d(void *u, const PspExp3DMorph *m) { (void)u; (void)m; }
static void op_draw3d(void *u, PspExpCacheEntry *tex, PspExpCacheEntry *pal, uint8_t prim, uint32_t vt, uint32_t count, const void *v)
{
    PspExpNullExecutor *x = (PspExpNullExecutor *)u;
    (void)pal; (void)prim; (void)vt; (void)v;
    x->stats.draws3d++;
    x->stats.vertices3d += count;
    if (tex) x->stats.textured3d++;
}

static void op_draw3d_indexed(void *u, PspExpCacheEntry *tex, PspExpCacheEntry *pal, uint8_t prim, uint32_t vt, uint32_t count,
                              const void *v, uint32_t nidx, const uint16_t *ix)
{
    op_draw3d(u, tex, pal, prim, vt, nidx, v);
    (void)count; (void)ix;
}

static void op_draw3d_mesh(void *u, PspExpCacheEntry *tex, PspExpCacheEntry *pal, uint8_t prim, uint32_t vt, uint32_t count,
                           const void *v, const uint16_t *ix, int inl)
{
    PspExpNullExecutor *x = (PspExpNullExecutor *)u;
    (void)pal; (void)prim; (void)vt; (void)v; (void)ix; (void)inl;
    x->stats.draws3d++;
    x->stats.mesh_draws3d++;
    x->stats.vertices3d += count;
    if (tex) x->stats.textured3d++;
}

static void op_target3d(void *u, PspExpCacheEntry *t) { if (t) ((PspExpNullExecutor *)u)->stats.targets3d++; }

static void op_log(void *u, const char *msg) { (void)u; fprintf(stderr, "null-executor: %s\n", msg); }
static int  op_audio_start(void *u, PspExpMixer *m, uint32_t rate) { (void)rate; ((PspExpNullExecutor *)u)->mixer = m; return 0; }
static void op_audio_stop(void *u) { ((PspExpNullExecutor *)u)->mixer = NULL; }

uint32_t pspexp_null_render_audio(PspExpNullExecutor *x, int16_t *stereo, uint32_t frames)
{
    if (!x->mixer) return 0;
    pspexp_mixer_render(x->mixer, stereo, frames);
    return frames;
}

static int tr_send(PspExpTransport *t, const void *data, size_t len)
{
    PspExpNullExecutor *x = (PspExpNullExecutor *)t->user;
    return pspexp_runtime_feed(&x->rt, data, len);
}

static long tr_recv(PspExpTransport *t, void *buf, size_t cap, int timeout_ms)
{
    PspExpNullExecutor *x = (PspExpNullExecutor *)t->user;
    size_t len, n;
    const uint8_t *out;
    (void)timeout_ms;   /* nothing arrives asynchronously on a loopback */
    out = (const uint8_t *)pspexp_runtime_out_peek(&x->rt, &len);
    if (len <= x->out_off) { x->out_off = 0; pspexp_runtime_out_consume(&x->rt); return 0; }
    n = len - x->out_off;
    if (n > cap) n = cap;
    memcpy(buf, out + x->out_off, n);
    x->out_off += n;
    if (x->out_off >= len) { x->out_off = 0; pspexp_runtime_out_consume(&x->rt); }
    return (long)n;
}

/* a PSP on battery, for the tests: 76%, 2 h 5 min left, 250 mA drawn, 31 C, 4.1 V */
static int op_power_get(void *u, PspExpPowerStatus *p)
{
    (void)u;
    p->flags = PSPEXP_POWER_BATTERY;
    p->percent = 76; p->minutes_left = 125; p->current_ma = -250; p->temp_c = 31; p->voltage_mv = 4100;
    return 0;
}

/* compressed audio, for the tests: an MP3 frame "decodes" to its sample count of 1000s */
static int op_audio_decode(void *u, int codec, const uint8_t *frame, uint32_t bytes, int16_t *stereo, uint32_t max_frames)
{
    uint32_t n = ((frame[1] >> 3) & 3) == 3 ? 1152u : 576u, i;
    (void)u; (void)bytes;
    if (codec != PSPEXP_CODEC_MP3 || n > max_frames) return -1;
    for (i = 0; i < n * 2; i++) stereo[i] = 1000;
    return (int)n;
}

static void tr_close(PspExpTransport *t) { (void)t; }

PspExpNullExecutor *pspexp_null_create(const PspExpRuntimeConfig *cfg)
{
    PspExpNullExecutor *x = (PspExpNullExecutor *)calloc(1, sizeof(*x));
    PspExpRuntimeOps ops;
    PspExpRuntimeConfig c;
    if (!x) return NULL;
    if (cfg) c = *cfg; else pspexp_runtime_default_config(&c, PSPEXP_EXECUTOR_NULL);
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
    ops.depth_buffer = op_depth_buffer;
    ops.state3d = op_state3d;
    ops.matrices3d = op_matrices3d;
    ops.lights3d = op_lights3d;
    ops.draw3d = op_draw3d;
    ops.clear3d = op_clear3d;
    ops.power_get = op_power_get;
    ops.audio_decode = op_audio_decode;
    ops.morph3d = op_morph3d;
    ops.draw3d_mesh = op_draw3d_mesh;
    ops.draw3d_indexed = op_draw3d_indexed;
    ops.target3d = op_target3d;
    ops.log = op_log;
    pspexp_fs_posix_install(&ops, NULL);   /* PSPEXP_MS0: a folder as the memory stick */
    pspexp_clock_posix_install(&ops);      /* a clock CLOCK_SET can move (in this process only) */
    if (pspexp_runtime_init(&x->rt, &ops, &c) < 0) { free(x); return NULL; }
    x->transport.name = "null";
    x->transport.user = x;
    x->transport.send = tr_send;
    x->transport.recv = tr_recv;
    x->transport.close = tr_close;
    pspexp_runtime_connect(&x->rt);
    return x;
}

void pspexp_null_destroy(PspExpNullExecutor *x)
{
    if (!x) return;
    pspexp_runtime_free(&x->rt);
    free(x);
}

PspExpTransport *pspexp_null_transport(PspExpNullExecutor *x) { return &x->transport; }
PspExpRuntime *pspexp_null_runtime(PspExpNullExecutor *x) { return &x->rt; }
const PspExpNullStats *pspexp_null_stats(const PspExpNullExecutor *x) { return &x->stats; }

void pspexp_null_simulate_reconnect(PspExpNullExecutor *x)
{
    x->out_off = 0;
    pspexp_runtime_disconnect(&x->rt);
    pspexp_runtime_connect(&x->rt);
}

void pspexp_null_push_input(PspExpNullExecutor *x, uint32_t buttons, uint8_t ax, uint8_t ay)
{
    pspexp_runtime_push_input(&x->rt, buttons, ax, ay);
}

void pspexp_null_set_paced(PspExpNullExecutor *x, int paced) { x->paced = paced; }
