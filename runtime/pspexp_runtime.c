#include "pspexp_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#if defined(__psp__)
#include <malloc.h>
#endif

#define RUNTIME_VERSION 1

static void rt_log(PspExpRuntime *rt, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    if (!rt->ops.log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    rt->ops.log(rt->ops.user, buf);
}

static uint32_t now_us(PspExpRuntime *rt) { return rt->ops.now_us(rt->ops.user); }

/* 16-byte aligned (the GE reads meshes in place); freed with free(). */
static void *memalign_portable(size_t n)
{
#if defined(__psp__)
    return memalign(16, n);
#else
    void *p = NULL;
    return posix_memalign(&p, 16, n ? n : 16) == 0 ? p : NULL;
#endif
}

void pspexp_runtime_default_config(PspExpRuntimeConfig *cfg, uint8_t executor)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->executor = executor;
    cfg->texture_formats = PSPEXP_TEX_FORMATS_V01;
    cfg->vram_budget = PSPEXP_VRAM_TEXTURE_BUDGET;
    cfg->ram_budget = 8u * 1024u * 1024u;
    cfg->max_batch_records = PSPEXP_BATCH_MAX_RECORDS;
    cfg->input_rate_max = 60;
    cfg->features = PSPEXP_FEATURE_BENCH | PSPEXP_FEATURE_TIMESTAMPS;
}

int pspexp_runtime_init(PspExpRuntime *rt, const PspExpRuntimeOps *ops, const PspExpRuntimeConfig *cfg)
{
    memset(rt, 0, sizeof(*rt));
    rt->ops = *ops;
    rt->cfg = *cfg;
    if (rt->cfg.max_batch_records == 0 || rt->cfg.max_batch_records > PSPEXP_BATCH_MAX_RECORDS)
        rt->cfg.max_batch_records = PSPEXP_BATCH_MAX_RECORDS;
    pspexp_cache_init(&rt->cache, cfg->vram_budget, cfg->ram_budget);
    if (pspexp_stream_init_dynamic(&rt->in, cfg->in_capacity ? cfg->in_capacity : 256u * 1024u) < 0) return -1;
    if (pspexp_writer_init_dynamic(&rt->out, cfg->out_capacity ? cfg->out_capacity : 16u * 1024u) < 0) return -1;
    if (!rt->ops.vertex_alloc) {
        rt->scratch_cap = (size_t)rt->cfg.max_batch_records * 2u * sizeof(PspExpVertex2D) + PSPEXP_ALIGN;
        rt->scratch = (uint8_t *)malloc(rt->scratch_cap);
        if (!rt->scratch) return -1;
    }
    return 0;
}

static void audio_lock(PspExpRuntime *rt) { if (rt->audio_active && rt->ops.audio_lock) rt->ops.audio_lock(rt->ops.user); }
static void audio_unlock(PspExpRuntime *rt) { if (rt->audio_active && rt->ops.audio_unlock) rt->ops.audio_unlock(rt->ops.user); }

static void destroy_entry(PspExpRuntime *rt, PspExpCacheEntry *e)
{
    if (e->kind == PSPEXP_KIND_PALETTE) { if (rt->ops.palette_destroy) rt->ops.palette_destroy(rt->ops.user, e); }
    else if (e->kind == PSPEXP_KIND_MESH) {
        free(e->user);
        e->user = NULL;
    }
    else if (e->kind == PSPEXP_KIND_SOUND) {
        audio_lock(rt);
        if (rt->audio_active) pspexp_mixer_stop_sound(&rt->mixer, e->id);
        audio_unlock(rt);
        free(e->user);
        e->user = NULL;
    }
    else if (rt->ops.texture_destroy) rt->ops.texture_destroy(rt->ops.user, e);
    if (e == rt->tex3d || e == rt->pal3d) rt->tex3d = rt->pal3d = NULL;   /* looked up again at the next draw */
    pspexp_cache_remove(&rt->cache, e);
}

static void audio_shutdown(PspExpRuntime *rt)
{
    if (!rt->audio_active) return;
    if (rt->ops.audio_stop) rt->ops.audio_stop(rt->ops.user);
    rt->audio_active = 0;
    pspexp_mixer_free(&rt->mixer);
}

static void release_all(PspExpRuntime *rt)
{
    int i;
    for (i = 0; i < PSPEXP_CACHE_MAX; i++) {
        PspExpCacheEntry *e = &rt->cache.entries[i];
        if (!e->used) continue;
        destroy_entry(rt, e);
    }
    audio_shutdown(rt);
    rt->connected = 0;
    rt->in_frame = 0;
    rt->tex3d = rt->pal3d = NULL;
    rt->tex3d_on = 0;
    rt->bench_active = 0;
    rt->uplink_remaining = 0;
    pspexp_stream_reset(&rt->in);
    pspexp_writer_reset(&rt->out);
}

void pspexp_runtime_free(PspExpRuntime *rt)
{
    release_all(rt);
    pspexp_stream_free(&rt->in);
    pspexp_writer_free(&rt->out);
    free(rt->scratch);
    free(rt->fs_buf); rt->fs_buf = NULL;
    rt->scratch = NULL;
}

static void send_hello(PspExpRuntime *rt)
{
    PspExpHello h;
    memset(&h, 0, sizeof(h));
    h.protocol = PSPEXP_PROTOCOL_VERSION;
    h.runtime_version = RUNTIME_VERSION;
    h.display_width = PSPEXP_DISPLAY_WIDTH;
    h.display_height = PSPEXP_DISPLAY_HEIGHT;
    h.refresh_hz = PSPEXP_DISPLAY_REFRESH;
    h.audio_channels = 8;
    h.input_rate_max = rt->cfg.input_rate_max;
    h.executor = rt->cfg.executor;
    h.texture_formats = rt->cfg.texture_formats;
    h.vertex_formats = PSPEXP_VTX_BIT(PSPEXP_VTX_SPRITE_2D_5650) | PSPEXP_VTX_BIT(PSPEXP_VTX_SPRITE_2D_4444);
    h.texture_max = PSPEXP_TEXTURE_MAX;
    h.max_batch_records = rt->cfg.max_batch_records;
    h.vram_budget = rt->cache.vram_budget;
    h.ram_budget = rt->cache.ram_budget;
    h.features = rt->cfg.features | (rt->ops.fs_list ? PSPEXP_FEATURE_FILES : 0) |
                 (rt->ops.clock_get && rt->ops.clock_set ? PSPEXP_FEATURE_CLOCK : 0) |
                 (rt->ops.draw3d ? PSPEXP_FEATURE_3D | PSPEXP_FEATURE_3D16 : 0) | (rt->ops.texture_patch ? PSPEXP_FEATURE_PATCH : 0) |
                 (rt->ops.draw3d && rt->ops.draw3d_indexed ? PSPEXP_FEATURE_3D_INDEXED : 0) |
                 (rt->ops.draw3d && rt->ops.morph3d ? PSPEXP_FEATURE_3D_MORPH : 0) |
                 (rt->ops.draw3d ? PSPEXP_FEATURE_3D_SPRITES : 0);
    h.audio_rate_min = 8000;
    h.audio_rate_max = 48000;
    h.usb_chunk = rt->cfg.usb_chunk;
    pspexp_writer_message(&rt->out, PSPEXP_CH_SYSTEM, PSPEXP_OP_HELLO, 0, &h, sizeof(h));
}

void pspexp_runtime_connect(PspExpRuntime *rt)
{
    release_all(rt);
    send_hello(rt);
}

void pspexp_runtime_disconnect(PspExpRuntime *rt)
{
    release_all(rt);
}

static void send_error(PspExpRuntime *rt, uint16_t code, const PspExpMessage *m, uint32_t detail)
{
    PspExpError e;
    memset(&e, 0, sizeof(e));
    e.code = code;
    e.channel = m->header->channel;
    e.opcode = m->header->opcode;
    e.sequence = m->header->sequence;
    e.detail = detail;
    rt->stats.protocol_errors++;
    rt_log(rt, "protocol error %u on %s seq %u", code, pspexp_opcode_name(m->header->opcode), m->header->sequence);
    pspexp_writer_message(&rt->out, PSPEXP_CH_SYSTEM, PSPEXP_OP_ERROR, 0, &e, sizeof(e));
}

static void send_resource_error(PspExpRuntime *rt, uint16_t opcode, uint16_t id, uint16_t reason, uint32_t frame_seq)
{
    PspExpResourceError e;
    memset(&e, 0, sizeof(e));
    e.id = id;
    e.reason = reason;
    e.frame_seq = frame_seq;
    if (opcode == PSPEXP_OP_RESOURCE_MISS) rt->stats.misses++; else rt->stats.resource_errors++;
    rt_log(rt, "%s id %u: %s", pspexp_opcode_name(opcode), id, pspexp_resource_error_name(reason));
    pspexp_writer_message(&rt->out, PSPEXP_CH_RESOURCE, opcode, 0, &e, sizeof(e));
}

static void send_ready(PspExpRuntime *rt, uint16_t opcode, uint16_t id, uint8_t tier)
{
    PspExpResourceReady r;
    memset(&r, 0, sizeof(r));
    r.id = id;
    r.tier = tier;
    pspexp_writer_message(&rt->out, PSPEXP_CH_RESOURCE, opcode, 0, &r, sizeof(r));
}

static PspExpVertex2D *vertex_space(PspExpRuntime *rt, uint32_t vertex_count)
{
    if (rt->ops.vertex_alloc) return rt->ops.vertex_alloc(rt->ops.user, vertex_count);
    return (PspExpVertex2D *)(((uintptr_t)rt->scratch + PSPEXP_ALIGN - 1) & ~(uintptr_t)(PSPEXP_ALIGN - 1));
}

static void handle_upload(PspExpRuntime *rt, const PspExpMessage *m)
{
    const PspExpTextureUpload *t;
    const PspExpFrame *frames;
    PspExpCacheEntry *e;
    int reason;

    if (pspexp_message_expect(m, sizeof(*t)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
    t = (const PspExpTextureUpload *)m->payload;
    reason = pspexp_texture_validate(t, rt->cfg.texture_formats, m->length - (uint32_t)sizeof(*t));
    if (reason != PSPEXP_RERR_NONE) { send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, t->id, (uint16_t)reason, 0); return; }
    if (PSPEXP_TEX_IS_INDEXED(t->format)) {
        const PspExpCacheEntry *pal = pspexp_cache_find(&rt->cache, t->palette);
        uint16_t need = t->format == PSPEXP_TEX_T4 ? PSPEXP_PALETTE_ENTRIES_T4 : PSPEXP_PALETTE_ENTRIES_T8;
        if (!pal || pal->kind != PSPEXP_KIND_PALETTE || pal->width < need) {
            send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, t->id, PSPEXP_RERR_BAD_PALETTE, 0);
            return;
        }
    }
    frames = (const PspExpFrame *)pspexp_texture_frames(t, m->payload + sizeof(*t));
    e = pspexp_cache_insert(&rt->cache, t, frames, &reason);
    if (!e) { send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, t->id, (uint16_t)reason, 0); return; }
    if (rt->ops.texture_create) {
        reason = rt->ops.texture_create(rt->ops.user, e, m->payload + sizeof(*t));
        if (reason != PSPEXP_RERR_NONE) {
            pspexp_cache_remove(&rt->cache, e);
            send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, t->id, (uint16_t)reason, 0);
            return;
        }
    }
    rt->stats.uploads++;
    send_ready(rt, PSPEXP_OP_RESOURCE_READY, e->id, e->tier);
}

static void handle_palette_upload(PspExpRuntime *rt, const PspExpMessage *m)
{
    const PspExpPaletteUpload *p;
    PspExpCacheEntry *e;
    int reason;
    uint32_t bytes;
    if (pspexp_message_expect(m, sizeof(*p)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
    p = (const PspExpPaletteUpload *)m->payload;
    reason = pspexp_palette_validate(p, m->length - (uint32_t)sizeof(*p));
    if (reason != PSPEXP_RERR_NONE) { send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, p->id, (uint16_t)reason, 0); return; }
    bytes = pspexp_palette_data_length(p->format, p->entries);
    e = pspexp_cache_insert_palette(&rt->cache, p, bytes, &reason);
    if (!e) { send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, p->id, (uint16_t)reason, 0); return; }
    if (rt->ops.palette_create) {
        reason = rt->ops.palette_create(rt->ops.user, e, m->payload + sizeof(*p));
        if (reason != PSPEXP_RERR_NONE) {
            pspexp_cache_remove(&rt->cache, e);
            send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, p->id, (uint16_t)reason, 0);
            return;
        }
    }
    rt->stats.palettes++;
    send_ready(rt, PSPEXP_OP_RESOURCE_READY, e->id, e->tier);
}

/* The atlas for a batch, or NULL after reporting why. `*palette` receives the
 * palette for an indexed atlas. */
static PspExpCacheEntry *batch_atlas(PspExpRuntime *rt, uint16_t id, uint16_t palette_override, PspExpCacheEntry **palette)
{
    PspExpCacheEntry *atlas = pspexp_cache_find(&rt->cache, id);
    *palette = NULL;
    if (!atlas) { send_resource_error(rt, PSPEXP_OP_RESOURCE_MISS, id, PSPEXP_RERR_UNKNOWN_ID, rt->frame_seq); return NULL; }
    if (atlas->kind != PSPEXP_KIND_TEXTURE) { send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, id, PSPEXP_RERR_WRONG_KIND, rt->frame_seq); return NULL; }
    if (PSPEXP_TEX_IS_INDEXED(atlas->format)) {
        uint16_t pid = palette_override != PSPEXP_PALETTE_DEFAULT ? palette_override : atlas->palette;
        uint16_t need = atlas->format == PSPEXP_TEX_T4 ? PSPEXP_PALETTE_ENTRIES_T4 : PSPEXP_PALETTE_ENTRIES_T8;
        PspExpCacheEntry *pal = pspexp_cache_find(&rt->cache, pid);
        if (!pal || pal->kind != PSPEXP_KIND_PALETTE) {
            send_resource_error(rt, PSPEXP_OP_RESOURCE_MISS, pid, PSPEXP_RERR_UNKNOWN_ID, rt->frame_seq);
            return NULL;
        }
        if (pal->width < need) {
            send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, pid, PSPEXP_RERR_BAD_PALETTE, rt->frame_seq);
            return NULL;
        }
        *palette = pal;
    }
    return atlas;
}

static int vertex_format_ok(PspExpRuntime *rt, const PspExpMessage *m, uint16_t fmt)
{
    if (fmt >= PSPEXP_VTX_COUNT) { send_error(rt, PSPEXP_ERR_BAD_VERTEX_FMT, m, fmt); return 0; }
    return 1;
}

static void handle_sprite_batch(PspExpRuntime *rt, const PspExpMessage *m)
{
    const PspExpSpriteBatch *b;
    const PspExpSpriteRec *recs;
    PspExpCacheEntry *atlas, *palette;
    PspExpVertex2D *v;

    if (pspexp_message_expect(m, sizeof(*b)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
    b = (const PspExpSpriteBatch *)m->payload;
    if (b->count > rt->cfg.max_batch_records) { send_error(rt, PSPEXP_ERR_BATCH_TOO_LARGE, m, b->count); return; }
    if (m->length < sizeof(*b) + (uint32_t)b->count * sizeof(PspExpSpriteRec)) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
    if (!vertex_format_ok(rt, m, b->vertex_format)) return;
    recs = (const PspExpSpriteRec *)(m->payload + sizeof(*b));
    atlas = batch_atlas(rt, b->atlas, b->palette, &palette);
    if (!atlas) return;
    if (b->count == 0) return;
    if (pspexp_sprites_max_frame(recs, b->count) >= atlas->frame_count) {
        send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, b->atlas, PSPEXP_RERR_BAD_FRAME_TABLE, rt->frame_seq);
        return;
    }
    v = vertex_space(rt, 2u * b->count);
    if (!v) { rt->stats.frame_dropped += b->count; rt->stats.sprites_dropped += b->count; return; }
    rt->stats.frame_sprites += b->count;
    if (b->flags & PSPEXP_BATCH_FLIP_X) pspexp_expand_sprites_flipped(atlas->frames, recs, b->count, v);
    else pspexp_expand_sprites(atlas->frames, recs, b->count, v);
    if (rt->ops.draw_sprites) rt->ops.draw_sprites(rt->ops.user, atlas, palette, b->vertex_format, v, b->count);
    rt->stats.batches++;
    rt->stats.sprites += b->count;
}

static void handle_quad_batch(PspExpRuntime *rt, const PspExpMessage *m)
{
    const PspExpSpriteBatch *b;
    const PspExpQuadRec *recs;
    const PspExpQuadExRec *recs_ex;
    PspExpCacheEntry *atlas, *palette;
    PspExpVertex2D *v;
    uint32_t i;
    int scaled;

    if (pspexp_message_expect(m, sizeof(*b)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
    b = (const PspExpSpriteBatch *)m->payload;
    scaled = (b->flags & PSPEXP_BATCH_SCALED) != 0;
    if (b->count > rt->cfg.max_batch_records) { send_error(rt, PSPEXP_ERR_BATCH_TOO_LARGE, m, b->count); return; }
    if (m->length < sizeof(*b) + (uint32_t)b->count * (scaled ? sizeof(PspExpQuadExRec) : sizeof(PspExpQuadRec))) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
    if (!vertex_format_ok(rt, m, b->vertex_format)) return;
    recs = (const PspExpQuadRec *)(m->payload + sizeof(*b));
    recs_ex = (const PspExpQuadExRec *)(m->payload + sizeof(*b));
    atlas = batch_atlas(rt, b->atlas, b->palette, &palette);
    if (!atlas) return;
    if (b->count == 0) return;
    for (i = 0; i < b->count; i++) {
        uint16_t u0 = scaled ? recs_ex[i].u0 : recs[i].u0, u1 = scaled ? recs_ex[i].u1 : recs[i].u1;
        uint16_t v0 = scaled ? recs_ex[i].v0 : recs[i].v0, v1 = scaled ? recs_ex[i].v1 : recs[i].v1;
        if (u0 > atlas->width || u1 > atlas->width || v0 > atlas->height || v1 > atlas->height || (!scaled && v0 > v1)) {
            send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, b->atlas, PSPEXP_RERR_BAD_FRAME_TABLE, rt->frame_seq);
            return;
        }
    }
    v = vertex_space(rt, 2u * b->count);
    if (!v) { rt->stats.frame_dropped += b->count; rt->stats.sprites_dropped += b->count; return; }
    rt->stats.frame_sprites += b->count;
    if (scaled) pspexp_expand_quads_ex(recs_ex, b->count, v);
    else pspexp_expand_quads(recs, b->count, v);
    if (rt->ops.draw_sprites) rt->ops.draw_sprites(rt->ops.user, atlas, palette, b->vertex_format, v, b->count);
    rt->stats.batches++;
    rt->stats.sprites += b->count;
}

static void handle_tile_batch(PspExpRuntime *rt, const PspExpMessage *m)
{
    const PspExpTileBatch *b;
    const PspExpTileRec *recs;
    PspExpCacheEntry *atlas, *palette;
    PspExpVertex2D *v;

    if (pspexp_message_expect(m, sizeof(*b)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
    b = (const PspExpTileBatch *)m->payload;
    if (b->count > rt->cfg.max_batch_records) { send_error(rt, PSPEXP_ERR_BATCH_TOO_LARGE, m, b->count); return; }
    if (m->length < sizeof(*b) + (uint32_t)b->count * sizeof(PspExpTileRec)) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
    if (!vertex_format_ok(rt, m, b->vertex_format)) return;
    recs = (const PspExpTileRec *)(m->payload + sizeof(*b));
    atlas = batch_atlas(rt, b->atlas, b->palette, &palette);
    if (!atlas) return;
    if (b->count == 0) return;
    if (pspexp_tiles_max_frame(recs, b->count) >= atlas->frame_count) {
        send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, b->atlas, PSPEXP_RERR_BAD_FRAME_TABLE, rt->frame_seq);
        return;
    }
    v = vertex_space(rt, 2u * b->count);
    if (!v) { rt->stats.frame_dropped += b->count; rt->stats.sprites_dropped += b->count; return; }
    rt->stats.frame_sprites += b->count;
    pspexp_expand_tiles(atlas->frames, b, recs, v);
    if (rt->ops.draw_sprites) rt->ops.draw_sprites(rt->ops.user, atlas, palette, b->vertex_format, v, b->count);
    rt->stats.batches++;
    rt->stats.tiles += b->count;
}

/* ---- 3D --------------------------------------------------------------------- */

static void handle_draw_mesh(PspExpRuntime *rt, const PspExpMessage *m);

static void handle_3d(PspExpRuntime *rt, const PspExpMessage *m)
{
    /* Into a target that is not here (no room for it made): nowhere. Drawn on whatever came
     * before, the screen mostly, it would cover the frame (a clear first, often). */
    if (rt->no_target && (m->header->opcode == PSPEXP_OP_3D_DRAW || m->header->opcode == PSPEXP_OP_3D_DRAW_MESH ||
                          m->header->opcode == PSPEXP_OP_3D_CLEAR))
        return;
    switch (m->header->opcode) {
    case PSPEXP_OP_3D_STATE: {
        const PspExp3DState *s;
        if (pspexp_message_expect(m, sizeof(*s)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        s = (const PspExp3DState *)m->payload;
        rt->tex3d_on = (s->enable & PSPEXP_3D_TEXTURE) != 0;
        if (!rt->tex3d_on || s->texture != rt->tex3d_id) rt->tex3d = rt->pal3d = NULL;
        rt->tex3d_id = s->texture;
        if (rt->ops.state3d) rt->ops.state3d(rt->ops.user, s);
        return;
    }
    case PSPEXP_OP_3D_MATRICES:
        if (pspexp_message_expect(m, sizeof(PspExp3DMatrices)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        if (rt->ops.matrices3d) rt->ops.matrices3d(rt->ops.user, (const PspExp3DMatrices *)m->payload);
        return;
    case PSPEXP_OP_3D_LIGHTS:
        if (pspexp_message_expect(m, sizeof(PspExp3DLights)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        if (rt->ops.lights3d) rt->ops.lights3d(rt->ops.user, (const PspExp3DLights *)m->payload);
        return;
    case PSPEXP_OP_3D_CLEAR:
        if (pspexp_message_expect(m, sizeof(PspExp3DClear)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        if (rt->ops.clear3d) rt->ops.clear3d(rt->ops.user, (const PspExp3DClear *)m->payload);
        return;
    case PSPEXP_OP_3D_DRAW_MESH:
        handle_draw_mesh(rt, m);
        return;
    case PSPEXP_OP_3D_MORPH:
        if (!rt->ops.morph3d) { send_error(rt, PSPEXP_ERR_BAD_OPCODE, m, 0); return; }
        if (pspexp_message_expect(m, sizeof(PspExp3DMorph)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        rt->ops.morph3d(rt->ops.user, (const PspExp3DMorph *)m->payload);
        return;
    case PSPEXP_OP_3D_TARGET: {
        const PspExp3DTarget *t;
        PspExpCacheEntry *e = NULL;
        if (pspexp_message_expect(m, sizeof(*t)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        t = (const PspExp3DTarget *)m->payload;
        if (t->id != PSPEXP_TARGET_SCREEN) {
            e = pspexp_cache_find(&rt->cache, t->id);
            if (!e || !e->target) { send_resource_error(rt, PSPEXP_OP_RESOURCE_MISS, t->id, e ? PSPEXP_RERR_WRONG_KIND : PSPEXP_RERR_UNKNOWN_ID, rt->frame_seq); e = NULL; }
        }
        rt->no_target = !e && t->id != PSPEXP_TARGET_SCREEN;
        if (rt->ops.target3d && (e || t->id == PSPEXP_TARGET_SCREEN)) rt->ops.target3d(rt->ops.user, e);
        return;
    }
    case PSPEXP_OP_3D_DRAW: {
        const PspExp3DDraw *d;
        uint32_t size;
        if (pspexp_message_expect(m, sizeof(*d)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        d = (const PspExp3DDraw *)m->payload;
        size = pspexp_3d_vertex_size(d->vertex_type);
        if (!size || d->primitive > PSPEXP_3D_PRIM_MAX) { send_error(rt, PSPEXP_ERR_BAD_VERTEX_FMT, m, d->vertex_type); return; }
        if (d->count > PSPEXP_3D_MAX_VERTICES) { send_error(rt, PSPEXP_ERR_BATCH_TOO_LARGE, m, d->count); return; }
        if ((uint64_t)d->count * size > m->length - sizeof(*d)) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, d->count); return; }
        if (!d->count) return;
        if (d->index_count) {   /* indexed: every index checked here, once */
            uint32_t vbytes = PSPEXP_ALIGN4(d->count * size), k;
            const uint16_t *ix = (const uint16_t *)(const void *)(m->payload + sizeof(*d) + vbytes);
            if (!rt->ops.draw3d_indexed) { send_error(rt, PSPEXP_ERR_BAD_OPCODE, m, 0); return; }
            if (d->index_count > PSPEXP_3D_MAX_VERTICES || (uint64_t)vbytes + (uint64_t)d->index_count * 2u > m->length - sizeof(*d)) {
                send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, d->index_count); return;
            }
            for (k = 0; k < d->index_count; k++) if (ix[k] >= d->count) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, ix[k]); return; }
            if (rt->tex3d_on && !rt->tex3d) {
                rt->tex3d = batch_atlas(rt, rt->tex3d_id, PSPEXP_PALETTE_DEFAULT, &rt->pal3d);
                if (!rt->tex3d) rt->tex3d_on = 0;
            }
            rt->ops.draw3d_indexed(rt->ops.user, rt->tex3d_on ? rt->tex3d : NULL, rt->tex3d_on ? rt->pal3d : NULL,
                                   d->primitive, d->vertex_type, d->count, m->payload + sizeof(*d), d->index_count, ix);
            rt->stats.draws3d++;
            rt->stats.vertices3d += d->index_count;
            return;
        }
        if (rt->tex3d_on && !rt->tex3d) {
            rt->tex3d = batch_atlas(rt, rt->tex3d_id, PSPEXP_PALETTE_DEFAULT, &rt->pal3d);
            if (!rt->tex3d) rt->tex3d_on = 0;   /* reported once; drawn untextured until the next state */
        }
        if (!rt->stats.draws3d) {   /* the first of a connection, in the log: what the host sends */
            const float *f = (const float *)(const void *)(m->payload + sizeof(*d));
            int k = size / 4 - 3;   /* the position is last */
            rt_log(rt, "3D: first draw: primitive %u, vertex type %x, %u vertices, the first at %d %d %d (x1000)", d->primitive,
                   (unsigned)d->vertex_type, (unsigned)d->count, (int)(f[k] * 1000), (int)(f[k + 1] * 1000), (int)(f[k + 2] * 1000));
        }
        if (rt->ops.draw3d)
            rt->ops.draw3d(rt->ops.user, rt->tex3d_on ? rt->tex3d : NULL, rt->tex3d_on ? rt->pal3d : NULL,
                           d->primitive, d->vertex_type, d->count, m->payload + sizeof(*d));
        rt->stats.draws3d++;
        rt->stats.vertices3d += d->count;
        return;
    }
    default:
        return;
    }
}

static void handle_patch(PspExpRuntime *rt, const PspExpMessage *m)
{
    const PspExpResourcePatch *p;
    PspExpCacheEntry *e;
    if (pspexp_message_expect(m, sizeof(*p)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
    p = (const PspExpResourcePatch *)m->payload;
    if (p->length > m->length - sizeof(*p)) { send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, p->id, PSPEXP_RERR_BAD_LENGTH, 0); return; }
    if (!(e = pspexp_cache_find(&rt->cache, p->id))) { send_resource_error(rt, PSPEXP_OP_RESOURCE_MISS, p->id, PSPEXP_RERR_UNKNOWN_ID, rt->frame_seq); return; }
    if (e->kind != PSPEXP_KIND_TEXTURE || e->target || !e->user) { send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, p->id, PSPEXP_RERR_WRONG_KIND, 0); return; }
    if ((uint64_t)p->offset + p->length > e->bytes) { send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, p->id, PSPEXP_RERR_BAD_LENGTH, 0); return; }
    rt->ops.texture_patch(rt->ops.user, e, p->offset, m->payload + sizeof(*p), p->length);
}

static void handle_target_create(PspExpRuntime *rt, const PspExpMessage *m)
{
    const PspExpTargetCreate *c;
    PspExpTextureUpload t;
    PspExpCacheEntry *e;
    int reason;
    if (pspexp_message_expect(m, sizeof(*c)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
    c = (const PspExpTargetCreate *)m->payload;
    memset(&t, 0, sizeof(t));
    t.id = c->id; t.width = c->width; t.height = c->height; t.format = c->format;
    t.layout = PSPEXP_LAYOUT_LINEAR; t.priority = PSPEXP_PRIORITY_HIGH;
    t.data_length = pspexp_texture_data_length(c->width, c->height, c->format);
    if (c->format > PSPEXP_TEX_8888 || (c->width & (c->width - 1)) || (c->height & (c->height - 1)) ||
        !c->width || !c->height || c->width > PSPEXP_TEXTURE_MAX || c->height > PSPEXP_TEXTURE_MAX) {
        send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, c->id, PSPEXP_RERR_BAD_DIMENSIONS, 0);
        return;
    }
    e = pspexp_cache_insert(&rt->cache, &t, NULL, &reason);
    if (!e) { send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, c->id, (uint16_t)reason, 0); return; }
    if (e->tier != PSPEXP_TIER_VRAM) {   /* the GE draws into VRAM only */
        pspexp_cache_remove(&rt->cache, e);
        send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, c->id, PSPEXP_RERR_OUT_OF_MEMORY, 0);
        return;
    }
    e->target = 1;
    reason = rt->ops.texture_create ? rt->ops.texture_create(rt->ops.user, e, NULL) : PSPEXP_RERR_NONE;
    if (reason != PSPEXP_RERR_NONE || (e->tier != PSPEXP_TIER_VRAM)) {
        destroy_entry(rt, e);
        send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, c->id, PSPEXP_RERR_OUT_OF_MEMORY, 0);
        return;
    }
    rt->stats.uploads++;
    send_ready(rt, PSPEXP_OP_RESOURCE_READY, e->id, e->tier);
}

static void handle_mesh_upload(PspExpRuntime *rt, const PspExpMessage *m)
{
    const PspExpMeshUpload *u;
    PspExpCacheEntry *e;
    int reason;
    uint32_t size, i;
    if (pspexp_message_expect(m, sizeof(*u)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
    u = (const PspExpMeshUpload *)m->payload;
    size = u->kind == PSPEXP_MESH_VERTICES ? pspexp_3d_vertex_size(u->vertex_type) : u->kind == PSPEXP_MESH_INDICES16 ? 2 : 0;
    if (!size || u->count == 0 || (uint64_t)u->count * size != u->data_length || u->data_length > m->length - sizeof(*u) ||
        (u->kind == PSPEXP_MESH_VERTICES && u->count > PSPEXP_3D_MAX_VERTICES + 1)) {
        send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, u->id, PSPEXP_RERR_BAD_LENGTH, 0);
        return;
    }
    e = pspexp_cache_insert_mesh(&rt->cache, u, &reason);
    if (!e) { send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, u->id, (uint16_t)reason, 0); return; }
    e->user = memalign_portable(u->data_length);
    if (!e->user) { pspexp_cache_remove(&rt->cache, e); send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, u->id, PSPEXP_RERR_OUT_OF_MEMORY, 0); return; }
    memcpy(e->user, m->payload + sizeof(*u), u->data_length);
    if (u->kind == PSPEXP_MESH_INDICES16)   /* once here, so a draw checks its range only */
        for (i = 0; i < u->count; i++) if (((const uint16_t *)e->user)[i] > e->max_index) e->max_index = ((const uint16_t *)e->user)[i];
    send_ready(rt, PSPEXP_OP_RESOURCE_READY, e->id, e->tier);
}

static PspExpCacheEntry *find_mesh(PspExpRuntime *rt, uint16_t id, uint8_t kind)
{
    PspExpCacheEntry *e = pspexp_cache_find(&rt->cache, id);
    if (!e) { send_resource_error(rt, PSPEXP_OP_RESOURCE_MISS, id, PSPEXP_RERR_UNKNOWN_ID, rt->frame_seq); return NULL; }
    if (e->kind != PSPEXP_KIND_MESH || e->format != kind) { send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, id, PSPEXP_RERR_WRONG_KIND, rt->frame_seq); return NULL; }
    return e;
}

static void handle_draw_mesh(PspExpRuntime *rt, const PspExpMessage *m)
{
    const PspExp3DDrawMesh *d;
    PspExpCacheEntry *v, *ix = NULL;
    const uint16_t *indices = NULL;
    int inl = 0;
    uint32_t size, i, top = 0;
    if (pspexp_message_expect(m, sizeof(*d)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
    d = (const PspExp3DDrawMesh *)m->payload;
    if (d->primitive > PSPEXP_3D_PRIM_MAX) { send_error(rt, PSPEXP_ERR_BAD_VERTEX_FMT, m, d->primitive); return; }
    if (!d->count) return;
    if (d->count > PSPEXP_3D_MAX_VERTICES) { send_error(rt, PSPEXP_ERR_BATCH_TOO_LARGE, m, d->count); return; }
    if (!(v = find_mesh(rt, d->vertices, PSPEXP_MESH_VERTICES))) return;
    size = pspexp_3d_vertex_size(v->vertex_type);
    if (d->indices == PSPEXP_MESH_NONE) {
        if ((uint64_t)d->first + d->count > v->count) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, d->count); return; }
    } else if (d->indices == PSPEXP_MESH_INLINE) {
        if ((uint64_t)d->count * 2u > m->length - sizeof(*d)) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, d->count); return; }
        indices = (const uint16_t *)(const void *)(m->payload + sizeof(*d));
        for (i = 0; i < d->count; i++) if (indices[i] > top) top = indices[i];
        if (top >= v->count) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, top); return; }
        inl = 1;
    } else {
        if (!(ix = find_mesh(rt, d->indices, PSPEXP_MESH_INDICES16))) return;
        if ((uint64_t)d->first + d->count > ix->count || ix->max_index >= v->count) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, d->count); return; }
        indices = (const uint16_t *)ix->user + d->first;
    }
    if (rt->tex3d_on && !rt->tex3d) {
        rt->tex3d = batch_atlas(rt, rt->tex3d_id, PSPEXP_PALETTE_DEFAULT, &rt->pal3d);
        if (!rt->tex3d) rt->tex3d_on = 0;
    }
    if (rt->ops.draw3d_mesh)
        rt->ops.draw3d_mesh(rt->ops.user, rt->tex3d_on ? rt->tex3d : NULL, rt->tex3d_on ? rt->pal3d : NULL, d->primitive,
                            v->vertex_type, d->count,
                            d->indices == PSPEXP_MESH_NONE ? (const uint8_t *)v->user + (size_t)d->first * size : v->user,
                            indices, inl);
    rt->stats.draws3d++;
    rt->stats.vertices3d += d->count;
}

static void handle_bench(PspExpRuntime *rt, const PspExpMessage *m)
{
    switch (m->header->opcode) {
    case PSPEXP_OP_BENCH_START:
        rt->bench_active = 1;
        rt->bench_bytes = rt->bench_msgs = 0;
        rt->bench_start_us = now_us(rt);
        break;
    case PSPEXP_OP_BENCH_DATA:
        if (rt->bench_active) {
            if (rt->bench_msgs == 0) rt->bench_start_us = now_us(rt);
            rt->bench_bytes += m->length;
            rt->bench_msgs++;
        }
        break;
    case PSPEXP_OP_BENCH_END: {
        PspExpBenchReport r;
        uint32_t t = now_us(rt);
        r.bytes = rt->bench_bytes;
        r.messages = rt->bench_msgs;
        r.elapsed_us = t - rt->bench_start_us;
        r.timestamp_us = t;
        rt->bench_active = 0;
        pspexp_writer_message(&rt->out, PSPEXP_CH_DEBUG, PSPEXP_OP_BENCH_REPORT, 0, &r, sizeof(r));
        break;
    }
    case PSPEXP_OP_BENCH_ECHO:
        pspexp_writer_message(&rt->out, PSPEXP_CH_DEBUG, PSPEXP_OP_BENCH_ECHO, m->header->flags, m->payload, m->length);
        break;
    case PSPEXP_OP_BENCH_UPLINK: {
        const PspExpBenchUplink *u;
        if (pspexp_message_expect(m, sizeof(*u)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        u = (const PspExpBenchUplink *)m->payload;
        rt->uplink_block = u->block_size > PSPEXP_MAX_PAYLOAD ? PSPEXP_MAX_PAYLOAD : PSPEXP_ALIGN_UP(u->block_size);
        rt->uplink_remaining = u->count;
        rt->bench_bytes = rt->bench_msgs = 0;
        rt->bench_start_us = now_us(rt);
        break;
    }
    default:
        send_error(rt, PSPEXP_ERR_BAD_OPCODE, m, 0);
    }
}

/* ---- FILE: the memory stick for the host ------------------------------------------ */

/* "ms0:/..." or "ef0:/..." (or the device alone), terminated, no ".." part */
static int fs_path_ok(const char *p)
{
    const char *s;
    if (!memchr(p, 0, PSPEXP_FS_PATH)) return 0;
    if (strncmp(p, "ms0:", 4) && strncmp(p, "ef0:", 4)) return 0;
    for (s = p; (s = strstr(s, "..")); s += 2)
        if ((s == p || s[-1] == '/') && (s[2] == 0 || s[2] == '/')) return 0;
    return 1;
}

static void handle_fs(PspExpRuntime *rt, const PspExpMessage *m)
{
    const PspExpFsRequest *q;
    PspExpFsReply *r;
    uint32_t len = sizeof(PspExpFsReply);
    uint64_t off;
    uint16_t op = m->header->opcode;
    if (pspexp_message_expect(m, sizeof(*q)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
    q = (const PspExpFsRequest *)m->payload;
    if (!rt->fs_buf && !(rt->fs_buf = (uint8_t *)malloc(sizeof(PspExpFsReply) + PSPEXP_FS_CHUNK))) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 1); return; }
    r = (PspExpFsReply *)rt->fs_buf;
    memset(r, 0, sizeof(*r));
    r->req_id = q->req_id;
    off = (uint64_t)q->offset_lo | ((uint64_t)q->offset_hi << 32);
    if (!rt->ops.fs_list) r->status = PSPEXP_FSERR_UNSUPPORTED;
    else if (!fs_path_ok(q->path) || (op == PSPEXP_OP_FS_RENAME && !fs_path_ok(q->path2))) r->status = PSPEXP_FSERR_PATH;
    else switch (op) {
    case PSPEXP_OP_FS_LIST: {
        int more = 0, n = rt->ops.fs_list(rt->ops.user, q->path, q->offset_lo, (PspExpFsEntry *)(r + 1), PSPEXP_FS_LIST_MAX, &more);
        if (n < 0) r->status = n;
        else { r->count = (uint32_t)n; r->more = (uint32_t)more; len += (uint32_t)n * (uint32_t)sizeof(PspExpFsEntry); }
        break;
    }
    case PSPEXP_OP_FS_STAT:
        r->status = rt->ops.fs_stat ? rt->ops.fs_stat(rt->ops.user, q->path, (PspExpFsEntry *)(r + 1)) : PSPEXP_FSERR_UNSUPPORTED;
        if (r->status >= 0) { r->count = 1; len += (uint32_t)sizeof(PspExpFsEntry); }
        break;
    case PSPEXP_OP_FS_READ: {
        uint32_t want = q->length > PSPEXP_FS_CHUNK ? PSPEXP_FS_CHUNK : q->length;
        r->status = rt->ops.fs_read ? rt->ops.fs_read(rt->ops.user, q->path, off, r + 1, want) : PSPEXP_FSERR_UNSUPPORTED;
        if (r->status > 0) len += (uint32_t)r->status;
        break;
    }
    case PSPEXP_OP_FS_WRITE:
        if (q->length > m->length - (uint32_t)sizeof(*q) || q->length > PSPEXP_FS_CHUNK) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        r->status = rt->ops.fs_write ? rt->ops.fs_write(rt->ops.user, q->path, off, q + 1, q->length, q->flags) : PSPEXP_FSERR_UNSUPPORTED;
        break;
    case PSPEXP_OP_FS_MKDIR:  r->status = rt->ops.fs_mkdir ? rt->ops.fs_mkdir(rt->ops.user, q->path) : PSPEXP_FSERR_UNSUPPORTED; break;
    case PSPEXP_OP_FS_REMOVE: r->status = rt->ops.fs_remove ? rt->ops.fs_remove(rt->ops.user, q->path) : PSPEXP_FSERR_UNSUPPORTED; break;
    case PSPEXP_OP_FS_RENAME: r->status = rt->ops.fs_rename ? rt->ops.fs_rename(rt->ops.user, q->path, q->path2) : PSPEXP_FSERR_UNSUPPORTED; break;
    case PSPEXP_OP_FS_INFO: {
        uint64_t fr = 0, tot = 0;
        r->status = rt->ops.fs_info ? rt->ops.fs_info(rt->ops.user, q->path, &fr, &tot) : PSPEXP_FSERR_UNSUPPORTED;
        r->value_lo = (uint32_t)fr; r->value_hi = (uint32_t)(fr >> 32);
        r->value2_lo = (uint32_t)tot; r->value2_hi = (uint32_t)(tot >> 32);
        break;
    }
    default: r->status = PSPEXP_FSERR_UNSUPPORTED;
    }
    pspexp_writer_message(&rt->out, PSPEXP_CH_FILE, PSPEXP_OP_FS_REPLY, 0, r, len);
}

static void send_audio_status(PspExpRuntime *rt)
{
    PspExpAudioStatus st;
    memset(&st, 0, sizeof(st));
    if (rt->audio_active) { audio_lock(rt); pspexp_mixer_status(&rt->mixer, &st); audio_unlock(rt); }
    pspexp_writer_message(&rt->out, PSPEXP_CH_AUDIO, PSPEXP_OP_AUDIO_STATUS, 0, &st, sizeof(st));
    rt->audio_status_us = now_us(rt);
}

static void handle_audio(PspExpRuntime *rt, const PspExpMessage *m)
{
    switch (m->header->opcode) {
    case PSPEXP_OP_RESOURCE_UPLOAD_SOUND: {
        const PspExpSoundUpload *su;
        PspExpCacheEntry *e;
        int reason;
        uint32_t bytes;
        if (pspexp_message_expect(m, sizeof(*su)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        su = (const PspExpSoundUpload *)m->payload;
        reason = pspexp_sound_validate(su, m->length - (uint32_t)sizeof(*su));
        if (reason != PSPEXP_RERR_NONE) { send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, su->id, (uint16_t)reason, 0); return; }
        bytes = pspexp_sound_data_length(su->channels, su->frames);
        e = pspexp_cache_insert_sound(&rt->cache, su, bytes, &reason);
        if (!e) { send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, su->id, (uint16_t)reason, 0); return; }
        e->user = malloc(bytes);
        if (!e->user) { pspexp_cache_remove(&rt->cache, e); send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, su->id, PSPEXP_RERR_OUT_OF_MEMORY, 0); return; }
        memcpy(e->user, m->payload + sizeof(*su), bytes);
        e->frame_count = 0;
        e->height = 0;
        /* frames kept in `bytes / (channels * 2)` */
        rt->stats.sounds++;
        send_ready(rt, PSPEXP_OP_RESOURCE_READY, e->id, e->tier);
        break;
    }
    case PSPEXP_OP_PLAY_SOUND: {
        const PspExpPlaySound *ps;
        PspExpCacheEntry *e;
        if (pspexp_message_expect(m, sizeof(*ps)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        ps = (const PspExpPlaySound *)m->payload;
        e = pspexp_cache_find(&rt->cache, ps->id);
        if (!e) { send_resource_error(rt, PSPEXP_OP_RESOURCE_MISS, ps->id, PSPEXP_RERR_UNKNOWN_ID, rt->frame_seq); return; }
        if (e->kind != PSPEXP_KIND_SOUND) { send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, ps->id, PSPEXP_RERR_WRONG_KIND, 0); return; }
        rt->stats.plays++;
        if (rt->audio_active) {
            audio_lock(rt);
            pspexp_mixer_play(&rt->mixer, e->id, (const int16_t *)e->user, e->bytes / ((uint32_t)e->width * 2u),
                              (uint8_t)e->width, ps->volume, ps->pan, ps->loop);
            audio_unlock(rt);
        }
        break;
    }
    case PSPEXP_OP_STOP_SOUND: {
        const PspExpStopSound *ss;
        if (pspexp_message_expect(m, sizeof(*ss)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        ss = (const PspExpStopSound *)m->payload;
        if (rt->audio_active) { audio_lock(rt); pspexp_mixer_stop_sound(&rt->mixer, ss->id); audio_unlock(rt); }
        break;
    }
    case PSPEXP_OP_AUDIO_STREAM: {
        const PspExpAudioStream *as;
        if (pspexp_message_expect(m, sizeof(*as)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        as = (const PspExpAudioStream *)m->payload;
        if (m->length < sizeof(*as) + as->frames * 4u) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        rt->stats.stream_frames += as->frames;
        if (rt->audio_active) {
            audio_lock(rt);
            pspexp_mixer_stream_write(&rt->mixer, (const int16_t *)(m->payload + sizeof(*as)), as->frames);
            audio_unlock(rt);
        }
        if (now_us(rt) - rt->audio_status_us >= 250000u) send_audio_status(rt);
        break;
    }
    case PSPEXP_OP_AUDIO_STREAM_RESET:
        if (rt->audio_active) { audio_lock(rt); pspexp_mixer_stream_reset(&rt->mixer); audio_unlock(rt); }
        send_audio_status(rt);
        break;
    default:
        send_error(rt, PSPEXP_ERR_BAD_OPCODE, m, 0);
    }
}

static void dispatch(PspExpRuntime *rt, const PspExpMessage *m)
{
    const uint16_t op = m->header->opcode;
    rt->stats.messages_in++;
    rt->stats.bytes_in += PSPEXP_HEADER_SIZE + m->length;
    if (rt->maintenance && m->header->channel != PSPEXP_CH_SYSTEM && m->header->channel != PSPEXP_CH_FILE) return;   /* another version's messages: not ours to read */
    if (rt->in_frame) rt->frame_bytes += PSPEXP_HEADER_SIZE + m->length;

    switch (op) {
    case PSPEXP_OP_HELLO_ACK:
        if (pspexp_message_expect(m, sizeof(PspExpHelloAck)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        memcpy(&rt->ack, m->payload, sizeof(rt->ack));
        rt->connected = 1;
        rt->maintenance = (rt->ack.flags & PSPEXP_ACK_MAINTENANCE) != 0;
        if (rt->maintenance) { rt_log(rt, "maintenance: the host speaks protocol %u; files only", rt->ack.protocol); return; }
        rt_log(rt, "connected: input %u Hz, audio %u Hz%s", rt->ack.input_rate, rt->ack.audio_rate,
               rt->ack.flags & PSPEXP_ACK_DEPTH ? ", depth buffer" : "");
        if (rt->ops.depth_buffer) rt->ops.depth_buffer(rt->ops.user, (rt->ack.flags & PSPEXP_ACK_DEPTH) != 0);
        if (rt->ack.audio_rate >= 8000 && rt->ack.audio_rate <= 48000 && rt->ops.audio_start && !rt->audio_active) {
            /* 32768-frame ring, start playing once 100 ms is buffered */
            if (pspexp_mixer_init(&rt->mixer, rt->ack.audio_rate, 32768, rt->ack.audio_rate / 10) == 0) {
                if (rt->ops.audio_start(rt->ops.user, &rt->mixer, rt->ack.audio_rate) == 0) rt->audio_active = 1;
                else pspexp_mixer_free(&rt->mixer);
            }
        }
        return;
    case PSPEXP_OP_DISCONNECT:
        rt_log(rt, "host disconnected");
        release_all(rt);
        return;
    case PSPEXP_OP_CLOCK_SET: {
        const PspExpClockSet *c;
        PspExpClockStatus st;
        uint64_t before = 0, after = 0, want;
        int64_t diff;
        int tz = 0, dst = 0;
        if (pspexp_message_expect(m, sizeof(*c)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        c = (const PspExpClockSet *)m->payload;
        memset(&st, 0, sizeof(st));
        if (m->length >= sizeof(PspExpClockSet) && (c->flags & PSPEXP_CLOCK_ZONE) && rt->ops.zone_set && rt->ops.clock_get &&
            rt->ops.clock_get(rt->ops.user, &before, &tz, &dst) == 0 && (tz != c->tz_minutes || dst != (c->dst != 0)))
            st.zone_result = rt->ops.zone_set(rt->ops.user, c->tz_minutes, c->dst != 0) < 0 ? -1 : 1;
        want = (uint64_t)c->utc_lo | ((uint64_t)c->utc_hi << 32);
        if (!rt->ops.clock_get || !rt->ops.clock_set || rt->ops.clock_get(rt->ops.user, &before, &tz, &dst) < 0) st.result = -1;
        else {
            diff = (int64_t)(want - before);
            if (c->min_diff != PSPEXP_CLOCK_REPORT_ONLY && (c->min_diff == 0 || diff > (int64_t)c->min_diff || -diff > (int64_t)c->min_diff))
                st.result = rt->ops.clock_set(rt->ops.user, want) < 0 ? -2 : 1;
            rt->ops.clock_get(rt->ops.user, &after, &tz, &dst);
        }
        st.before_lo = (uint32_t)before; st.before_hi = (uint32_t)(before >> 32);
        st.after_lo = (uint32_t)after; st.after_hi = (uint32_t)(after >> 32);
        st.tz_minutes = tz; st.dst = dst;
        pspexp_writer_message(&rt->out, PSPEXP_CH_SYSTEM, PSPEXP_OP_CLOCK_STATUS, 0, &st, sizeof(st));
        return;
    }
    case PSPEXP_OP_MEMORY_QUERY: {
        PspExpMemoryStatus s;
        pspexp_cache_status(&rt->cache, &s);
        pspexp_writer_message(&rt->out, PSPEXP_CH_SYSTEM, PSPEXP_OP_MEMORY_STATUS, 0, &s, sizeof(s));
        return;
    }
    case PSPEXP_OP_BENCH_START: case PSPEXP_OP_BENCH_DATA: case PSPEXP_OP_BENCH_END:
    case PSPEXP_OP_BENCH_ECHO: case PSPEXP_OP_BENCH_UPLINK:
        handle_bench(rt, m);
        return;
    case PSPEXP_OP_DEBUG_SCREENSHOT: {
        const PspExpScreenshot *sc;
        if (pspexp_message_expect(m, sizeof(*sc)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        sc = (const PspExpScreenshot *)m->payload;
        if (rt->ops.screenshot) rt->ops.screenshot(rt->ops.user, sc->index);
        return;
    }
    default:
        break;
    }

    if (!rt->connected) { send_error(rt, PSPEXP_ERR_NOT_CONNECTED, m, 0); return; }

    switch (op) {
    case PSPEXP_OP_RESOURCE_UPLOAD_TEXTURE:
        handle_upload(rt, m);
        break;
    case PSPEXP_OP_RESOURCE_UPLOAD_PALETTE:
        handle_palette_upload(rt, m);
        break;
    case PSPEXP_OP_RESOURCE_UPLOAD_SOUND: case PSPEXP_OP_PLAY_SOUND: case PSPEXP_OP_STOP_SOUND:
    case PSPEXP_OP_AUDIO_STREAM: case PSPEXP_OP_AUDIO_STREAM_RESET:
        handle_audio(rt, m);
        break;
    case PSPEXP_OP_FS_LIST: case PSPEXP_OP_FS_READ: case PSPEXP_OP_FS_WRITE: case PSPEXP_OP_FS_MKDIR:
    case PSPEXP_OP_FS_REMOVE: case PSPEXP_OP_FS_RENAME: case PSPEXP_OP_FS_INFO: case PSPEXP_OP_FS_STAT:
        handle_fs(rt, m);
        break;
    case PSPEXP_OP_RESOURCE_RELEASE: {
        const PspExpResourceId *r;
        PspExpCacheEntry *e;
        if (pspexp_message_expect(m, sizeof(*r)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        r = (const PspExpResourceId *)m->payload;
        e = pspexp_cache_find(&rt->cache, r->id);
        if (!e) { send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, r->id, PSPEXP_RERR_UNKNOWN_ID, 0); return; }
        destroy_entry(rt, e);
        rt->stats.releases++;
        break;
    }
    case PSPEXP_OP_RESOURCE_HINT: {
        const PspExpResourceHint *h;
        PspExpCacheEntry *e;
        if (pspexp_message_expect(m, sizeof(*h)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        h = (const PspExpResourceHint *)m->payload;
        e = pspexp_cache_find(&rt->cache, h->id);
        if (!e) { send_resource_error(rt, PSPEXP_OP_RESOURCE_ERROR, h->id, PSPEXP_RERR_UNKNOWN_ID, 0); return; }
        e->priority = h->priority;   /* v0.1: recorded, not acted on (section 6.3) */
        break;
    }
    case PSPEXP_OP_BEGIN_FRAME: {
        const PspExpBeginFrame *b;
        if (pspexp_message_expect(m, sizeof(*b)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
        b = (const PspExpBeginFrame *)m->payload;
        if (rt->in_frame) rt_log(rt, "BEGIN_FRAME %u while frame %u open", b->frame_seq, rt->frame_seq);
        rt->in_frame = 1;
        rt->no_target = 0;
        rt->frame_seq = b->frame_seq;
        rt->frame_start_us = now_us(rt);
        rt->frame_busy_us = 0;
        rt->frame_bytes = PSPEXP_HEADER_SIZE + m->length;
        rt->stats.frame_sprites = 0;
        rt->stats.frame_dropped = 0;
        if (rt->ops.frame_begin) rt->ops.frame_begin(rt->ops.user, b->frame_seq);
        break;
    }
    case PSPEXP_OP_SET_CLIP: case PSPEXP_OP_DRAW_SPRITE_BATCH: case PSPEXP_OP_DRAW_TILE_BATCH:
    case PSPEXP_OP_DRAW_QUAD_BATCH: case PSPEXP_OP_DRAW_RECT: case PSPEXP_OP_DRAW_LINE: case PSPEXP_OP_PRESENT:
        if (!rt->in_frame) { send_error(rt, PSPEXP_ERR_NOT_IN_FRAME, m, 0); return; }
        if (op == PSPEXP_OP_SET_CLIP) {
            const PspExpSetClip *c;
            if (pspexp_message_expect(m, sizeof(*c)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
            c = (const PspExpSetClip *)m->payload;
            if (rt->ops.set_clip) rt->ops.set_clip(rt->ops.user, c->x, c->y, c->w, c->h);
        } else if (op == PSPEXP_OP_DRAW_SPRITE_BATCH) {
            handle_sprite_batch(rt, m);
        } else if (op == PSPEXP_OP_DRAW_TILE_BATCH) {
            handle_tile_batch(rt, m);
        } else if (op == PSPEXP_OP_DRAW_QUAD_BATCH) {
            handle_quad_batch(rt, m);
        } else if (op == PSPEXP_OP_DRAW_RECT) {
            const PspExpDrawRect *r;
            if (pspexp_message_expect(m, sizeof(*r)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
            r = (const PspExpDrawRect *)m->payload;
            if (rt->ops.draw_rect) rt->ops.draw_rect(rt->ops.user, r->x, r->y, r->w, r->h, r->color, r->alpha);
            rt->stats.rects++;
        } else if (op == PSPEXP_OP_DRAW_LINE) {
            const PspExpDrawLine *l;
            if (pspexp_message_expect(m, sizeof(*l)) < 0) { send_error(rt, PSPEXP_ERR_BAD_LENGTH, m, 0); return; }
            l = (const PspExpDrawLine *)m->payload;
            if (rt->ops.draw_line) rt->ops.draw_line(rt->ops.user, l->x0, l->y0, l->x1, l->y1, l->color, l->alpha);
            rt->stats.lines++;
        } else {
            PspExpPresentDone d;
            uint32_t exec_us = rt->frame_busy_us;   /* (before 2026-10-01: from BEGIN_FRAME, the USB waits with it) */
            uint32_t ts = rt->ops.present ? rt->ops.present(rt->ops.user) : now_us(rt);
            rt->in_frame = 0;
            rt->stats.frames++;
            if (rt->stats.frames % 600 == 0) {
                char line[160];
                int k, o = 0;
                for (k = 0; k < 24 && rt->prof_op[k]; k++) {
                    if (rt->prof_us[k] >= 60000u)   /* 0.1 ms a frame or more */
                        o += snprintf(line + o, sizeof(line) - (size_t)o, " %s %u/%u", pspexp_opcode_name(rt->prof_op[k]),
                                      (unsigned)(rt->prof_us[k] / 600u), (unsigned)(rt->prof_bytes[k] / 600u));
                    rt->prof_us[k] = rt->prof_bytes[k] = 0;
                    if (o > (int)sizeof(line) - 40) break;
                }
                rt_log(rt, "us/bytes a frame:%s", line);
            }
            rt->stats.last_frame_bytes = rt->frame_bytes;
            rt->stats.last_execute_us = exec_us;
            memset(&d, 0, sizeof(d));
            d.frame_seq = rt->frame_seq;
            d.timestamp_us = ts;
            d.queue_depth = 0;
            d.dropped = 0;
            d.execute_us = exec_us;
            d.ge_us = rt->ops.frame_ge_us ? rt->ops.frame_ge_us(rt->ops.user) : 0;
            d.frame_bytes = rt->frame_bytes;
            d.sprites_drawn = rt->stats.frame_sprites;
            d.sprites_dropped = rt->stats.frame_dropped;
            pspexp_writer_message(&rt->out, PSPEXP_CH_SYSTEM, PSPEXP_OP_PRESENT_DONE, 0, &d, sizeof(d));
        }
        break;
    case PSPEXP_OP_RESOURCE_UPLOAD_MESH:
        if (!rt->ops.draw3d) { send_error(rt, PSPEXP_ERR_BAD_OPCODE, m, 0); return; }
        handle_mesh_upload(rt, m);
        break;
    case PSPEXP_OP_RESOURCE_PATCH:
        if (!rt->ops.texture_patch) { send_error(rt, PSPEXP_ERR_BAD_OPCODE, m, 0); return; }
        handle_patch(rt, m);
        break;
    case PSPEXP_OP_RESOURCE_CREATE_TARGET:
        if (!rt->ops.draw3d) { send_error(rt, PSPEXP_ERR_BAD_OPCODE, m, 0); return; }
        handle_target_create(rt, m);
        break;
    case PSPEXP_OP_3D_STATE: case PSPEXP_OP_3D_MATRICES: case PSPEXP_OP_3D_LIGHTS:
    case PSPEXP_OP_3D_DRAW: case PSPEXP_OP_3D_CLEAR: case PSPEXP_OP_3D_DRAW_MESH: case PSPEXP_OP_3D_TARGET: case PSPEXP_OP_3D_MORPH:
        if (!rt->ops.draw3d) { send_error(rt, PSPEXP_ERR_BAD_OPCODE, m, 0); return; }
        if (!rt->in_frame) { send_error(rt, PSPEXP_ERR_NOT_IN_FRAME, m, 0); return; }
        handle_3d(rt, m);
        break;
    default:
        send_error(rt, PSPEXP_ERR_BAD_OPCODE, m, 0);
    }
}

static int process(PspExpRuntime *rt)
{
    PspExpMessage m;
    int r;
    while ((r = pspexp_stream_next(&rt->in, &m)) == 1) {
        /* the frame's work: its messages handled, not the waits for them between (execute_us) */
        uint32_t t0 = now_us(rt), dt;
        int was = rt->in_frame, k;
        uint16_t op = m.header->opcode;
        dispatch(rt, &m);
        dt = now_us(rt) - t0;
        if (was && rt->in_frame) rt->frame_busy_us += dt;
        /* the time by message kind, in the log every 600 frames (which of them a frame's work goes to) */
        for (k = 0; k < 24 && rt->prof_op[k] && rt->prof_op[k] != op; k++) { }
        if (k < 24) { rt->prof_op[k] = op; rt->prof_us[k] += dt; rt->prof_bytes[k] += m.header->length; }
    }
    if (r < 0) { rt_log(rt, "inbound stream corrupt"); return -1; }
    return 0;
}

int pspexp_runtime_feed(PspExpRuntime *rt, const void *data, size_t len)
{
    if (pspexp_stream_feed(&rt->in, data, len) < 0) return -1;
    return process(rt);
}

size_t pspexp_runtime_in_writable(PspExpRuntime *rt, void **where)
{
    return pspexp_stream_writable(&rt->in, where);
}

int pspexp_runtime_in_commit(PspExpRuntime *rt, size_t len)
{
    pspexp_stream_commit(&rt->in, len);
    return process(rt);
}

void pspexp_runtime_push_input(PspExpRuntime *rt, uint32_t buttons, uint8_t analog_x, uint8_t analog_y)
{
    PspExpInput in;
    memset(&in, 0, sizeof(in));
    in.sequence = rt->input_seq++;
    in.timestamp_us = now_us(rt);
    in.buttons = buttons;
    in.analog_x = analog_x;
    in.analog_y = analog_y;
    pspexp_writer_message(&rt->out, PSPEXP_CH_INPUT, PSPEXP_OP_INPUT_SAMPLE, 0, &in, sizeof(in));
}

const void *pspexp_runtime_out_peek(PspExpRuntime *rt, size_t *len)
{
    if (rt->out.len == 0 && rt->uplink_remaining > 0) {
        if (pspexp_writer_begin(&rt->out, PSPEXP_CH_DEBUG, PSPEXP_OP_BENCH_DATA, 0) == 0 &&
            pspexp_writer_reserve(&rt->out, rt->uplink_block) != NULL &&
            pspexp_writer_end(&rt->out) == 0) {
            rt->bench_bytes += rt->uplink_block;
            rt->bench_msgs++;
        }
        if (--rt->uplink_remaining == 0) {
            PspExpBenchReport r;
            uint32_t t = now_us(rt);
            r.bytes = rt->bench_bytes;
            r.messages = rt->bench_msgs;
            r.elapsed_us = t - rt->bench_start_us;
            r.timestamp_us = t;
            pspexp_writer_message(&rt->out, PSPEXP_CH_DEBUG, PSPEXP_OP_BENCH_REPORT, 0, &r, sizeof(r));
        }
    }
    *len = rt->out.len;
    return rt->out.buf;
}

void pspexp_runtime_out_consume(PspExpRuntime *rt)
{
    rt->stats.bytes_out += rt->out.len;
    pspexp_writer_reset(&rt->out);
}
