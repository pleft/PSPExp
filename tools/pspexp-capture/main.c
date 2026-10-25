/*
 * pspexp-capture: the receiving end of PSPExp Capture (psp/capture, a custom-firmware plugin that
 * sends a native PSP game's picture and sound over USB). It reads the frames and the sound,
 * tells the game's frame rate and what arrives, saves a frame as a picture every few seconds,
 * and with -o makes an MP4: the picture through ffmpeg as it comes, the sound mixed here and
 * added at the end.
 *   pspexp-capture [-o clip.mp4 | -s clips-folder [-b seconds]] [-d folder-for-frames] [-t seconds] [-c control-fifo] [-k] [-q]
 * -s: the note button's clips (psp/capture: note+R on the PSP saves the last -b seconds, 30 by
 * default, as clip-DATE.mp4; note+L starts and stops recording-DATE.mp4). The picture is kept
 * as it comes in 1 s pieces, the sound mixed in memory, both only as far back as a clip or a
 * recording under way needs. -b 0: recordings only, no last seconds kept: the PSP (the plugin,
 * PSPExp) sends the picture and the sound only while a note+L recording runs, and nothing is
 * encoded otherwise (a board's console runs it so unless Settings keeps the last 30 seconds).
 * -d also keeps latest.ppm there, the screen as it is now (twice a second), and psp.log (the
 * plugin's log, sent over the cable as it goes; also printed with "PSP: ").
 * -c: a FIFO (mkfifo) taking commands for the PSP, one a line, for testing without hands on it:
 *   press BUTTONS MS | hold BUTTONS | release   (BUTTONS: cross+up, ...: the game sees them)
 *   put FILE [NAME]   (into ms0:/seplugins/, e.g. a new pspexp_capture.prx; developer builds, make DEV=1)
 *   log [old] | restart (the game again, from its ISO: a new plugin loads; developer builds) | stop | quit
 *   pause | resume   (no frames copied meanwhile: the game's own frame rate, for comparing)
 *   vram   (the game's GE buffer addresses, a fingerprint of each 32 KB of VRAM)
 *   exp N   (0 normal, 1 the copy alone, 2 the sending alone: what costs the game its speed)
 *   copyprio N   (the copier thread's priority)
 *   band KB   (32, 64 or 128: the ring's band, each one USB write) | tx KB SPLIT (the largest transfer; 1 split short ones)
 *   clip | record   (with -s: as note+R and note+L on the PSP)
 * -k: after the link goes (a restart), wait for the game again: clip.mp4, clip-2.mp4, ...
 * -q: only the connections, the clips and what went wrong (a board's console runs it so: its log)
 * The link is PSPExp's own USB transport (host/transport-usb), under the plugin's own product ID
 * (PSPEXP_CAPTURE_USB_PID), so it runs beside a board's console. Inside PSPExp the same packets
 * come over a local socket instead (host/libpspexp/pspexp_capture.h): the PSP's runtime captures
 * what it shows, and the program holding PSPExp's link passes them on.
 */
#include "../../host/transport-usb/pspexp_usb.h"
#include "../../protocol/pspexp_usb_link.h"
#include "../../host/libpspexp/pspexp_capture.h"
#include "../../protocol/pspexp_protocol.h"
#include "../../host/libpspexp/pspexp_disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>

#define W 480
#define H 272
#define RATE 44100      /* the clip's sound: the PSP's own rate, stereo */

static volatile int g_stop, g_quit;   /* the session ends; the program ends */
static int g_quiet;                   /* -q */
/* the encoder's speed: a 32-bit ARM board (the GameShell's four Cortex-A7) the fastest */
#if defined(__arm__) && !defined(__aarch64__)
#define X264_PRESET "ultrafast"
#else
#define X264_PRESET "veryfast"
#endif
static void on_signal(int s) { (void)s; g_stop = 1; g_quit = 1; }

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint32_t get16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }

/* the game's pixels (its format: 0 565, 1 5551, 2 4444, 3 8888; rows of `stride` pixels, the
 * plugin sending VRAM's rows as they are) as 8-bit RGB, 480 wide */
static void to_rgb(const uint8_t *src, int fmt, int stride, uint8_t *rgb)
{
    int i;
    for (i = 0; i < W * H; i++, rgb += 3) {
        int at = (i / W) * stride + i % W;
        if (fmt == 3) { rgb[0] = src[at * 4]; rgb[1] = src[at * 4 + 1]; rgb[2] = src[at * 4 + 2]; continue; }
        uint32_t v = get16(src + at * 2);
        if (fmt == 0) { rgb[0] = (v & 31) << 3; rgb[1] = (v >> 5 & 63) << 2; rgb[2] = (v >> 11 & 31) << 3; }
        else if (fmt == 1) { rgb[0] = (v & 31) << 3; rgb[1] = (v >> 5 & 31) << 3; rgb[2] = (v >> 10 & 31) << 3; }
        else { rgb[0] = (v & 15) << 4; rgb[1] = (v >> 4 & 15) << 4; rgb[2] = (v >> 8 & 15) << 4; }
    }
}

/* exactly n bytes from the link; 0, or -1 when it went */
static int need(PspExpTransport *t, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n && !g_stop) {
        long r = t->recv(t, buf + got, n - got, 500);
        if (r < 0) return -1;
        got += (size_t)r;
    }
    return got == n ? 0 : -1;
}

/* ---- the sound: the game's channels mixed as the PSP's hardware does ----
 * A block is placed where its channel's previous one ended (the channel plays without gaps while
 * the game keeps it fed); a channel that went quiet for longer starts again at its block's time
 * (the PSP's clock, as the frames'). Mixed into a growing stereo track of 32-bit sums. */
static int32_t *g_mix;
static int64_t g_mix_len, g_mix_cap;   /* the track's end, and room, in samples: from g_mix_base on */
static int64_t g_mix_base;             /* the samples dropped from its start (-s keeps only what a clip may need) */
static int64_t g_end[3][8];          /* where each channel's last block ended; -1 none yet */
static uint64_t g_audio_blocks, g_audio_bytes;

static void mix_grow(int64_t n)
{
    if (n <= g_mix_cap) return;
    int64_t cap = g_mix_cap ? g_mix_cap : RATE * 60;
    while (cap < n) cap *= 2;
    g_mix = realloc(g_mix, (size_t)cap * 2 * sizeof(int32_t));
    if (!g_mix) { fprintf(stderr, "out of memory for the sound\n"); exit(1); }
    memset(g_mix + g_mix_cap * 2, 0, (size_t)(cap - g_mix_cap) * 2 * sizeof(int32_t));
    g_mix_cap = cap;
}

static void mix_block(int kind, int ch, int chans, uint32_t rate, int lvol, int rvol, uint32_t samples,
                      const int16_t *pcm, double at_us)
{
    if (kind > 2 || ch > 7 || !rate || !samples || at_us < 0) return;
    int64_t pos = (int64_t)(at_us * RATE / 1e6), *end = &g_end[kind][ch];
    int64_t start = (*end >= 0 && pos <= *end + RATE / 4) ? *end : pos;   /* contiguous unless a gap > 0.25 s */
    int64_t n = (int64_t)samples * RATE / rate;                           /* at the clip's rate */
    int64_t rel = start - g_mix_base;
    mix_grow(rel + n);
    double l = lvol / 32768.0, r = rvol / 32768.0;
    for (int64_t i = rel < 0 ? -rel : 0; i < n; i++) {
        uint32_t j = (uint32_t)(i * rate / RATE);                          /* (nearest: SRC rates only) */
        int a = chans == 2 ? pcm[j * 2] : pcm[j], b = chans == 2 ? pcm[j * 2 + 1] : pcm[j];
        g_mix[(rel + i) * 2] += (int32_t)(a * l);
        g_mix[(rel + i) * 2 + 1] += (int32_t)(b * r);
    }
    *end = start + n;
    if (*end > g_mix_len) g_mix_len = *end;
}

/* the track as 16-bit samples, from sample `from` (counted from the session's start) to `to` */
static int write_sound_range(const char *path, int64_t from, int64_t to)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    mix_grow(to - g_mix_base);
    for (int64_t i = from * 2; i < to * 2; i++) {
        int32_t v = i >= g_mix_base * 2 ? g_mix[i - g_mix_base * 2] : 0;
        int16_t s = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return 0;
}
static int write_sound(const char *path, int64_t len) { return write_sound_range(path, 0, len); }

/* the samples before `from` dropped (in steps of 20 s or more: a move of the whole track) */
static void mix_drop(int64_t from)
{
    int64_t n = from - g_mix_base;
    if (n < RATE * 20) return;
    if (n > g_mix_cap) n = g_mix_cap;
    memmove(g_mix, g_mix + n * 2, (size_t)(g_mix_cap - n) * 2 * sizeof(int32_t));
    memset(g_mix + (g_mix_cap - n) * 2, 0, (size_t)n * 2 * sizeof(int32_t));
    g_mix_base += n;
}

/* ---- the note button's clips (-s) ----
 * The picture goes to ffmpeg as it comes and out in 1 s pieces (seg-N.ts: frames 30N to 30N+29, a
 * key frame each second); a clip is the pieces it spans joined as they are, with the sound of the
 * same seconds, made once its last piece is complete (the next one begun), in a thread of its own
 * so the link is read meanwhile. Pieces and sound older than a clip or a recording under way may
 * need are dropped. */
typedef struct { int first, last; volatile int state; char name[1200]; char seg[1100]; pthread_t th; } Job;   /* state: 1 waiting for its last piece, 2 being made, 3 made */
static Job g_jobs[8];
static const char *g_clips;          /* -s */
static int g_clip_len = 30;          /* -b */
static char g_segdir[1100];
static int g_rec_first = -1;         /* the recording under way: its first piece */
static volatile int g_req_clip, g_req_rec;   /* the FIFO's clip and record */
static uint64_t g_frames_out;        /* the pieces' frames so far (as `written`) */
static int g_seg_closed, g_seg_final;   /* ffmpeg ended: every piece complete, the last one this */
static int g_seg_dropped;            /* the pieces before this one deleted */

static int seg_complete(int k)
{
    char p[1200];
    struct stat st;
    if (g_seg_closed) return 1;
    snprintf(p, sizeof(p), "%s/seg-%06d.ts", g_segdir, k + 1);
    return stat(p, &st) == 0;
}

static void *job_run(void *arg)
{
    Job *j = arg;
    char cmd[4000];
    snprintf(cmd, sizeof(cmd), "ffmpeg -loglevel error -y -f concat -safe 0 -i '%s.txt' -f s16le -ar %d -ac 2 -i '%s.raw' "
             "-c:v copy -c:a aac -b:a 160k -shortest -movflags +faststart '%s'", j->seg, RATE, j->seg, j->name);
    int r = system(cmd);
    char p[1200];
    snprintf(p, sizeof(p), "%s.txt", j->seg); unlink(p);
    snprintf(p, sizeof(p), "%s.raw", j->seg); unlink(p);
    if (r == 0) fprintf(stderr, "pspexp-capture: saved %s (%d s)\n", j->name, j->last - j->first + 1);
    else fprintf(stderr, "pspexp-capture: ffmpeg could not make %s\n", j->name);
    j->state = 3;
    return NULL;
}

/* a clip or a recording of pieces first..last, named for now */
static void job_add(int first, int last, const char *kind)
{
    char when[64];
    time_t tt = time(NULL);
    strftime(when, sizeof(when), "%Y-%m-%d-%H.%M.%S", localtime(&tt));
    for (int i = 0; i < 8; i++) {
        Job *j = &g_jobs[i];
        if (j->state == 3) { pthread_join(j->th, NULL); j->state = 0; }
        if (j->state) continue;
        j->first = first < g_seg_dropped ? g_seg_dropped : first;
        j->last = last;
        snprintf(j->name, sizeof(j->name), "%s/%s-%s.mp4", g_clips, kind, when);
        snprintf(j->seg, sizeof(j->seg), "%s/job-%d", g_segdir, i);
        j->state = 1;
        fprintf(stderr, "pspexp-capture: %s of %d s to come: %s\n", kind, j->last - j->first + 1, j->name);
        return;
    }
    fprintf(stderr, "pspexp-capture: too many clips at once: %s not made\n", kind);
}

/* the jobs whose pieces are complete started; old pieces and sound dropped */
static void jobs_step(void)
{
    int keep = (int)(g_frames_out / 30) - g_clip_len - 2;
    if (g_rec_first >= 0 && g_rec_first < keep) keep = g_rec_first;
    for (int i = 0; i < 8; i++) {
        Job *j = &g_jobs[i];
        if (j->state == 3) { pthread_join(j->th, NULL); j->state = 0; }
        if (j->state == 1 && g_seg_closed && j->last > g_seg_final) j->last = g_seg_final;   /* (the game ended first) */
        if (j->state == 1 && j->last < j->first) { j->state = 0; continue; }
        if (j->state == 1 && seg_complete(j->last)) {
            char p[1200];
            snprintf(p, sizeof(p), "%s.txt", j->seg);
            FILE *f = fopen(p, "w");
            if (f) { for (int k = j->first; k <= j->last; k++) fprintf(f, "file '%s/seg-%06d.ts'\n", g_segdir, k); fclose(f); }
            snprintf(p, sizeof(p), "%s.raw", j->seg);
            write_sound_range(p, (int64_t)j->first * RATE, (int64_t)(j->last + 1) * RATE);
            j->state = 2;
            if (pthread_create(&j->th, NULL, job_run, j) != 0) { fprintf(stderr, "pspexp-capture: no thread for %s\n", j->name); j->state = 0; }
        }
        if ((j->state == 1 || j->state == 2) && j->first < keep) keep = j->first;
    }
    if (keep < 0) keep = 0;
    for (; g_seg_dropped < keep; g_seg_dropped++) {
        char p[1200];
        snprintf(p, sizeof(p), "%s/seg-%06d.ts", g_segdir, g_seg_dropped);
        unlink(p);
    }
    mix_drop((int64_t)keep * RATE);
}

/* an event from the plugin (note+R, note+L), or the FIFO's: at frame `idx` of the session */
static int g_have_pic;   /* a picture of the game in the pieces (a clip of none is not made) */
static void note_event(int type, uint64_t idx)
{
    int k = (int)(idx / 30);
    if (!g_clips) return;
    if (type == 1 && !g_have_pic) fprintf(stderr, "pspexp-capture: no picture yet: no clip\n");
    else if (type == 1) job_add(k - g_clip_len + 1, k, "clip");
    else if (type == 2 && g_rec_first < 0) { g_rec_first = k; fprintf(stderr, "pspexp-capture: recording\n"); }
    else if (type == 3 && g_rec_first >= 0) { job_add(g_rec_first, k, "recording"); g_rec_first = -1; }
}

/* ---- control: commands for the plugin (psp/capture), from a FIFO ---- */
static PspExpTransport *volatile g_link;
static pthread_mutex_t g_link_lock = PTHREAD_MUTEX_INITIALIZER;

static void put32le(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
#define CMD_KEEP 14   /* u32 1: everything, all the time (the last seconds kept); 0: only during a note+L recording */
#define CMD_DISC 15   /* the answer to a "PXDQ": u32 seq, u32 status (0 ok, 1 no disc, 2 unreadable), u64 the disc's size, then the bytes */

/* ---- the virtual disc: a PSP game's sectors for the plugin (architecture §12c) ----
 * The plugin asks ("PXDQ": u32 seq at 4, u64 offset at 8, u32 length at 16; 0 for the size only)
 * for the game the console prepared (pspexp_capture_disc_file), opened at the first ask and again
 * when that file names another. At most 64 KB an answer. */
static int send_cmd(uint32_t cmd, const void *a, uint32_t alen, const void *b, uint32_t blen);
static PspExpDisc *g_disc;
static char g_disc_path[1024];
static uint64_t g_disc_reads, g_disc_bytes;

static void disc_serve(uint32_t seq, uint64_t off, uint32_t n)
{
    static uint8_t buf[64 * 1024 + 2 * PSPEXP_DISC_SECTOR];
    uint8_t rep[16];
    char f[600], path[1024] = "", err[200] = "";
    uint32_t status = 0, got = 0;
    uint64_t size = 0;
    FILE *pf;
    pspexp_capture_disc_file(f, sizeof(f));
    if ((pf = fopen(f, "r"))) {
        if (fgets(path, sizeof(path), pf)) path[strcspn(path, "\r\n")] = 0;
        fclose(pf);
    }
    if (path[0] && strcmp(path, g_disc_path) != 0) {   /* another game: that one */
        pspexp_disc_close(g_disc);
        g_disc = pspexp_disc_open(path, err, sizeof(err));
        snprintf(g_disc_path, sizeof(g_disc_path), "%s", path);
        fprintf(stderr, "pspexp-capture: the disc %s%s%s\n", path, g_disc ? "" : ": ", g_disc ? "" : err);
    }
    if (!g_disc) status = 1;
    else {
        size = pspexp_disc_size(g_disc);
        if (n > 64 * 1024) n = 64 * 1024;
        if (off >= size) n = 0;
        else if (n > size - off) n = (uint32_t)(size - off);
        if (n) {   /* whole sectors read, the part asked for sent */
            uint32_t first = (uint32_t)(off / PSPEXP_DISC_SECTOR), skip = (uint32_t)(off % PSPEXP_DISC_SECTOR);
            uint32_t sectors = (skip + n + PSPEXP_DISC_SECTOR - 1) / PSPEXP_DISC_SECTOR;
            if (pspexp_disc_read(g_disc, first, sectors, buf) < 0) status = 2;
            else { memmove(buf, buf + skip, n); got = n; }
        }
    }
    put32le(rep, seq); put32le(rep + 4, status); put32le(rep + 8, (uint32_t)size); put32le(rep + 12, (uint32_t)(size >> 32));
    send_cmd(CMD_DISC, rep, 16, buf, got);
    g_disc_reads++; g_disc_bytes += got;
}

static int send_cmd(uint32_t cmd, const void *a, uint32_t alen, const void *b, uint32_t blen)
{
    uint8_t h[12];
    int r = -1;
    memcpy(h, "PXCC", 4); put32le(h + 4, cmd); put32le(h + 8, alen + blen);
    pthread_mutex_lock(&g_link_lock);
    if (g_link && g_link->send(g_link, h, 12) == 0 && (!alen || g_link->send(g_link, a, alen) == 0)
        && (!blen || g_link->send(g_link, b, blen) == 0)) r = 0;
    pthread_mutex_unlock(&g_link_lock);
    return r;
}

static uint32_t buttons(const char *names)
{
    static const struct { const char *n; uint32_t b; } k[] = {
        {"select", 0x1}, {"start", 0x8}, {"up", 0x10}, {"right", 0x20}, {"down", 0x40}, {"left", 0x80},
        {"l", 0x100}, {"r", 0x200}, {"triangle", 0x1000}, {"circle", 0x2000}, {"cross", 0x4000}, {"square", 0x8000}};
    uint32_t b = 0;
    char buf[128], *tok, *save;
    snprintf(buf, sizeof(buf), "%s", names);
    for (tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save))
        for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) if (!strcmp(tok, k[i].n)) b |= k[i].b;
    return b;
}

static void control_line(char *line)
{
    char a[512] = "", b[128] = "", c[64] = "";
    int n = sscanf(line, "%511s %127s %63s", a, b, c);
    if (n < 1) return;
    if (!strcmp(a, "press") || !strcmp(a, "hold") || !strcmp(a, "release")) {
        uint8_t p[8];
        put32le(p, !strcmp(a, "release") ? 0 : buttons(b));
        put32le(p + 4, !strcmp(a, "press") ? (uint32_t)(n >= 3 ? atoi(c) : 150) : 0);
        send_cmd(1, p, 8, NULL, 0);
    } else if (!strcmp(a, "put")) {
        FILE *f = fopen(b, "rb");
        if (!f) { fprintf(stderr, "control: cannot read %s\n", b); return; }
        static uint8_t data[4 << 20];
        size_t len = fread(data, 1, sizeof(data), f);
        fclose(f);
        char name[32] = "";
        const char *base = strrchr(b, '/') ? strrchr(b, '/') + 1 : b;
        snprintf(name, sizeof(name), "%s", n >= 3 ? c : base);
        send_cmd(2, name, 32, data, (uint32_t)len);
        fprintf(stderr, "control: sent %s (%zu bytes) as %s\n", b, len, name);
    } else if (!strcmp(a, "log")) send_cmd(3, NULL, 0, NULL, 0);
    else if (!strcmp(a, "restart")) send_cmd(4, NULL, 0, NULL, 0);
    else if (!strcmp(a, "vram")) send_cmd(7, NULL, 0, NULL, 0);
    else if (!strcmp(a, "band")) { uint8_t p[4]; put32le(p, (uint32_t)strtoul(b, NULL, 0) * 1024); send_cmd(12, p, 4, NULL, 0); }
    else if (!strcmp(a, "tx")) { uint8_t p[8]; put32le(p, (uint32_t)strtoul(b, NULL, 0) * 1024); put32le(p + 4, (uint32_t)(n >= 3 ? atoi(c) : 1)); send_cmd(13, p, 8, NULL, 0); }
    else if (!strcmp(a, "exp") || !strcmp(a, "copyprio")) { uint8_t p[4]; put32le(p, (uint32_t)strtoul(b, NULL, 0)); send_cmd(!strcmp(a, "exp") ? 10 : 11, p, 4, NULL, 0); }
    else if (!strcmp(a, "pause") || !strcmp(a, "resume")) { uint8_t p[4]; put32le(p, !strcmp(a, "pause")); send_cmd(5, p, 4, NULL, 0); }
    else if (!strcmp(a, "clip")) g_req_clip = 1;
    else if (!strcmp(a, "record")) g_req_rec = 1;
    else if (!strcmp(a, "stop")) g_stop = 1;
    else if (!strcmp(a, "quit")) { g_quit = 1; g_stop = 1; }
    else fprintf(stderr, "control: unknown command: %s\n", a);
}

static void *control_thread(void *arg)
{
    const char *fifo = arg;
    char line[1024];
    while (!g_quit) {
        FILE *f = fopen(fifo, "r");   /* (waits for a writer) */
        if (!f) { perror(fifo); return NULL; }
        while (fgets(line, sizeof(line), f)) control_line(line);
        fclose(f);
    }
    return NULL;
}

/* ---- PSPExp's own games: the socket source ----
 * The program holding PSPExp's link connects and writes the PSP's capture packets. The link moves
 * between programs (the console hands the PSP to a game and takes it back), each connecting anew:
 * a session goes on across them, as the PSP's clock does, when the next connects within 8 s. */
static int g_listen = -1;
typedef struct { PspExpTransport t; int fd; } SockSource;

static int sock_accept(int timeout_ms)
{
    struct pollfd p = { g_listen, POLLIN, 0 };
    if (g_listen < 0 || poll(&p, 1, timeout_ms) <= 0) return -1;
    return accept(g_listen, NULL, NULL);
}

/* what this receiver keeps, to each program connecting ("PXCH", PSPEXP_CAPTURE_*): it asks the PSP
 * for that (recordings only: the picture and the sound only while a note+L recording runs) */
static int recordings_only(void) { return g_clips && g_clip_len == 0; }
static void sock_hello(int fd)
{
    uint8_t h[32];
    memset(h, 0, sizeof(h));
    memcpy(h, "PXCH", 4);
    h[4] = recordings_only() ? PSPEXP_CAPTURE_RECORDINGS : PSPEXP_CAPTURE_ALL;
    if (write(fd, h, sizeof(h)) != (ssize_t)sizeof(h)) fprintf(stderr, "pspexp-capture: the hello did not go\n");
}

/* the plugin's USB link coming up while PSPExp's session waits for its next holder: PSPExp left for
 * a PSP game (the shelf), whose start waits for its disc from here; taken at once, the session
 * ended (it waited out its 8 s first: 28 s, 2026-10-25, the plugin's safety net firing meanwhile) */
static PspExpUsbOptions g_usb_opts;
static PspExpTransport *g_usb_pending;

static long sock_recv(PspExpTransport *t, void *buf, size_t cap, int timeout_ms)
{
    SockSource *s = (SockSource *)t;
    if (s->fd < 0) {   /* a hand-over: the next holder of the link, or the end of the session */
        static double since;
        char err[200];
        if (!since) since = now();
        if ((s->fd = sock_accept(timeout_ms)) >= 0) { sock_hello(s->fd); since = 0; return 0; }
        if (g_usb_opts.pid && (g_usb_pending = pspexp_usb_wait(&g_usb_opts, 0, err, sizeof(err)))) { since = 0; return -1; }
        if (now() - since > 8) { since = 0; return -1; }
        return 0;
    }
    struct pollfd p = { s->fd, POLLIN, 0 };
    int r = poll(&p, 1, timeout_ms);
    if (r <= 0) return 0;
    long n = read(s->fd, buf, cap);
    if (n > 0) return n;
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return 0;
    close(s->fd); s->fd = -1;   /* (this holder gave the link up) */
    return 0;
}
static int sock_send(PspExpTransport *t, const void *d, size_t n) { (void)t; (void)d; (void)n; return -1; }   /* (no commands into PSPExp) */
static void sock_close(PspExpTransport *t) { SockSource *s = (SockSource *)t; if (s->fd >= 0) close(s->fd); s->fd = -1; }

/* -T PORT (tests): a stand-in for the plugin over TCP on 127.0.0.1, instead of the USB cable
 * (tests/run_disc_serve.sh drives the virtual disc through it) */
static int g_tcp_listen = -1;
typedef struct { PspExpTransport t; int fd; } TcpSource;
static long tcp_recv(PspExpTransport *t, void *buf, size_t cap, int timeout_ms)
{
    TcpSource *s = (TcpSource *)t;
    struct pollfd p = { s->fd, POLLIN, 0 };
    long n;
    if (poll(&p, 1, timeout_ms) <= 0) return 0;
    n = read(s->fd, buf, cap);
    return n > 0 ? n : n < 0 && (errno == EINTR || errno == EAGAIN) ? 0 : -1;
}
static int tcp_send(PspExpTransport *t, const void *d, size_t n)
{
    TcpSource *s = (TcpSource *)t;
    const uint8_t *p = (const uint8_t *)d;
    while (n) { ssize_t w = write(s->fd, p, n); if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; } p += w; n -= (size_t)w; }
    return 0;
}
static void tcp_close(PspExpTransport *t) { TcpSource *s = (TcpSource *)t; if (s->fd >= 0) close(s->fd); s->fd = -1; }
static int tcp_listen(int port)
{
    struct sockaddr_in a;
    int one = 1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if ((g_tcp_listen = socket(AF_INET, SOCK_STREAM, 0)) < 0) return -1;
    setsockopt(g_tcp_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(g_tcp_listen, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(g_tcp_listen, 1) < 0) { close(g_tcp_listen); g_tcp_listen = -1; return -1; }
    return 0;
}

static void sock_listen(void)
{
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    pspexp_capture_socket_path(a.sun_path, sizeof(a.sun_path));
    unlink(a.sun_path);   /* (a stale one: only one receiver runs, see the lock) */
    g_listen = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen < 0 || bind(g_listen, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(g_listen, 4) < 0) {
        fprintf(stderr, "pspexp-capture: no socket for PSPExp's own games (%s)\n", a.sun_path);
        if (g_listen >= 0) close(g_listen);
        g_listen = -1;
        return;
    }
    chmod(a.sun_path, 0600);
}

static void mix_reset(void)
{
    free(g_mix); g_mix = NULL; g_mix_len = g_mix_cap = 0;
    memset(g_end, 0xff, sizeof(g_end));
    g_audio_blocks = g_audio_bytes = 0;
    g_mix_base = 0;
}

/* the note button's clips: the picture in 1 s pieces (see jobs_step), from piece 0 */
static FILE *seg_open(void)
{
    char cmd[1600];
    FILE *ff;
    snprintf(g_segdir, sizeof(g_segdir), "%s/pspexp-capture-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    mkdir(g_segdir, 0700);
    g_rec_first = -1; g_frames_out = 0; g_seg_closed = 0; g_seg_final = -1; g_seg_dropped = 0;
    snprintf(cmd, sizeof(cmd), "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s %dx%d -r 30 -i - "
             "-c:v libx264 -preset " X264_PRESET " -crf 18 -pix_fmt yuv420p -g 30 -force_key_frames 'expr:gte(t,n_forced)' "
             "-f segment -segment_time 1 -segment_time_delta 0.0167 -reset_timestamps 1 '%s/seg-%%06d.ts'", W, H, g_segdir);   /* (the delta, half a frame: without it the first piece took 2 s, every clip a second off) */
    ff = popen(cmd, "w");
    if (!ff) perror("ffmpeg");
    return ff;
}

/* the pieces end (the game ended, or a recording only): a recording under way saved, every clip
 * made before going on, the pieces deleted */
static void seg_close(FILE *ff, uint64_t written)
{
    pclose(ff);
    g_seg_closed = 1;
    g_seg_final = written ? (int)((written - 1) / 30) : -1;
    if (g_rec_first >= 0) { job_add(g_rec_first, g_seg_final, "recording"); g_rec_first = -1; }
    jobs_step();
    for (int i = 0; i < 8; i++)
        if (g_jobs[i].state >= 2) { pthread_join(g_jobs[i].th, NULL); g_jobs[i].state = 0; }
    for (int k = g_seg_dropped; k <= g_seg_final + 1; k++) {
        char p[1200];
        snprintf(p, sizeof(p), "%s/seg-%06d.ts", g_segdir, k);
        unlink(p);
    }
    rmdir(g_segdir);
}

/* one run of the game, from its connection until the link goes (or -t, or stop) */
static int session(PspExpTransport *t, const char *out, const char *dir, double limit)
{
    static uint8_t hdr[32], px[1024 * H * 4], rgb[W * H * 3], prev[W * H * 3];
    static int16_t pcm[64 * 1024];
    static uint8_t reply[64 * 1024];
    char video[1100] = "";
    FILE *ff = NULL;
    int rec_only = recordings_only(), pic = 0;   /* (pic: a frame of the game in this picture, not only black) */
    g_have_pic = 0;
    mix_reset();
    memset(prev, 0, sizeof(prev));
    if (g_clips && !rec_only) {   /* (recordings only: the pieces start with each recording) */
        if (!(ff = seg_open())) return -1;
    } else if (out) {
        char cmd[1400];
        /* the picture first on its own (the sound is mixed while it comes, added at the end); a
         * steady 30 a second, each frame placed at the time the PSP showed it: one that came late
         * is held, so a clip plays at the game's own speed */
        snprintf(video, sizeof(video), "%s.picture.mp4", out);
        snprintf(cmd, sizeof(cmd), "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s %dx%d -r 30 -i - "
                 "-c:v libx264 -preset " X264_PRESET " -crf 18 -pix_fmt yuv420p '%s'", W, H, video);
        ff = popen(cmd, "w");
        if (!ff) { perror("ffmpeg"); return -1; }
    }
    double t0 = now(), tl = t0, last_shot = 0, last_latest = 0, last_jobs = 0;
    uint32_t frames = 0, f_last = 0, ch_first = 0, ch_last = 0, ch_at = 0, base = 0;
    uint64_t bytes = 0, b_last = 0, written = 0, a_last = 0;
    int have_base = 0;
    while (!g_stop) {
        if (g_clips) {   /* the FIFO's clip and record; the clips' pieces looked at twice a second */
            if ((g_req_clip || g_req_rec) && rec_only) fprintf(stderr, "pspexp-capture: recordings only (-b 0): the PSP's note+L starts one\n");
            if (g_req_clip) { g_req_clip = 0; if (!rec_only) note_event(1, written); }
            if (g_req_rec) { g_req_rec = 0; if (!rec_only) note_event(g_rec_first < 0 ? 2 : 3, written); }
            if (ff && now() - last_jobs >= 0.5) { jobs_step(); last_jobs = now(); }
        }
        if (need(t, hdr, 4) < 0) break;
        int sound = memcmp(hdr, "PXCA", 4) == 0, rep = !memcmp(hdr, "PXCR", 4) || !memcmp(hdr, "PXCL", 4), dq = !memcmp(hdr, "PXDQ", 4);
        int msg = memcmp(hdr, "PXCM", 4) == 0, ev = memcmp(hdr, "PXCE", 4) == 0;
        if (!sound && !rep && !msg && !ev && !dq && memcmp(hdr, "PXCF", 4) != 0) {   /* out of step: slide one byte at a time to the next header */
            memmove(hdr, hdr + 1, 3);
            continue;
        }
        if (need(t, hdr + 4, 28) < 0) break;
        uint32_t when = get32(hdr + 20), len = get32(hdr + 24);
        if (dq) {   /* the virtual disc: the plugin's read, answered at once */
            disc_serve(get32(hdr + 4), (uint64_t)get32(hdr + 8) | (uint64_t)get32(hdr + 12) << 32, get32(hdr + 16));
            continue;
        }
        if (msg) {   /* the plugin's log, sent as it goes (it never writes to the memory stick) */
            if (len > sizeof(reply) || need(t, reply, len) < 0) break;
            if (dir) { char p[512]; snprintf(p, sizeof(p), "%s/psp.log", dir); FILE *f = fopen(p, "ab"); if (f) { fwrite(reply, 1, len, f); fclose(f); } }
            else if (g_quiet) {   /* a board's quiet receiver: all of it in ~/.cache/pspexp/psp-plugin.log (1 MB, then .old), for
                                   * when a game goes wrong (the first virtual discs, 2026-10-25: the PSP off, its last lines lost) */
                char p[600], q[620];
                const char *h = getenv("HOME");
                struct stat st;
                FILE *f;
                snprintf(p, sizeof(p), "%s/.cache/pspexp/psp-plugin.log", h && *h ? h : "/tmp");
                if (stat(p, &st) == 0 && st.st_size > (1 << 20)) { snprintf(q, sizeof(q), "%s.old", p); rename(p, q); }
                if ((f = fopen(p, "ab"))) { fwrite(reply, 1, len, f); fclose(f); }
            }
            for (uint32_t i = 0, b0 = 0; i < len; i++)
                if (reply[i] == '\n') {   /* (-q: the note button's and the start's lines only: a board's log) */
                    const char *l = (char *)reply + b0;
                    int n = (int)(i - b0);
                    if (!g_quiet || (n > 5 && !memcmp(l, "note+", 5)) || (n > 16 && !memcmp(l, "the game is set ", 16)) ||
                        (n > 16 && !memcmp(l, "the virtual disc", 16)) || (n > 12 && !memcmp(l, "started from", 12)))
                        fprintf(stderr, "PSP: %.*s\n", n, l);
                    b0 = i + 1;
                }
            continue;
        }
        if (rep) {   /* the plugin's answer to a command */
            if (len > sizeof(reply) || need(t, reply, len) < 0) break;
            if (hdr[3] == 'L') {
                fprintf(stderr, "control: the PSP's log (%d bytes, its end below)\n", (int)get32(hdr + 8));
                if (dir) { char p[512]; snprintf(p, sizeof(p), "%s/psp-log.txt", dir); FILE *f = fopen(p, "wb"); if (f) { fwrite(reply, 1, len, f); fclose(f); } }
                fwrite(reply, 1, len, stderr);
            } else if (get32(hdr + 4) == 7 && len >= 8 * 4 + 128 * 4) {   /* vram */
                fprintf(stderr, "control: vram: FBP %08x FBW %08x ZBP %08x ZBW %08x TBP0 %08x TBW0 %08x PSM %08x shown %08x\n",
                        get32(reply), get32(reply + 4), get32(reply + 8), get32(reply + 12), get32(reply + 16),
                        get32(reply + 20), get32(reply + 24), get32(reply + 28));
                for (int b = 0; b < 64; b++)
                    fprintf(stderr, "control: vram %06x sum %08x nonzero %u\n", b * 32768, get32(reply + 32 + b * 8), get32(reply + 36 + b * 8));
            } else fprintf(stderr, "control: command %u: %d\n", get32(hdr + 4), (int)get32(hdr + 8));
            continue;
        }
        if (!have_base) { base = when; have_base = 1; }   /* the clip's start: the first thing to arrive */
        double at = (double)(int32_t)(when - base);       /* us into the clip (the PSP's 32-bit clock) */
        if (ev) {   /* the note button: 1 a clip, 2 a recording starts, 3 it stops (at the PSP's time of the press) */
            int type = (int)get32(hdr + 4);
            if (len > sizeof(reply) || (len && need(t, reply, len) < 0)) break;
            if (!rec_only) note_event(type, at > 0 ? (uint64_t)(at * 30 / 1e6) : 0);
            else if (type == 2 && !ff) {   /* recordings only: pieces of its own, its time from the press */
                mix_reset();
                if (!(ff = seg_open())) break;
                base = when; written = 0; pic = 0;
                memset(prev, 0, sizeof(prev));
                g_rec_first = 0;
                fprintf(stderr, "pspexp-capture: recording\n");
            } else if (type == 3 && ff) {   /* the picture held up to the press, then the recording saved */
                uint64_t due = at > 0 ? (uint64_t)(at * 30 / 1e6) : 0;
                int bad = 0;
                while (pic && written < due && !bad) { bad = fwrite(prev, 1, sizeof(prev), ff) != sizeof(prev); written++; }
                g_frames_out = written;
                if (!written) fprintf(stderr, "pspexp-capture: nothing recorded (no picture came)\n");
                seg_close(ff, written); ff = NULL;
            }
            continue;
        }
        if (sound) {
            uint32_t chans = get16(hdr + 6), samples = get32(hdr + 16);
            if (len > sizeof(pcm) || len != samples * chans * 2) { fprintf(stderr, "bad sound block (%u bytes)\n", len); continue; }
            if (need(t, (uint8_t *)pcm, len) < 0) break;
            if (rec_only && !ff) continue;   /* (between recordings: an older PSP side still sending) */
            mix_block(hdr[4], hdr[5], (int)chans, get32(hdr + 8), (int)get16(hdr + 12), (int)get16(hdr + 14), samples, pcm, at);
            g_audio_blocks++; g_audio_bytes += len; bytes += 32 + len;
            if (ff && pic && at > 500000) {   /* a picture that holds still (PSPExp draws nothing new): the last frame
                                                  * held up to the sound's time, less half a second for a frame on its way */
                uint64_t due = (uint64_t)((at - 500000) * 30 / 1e6);
                int bad = 0;
                while (written < due && !bad) { bad = fwrite(prev, 1, sizeof(prev), ff) != sizeof(prev); written++; }
                g_frames_out = written;
            }
            continue;
        }
        uint32_t w = get16(hdr + 4), h = get16(hdr + 6), fmt = get16(hdr + 8), changes = get32(hdr + 16);
        if (w < W || w > 1024 || h != H || len > sizeof(px) || fmt > 3 || len != w * h * (fmt == 3 ? 4 : 2)) { fprintf(stderr, "bad header (%ux%u fmt %u len %u)\n", w, h, fmt, len); continue; }
        if (need(t, px, len) < 0) break;
        if (!frames) { ch_first = changes; ch_at = changes; }
        frames++; bytes += 32 + len; ch_last = changes;
        double n = now();
        if (ff || dir) to_rgb(px, (int)fmt, (int)w, rgb);
        if (ff) {   /* the previous frame (black before the first) held until this one's time, then this one */
            uint64_t due = at > 0 ? (uint64_t)(at * 30 / 1e6) : 0;
            int bad = 0;
            while (written < due && !bad) { bad = fwrite(prev, 1, sizeof(prev), ff) != sizeof(prev); written++; }
            if (!bad && written <= due) { bad = fwrite(rgb, 1, sizeof(rgb), ff) != sizeof(rgb); written++; pic = g_have_pic = 1; }
            if (bad) { fprintf(stderr, "ffmpeg stopped\n"); break; }
            memcpy(prev, rgb, sizeof(rgb));
            g_frames_out = written;
        }
        if (dir && n - last_latest >= 0.5) {   /* the screen now, for whoever drives the game from here */
            char p[512], q[512];
            snprintf(p, sizeof(p), "%s/latest.ppm.tmp", dir); snprintf(q, sizeof(q), "%s/latest.ppm", dir);
            FILE *f = fopen(p, "wb");
            if (f) { fprintf(f, "P6\n%d %d\n255\n", W, H); fwrite(rgb, 1, sizeof(rgb), f); fclose(f); rename(p, q); }
            last_latest = n;
        }
        if (dir && n - last_shot >= 5) {
            char p[512];
            snprintf(p, sizeof(p), "%s/frame-%05u.ppm", dir, frames);
            FILE *f = fopen(p, "wb");
            if (f) { fprintf(f, "P6\n%d %d\n255\n", W, H); fwrite(rgb, 1, sizeof(rgb), f); fclose(f); }
            last_shot = n;
        }
        if (n - tl >= 5) {
            if (!g_quiet) fprintf(stderr, "received %.1f frames/s, %.1f MB/s, sound %.0f KB/s; the game %.1f fps; format %u\n",
                    (frames - f_last) / (n - tl), (bytes - b_last) / (n - tl) / 1e6,
                    (g_audio_bytes - a_last) / (n - tl) / 1e3, (changes - ch_at) / (n - tl), fmt);
            tl = n; f_last = frames; b_last = bytes; ch_at = changes; a_last = g_audio_bytes;
        }
        if (limit > 0 && n - t0 >= limit) break;
    }
    double dt = now() - t0;
    fprintf(stderr, "pspexp-capture: %u frames in %.1f s (%.1f/s); the game changed frames %u times (%.1f/s); "
            "%llu sound blocks (%.1f MB)\n", frames, dt, frames / dt, ch_last - ch_first, (ch_last - ch_first) / dt,
            (unsigned long long)g_audio_blocks, g_audio_bytes / 1e6);
    if (g_clips) {   /* a recording under way ends with the game; every clip made before the next session */
        if (ff) seg_close(ff, written);
        return 0;
    }
    if (ff) {
        pclose(ff);
        if (!frames) { unlink(video); return 0; }
        if (!g_audio_blocks) { rename(video, out); return 0; }
        /* the sound as long as the picture, then both into the clip */
        char raw[1100], cmd[3600];
        snprintf(raw, sizeof(raw), "%s.sound.raw", out);
        if (write_sound(raw, (int64_t)(written * RATE / 30)) < 0) { perror(raw); return -1; }
        snprintf(cmd, sizeof(cmd), "ffmpeg -loglevel error -y -i '%s' -f s16le -ar %d -ac 2 -i '%s' "
                 "-c:v copy -c:a aac -b:a 160k -shortest '%s'", video, RATE, raw, out);
        if (system(cmd) != 0) { fprintf(stderr, "pspexp-capture: ffmpeg could not join the sound (kept: %s, %s)\n", video, raw); return -1; }
        unlink(video); unlink(raw);
        fprintf(stderr, "pspexp-capture: %s\n", out);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *out = NULL, *dir = NULL, *fifo = NULL;
    double limit = 0;
    int c, keep = 0;
    int test_port = 0;
    while ((c = getopt(argc, argv, "o:s:b:d:t:c:kqT:")) != -1) {
        if (c == 'o') out = optarg;
        else if (c == 's') g_clips = optarg;
        else if (c == 'b') g_clip_len = atoi(optarg) >= 0 ? atoi(optarg) : 30;   /* (0: recordings only) */
        else if (c == 'd') dir = optarg;
        else if (c == 't') limit = atof(optarg);
        else if (c == 'c') fifo = optarg;
        else if (c == 'k') keep = 1;
        else if (c == 'q') g_quiet = 1;
        else if (c == 'T') test_port = atoi(optarg);
        else { fprintf(stderr, "usage: pspexp-capture [-o clip.mp4 | -s clips-folder [-b seconds, 0: recordings only]] [-d frames-folder] [-t seconds] [-c control-fifo] [-k] [-q]\n"); return 2; }
    }
    if (out && g_clips) { fprintf(stderr, "pspexp-capture: -o or -s, not both\n"); return 2; }
    if (g_clips) {   /* the clips' folder, made if missing (a board's ~/Videos/PSPExp) */
        char p[1100];
        snprintf(p, sizeof(p), "%s", g_clips);
        for (char *q = p + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(p, 0755); *q = '/'; }
        mkdir(p, 0755);
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);   /* (the console stopping it: the clips under way still made) */
    signal(SIGPIPE, SIG_IGN);
    {   /* one receiver at a time: a second one, idle, read the FIFO's commands meant for the one
         * holding the link (button presses lost, 2026-10-23) */
        char lock[1100];
        snprintf(lock, sizeof(lock), "%s/pspexp-capture.lock", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        int fd = open(lock, O_CREAT | O_RDWR, 0600);
        if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) { fprintf(stderr, "pspexp-capture: another one is running (%s)\n", lock); return 1; }
    }
    sock_listen();
    if (test_port && tcp_listen(test_port) < 0) { fprintf(stderr, "pspexp-capture: no TCP port %d\n", test_port); return 1; }
    if (fifo) {
        pthread_t th;
        struct stat st;
        if (stat(fifo, &st) != 0 && mkfifo(fifo, 0600) != 0) { perror(fifo); return 1; }
        pthread_create(&th, NULL, control_thread, (void *)fifo);
        pthread_detach(th);
    }
    for (int run = 1; !g_quit; run++) {
        PspExpUsbOptions o;
        char err[256], name[1100];
        pspexp_usb_options_default(&o);
        o.pid = PSPEXP_CAPTURE_USB_PID;   /* (the plugin's own product: PSPExp's console waits for PSPExp's) */
        g_usb_opts = o;
        if (!g_quiet || run == 1) fprintf(stderr, "pspexp-capture: waiting for a PSP game with PSPExp Capture on the USB cable, or for PSPExp...\n");
        PspExpTransport *t = g_usb_pending;   /* (the plugin, come while PSPExp's session waited) */
        static SockSource ss;
        g_usb_pending = NULL;
        int from_psp_exp = 0;
        while (!t && !g_quit) {   /* (in steps: Ctrl-C ends a wait) a native game on the USB cable, or PSPExp's own on the socket */
            static TcpSource ts;
            if (g_tcp_listen >= 0) {   /* (tests: the plugin's stand-in) */
                struct pollfd p = { g_tcp_listen, POLLIN, 0 };
                if (poll(&p, 1, 200) > 0 && (ts.fd = accept(g_tcp_listen, NULL, NULL)) >= 0) {
                    memset(&ts.t, 0, sizeof(ts.t));
                    ts.t.name = "test"; ts.t.recv = tcp_recv; ts.t.send = tcp_send; ts.t.close = tcp_close;
                    t = &ts.t;
                    break;
                }
            }
            int fd = sock_accept(0);
            if (fd >= 0) {
                sock_hello(fd);
                memset(&ss, 0, sizeof(ss));
                ss.t.name = "pspexp"; ss.t.recv = sock_recv; ss.t.send = sock_send; ss.t.close = sock_close; ss.fd = fd;
                t = &ss.t; from_psp_exp = 1;
                break;
            }
            t = pspexp_usb_wait(&o, 500, err, sizeof(err));
        }
        if (!t) break;
        fprintf(stderr, "pspexp-capture: connected%s\n", from_psp_exp ? " (PSPExp)" : "");
        pthread_mutex_lock(&g_link_lock); g_link = from_psp_exp ? NULL : t; pthread_mutex_unlock(&g_link_lock);
        if (!from_psp_exp) {   /* what to send: everything (the last seconds kept), or only during a note+L recording */
            uint8_t m[4] = { recordings_only() ? 0 : 1, 0, 0, 0 };
            send_cmd(CMD_KEEP, m, 4, NULL, 0);
        }
        if (out && run > 1) {   /* clip.mp4, clip-2.mp4, ... */
            const char *dot = strrchr(out, '.');
            int stem = dot ? (int)(dot - out) : (int)strlen(out);
            snprintf(name, sizeof(name), "%.*s-%d%s", stem, out, run, dot ? dot : "");
        } else if (out) snprintf(name, sizeof(name), "%s", out);
        g_stop = g_quit;   /* (a Ctrl-C while waiting ends this one too) */
        session(t, out ? name : NULL, dir, limit);
        pthread_mutex_lock(&g_link_lock); g_link = NULL; pthread_mutex_unlock(&g_link_lock);
        t->close(t);
        if (!keep) break;
    }
    return 0;
}
