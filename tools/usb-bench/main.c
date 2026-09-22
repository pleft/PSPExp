/*
 * pspexp-bench: measures the transport the architecture is parameterised on
 * (section 3.3, section 17). Works against the Null executor (loopback, a
 * pure CPU baseline) or a real PSP over USB.
 *
 *   host -> PSP throughput per block size
 *   PSP -> host throughput
 *   round-trip latency for small and frame-sized messages
 *   an end-to-end frame loop through libPSPExp with PRESENT_DONE pacing
 */
#include "../../host/libpspexp/pspexp.h"
#include "../../host/executor-null/pspexp_null.h"
#ifdef PSPEXP_HAVE_USB
#include "../../host/transport-usb/pspexp_usb.h"
#endif
#include "../../host/transport-tcp/pspexp_tcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Capture {
    int have_report;
    PspExpBenchReport report;
    int have_echo;
    uint32_t echo_len;
    uint64_t data_bytes;
    uint32_t data_msgs;
} Capture;

static int hook(void *user, PspExpContext *ctx, const PspExpMessage *m)
{
    Capture *c = (Capture *)user;
    (void)ctx;
    switch (m->header->opcode) {
    case PSPEXP_OP_BENCH_REPORT:
        if (m->length >= sizeof(PspExpBenchReport)) { memcpy(&c->report, m->payload, sizeof(c->report)); c->have_report = 1; }
        return 1;
    case PSPEXP_OP_BENCH_ECHO:
        c->have_echo = 1; c->echo_len = m->length;
        return 1;
    case PSPEXP_OP_BENCH_DATA:
        c->data_bytes += m->length; c->data_msgs++;
        return 1;
    default:
        return 0;
    }
}

static int wait_report(PspExpContext *ctx, Capture *c, int timeout_ms)
{
    uint64_t deadline = pspexp_time_us() + (uint64_t)timeout_ms * 1000u;
    c->have_report = 0;
    while (!c->have_report) {
        if (pspexp_time_us() >= deadline) return -1;
        if (pspexp_pump(ctx, 50) < 0) return -1;
    }
    return 0;
}

static double mbps(uint64_t bytes, uint64_t us) { return us ? (double)bytes / (double)us : 0.0; }

static int run_downlink(PspExpContext *ctx, Capture *c, uint32_t block, uint64_t total)
{
    PspExpBenchStart s;
    uint8_t *payload = (uint8_t *)malloc(block);
    uint64_t sent = 0, t0, t1;
    uint32_t msgs = 0, i;
    if (!payload) return -1;
    for (i = 0; i < block; i++) payload[i] = (uint8_t)(i * 7 + 3);
    memset(&s, 0, sizeof(s));
    s.expected_bytes = (uint32_t)total;
    s.block_size = block;
    if (pspexp_send_raw(ctx, PSPEXP_CH_DEBUG, PSPEXP_OP_BENCH_START, &s, sizeof(s)) < 0) { free(payload); return -1; }
    t0 = pspexp_time_us();
    while (sent < total) {
        if (pspexp_send_raw(ctx, PSPEXP_CH_DEBUG, PSPEXP_OP_BENCH_DATA, payload, block) < 0) { free(payload); return -1; }
        sent += block; msgs++;
        if ((msgs & 63) == 0 && pspexp_pump(ctx, 0) < 0) { free(payload); return -1; }
    }
    if (pspexp_send_raw(ctx, PSPEXP_CH_DEBUG, PSPEXP_OP_BENCH_END, NULL, 0) < 0) { free(payload); return -1; }
    if (wait_report(ctx, c, 30000) < 0) { printf("  no BENCH_REPORT\n"); free(payload); return -1; }
    t1 = pspexp_time_us();
    printf("  host->psp  block %7u B  %6u msgs  %8.2f MB/s host-timed  %8.2f MB/s psp-timed  (psp counted %u msgs, %u B)\n",
           block, msgs, mbps(sent, t1 - t0), mbps(c->report.bytes, c->report.elapsed_us), c->report.messages, c->report.bytes);
    free(payload);
    return 0;
}

static int run_uplink(PspExpContext *ctx, Capture *c, uint32_t block, uint64_t total)
{
    PspExpBenchUplink u;
    uint64_t t0, t1;
    memset(&u, 0, sizeof(u));
    u.block_size = block;
    u.count = (uint32_t)(total / block);
    c->data_bytes = 0; c->data_msgs = 0;
    t0 = pspexp_time_us();
    if (pspexp_send_raw(ctx, PSPEXP_CH_DEBUG, PSPEXP_OP_BENCH_UPLINK, &u, sizeof(u)) < 0) return -1;
    if (wait_report(ctx, c, 60000) < 0) { printf("  no BENCH_REPORT\n"); return -1; }
    t1 = pspexp_time_us();
    printf("  psp->host  block %7u B  %6u msgs  %8.2f MB/s host-timed  %8.2f MB/s psp-timed  (received %u msgs, %llu B)\n",
           block, u.count, mbps(c->data_bytes, t1 - t0), mbps(c->report.bytes, c->report.elapsed_us),
           c->data_msgs, (unsigned long long)c->data_bytes);
    return 0;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int run_echo(PspExpContext *ctx, Capture *c, uint32_t payload_len, int rounds)
{
    uint8_t *payload = (uint8_t *)calloc(1, payload_len ? payload_len : 1);
    uint32_t *samples = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)rounds);
    int i;
    uint64_t sum = 0;
    if (!payload || !samples) { free(payload); free(samples); return -1; }
    for (i = 0; i < rounds; i++) {
        uint64_t t0 = pspexp_time_us(), deadline = t0 + 5000000u;
        c->have_echo = 0;
        if (pspexp_send_raw(ctx, PSPEXP_CH_DEBUG, PSPEXP_OP_BENCH_ECHO, payload, payload_len) < 0) { free(payload); free(samples); return -1; }
        while (!c->have_echo) {
            if (pspexp_time_us() >= deadline) { printf("  echo timeout\n"); free(payload); free(samples); return -1; }
            if (pspexp_pump(ctx, 10) < 0) { free(payload); free(samples); return -1; }
        }
        samples[i] = (uint32_t)(pspexp_time_us() - t0);
        sum += samples[i];
    }
    qsort(samples, (size_t)rounds, sizeof(uint32_t), cmp_u32);
    printf("  echo %6u B x%-4d  min %6u us  median %6u us  mean %6u us  p95 %6u us  max %6u us\n",
           payload_len, rounds, samples[0], samples[rounds / 2], (uint32_t)(sum / (uint64_t)rounds),
           samples[(rounds * 95) / 100], samples[rounds - 1]);
    free(payload); free(samples);
    return 0;
}

/* frames x sprites, paced one frame in flight. With upload_kbps > 0, a
 * stream of upload_kib-sized textures is sent alongside at that rate
 * (section 17 workload c), to measure head-of-line blocking (section 10.3). */
static int run_frames(PspExpContext *ctx, int frames, int sprites, int upload_kbps, int upload_kib)
{
    static uint16_t pixels[64 * 64];
    static PspExpFrame atlas_frames[4];
    PspExpTextureDesc desc;
    PspExpSpriteRec *recs = (PspExpSpriteRec *)malloc(sizeof(PspExpSpriteRec) * (size_t)sprites);
    uint32_t *lat = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)frames);
    int f, i, timeouts = 0, uploads = 0, over17 = 0, over33 = 0;
    uint64_t t0, t1, lat_sum = 0, exec_sum = 0, ge_sum = 0;
    uint32_t lat_max = 0;
    uint16_t *big = NULL;
    PspExpFrame big_frame = { 0, 0, 64, 0 };
    PspExpTextureDesc big_desc;
    double upload_budget = 0.0, upload_per_frame = 0.0;
    uint16_t big_id = 100;
    if (!recs || !lat) { free(recs); free(lat); return -1; }
    if (upload_kbps > 0) {
        /* upload_kib KiB of RGB565: width 512, height = KiB * 1024 / 1024 = KiB rows */
        int h = upload_kib;
        if (h < 8) h = 8;
        if (h > 512) h = 512;
        { int p2 = 8; while (p2 < h) p2 <<= 1; h = p2; }
        big = (uint16_t *)calloc((size_t)512 * h, 2);
        memset(&big_desc, 0, sizeof(big_desc));
        big_desc.width = 512; big_desc.height = (uint16_t)h; big_desc.format = PSPEXP_TEX_5650;
        big_desc.layout = PSPEXP_LAYOUT_LINEAR; big_desc.priority = PSPEXP_PRIORITY_LOW;
        big_frame.h = (uint16_t)h;
        upload_per_frame = (double)upload_kbps * 1024.0 / 60.0;
        upload_kib = h;   /* actual KiB per upload (512 x h x 2 bytes = h KiB) */
    }
    for (i = 0; i < 64 * 64; i++) pixels[i] = pspexp_color_5650((uint8_t)(i & 255), (uint8_t)(i >> 4), 128);
    for (i = 0; i < 4; i++) { atlas_frames[i].u0 = (uint16_t)(i % 2 * 32); atlas_frames[i].v0 = (uint16_t)(i / 2 * 32); atlas_frames[i].w = 32; atlas_frames[i].h = 32; }
    memset(&desc, 0, sizeof(desc));
    desc.width = 64; desc.height = 64; desc.format = PSPEXP_TEX_5650; desc.layout = PSPEXP_LAYOUT_LINEAR; desc.priority = PSPEXP_PRIORITY_HIGH;
    if (pspexp_upload_texture(ctx, 1, &desc, pixels, sizeof(pixels), atlas_frames, 4) < 0) { printf("  upload failed\n"); free(recs); return -1; }
    for (i = 0; i < sprites; i++) { recs[i].x = (int16_t)((i * 37) % 448); recs[i].y = (int16_t)((i * 53) % 240); recs[i].frame = (uint16_t)(i & 3); recs[i].color = PSPEXP_COLOR_NONE; }

    t0 = pspexp_time_us();
    for (f = 0; f < frames; f++) {
        const PspExpStats *st;
        int w;
        if (big) {
            upload_budget += upload_per_frame;
            if (upload_budget >= (double)upload_kib * 1024.0) {
                upload_budget -= (double)upload_kib * 1024.0;
                /* Two rotating handles: release the older, upload the newer. */
                pspexp_release(ctx, (uint16_t)(big_id ^ 1));
                if (pspexp_upload_texture(ctx, big_id, &big_desc, big, (uint32_t)(512 * big_desc.height * 2), &big_frame, 1) < 0) {
                    printf("  upload failed\n"); break;
                }
                big_id ^= 1;
                uploads++;
            }
        }
        pspexp_begin_frame(ctx);
        pspexp_set_clip(ctx, 0, 0, PSPEXP_DISPLAY_WIDTH, PSPEXP_DISPLAY_HEIGHT);
        pspexp_draw_rect(ctx, 0, 0, PSPEXP_DISPLAY_WIDTH, PSPEXP_DISPLAY_HEIGHT, pspexp_color_5650(20, 30, 60));
        for (i = 0; i < sprites; i++) recs[i].x = (int16_t)((recs[i].x + 1) % 448);
        pspexp_draw_sprite_batch(ctx, 1, recs, (uint32_t)sprites);
        if (pspexp_present(ctx) < 0) { printf("  present failed\n"); free(recs); return -1; }
        w = pspexp_wait_present(ctx, 2000);
        if (w < 0) { printf("  link lost\n"); free(recs); return -1; }
        if (w > 0) timeouts++;
        st = pspexp_stats(ctx);
        lat[f] = st->last_present_latency_us;
        lat_sum += st->last_present_latency_us;
        exec_sum += st->last_execute_us;
        ge_sum += st->last_ge_us;
        if (st->last_present_latency_us > lat_max) lat_max = st->last_present_latency_us;
        if (st->last_present_latency_us > 17500) over17++;
        if (st->last_present_latency_us > 34000) over33++;
    }
    t1 = pspexp_time_us();
    {
        const PspExpStats *st = pspexp_stats(ctx);
        int n = f > 0 ? f : 1;
        qsort(lat, (size_t)n, sizeof(uint32_t), cmp_u32);
        printf("  frames %d x %d sprites: %.1f fps, %u B/frame, present->PRESENT_DONE mean %llu us p95 %u us max %u us, psp cpu %llu us ge %llu us, timeouts %d, inputs %u\n",
               n, sprites, (double)n * 1e6 / (double)(t1 - t0), st->last_frame_bytes,
               (unsigned long long)(lat_sum / (uint64_t)n), lat[(n * 95) / 100], lat_max,
               (unsigned long long)(exec_sum / (uint64_t)n), (unsigned long long)(ge_sum / (uint64_t)n),
               timeouts, st->inputs_received);
        if (big)
            printf("  with %d KiB/s of %d KiB uploads (%d sent): frames later than one vblank %d (%.1f%%), later than two %d, resource errors %u\n",
                   upload_kbps, upload_kib, uploads, over17, 100.0 * over17 / n, over33, st->resource_errors);
    }
    pspexp_release(ctx, 1);
    if (big) { pspexp_release(ctx, 100); pspexp_release(ctx, 101); }
    pspexp_pump(ctx, 10);
    free(recs); free(lat); free(big);
    return 0;
}

static void usage(void)
{
    printf("usage: pspexp-bench [--null] [--tcp [host[:port]]] [--mb N] [--echo N] [--frames N] [--sprites N] [--upload KBPS] [--upload-kib N] [--frames-only] [--pid 0x1CA] [--verbose]\n"
           "  --null        loopback through the Null executor instead of USB\n"
           "  --tcp [h[:p]] connect over TCP (PSP over Wi-Fi, or the runtime under PPSSPP; default 127.0.0.1:3838)\n"
           "  --mb N        bytes per throughput run in MiB (default 4)\n"
           "  --echo N      round trips per echo size (default 200)\n"
           "  --frames N    frames in the frame loop (default 300)\n"
           "  --sprites N   sprites per frame (default 500)\n"
           "  --upload KBPS concurrent texture upload stream during the frame loop (section 17 workload c; 0 = off)\n"
           "  --upload-kib N size of each upload in KiB, rounded to a power of two (default 256)\n"
           "  --frames-only skip the throughput and echo sections\n");
}

int main(int argc, char **argv)
{
    int use_null = 0, verbose = 0, echo_rounds = 200, frames = 300, sprites = 500, i;
    int upload_kbps = 0, upload_kib = 256, frames_only = 0, use_tcp = 0;
    const char *tcp_target = NULL;
    uint64_t mb = 4;
    uint16_t pid = 0;
    PspExpTransport *t = NULL;
    PspExpNullExecutor *nul = NULL;
    PspExpContext *ctx;
    Capture cap;
    const PspExpHello *h;
    static const uint32_t blocks[] = { 4096 - 16, 16384 - 16, 65536 - 16, 262144 - 16 };
    static const uint32_t echo_sizes[] = { 16, 4096, 40000 };
    char err[256];

    setvbuf(stdout, NULL, _IONBF, 0);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--null")) use_null = 1;
        else if (!strcmp(argv[i], "--tcp")) { use_tcp = 1; if (i + 1 < argc && argv[i + 1][0] != '-') tcp_target = argv[++i]; }
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--mb") && i + 1 < argc) mb = (uint64_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--echo") && i + 1 < argc) echo_rounds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sprites") && i + 1 < argc) sprites = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pid") && i + 1 < argc) pid = (uint16_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--upload") && i + 1 < argc) upload_kbps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--upload-kib") && i + 1 < argc) upload_kib = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames-only")) frames_only = 1;
        else { usage(); return 1; }
    }
    (void)verbose; (void)pid; (void)err;

    if (use_null) {
        nul = pspexp_null_create(NULL);
        t = pspexp_null_transport(nul);
    } else if (use_tcp) {
        PspExpTcpOptions o;
        static char hostbuf[256];
        pspexp_tcp_options_default(&o);
        if (tcp_target) {
            char *colon;
            strncpy(hostbuf, tcp_target, sizeof(hostbuf) - 1);
            colon = strrchr(hostbuf, ':');
            if (colon) { *colon = 0; o.port = (uint16_t)atoi(colon + 1); }
            if (hostbuf[0]) o.host = hostbuf;
        }
        o.verbose = verbose;
        printf("connecting to %s:%u over TCP...\n", o.host, o.port);
        t = pspexp_tcp_wait(&o, 30000, err, sizeof(err));
        if (!t) { fprintf(stderr, "%s\n", err); return 1; }
        printf("link synchronised (tcp)\n");
    } else {
#ifdef PSPEXP_HAVE_USB
        PspExpUsbOptions o;
        pspexp_usb_options_default(&o);
        o.verbose = verbose;
        if (pid) o.pid = pid;
        printf("waiting for PSP %04x:%04x over USB...\n", o.vid, o.pid);
        t = pspexp_usb_wait(&o, -1, err, sizeof(err));
        if (!t) { fprintf(stderr, "%s\n", err); return 1; }
        printf("link synchronised (packet %d B)\n", pspexp_usb_stats(t)->packet_size);
#else
        fprintf(stderr, "built without libusb; use --null\n");
        return 1;
#endif
    }

    ctx = pspexp_create(t, NULL);
    memset(&cap, 0, sizeof(cap));
    pspexp_set_message_hook(ctx, hook, &cap);
    if (pspexp_connect(ctx, 5000) < 0) { fprintf(stderr, "no HELLO within 5 s\n"); pspexp_destroy(ctx); return 1; }
    h = pspexp_caps(ctx);
    printf("connected: executor %u runtime v%u, %ux%u@%u, vram %u KiB ram %u KiB, formats 0x%03x, batch %u, features 0x%x\n",
           h->executor, h->runtime_version, h->display_width, h->display_height, h->refresh_hz,
           h->vram_budget / 1024, h->ram_budget / 1024, h->texture_formats, h->max_batch_records, h->features);

    if (!frames_only) {
        printf("\n[1] host -> psp throughput, %llu MiB per run\n", (unsigned long long)mb);
        for (i = 0; i < (int)(sizeof(blocks) / sizeof(blocks[0])); i++)
            if (run_downlink(ctx, &cap, blocks[i], mb << 20) < 0) break;

        printf("\n[2] psp -> host throughput, %llu MiB per run\n", (unsigned long long)mb);
        for (i = 0; i < (int)(sizeof(blocks) / sizeof(blocks[0])); i++)
            if (run_uplink(ctx, &cap, blocks[i], mb << 20) < 0) break;

        printf("\n[3] round-trip latency\n");
        for (i = 0; i < (int)(sizeof(echo_sizes) / sizeof(echo_sizes[0])); i++)
            if (run_echo(ctx, &cap, echo_sizes[i], echo_rounds) < 0) break;
    }

    printf("\n[4] frame loop through libPSPExp (one frame in flight)\n");
    run_frames(ctx, frames, sprites, 0, 0);
    if (upload_kbps > 0) {
        printf("\n[5] frame loop with a concurrent upload stream (workload c)\n");
        run_frames(ctx, frames, sprites, upload_kbps, upload_kib);
    }

    {
        const PspExpStats *st = pspexp_stats(ctx);
        printf("\ntotals: sent %llu B, received %llu B, resource errors %u, protocol errors %u\n",
               (unsigned long long)st->bytes_sent, (unsigned long long)st->bytes_received,
               st->resource_errors, st->protocol_errors);
#ifdef PSPEXP_HAVE_USB
        if (!use_null && !use_tcp) {
            const PspExpUsbStats *us = pspexp_usb_stats(t);
            printf("usb: %u transfers out, %u in, %u splits, %u idle read timeouts\n",
                   us->transfers_out, us->transfers_in, us->splits, us->read_timeouts);
        }
#endif
    }
    pspexp_send_raw(ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_DISCONNECT, NULL, 0);
    pspexp_destroy(ctx);
    if (nul) pspexp_null_destroy(nul);
    return 0;
}
