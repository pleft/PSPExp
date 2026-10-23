/*
 * pspexp-capture: the receiving end of PSPExp Capture (psp/capture, a custom-firmware plugin that
 * sends a native PSP game's picture over USB). Stage 1, the prototype: it reads the frames, tells
 * the game's frame rate and what arrives, saves a frame as a picture every few seconds, and with
 * -o pipes them into ffmpeg for an MP4.
 *   pspexp-capture [-o clip.mp4] [-d folder-for-frames] [-t seconds]
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

#define W 480
#define H 272

static volatile int g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint32_t get16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }

/* the game's pixels (its format: 0 565, 1 5551, 2 4444, 3 8888) as 8-bit RGB */
static void to_rgb(const uint8_t *src, int fmt, uint8_t *rgb)
{
    int i;
    for (i = 0; i < W * H; i++, rgb += 3) {
        if (fmt == 3) { rgb[0] = src[i * 4]; rgb[1] = src[i * 4 + 1]; rgb[2] = src[i * 4 + 2]; continue; }
        uint32_t v = get16(src + i * 2);
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

int main(int argc, char **argv)
{
    const char *out = NULL, *dir = NULL;
    double limit = 0;
    int c;
    while ((c = getopt(argc, argv, "o:d:t:")) != -1) {
        if (c == 'o') out = optarg;
        else if (c == 'd') dir = optarg;
        else if (c == 't') limit = atof(optarg);
        else { fprintf(stderr, "usage: pspexp-capture [-o clip.mp4] [-d frames-folder] [-t seconds]\n"); return 2; }
    }
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    PspExpUsbOptions o;
    char err[256];
    pspexp_usb_options_default(&o);
    fprintf(stderr, "pspexp-capture: waiting for a PSP game with PSPExp Capture on the USB cable...\n");
    PspExpTransport *t = pspexp_usb_wait(&o, -1, err, sizeof(err));
    if (!t) { fprintf(stderr, "pspexp-capture: %s\n", err); return 1; }
    fprintf(stderr, "pspexp-capture: connected\n");

    static uint8_t hdr[32], px[W * H * 4], rgb[W * H * 3], prev[W * H * 3];
    FILE *ff = NULL;
    if (out) {
        char cmd[1024];
        /* a steady 30 a second, each frame placed at the time the PSP showed it (header): one that
         * came late is held, the frames the PSP skipped meanwhile are its copies; a clip plays at
         * the game's own speed */
        snprintf(cmd, sizeof(cmd), "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s %dx%d -r 30 -i - "
                 "-c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p '%s'", W, H, out);
        ff = popen(cmd, "w");
        if (!ff) { perror("ffmpeg"); return 1; }
    }
    double t0 = now(), tl = t0, last_shot = 0;
    uint32_t frames = 0, f_last = 0, ch_first = 0, ch_last = 0, ch_at = 0;
    uint64_t bytes = 0, b_last = 0, written = 0, us = 0;
    uint32_t t_prev = 0;
    while (!g_stop) {
        if (need(t, hdr, 4) < 0) break;
        if (memcmp(hdr, "PXCF", 4) != 0) {   /* out of step: slide one byte at a time to the next header */
            memmove(hdr, hdr + 1, 3);
            continue;
        }
        if (need(t, hdr + 4, 28) < 0) break;
        uint32_t w = get16(hdr + 4), h = get16(hdr + 6), fmt = get16(hdr + 8), changes = get32(hdr + 16), shown = get32(hdr + 20), len = get32(hdr + 24);
        if (w != W || h != H || len > sizeof(px) || fmt > 3) { fprintf(stderr, "bad header (%ux%u fmt %u len %u)\n", w, h, fmt, len); continue; }
        if (need(t, px, len) < 0) break;
        if (!frames) { ch_first = changes; ch_at = changes; t_prev = shown; }
        us += (uint32_t)(shown - t_prev); t_prev = shown;   /* (the PSP's clock: 32 bits of us, wraps) */
        frames++; bytes += 32 + len; ch_last = changes;
        double n = now();
        if (ff || dir) to_rgb(px, (int)fmt, rgb);
        if (ff) {   /* the previous frame held until this one's time, then this one */
            uint64_t due = us * 30 / 1000000;
            int bad = 0;
            while (written && written < due && !bad) { bad = fwrite(prev, 1, sizeof(prev), ff) != sizeof(prev); written++; }
            if (!bad && (!written || written <= due)) { bad = fwrite(rgb, 1, sizeof(rgb), ff) != sizeof(rgb); written++; }
            if (bad) { fprintf(stderr, "ffmpeg stopped\n"); break; }
            memcpy(prev, rgb, sizeof(rgb));
        }
        if (dir && n - last_shot >= 5) {
            char p[512];
            snprintf(p, sizeof(p), "%s/frame-%05u.ppm", dir, frames);
            FILE *f = fopen(p, "wb");
            if (f) { fprintf(f, "P6\n%d %d\n255\n", W, H); fwrite(rgb, 1, sizeof(rgb), f); fclose(f); }
            last_shot = n;
        }
        if (n - tl >= 5) {
            fprintf(stderr, "received %.1f frames/s, %.1f MB/s; the game %.1f fps; format %u\n",
                    (frames - f_last) / (n - tl), (bytes - b_last) / (n - tl) / 1e6, (changes - ch_at) / (n - tl), fmt);
            tl = n; f_last = frames; b_last = bytes; ch_at = changes;
        }
        if (limit > 0 && n - t0 >= limit) break;
    }
    double dt = now() - t0;
    fprintf(stderr, "pspexp-capture: %u frames in %.1f s (%.1f/s); the game changed frames %u times (%.1f/s)\n",
            frames, dt, frames / dt, ch_last - ch_first, (ch_last - ch_first) / dt);
    if (ff) pclose(ff);
    t->close(t);
    return 0;
}
