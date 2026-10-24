/*
 * pspexp-capture: the receiving end of PSPExp Capture (psp/capture, a custom-firmware plugin that
 * sends a native PSP game's picture and sound over USB). It reads the frames and the sound,
 * tells the game's frame rate and what arrives, saves a frame as a picture every few seconds,
 * and with -o makes an MP4: the picture through ffmpeg as it comes, the sound mixed here and
 * added at the end.
 *   pspexp-capture [-o clip.mp4] [-d folder-for-frames] [-t seconds] [-c control-fifo] [-k]
 * -d also keeps latest.ppm there, the screen as it is now (twice a second), and psp-log.txt.
 * -c: a FIFO (mkfifo) taking commands for the PSP, one a line, for testing without hands on it:
 *   press BUTTONS MS | hold BUTTONS | release   (BUTTONS: cross+up, ...: the game sees them)
 *   put FILE [NAME]   (into ms0:/seplugins/, e.g. a new pspexp_capture.prx)
 *   log | restart (the game again, from its ISO: a new plugin loads) | stop | quit
 *   pause | resume   (no frames copied meanwhile: the game's own frame rate, for comparing)
 *   vram   (the game's GE buffer addresses, a fingerprint of each 32 KB of VRAM)
 *   gestat on | off   (the PSP's log adds what the game hands the GE a frame: commands, vertices, textures)
 *   exp N   (0 normal, 1 the copy alone, 2 the sending alone: what costs the game its speed)
 *   copyprio N   (the copier thread's priority)
 *   band KB   (32, 64 or 128: the ring's band, each one USB write) | tx KB SPLIT (the largest transfer; 1 split short ones)
 *   idle on | off   (the PSP's log adds the processor time the game leaves free)
 *   ge on | off | 1 | 2   (the GE makes each frame 16-bit through the game's depth buffer, half the
 *     bytes; 1 only its drawing, 2 only its copy out: for finding faults)
 * -k: after the link goes (a restart), wait for the game again: clip.mp4, clip-2.mp4, ...
 * The link is PSPExp's own USB transport (host/transport-usb).
 */
#include "../../host/transport-usb/pspexp_usb.h"
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

#define W 480
#define H 272
#define RATE 44100      /* the clip's sound: the PSP's own rate, stereo */

static volatile int g_stop, g_quit;   /* the session ends; the program ends */
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
static int64_t g_mix_len, g_mix_cap;
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
    mix_grow(start + n);
    double l = lvol / 32768.0, r = rvol / 32768.0;
    for (int64_t i = 0; i < n; i++) {
        uint32_t j = (uint32_t)(i * rate / RATE);                          /* (nearest: SRC rates only) */
        int a = chans == 2 ? pcm[j * 2] : pcm[j], b = chans == 2 ? pcm[j * 2 + 1] : pcm[j];
        g_mix[(start + i) * 2] += (int32_t)(a * l);
        g_mix[(start + i) * 2 + 1] += (int32_t)(b * r);
    }
    *end = start + n;
    if (*end > g_mix_len) g_mix_len = *end;
}

/* the track as 16-bit samples, from the start of the clip to its end */
static int write_sound(const char *path, int64_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    mix_grow(len);
    for (int64_t i = 0; i < len * 2; i++) {
        int32_t v = g_mix[i];
        int16_t s = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return 0;
}

/* ---- control: commands for the plugin (psp/capture), from a FIFO ---- */
static PspExpTransport *volatile g_link;
static pthread_mutex_t g_link_lock = PTHREAD_MUTEX_INITIALIZER;

static void put32le(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

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
    else if (!strcmp(a, "band") || !strcmp(a, "idle")) { uint8_t p[4]; put32le(p, !strcmp(a, "idle") ? (uint32_t)(strcmp(b, "off") != 0) : (uint32_t)strtoul(b, NULL, 0) * 1024); send_cmd(!strcmp(a, "band") ? 12 : 14, p, 4, NULL, 0); }
    else if (!strcmp(a, "tx")) { uint8_t p[8]; put32le(p, (uint32_t)strtoul(b, NULL, 0) * 1024); put32le(p + 4, (uint32_t)(n >= 3 ? atoi(c) : 1)); send_cmd(13, p, 8, NULL, 0); }
    else if (!strcmp(a, "exp") || !strcmp(a, "copyprio")) { uint8_t p[4]; put32le(p, (uint32_t)strtoul(b, NULL, 0)); send_cmd(!strcmp(a, "exp") ? 10 : 11, p, 4, NULL, 0); }
    else if (!strcmp(a, "gestat")) { uint8_t p[4]; put32le(p, strcmp(b, "off") != 0); send_cmd(9, p, 4, NULL, 0); }
    else if (!strcmp(a, "ge")) { uint8_t p[4]; put32le(p, !strcmp(b, "on") ? 3 : (uint32_t)atoi(b)); send_cmd(8, p, 4, NULL, 0); }
    else if (!strcmp(a, "pause") || !strcmp(a, "resume")) { uint8_t p[4]; put32le(p, !strcmp(a, "pause")); send_cmd(5, p, 4, NULL, 0); }
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

/* one run of the game, from its connection until the link goes (or -t, or stop) */
static int session(PspExpTransport *t, const char *out, const char *dir, double limit)
{
    static uint8_t hdr[32], px[1024 * H * 4], rgb[W * H * 3], prev[W * H * 3];
    static int16_t pcm[64 * 1024];
    static uint8_t reply[64 * 1024];
    char video[1100] = "";
    FILE *ff = NULL;
    free(g_mix); g_mix = NULL; g_mix_len = g_mix_cap = 0;
    memset(g_end, 0xff, sizeof(g_end));
    g_audio_blocks = g_audio_bytes = 0;
    memset(prev, 0, sizeof(prev));
    if (out) {
        char cmd[1400];
        /* the picture first on its own (the sound is mixed while it comes, added at the end); a
         * steady 30 a second, each frame placed at the time the PSP showed it: one that came late
         * is held, so a clip plays at the game's own speed */
        snprintf(video, sizeof(video), "%s.picture.mp4", out);
        snprintf(cmd, sizeof(cmd), "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s %dx%d -r 30 -i - "
                 "-c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p '%s'", W, H, video);
        ff = popen(cmd, "w");
        if (!ff) { perror("ffmpeg"); return -1; }
    }
    double t0 = now(), tl = t0, last_shot = 0, last_latest = 0;
    uint32_t frames = 0, f_last = 0, ch_first = 0, ch_last = 0, ch_at = 0, base = 0;
    uint64_t bytes = 0, b_last = 0, written = 0, a_last = 0;
    int have_base = 0;
    while (!g_stop) {
        if (need(t, hdr, 4) < 0) break;
        int sound = memcmp(hdr, "PXCA", 4) == 0, rep = !memcmp(hdr, "PXCR", 4) || !memcmp(hdr, "PXCL", 4);
        if (!sound && !rep && memcmp(hdr, "PXCF", 4) != 0) {   /* out of step: slide one byte at a time to the next header */
            memmove(hdr, hdr + 1, 3);
            continue;
        }
        if (need(t, hdr + 4, 28) < 0) break;
        uint32_t when = get32(hdr + 20), len = get32(hdr + 24);
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
        if (sound) {
            uint32_t chans = get16(hdr + 6), samples = get32(hdr + 16);
            if (len > sizeof(pcm) || len != samples * chans * 2) { fprintf(stderr, "bad sound block (%u bytes)\n", len); continue; }
            if (need(t, (uint8_t *)pcm, len) < 0) break;
            mix_block(hdr[4], hdr[5], (int)chans, get32(hdr + 8), (int)get16(hdr + 12), (int)get16(hdr + 14), samples, pcm, at);
            g_audio_blocks++; g_audio_bytes += len; bytes += 32 + len;
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
            if (!bad && written <= due) { bad = fwrite(rgb, 1, sizeof(rgb), ff) != sizeof(rgb); written++; }
            if (bad) { fprintf(stderr, "ffmpeg stopped\n"); break; }
            memcpy(prev, rgb, sizeof(rgb));
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
            fprintf(stderr, "received %.1f frames/s, %.1f MB/s, sound %.0f KB/s; the game %.1f fps; format %u\n",
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
    while ((c = getopt(argc, argv, "o:d:t:c:k")) != -1) {
        if (c == 'o') out = optarg;
        else if (c == 'd') dir = optarg;
        else if (c == 't') limit = atof(optarg);
        else if (c == 'c') fifo = optarg;
        else if (c == 'k') keep = 1;
        else { fprintf(stderr, "usage: pspexp-capture [-o clip.mp4] [-d frames-folder] [-t seconds] [-c control-fifo] [-k]\n"); return 2; }
    }
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    {   /* one receiver at a time: a second one, idle, read the FIFO's commands meant for the one
         * holding the link (button presses lost, 2026-10-23) */
        char lock[1100];
        snprintf(lock, sizeof(lock), "%s/pspexp-capture.lock", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        int fd = open(lock, O_CREAT | O_RDWR, 0600);
        if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) { fprintf(stderr, "pspexp-capture: another one is running (%s)\n", lock); return 1; }
    }
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
        fprintf(stderr, "pspexp-capture: waiting for a PSP game with PSPExp Capture on the USB cable...\n");
        PspExpTransport *t = NULL;
        while (!t && !g_quit) t = pspexp_usb_wait(&o, 1000, err, sizeof(err));   /* (in steps: Ctrl-C ends a wait) */
        if (!t) break;
        fprintf(stderr, "pspexp-capture: connected\n");
        pthread_mutex_lock(&g_link_lock); g_link = t; pthread_mutex_unlock(&g_link_lock);
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
