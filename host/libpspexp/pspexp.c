#include "pspexp.h"
#include "../../protocol/serialization/pspexp_writer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define RX_BUFFER_SIZE (256u * 1024u)
#define SEND_RING 32

typedef struct Resource {
    uint16_t id;
    uint8_t  state;     /* PspExpResourceState */
    uint8_t  tier;
    uint8_t *retained;  /* upload payload copy for replay after reconnect */
    uint32_t retained_len;
} Resource;

typedef struct FrameRecord {
    uint32_t seq;
    uint64_t sent_us;           /* host clock */
    uint32_t input_timestamp_us;/* PSP clock of the input the frame consumed */
} FrameRecord;

struct PspExpContext {
    PspExpTransport *t;
    PspExpConfig cfg;
    PspExpStream in;
    PspExpWriter frame;
    PspExpWriter msg;
    uint8_t *rx;
    PspExpLinkState state;
    PspExpHello hello;
    Resource *res;
    size_t nres, cres;
    PspExpInput input;
    int have_input;
    int in_frame;
    uint32_t next_frame_seq, last_sent_seq;
    int in_flight;
    FrameRecord ring[SEND_RING];
    PspExpMemoryStatus mem;
    PspExpStats stats;
    PspExpLogFn log; void *log_user;
    PspExpMessageHook hook; void *hook_user;
};

static void ctx_log(PspExpContext *ctx, const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (ctx->log) ctx->log(ctx->log_user, buf);
    else fprintf(stderr, "libpspexp: %s\n", buf);
}

void pspexp_config_default(PspExpConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->input_rate = 60;
    cfg->max_batch_records = PSPEXP_BATCH_MAX_RECORDS;
    cfg->max_in_flight = 1;
    cfg->retain_uploads = 1;
}

PspExpContext *pspexp_create(PspExpTransport *transport, const PspExpConfig *cfg)
{
    PspExpContext *ctx = (PspExpContext *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->t = transport;
    if (cfg) ctx->cfg = *cfg; else pspexp_config_default(&ctx->cfg);
    if (ctx->cfg.max_in_flight < 1) ctx->cfg.max_in_flight = 1;
    if (ctx->cfg.max_batch_records == 0 || ctx->cfg.max_batch_records > PSPEXP_BATCH_MAX_RECORDS)
        ctx->cfg.max_batch_records = PSPEXP_BATCH_MAX_RECORDS;
    ctx->rx = (uint8_t *)malloc(RX_BUFFER_SIZE);
    if (!ctx->rx || pspexp_stream_init_dynamic(&ctx->in, RX_BUFFER_SIZE) < 0 ||
        pspexp_writer_init_dynamic(&ctx->frame, 64u * 1024u) < 0 ||
        pspexp_writer_init_dynamic(&ctx->msg, 4096) < 0) {
        pspexp_destroy(ctx);
        return NULL;
    }
    ctx->next_frame_seq = 1;
    return ctx;
}

void pspexp_destroy(PspExpContext *ctx)
{
    size_t i;
    if (!ctx) return;
    if (ctx->t && ctx->t->close) ctx->t->close(ctx->t);
    for (i = 0; i < ctx->nres; i++) free(ctx->res[i].retained);
    free(ctx->res);
    free(ctx->rx);
    pspexp_stream_free(&ctx->in);
    pspexp_writer_free(&ctx->frame);
    pspexp_writer_free(&ctx->msg);
    free(ctx);
}

void pspexp_set_log(PspExpContext *ctx, PspExpLogFn fn, void *user) { ctx->log = fn; ctx->log_user = user; }
void pspexp_set_message_hook(PspExpContext *ctx, PspExpMessageHook fn, void *user) { ctx->hook = fn; ctx->hook_user = user; }
PspExpLinkState pspexp_link_state(const PspExpContext *ctx) { return ctx->state; }
const PspExpHello *pspexp_caps(const PspExpContext *ctx) { return &ctx->hello; }
const PspExpInput *pspexp_input(const PspExpContext *ctx) { return &ctx->input; }
const PspExpStats *pspexp_stats(const PspExpContext *ctx) { return &ctx->stats; }
const PspExpMemoryStatus *pspexp_memory_status(const PspExpContext *ctx) { return &ctx->mem; }
PspExpTransport *pspexp_transport(PspExpContext *ctx) { return ctx->t; }
int pspexp_frames_in_flight(const PspExpContext *ctx) { return ctx->in_flight; }

static void link_lost(PspExpContext *ctx)
{
    size_t i;
    if (ctx->state != PSPEXP_LINK_DISCONNECTED) ctx_log(ctx, "link lost");
    ctx->state = PSPEXP_LINK_DISCONNECTED;
    ctx->in_flight = 0;
    for (i = 0; i < ctx->nres; i++)
        if (ctx->res[i].state != PSPEXP_RES_FAILED) ctx->res[i].state = PSPEXP_RES_NONE;
    pspexp_stream_reset(&ctx->in);
}

static int send_bytes(PspExpContext *ctx, const void *data, size_t len)
{
    if (ctx->t->send(ctx->t, data, len) < 0) { link_lost(ctx); return -1; }
    ctx->stats.bytes_sent += len;
    return 0;
}

void pspexp_set_transport(PspExpContext *ctx, PspExpTransport *transport)
{
    if (ctx->t && ctx->t != transport && ctx->t->close) ctx->t->close(ctx->t);
    ctx->t = transport;
    link_lost(ctx);
}

int pspexp_send_raw(PspExpContext *ctx, uint8_t channel, uint16_t opcode, const void *payload, size_t len)
{
    pspexp_writer_reset(&ctx->msg);
    if (pspexp_writer_message(&ctx->msg, channel, opcode, 0, payload, len) < 0) return -1;
    return send_bytes(ctx, ctx->msg.buf, ctx->msg.len);
}

static Resource *find_res(PspExpContext *ctx, uint16_t id)
{
    size_t i;
    for (i = 0; i < ctx->nres; i++) if (ctx->res[i].id == id) return &ctx->res[i];
    return NULL;
}

static Resource *add_res(PspExpContext *ctx, uint16_t id)
{
    Resource *r = find_res(ctx, id);
    if (r) return r;
    if (ctx->nres == ctx->cres) {
        size_t nc = ctx->cres ? ctx->cres * 2 : 32;
        Resource *nr = (Resource *)realloc(ctx->res, nc * sizeof(Resource));
        if (!nr) return NULL;
        ctx->res = nr; ctx->cres = nc;
    }
    r = &ctx->res[ctx->nres++];
    memset(r, 0, sizeof(*r));
    r->id = id;
    return r;
}

static void remove_res(PspExpContext *ctx, Resource *r)
{
    size_t idx = (size_t)(r - ctx->res);
    free(r->retained);
    if (idx + 1 < ctx->nres) ctx->res[idx] = ctx->res[ctx->nres - 1];
    ctx->nres--;
}

static FrameRecord *record_for(PspExpContext *ctx, uint32_t seq)
{
    FrameRecord *r = &ctx->ring[seq % SEND_RING];
    return r->seq == seq ? r : NULL;
}

static void on_hello(PspExpContext *ctx, const PspExpMessage *m)
{
    PspExpHelloAck ack;
    size_t i;
    if (pspexp_message_expect(m, sizeof(PspExpHello)) < 0) { ctx_log(ctx, "short HELLO"); return; }
    memcpy(&ctx->hello, m->payload, sizeof(ctx->hello));
    if (ctx->hello.protocol != PSPEXP_PROTOCOL_VERSION) {
        ctx_log(ctx, "protocol %u not supported", ctx->hello.protocol);
        return;
    }
    if (ctx->state == PSPEXP_LINK_CONNECTED) {
        ctx->stats.reconnects++;
        ctx_log(ctx, "runtime restarted (reconnect %u)", ctx->stats.reconnects);
    }
    ctx->in_flight = 0;
    for (i = 0; i < ctx->nres; i++) ctx->res[i].state = PSPEXP_RES_NONE;

    memset(&ack, 0, sizeof(ack));
    ack.protocol = PSPEXP_PROTOCOL_VERSION;
    ack.input_rate = ctx->cfg.input_rate;
    if (ctx->hello.input_rate_max && ack.input_rate > ctx->hello.input_rate_max) ack.input_rate = ctx->hello.input_rate_max;
    ack.audio_rate = ctx->cfg.audio_rate;
    ack.audio_channels = ctx->cfg.audio_channels;
    if (pspexp_send_raw(ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_HELLO_ACK, &ack, sizeof(ack)) < 0) return;
    ctx->state = PSPEXP_LINK_CONNECTED;
    ctx_log(ctx, "connected: executor %u, %ux%u, vram %u KiB, ram %u KiB, formats 0x%x",
            ctx->hello.executor, ctx->hello.display_width, ctx->hello.display_height,
            ctx->hello.vram_budget / 1024, ctx->hello.ram_budget / 1024, ctx->hello.texture_formats);

    /* Replay the working set (section 9.2). */
    for (i = 0; i < ctx->nres; i++) {
        Resource *r = &ctx->res[i];
        if (!r->retained) continue;
        if (pspexp_send_raw(ctx, PSPEXP_CH_RESOURCE, PSPEXP_OP_RESOURCE_UPLOAD_TEXTURE, r->retained, r->retained_len) < 0) return;
        r->state = PSPEXP_RES_PENDING;
    }
}

static void handle(PspExpContext *ctx, const PspExpMessage *m)
{
    ctx->stats.bytes_received += PSPEXP_HEADER_SIZE + m->length;
    if (ctx->hook && ctx->hook(ctx->hook_user, ctx, m)) return;

    switch (m->header->opcode) {
    case PSPEXP_OP_HELLO:
        on_hello(ctx, m);
        break;
    case PSPEXP_OP_PRESENT_DONE: {
        const PspExpPresentDone *d;
        FrameRecord *rec;
        if (pspexp_message_expect(m, sizeof(*d)) < 0) break;
        d = (const PspExpPresentDone *)m->payload;
        ctx->stats.frames_presented++;
        ctx->stats.frames_dropped_remote = d->dropped;
        ctx->stats.last_execute_us = d->execute_us;
        ctx->in_flight = (int)(ctx->last_sent_seq - d->frame_seq);
        if (ctx->in_flight < 0) ctx->in_flight = 0;
        rec = record_for(ctx, d->frame_seq);
        if (rec) {
            ctx->stats.last_present_latency_us = (uint32_t)(pspexp_time_us() - rec->sent_us);
            ctx->stats.last_input_to_present_us = rec->input_timestamp_us ? d->timestamp_us - rec->input_timestamp_us : 0;
        }
        break;
    }
    case PSPEXP_OP_INPUT_SAMPLE:
        if (pspexp_message_expect(m, sizeof(PspExpInput)) < 0) break;
        memcpy(&ctx->input, m->payload, sizeof(ctx->input));
        ctx->have_input = 1;
        ctx->stats.inputs_received++;
        break;
    case PSPEXP_OP_RESOURCE_READY:
    case PSPEXP_OP_RESOURCE_TIER: {
        const PspExpResourceReady *r;
        Resource *res;
        if (pspexp_message_expect(m, sizeof(*r)) < 0) break;
        r = (const PspExpResourceReady *)m->payload;
        res = find_res(ctx, r->id);
        if (res) { res->state = PSPEXP_RES_RESIDENT; res->tier = r->tier; }
        break;
    }
    case PSPEXP_OP_RESOURCE_ERROR:
    case PSPEXP_OP_RESOURCE_MISS: {
        const PspExpResourceError *e;
        Resource *res;
        if (pspexp_message_expect(m, sizeof(*e)) < 0) break;
        e = (const PspExpResourceError *)m->payload;
        ctx->stats.resource_errors++;
        ctx_log(ctx, "%s id %u: %s (frame %u)", pspexp_opcode_name(m->header->opcode), e->id,
                pspexp_resource_error_name(e->reason), e->frame_seq);
        if (m->header->opcode == PSPEXP_OP_RESOURCE_ERROR) {
            /* Only an unacknowledged upload fails; a draw-time error leaves
             * the resource resident. */
            res = find_res(ctx, e->id);
            if (res && res->state == PSPEXP_RES_PENDING) res->state = PSPEXP_RES_FAILED;
        }
        break;
    }
    case PSPEXP_OP_MEMORY_STATUS:
        if (pspexp_message_expect(m, sizeof(PspExpMemoryStatus)) < 0) break;
        memcpy(&ctx->mem, m->payload, sizeof(ctx->mem));
        break;
    case PSPEXP_OP_ERROR: {
        const PspExpError *e;
        if (pspexp_message_expect(m, sizeof(*e)) < 0) break;
        e = (const PspExpError *)m->payload;
        ctx->stats.protocol_errors++;
        ctx_log(ctx, "runtime reported error %u for %s seq %u (detail %u)", e->code,
                pspexp_opcode_name(e->opcode), e->sequence, e->detail);
        break;
    }
    case PSPEXP_OP_DISCONNECT:
        ctx_log(ctx, "runtime disconnected");
        link_lost(ctx);
        break;
    case PSPEXP_OP_BENCH_DATA: case PSPEXP_OP_BENCH_REPORT: case PSPEXP_OP_BENCH_ECHO:
        break;  /* for the hook */
    default:
        ctx_log(ctx, "unexpected %s from runtime", pspexp_opcode_name(m->header->opcode));
    }
}

int pspexp_pump(PspExpContext *ctx, int timeout_ms)
{
    int count = 0;
    int wait = timeout_ms;
    for (;;) {
        PspExpMessage m;
        int r;
        long n = ctx->t->recv(ctx->t, ctx->rx, RX_BUFFER_SIZE, wait);
        if (n < 0) { link_lost(ctx); return -1; }
        if (n == 0) break;
        if (pspexp_stream_feed(&ctx->in, ctx->rx, (size_t)n) < 0) { link_lost(ctx); return -1; }
        while ((r = pspexp_stream_next(&ctx->in, &m)) == 1) { handle(ctx, &m); count++; }
        if (r < 0) { ctx_log(ctx, "inbound stream corrupt"); link_lost(ctx); return -1; }
        wait = 0;  /* drain whatever else is already there */
    }
    return count;
}

int pspexp_connect(PspExpContext *ctx, int timeout_ms)
{
    uint64_t deadline = pspexp_time_us() + (uint64_t)timeout_ms * 1000u;
    while (ctx->state != PSPEXP_LINK_CONNECTED) {
        uint64_t now = pspexp_time_us();
        int remaining;
        if (now >= deadline) return -1;
        remaining = (int)((deadline - now) / 1000u);
        if (remaining < 1) remaining = 1;
        if (pspexp_pump(ctx, remaining) < 0) return -1;
    }
    return 0;
}

int pspexp_upload_texture(PspExpContext *ctx, uint16_t id, const PspExpTextureDesc *desc,
                          const void *pixels, uint32_t pixel_bytes,
                          const PspExpFrame *frames, uint16_t frame_count)
{
    PspExpTextureUpload t;
    uint16_t formats = ctx->state == PSPEXP_LINK_CONNECTED ? ctx->hello.texture_formats : PSPEXP_TEX_FORMATS_V01;
    uint32_t payload_len;
    int reason;
    uint16_t i;
    Resource *r;
    uint8_t *p;

    memset(&t, 0, sizeof(t));
    t.id = id;
    t.width = desc->width;
    t.height = desc->height;
    t.format = desc->format;
    t.layout = desc->layout;
    t.priority = desc->priority;
    t.frame_count = frame_count;
    t.data_length = pixel_bytes;
    payload_len = pspexp_texture_payload_length(pixel_bytes, frame_count);

    /* Host-side construction rules (section 5.4): frames no wider than 64 px. */
    for (i = 0; i < frame_count; i++)
        if (frames[i].w > PSPEXP_FRAME_MAX_WIDTH) {
            ctx_log(ctx, "texture %u frame %u is %u px wide (max %u)", id, i, frames[i].w, PSPEXP_FRAME_MAX_WIDTH);
            return -PSPEXP_RERR_BAD_FRAME_TABLE;
        }

    pspexp_writer_reset(&ctx->msg);
    if (pspexp_writer_begin(&ctx->msg, PSPEXP_CH_RESOURCE, PSPEXP_OP_RESOURCE_UPLOAD_TEXTURE, 0) < 0) return -1;
    p = (uint8_t *)pspexp_writer_reserve(&ctx->msg, sizeof(t) + payload_len);
    if (!p) return -1;
    memcpy(p, &t, sizeof(t));
    memcpy(p + sizeof(t), pixels, pixel_bytes);
    if (frame_count) memcpy(p + sizeof(t) + PSPEXP_ALIGN_UP(pixel_bytes), frames, (size_t)frame_count * sizeof(PspExpFrame));
    if (pspexp_writer_end(&ctx->msg) < 0) return -1;

    /* Validate exactly as the executor will, so mistakes surface on the desktop. */
    reason = pspexp_texture_validate((const PspExpTextureUpload *)p, formats, payload_len);
    if (reason != PSPEXP_RERR_NONE) {
        ctx_log(ctx, "texture %u rejected locally: %s", id, pspexp_resource_error_name((uint16_t)reason));
        return -reason;
    }

    r = add_res(ctx, id);
    if (!r) return -1;
    if (r->retained) { free(r->retained); r->retained = NULL; }
    if (ctx->cfg.retain_uploads) {
        r->retained = (uint8_t *)malloc(sizeof(t) + payload_len);
        if (r->retained) { memcpy(r->retained, p, sizeof(t) + payload_len); r->retained_len = (uint32_t)(sizeof(t) + payload_len); }
    }
    r->state = PSPEXP_RES_PENDING;
    if (ctx->state != PSPEXP_LINK_CONNECTED) return 0;   /* sent on connect */
    return send_bytes(ctx, ctx->msg.buf, ctx->msg.len);
}

int pspexp_release(PspExpContext *ctx, uint16_t id)
{
    PspExpResourceId rid;
    Resource *r = find_res(ctx, id);
    int was_known = r && (r->state == PSPEXP_RES_PENDING || r->state == PSPEXP_RES_RESIDENT);
    if (r) remove_res(ctx, r);
    if (!was_known || ctx->state != PSPEXP_LINK_CONNECTED) return 0;
    memset(&rid, 0, sizeof(rid));
    rid.id = id;
    return pspexp_send_raw(ctx, PSPEXP_CH_RESOURCE, PSPEXP_OP_RESOURCE_RELEASE, &rid, sizeof(rid));
}

int pspexp_hint(PspExpContext *ctx, uint16_t id, uint8_t priority)
{
    PspExpResourceHint h;
    memset(&h, 0, sizeof(h));
    h.id = id;
    h.priority = priority;
    return pspexp_send_raw(ctx, PSPEXP_CH_RESOURCE, PSPEXP_OP_RESOURCE_HINT, &h, sizeof(h));
}

PspExpResourceState pspexp_resource_state(const PspExpContext *ctx, uint16_t id, uint8_t *tier)
{
    size_t i;
    for (i = 0; i < ctx->nres; i++)
        if (ctx->res[i].id == id) {
            if (tier) *tier = ctx->res[i].tier;
            return (PspExpResourceState)ctx->res[i].state;
        }
    return PSPEXP_RES_NONE;
}

int pspexp_memory_query(PspExpContext *ctx)
{
    return pspexp_send_raw(ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_MEMORY_QUERY, NULL, 0);
}

/* ---- frames ------------------------------------------------------------ */

void pspexp_begin_frame(PspExpContext *ctx)
{
    PspExpBeginFrame b;
    FrameRecord *rec;
    pspexp_writer_reset(&ctx->frame);
    memset(&b, 0, sizeof(b));
    b.frame_seq = ctx->next_frame_seq;
    if (ctx->have_input) { b.input_seq = ctx->input.sequence; b.input_timestamp_us = ctx->input.timestamp_us; }
    pspexp_writer_message(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_BEGIN_FRAME, 0, &b, sizeof(b));
    rec = &ctx->ring[b.frame_seq % SEND_RING];
    rec->seq = b.frame_seq;
    rec->input_timestamp_us = b.input_timestamp_us;
    ctx->in_frame = 1;
    ctx->stats.last_frame_batches = 0;
    ctx->stats.last_frame_sprites = 0;
}

void pspexp_set_clip(PspExpContext *ctx, int16_t x, int16_t y, int16_t w, int16_t h)
{
    PspExpSetClip c;
    memset(&c, 0, sizeof(c));
    c.x = x; c.y = y; c.w = w; c.h = h;
    pspexp_writer_message(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_SET_CLIP, 0, &c, sizeof(c));
}

static uint32_t batch_limit(const PspExpContext *ctx)
{
    uint32_t lim = ctx->cfg.max_batch_records;
    if (ctx->state == PSPEXP_LINK_CONNECTED && ctx->hello.max_batch_records && ctx->hello.max_batch_records < lim)
        lim = ctx->hello.max_batch_records;
    return lim;
}

void pspexp_draw_sprite_batch(PspExpContext *ctx, uint16_t atlas, const PspExpSpriteRec *recs, uint32_t count)
{
    const uint32_t lim = batch_limit(ctx);
    while (count > 0) {
        uint32_t n = count > lim ? lim : count;
        PspExpSpriteBatch *b;
        if (pspexp_writer_begin(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_DRAW_SPRITE_BATCH, 0) < 0) return;
        b = (PspExpSpriteBatch *)pspexp_writer_reserve(&ctx->frame, sizeof(*b));
        if (!b) return;
        b->atlas = atlas;
        b->count = (uint16_t)n;
        b->vertex_format = PSPEXP_VTX_SPRITE_2D_5650;
        pspexp_writer_append(&ctx->frame, recs, (size_t)n * sizeof(PspExpSpriteRec));
        pspexp_writer_end(&ctx->frame);
        ctx->stats.last_frame_batches++;
        ctx->stats.last_frame_sprites += n;
        recs += n;
        count -= n;
    }
}

void pspexp_draw_sprite(PspExpContext *ctx, uint16_t atlas, int16_t x, int16_t y, uint16_t frame, uint16_t color)
{
    PspExpSpriteRec r;
    r.x = x; r.y = y; r.frame = frame; r.color = color;
    pspexp_draw_sprite_batch(ctx, atlas, &r, 1);
}

void pspexp_draw_tile_batch(PspExpContext *ctx, uint16_t atlas, int16_t origin_x, int16_t origin_y,
                            uint16_t cell_w, uint16_t cell_h, uint16_t columns,
                            const PspExpTileRec *recs, uint32_t count)
{
    const uint32_t lim = batch_limit(ctx);
    uint32_t done = 0;
    if (columns == 0) columns = 1;
    while (done < count) {
        uint32_t n = count - done > lim ? lim : count - done;
        PspExpTileBatch *b;
        /* Each split starts on a row boundary so the grid stays consistent. */
        if (n < count - done) n -= n % columns;
        if (n == 0) n = columns < count - done ? columns : count - done;
        if (pspexp_writer_begin(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_DRAW_TILE_BATCH, 0) < 0) return;
        b = (PspExpTileBatch *)pspexp_writer_reserve(&ctx->frame, sizeof(*b));
        if (!b) return;
        b->atlas = atlas;
        b->count = (uint16_t)n;
        b->origin_x = origin_x;
        b->origin_y = (int16_t)(origin_y + (int32_t)(done / columns) * cell_h);
        b->cell_w = cell_w;
        b->cell_h = cell_h;
        b->columns = columns;
        b->vertex_format = PSPEXP_VTX_SPRITE_2D_5650;
        pspexp_writer_append(&ctx->frame, recs + done, (size_t)n * sizeof(PspExpTileRec));
        pspexp_writer_end(&ctx->frame);
        ctx->stats.last_frame_batches++;
        ctx->stats.last_frame_sprites += n;
        done += n;
    }
}

void pspexp_draw_rect(PspExpContext *ctx, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    PspExpDrawRect r;
    memset(&r, 0, sizeof(r));
    r.x = x; r.y = y; r.w = w; r.h = h; r.color = color;
    pspexp_writer_message(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_DRAW_RECT, 0, &r, sizeof(r));
}

long pspexp_present(PspExpContext *ctx)
{
    uint32_t seq = ctx->next_frame_seq;
    FrameRecord *rec;
    if (!ctx->in_frame) return -1;
    pspexp_writer_message(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_PRESENT, 0, NULL, 0);
    ctx->in_frame = 0;
    if (ctx->frame.error) { ctx_log(ctx, "frame %u too large", seq); return -1; }
    rec = &ctx->ring[seq % SEND_RING];
    rec->sent_us = pspexp_time_us();
    ctx->stats.last_frame_bytes = (uint32_t)ctx->frame.len;
    if (ctx->state != PSPEXP_LINK_CONNECTED) return -1;
    if (send_bytes(ctx, ctx->frame.buf, ctx->frame.len) < 0) return -1;
    ctx->stats.frames_sent++;
    ctx->last_sent_seq = seq;
    ctx->next_frame_seq++;
    ctx->in_flight++;
    return (long)seq;
}

int pspexp_should_render(const PspExpContext *ctx)
{
    return ctx->state == PSPEXP_LINK_CONNECTED && ctx->in_flight < ctx->cfg.max_in_flight;
}

int pspexp_wait_present(PspExpContext *ctx, int timeout_ms)
{
    uint64_t deadline = pspexp_time_us() + (uint64_t)timeout_ms * 1000u;
    while (!pspexp_should_render(ctx)) {
        uint64_t now = pspexp_time_us();
        int remaining;
        if (ctx->state != PSPEXP_LINK_CONNECTED) return -1;
        if (now >= deadline) return 1;
        remaining = (int)((deadline - now) / 1000u);
        if (remaining < 1) remaining = 1;
        if (pspexp_pump(ctx, remaining) < 0) return -1;
    }
    return 0;
}
