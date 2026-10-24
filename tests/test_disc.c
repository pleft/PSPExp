/* PSP game discs on the board (host/libpspexp/pspexp_disc.c): a small disc made here (PSP_GAME
 * with PARAM.SFO and ICON0.PNG), read as an ISO, a CSO and a zip (stored and deflated). */
#include "test_util.h"
#include "../host/libpspexp/pspexp_disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#define SECTORS 64
static uint8_t iso[SECTORS * 2048];

static void put32b(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; p[4] = v >> 24; p[5] = v >> 16; p[6] = v >> 8; p[7] = v; }   /* both-endian */
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *p, uint32_t v) { p16(p, (uint16_t)v); p16(p + 2, (uint16_t)(v >> 16)); }

/* a directory record at p: its length */
static int rec(uint8_t *p, const char *name, uint32_t lba, uint32_t size, int dir)
{
    int nl = (int)strlen(name), l = 33 + nl + !(nl & 1);
    memset(p, 0, (size_t)l);
    p[0] = (uint8_t)l;
    put32b(p + 2, lba);
    put32b(p + 10, size);
    p[25] = dir ? 2 : 0;
    p[32] = (uint8_t)nl;
    memcpy(p + 33, name, (size_t)nl);
    return l;
}

static uint32_t make_sfo(uint8_t *s)
{
    /* two keys: DISC_ID, TITLE (utf8 strings) */
    const char *k[2] = { "DISC_ID", "TITLE" }, *v[2] = { "ULES00151", "Test Racer \xc3\xa9" };
    uint32_t keys = 20 + 2 * 16, data = keys + 16, ko = 0, dof = 0, i;
    memset(s, 0, 256);
    memcpy(s, "\0PSF", 4); p32(s + 4, 0x101); p32(s + 8, keys); p32(s + 12, data); p32(s + 16, 2);
    for (i = 0; i < 2; i++) {
        uint8_t *e = s + 20 + i * 16;
        uint32_t vl = (uint32_t)strlen(v[i]) + 1;
        p16(e, (uint16_t)ko); p16(e + 2, 0x0204); p32(e + 4, vl); p32(e + 8, 32); p32(e + 12, dof);
        strcpy((char *)s + keys + ko, k[i]); ko += (uint32_t)strlen(k[i]) + 1;
        memcpy(s + data + dof, v[i], vl); dof += 32;
    }
    return data + dof;
}

static void make_iso(void)
{
    uint8_t *pvd = iso + 16 * 2048, *root = iso + 20 * 2048, *game = iso + 21 * 2048;
    uint32_t sfo_n, i;
    int o;
    for (i = 0; i < sizeof(iso); i++) iso[i] = (uint8_t)(i * 7 + i / 2048);   /* (sectors that differ) */
    memset(pvd, 0, 2048); pvd[0] = 1; memcpy(pvd + 1, "CD001", 5); pvd[6] = 1;
    rec(pvd + 156, "\0", 20, 2048, 1);
    memset(root, 0, 2048);
    o = rec(root, "\0", 20, 2048, 1); o += rec(root + o, "\1", 20, 2048, 1);
    o += rec(root + o, "UMD_DATA.BIN", 30, 16, 0);
    rec(root + o, "PSP_GAME", 21, 2048, 1);
    sfo_n = make_sfo(iso + 22 * 2048);
    memset(iso + 23 * 2048, 0, 2048); memcpy(iso + 23 * 2048, "\x89PNG-test-icon", 14);
    memset(game, 0, 2048);
    o = rec(game, "\0", 21, 2048, 1); o += rec(game + o, "\1", 20, 2048, 1);
    o += rec(game + o, "ICON0.PNG;1", 23, 14, 0);
    rec(game + o, "PARAM.SFO;1", 22, sfo_n, 0);
}

static void write_file(const char *p, const void *d, size_t n) { FILE *f = fopen(p, "wb"); fwrite(d, 1, n, f); fclose(f); }

/* raw deflate of n bytes into out; its length */
static size_t deflate_raw(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    z_stream z;
    memset(&z, 0, sizeof(z));
    deflateInit2(&z, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    z.next_in = (uint8_t *)in; z.avail_in = (uInt)n; z.next_out = out; z.avail_out = (uInt)cap;
    deflate(&z, Z_FINISH);
    n = z.total_out;
    deflateEnd(&z);
    return n;
}

static void make_zip(const char *p, int method)
{
    static uint8_t body[sizeof(iso) * 2];
    uint8_t lh[30], cd[46], eo[22];
    const char *name = "Test Racer (Europe).iso";
    uint16_t nl = (uint16_t)strlen(name);
    size_t cn = method ? deflate_raw(iso, sizeof(iso), body, sizeof(body)) : sizeof(iso);
    uint32_t crc = (uint32_t)crc32(0, iso, sizeof(iso));
    FILE *f = fopen(p, "wb");
    if (!method) memcpy(body, iso, sizeof(iso));
    memset(lh, 0, 30); p32(lh, 0x04034b50); p16(lh + 8, (uint16_t)method); p32(lh + 14, crc); p32(lh + 18, (uint32_t)cn); p32(lh + 22, sizeof(iso)); p16(lh + 26, nl);
    fwrite(lh, 1, 30, f); fwrite(name, 1, nl, f); fwrite(body, 1, cn, f);
    memset(cd, 0, 46); p32(cd, 0x02014b50); p16(cd + 10, (uint16_t)method); p32(cd + 16, crc); p32(cd + 20, (uint32_t)cn); p32(cd + 24, sizeof(iso)); p16(cd + 28, nl); p32(cd + 42, 0);
    fwrite(cd, 1, 46, f); fwrite(name, 1, nl, f);
    memset(eo, 0, 22); p32(eo, 0x06054b50); p16(eo + 8, 1); p16(eo + 10, 1); p32(eo + 12, 46u + nl); p32(eo + 16, 30u + nl + (uint32_t)cn);
    fwrite(eo, 1, 22, f);
    fclose(f);
}

static void make_cso(const char *p)
{
    static uint8_t out[sizeof(iso) * 2];
    uint32_t idx[SECTORS + 1], at = 24 + 4 * (SECTORS + 1), i;
    uint8_t h[24];
    FILE *f = fopen(p, "wb");
    memset(h, 0, 24); memcpy(h, "CISO", 4); p32(h + 4, 24); p32(h + 8, sizeof(iso)); p32(h + 16, 2048); h[20] = 1; h[21] = 0;
    fwrite(h, 1, 24, f);
    fseek(f, (long)at, SEEK_SET);
    for (i = 0; i < SECTORS; i++) {
        size_t n = deflate_raw(iso + i * 2048, 2048, out, sizeof(out));
        idx[i] = at;
        if (i % 5 == 0 || n >= 2048) { fwrite(iso + i * 2048, 1, 2048, f); idx[i] |= 0x80000000u; at += 2048; }   /* (some stored as they are) */
        else { fwrite(out, 1, n, f); at += (uint32_t)n; }
    }
    idx[SECTORS] = at;
    fseek(f, 24, SEEK_SET);
    for (i = 0; i <= SECTORS; i++) { uint8_t b[4]; p32(b, idx[i]); fwrite(b, 1, 4, f); }
    fclose(f);
}

static void check(const char *path, const char *kind)
{
    PspExpDiscInfo in;
    char err[200] = "", icon[600];
    uint8_t s[3 * 2048];
    PspExpDisc *d;
    FILE *f;
    snprintf(icon, sizeof(icon), "%s.icon.png", path);
    CHECK_EQ(pspexp_disc_info(path, &in, icon, err, sizeof(err)), 0);
    if (err[0]) fprintf(stderr, "%s: %s\n", path, err);
    CHECK(!strcmp(in.kind, kind));
    CHECK(!strcmp(in.title, "Test Racer \xc3\xa9"));
    CHECK(!strcmp(in.disc_id, "ULES00151"));
    CHECK_EQ(in.size, sizeof(iso));
    f = fopen(icon, "rb");
    CHECK(f != NULL);
    if (f) { char b[20] = ""; CHECK_EQ(fread(b, 1, 20, f), 14); CHECK(!memcmp(b, "\x89PNG-test-icon", 14)); fclose(f); }
    d = pspexp_disc_open(path, err, sizeof(err));
    CHECK(d != NULL);
    if (!d) return;
    CHECK_EQ(pspexp_disc_read(d, 40, 3, s), 0);   /* forward, then back (a zip starts again) */
    CHECK(!memcmp(s, iso + 40 * 2048, sizeof(s)));
    CHECK_EQ(pspexp_disc_read(d, 2, 3, s), 0);
    CHECK(!memcmp(s, iso + 2 * 2048, sizeof(s)));
    CHECK_EQ(pspexp_disc_read(d, SECTORS - 1, 2, s), -1);   /* past the end */
    pspexp_disc_close(d);
    unlink(icon);
}

int main(void)
{
    char dir[] = "/tmp/pspexp-test-disc-XXXXXX", p[4][300];
    char err[200];
    CHECK(mkdtemp(dir) != NULL);
    make_iso();
    snprintf(p[0], sizeof(p[0]), "%s/game.iso", dir); write_file(p[0], iso, sizeof(iso));
    snprintf(p[1], sizeof(p[1]), "%s/game.cso", dir); make_cso(p[1]);
    snprintf(p[2], sizeof(p[2]), "%s/stored.zip", dir); make_zip(p[2], 0);
    snprintf(p[3], sizeof(p[3]), "%s/deflated.zip", dir); make_zip(p[3], 8);
    check(p[0], "iso"); check(p[1], "cso"); check(p[2], "zip"); check(p[3], "zip");
    {   /* not a disc: refused with a reason */
        char q[300];
        snprintf(q, sizeof(q), "%s/notes.txt", dir); write_file(q, "hello, this is not a disc image at all...", 41);
        CHECK(pspexp_disc_open(q, err, sizeof(err)) == NULL);
        CHECK(err[0] != 0);
        unlink(q);
    }
    for (int i = 0; i < 4; i++) unlink(p[i]);
    rmdir(dir);
    return test_summary("disc");
}
