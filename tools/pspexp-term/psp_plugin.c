/* The PSPExp plugin on the PSP's stick: see psp_plugin.h */
#include "psp_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_TXT_MAX 8192   /* (PRO's game.txt: a line a plugin) */

static uint8_t g_buf[PSPEXP_FS_CHUNK];

/* a line (without its end) names the plugin */
static int names_plugin(const char *p, size_t len)
{
    size_t k = strlen(PSP_PLUGIN_FILE), i;
    for (i = 0; i + k <= len; i++) if (!memcmp(p + i, PSP_PLUGIN_FILE, k)) return 1;
    return 0;
}

/* a line's length without its end ("\n", "\r\n") or trailing blanks */
static size_t body_len(const char *p, size_t len)
{
    while (len && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
    return len;
}

int psp_plugin_listed(const char *txt)
{
    const char *p = txt;
    while (*p) {
        const char *e = strchr(p, '\n'), *end = e ? e + 1 : p + strlen(p);
        size_t l = body_len(p, (size_t)(end - p));
        if (names_plugin(p, l) && l >= 2 && p[l - 1] == '1' && (p[l - 2] == ' ' || p[l - 2] == '\t')) return 1;
        p = end;
    }
    return 0;
}

static int put(char *out, size_t n, size_t *o, const char *s, size_t len)
{
    if (*o + len + 1 > n) return -1;
    memcpy(out + *o, s, len);
    *o += len;
    out[*o] = 0;
    return 0;
}

int psp_plugin_game_txt(const char *in, const char *path, int on, char *out, size_t n)
{
    const char *eol = strstr(in, "\r\n") ? "\r\n" : "\n", *p = in;
    const char *flag = on ? " 1" : " 0";
    size_t o = 0;
    int found = 0;
    if (!n) return -1;
    out[0] = 0;
    while (*p) {   /* every line naming the plugin set (an old one left on would still load it) */
        const char *e = strchr(p, '\n'), *end = e ? e + 1 : p + strlen(p);
        size_t len = (size_t)(end - p), l = body_len(p, len), tok = 0, k = l;
        if (names_plugin(p, l)) {
            while (tok < l && p[tok] != ' ' && p[tok] != '\t') tok++;
            while (k < len && p[k] != '\r' && p[k] != '\n') k++;   /* (its own line end kept) */
            if (put(out, n, &o, p, tok) < 0 || put(out, n, &o, flag, 2) < 0 || put(out, n, &o, p + k, len - k) < 0) return -1;
            found = 1;
        } else if (put(out, n, &o, p, len) < 0) return -1;
        p = end;
    }
    if (found) return (int)o;
    if (!on) return 0;   /* (no line: it is off already) */
    if (o && out[o - 1] != '\n' && put(out, n, &o, eol, strlen(eol)) < 0) return -1;
    if (put(out, n, &o, path, strlen(path)) < 0 || put(out, n, &o, " 1", 2) < 0 || put(out, n, &o, eol, strlen(eol)) < 0) return -1;
    return (int)o;
}

static void plugin_path(char *out, size_t n, const char *install, const char *suffix)
{
    snprintf(out, n, "%s/plugin/%s%s", install, PSP_PLUGIN_FILE, suffix);
}

/* game.txt from the stick (none: empty): its length, or < 0 */
static int game_txt_read(PspExpContext *ctx, char *t)
{
    int r = pspexp_fs_read(ctx, PSP_PLUGIN_GAME_TXT, 0, t, GAME_TXT_MAX - 1);
    if (r == PSPEXP_FSERR_NOT_FOUND) r = 0;
    if (r >= 0) t[r] = 0;
    return r;
}

int psp_plugin_state(PspExpContext *ctx, const char *install)
{
    static char t[GAME_TXT_MAX];
    PspExpFsEntry e;
    char p[400];
    int r;
    plugin_path(p, sizeof(p), install, "");
    if ((r = pspexp_fs_stat(ctx, p, &e)) == PSPEXP_FSERR_NOT_FOUND) return PSP_PLUGIN_MISSING;
    if (r < 0 || game_txt_read(ctx, t) < 0) return PSP_PLUGIN_UNKNOWN;
    return psp_plugin_listed(t) ? PSP_PLUGIN_ON : PSP_PLUGIN_OFF;
}

/* the board's plugin, read whole (it is small): its bytes (malloc'd) and length, or NULL */
static uint8_t *board_plugin(const char *board_dir, size_t *len)
{
    char p[600];
    FILE *f;
    uint8_t *b = NULL;
    long n;
    snprintf(p, sizeof(p), "%s/plugin/%s", board_dir, PSP_PLUGIN_FILE);
    if (!(f = fopen(p, "rb"))) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && n < (8l << 20) && fseek(f, 0, SEEK_SET) == 0 && (b = malloc((size_t)n))
        && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

/* the stick's file at PATH holds exactly these bytes: 1, 0 not, < 0 unreadable */
static int same_on_stick(PspExpContext *ctx, const char *path, const uint8_t *b, size_t len)
{
    PspExpFsEntry e;
    uint64_t off = 0;
    int r = pspexp_fs_stat(ctx, path, &e);
    if (r == PSPEXP_FSERR_NOT_FOUND) return 0;
    if (r < 0) return r;
    if (((uint64_t)e.size_hi << 32 | e.size_lo) != len) return 0;
    while (off < len) {
        uint32_t want = len - off > PSPEXP_FS_CHUNK ? PSPEXP_FS_CHUNK : (uint32_t)(len - off);
        if ((r = pspexp_fs_read(ctx, path, off, g_buf, want)) < 0) return r;
        if (r == 0 || memcmp(g_buf, b + off, (size_t)r)) return 0;
        off += (uint64_t)r;
    }
    return 1;
}

int psp_plugin_outdated(PspExpContext *ctx, const char *board_dir, const char *install)
{
    size_t len;
    uint8_t *b = board_plugin(board_dir, &len);
    char p[400];
    int r;
    if (!b) return 0;
    plugin_path(p, sizeof(p), install, "");
    r = same_on_stick(ctx, p, b, len);
    free(b);
    return r < 0 ? r : !r;
}

/* NEW into place as PATH (the old one gone first: the stick's rename does not replace) */
static int replace(PspExpContext *ctx, const char *path, const char *new_path)
{
    int r = pspexp_fs_remove(ctx, path);
    if (r < 0 && r != PSPEXP_FSERR_NOT_FOUND) return r;
    return pspexp_fs_rename(ctx, new_path, path);
}

int psp_plugin_install(PspExpContext *ctx, const char *board_dir, const char *install, char *err, size_t n)
{
    size_t len, off = 0;
    uint8_t *b = board_plugin(board_dir, &len);
    char dir[400], p[400], pn[400];
    int r = 0;
    if (!b) { snprintf(err, n, "this board has no PSPExp plugin (pspexp-runtime)"); return -1; }
    snprintf(dir, sizeof(dir), "%s/plugin", install);
    plugin_path(p, sizeof(p), install, "");
    plugin_path(pn, sizeof(pn), install, ".new");
    if ((r = pspexp_fs_mkdir(ctx, dir)) < 0 && r != PSPEXP_FSERR_EXISTS) { snprintf(err, n, "no folder %s (%d)", dir, r); free(b); return -1; }
    while (off < len) {
        uint32_t k = len - off > PSPEXP_FS_CHUNK ? PSPEXP_FS_CHUNK : (uint32_t)(len - off);
        if ((r = pspexp_fs_write(ctx, pn, off, b + off, k, off ? 0 : PSPEXP_FS_CREATE | PSPEXP_FS_TRUNCATE)) < 0) break;
        off += k;
    }
    if (r >= 0 && same_on_stick(ctx, pn, b, len) != 1) r = -1;
    free(b);
    if (r < 0) { pspexp_fs_remove(ctx, pn); snprintf(err, n, "writing %s did not check out", pn); return -1; }
    if ((r = replace(ctx, p, pn)) < 0) { snprintf(err, n, "%s not put in place (%d)", p, r); return -1; }
    return 0;
}

int psp_plugin_switch(PspExpContext *ctx, const char *install, int on, char *err, size_t n)
{
    static char t[GAME_TXT_MAX], out[GAME_TXT_MAX + 512], back[GAME_TXT_MAX + 512];
    char p[400];
    int len, had, r;
    if ((had = game_txt_read(ctx, t)) < 0) { snprintf(err, n, "%s could not be read", PSP_PLUGIN_GAME_TXT); return -1; }
    plugin_path(p, sizeof(p), install, "");
    if ((len = psp_plugin_game_txt(t, p, on, out, sizeof(out))) < 0) { snprintf(err, n, "%s is too long", PSP_PLUGIN_GAME_TXT); return -1; }
    if (len == 0) return 0;
    if ((r = pspexp_fs_mkdir(ctx, "ms0:/seplugins")) < 0 && r != PSPEXP_FSERR_EXISTS) { snprintf(err, n, "no folder ms0:/seplugins (%d)", r); return -1; }
    /* one write, read back (not a .new renamed in: the old one removed first, a failed rename would
     * lose the other plugins' lines); the old text put back if it did not check out */
    if ((r = pspexp_fs_write(ctx, PSP_PLUGIN_GAME_TXT, 0, out, (uint32_t)len, PSPEXP_FS_CREATE | PSPEXP_FS_TRUNCATE)) < 0
        || pspexp_fs_read(ctx, PSP_PLUGIN_GAME_TXT, 0, back, sizeof(back)) != len || memcmp(back, out, (size_t)len)) {
        if (had > 0) pspexp_fs_write(ctx, PSP_PLUGIN_GAME_TXT, 0, t, (uint32_t)had, PSPEXP_FS_CREATE | PSPEXP_FS_TRUNCATE);
        snprintf(err, n, "writing %s did not check out", PSP_PLUGIN_GAME_TXT);
        return -1;
    }
    return 0;
}
