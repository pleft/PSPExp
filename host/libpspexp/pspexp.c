#include "pspexp.h"
#include "pspexp_local_audio.h"
#include "pspexp_time.h"
#include "../../protocol/serialization/pspexp_writer.h"
#include "../../runtime/pspexp_cache.h"   /* PSPEXP_KIND_* for replay ordering */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "pspexp_capture.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

#define RX_BUFFER_SIZE (256u * 1024u)
#define SEND_RING 32

typedef struct Resource {
    uint16_t id;
    uint8_t  state;     /* PspExpResourceState */
    uint8_t  tier;
    uint8_t  kind;      /* PspExpResourceKind: palettes replay before textures */
    uint16_t opcode;    /* upload opcode for replay */
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
    uint8_t *pending_upload;            /* between pspexp_upload_texture_begin and _commit */
    uint8_t *batch; size_t batch_len, batch_cap;   /* sends gathered into one transfer */
    int batching;
    uint32_t pending_upload_len;
    int pending_patch;           /* a RESOURCE_PATCH begun */
    uint16_t pending_id;                /* between pspexp_upload_mesh_begin and _commit */
    PspExpStream in;
    PspExpWriter frame;
    size_t last_draw;            /* where in `frame` the last 3D_DRAW's header is (pspexp_3d_draw_more) */
    size_t body_start, mark;     /* the frame's draws begin there; pspexp_frame_mark: the kept ones end there */
    uint8_t *kept; size_t kept_len, kept_cap;   /* the last frame's draws, for pspexp_replay_frame (0: none) */
    PspExpWriter msg;
    uint8_t *rx;
    PspExpLinkState state;
    int ever_connected;
    int maintenance;   /* the runtime speaks another protocol version: only SYSTEM and FILE (§6.1) */
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
    PspExpAudioStatus audio;
    PspExpLocalAudio la;                /* sound on the board's output (Bluetooth): pspexp_local_audio.c */
    int la_was;
    uint64_t osd_until;                 /* the PSP's volume bar shows until then */
    PspExpStats stats;
    PspExpLogFn log; void *log_user;
    PspExpMessageHook hook; void *hook_user;
    /* the FILE channel: the last reply, matched to a request by its id */
    uint32_t fs_next_id, fs_reply_id, fs_reply_len;
    int fs_have;
    uint8_t *fs_reply;
    /* CLOCK_STATUS, the answer to CLOCK_SET */
    int clock_have;
    PspExpClockStatus clock_status;
    /* POWER_STATUS, the answer to POWER_QUERY: when it came (0 none on this connection) */
    uint64_t power_at;
    PspExpPowerStatus power;
    /* the PSP's keyboard (TEXT_REQUEST): asked and not answered yet (no frames meanwhile); the answer */
    int text_pending, text_have, text_status;
    char text[1025];
    /* PSPExp Capture inside PSPExp: the receiver's socket while the PSP captures (-1 none); the
     * mode its hello gave (0: not yet: the PSP not asked) */
    int cap_fd, cap_mode, cap_got;
    uint8_t cap_hello[32];
    uint64_t cap_dropped, cap_tried;
};

static void ctx_log(PspExpContext *ctx, const char *fmt, ...)
{
    char buf[1100];   /* the byte tally (PSPEXP_TRACE_BYTES) is one long line */
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
    cfg->audio_rate = 44100;
    cfg->audio_channels = 2;
    cfg->max_batch_records = PSPEXP_BATCH_MAX_RECORDS;
    cfg->max_in_flight = 1;
    cfg->retain_uploads = 1;
}

PspExpContext *pspexp_create(PspExpTransport *transport, const PspExpConfig *cfg)
{
    PspExpContext *ctx = (PspExpContext *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->t = transport;
    pspexp_la_init(&ctx->la);
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
    ctx->cap_fd = -1;
    return ctx;
}

/* ---- PSPExp Capture inside PSPExp ----
 * With a receiver there (pspexp-capture, which a board's console runs) and a PSP that can
 * (PSPEXP_FEATURE_CAPTURE), the PSP is asked to capture what it shows and plays, and the bytes
 * it sends back (CAPTURE_DATA) go to the receiver's socket as they come. Every program holding
 * the PSP does so through this library, so the receiver hears from each in turn. A receiver that
 * cannot keep up loses data, never the game its pace: the socket never blocks. PSPEXP_CAPTURE=0:
 * not asked. */
static void capture_close(PspExpContext *ctx)
{
    if (ctx->cap_fd >= 0) close(ctx->cap_fd);
    ctx->cap_fd = -1;
    ctx->cap_mode = ctx->cap_got = 0;
}

static void capture_open(PspExpContext *ctx)
{
    struct sockaddr_un a;
    const char *e = getenv("PSPEXP_CAPTURE");
    int fd, big = 4 << 20;
    capture_close(ctx);
    ctx->cap_tried = pspexp_time_us();
    if (!(ctx->hello.features & PSPEXP_FEATURE_CAPTURE) || (e && !strcmp(e, "0"))) return;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    pspexp_capture_socket_path(a.sun_path, sizeof(a.sun_path));
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) return;
#ifdef SO_NOSIGPIPE
    { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); }   /* (a receiver gone: no SIGPIPE into the game) */
#endif
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return; }   /* (no receiver: nothing asked) */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
    ctx->cap_fd = fd;
    ctx->cap_mode = ctx->cap_got = 0;   /* (the PSP asked once the receiver's hello says what it keeps: capture_hello) */
}

/* the receiver's hello ("PXCH", its PSPEXP_CAPTURE_*: the last 30 seconds kept, or recordings
 * only), read as it comes; then the PSP asked */
static void capture_hello(PspExpContext *ctx)
{
    PspExpCaptureSet c;
    ssize_t r = recv(ctx->cap_fd, ctx->cap_hello + ctx->cap_got, sizeof(ctx->cap_hello) - (size_t)ctx->cap_got, MSG_DONTWAIT);
    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { capture_close(ctx); return; }
    if (r < 0) return;
    ctx->cap_got += (int)r;
    if (ctx->cap_got < (int)sizeof(ctx->cap_hello)) return;
    if (memcmp(ctx->cap_hello, "PXCH", 4) != 0) { capture_close(ctx); return; }
    memset(&c, 0, sizeof(c));
    c.on = ctx->cap_hello[4] == PSPEXP_CAPTURE_RECORDINGS ? PSPEXP_CAPTURE_RECORDINGS : PSPEXP_CAPTURE_ALL;
    ctx->cap_mode = (int)c.on;
    pspexp_send_raw(ctx, PSPEXP_CH_DEBUG, PSPEXP_OP_CAPTURE_SET, &c, sizeof(c));
    ctx_log(ctx, c.on == PSPEXP_CAPTURE_ALL ? "PSPExp Capture: the PSP sends what it shows to the receiver (♪+R, ♪+L)"
                                             : "PSPExp Capture: the PSP sends what it shows to the receiver during ♪+L recordings");
}

static void capture_data(PspExpContext *ctx, const PspExpMessage *m)
{
    const PspExpCaptureData *d = (const PspExpCaptureData *)m->payload;
    const uint8_t *p = (const uint8_t *)m->payload + sizeof(*d);
    PspExpCaptureSet off;
    size_t n;
    if (ctx->cap_fd < 0 || !ctx->cap_mode || m->length < sizeof(*d)) return;
    n = d->bytes < m->length - sizeof(*d) ? d->bytes : m->length - sizeof(*d);
    while (n) {
#ifdef MSG_NOSIGNAL
        ssize_t w = send(ctx->cap_fd, p, n, MSG_NOSIGNAL);
#else
        ssize_t w = send(ctx->cap_fd, p, n, 0);
#endif
        if (w > 0) { p += w; n -= (size_t)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { ctx->cap_dropped += n; return; }   /* (the receiver resyncs at the next packet) */
        if (w < 0 && errno == EINTR) continue;
        capture_close(ctx);   /* the receiver went: the PSP stops sending until one is back */
        memset(&off, 0, sizeof(off));
        pspexp_send_raw(ctx, PSPEXP_CH_DEBUG, PSPEXP_OP_CAPTURE_SET, &off, sizeof(off));
        return;
    }
}

void pspexp_destroy(PspExpContext *ctx)
{
    size_t i;
    if (!ctx) return;
    pspexp_la_close(&ctx->la);
    capture_close(ctx);
    if (ctx->t && ctx->t->close) ctx->t->close(ctx->t);
    for (i = 0; i < ctx->nres; i++) free(ctx->res[i].retained);
    free(ctx->res);
    free(ctx->rx);
    free(ctx->batch);
    free(ctx->kept);
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
const PspExpAudioStatus *pspexp_audio_status(const PspExpContext *ctx) { return &ctx->audio; }
PspExpTransport *pspexp_transport(PspExpContext *ctx) { return ctx->t; }
int pspexp_frames_in_flight(const PspExpContext *ctx) { return ctx->in_flight; }
void pspexp_set_max_in_flight(PspExpContext *ctx, int n) { ctx->cfg.max_in_flight = n < 1 ? 1 : n; }

static void link_lost(PspExpContext *ctx)
{
    size_t i;
    if (ctx->state != PSPEXP_LINK_DISCONNECTED) ctx_log(ctx, "link lost");
    ctx->state = PSPEXP_LINK_DISCONNECTED;
    ctx->in_flight = 0;
    for (i = 0; i < ctx->nres; i++)
        if (ctx->res[i].state != PSPEXP_RES_FAILED) ctx->res[i].state = PSPEXP_RES_NONE;
    pspexp_stream_reset(&ctx->in);
    ctx->batch_len = 0;   /* half a batch is worth nothing to a new link */
    capture_close(ctx);   /* (the receiver waits for the next holder of the PSP) */
}

/* PSPEXP_RECORD=FILE: every byte sent, in order (debugging: tools replay it) */
static void record(const void *data, size_t len)
{
    static FILE *f;
    static int opened;
    if (!opened) { const char *p = getenv("PSPEXP_RECORD"); opened = 1; if (p && *p) f = fopen(p, "wb"); }
    if (f) { fwrite(data, 1, len, f); fflush(f); }
}

static int send_now(PspExpContext *ctx, const void *data, size_t len)
{
    if (!ctx->t) return -1;
    record(data, len);
    if (ctx->t->send(ctx->t, data, len) < 0) { ctx_log(ctx, "send failed (%u bytes)", (unsigned)len); link_lost(ctx); return -1; }
    return 0;
}

int pspexp_batch_flush(PspExpContext *ctx)
{
    int rc = 0;
    if (ctx->batch_len) rc = send_now(ctx, ctx->batch, ctx->batch_len);
    ctx->batch_len = 0;
    ctx->batching = 0;
    return rc;
}

void pspexp_batch_begin(PspExpContext *ctx)
{
    if (!ctx->batch) {
        ctx->batch_cap = 192u * 1024u;
        ctx->batch = (uint8_t *)malloc(ctx->batch_cap);
        if (!ctx->batch) { ctx->batch_cap = 0; return; }
    }
    ctx->batching = 1;
}

/* PSPEXP_TRACE_BYTES=1: what the bytes sent were, by message, in the log every 300 frames (=N above 1:
 * every N frames) */
static int trace_bytes = -1;
static uint32_t trace_every = 300;
static uint64_t trace_op_bytes[64][2];   /* [opcode hash] -> opcode, bytes */

static void trace_tally(const void *data, size_t len)
{
    size_t off = 0;
    while (off + PSPEXP_HEADER_SIZE <= len) {
        const PspExpMessageHeader *h = (const PspExpMessageHeader *)(const void *)((const uint8_t *)data + off);
        unsigned k = (h->opcode * 2654435761u) >> 26, i;
        for (i = 0; i < 64; i++, k = (k + 1) & 63)
            if (!trace_op_bytes[k][1] || trace_op_bytes[k][0] == h->opcode) {
                trace_op_bytes[k][0] = h->opcode; trace_op_bytes[k][1] += PSPEXP_HEADER_SIZE + h->length; break;
            }
        off += PSPEXP_HEADER_SIZE + h->length;
    }
}

static void trace_report(PspExpContext *ctx, uint32_t frames)
{
    static uint64_t last_us;
    uint64_t now = pspexp_time_us();
    char line[1024];
    int n = snprintf(line, sizeof(line), "bytes a frame over %u frames (%.1f fps):", frames,
                     last_us && now > last_us ? frames * 1e6 / (double)(now - last_us) : 0.0), i;
    last_us = now;
    uint64_t total = 0;
    for (i = 0; i < 64; i++) total += trace_op_bytes[i][1];
    for (i = 0; i < 64 && n < (int)sizeof(line) - 48; i++)
        if (trace_op_bytes[i][1]) n += snprintf(line + n, sizeof(line) - (size_t)n, " %s %llu", pspexp_opcode_name((uint16_t)trace_op_bytes[i][0]),
                                                (unsigned long long)(trace_op_bytes[i][1] / frames));
    ctx_log(ctx, "%s (total %llu)", line, (unsigned long long)(total / frames));
    memset(trace_op_bytes, 0, sizeof(trace_op_bytes));
}

static int send_bytes(PspExpContext *ctx, const void *data, size_t len)
{
    if (!ctx->t) return -1;
    if (trace_bytes < 0) {
        trace_bytes = getenv("PSPEXP_TRACE_BYTES") && atoi(getenv("PSPEXP_TRACE_BYTES")) > 0;
        if (trace_bytes && atoi(getenv("PSPEXP_TRACE_BYTES")) > 1) trace_every = (uint32_t)atoi(getenv("PSPEXP_TRACE_BYTES"));
    }
    if (trace_bytes) trace_tally(data, len);
    if (ctx->batching) {
        if (ctx->batch_len + len > ctx->batch_cap) {   /* full: out with what is there */
            if (ctx->batch_len && send_now(ctx, ctx->batch, ctx->batch_len) < 0) { ctx->batch_len = 0; return -1; }
            ctx->batch_len = 0;
        }
        if (len <= ctx->batch_cap) {
            memcpy(ctx->batch + ctx->batch_len, data, len);
            ctx->batch_len += len;
            ctx->stats.bytes_sent += len;
            return 0;
        }
    }
    record(data, len);
    if (ctx->t->send(ctx->t, data, len) < 0) { ctx_log(ctx, "send failed (%u bytes)", (unsigned)len); link_lost(ctx); return -1; }
    ctx->stats.bytes_sent += len;
    return 0;
}

void pspexp_set_transport(PspExpContext *ctx, PspExpTransport *transport)
{
    /* NULL closes the current transport and leaves the context without a
     * link (resources kept for replay); a later call installs the new one. */
    if (ctx->t && ctx->t != transport && ctx->t->close) ctx->t->close(ctx->t);
    ctx->t = transport;
    link_lost(ctx);
}

int pspexp_request_screenshot(PspExpContext *ctx, uint16_t index)
{
    PspExpScreenshot sc;
    memset(&sc, 0, sizeof(sc));
    sc.index = index;
    return pspexp_send_raw(ctx, PSPEXP_CH_DEBUG, PSPEXP_OP_DEBUG_SCREENSHOT, &sc, sizeof(sc));
}

/* ---- the PSP's clock ---------------------------------------------------------------------- */

int pspexp_clock_supported(const PspExpContext *ctx)
{
    return ctx->state == PSPEXP_LINK_CONNECTED && (ctx->hello.features & PSPEXP_FEATURE_CLOCK) != 0;
}

int pspexp_clock_sync(PspExpContext *ctx, uint64_t utc, int tz_minutes, int dst, uint32_t flags, uint32_t min_diff, PspExpClockStatus *out)
{
    PspExpClockSet c;
    uint64_t deadline;
    if (!pspexp_clock_supported(ctx)) return -1;
    memset(&c, 0, sizeof(c));
    c.utc_lo = (uint32_t)utc; c.utc_hi = (uint32_t)(utc >> 32);
    c.tz_minutes = tz_minutes;
    c.min_diff = min_diff;
    c.dst = dst;
    c.flags = flags;
    ctx->clock_have = 0;
    if (pspexp_send_raw(ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_CLOCK_SET, &c, sizeof(c)) < 0) return -1;
    deadline = pspexp_time_us() + 3000000u;
    while (!ctx->clock_have) {
        if (pspexp_pump(ctx, 20) < 0 || pspexp_time_us() > deadline) return -1;
    }
    if (out) *out = ctx->clock_status;
    return ctx->clock_status.result;
}

/* ---- the PSP's battery ---------------------------------------------------------------------- */

int pspexp_power_supported(const PspExpContext *ctx)
{
    return ctx->state == PSPEXP_LINK_CONNECTED && (ctx->hello.features & PSPEXP_FEATURE_POWER) != 0;
}

int pspexp_power_query(PspExpContext *ctx)
{
    if (!pspexp_power_supported(ctx)) return -1;
    return pspexp_send_raw(ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_POWER_QUERY, NULL, 0);
}

int pspexp_power_status(const PspExpContext *ctx, PspExpPowerStatus *out, uint64_t *age_us)
{
    if (!ctx->power_at || ctx->state != PSPEXP_LINK_CONNECTED) return -1;
    if (out) *out = ctx->power;
    if (age_us) *age_us = pspexp_time_us() - ctx->power_at;
    return 0;
}

/* ---- the PSP's keyboard ------------------------------------------------------------------ */

int pspexp_text_supported(const PspExpContext *ctx)
{
    return ctx->state == PSPEXP_LINK_CONNECTED && (ctx->hello.features & PSPEXP_FEATURE_TEXT) != 0;
}

int pspexp_text_request(PspExpContext *ctx, const char *title, const char *text, unsigned max_chars)
{
    uint8_t b[sizeof(PspExpTextRequest) + 128 + 512];
    PspExpTextRequest q;
    size_t tl = title ? strlen(title) : 0, xl = text ? strlen(text) : 0;
    if (!pspexp_text_supported(ctx) || ctx->text_pending || ctx->in_frame) return -1;
    if (tl > 127) tl = 127;
    if (xl > 511) xl = 511;
    memset(&q, 0, sizeof(q));
    q.max_chars = (uint16_t)(max_chars ? max_chars : 64); q.title_len = (uint16_t)tl; q.text_len = (uint16_t)xl;
    memcpy(b, &q, sizeof(q));
    if (tl) memcpy(b + sizeof(q), title, tl);
    if (xl) memcpy(b + sizeof(q) + tl, text, xl);
    if (pspexp_send_raw(ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_TEXT_REQUEST, b, sizeof(q) + tl + xl) < 0) return -1;
    ctx->text_pending = 1; ctx->text_have = 0;
    return 0;
}

int pspexp_text_pending(const PspExpContext *ctx) { return ctx->text_pending && ctx->state == PSPEXP_LINK_CONNECTED; }

int pspexp_text_result(PspExpContext *ctx, char *out, size_t cap, int *status)
{
    if (!ctx->text_have) return 0;
    ctx->text_have = 0;
    if (status) *status = ctx->text_status;
    if (out && cap) { size_t n = strlen(ctx->text); if (n > cap - 1) n = cap - 1; memcpy(out, ctx->text, n); out[n] = 0; }
    return 1;
}

/* ---- FILE: the PSP's memory stick ------------------------------------------------------ */

int pspexp_fs_supported(const PspExpContext *ctx)
{
    return ctx->state == PSPEXP_LINK_CONNECTED && (ctx->hello.features & PSPEXP_FEATURE_FILES) != 0;
}

/* one request and its reply: the status (or PSPEXP_FSERR_LINK), the reply's
 * header in *rep, what follows it in out (up to cap; its length in *out_len) */
static int fs_call(PspExpContext *ctx, uint16_t op, const char *path, const char *path2, uint64_t offset,
                   uint32_t length, uint32_t flags, const void *data, PspExpFsReply *rep, void *out, uint32_t cap, uint32_t *out_len)
{
    static uint8_t *buf;
    PspExpFsRequest *q;
    uint64_t deadline;
    uint32_t id, n = (uint32_t)sizeof(*q) + (data ? length : 0);
    if (!pspexp_fs_supported(ctx)) return PSPEXP_FSERR_UNSUPPORTED;
    if (strlen(path) >= PSPEXP_FS_PATH || (path2 && strlen(path2) >= PSPEXP_FS_PATH)) return PSPEXP_FSERR_PATH;
    if (!buf && !(buf = (uint8_t *)malloc(sizeof(*q) + PSPEXP_FS_CHUNK + 16))) return PSPEXP_FSERR_IO;
    q = (PspExpFsRequest *)buf;
    memset(q, 0, sizeof(*q));
    id = ++ctx->fs_next_id;
    q->req_id = id;
    q->flags = flags;
    q->offset_lo = (uint32_t)offset; q->offset_hi = (uint32_t)(offset >> 32);
    q->length = length;
    strcpy(q->path, path);
    if (path2) strcpy(q->path2, path2);
    if (data && length) memcpy(buf + sizeof(*q), data, length);
    ctx->fs_have = 0;
    if (pspexp_send_raw(ctx, PSPEXP_CH_FILE, op, buf, PSPEXP_ALIGN_UP(n)) < 0) return PSPEXP_FSERR_LINK;
    deadline = pspexp_time_us() + 15000000u;   /* a slow memory stick, a slow Wi-Fi: 15 s */
    while (!(ctx->fs_have && ctx->fs_reply_id == id)) {
        if (pspexp_pump(ctx, 20) < 0 || pspexp_time_us() > deadline) return PSPEXP_FSERR_LINK;
    }
    memcpy(rep, ctx->fs_reply, sizeof(*rep));
    if (out) {
        uint32_t have = ctx->fs_reply_len - (uint32_t)sizeof(*rep);
        if (have > cap) have = cap;
        memcpy(out, ctx->fs_reply + sizeof(*rep), have);
        if (out_len) *out_len = have;
    }
    return rep->status;
}

int pspexp_fs_list(PspExpContext *ctx, const char *path, uint32_t first, PspExpFsEntry *out, int max, int *more)
{
    PspExpFsReply r;
    uint32_t got = 0;
    int st = fs_call(ctx, PSPEXP_OP_FS_LIST, path, NULL, first, 0, 0, NULL, &r, out, (uint32_t)max * (uint32_t)sizeof(PspExpFsEntry), &got);
    if (st < 0) return st;
    if (more) *more = (int)r.more;
    return (int)(got / sizeof(PspExpFsEntry));
}

int pspexp_fs_stat(PspExpContext *ctx, const char *path, PspExpFsEntry *out)
{
    PspExpFsReply r;
    return fs_call(ctx, PSPEXP_OP_FS_STAT, path, NULL, 0, 0, 0, NULL, &r, out, sizeof(*out), NULL);
}

int pspexp_fs_read(PspExpContext *ctx, const char *path, uint64_t offset, void *buf, uint32_t len)
{
    PspExpFsReply r;
    uint32_t got = 0;
    int st;
    if (len > PSPEXP_FS_CHUNK) len = PSPEXP_FS_CHUNK;
    st = fs_call(ctx, PSPEXP_OP_FS_READ, path, NULL, offset, len, 0, NULL, &r, buf, len, &got);
    return st < 0 ? st : (int)(got < (uint32_t)st ? got : (uint32_t)st);
}

int pspexp_fs_write(PspExpContext *ctx, const char *path, uint64_t offset, const void *data, uint32_t len, uint32_t flags)
{
    PspExpFsReply r;
    if (len > PSPEXP_FS_CHUNK) return PSPEXP_FSERR_PATH;
    return fs_call(ctx, PSPEXP_OP_FS_WRITE, path, NULL, offset, len, flags, data ? data : "", &r, NULL, 0, NULL);
}

int pspexp_fs_mkdir(PspExpContext *ctx, const char *path)
{ PspExpFsReply r; return fs_call(ctx, PSPEXP_OP_FS_MKDIR, path, NULL, 0, 0, 0, NULL, &r, NULL, 0, NULL); }

int pspexp_fs_remove(PspExpContext *ctx, const char *path)
{ PspExpFsReply r; return fs_call(ctx, PSPEXP_OP_FS_REMOVE, path, NULL, 0, 0, 0, NULL, &r, NULL, 0, NULL); }

int pspexp_fs_rename(PspExpContext *ctx, const char *from, const char *to)
{ PspExpFsReply r; return fs_call(ctx, PSPEXP_OP_FS_RENAME, from, to, 0, 0, 0, NULL, &r, NULL, 0, NULL); }

int pspexp_fs_info(PspExpContext *ctx, const char *path, uint64_t *free_bytes, uint64_t *total_bytes)
{
    PspExpFsReply r;
    int st = fs_call(ctx, PSPEXP_OP_FS_INFO, path, NULL, 0, 0, 0, NULL, &r, NULL, 0, NULL);
    if (st >= 0) {
        if (free_bytes) *free_bytes = (uint64_t)r.value_lo | ((uint64_t)r.value_hi << 32);
        if (total_bytes) *total_bytes = (uint64_t)r.value2_lo | ((uint64_t)r.value2_hi << 32);
    }
    return st;
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
    {   /* PSPEXP_TEST_PROTOCOL: act as a host of that version (tests of maintenance mode) */
        const char *tp = getenv("PSPEXP_TEST_PROTOCOL");
        unsigned mine = tp && *tp ? (unsigned)atoi(tp) : PSPEXP_PROTOCOL_VERSION;
        ctx->maintenance = 0;
        if (ctx->hello.protocol != mine) {
            if (!(ctx->hello.features & PSPEXP_FEATURE_FILES)) {
                ctx_log(ctx, "protocol %u not supported (this host speaks %u), and the runtime has no file channel to update it", ctx->hello.protocol, mine);
                return;
            }
            /* the frozen core (HELLO's first fields, the framing, FILE) still works:
             * connect for maintenance only, to install a matching runtime */
            memset(&ack, 0, sizeof(ack));
            ack.protocol = (uint16_t)mine;
            ack.flags = PSPEXP_ACK_MAINTENANCE;
            if (pspexp_send_raw(ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_HELLO_ACK, &ack, sizeof(ack)) < 0) return;
            ctx->maintenance = 1;
            ctx->state = PSPEXP_LINK_CONNECTED;
            ctx_log(ctx, "maintenance: the PSP's runtime speaks protocol %u, this host %u: files only", ctx->hello.protocol, mine);
            return;
        }
    }
    if (ctx->ever_connected) {
        ctx->stats.reconnects++;
        ctx_log(ctx, "reconnect %u", ctx->stats.reconnects);
    }
    ctx->ever_connected = 1;
    ctx->in_flight = 0;
    for (i = 0; i < ctx->nres; i++) ctx->res[i].state = PSPEXP_RES_NONE;

    memset(&ack, 0, sizeof(ack));
    ack.protocol = PSPEXP_PROTOCOL_VERSION;
    ack.input_rate = ctx->cfg.input_rate;
    if (ctx->hello.input_rate_max && ack.input_rate > ctx->hello.input_rate_max) ack.input_rate = ctx->hello.input_rate_max;
    ack.audio_rate = ctx->cfg.audio_rate;
    ack.audio_channels = ctx->cfg.audio_channels;
    if (ctx->cfg.depth_buffer && (ctx->hello.features & PSPEXP_FEATURE_3D)) ack.flags |= PSPEXP_ACK_DEPTH;
    if (pspexp_send_raw(ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_HELLO_ACK, &ack, sizeof(ack)) < 0) return;
    ctx->state = PSPEXP_LINK_CONNECTED;
    ctx_log(ctx, "connected: executor %u, %ux%u, vram %u KiB, ram %u KiB, formats 0x%x",
            ctx->hello.executor, ctx->hello.display_width, ctx->hello.display_height,
            ctx->hello.vram_budget / 1024, ctx->hello.ram_budget / 1024, ctx->hello.texture_formats);
    capture_open(ctx);

    /* Replay the working set (section 9.2): palettes first, since indexed
     * textures require theirs to be resident at upload. */
    {
        int pass;
        for (pass = 0; pass < 2; pass++)
            for (i = 0; i < ctx->nres; i++) {
                Resource *r = &ctx->res[i];
                if (!r->retained) continue;
                if ((pass == 0) != (r->kind == PSPEXP_KIND_PALETTE)) continue;
                if (pspexp_send_raw(ctx, PSPEXP_CH_RESOURCE, r->opcode, r->retained, r->retained_len) < 0) return;
                r->state = PSPEXP_RES_PENDING;
            }
    }
}

static void handle(PspExpContext *ctx, const PspExpMessage *m)
{
    ctx->stats.bytes_received += PSPEXP_HEADER_SIZE + m->length;
    if (ctx->hook && ctx->hook(ctx->hook_user, ctx, m)) return;

    switch (m->header->opcode) {
    case PSPEXP_OP_HELLO:
        ctx->power_at = 0;   /* (another PSP, or the same after a restart) */
        ctx->text_pending = 0;   /* (a keyboard that PSP had open is gone with it) */
        on_hello(ctx, m);
        break;
    case PSPEXP_OP_CAPTURE_DATA:
        capture_data(ctx, m);
        break;
    case PSPEXP_OP_TEXT_RESULT:
        if (m->length >= sizeof(PspExpTextResult)) {
            const PspExpTextResult *r = (const PspExpTextResult *)m->payload;
            size_t n = r->text_len;
            if (n > m->length - sizeof(*r)) n = m->length - sizeof(*r);
            if (n > sizeof(ctx->text) - 1) n = sizeof(ctx->text) - 1;
            memcpy(ctx->text, (const uint8_t *)m->payload + sizeof(*r), n); ctx->text[n] = 0;
            ctx->text_status = r->status; ctx->text_have = 1; ctx->text_pending = 0;
        }
        break;
    case PSPEXP_OP_POWER_STATUS:
        if (m->length >= sizeof(PspExpPowerStatus)) { memcpy(&ctx->power, m->payload, sizeof(ctx->power)); ctx->power_at = pspexp_time_us(); }
        break;
    case PSPEXP_OP_CLOCK_STATUS:
        if (m->length >= sizeof(PspExpClockStatus)) { memcpy(&ctx->clock_status, m->payload, sizeof(ctx->clock_status)); ctx->clock_have = 1; }
        break;
    case PSPEXP_OP_FS_REPLY: {
        uint32_t cap = (uint32_t)sizeof(PspExpFsReply) + PSPEXP_FS_CHUNK;
        if (m->length < sizeof(PspExpFsReply)) break;
        if (!ctx->fs_reply && !(ctx->fs_reply = (uint8_t *)malloc(cap))) break;
        ctx->fs_reply_len = m->length < cap ? m->length : cap;
        memcpy(ctx->fs_reply, m->payload, ctx->fs_reply_len);
        ctx->fs_reply_id = ((const PspExpFsReply *)m->payload)->req_id;
        ctx->fs_have = 1;
        break;
    }
    case PSPEXP_OP_PRESENT_DONE: {
        const PspExpPresentDone *d;
        FrameRecord *rec;
        if (pspexp_message_expect(m, sizeof(*d)) < 0) break;
        d = (const PspExpPresentDone *)m->payload;
        ctx->stats.frames_presented++;
        ctx->stats.frames_dropped_remote = d->dropped;
        ctx->stats.last_execute_us = d->execute_us;
        ctx->stats.last_ge_us = d->ge_us;
        ctx->stats.last_psp_sprites = d->sprites_drawn;
        ctx->stats.last_psp_dropped = d->sprites_dropped;
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
        pspexp_la_buttons(&ctx->la, ctx->input.buttons, pspexp_time_us());   /* volume, while the sound is on Bluetooth */
        if (ctx->la.vol_report) { ctx->la.vol_report = 0; pspexp_osd_volume(ctx, ctx->la.vol_now); }   /* the PSP shows it over the frames */
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
    case PSPEXP_OP_AUDIO_STATUS:
        if (pspexp_message_expect(m, sizeof(PspExpAudioStatus)) < 0) break;
        memcpy(&ctx->audio, m->payload, sizeof(ctx->audio));
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
    if (!ctx->t) { link_lost(ctx); return -1; }
    for (;;) {
        PspExpMessage m;
        int r;
        long n = ctx->t->recv(ctx->t, ctx->rx, RX_BUFFER_SIZE, wait);
        if (n < 0) { ctx_log(ctx, "receive failed"); link_lost(ctx); return -1; }
        if (n == 0) break;
        if (pspexp_stream_feed(&ctx->in, ctx->rx, (size_t)n) < 0) { link_lost(ctx); return -1; }
        while ((r = pspexp_stream_next(&ctx->in, &m)) == 1) { handle(ctx, &m); count++; }
        if (r < 0) { ctx_log(ctx, "inbound stream corrupt"); link_lost(ctx); return -1; }
        wait = 0;  /* drain whatever else is already there */
    }
    /* a receiver that started after the PSP connected (the console starts it then): looked for
     * every 2 s */
    if (ctx->state == PSPEXP_LINK_CONNECTED && !ctx->maintenance && ctx->cap_fd < 0 &&
        (ctx->hello.features & PSPEXP_FEATURE_CAPTURE) && pspexp_time_us() - ctx->cap_tried >= 2000000)
        capture_open(ctx);
    if (ctx->cap_fd >= 0 && !ctx->cap_mode && ctx->state == PSPEXP_LINK_CONNECTED) capture_hello(ctx);
    return count;
}

int pspexp_connect(PspExpContext *ctx, int timeout_ms)
{
    uint64_t deadline = pspexp_time_us() + (uint64_t)timeout_ms * 1000u;
    ctx->kept_len = 0;   /* a new link: nothing of the kept frame is resident */
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

/* Two-step upload: begin reserves the message and returns where the pixel
 * bytes go (the caller writes them in place: no intermediate buffer), commit
 * validates, records the resource and sends it. retain = 0 skips the copy
 * kept for replay after a reconnect (textures re-sent every frame). */
uint8_t *pspexp_upload_texture_begin(PspExpContext *ctx, uint16_t id, const PspExpTextureDesc *desc,
                                     uint32_t pixel_bytes, const PspExpFrame *frames, uint16_t frame_count)
{
    PspExpTextureUpload t;
    uint32_t payload_len;
    uint16_t i;
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
    t.palette = desc->palette;
    payload_len = pspexp_texture_payload_length(pixel_bytes, frame_count);

    /* Host-side construction rules (section 5.4): frames no wider than 64 px. */
    for (i = 0; i < frame_count; i++)
        if (frames[i].w > PSPEXP_FRAME_MAX_WIDTH) {
            ctx_log(ctx, "texture %u frame %u is %u px wide (max %u)", id, i, frames[i].w, PSPEXP_FRAME_MAX_WIDTH);
            return NULL;
        }

    pspexp_writer_reset(&ctx->msg);
    if (pspexp_writer_begin(&ctx->msg, PSPEXP_CH_RESOURCE, PSPEXP_OP_RESOURCE_UPLOAD_TEXTURE, 0) < 0) return NULL;
    p = (uint8_t *)pspexp_writer_reserve(&ctx->msg, sizeof(t) + payload_len);
    if (!p) return NULL;
    memcpy(p, &t, sizeof(t));
    if (frame_count) memcpy(p + sizeof(t) + PSPEXP_ALIGN_UP(pixel_bytes), frames, (size_t)frame_count * sizeof(PspExpFrame));
    ctx->pending_upload = p;
    ctx->pending_upload_len = (uint32_t)(sizeof(t) + payload_len);
    return p + sizeof(t);
}

int pspexp_upload_texture_commit(PspExpContext *ctx, int retain)
{
    uint16_t formats = ctx->state == PSPEXP_LINK_CONNECTED ? ctx->hello.texture_formats : PSPEXP_TEX_FORMATS_V01;
    const PspExpTextureUpload *t = (const PspExpTextureUpload *)ctx->pending_upload;
    uint32_t len = ctx->pending_upload_len;
    int reason;
    Resource *r;

    if (!t) return -1;
    ctx->pending_upload = NULL;
    if (pspexp_writer_end(&ctx->msg) < 0) return -1;

    /* Validate exactly as the executor will, so mistakes surface on the desktop. */
    reason = pspexp_texture_validate(t, formats, len - (uint32_t)sizeof(*t));
    if (reason != PSPEXP_RERR_NONE) {
        ctx_log(ctx, "texture %u rejected locally: %s", t->id, pspexp_resource_error_name((uint16_t)reason));
        return -reason;
    }

    if (PSPEXP_TEX_IS_INDEXED(t->format)) {
        uint8_t pt = 0;
        if (pspexp_resource_state(ctx, t->palette, &pt) == PSPEXP_RES_NONE) {
            ctx_log(ctx, "texture %u: palette %u was not uploaded", t->id, t->palette);
            return -PSPEXP_RERR_BAD_PALETTE;
        }
    }

    r = add_res(ctx, t->id);
    if (!r) return -1;
    if (r->retained) { free(r->retained); r->retained = NULL; }
    r->kind = PSPEXP_KIND_TEXTURE;
    r->opcode = PSPEXP_OP_RESOURCE_UPLOAD_TEXTURE;
    if (ctx->cfg.retain_uploads && retain) {
        r->retained = (uint8_t *)malloc(len);
        if (r->retained) { memcpy(r->retained, t, len); r->retained_len = len; }
    }
    r->state = PSPEXP_RES_PENDING;
    if (ctx->state != PSPEXP_LINK_CONNECTED) return 0;   /* sent on connect (if retained) */
    return send_bytes(ctx, ctx->msg.buf, ctx->msg.len);
}

int pspexp_upload_texture(PspExpContext *ctx, uint16_t id, const PspExpTextureDesc *desc,
                          const void *pixels, uint32_t pixel_bytes,
                          const PspExpFrame *frames, uint16_t frame_count)
{
    uint8_t *dst;
    uint16_t i;
    for (i = 0; i < frame_count; i++)
        if (frames[i].w > PSPEXP_FRAME_MAX_WIDTH) {
            ctx_log(ctx, "texture %u frame %u is %u px wide (max %u)", id, i, frames[i].w, PSPEXP_FRAME_MAX_WIDTH);
            return -PSPEXP_RERR_BAD_FRAME_TABLE;
        }
    dst = pspexp_upload_texture_begin(ctx, id, desc, pixel_bytes, frames, frame_count);
    if (!dst) return -1;
    memcpy(dst, pixels, pixel_bytes);
    return pspexp_upload_texture_commit(ctx, 1);
}

int pspexp_upload_palette(PspExpContext *ctx, uint16_t id, uint8_t format, uint16_t entries,
                          const void *colors, uint32_t bytes)
{
    PspExpPaletteUpload pu;
    uint32_t need = pspexp_palette_data_length(format, entries);
    uint8_t *p;
    Resource *r;
    int reason;
    if (need == 0 || bytes != need) {
        ctx_log(ctx, "palette %u rejected locally: %u entries of format %u need %u bytes, got %u", id, entries, format, need, bytes);
        return -PSPEXP_RERR_BAD_PALETTE;
    }
    memset(&pu, 0, sizeof(pu));
    pu.id = id;
    pu.format = format;
    pu.entries = entries;
    pspexp_writer_reset(&ctx->msg);
    if (pspexp_writer_begin(&ctx->msg, PSPEXP_CH_RESOURCE, PSPEXP_OP_RESOURCE_UPLOAD_PALETTE, 0) < 0) return -1;
    p = (uint8_t *)pspexp_writer_reserve(&ctx->msg, sizeof(pu) + PSPEXP_ALIGN_UP(bytes));
    if (!p) return -1;
    memcpy(p, &pu, sizeof(pu));
    memcpy(p + sizeof(pu), colors, bytes);
    if (pspexp_writer_end(&ctx->msg) < 0) return -1;
    reason = pspexp_palette_validate((const PspExpPaletteUpload *)p, PSPEXP_ALIGN_UP(bytes));
    if (reason != PSPEXP_RERR_NONE) return -reason;

    r = add_res(ctx, id);
    if (!r) return -1;
    if (r->retained) { free(r->retained); r->retained = NULL; }
    r->kind = PSPEXP_KIND_PALETTE;
    r->opcode = PSPEXP_OP_RESOURCE_UPLOAD_PALETTE;
    if (ctx->cfg.retain_uploads) {
        uint32_t len = (uint32_t)(sizeof(pu) + PSPEXP_ALIGN_UP(bytes));
        r->retained = (uint8_t *)malloc(len);
        if (r->retained) { memcpy(r->retained, p, len); r->retained_len = len; }
    }
    r->state = PSPEXP_RES_PENDING;
    if (ctx->state != PSPEXP_LINK_CONNECTED) return 0;
    return send_bytes(ctx, ctx->msg.buf, ctx->msg.len);
}

int pspexp_upload_sound(PspExpContext *ctx, uint16_t id, const int16_t *pcm, uint32_t frames, uint8_t channels)
{
    PspExpSoundUpload su;
    uint32_t bytes = pspexp_sound_data_length(channels, frames);
    uint8_t *p;
    Resource *r;
    if (bytes == 0) { ctx_log(ctx, "sound %u rejected locally: %u frames x %u channels", id, frames, channels); return -PSPEXP_RERR_BAD_FORMAT; }
    memset(&su, 0, sizeof(su));
    su.id = id;
    su.channels = channels;
    su.frames = frames;
    su.sample_rate = ctx->cfg.audio_rate;
    pspexp_writer_reset(&ctx->msg);
    if (pspexp_writer_begin(&ctx->msg, PSPEXP_CH_AUDIO, PSPEXP_OP_RESOURCE_UPLOAD_SOUND, 0) < 0) return -1;
    p = (uint8_t *)pspexp_writer_reserve(&ctx->msg, sizeof(su) + PSPEXP_ALIGN_UP(bytes));
    if (!p) return -1;
    memcpy(p, &su, sizeof(su));
    memcpy(p + sizeof(su), pcm, bytes);
    if (pspexp_writer_end(&ctx->msg) < 0) return -1;
    r = add_res(ctx, id);
    if (!r) return -1;
    if (r->retained) { free(r->retained); r->retained = NULL; }
    r->kind = PSPEXP_KIND_SOUND;
    r->opcode = PSPEXP_OP_RESOURCE_UPLOAD_SOUND;
    if (ctx->cfg.retain_uploads) {
        uint32_t len = (uint32_t)(sizeof(su) + PSPEXP_ALIGN_UP(bytes));
        r->retained = (uint8_t *)malloc(len);
        if (r->retained) { memcpy(r->retained, p, len); r->retained_len = len; }
    }
    r->state = PSPEXP_RES_PENDING;
    if (ctx->state != PSPEXP_LINK_CONNECTED) return 0;
    return send_bytes(ctx, ctx->msg.buf, ctx->msg.len);
}

int pspexp_play_sound(PspExpContext *ctx, uint16_t id, uint8_t volume, int8_t pan, uint8_t loop)
{
    PspExpPlaySound ps;
    memset(&ps, 0, sizeof(ps));
    ps.id = id; ps.volume = volume; ps.pan = pan; ps.loop = loop;
    return pspexp_send_raw(ctx, PSPEXP_CH_AUDIO, PSPEXP_OP_PLAY_SOUND, &ps, sizeof(ps));
}

int pspexp_stop_sound(PspExpContext *ctx, uint16_t id)
{
    PspExpStopSound ss;
    memset(&ss, 0, sizeof(ss));
    ss.id = id;
    return pspexp_send_raw(ctx, PSPEXP_CH_AUDIO, PSPEXP_OP_STOP_SOUND, &ss, sizeof(ss));
}

int pspexp_audio_stream(PspExpContext *ctx, const int16_t *stereo, uint32_t frames)
{
    PspExpAudioStream as;
    uint8_t *p;
    if (frames == 0) return 0;
    if (pspexp_audio_local(ctx)) {
        ctx->stats.audio_frames_sent += frames;
        return pspexp_la_write(&ctx->la, stereo, frames);
    }
    if (ctx->state != PSPEXP_LINK_CONNECTED) return -1;
    memset(&as, 0, sizeof(as));
    as.frames = frames;
    pspexp_writer_reset(&ctx->msg);
    if (pspexp_writer_begin(&ctx->msg, PSPEXP_CH_AUDIO, PSPEXP_OP_AUDIO_STREAM, 0) < 0) return -1;
    p = (uint8_t *)pspexp_writer_reserve(&ctx->msg, sizeof(as) + PSPEXP_ALIGN_UP(frames * 4u));
    if (!p) return -1;
    memcpy(p, &as, sizeof(as));
    memcpy(p + sizeof(as), stereo, (size_t)frames * 4u);
    if (pspexp_writer_end(&ctx->msg) < 0) return -1;
    ctx->stats.audio_frames_sent += frames;
    return send_bytes(ctx, ctx->msg.buf, ctx->msg.len);
}

int pspexp_audio_mp3_supported(const PspExpContext *ctx)
{
    return ctx->state == PSPEXP_LINK_CONNECTED && !ctx->maintenance && (ctx->hello.features & PSPEXP_FEATURE_AUDIO_MP3) != 0;
}

int pspexp_audio_coded(PspExpContext *ctx, int codec, const void *frames, uint32_t bytes)
{
    PspExpAudioCoded ac;
    uint8_t *p;
    if (bytes == 0) return 0;
    if (ctx->state != PSPEXP_LINK_CONNECTED) return -1;
    memset(&ac, 0, sizeof(ac));
    ac.codec = (uint8_t)codec;
    ac.bytes = bytes;
    pspexp_writer_reset(&ctx->msg);
    if (pspexp_writer_begin(&ctx->msg, PSPEXP_CH_AUDIO, PSPEXP_OP_AUDIO_CODED_STREAM, 0) < 0) return -1;
    p = (uint8_t *)pspexp_writer_reserve(&ctx->msg, sizeof(ac) + PSPEXP_ALIGN_UP(bytes));
    if (!p) return -1;
    memcpy(p, &ac, sizeof(ac));
    memcpy(p + sizeof(ac), frames, bytes);
    memset(p + sizeof(ac) + bytes, 0, PSPEXP_ALIGN_UP(bytes) - bytes);
    if (pspexp_writer_end(&ctx->msg) < 0) return -1;
    return send_bytes(ctx, ctx->msg.buf, ctx->msg.len);
}

int pspexp_audio_local(PspExpContext *ctx)
{
    int on = pspexp_la_active(&ctx->la, ctx->cfg.audio_rate, pspexp_time_us());
    if (on && !ctx->la_was && ctx->state == PSPEXP_LINK_CONNECTED)
        pspexp_send_raw(ctx, PSPEXP_CH_AUDIO, PSPEXP_OP_AUDIO_STREAM_RESET, NULL, 0);   /* what the PSP still holds: not twice */
    ctx->la_was = on;
    return on;
}

int pspexp_osd_volume(PspExpContext *ctx, int percent)
{
    PspExpOsdVolume v;
    if (!(ctx->hello.features & PSPEXP_FEATURE_OSD) || ctx->state != PSPEXP_LINK_CONNECTED) return -1;
    memset(&v, 0, sizeof(v));
    v.percent = (int8_t)(percent > 100 ? 100 : percent);
    ctx->osd_until = percent < 0 ? 0 : pspexp_time_us() + 1500000u;
    return pspexp_send_raw(ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_OSD_VOLUME, &v, sizeof(v));
}

uint64_t pspexp_osd_until(const PspExpContext *ctx) { return ctx->osd_until; }

int pspexp_restart_supported(const PspExpContext *ctx)
{
    return ctx->state == PSPEXP_LINK_CONNECTED && (ctx->hello.features & PSPEXP_FEATURE_RESTART);
}

int pspexp_restart_psp(PspExpContext *ctx)
{
    if (!pspexp_restart_supported(ctx)) return -1;
    return pspexp_send_raw(ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_RESTART, NULL, 0);
}

int pspexp_boot_disc_supported(const PspExpContext *ctx)
{
    return ctx->state == PSPEXP_LINK_CONNECTED && (ctx->hello.features & PSPEXP_FEATURE_BOOT_DISC);
}

int pspexp_boot_disc(PspExpContext *ctx, const char *path)
{
    PspExpBootDisc b;
    if (!pspexp_boot_disc_supported(ctx) || !path || strlen(path) >= sizeof(b.path)) return -1;
    memset(&b, 0, sizeof(b));
    snprintf(b.path, sizeof(b.path), "%s", path);
    return pspexp_send_raw(ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_BOOT_DISC, &b, sizeof(b));
}

int pspexp_audio_stream_reset(PspExpContext *ctx)
{
    if (pspexp_audio_local(ctx)) pspexp_la_reset(&ctx->la);
    return pspexp_send_raw(ctx, PSPEXP_CH_AUDIO, PSPEXP_OP_AUDIO_STREAM_RESET, NULL, 0);
}

int pspexp_release(PspExpContext *ctx, uint16_t id)
{
    PspExpResourceId rid;
    Resource *r = find_res(ctx, id);
    int was_known = r && (r->state == PSPEXP_RES_PENDING || r->state == PSPEXP_RES_RESIDENT);
    if (r) remove_res(ctx, r);
    ctx->kept_len = 0;   /* the kept frame may draw it */
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
    ctx->body_start = ctx->frame.len; ctx->mark = 0;
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
    pspexp_draw_sprite_batch_ex(ctx, atlas, recs, count, 0, PSPEXP_PALETTE_DEFAULT, PSPEXP_VTX_SPRITE_2D_5650);
}

void pspexp_draw_sprite_batch_ex(PspExpContext *ctx, uint16_t atlas, const PspExpSpriteRec *recs, uint32_t count,
                                 uint16_t flags, uint16_t palette, uint16_t vertex_format)
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
        b->vertex_format = vertex_format;
        b->flags = flags;
        b->palette = palette;
        pspexp_writer_append(&ctx->frame, recs, (size_t)n * sizeof(PspExpSpriteRec));
        pspexp_writer_end(&ctx->frame);
        ctx->stats.last_frame_batches++;
        ctx->stats.last_frame_sprites += n;
        recs += n;
        count -= n;
    }
}

void pspexp_draw_quad_batch(PspExpContext *ctx, uint16_t atlas, const PspExpQuadRec *recs, uint32_t count,
                            uint16_t palette, uint16_t vertex_format)
{
    const uint32_t lim = batch_limit(ctx);
    while (count > 0) {
        uint32_t n = count > lim ? lim : count;
        PspExpSpriteBatch *b;
        if (pspexp_writer_begin(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_DRAW_QUAD_BATCH, 0) < 0) return;
        b = (PspExpSpriteBatch *)pspexp_writer_reserve(&ctx->frame, sizeof(*b));
        if (!b) return;
        b->atlas = atlas;
        b->count = (uint16_t)n;
        b->vertex_format = vertex_format;
        b->flags = 0;
        b->palette = palette;
        pspexp_writer_append(&ctx->frame, recs, (size_t)n * sizeof(PspExpQuadRec));
        pspexp_writer_end(&ctx->frame);
        ctx->stats.last_frame_batches++;
        ctx->stats.last_frame_sprites += n;
        recs += n;
        count -= n;
    }
}

void pspexp_draw_quad_ex_batch(PspExpContext *ctx, uint16_t atlas, const PspExpQuadExRec *recs, uint32_t count,
                               uint16_t palette, uint16_t vertex_format)
{
    const uint32_t lim = batch_limit(ctx);
    while (count > 0) {
        uint32_t n = count > lim ? lim : count;
        PspExpSpriteBatch *b;
        if (pspexp_writer_begin(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_DRAW_QUAD_BATCH, 0) < 0) return;
        b = (PspExpSpriteBatch *)pspexp_writer_reserve(&ctx->frame, sizeof(*b));
        if (!b) return;
        b->atlas = atlas;
        b->count = (uint16_t)n;
        b->vertex_format = vertex_format;
        b->flags = PSPEXP_BATCH_SCALED;
        b->palette = palette;
        pspexp_writer_append(&ctx->frame, recs, (size_t)n * sizeof(PspExpQuadExRec));
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
    pspexp_draw_tile_batch_ex(ctx, atlas, origin_x, origin_y, cell_w, cell_h, columns, recs, count, PSPEXP_PALETTE_DEFAULT);
}

void pspexp_draw_tile_batch_ex(PspExpContext *ctx, uint16_t atlas, int16_t origin_x, int16_t origin_y,
                               uint16_t cell_w, uint16_t cell_h, uint16_t columns,
                               const PspExpTileRec *recs, uint32_t count, uint16_t palette)
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
        b->palette = palette;
        pspexp_writer_append(&ctx->frame, recs + done, (size_t)n * sizeof(PspExpTileRec));
        pspexp_writer_end(&ctx->frame);
        ctx->stats.last_frame_batches++;
        ctx->stats.last_frame_sprites += n;
        done += n;
    }
}

void pspexp_fill_rect(PspExpContext *ctx, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color, uint8_t alpha)
{
    PspExpDrawRect r;
    memset(&r, 0, sizeof(r));
    r.x = x; r.y = y; r.w = w; r.h = h; r.color = color; r.alpha = alpha;
    pspexp_writer_message(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_DRAW_RECT, 0, &r, sizeof(r));
}

void pspexp_draw_rect(PspExpContext *ctx, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    pspexp_fill_rect(ctx, x, y, w, h, color, 255);
}

void pspexp_draw_line(PspExpContext *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color, uint8_t alpha)
{
    PspExpDrawLine l;
    memset(&l, 0, sizeof(l));
    l.x0 = x0; l.y0 = y0; l.x1 = x1; l.y1 = y1; l.color = color; l.alpha = alpha;
    pspexp_writer_message(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_DRAW_LINE, 0, &l, sizeof(l));
}

int pspexp_3d_supported(const PspExpContext *ctx)
{
    return ctx->state == PSPEXP_LINK_CONNECTED && !ctx->maintenance && (ctx->hello.features & PSPEXP_FEATURE_3D) != 0;
}

int pspexp_3d16_supported(const PspExpContext *ctx)
{
    return pspexp_3d_supported(ctx) && (ctx->hello.features & PSPEXP_FEATURE_3D16) != 0;
}

int pspexp_patch_supported(const PspExpContext *ctx)
{
    return ctx->state == PSPEXP_LINK_CONNECTED && !ctx->maintenance && (ctx->hello.features & PSPEXP_FEATURE_PATCH) != 0;
}

uint8_t *pspexp_patch_texture_begin(PspExpContext *ctx, uint16_t id, uint32_t offset, uint32_t length)
{
    PspExpResourcePatch p;
    uint8_t *out;
    if (!pspexp_patch_supported(ctx) || !length || pspexp_resource_state(ctx, id, NULL) == PSPEXP_RES_NONE) return NULL;
    memset(&p, 0, sizeof(p));
    p.id = id; p.offset = offset; p.length = length;
    pspexp_writer_reset(&ctx->msg);
    if (pspexp_writer_begin(&ctx->msg, PSPEXP_CH_RESOURCE, PSPEXP_OP_RESOURCE_PATCH, 0) < 0) return NULL;
    if (!(out = (uint8_t *)pspexp_writer_reserve(&ctx->msg, sizeof(p) + length))) return NULL;
    memcpy(out, &p, sizeof(p));
    ctx->pending_patch = 1;
    return out + sizeof(p);
}

int pspexp_patch_texture_commit(PspExpContext *ctx)
{
    if (!ctx->pending_patch) return -1;
    ctx->pending_patch = 0;
    if (pspexp_writer_end(&ctx->msg) < 0) return -1;
    return send_bytes(ctx, ctx->msg.buf, ctx->msg.len);
}

/* Connected already: the answer to HELLO again, now asking for it (the
 * runtime takes a repeated HELLO_ACK as a change of these settings). */
void pspexp_set_depth_buffer(PspExpContext *ctx, int on)
{
    PspExpHelloAck ack;
    if (ctx->cfg.depth_buffer == on) return;
    ctx->cfg.depth_buffer = on;
    if (!pspexp_3d_supported(ctx)) return;
    memset(&ack, 0, sizeof(ack));
    ack.protocol = PSPEXP_PROTOCOL_VERSION;
    ack.input_rate = ctx->cfg.input_rate;
    if (ctx->hello.input_rate_max && ack.input_rate > ctx->hello.input_rate_max) ack.input_rate = ctx->hello.input_rate_max;
    ack.audio_rate = ctx->cfg.audio_rate;
    ack.audio_channels = ctx->cfg.audio_channels;
    ack.flags = on ? PSPEXP_ACK_DEPTH : 0;
    pspexp_send_raw(ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_HELLO_ACK, &ack, sizeof(ack));
}

void pspexp_3d_state(PspExpContext *ctx, const PspExp3DState *s)
{
    pspexp_writer_message(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_3D_STATE, 0, s, sizeof(*s));
}

void pspexp_3d_matrices(PspExpContext *ctx, const float projection[16], const float modelview[16], const float texture[16])
{
    PspExp3DMatrices m;
    memcpy(m.projection, projection, sizeof(m.projection));
    memcpy(m.modelview, modelview, sizeof(m.modelview));
    memcpy(m.texture, texture, sizeof(m.texture));
    pspexp_writer_message(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_3D_MATRICES, 0, &m, sizeof(m));
}

void pspexp_3d_lights(PspExpContext *ctx, const PspExp3DLights *l)
{
    pspexp_writer_message(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_3D_LIGHTS, 0, l, sizeof(*l));
}

void pspexp_3d_clear(PspExpContext *ctx, uint32_t flags, uint32_t color, uint16_t depth)
{
    PspExp3DClear c;
    memset(&c, 0, sizeof(c));
    c.flags = flags; c.color = color; c.depth = depth;
    pspexp_writer_message(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_3D_CLEAR, 0, &c, sizeof(c));
}

int pspexp_3d_copy_supported(const PspExpContext *ctx)
{
    return pspexp_3d_supported(ctx) && (ctx->hello.features & PSPEXP_FEATURE_3D_COPY) != 0;
}

void pspexp_3d_copy(PspExpContext *ctx, uint16_t target, int sx, int sy, int w, int h, int dx, int dy, int flip)
{
    PspExp3DCopy c;
    memset(&c, 0, sizeof(c));
    c.target = target; c.flags = flip ? PSPEXP_3D_COPY_FLIP : 0;
    c.sx = (int16_t)sx; c.sy = (int16_t)sy; c.w = (uint16_t)w; c.h = (uint16_t)h; c.dx = (int16_t)dx; c.dy = (int16_t)dy;
    pspexp_writer_message(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_3D_COPY, 0, &c, sizeof(c));
}

int pspexp_3d_sprites_supported(const PspExpContext *ctx)
{
    return pspexp_3d_supported(ctx) && (ctx->hello.features & PSPEXP_FEATURE_3D_SPRITES) != 0;
}

int pspexp_3d_morph_supported(const PspExpContext *ctx)
{
    return pspexp_3d_supported(ctx) && (ctx->hello.features & PSPEXP_FEATURE_3D_MORPH) != 0;
}

void pspexp_3d_morph(PspExpContext *ctx, const float weight[8])
{
    PspExp3DMorph m;
    memcpy(m.weight, weight, sizeof(m.weight));
    pspexp_writer_message(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_3D_MORPH, 0, &m, sizeof(m));
}

void *pspexp_3d_draw(PspExpContext *ctx, uint8_t primitive, uint32_t vertex_type, uint32_t count)
{
    PspExp3DDraw d;
    uint32_t size = pspexp_3d_vertex_size(vertex_type);
    void *v;
    if (!size || !count || count > PSPEXP_3D_MAX_VERTICES || primitive > PSPEXP_3D_PRIM_MAX) return NULL;
    memset(&d, 0, sizeof(d));
    d.primitive = primitive; d.vertex_type = vertex_type; d.count = count;
    ctx->last_draw = ctx->frame.len;
    if (pspexp_writer_begin(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_3D_DRAW, 0) < 0 ||
        pspexp_writer_append(&ctx->frame, &d, sizeof(d)) < 0 ||
        !(v = pspexp_writer_reserve(&ctx->frame, PSPEXP_ALIGN_UP((size_t)size * count)))) return NULL;
    pspexp_writer_end(&ctx->frame);   /* already padded: nothing moves the reserved bytes */
    return v;
}

void *pspexp_3d_draw_more(PspExpContext *ctx, uint8_t primitive, uint32_t vertex_type, uint32_t count)
{
    PspExpWriter *w = &ctx->frame;
    PspExp3DDraw d;
    uint32_t size = pspexp_3d_vertex_size(vertex_type);
    uint8_t *p;
    /* lists of separate points, lines, triangles or sprites only (GE 0, 1, 3, 6): a strip or fan cannot go on */
    if (!size || !count || (primitive != 0 && primitive != 1 && primitive != 3 && primitive != PSPEXP_3D_PRIM_SPRITES)) return NULL;
    if (ctx->last_draw + PSPEXP_HEADER_SIZE + sizeof(d) > w->len) return NULL;
    memcpy(&d, w->buf + ctx->last_draw + PSPEXP_HEADER_SIZE, sizeof(d));
    if (((const PspExpMessageHeader *)(const void *)(w->buf + ctx->last_draw))->opcode != PSPEXP_OP_3D_DRAW ||
        d.primitive != primitive || d.vertex_type != vertex_type || d.index_count ||
        d.count + count > PSPEXP_3D_MAX_VERTICES || (size_t)size * (d.count + count) > 64u * 1024u) return NULL;
    if (!(p = (uint8_t *)pspexp_writer_regrow(w, ctx->last_draw, sizeof(d) + (size_t)size * (d.count + count)))) return NULL;
    p += sizeof(d) + (size_t)size * d.count;
    d.count += count;
    memcpy(w->buf + ctx->last_draw + PSPEXP_HEADER_SIZE, &d, sizeof(d));
    return p;
}

int pspexp_3d_indexed_supported(const PspExpContext *ctx)
{
    return pspexp_3d_supported(ctx) && (ctx->hello.features & PSPEXP_FEATURE_3D_INDEXED) != 0;
}

void *pspexp_3d_draw_indexed(PspExpContext *ctx, uint8_t primitive, uint32_t vertex_type, uint32_t count, uint32_t index_count,
                             uint16_t **indices)
{
    PspExp3DDraw d;
    uint32_t size = pspexp_3d_vertex_size(vertex_type), vbytes;
    uint8_t *v;
    if (!pspexp_3d_indexed_supported(ctx) || !size || !count || !index_count || count > PSPEXP_3D_MAX_VERTICES ||
        index_count > PSPEXP_3D_MAX_VERTICES || primitive > PSPEXP_3D_PRIM_MAX) return NULL;
    vbytes = PSPEXP_ALIGN4(size * count);
    memset(&d, 0, sizeof(d));
    d.primitive = primitive; d.vertex_type = vertex_type; d.count = count; d.index_count = index_count;
    if (pspexp_writer_begin(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_3D_DRAW, 0) < 0 ||
        pspexp_writer_append(&ctx->frame, &d, sizeof(d)) < 0 ||
        !(v = (uint8_t *)pspexp_writer_reserve(&ctx->frame, PSPEXP_ALIGN_UP((size_t)vbytes + (size_t)index_count * 2u)))) return NULL;
    pspexp_writer_end(&ctx->frame);
    *indices = (uint16_t *)(void *)(v + vbytes);
    return v;
}

void *pspexp_upload_mesh_begin(PspExpContext *ctx, uint16_t id, uint8_t kind, uint32_t vertex_type, uint32_t count)
{
    PspExpMeshUpload u;
    uint32_t size = kind == PSPEXP_MESH_VERTICES ? pspexp_3d_vertex_size(vertex_type) : kind == PSPEXP_MESH_INDICES16 ? 2 : 0;
    uint8_t *p;
    if (!size || !count) return NULL;
    memset(&u, 0, sizeof(u));
    u.id = id; u.kind = kind; u.vertex_type = vertex_type; u.count = count; u.data_length = size * count;
    pspexp_writer_reset(&ctx->msg);
    if (pspexp_writer_begin(&ctx->msg, PSPEXP_CH_RESOURCE, PSPEXP_OP_RESOURCE_UPLOAD_MESH, 0) < 0) return NULL;
    p = (uint8_t *)pspexp_writer_reserve(&ctx->msg, sizeof(u) + PSPEXP_ALIGN_UP(u.data_length));
    if (!p) return NULL;
    memcpy(p, &u, sizeof(u));
    ctx->pending_id = id;
    return p + sizeof(u);
}

int pspexp_upload_mesh_commit(PspExpContext *ctx)
{
    Resource *r;
    const uint8_t *payload;
    uint32_t len;
    if (pspexp_writer_end(&ctx->msg) < 0) return -1;
    payload = ctx->msg.buf + PSPEXP_HEADER_SIZE;
    len = (uint32_t)(ctx->msg.len - PSPEXP_HEADER_SIZE);
    r = add_res(ctx, ctx->pending_id);
    if (!r) return -1;
    if (r->retained) { free(r->retained); r->retained = NULL; }
    r->kind = PSPEXP_KIND_MESH;
    r->opcode = PSPEXP_OP_RESOURCE_UPLOAD_MESH;
    if (ctx->cfg.retain_uploads && (r->retained = (uint8_t *)malloc(len))) { memcpy(r->retained, payload, len); r->retained_len = len; }
    r->state = PSPEXP_RES_PENDING;
    if (ctx->state != PSPEXP_LINK_CONNECTED) return 0;
    return send_bytes(ctx, ctx->msg.buf, ctx->msg.len);
}

int pspexp_create_target(PspExpContext *ctx, uint16_t id, uint16_t width, uint16_t height, uint8_t format)
{
    PspExpTargetCreate c;
    Resource *r;
    memset(&c, 0, sizeof(c));
    c.id = id; c.width = width; c.height = height; c.format = format;
    pspexp_writer_reset(&ctx->msg);
    if (pspexp_writer_message(&ctx->msg, PSPEXP_CH_RESOURCE, PSPEXP_OP_RESOURCE_CREATE_TARGET, 0, &c, sizeof(c)) < 0) return -1;
    if (!(r = add_res(ctx, id))) return -1;
    if (r->retained) { free(r->retained); r->retained = NULL; }
    r->kind = PSPEXP_KIND_TEXTURE;
    r->opcode = PSPEXP_OP_RESOURCE_CREATE_TARGET;
    if ((r->retained = (uint8_t *)malloc(sizeof(c)))) { memcpy(r->retained, &c, sizeof(c)); r->retained_len = sizeof(c); }
    r->state = PSPEXP_RES_PENDING;
    if (ctx->state != PSPEXP_LINK_CONNECTED) return 0;
    return send_bytes(ctx, ctx->msg.buf, ctx->msg.len);
}

void pspexp_3d_target(PspExpContext *ctx, uint16_t id)
{
    PspExp3DTarget t;
    memset(&t, 0, sizeof(t));
    t.id = id;
    pspexp_writer_message(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_3D_TARGET, 0, &t, sizeof(t));
}

uint16_t *pspexp_3d_draw_mesh(PspExpContext *ctx, uint16_t vertices, uint16_t indices, uint8_t primitive,
                              uint32_t first, uint32_t count)
{
    static uint16_t none;
    PspExp3DDrawMesh d;
    uint16_t *p;
    if (!count || count > PSPEXP_3D_MAX_VERTICES || primitive > PSPEXP_3D_PRIM_MAX) return NULL;
    memset(&d, 0, sizeof(d));
    d.vertices = vertices; d.indices = indices; d.primitive = primitive; d.first = first; d.count = count;
    if (pspexp_writer_begin(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_3D_DRAW_MESH, 0) < 0 ||
        pspexp_writer_append(&ctx->frame, &d, sizeof(d)) < 0) return NULL;
    if (indices != PSPEXP_MESH_INLINE) { pspexp_writer_end(&ctx->frame); return &none; }
    if (!(p = (uint16_t *)pspexp_writer_reserve(&ctx->frame, PSPEXP_ALIGN_UP((size_t)count * 2u)))) return NULL;
    pspexp_writer_end(&ctx->frame);   /* already padded: nothing moves the reserved bytes */
    return p;
}

long pspexp_present(PspExpContext *ctx)
{
    uint32_t seq = ctx->next_frame_seq;
    FrameRecord *rec;
    if (!ctx->in_frame) return -1;
    if (!ctx->frame.error) {   /* its draws kept, up to the mark (what is drawn after it, a pointer, is not) */
        size_t end = ctx->mark ? ctx->mark : ctx->frame.len, n = end > ctx->body_start ? end - ctx->body_start : 0;
        if (n > ctx->kept_cap) { uint8_t *b = (uint8_t *)realloc(ctx->kept, n); if (b) { ctx->kept = b; ctx->kept_cap = n; } }
        if (n <= ctx->kept_cap) { if (n) memcpy(ctx->kept, ctx->frame.buf + ctx->body_start, n); ctx->kept_len = n; }
        else ctx->kept_len = 0;
    }
    pspexp_writer_message(&ctx->frame, PSPEXP_CH_GRAPHICS, PSPEXP_OP_PRESENT, 0, NULL, 0);
    ctx->in_frame = 0;
    if (ctx->frame.error) { ctx_log(ctx, "frame %u too large", seq); return -1; }
    {   /* PSPEXP_TRACE_FRAMES=N: the messages of the first N frames, in the log */
        static long trace = -1;
        if (trace < 0) trace = getenv("PSPEXP_TRACE_FRAMES") ? atol(getenv("PSPEXP_TRACE_FRAMES")) : 0;
        if (trace > 0 && (long)seq <= trace) {
            size_t off = 0;
            char line[1024];
            int n = snprintf(line, sizeof(line), "frame %u:", seq);
            while (off + PSPEXP_HEADER_SIZE <= ctx->frame.len && n < (int)sizeof(line) - 40) {
                const PspExpMessageHeader *h = (const PspExpMessageHeader *)(const void *)(ctx->frame.buf + off);
                n += snprintf(line + n, sizeof(line) - (size_t)n, " %s", pspexp_opcode_name(h->opcode));
                off += PSPEXP_HEADER_SIZE + h->length;
            }
            ctx_log(ctx, "%s", line);
        }
    }
    if (trace_bytes > 0 && seq % trace_every == 0) trace_report(ctx, trace_every);
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

void pspexp_frame_mark(PspExpContext *ctx) { if (ctx->in_frame) ctx->mark = ctx->frame.len; }

int pspexp_replay_frame(PspExpContext *ctx)
{
    if (!ctx->kept_len || ctx->in_frame || ctx->state != PSPEXP_LINK_CONNECTED) return -1;
    pspexp_begin_frame(ctx);
    if (pspexp_writer_raw(&ctx->frame, ctx->kept, ctx->kept_len) < 0) { ctx->in_frame = 0; return -1; }
    ctx->mark = ctx->frame.len;   /* kept again as it was: a pointer drawn over it is not (each replay's pointer had stayed, a trail) */
    return 0;
}

int pspexp_should_render(const PspExpContext *ctx)
{
    return ctx->state == PSPEXP_LINK_CONNECTED && !ctx->maintenance && !ctx->text_pending && ctx->in_flight < ctx->cfg.max_in_flight;   /* (the PSP's keyboard owns the screen until it answers) */
}

int pspexp_link_maintenance(const PspExpContext *ctx) { return ctx->state == PSPEXP_LINK_CONNECTED && ctx->maintenance; }

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
