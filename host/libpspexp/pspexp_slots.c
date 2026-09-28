/* The PSP's runtime slots, from the host: see pspexp_slots.h */
#include "pspexp_slots.h"
#include "../../protocol/pspexp_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_buf[PSPEXP_FS_CHUNK];

/* a small file of the stick into buf (NUL-terminated); its length or < 0 */
static int read_small(PspExpContext *ctx, const char *path, char *buf, int max)
{
    int n = pspexp_fs_read(ctx, path, 0, buf, (uint32_t)(max - 1));
    if (n < 0) return n;
    buf[n] = 0;
    return n;
}

static char slot_of(const char *text)
{
    const char *s = strstr(text, "slot=");
    return s && (s[5] == 'a' || s[5] == 'b') && strstr(text, "tries=") ? s[5] : 0;
}

int pspexp_slots_read(PspExpContext *ctx, PspExpSlots *out)
{
    PspExpFsEntry e[PSPEXP_FS_LIST_MAX], st;
    char path[320], text[512];
    int n, i, more = 0, k;
    memset(out, 0, sizeof(*out));
    n = pspexp_fs_list(ctx, "ms0:/PSP/GAME", 0, e, PSPEXP_FS_LIST_MAX, &more);
    for (i = 0; i < n && !out->dir[0]; i++) {
        if (!(e[i].flags & PSPEXP_FS_DIR)) continue;
        snprintf(path, sizeof(path), "ms0:/PSP/GAME/%s/active", e[i].name);
        if (pspexp_fs_stat(ctx, path, &st) == 0) snprintf(out->dir, sizeof(out->dir), "ms0:/PSP/GAME/%s", e[i].name);
    }
    if (!out->dir[0]) return -1;
    snprintf(path, sizeof(path), "%s/active", out->dir);
    if (read_small(ctx, path, text, sizeof(text)) > 0) out->current = slot_of(text);
    snprintf(path, sizeof(path), "%s/active.new", out->dir);
    if (read_small(ctx, path, text, sizeof(text)) > 0) out->pending = slot_of(text);
    for (k = 0; k < 2; k++) {
        char *v;
        snprintf(path, sizeof(path), "%s/slot-%c/VERSION", out->dir, 'a' + k);
        if (read_small(ctx, path, text, sizeof(text)) > 0 && (v = strstr(text, "version="))) {
            v += 8;
            snprintf(out->version[k], sizeof(out->version[k]), "%.*s", (int)strcspn(v, "\r\n"), v);
        }
    }
    snprintf(path, sizeof(path), "%s/fallback", out->dir);
    if (read_small(ctx, path, text, sizeof(text)) > 0) snprintf(out->fallback, sizeof(out->fallback), "%.*s", (int)strcspn(text, "\r\n"), text);
    return 0;
}

static void say(PspExpSlotLog log, void *u, const char *fmt, const char *a, const char *b)
{
    char m[400];
    if (!log) return;
    snprintf(m, sizeof(m), fmt, a, b);
    log(u, m);
}

/* write a local file to the stick; stop after the first chunk when partial */
static int put_file(PspExpContext *ctx, const char *local, const char *remote, int partial)
{
    FILE *f = fopen(local, "rb");
    uint64_t off = 0;
    size_t n;
    int r = 0;
    if (!f) return -1;
    do {
        n = fread(g_buf, 1, sizeof(g_buf), f);
        if ((r = pspexp_fs_write(ctx, remote, off, g_buf, (uint32_t)n, off ? 0 : PSPEXP_FS_CREATE | PSPEXP_FS_TRUNCATE)) < 0) break;
        off += n;
        if (partial) break;
    } while (n == sizeof(g_buf));
    fclose(f);
    return r < 0 ? -1 : 0;
}

/* the stick's copy has this size and SHA-256 */
static int check_file(PspExpContext *ctx, const char *remote, unsigned size, const char *hex)
{
    PspExpSha256 h;
    uint8_t d[32];
    char got[65];
    uint64_t off = 0;
    int n;
    pspexp_sha256_init(&h);
    while ((n = pspexp_fs_read(ctx, remote, off, g_buf, sizeof(g_buf))) > 0) { pspexp_sha256_update(&h, g_buf, (size_t)n); off += (uint64_t)n; }
    if (n < 0 || off != size) return -1;
    pspexp_sha256_final(&h, d);
    pspexp_sha256_hex(d, got);
    return strcmp(got, hex) ? -1 : 0;
}

#define CRASH(point) do { if (crash && !strcmp(crash, point)) { say(log, u, "test: stopping dead after %s%s", point, ""); return -2; } } while (0)
#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); return -1; } while (0)

int pspexp_slots_update(PspExpContext *ctx, const char *srcdir, const char *crash,
                        PspExpSlotLog log, void *u, char *err, size_t errlen)
{
    PspExpSlots s;
    char path[512], local[768], version[4096], vread[4096], tgt, cur;
    char names[8][64], hexes[8][65];
    unsigned sizes[8];
    int nfiles = 0, i, vl;
    FILE *f;
    err[0] = 0;
    if (!pspexp_fs_supported(ctx)) FAIL("the PSP does not share its memory stick");
    if (pspexp_slots_read(ctx, &s) < 0) FAIL("the PSP's PSPExp has no slots yet: its boot loader must be replaced once, by hand (docs/Package_Manager.md §6.1)");
    cur = s.pending ? s.pending : s.current ? s.current : 'a';
    tgt = cur == 'a' ? 'b' : 'a';
    /* the set to install: its VERSION names every file with size and checksum */
    snprintf(local, sizeof(local), "%s/VERSION", srcdir);
    if (!(f = fopen(local, "rb"))) FAIL("no %s", local);
    vl = (int)fread(version, 1, sizeof(version) - 1, f);
    fclose(f);
    version[vl > 0 ? vl : 0] = 0;
    {
        char *line = version, *next;
        char copy[4096];
        snprintf(copy, sizeof(copy), "%s", version);
        for (line = copy; line && *line && nfiles < 8; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (strncmp(line, "file=", 5)) continue;
            if (sscanf(line, "file=%63s size=%u sha256=%64s", names[nfiles], &sizes[nfiles], hexes[nfiles]) == 3 && !strchr(names[nfiles], '/')) nfiles++;
        }
    }
    if (!nfiles) FAIL("%s lists no files", local);
    if (!strstr(version, "\nend\n")) FAIL("%s has no end line: not a complete runtime set", local);
    say(log, u, "installing into slot %s (the PSP runs slot %s)", tgt == 'a' ? "a" : "b", cur == 'a' ? "a" : "b");
    /* 1. invalidate first: an older release's VERSION never vouches for half-written files */
    snprintf(path, sizeof(path), "%s/slot-%c/VERSION", s.dir, tgt);
    if (pspexp_fs_remove(ctx, path) < 0 && pspexp_fs_stat(ctx, path, (PspExpFsEntry[1]){{0}}) == 0) FAIL("cannot remove %s", path);
    snprintf(path, sizeof(path), "%s/slot-%c", s.dir, tgt);
    pspexp_fs_mkdir(ctx, path);
    CRASH("invalidated");
    /* 2. copy, 3. read every file back and check it */
    for (i = 0; i < nfiles; i++) {
        snprintf(local, sizeof(local), "%s/%s", srcdir, names[i]);
        snprintf(path, sizeof(path), "%s/slot-%c/%s", s.dir, tgt, names[i]);
        if (put_file(ctx, local, path, crash && !strcmp(crash, "copy-partial")) < 0) FAIL("cannot copy %s", names[i]);
        CRASH("copy-partial");
        if (check_file(ctx, path, sizes[i], hexes[i]) < 0) FAIL("%s on the stick does not match its checksum", names[i]);
        say(log, u, "%s copied and checked", names[i], "");
    }
    CRASH("copied");
    /* 4. VERSION last, as the slot's "complete" mark, read back too */
    snprintf(path, sizeof(path), "%s/slot-%c/VERSION", s.dir, tgt);
    if (pspexp_fs_write(ctx, path, 0, version, (uint32_t)(crash && !strcmp(crash, "version-partial") ? vl / 2 : vl), PSPEXP_FS_CREATE | PSPEXP_FS_TRUNCATE) < 0) FAIL("cannot write VERSION");
    CRASH("version-partial");
    if (read_small(ctx, path, vread, sizeof(vread)) != vl || memcmp(vread, version, (size_t)vl)) FAIL("VERSION on the stick does not read back");
    CRASH("version");
    /* 5. the switch: active.new, written completely, is the one switch point */
    {
        char sw[32];
        int sl = snprintf(sw, sizeof(sw), "slot=%c\ntries=0\n", tgt);
        snprintf(path, sizeof(path), "%s/active.new", s.dir);
        if (pspexp_fs_write(ctx, path, 0, sw, (uint32_t)(crash && !strcmp(crash, "switch-partial") ? 4 : sl), PSPEXP_FS_CREATE | PSPEXP_FS_TRUNCATE) < 0) FAIL("cannot write active.new");
        CRASH("switch-partial");
        if (read_small(ctx, path, vread, sizeof(vread)) != sl || memcmp(vread, sw, (size_t)sl)) FAIL("active.new does not read back");
    }
    say(log, u, "slot %s is ready: it runs at the next start of PSPExp", tgt == 'a' ? "a" : "b", "");
    return 0;
}
