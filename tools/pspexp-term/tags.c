/* A song's title and artist from its file: see tags.h. (player.c reads the playing track's own,
 * its cover too, the same way for MP3.) */
#include "tags.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define HEAD 32768

/* text in ID3's encoding enc (0 Latin-1, 1 UTF-16 with BOM, 2 UTF-16BE, 3 UTF-8) to UTF-8 */
static void id3_text(int enc, const uint8_t *s, size_t n, char *out, size_t on)
{
    size_t o = 0, i = 0;
    int be = enc == 2;
    if (!on) return;
    if (enc == 1 && n >= 2) { be = s[0] == 0xFE; i = 2; }
    while (i < n && o + 4 < on) {
        unsigned c;
        if (enc == 0) c = s[i++];
        else if (enc == 3) { if (!s[i]) break; out[o++] = (char)s[i++]; continue; }
        else { if (i + 1 >= n) break; c = be ? (unsigned)(s[i] << 8 | s[i + 1]) : (unsigned)(s[i + 1] << 8 | s[i]); i += 2; }
        if (!c) break;
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 63)); }
        else { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | (c >> 6 & 63)); out[o++] = (char)(0x80 | (c & 63)); }
    }
    while (o && out[o - 1] == ' ') o--;   /* (ID3v1 pads with spaces) */
    out[o] = 0;
}

static int id3v2(const uint8_t *b, size_t n, char *title, size_t tn, char *artist, size_t an)
{
    size_t size, i = 10;
    int ver;
    if (n < 10 || memcmp(b, "ID3", 3)) return 0;
    ver = b[3];
    size = (size_t)(b[6] & 127) << 21 | (size_t)(b[7] & 127) << 14 | (size_t)(b[8] & 127) << 7 | (b[9] & 127);
    size += 10;
    if (size > n) size = n;   /* (a large tag, its picture first: as far as was read) */
    if (ver >= 3 && (b[5] & 0x40) && i + 4 <= size)
        i += ver == 4 ? ((size_t)(b[i] & 127) << 21 | (size_t)(b[i + 1] & 127) << 14 | (size_t)(b[i + 2] & 127) << 7 | (b[i + 3] & 127))
                      : ((size_t)b[i] << 24 | (size_t)b[i + 1] << 16 | (size_t)b[i + 2] << 8 | b[i + 3]) + 4;
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
        if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) id3_text(d[0], d + 1, fs - 1, title, tn);
        else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) id3_text(d[0], d + 1, fs - 1, artist, an);
        if (title[0] && artist[0]) break;
        i += hs + fs;
    }
    return title[0] != 0;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Vorbis comments (FLAC's block, Ogg Vorbis's and Opus's header): vendor, then "KEY=value" */
static int vorbis_comments(const uint8_t *p, size_t n, char *title, size_t tn, char *artist, size_t an)
{
    size_t i = 0;
    uint32_t vl, count, k;
    if (n < 8) return 0;
    vl = le32(p); i = 4 + (size_t)vl;
    if (i + 4 > n) return 0;
    count = le32(p + i); i += 4;
    for (k = 0; k < count && i + 4 <= n; k++) {
        uint32_t l = le32(p + i);
        const char *c = (const char *)p + i + 4;
        i += 4;
        if (l > n - i) break;
        if (l > 6 && !strncasecmp(c, "TITLE=", 6) && !title[0]) snprintf(title, tn, "%.*s", (int)(l - 6), c + 6);
        else if (l > 7 && !strncasecmp(c, "ARTIST=", 7) && !artist[0]) snprintf(artist, an, "%.*s", (int)(l - 7), c + 7);
        i += l;
        if (title[0] && artist[0]) break;
    }
    return title[0] != 0;
}

static int flac(const uint8_t *b, size_t n, char *title, size_t tn, char *artist, size_t an)
{
    size_t i = 4;
    if (n < 8 || memcmp(b, "fLaC", 4)) return 0;
    while (i + 4 <= n) {
        int last = b[i] & 0x80, type = b[i] & 0x7F;
        size_t len = (size_t)b[i + 1] << 16 | (size_t)b[i + 2] << 8 | b[i + 3];
        i += 4;
        if (type == 4) return vorbis_comments(b + i, len < n - i ? len : n - i, title, tn, artist, an);
        if (last) break;
        i += len;
    }
    return 0;
}

/* Ogg: the comment header ("\x03vorbis" or "OpusTags"), assumed within the first page that holds
 * it (its packet split over pages: what was read of it) */
static int ogg(const uint8_t *b, size_t n, char *title, size_t tn, char *artist, size_t an)
{
    size_t i;
    if (n < 28 || memcmp(b, "OggS", 4)) return 0;
    for (i = 0; i + 8 <= n; i++) {
        if (!memcmp(b + i, "\x03vorbis", 7)) return vorbis_comments(b + i + 7, n - i - 7, title, tn, artist, an);
        if (!memcmp(b + i, "OpusTags", 8)) return vorbis_comments(b + i + 8, n - i - 8, title, tn, artist, an);
    }
    return 0;
}

int tags_parse(const uint8_t *b, size_t n, const uint8_t *tail, char *title, size_t tn, char *artist, size_t an)
{
    if (tn) title[0] = 0;
    if (an) artist[0] = 0;
    if (!tn || !an) return 0;
    if (id3v2(b, n, title, tn, artist, an) || flac(b, n, title, tn, artist, an) || ogg(b, n, title, tn, artist, an)) return 1;
    if (tail && !memcmp(tail, "TAG", 3)) {   /* ID3v1 */
        id3_text(0, tail + 3, 30, title, tn);
        if (!artist[0]) id3_text(0, tail + 33, 30, artist, an);
    }
    return title[0] != 0;
}

int tags_peek(const char *path, char *title, size_t tn, char *artist, size_t an)
{
    static uint8_t head[HEAD];
    uint8_t tail[128];
    size_t n;
    int have_tail = 0;
    FILE *f = fopen(path, "rb");
    if (tn) title[0] = 0;
    if (an) artist[0] = 0;
    if (!f) return 0;
    n = fread(head, 1, sizeof(head), f);
    if (fseek(f, -128, SEEK_END) == 0 && fread(tail, 1, sizeof(tail), f) == sizeof(tail)) have_tail = 1;
    fclose(f);
    return tags_parse(head, n, have_tail ? tail : NULL, title, tn, artist, an);
}
