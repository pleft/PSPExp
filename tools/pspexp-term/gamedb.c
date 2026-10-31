/* A ROM's facts from OpenVGDB: see gamedb.h */
#include "gamedb.h"
#include "sqlite/sqlite3.h"
#include "../../host/libpspexp/pspexp_disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#define DB_URL "https://github.com/OpenVGDB/OpenVGDB/releases/download/v29.0/openvgdb.zip"
#define RETRY_S (24 * 3600)   /* a failed download: tried again a day later */

static pid_t g_dl_pid, g_look_pid;

/* our systems and OpenVGDB's (SYSTEMS.systemShortName); not there (Dreamcast): no facts */
static const struct { const char *ours, *theirs; } SYS[] = {
    { "nes", "NES" }, { "snes", "SNES" }, { "md", "MD" }, { "sms", "SMS" }, { "gg", "GG" }, { "gb", "GB" },
    { "gbc", "GBC" }, { "gba", "GBA" }, { "lynx", "Lynx" }, { "ws", "WonderSwan" }, { "wsc", "WonderSwan Color" },
    { "n64", "N64" }, { "nds", "NDS" }, { "saturn", "Saturn" }, { "psp", "PSP" }, { "pce", "PCE" }, { "32x", "32X" },
    { "segacd", "SCD" }, { "psx", "PSX" }, { "ngp", "NGP" }, { "ngpc", "NGPC" }, { "vb", "VB" }, { "atari2600", "2600" },
    { "atari7800", "7800" }, { "fds", "FDS" }, { "sg1000", "SG1000" }, { "msx", "MSX" }, { "coleco", "ColecoVision" },
};

static void cache_dir(char *out, size_t n)
{
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/.cache/pspexp-launcher", home ? home : "/tmp");
}

static void mkdirs(const char *path)
{
    char p[1024];
    size_t i;
    snprintf(p, sizeof(p), "%s", path);
    for (i = 1; p[i]; i++) if (p[i] == '/') { p[i] = 0; mkdir(p, 0755); p[i] = '/'; }
    mkdir(p, 0755);
}

/* the facts file for path (and its ".none"): facts/<system>/<file name without its ending>.txt */
static void facts_paths(const char *path, const char *sys, char *txt, size_t n, char *none, size_t m)
{
    char dir[600], stem[300], *dot;
    const char *base = strrchr(path, '/');
    size_t i;
    snprintf(stem, sizeof(stem), "%s", base ? base + 1 : path);
    if ((dot = strrchr(stem, '.')) && dot != stem) *dot = 0;
    for (i = 0; stem[i]; i++) if (strchr("/\\:*?\"<>|`&", stem[i])) stem[i] = '_';
    cache_dir(dir, sizeof(dir));
    snprintf(txt, n, "%s/facts/%s/%s.txt", dir, sys, stem);
    snprintf(none, m, "%s/facts/%s/%s.none", dir, sys, stem);
}

/* ---- the lookup (in the child) ---------------------------------------------------------------- */

static uint32_t crc_table[256];
static void crc_init(void)
{
    uint32_t i, k, c;
    for (i = 0; i < 256; i++) { for (c = i, k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_table[i] = c; }
}
static uint32_t crc_add(uint32_t crc, const unsigned char *p, size_t n)
{
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}

static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* a zip's first entry's CRC, from its central directory (nothing unpacked); 0 when not a zip */
static int zip_crc(FILE *f, uint32_t *crc)
{
    unsigned char b[65536 + 22];
    long size, from;
    size_t got, i;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 22) return 0;
    from = size > (long)sizeof(b) ? size - (long)sizeof(b) : 0;
    fseek(f, from, SEEK_SET);
    got = fread(b, 1, sizeof(b), f);
    for (i = got - 22 + 1; i-- > 0;)
        if (b[i] == 'P' && b[i + 1] == 'K' && b[i + 2] == 5 && b[i + 3] == 6) {   /* the end of the central directory */
            unsigned char c[46];
            fseek(f, (long)le32(b + i + 16), SEEK_SET);
            if (fread(c, 1, 46, f) != 46 || memcmp(c, "PK\1\2", 4)) return 0;
            *crc = le32(c + 16);
            return 1;
        }
    return 0;
}

/* the ROM's CRC as OpenVGDB keeps it: the data without an NES (16 bytes), Lynx (64) or a copier's
 * SNES header (512); a zip's from its directory. 0 when it cannot be read */
static int rom_crc(const char *path, uint32_t *out)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf, head[4];
    size_t n, skip = 0;
    long size;
    uint32_t crc = 0;
    const char *ext = strrchr(path, '.');
    if (!f) return 0;
    if (ext && !strcasecmp(ext, ".zip")) { int ok = zip_crc(f, out); fclose(f); return ok; }
    fseek(f, 0, SEEK_END); size = ftell(f); rewind(f);
    if (size <= 0 || size > (512L << 20) || fread(head, 1, 4, f) != 4) { fclose(f); return 0; }
    if (!memcmp(head, "NES\x1a", 4)) skip = 16;
    else if (!memcmp(head, "LYNX", 4)) skip = 64;
    else if (ext && (!strcasecmp(ext, ".smc") || !strcasecmp(ext, ".sfc")) && size % 1024 == 512) skip = 512;
    fseek(f, (long)skip, SEEK_SET);
    if (!(buf = (unsigned char *)malloc(1 << 16))) { fclose(f); return 0; }
    crc_init();
    while ((n = fread(buf, 1, 1 << 16, f)) > 0) crc = crc_add(crc, buf, n);
    free(buf);
    fclose(f);
    *out = crc;
    return 1;
}

/* a title as words, lower case, letters and digits only, "the" left out, what is in brackets left out:
 * "Legend of Zelda, The - A Link to the Past (E) [!]" and "The Legend of Zelda: A Link to the Past" match */
static void norm(const char *s, char *out, size_t n)
{
    size_t o = 0;
    int depth = 0;
    while (*s && o + 1 < n) {
        char w[64];
        size_t k = 0;
        if (*s == '(' || *s == '[') { depth++; s++; continue; }
        if (*s == ')' || *s == ']') { if (depth) depth--; s++; continue; }
        if (depth || !isalnum((unsigned char)*s)) { s++; continue; }
        while (isalnum((unsigned char)*s) && k + 1 < sizeof(w)) w[k++] = (char)tolower((unsigned char)*s++);
        w[k] = 0;
        if (!strcmp(w, "the")) continue;
        for (k = 0; w[k] && o + 1 < n; k++) out[o++] = w[k];
    }
    out[o] = 0;
}

static void col(sqlite3_stmt *st, int i, char *out, size_t n)
{
    const unsigned char *t = sqlite3_column_text(st, i);
    snprintf(out, n, "%s", t ? (const char *)t : "");
}

/* the release's facts into out; 1 when it has a description (the best kind of match) */
static int take(sqlite3_stmt *st, GameFacts *out)
{
    char date[64];
    size_t i;
    col(st, 0, out->desc, sizeof(out->desc));
    col(st, 1, out->developer, sizeof(out->developer));
    col(st, 2, out->publisher, sizeof(out->publisher));
    col(st, 3, out->genre, sizeof(out->genre));
    col(st, 4, date, sizeof(date));
    out->year[0] = 0;
    for (i = 0; date[i]; i++)   /* "Aug 26, 2001", "1992", "Sep9, 2001": the year */
        if (isdigit((unsigned char)date[i]) && isdigit((unsigned char)date[i + 1]) && isdigit((unsigned char)date[i + 2]) && isdigit((unsigned char)date[i + 3])
            && (date[i] == '1' || date[i] == '2') && !isdigit((unsigned char)date[i + 4])) { snprintf(out->year, sizeof(out->year), "%.4s", date + i); break; }
    for (i = 0; out->desc[i]; i++) if (out->desc[i] == '\n' || out->desc[i] == '\r') out->desc[i] = ' ';
    return out->desc[0] != 0;
}

#define FACTS "SELECT r.releaseDescription, r.releaseDeveloper, r.releasePublisher, r.releaseGenre, r.releaseDate, " \
              "r.releaseTitleName, m.romExtensionlessFileName FROM ROMs m JOIN RELEASES r ON r.romID = m.romID " \
              "JOIN SYSTEMS s ON s.systemID = m.systemID WHERE s.systemShortName = ?1 "

static int lookup(const char *db_path, const char *path, const char *sys, const char *title, GameFacts *out)
{
    sqlite3 *db;
    sqlite3_stmt *st;
    const char *theirs = NULL;
    char key[32], want[200];
    uint32_t crc;
    int found = 0, k;
    for (k = 0; k < (int)(sizeof(SYS) / sizeof(SYS[0])); k++) if (!strcmp(SYS[k].ours, sys)) theirs = SYS[k].theirs;
    if (!theirs || sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) return 0;
    memset(out, 0, sizeof(*out));
    key[0] = 0;
    if (!strcmp(sys, "psp")) {   /* its disc's serial: ULES00151 as ULES-00151 */
        PspExpDiscInfo in;
        char err[160];
        if (pspexp_disc_info(path, &in, NULL, err, sizeof(err)) == 0 && strlen(in.disc_id) > 4) snprintf(key, sizeof(key), "%.4s-%s", in.disc_id, in.disc_id + 4);
        if (key[0] && sqlite3_prepare_v2(db, FACTS "AND m.romSerial = ?2 ORDER BY r.releaseDescription IS NULL LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, theirs, -1, SQLITE_STATIC); sqlite3_bind_text(st, 2, key, -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) found = take(st, out) || out->developer[0];
            sqlite3_finalize(st);
        }
    } else if (rom_crc(path, &crc)) {
        snprintf(key, sizeof(key), "%08X", (unsigned)crc);
        if (sqlite3_prepare_v2(db, FACTS "AND m.romHashCRC = ?2 ORDER BY r.releaseDescription IS NULL LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, theirs, -1, SQLITE_STATIC); sqlite3_bind_text(st, 2, key, -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) found = take(st, out) || out->developer[0];
            sqlite3_finalize(st);
        }
    }
    if (!found && sqlite3_prepare_v2(db, FACTS, -1, &st, NULL) == SQLITE_OK) {   /* by name: a hack, a translation, a zipped NES ROM */
        GameFacts any;
        int have_any = 0;
        norm(title, want, sizeof(want));
        sqlite3_bind_text(st, 1, theirs, -1, SQLITE_STATIC);
        while (want[0] && sqlite3_step(st) == SQLITE_ROW) {
            char a[300], b[300], na[200], nb[200];
            col(st, 5, a, sizeof(a)); col(st, 6, b, sizeof(b));
            norm(a, na, sizeof(na)); norm(b, nb, sizeof(nb));
            if (strcmp(na, want) && strcmp(nb, want)) continue;
            if (take(st, out)) { found = 1; break; }   /* (one with a description first) */
            if (!have_any) { any = *out; have_any = 1; }
        }
        if (!found && have_any) { *out = any; found = 1; }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return found;
}

static void facts_write(const char *txt, const GameFacts *g)
{
    char part[1100];
    FILE *f;
    snprintf(part, sizeof(part), "%s.part", txt);
    if (!(f = fopen(part, "w"))) return;
    fprintf(f, "developer=%s\npublisher=%s\ngenre=%s\nyear=%s\ndesc=%s\n", g->developer, g->publisher, g->genre, g->year, g->desc);
    fclose(f);
    rename(part, txt);
}

static int facts_read(const char *txt, GameFacts *g)
{
    char line[1400];
    FILE *f = fopen(txt, "r");
    if (!f) return 0;
    memset(g, 0, sizeof(*g));
    while (fgets(line, sizeof(line), f)) {
        char *v = strchr(line, '=');
        size_t l;
        if (!v) continue;
        *v++ = 0;
        l = strlen(v);
        while (l && (v[l - 1] == '\n' || v[l - 1] == '\r')) v[--l] = 0;
        if (!strcmp(line, "developer")) snprintf(g->developer, sizeof(g->developer), "%s", v);
        else if (!strcmp(line, "publisher")) snprintf(g->publisher, sizeof(g->publisher), "%s", v);
        else if (!strcmp(line, "genre")) snprintf(g->genre, sizeof(g->genre), "%s", v);
        else if (!strcmp(line, "year")) snprintf(g->year, sizeof(g->year), "%s", v);
        else if (!strcmp(line, "desc")) snprintf(g->desc, sizeof(g->desc), "%s", v);
    }
    fclose(f);
    return 1;
}

/* ---- the database's download ----------------------------------------------------------------- */

static void db_paths(char *db, size_t n, char *failed, size_t m)
{
    char dir[600];
    cache_dir(dir, sizeof(dir));
    snprintf(db, n, "%s/openvgdb.sqlite", dir);
    snprintf(failed, m, "%s/openvgdb.failed", dir);
}

static void download_start(void)
{
    char dir[600];
    pid_t pid;
    cache_dir(dir, sizeof(dir));
    mkdirs(dir);
    pid = fork();
    if (pid == 0) {
        setenv("D", dir, 1); setenv("U", DB_URL, 1);
        /* curl, then the one file out of the zip (bsdtar: libarchive-tools, a dependency of pspexp-core) */
        execl("/bin/sh", "sh", "-c",
              "cd \"$D\" && curl -fsSL --max-time 600 -o openvgdb.zip.part \"$U\" && bsdtar -xOf openvgdb.zip.part openvgdb.sqlite > openvgdb.sqlite.part "
              "&& [ -s openvgdb.sqlite.part ] && mv openvgdb.sqlite.part openvgdb.sqlite && rm -f openvgdb.zip.part openvgdb.failed "
              "|| { rm -f openvgdb.zip.part openvgdb.sqlite.part; touch openvgdb.failed; }",
              (char *)NULL);
        _exit(127);
    }
    if (pid > 0) g_dl_pid = pid;
}

int gamedb_facts(const char *path, const char *sys, const char *title, int allow, GameFacts *out)
{
    char db[700], failed[700], txt[1000], none[1000];
    struct stat st;
    pid_t pid;
    facts_paths(path, sys, txt, sizeof(txt), none, sizeof(none));
    if (facts_read(txt, out)) return 1;
    if (stat(none, &st) == 0) return -1;
    db_paths(db, sizeof(db), failed, sizeof(failed));
    if (stat(db, &st) != 0) {   /* the database first: once (a failure tried again a day later) */
        if (!allow) return 0;
        if (!g_dl_pid && (stat(failed, &st) != 0 || time(NULL) - st.st_mtime > RETRY_S)) download_start();
        return g_dl_pid ? 0 : -1;
    }
    if (g_look_pid) return 0;   /* (one at a time: asked again when it ends) */
    {
        char dir[1000], *sl;
        snprintf(dir, sizeof(dir), "%s", txt);
        if ((sl = strrchr(dir, '/'))) { *sl = 0; mkdirs(dir); }
    }
    pid = fork();
    if (pid == 0) {
        GameFacts g;
        if (lookup(db, path, sys, title, &g)) facts_write(txt, &g);
        else { FILE *f = fopen(none, "w"); if (f) fclose(f); }
        _exit(0);
    }
    if (pid > 0) g_look_pid = pid;
    return 0;
}

int gamedb_poll(void)
{
    int done = 0;
    if (g_dl_pid > 0 && waitpid(g_dl_pid, NULL, WNOHANG) == g_dl_pid) { g_dl_pid = 0; done = 1; }
    if (g_look_pid > 0 && waitpid(g_look_pid, NULL, WNOHANG) == g_look_pid) { g_look_pid = 0; done = 1; }
    return done;
}

int gamedb_busy(void) { return g_dl_pid ? 1 : g_look_pid ? 2 : 0; }
