/*
 * PSP game discs on the board (pspexp_disc.h): ISO, CSO (version 1: deflate blocks, an index of
 * their offsets), or a zip holding an ISO (read front to back).
 */
#include "pspexp_disc.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PSPEXP_HAVE_ZLIB
#include <zlib.h>
#else   /* (ISOs only: CSO and zip need deflate) */
typedef struct { int unused; } z_stream;
#define inflateEnd(z) ((void)(z), 0)
#endif

enum { K_ISO, K_CSO, K_ZIP };

struct PspExpDisc {
    FILE *f;
    int kind;
    uint64_t size;
    /* CSO */
    uint32_t *index, blocks, block_size, align;
    uint8_t *cbuf, *block;
    uint32_t cached;                  /* the block in `block` (+1; 0 none) */
    /* zip: the entry's deflate stream, read so far up to pos */
    uint64_t data_at, csize, pos;
    int method;                       /* 0 stored, 8 deflate */
    z_stream z;
    int z_on;
    uint8_t zin[64 * 1024];
};

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int set_err(char *err, size_t n, const char *msg) { if (err && n) snprintf(err, n, "%s", msg); return -1; }

/* ---- zip: the first entry's local header, from the central directory (sizes are there even
 * when the local header defers them) ---- */
static int zip_setup(PspExpDisc *d, char *err, size_t errn)
{
    uint8_t tail[65536 + 22], h[46], lh[30];
    long len, i, n;
    uint32_t cd_at, local_at, nlen;
    if (fseek(d->f, 0, SEEK_END) != 0 || (len = ftell(d->f)) < 22) return set_err(err, errn, "not a zip");
    n = len < (long)sizeof(tail) ? len : (long)sizeof(tail);
    if (fseek(d->f, len - n, SEEK_SET) != 0 || fread(tail, 1, (size_t)n, d->f) != (size_t)n) return set_err(err, errn, "unreadable");
    for (i = n - 22; i >= 0 && le32(tail + i) != 0x06054b50; i--) ;
    if (i < 0) return set_err(err, errn, "a zip without its directory");
    cd_at = le32(tail + i + 16);
    if (fseek(d->f, (long)cd_at, SEEK_SET) != 0 || fread(h, 1, 46, d->f) != 46 || le32(h) != 0x02014b50) return set_err(err, errn, "a damaged zip");
    d->method = le16(h + 10);
    d->csize = le32(h + 20);
    d->size = le32(h + 24);
    nlen = le16(h + 28);
    local_at = le32(h + 42);
    {
        char name[512];
        size_t k = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
        if (fread(name, 1, k, d->f) != k) return set_err(err, errn, "a damaged zip");
        name[k] = 0;
        if (k < 4 || strcasecmp(name + k - 4, ".iso")) return set_err(err, errn, "the zip holds no ISO first");
    }
    if (d->method != 0 && d->method != 8) return set_err(err, errn, "a zip compressed other than by deflate");
    if (d->size == 0xFFFFFFFFu || d->csize == 0xFFFFFFFFu) return set_err(err, errn, "a zip64 entry (larger than 4 GB)");
    if (fseek(d->f, (long)local_at, SEEK_SET) != 0 || fread(lh, 1, 30, d->f) != 30 || le32(lh) != 0x04034b50) return set_err(err, errn, "a damaged zip");
    d->data_at = local_at + 30u + le16(lh + 26) + le16(lh + 28);
    return 0;
}

#ifdef PSPEXP_HAVE_ZLIB
static void zip_restart(PspExpDisc *d)
{
    if (d->z_on) inflateEnd(&d->z);
    memset(&d->z, 0, sizeof(d->z));
    d->z_on = d->method == 8 && inflateInit2(&d->z, -15) == Z_OK;
    d->pos = 0;
    fseek(d->f, (long)d->data_at, SEEK_SET);
}

/* n bytes of the entry from pos on into out (NULL: skipped); 0 or -1 */
static int zip_pull(PspExpDisc *d, uint8_t *out, uint64_t n)
{
    static uint8_t junk[64 * 1024];
    while (n) {
        size_t want = n > sizeof(junk) ? sizeof(junk) : (size_t)n;
        uint8_t *dst = out ? out : junk;
        if (d->method == 0) {
            if (fread(dst, 1, want, d->f) != want) return -1;
        } else {
            d->z.next_out = dst;
            d->z.avail_out = (uInt)want;
            while (d->z.avail_out) {
                int r;
                if (!d->z.avail_in) {
                    uint64_t used = (uint64_t)ftell(d->f) - d->data_at, left = d->csize - used;
                    size_t k = left < sizeof(d->zin) ? (size_t)left : sizeof(d->zin);
                    if (!k || fread(d->zin, 1, k, d->f) != k) return -1;
                    d->z.next_in = d->zin;
                    d->z.avail_in = (uInt)k;
                }
                r = inflate(&d->z, Z_NO_FLUSH);
                if (r != Z_OK && !(r == Z_STREAM_END && !d->z.avail_out)) return -1;
            }
        }
        d->pos += want;
        n -= want;
        if (out) out += want;
    }
    return 0;
}

/* ---- CSO version 1 ---- */
static int cso_setup(PspExpDisc *d, const uint8_t *h, char *err, size_t errn)
{
    uint32_t i;
    d->size = (uint64_t)le32(h + 8) | (uint64_t)le32(h + 12) << 32;
    d->block_size = le32(h + 16);
    d->align = h[21];
    if (h[20] > 1 || d->block_size != PSPEXP_DISC_SECTOR) return set_err(err, errn, "a CSO of another version or block size");
    d->blocks = (uint32_t)((d->size + d->block_size - 1) / d->block_size);
    d->index = (uint32_t *)malloc(((size_t)d->blocks + 1) * 4);
    d->cbuf = (uint8_t *)malloc((size_t)d->block_size * 2 + 64);
    d->block = (uint8_t *)malloc(d->block_size);
    if (!d->index || !d->cbuf || !d->block) return set_err(err, errn, "no memory");
    if (fseek(d->f, 24, SEEK_SET) != 0 || fread(d->index, 4, (size_t)d->blocks + 1, d->f) != (size_t)d->blocks + 1) return set_err(err, errn, "a damaged CSO");
    for (i = 0; i <= d->blocks; i++) d->index[i] = le32((const uint8_t *)&d->index[i]);
    return 0;
}

static int cso_block(PspExpDisc *d, uint32_t b, uint8_t *out)
{
    uint64_t at = (uint64_t)(d->index[b] & 0x7FFFFFFFu) << d->align, end = (uint64_t)(d->index[b + 1] & 0x7FFFFFFFu) << d->align;
    size_t n = (size_t)(end - at);
    if (end < at || n > (size_t)d->block_size * 2 + 64) return -1;
    if (fseek(d->f, (long)at, SEEK_SET) != 0) return -1;
    if (d->index[b] & 0x80000000u) return fread(out, 1, d->block_size, d->f) == d->block_size ? 0 : -1;   /* stored as it is */
    if (fread(d->cbuf, 1, n, d->f) != n) return -1;
    {
        z_stream z;
        int r;
        memset(&z, 0, sizeof(z));
        if (inflateInit2(&z, -15) != Z_OK) return -1;
        z.next_in = d->cbuf; z.avail_in = (uInt)n;
        z.next_out = out; z.avail_out = d->block_size;
        r = inflate(&z, Z_FINISH);
        inflateEnd(&z);
        return (r == Z_STREAM_END || r == Z_OK || r == Z_BUF_ERROR) && z.avail_out == 0 ? 0 : -1;
    }
}

#else
static void zip_restart(PspExpDisc *d) { (void)d; }
static int zip_pull(PspExpDisc *d, uint8_t *out, uint64_t n) { (void)d; (void)out; (void)n; return -1; }
static int cso_block(PspExpDisc *d, uint32_t b, uint8_t *out) { (void)d; (void)b; (void)out; return -1; }
static int cso_setup(PspExpDisc *d, const uint8_t *h, char *err, size_t errn) { (void)d; (void)h; return set_err(err, errn, "no zlib"); }
#endif

/* ---- the disc ---- */
PspExpDisc *pspexp_disc_open(const char *path, char *err, size_t errn)
{
    PspExpDisc *d = (PspExpDisc *)calloc(1, sizeof(*d));
    uint8_t h[24];
    if (!d) return NULL;
    if (!(d->f = fopen(path, "rb"))) { set_err(err, errn, "cannot open it"); free(d); return NULL; }
    if (fread(h, 1, sizeof(h), d->f) != sizeof(h)) { set_err(err, errn, "too short"); pspexp_disc_close(d); return NULL; }
#ifndef PSPEXP_HAVE_ZLIB
    if (!memcmp(h, "CISO", 4) || le32(h) == 0x04034b50) { set_err(err, errn, "a CSO or zip: this build reads ISOs only (no zlib)"); pspexp_disc_close(d); return NULL; }
#endif
    if (!memcmp(h, "CISO", 4)) {
        d->kind = K_CSO;
        if (cso_setup(d, h, err, errn) < 0) { pspexp_disc_close(d); return NULL; }
    } else if (le32(h) == 0x04034b50) {
        d->kind = K_ZIP;
        if (zip_setup(d, err, errn) < 0) { pspexp_disc_close(d); return NULL; }
        zip_restart(d);
    } else {
        d->kind = K_ISO;
        fseek(d->f, 0, SEEK_END);
        d->size = (uint64_t)ftell(d->f);
    }
    {   /* an ISO 9660 disc: its descriptor at sector 16 */
        uint8_t s[PSPEXP_DISC_SECTOR];
        if (pspexp_disc_read(d, 16, 1, s) < 0 || memcmp(s + 1, "CD001", 5) != 0) {
            set_err(err, errn, "not a disc image (no ISO 9660 descriptor)");
            pspexp_disc_close(d);
            return NULL;
        }
    }
    return d;
}

void pspexp_disc_close(PspExpDisc *d)
{
    if (!d) return;
    if (d->z_on) inflateEnd(&d->z);
    if (d->f) fclose(d->f);
    free(d->index); free(d->cbuf); free(d->block);
    free(d);
}

uint64_t pspexp_disc_size(const PspExpDisc *d) { return d->size; }
const char *pspexp_disc_kind(const PspExpDisc *d) { return d->kind == K_CSO ? "cso" : d->kind == K_ZIP ? "zip" : "iso"; }

int pspexp_disc_read(PspExpDisc *d, uint32_t lba, uint32_t count, void *out)
{
    uint64_t at = (uint64_t)lba * PSPEXP_DISC_SECTOR, n = (uint64_t)count * PSPEXP_DISC_SECTOR;
    uint8_t *o = (uint8_t *)out;
    uint32_t i;
    if (at + n > d->size) return -1;
    switch (d->kind) {
    case K_ISO:
        return fseek(d->f, (long)at, SEEK_SET) == 0 && fread(out, 1, (size_t)n, d->f) == (size_t)n ? 0 : -1;
    case K_CSO:
        for (i = 0; i < count; i++) {
            uint32_t b = lba + i;
            if (d->cached != b + 1) { if (cso_block(d, b, d->block) < 0) { d->cached = 0; return -1; } d->cached = b + 1; }
            memcpy(o + (size_t)i * PSPEXP_DISC_SECTOR, d->block, PSPEXP_DISC_SECTOR);
        }
        return 0;
    default:
        if (at < d->pos) zip_restart(d);   /* (behind: from the start again) */
        if (zip_pull(d, NULL, at - d->pos) < 0) return -1;
        return zip_pull(d, o, n);
    }
}

/* one directory's entry named name (any case): its extent and size, and whether a directory */
static int dir_find(PspExpDisc *d, uint32_t lba, uint32_t size, const char *name, size_t nl, uint32_t *elba, uint32_t *esize, int *isdir)
{
    uint8_t s[PSPEXP_DISC_SECTOR];
    uint32_t sectors = (size + PSPEXP_DISC_SECTOR - 1) / PSPEXP_DISC_SECTOR, k;
    if (sectors > 256) sectors = 256;   /* (a directory of half a MB: none on a PSP disc) */
    for (k = 0; k < sectors; k++) {
        uint32_t i = 0;
        if (pspexp_disc_read(d, lba + k, 1, s) < 0) return -1;
        while (i + 33 < PSPEXP_DISC_SECTOR) {
            uint32_t rl = s[i], fl;
            const char *fn;
            size_t j;
            if (!rl) break;   /* (records never cross a sector: the rest is padding) */
            if (i + rl > PSPEXP_DISC_SECTOR) break;
            fl = s[i + 32];
            fn = (const char *)s + i + 33;
            for (j = 0; j < fl && fn[j] != ';'; j++) ;
            if (j == nl && !strncasecmp(fn, name, nl)) {
                *elba = le32(s + i + 2);
                *esize = le32(s + i + 10);
                *isdir = (s[i + 25] & 2) != 0;
                return 0;
            }
            i += rl;
        }
    }
    return -1;
}

int pspexp_disc_find(PspExpDisc *d, const char *path, uint32_t *lba, uint32_t *size)
{
    uint8_t s[PSPEXP_DISC_SECTOR];
    uint32_t cl, cs;
    int isdir = 1;
    if (pspexp_disc_read(d, 16, 1, s) < 0) return -1;
    cl = le32(s + 156 + 2);   /* the root directory's record */
    cs = le32(s + 156 + 10);
    while (*path == '/') path++;
    while (*path) {
        const char *e = strchr(path, '/');
        size_t nl = e ? (size_t)(e - path) : strlen(path);
        if (!isdir || dir_find(d, cl, cs, path, nl, &cl, &cs, &isdir) < 0) return -1;
        path += nl;
        while (*path == '/') path++;
    }
    *lba = cl;
    *size = cs;
    return 0;
}

uint8_t *pspexp_disc_load(PspExpDisc *d, const char *path, uint32_t max, uint32_t *size)
{
    uint32_t lba, n, sectors;
    uint8_t *buf;
    if (pspexp_disc_find(d, path, &lba, &n) < 0 || n > max) return NULL;
    sectors = (n + PSPEXP_DISC_SECTOR - 1) / PSPEXP_DISC_SECTOR;
    if (!(buf = (uint8_t *)malloc((size_t)sectors * PSPEXP_DISC_SECTOR + 1))) return NULL;
    if (sectors && pspexp_disc_read(d, lba, sectors, buf) < 0) { free(buf); return NULL; }
    if (size) *size = n;
    return buf;
}

int pspexp_sfo_get(const uint8_t *sfo, size_t n, const char *key, char *out, size_t outn)
{
    uint32_t keys, data, count, i;
    if (n < 20 || memcmp(sfo, "\0PSF", 4) != 0 || !outn) return -1;
    keys = le32(sfo + 8); data = le32(sfo + 12); count = le32(sfo + 16);
    for (i = 0; i < count && 20 + i * 16 + 16 <= n; i++) {
        const uint8_t *e = sfo + 20 + i * 16;
        uint32_t ko = keys + le16(e), fmt = le16(e + 2), len = le32(e + 4), at = data + le32(e + 12);
        if (ko >= n || strncmp((const char *)sfo + ko, key, n - ko) != 0 || at + len > n) continue;
        if (fmt == 0x0404 && len >= 4) snprintf(out, outn, "%u", le32(sfo + at));
        else {
            size_t k = len < outn - 1 ? len : outn - 1;
            memcpy(out, sfo + at, k);
            out[k] = 0;
        }
        return 0;
    }
    return -1;
}

int pspexp_disc_info(const char *path, PspExpDiscInfo *info, const char *icon_png, char *err, size_t errn)
{
    PspExpDisc *d = pspexp_disc_open(path, err, errn);
    uint8_t *sfo, *icon;
    uint32_t n;
    memset(info, 0, sizeof(*info));
    if (!d) return -1;
    snprintf(info->kind, sizeof(info->kind), "%s", pspexp_disc_kind(d));
    info->size = pspexp_disc_size(d);
    if (!(sfo = pspexp_disc_load(d, "PSP_GAME/PARAM.SFO", 64 * 1024, &n))) {
        pspexp_disc_close(d);
        return set_err(err, errn, "no PSP_GAME/PARAM.SFO: not a PSP game disc");
    }
    pspexp_sfo_get(sfo, n, "TITLE", info->title, sizeof(info->title));
    pspexp_sfo_get(sfo, n, "DISC_ID", info->disc_id, sizeof(info->disc_id));
    free(sfo);
    if (icon_png && (icon = pspexp_disc_load(d, "PSP_GAME/ICON0.PNG", 512 * 1024, &n))) {
        char tmp[1100];
        FILE *f;
        snprintf(tmp, sizeof(tmp), "%s.tmp", icon_png);
        if ((f = fopen(tmp, "wb"))) {
            int ok = fwrite(icon, 1, n, f) == n;
            ok = fclose(f) == 0 && ok;
            if (ok) rename(tmp, icon_png); else remove(tmp);
        }
        free(icon);
    }
    pspexp_disc_close(d);
    return 0;
}
