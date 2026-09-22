#include "pspexp_null.h"
#include "../libpspexp/pspexp_time.h"
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
static void op_draw_sprites(void *u, PspExpCacheEntry *atlas, const PspExpVertex2D *v, uint32_t n)
{
    PspExpNullExecutor *x = (PspExpNullExecutor *)u;
    (void)atlas; (void)v;
    x->stats.sprite_draws++;
    x->stats.sprites += n;
    x->stats.vertices_expanded += 2 * n;
}
static void op_draw_rect(void *u, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; ((PspExpNullExecutor *)u)->stats.rects++; }
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
static void op_log(void *u, const char *msg) { (void)u; fprintf(stderr, "null-executor: %s\n", msg); }

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
    ops.frame_begin = op_frame_begin;
    ops.set_clip = op_set_clip;
    ops.draw_sprites = op_draw_sprites;
    ops.draw_rect = op_draw_rect;
    ops.present = op_present;
    ops.log = op_log;
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
