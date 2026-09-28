#include "pspexp_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

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
    else if (e->kind == PSPEXP_KIND_SOUND) {
        audio_lock(rt);
        if (rt->audio_active) pspexp_mixer_stop_sound(&rt->mixer, e->id);
        audio_unlock(rt);
        free(e->user);
        e->user = NULL;
    }
    else if (rt->ops.texture_destroy) rt->ops.texture_destroy(rt->ops.user, e);
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
                 (rt->ops.clock_get && rt->ops.clock_set ? PSPEXP_FEATURE_CLOCK : 0);
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
        rt_log(rt, "connected: input %u Hz, audio %u Hz", rt->ack.input_rate, rt->ack.audio_rate);
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
        rt->frame_seq = b->frame_seq;
        rt->frame_start_us = now_us(rt);
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
            uint32_t exec_us = now_us(rt) - rt->frame_start_us;
            uint32_t ts = rt->ops.present ? rt->ops.present(rt->ops.user) : now_us(rt);
            rt->in_frame = 0;
            rt->stats.frames++;
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
    default:
        send_error(rt, PSPEXP_ERR_BAD_OPCODE, m, 0);
    }
}

static int process(PspExpRuntime *rt)
{
    PspExpMessage m;
    int r;
    while ((r = pspexp_stream_next(&rt->in, &m)) == 1)
        dispatch(rt, &m);
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
