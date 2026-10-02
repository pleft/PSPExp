/* pspexp-term's music player: see player.h */
#include "player.h"
#include "../../protocol/pspexp_mp3.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define RATE 44100            /* the console's stream rate (HELLO_ACK) */
#define LEAD 0.40             /* seconds sent ahead of the play clock */
#define MP3_MAX (100u << 20)  /* an MP3 is read whole */
#define COVER_FILE "/tmp/pspexp-player-cover"

enum { K_NONE, K_MP3, K_WAV, K_PIPE };

static struct {
    int kind;
    char path[1024];
    PlayerInfo info;
    /* MP3, in memory */
    uint8_t *buf; size_t start, end, cur;
    PspExpMp3Frame first;
    uint32_t xing_frames, xing_bytes; uint8_t toc[100]; int has_toc, vbr;
    /* WAV */
    FILE *f; long data_at, data_len; int channels;
    /* decoded by ffmpeg */
    pid_t pid; int fd; int partial; uint8_t pend[4];
    /* the clock: pos = base + (now - t0), never past what was sent */
    double base, t0, sent, paused_at;
    int eof, reset_due;
    int local;                 /* started while the sound went to the board's output (Bluetooth) */
} P;

static double now_s(void) { return (double)pspexp_time_us() / 1e6; }

int player_handles(const char *name)
{
    static const char *EXT[] = { "mp3", "wav", "ogg", "oga", "flac", "opus", "m4a", "aac", "wma", "mod", "xm", "it", "s3m", NULL };
    const char *dot = strrchr(name, '.');
    int i;
    if (!dot) return 0;
    for (i = 0; EXT[i]; i++) if (!strcasecmp(dot + 1, EXT[i])) return 1;
    return 0;
}

static int have_ffmpeg(void)
{
    static int have = -1;
    if (have < 0) have = system("command -v ffmpeg >/dev/null 2>&1") == 0;
    return have;
}

/* ---- text from tags ----------------------------------------------------------- */

static void put_utf8(char **o, char *end, unsigned cp)
{
    char *p = *o;
    if (cp < 0x80) { if (p + 1 < end) *p++ = (char)cp; }
    else if (cp < 0x800) { if (p + 2 < end) { *p++ = (char)(0xC0 | cp >> 6); *p++ = (char)(0x80 | (cp & 63)); } }
    else if (p + 3 < end) { *p++ = (char)(0xE0 | cp >> 12); *p++ = (char)(0x80 | ((cp >> 6) & 63)); *p++ = (char)(0x80 | (cp & 63)); }
    *o = p;
}

/* an ID3 text in encoding enc (0 Latin-1, 1 UTF-16 with BOM, 2 UTF-16BE, 3 UTF-8) to UTF-8 */
static void id3_text(int enc, const uint8_t *s, size_t n, char *out, size_t on)
{
    char *o = out, *end = out + on;
    size_t i = 0;
    if (!on) return;
    if (enc == 1 || enc == 2) {
        int be = enc == 2;
        if (n >= 2 && s[0] == 0xFF && s[1] == 0xFE) { be = 0; i = 2; }
        else if (n >= 2 && s[0] == 0xFE && s[1] == 0xFF) { be = 1; i = 2; }
        for (; i + 1 < n; i += 2) {
            unsigned c = be ? (unsigned)(s[i] << 8 | s[i + 1]) : (unsigned)(s[i + 1] << 8 | s[i]);
            if (!c) break;
            if (c >= 0xD800 && c < 0xE000) c = '?';   /* outside the fonts' plane anyway */
            put_utf8(&o, end, c);
        }
    } else {
        for (; i < n && s[i]; i++) {
            if (enc == 3) { if (o + 1 < end) *o++ = (char)s[i]; }
            else put_utf8(&o, end, s[i]);
        }
    }
    *o = 0;
    while (o > out && (o[-1] == ' ')) *--o = 0;
}

/* the end of a terminated string in encoding enc, past its terminator */
static size_t id3_skip_str(int enc, const uint8_t *s, size_t n)
{
    size_t i = 0;
    if (enc == 1 || enc == 2) { for (; i + 1 < n; i += 2) if (!s[i] && !s[i + 1]) return i + 2; return n; }
    for (; i < n; i++) if (!s[i]) return i + 1;
    return n;
}

static void save_cover(const char *mime, const uint8_t *d, size_t n)
{
    char path[64];
    FILE *f;
    if (P.info.cover[0] || n < 64) return;
    snprintf(path, sizeof(path), "%s.%s", COVER_FILE, (mime && strstr(mime, "png")) || (n > 4 && d[0] == 0x89 && d[1] == 'P') ? "png" : "jpg");
    if (!(f = fopen(path, "wb"))) return;
    if (fwrite(d, 1, n, f) == n) snprintf(P.info.cover, sizeof(P.info.cover), "%s", path);
    fclose(f);
}

/* ID3v2 (2.2, 2.3, 2.4) at the file's start: title, artist, album, picture; returns its size */
static size_t id3v2(const uint8_t *b, size_t n)
{
    size_t size, i;
    int ver;
    if (n < 10 || memcmp(b, "ID3", 3)) return 0;
    ver = b[3];
    size = (size_t)(b[6] & 127) << 21 | (size_t)(b[7] & 127) << 14 | (size_t)(b[8] & 127) << 7 | (b[9] & 127);
    size += 10 + ((b[5] & 0x10) ? 10 : 0);
    if (size > n) return n;
    i = 10;
    if (ver >= 3 && (b[5] & 0x40) && i + 4 <= size) {   /* an extended header */
        size_t x = ver == 4 ? ((size_t)(b[i] & 127) << 21 | (size_t)(b[i + 1] & 127) << 14 | (size_t)(b[i + 2] & 127) << 7 | (b[i + 3] & 127))
                            : ((size_t)b[i] << 24 | (size_t)b[i + 1] << 16 | (size_t)b[i + 2] << 8 | b[i + 3]) + 4;
        i += x;
    }
    while (i + (ver == 2 ? 6 : 10) <= size) {
        char id[5] = { 0 };
        size_t fs, hs = ver == 2 ? 6 : 10;
        const uint8_t *d;
        if (!b[i]) break;   /* padding */
        memcpy(id, b + i, ver == 2 ? 3 : 4);
        if (ver == 2) fs = (size_t)b[i + 3] << 16 | (size_t)b[i + 4] << 8 | b[i + 5];
        else if (ver == 4) fs = (size_t)(b[i + 4] & 127) << 21 | (size_t)(b[i + 5] & 127) << 14 | (size_t)(b[i + 6] & 127) << 7 | (b[i + 7] & 127);
        else fs = (size_t)b[i + 4] << 24 | (size_t)b[i + 5] << 16 | (size_t)b[i + 6] << 8 | b[i + 7];
        if (fs == 0 || i + hs + fs > size) break;
        d = b + i + hs;
        if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) id3_text(d[0], d + 1, fs - 1, P.info.title, sizeof(P.info.title));
        else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) id3_text(d[0], d + 1, fs - 1, P.info.artist, sizeof(P.info.artist));
        else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) id3_text(d[0], d + 1, fs - 1, P.info.album, sizeof(P.info.album));
        else if (!strcmp(id, "APIC") && fs > 4) {   /* encoding, MIME type, picture type, description, data */
            int enc = d[0];
            size_t k = 1, m = id3_skip_str(0, d + 1, fs - 1);
            char mime[32];
            snprintf(mime, sizeof(mime), "%.*s", (int)(m > 31 ? 31 : m), (const char *)d + 1);
            k += m + 1;
            if (k < fs) k += id3_skip_str(enc, d + k, fs - k);
            if (k < fs) save_cover(mime, d + k, fs - k);
        } else if (!strcmp(id, "PIC") && fs > 6) {   /* encoding, format (3), type, description, data */
            int enc = d[0];
            size_t k = 5;
            k += id3_skip_str(enc, d + k, fs - k);
            if (k < fs) save_cover(memcmp(d + 1, "PNG", 3) ? "jpeg" : "png", d + k, fs - k);
        }
        i += hs + fs;
    }
    return size;
}

static void id3v1(const uint8_t *t)
{
    if (!P.info.title[0]) id3_text(0, t + 3, 30, P.info.title, sizeof(P.info.title));
    if (!P.info.artist[0]) id3_text(0, t + 33, 30, P.info.artist, sizeof(P.info.artist));
    if (!P.info.album[0]) id3_text(0, t + 63, 30, P.info.album, sizeof(P.info.album));
}

/* a picture next to the file: folder.jpg, cover.jpg... */
static void cover_beside(const char *path)
{
    static const char *NAMES[] = { "cover.jpg", "folder.jpg", "Folder.jpg", "Cover.jpg", "front.jpg", "cover.png", "folder.png", NULL };
    char dir[1024], p[1100];
    const char *sl = strrchr(path, '/');
    int i;
    struct stat st;
    if (P.info.cover[0] || !sl) return;
    snprintf(dir, sizeof(dir), "%.*s", (int)(sl - path), path);
    for (i = 0; NAMES[i]; i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, NAMES[i]);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) { snprintf(P.info.cover, sizeof(P.info.cover), "%s", p); return; }
    }
}

/* ---- MP3 ----------------------------------------------------------------------- */

/* a frame header at i that another follows (or the data ends): not a false sync */
static int mp3_frame_at(size_t i, PspExpMp3Frame *f)
{
    PspExpMp3Frame g;
    if (pspexp_mp3_header(P.buf + i, P.end - i, f) < 0) return 0;
    if (i + (size_t)f->bytes >= P.end) return i + (size_t)f->bytes == P.end;
    return pspexp_mp3_header(P.buf + i + f->bytes, P.end - i - (size_t)f->bytes, &g) == 0 && g.sample_rate == f->sample_rate;
}

static size_t mp3_sync(size_t i)
{
    PspExpMp3Frame f;
    for (; i + 4 <= P.end; i++) if (P.buf[i] == 0xFF && mp3_frame_at(i, &f)) return i;
    return P.end;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* the first frame's Xing / Info header (VBR: frame count and seek table) */
static void mp3_xing(void)
{
    const uint8_t *fr = P.buf + P.start, *x;
    int side = P.first.version == 10 ? (P.first.channels == 2 ? 32 : 17) : (P.first.channels == 2 ? 17 : 9);
    uint32_t flags;
    if ((size_t)(4 + side + 120) > (size_t)P.first.bytes) return;
    x = fr + 4 + side;
    if (memcmp(x, "Xing", 4) && memcmp(x, "Info", 4)) return;
    P.vbr = !memcmp(x, "Xing", 4);   /* "Info": the same header on a constant-bitrate file */
    flags = be32(x + 4);
    x += 8;
    if (flags & 1) { P.xing_frames = be32(x); x += 4; }
    if (flags & 2) { P.xing_bytes = be32(x); x += 4; }
    if (flags & 4) { memcpy(P.toc, x, 100); P.has_toc = 1; }
    P.start += (size_t)P.first.bytes;   /* not audio: the decoder would give silence */
}

static int mp3_open(const char *path, char *err, size_t en)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) { snprintf(err, en, "Cannot read it: %s", strerror(errno)); return -1; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || (unsigned long)n > MP3_MAX || !(P.buf = malloc((size_t)n))) { fclose(f); snprintf(err, en, "It is too large to play (over 100 MB)."); return -1; }
    if (fread(P.buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); snprintf(err, en, "Cannot read it: %s", strerror(errno)); return -1; }
    fclose(f);
    P.end = (size_t)n;
    if (P.end >= 128 && !memcmp(P.buf + P.end - 128, "TAG", 3)) { id3v1(P.buf + P.end - 128); P.end -= 128; }
    P.start = mp3_sync(id3v2(P.buf, P.end));
    if (P.start >= P.end || pspexp_mp3_header(P.buf + P.start, P.end - P.start, &P.first) < 0) {
        snprintf(err, en, "No MP3 (MPEG layer III) frames found in it."); return -1;
    }
    mp3_xing();
    P.cur = P.start;
    if (P.xing_frames) P.info.dur = (double)P.xing_frames * P.first.samples / P.first.sample_rate;
    else P.info.dur = (double)(P.end - P.start) * 8.0 / (P.first.bitrate_kbps * 1000.0);
    snprintf(P.info.format, sizeof(P.info.format), "MP3  %s%d kbps  %.1f kHz%s", P.vbr ? "~" : "",
             P.vbr && P.info.dur > 0 ? (int)((P.end - P.start) * 8.0 / P.info.dur / 1000.0 + 0.5) : P.first.bitrate_kbps,
             P.first.sample_rate / 1000.0, P.first.channels == 1 ? "  mono" : "");
    return 0;
}

static void mp3_seek(double t)
{
    size_t span = P.end - P.start, off;
    if (P.info.dur <= 0) { P.cur = P.start; return; }
    if (P.has_toc) {
        double pct = t / P.info.dur * 100.0, a, b;
        int k = (int)pct;
        if (k > 99) k = 99;
        a = P.toc[k]; b = k < 99 ? P.toc[k + 1] : 256.0;
        off = (size_t)((a + (b - a) * (pct - k)) / 256.0 * (P.xing_bytes ? P.xing_bytes : span));
    } else off = (size_t)(t / P.info.dur * (double)span);
    P.cur = mp3_sync(P.start + (off < span ? off : span));
}

static void mp3_feed(PspExpContext *ctx, double until)
{
    while (P.sent < until && P.cur < P.end) {
        size_t from = P.cur;
        int frames = 0;
        PspExpMp3Frame f;
        while (P.cur < P.end && frames < 8 && P.cur - from < 12000) {   /* up to 8 frames (0.2 s) a message */
            if (pspexp_mp3_header(P.buf + P.cur, P.end - P.cur, &f) < 0 || P.cur + (size_t)f.bytes > P.end) {
                P.cur = mp3_sync(P.cur + 1);   /* damage: on to the next good frame */
                break;
            }
            P.cur += (size_t)f.bytes;
            P.sent += (double)f.samples / f.sample_rate;
            frames++;
        }
        if (P.cur > from && frames && pspexp_audio_coded(ctx, PSPEXP_CODEC_MP3, P.buf + from, (uint32_t)(P.cur - from)) < 0) break;
    }
    if (P.cur >= P.end) P.eof = 1;
}

/* ---- WAV ------------------------------------------------------------------------- */

static int wav_open(const char *path, char *err, size_t en)
{
    uint8_t h[12], c[8], fmt[16] = { 0 };
    int have_fmt = 0;
    if (!(P.f = fopen(path, "rb"))) { snprintf(err, en, "Cannot read it: %s", strerror(errno)); return -1; }
    if (fread(h, 1, 12, P.f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return 1;
    while (fread(c, 1, 8, P.f) == 8) {
        long len = (long)(c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24);
        if (!memcmp(c, "fmt ", 4) && len >= 16) {
            if (fread(fmt, 1, 16, P.f) != 16) return 1;
            have_fmt = 1;
            fseek(P.f, len - 16 + (len & 1), SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            int format = fmt[0] | fmt[1] << 8, rate = (int)(fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (uint32_t)fmt[7] << 24);
            P.channels = fmt[2];
            if (!have_fmt || (format != 1 && format != 0xFFFE) || (fmt[14] | fmt[15] << 8) != 16 || rate != RATE || P.channels < 1 || P.channels > 2) return 1;   /* ffmpeg's */
            P.data_at = ftell(P.f);
            P.data_len = len;
            P.info.dur = (double)len / (2.0 * P.channels) / RATE;
            snprintf(P.info.format, sizeof(P.info.format), "WAV  16-bit  44.1 kHz%s", P.channels == 1 ? "  mono" : "");
            return 0;
        } else fseek(P.f, len + (len & 1), SEEK_CUR);
    }
    return 1;
}

static void wav_seek(double t)
{
    long frame = (long)(t * RATE);
    fseek(P.f, P.data_at + frame * 2 * P.channels, SEEK_SET);
}

static void wav_feed(PspExpContext *ctx, double until)
{
    static int16_t in[2048 * 2], out[2048 * 2];
    while (P.sent < until && !P.eof) {
        long left = P.data_at + P.data_len - ftell(P.f);
        size_t want = 1024, n, i;
        if (left <= 0) { P.eof = 1; break; }
        if ((long)(want * 2 * (size_t)P.channels) > left) want = (size_t)left / (2u * (size_t)P.channels);
        n = fread(in, 2u * (size_t)P.channels, want, P.f);
        if (!n) { P.eof = 1; break; }
        if (P.channels == 1) for (i = 0; i < n; i++) out[2 * i] = out[2 * i + 1] = in[i];
        else memcpy(out, in, n * 4);
        if (pspexp_audio_stream(ctx, out, (uint32_t)n) < 0) { fseek(P.f, -(long)(n * 2 * (size_t)P.channels), SEEK_CUR); break; }
        P.sent += (double)n / RATE;
    }
}

/* ---- decoded by ffmpeg ------------------------------------------------------------ */

/* argv's standard output, whole (up to n - 1 bytes); its exit status */
static int capture(char *const argv[], char *out, size_t n)
{
    int fds[2], st = -1;
    size_t got = 0;
    ssize_t r;
    pid_t pid;
    if (pipe(fds) < 0) return -1;
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        dup2(fds[1], 1); dup2(nul, 2); dup2(nul, 0);
        close(fds[0]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    while (got + 1 < n && (r = read(fds[0], out + got, n - 1 - got)) > 0) got += (size_t)r;
    out[got] = 0;
    close(fds[0]);
    waitpid(pid, &st, 0);
    return st;
}

static void probe_line(const char *all, const char *key, char *out, size_t n)
{
    const char *p = all;
    size_t kl = strlen(key);
    if (out[0]) return;   /* a tag read already wins */
    while (p && *p) {
        if (!strncasecmp(p, key, kl) && p[kl] == '=') {
            const char *e = strchr(p, '\n');
            snprintf(out, n, "%.*s", (int)(e ? e - p - (int)kl - 1 : (long)strlen(p + kl + 1)), p + kl + 1);
            return;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
}

static void probe(const char *path)
{
    static char out[8192];
    char dur[32] = "", fmt[64] = "", codec[64] = "", rate[32] = "";
    char *argv[] = { "ffprobe", "-v", "error", "-show_entries",
                     "format=duration,format_name:format_tags=title,artist,album:stream=codec_name,sample_rate",
                     "-select_streams", "a:0", "-of", "default=nw=1", (char *)path, NULL };
    if (capture(argv, out, sizeof(out)) != 0) return;
    probe_line(out, "TAG:title", P.info.title, sizeof(P.info.title));
    probe_line(out, "TAG:artist", P.info.artist, sizeof(P.info.artist));
    probe_line(out, "TAG:album", P.info.album, sizeof(P.info.album));
    probe_line(out, "duration", dur, sizeof(dur));
    probe_line(out, "format_name", fmt, sizeof(fmt));
    probe_line(out, "codec_name", codec, sizeof(codec));
    probe_line(out, "sample_rate", rate, sizeof(rate));
    if (dur[0] && strcmp(dur, "N/A")) P.info.dur = atof(dur);
    if (!P.info.format[0] && codec[0]) {
        char *s;
        for (s = codec; *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
        snprintf(P.info.format, sizeof(P.info.format), "%s  %.1f kHz", codec, atoi(rate) / 1000.0);
    }
}

static void pipe_close(void)
{
    if (P.fd >= 0) close(P.fd);
    if (P.pid > 0) { kill(P.pid, SIGKILL); waitpid(P.pid, NULL, 0); }   /* (TERM: ffmpeg finishes its write first, to a pipe nobody reads) */
    P.pid = -1; P.fd = -1; P.partial = 0;
}

static int pipe_open(double at)
{
    int fds[2];
    char ss[32];
    snprintf(ss, sizeof(ss), "%.3f", at);
    pipe_close();
    if (pipe(fds) < 0) return -1;
    P.pid = fork();
    if (P.pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (P.pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        dup2(fds[1], 1); dup2(nul, 2); dup2(nul, 0);
        close(fds[0]);
        execlp("ffmpeg", "ffmpeg", "-nostdin", "-v", "error", "-ss", ss, "-i", P.path, "-vn",
               "-f", "s16le", "-ac", "2", "-ar", "44100", "-", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    P.fd = fds[0];
    fcntl(P.fd, F_SETFL, fcntl(P.fd, F_GETFL) | O_NONBLOCK);
    return 0;
}

static void pipe_feed(PspExpContext *ctx, double until)
{
    static uint8_t b[4096 * 4 + 4];
    while (P.sent < until && P.fd >= 0) {
        size_t want = (size_t)((until - P.sent) * RATE) + 1;
        ssize_t r;
        if (want > 4096) want = 4096;
        memcpy(b, P.pend, (size_t)P.partial);
        r = read(P.fd, b + P.partial, want * 4 - (size_t)P.partial);
        if (r == 0) { P.eof = 1; pipe_close(); break; }
        if (r < 0) break;   /* nothing yet (EAGAIN) */
        r += P.partial;
        P.partial = (int)(r % 4);
        memcpy(P.pend, b + r - P.partial, (size_t)P.partial);
        if (r / 4) { pspexp_audio_stream(ctx, (const int16_t *)b, (uint32_t)(r / 4)); P.sent += (double)(r / 4) / RATE; }
    }
}

/* ---- the player ---------------------------------------------------------------------- */

void player_stop(void)
{
    if (P.kind == K_PIPE) pipe_close();
    if (P.f) fclose(P.f);
    free(P.buf);
    if (P.info.cover[0] && !strncmp(P.info.cover, COVER_FILE, strlen(COVER_FILE))) unlink(P.info.cover);
    memset(&P, 0, sizeof(P));
    P.fd = -1; P.pid = -1;
    P.reset_due = 1;   /* silence what the PSP still holds */
}

static void restart_at(double t)
{
    if (t < 0) t = 0;
    if (P.info.dur > 0 && t > P.info.dur) t = P.info.dur;
    if (P.kind == K_MP3) mp3_seek(t);
    else if (P.kind == K_WAV) wav_seek(t);
    else pipe_open(t);
    P.base = P.sent = t;
    P.t0 = now_s() + 0.1;   /* the PSP holds back a tenth of a second before it plays */
    P.eof = 0;
    P.reset_due = 1;
    P.info.paused = 0; P.info.ended = 0; P.info.playing = 1;
}

int player_start(PspExpContext *ctx, const char *path, char *err, size_t en)
{
    const char *dot = strrchr(path, '.'), *sl = strrchr(path, '/');
    int r = 1;
    player_stop();
    if (!ctx || pspexp_link_state(ctx) != PSPEXP_LINK_CONNECTED) { snprintf(err, en, "The PSP is not connected."); return -1; }
    snprintf(P.path, sizeof(P.path), "%s", path);
    if (dot && !strcasecmp(dot, ".mp3")) {
        if (mp3_open(path, err, en) < 0) { player_stop(); return -1; }
        if (pspexp_audio_mp3_supported(ctx) && !pspexp_audio_local(ctx)) { P.kind = K_MP3; snprintf(P.info.decoder, sizeof(P.info.decoder), "Decoded on the PSP (Media Engine)"); r = 0; }
        else { free(P.buf); P.buf = NULL; }   /* an older runtime: ffmpeg's, below */
    } else if (dot && !strcasecmp(dot, ".wav")) {
        r = wav_open(path, err, en);
        if (r < 0) { player_stop(); return -1; }
        if (r == 0) { P.kind = K_WAV; snprintf(P.info.decoder, sizeof(P.info.decoder), "Sent as it is (PCM)"); }
        else if (P.f) { fclose(P.f); P.f = NULL; }
    }
    if (r) {
        if (!have_ffmpeg()) {
            snprintf(err, en, dot && !strcasecmp(dot, ".mp3") ? "The PSP's PSPExp is too old to play MP3 (System > Update the PSP), and ffmpeg is not installed on the board."
                                                               : "Only MP3 and 44.1 kHz WAV play without ffmpeg, which is not installed on the board.");
            player_stop();
            return -1;
        }
        probe(path);
        P.kind = K_PIPE; P.fd = -1; P.pid = -1;
        snprintf(P.info.decoder, sizeof(P.info.decoder), "Decoded on the board (ffmpeg)");
    }
    P.local = pspexp_audio_local(ctx);
    if (P.local && P.kind != K_MP3) snprintf(P.info.decoder, sizeof(P.info.decoder), "%s", P.kind == K_WAV ? "On the board's Bluetooth device" : "Decoded on the board, to Bluetooth");
    if (!P.info.title[0]) {   /* the file's name, without its extension */
        const char *base = sl ? sl + 1 : path;
        int len = dot && dot > base ? (int)(dot - base) : (int)strlen(base);
        snprintf(P.info.title, sizeof(P.info.title), "%.*s", len, base);
    }
    cover_beside(path);
    restart_at(0);
    return 0;
}

static double clock_pos(void)
{
    double t;
    if (!P.kind) return 0;
    if (P.info.paused) return P.paused_at;
    t = P.base + (now_s() - P.t0);
    if (t < P.base) t = P.base;
    if (t > P.sent) t = P.sent;
    return t;
}

void player_toggle_pause(void)
{
    if (!P.kind || P.info.ended) return;
    if (!P.info.paused) {
        P.paused_at = clock_pos();
        P.info.paused = 1;
        P.reset_due = 1;   /* stop the sound now, not after what is buffered */
    } else restart_at(P.paused_at);
}

void player_seek(double delta_s)
{
    double t;
    if (!P.kind) return;
    t = clock_pos() + delta_s;
    if (P.info.dur > 0 && t >= P.info.dur) t = P.info.dur - 0.5;
    if (P.info.paused) { P.paused_at = t < 0 ? 0 : t; return; }
    restart_at(t);
}

void player_pump(PspExpContext *ctx)
{
    double now, until;
    if (!ctx) return;
    if (P.kind && !P.info.ended && pspexp_audio_local(ctx) != P.local) {   /* the earbuds came or went: the track again from here, the right way */
        char path[sizeof(P.path)], err[200];
        double at = P.info.paused ? P.paused_at : clock_pos();
        int paused = P.info.paused;
        snprintf(path, sizeof(path), "%s", P.path);
        if (player_start(ctx, path, err, sizeof(err)) == 0) {
            restart_at(at);
            if (paused) player_toggle_pause();
        }
    }
    if (!P.local && pspexp_link_state(ctx) != PSPEXP_LINK_CONNECTED) return;
    if (P.reset_due) { pspexp_audio_stream_reset(ctx); P.reset_due = 0; }
    if (!P.kind || P.info.paused || P.info.ended) return;
    now = now_s();
    if (!P.eof && P.base + (now - P.t0) > P.sent + 0.5) P.t0 = now - (P.sent - P.base);   /* starved (the link, ffmpeg): the clock waits */
    until = P.base + (now - P.t0) + LEAD;
    if (P.kind == K_MP3) mp3_feed(ctx, until);
    else if (P.kind == K_WAV) wav_feed(ctx, until);
    else pipe_feed(ctx, until);
    if (P.eof && P.base + (now - P.t0) >= P.sent + 0.15) { P.info.ended = 1; P.info.playing = 0; }
}

int player_info(PlayerInfo *out)
{
    if (!P.kind) return 0;
    *out = P.info;
    out->pos = P.info.ended ? P.sent : clock_pos();
    if (out->dur > 0 && out->pos > out->dur) out->pos = out->dur;
    return 1;
}
