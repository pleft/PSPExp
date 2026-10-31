/* A ROM's facts from the LaunchBox Games Database: see gamedb.h */
#include "gamedb.h"
#include "sqlite/sqlite3.h"
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
#include <signal.h>

#define LB_URL    "https://gamesdb.launchbox-app.com/Metadata.zip"
#define RETRY_S   600         /* a failed download: tried again 10 minutes later */

static pid_t g_lb_pid, g_look_pid;

/* our systems and LaunchBox's (<Platform>) */
static const struct { const char *ours, *lb; } SYS[] = {
    { "nes", "Nintendo Entertainment System" }, { "snes", "Super Nintendo Entertainment System" }, { "md", "Sega Genesis" },
    { "sms", "Sega Master System" }, { "gg", "Sega Game Gear" }, { "gb", "Nintendo Game Boy" }, { "gbc", "Nintendo Game Boy Color" },
    { "gba", "Nintendo Game Boy Advance" }, { "lynx", "Atari Lynx" }, { "ws", "WonderSwan" }, { "wsc", "WonderSwan Color" },
    { "n64", "Nintendo 64" }, { "nds", "Nintendo DS" }, { "saturn", "Sega Saturn" }, { "dreamcast", "Sega Dreamcast" },
    { "psp", "Sony PSP" }, { "pce", "NEC TurboGrafx-16" }, { "32x", "Sega 32X" }, { "segacd", "Sega CD" }, { "psx", "Sony Playstation" },
    { "ngp", "SNK Neo Geo Pocket" }, { "ngpc", "SNK Neo Geo Pocket Color" }, { "vb", "Nintendo Virtual Boy" }, { "atari2600", "Atari 2600" },
    { "atari7800", "Atari 7800" }, { "fds", "Nintendo Famicom Disk System" }, { "sg1000", "Sega SG-1000" }, { "msx", "Microsoft MSX" },
    { "coleco", "ColecoVision" },
};
#define NSYS ((int)(sizeof(SYS) / sizeof(SYS[0])))

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

/* the databases and their failure marks */
static void db_path(const char *name, char *out, size_t n) { char d[600]; cache_dir(d, sizeof(d)); snprintf(out, n, "%s/%s", d, name); }

/* the ROM's file name without its ending, as a facts file's name */
static void rom_stem(const char *path, char *stem, size_t n)
{
    const char *base = strrchr(path, '/');
    char *dot;
    snprintf(stem, n, "%s", base ? base + 1 : path);
    if ((dot = strrchr(stem, '.')) && dot != stem) *dot = 0;
}

/* the facts file for path (and its ".none"): facts/<system>/<file name without its ending>.txt */
static void facts_paths(const char *path, const char *sys, char *txt, size_t n, char *none, size_t m)
{
    char dir[600], stem[300];
    size_t i;
    rom_stem(path, stem, sizeof(stem));
    for (i = 0; stem[i]; i++) if (strchr("/\\:*?\"<>|`&", stem[i])) stem[i] = '_';
    cache_dir(dir, sizeof(dir));
    snprintf(txt, n, "%s/facts/%s/%s.txt", dir, sys, stem);
    snprintf(none, m, "%s/facts/%s/%s.none", dir, sys, stem);
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

/* "Aug 26, 2001", "1992-03-01", "Sep9, 2001": its year */
static void year_of(const char *date, char *out, size_t n)
{
    size_t i;
    out[0] = 0;
    for (i = 0; date[i]; i++)
        if ((date[i] == '1' || date[i] == '2') && isdigit((unsigned char)date[i + 1]) && isdigit((unsigned char)date[i + 2]) && isdigit((unsigned char)date[i + 3])
            && !isdigit((unsigned char)date[i + 4]) && (i == 0 || !isdigit((unsigned char)date[i - 1]))) { snprintf(out, n, "%.4s", date + i); return; }
}

/* ---- the LaunchBox Games Database, made compact on the board (in a child) ---------------------
 * Its Metadata.zip streamed (curl | bsdtar: never stored whole), Metadata.xml's <Game>s and
 * <GameAlternateName>s and Files.xml's <File>s kept for our systems only, into launchbox.sqlite:
 * games (its id, platform, name, overview, developer, publisher, year, genres, players), names (a
 * game's name and its other names, as norm() makes them) and files (a ROM's file name, No-Intro's or
 * Redump's, to the game's). The XML's elements are one a line; an overview may run over lines. */

static void unescape(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '&') {
            if (!strncmp(s, "&amp;", 5)) { *o++ = '&'; s += 5; continue; }
            if (!strncmp(s, "&lt;", 4)) { *o++ = '<'; s += 4; continue; }
            if (!strncmp(s, "&gt;", 4)) { *o++ = '>'; s += 4; continue; }
            if (!strncmp(s, "&quot;", 6)) { *o++ = '"'; s += 6; continue; }
            if (!strncmp(s, "&apos;", 6)) { *o++ = '\''; s += 6; continue; }
        }
        *o++ = *s++;
    }
    *o = 0;
}

static int lb_platform_kept(const char *p)
{
    int k;
    for (k = 0; k < NSYS; k++) if (!strcmp(SYS[k].lb, p)) return 1;
    return 0;
}

typedef struct LbRec { char name[300], platform[96], developer[160], publisher[160], date[40], genres[200], players[16], id[24];
                       char alt[300], file[300], game[300]; char *overview; size_t olen, ocap; } LbRec;

static void rec_reset(LbRec *r) { char *o = r->overview; size_t c = r->ocap; memset(r, 0, sizeof(*r)); r->overview = o; r->ocap = c; if (o) o[0] = 0; }

static void over_add(LbRec *r, const char *s)
{
    size_t l = strlen(s);
    if (r->olen + l + 2 > r->ocap) { r->ocap = (r->olen + l + 2) * 2; r->overview = (char *)realloc(r->overview, r->ocap); }
    if (r->olen) r->overview[r->olen++] = '\n';   /* (its paragraphs kept apart) */
    memcpy(r->overview + r->olen, s, l + 1);
    r->olen += l;
}

/* "<Tag>value</Tag>": the value into out; 1 when the line is that tag */
static int field(const char *line, const char *tag, char *out, size_t n)
{
    char open[40], close[40];
    const char *v, *e;
    snprintf(open, sizeof(open), "<%s>", tag);
    if (strncmp(line, open, strlen(open))) return 0;
    v = line + strlen(open);
    snprintf(close, sizeof(close), "</%s>", tag);
    e = strstr(v, close);
    snprintf(out, n, "%.*s", e ? (int)(e - v) : (int)strlen(v), v);
    unescape(out);
    return 1;
}

static int lb_build(FILE *in, const char *out_path)
{
    sqlite3 *db;
    sqlite3_stmt *ins_game, *ins_name, *ins_alt, *ins_file;
    char line[8192], val[2048], nn[300];
    LbRec r;
    enum { NONE, GAME, ALT, FILE_ } mode = NONE;
    int in_over = 0, games = 0;
    unlink(out_path);
    if (sqlite3_open(out_path, &db) != SQLITE_OK) return -1;
    sqlite3_exec(db, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; BEGIN;"
                     "CREATE TABLE games(id INTEGER PRIMARY KEY, platform TEXT, name TEXT, overview TEXT, developer TEXT, publisher TEXT, year TEXT, genres TEXT, players TEXT);"
                     "CREATE TABLE names(platform TEXT, norm TEXT, id INTEGER);"
                     "CREATE TABLE files(platform TEXT, file TEXT, game TEXT);", NULL, NULL, NULL);
    sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO games VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)", -1, &ins_game, NULL);
    sqlite3_prepare_v2(db, "INSERT INTO names VALUES(?1,?2,?3)", -1, &ins_name, NULL);
    sqlite3_prepare_v2(db, "INSERT INTO names SELECT platform, ?1, id FROM games WHERE id = ?2", -1, &ins_alt, NULL);
    sqlite3_prepare_v2(db, "INSERT INTO files VALUES(?1,?2,?3)", -1, &ins_file, NULL);
    memset(&r, 0, sizeof(r));
    while (fgets(line, sizeof(line), in)) {
        char *s = line, *e;
        size_t l = strlen(s);
        while (l && (s[l - 1] == '\n' || s[l - 1] == '\r')) s[--l] = 0;
        if (in_over) {   /* an overview's next lines */
            if ((e = strstr(s, "</Overview>"))) { *e = 0; in_over = 0; }
            unescape(s); over_add(&r, s);
            continue;
        }
        while (*s == ' ' || *s == '\t') s++;
        if (!strcmp(s, "<Game>")) { mode = GAME; rec_reset(&r); continue; }
        if (!strcmp(s, "<GameAlternateName>")) { mode = ALT; rec_reset(&r); continue; }
        if (!strcmp(s, "<File>")) { mode = FILE_; rec_reset(&r); continue; }
        if (!strcmp(s, "</Game>")) {
            if (mode == GAME && r.id[0] && lb_platform_kept(r.platform)) {
                char year[8];
                if (!strncmp(r.date, "<", 1)) r.date[0] = 0;
                year_of(r.date, year, sizeof(year));
                sqlite3_bind_int64(ins_game, 1, atoll(r.id)); sqlite3_bind_text(ins_game, 2, r.platform, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins_game, 3, r.name, -1, SQLITE_STATIC); sqlite3_bind_text(ins_game, 4, r.overview ? r.overview : "", -1, SQLITE_STATIC);
                sqlite3_bind_text(ins_game, 5, r.developer, -1, SQLITE_STATIC); sqlite3_bind_text(ins_game, 6, r.publisher, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins_game, 7, year, -1, SQLITE_STATIC); sqlite3_bind_text(ins_game, 8, r.genres, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins_game, 9, r.players, -1, SQLITE_STATIC);
                sqlite3_step(ins_game); sqlite3_reset(ins_game);
                norm(r.name, nn, sizeof(nn));
                sqlite3_bind_text(ins_name, 1, r.platform, -1, SQLITE_STATIC); sqlite3_bind_text(ins_name, 2, nn, -1, SQLITE_STATIC);
                sqlite3_bind_int64(ins_name, 3, atoll(r.id));
                sqlite3_step(ins_name); sqlite3_reset(ins_name);
                games++;
            }
            mode = NONE; continue;
        }
        if (!strcmp(s, "</GameAlternateName>")) {
            if (mode == ALT && r.alt[0] && r.id[0]) {
                norm(r.alt, nn, sizeof(nn));
                sqlite3_bind_text(ins_alt, 1, nn, -1, SQLITE_STATIC); sqlite3_bind_int64(ins_alt, 2, atoll(r.id));
                sqlite3_step(ins_alt); sqlite3_reset(ins_alt);
            }
            mode = NONE; continue;
        }
        if (!strcmp(s, "</File>")) {
            if (mode == FILE_ && r.file[0] && lb_platform_kept(r.platform)) {
                char nf[300];
                norm(r.file, nf, sizeof(nf)); norm(r.game, nn, sizeof(nn));
                if (nf[0] && nn[0]) {   /* (a file whose name says only the game's: nothing to add) */
                    sqlite3_bind_text(ins_file, 1, r.platform, -1, SQLITE_STATIC); sqlite3_bind_text(ins_file, 2, r.file, -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins_file, 3, nn, -1, SQLITE_STATIC);
                    sqlite3_step(ins_file); sqlite3_reset(ins_file);
                }
            }
            mode = NONE; continue;
        }
        if (mode == NONE) continue;
        if (!strncmp(s, "<Overview>", 10) && mode == GAME) {
            s += 10;
            if ((e = strstr(s, "</Overview>"))) *e = 0; else in_over = 1;
            unescape(s); over_add(&r, s);
            continue;
        }
        if (field(s, "Name", r.name, sizeof(r.name)) || field(s, "Platform", r.platform, sizeof(r.platform)) ||
            field(s, "Developer", r.developer, sizeof(r.developer)) || field(s, "Publisher", r.publisher, sizeof(r.publisher)) ||
            field(s, "Genres", r.genres, sizeof(r.genres)) || field(s, "MaxPlayers", r.players, sizeof(r.players)) ||
            field(s, "DatabaseID", r.id, sizeof(r.id)) || field(s, "AlternateName", r.alt, sizeof(r.alt)) ||
            field(s, "FileName", r.file, sizeof(r.file)) || field(s, "GameName", r.game, sizeof(r.game))) continue;
        if (field(s, "ReleaseDate", val, sizeof(val)) || field(s, "ReleaseYear", val, sizeof(val))) { if (!r.date[0]) snprintf(r.date, sizeof(r.date), "%s", val); continue; }
    }
    free(r.overview);
    sqlite3_finalize(ins_game); sqlite3_finalize(ins_name); sqlite3_finalize(ins_alt); sqlite3_finalize(ins_file);
    sqlite3_exec(db, "CREATE INDEX names_i ON names(platform, norm); CREATE INDEX files_i ON files(platform, file); COMMIT;", NULL, NULL, NULL);
    sqlite3_close(db);
    return games > 1000 ? 0 : -1;   /* (a cut-off download: not kept) */
}

static void col(sqlite3_stmt *st, int i, char *out, size_t n)
{
    const unsigned char *t = sqlite3_column_text(st, i);
    snprintf(out, n, "%s", t ? (const char *)t : "");
}

/* the game in LaunchBox's copy, by a name as norm() makes it */
static int lb_by_norm(sqlite3 *db, const char *platform, const char *nn, GameFacts *out)
{
    sqlite3_stmt *st;
    int found = 0;
    if (!nn[0] || sqlite3_prepare_v2(db, "SELECT g.overview, g.developer, g.publisher, g.genres, g.year, g.players FROM names n JOIN games g ON g.id = n.id "
                                         "WHERE n.platform = ?1 AND n.norm = ?2 ORDER BY g.overview = '' LIMIT 1", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, platform, -1, SQLITE_STATIC); sqlite3_bind_text(st, 2, nn, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        col(st, 0, out->desc, sizeof(out->desc)); col(st, 1, out->developer, sizeof(out->developer));
        col(st, 2, out->publisher, sizeof(out->publisher)); col(st, 3, out->genre, sizeof(out->genre));
        col(st, 4, out->year, sizeof(out->year)); col(st, 5, out->players, sizeof(out->players));
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

/* LaunchBox: by the ROM's title (its names and other names: "Pokemon - Red Version (UE) [S][!]" is
 * its "Pokemon Red Version"), else by its file name (No-Intro's, Redump's, in Files.xml) */
static int lb_lookup(sqlite3 *db, const char *platform, const char *path, const char *title, GameFacts *out)
{
    char nn[300], stem[300];
    sqlite3_stmt *st;
    norm(title, nn, sizeof(nn));
    if (lb_by_norm(db, platform, nn, out)) return 1;
    rom_stem(path, stem, sizeof(stem));
    if (sqlite3_prepare_v2(db, "SELECT game FROM files WHERE platform = ?1 AND file = ?2 LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
        int found = 0;
        sqlite3_bind_text(st, 1, platform, -1, SQLITE_STATIC); sqlite3_bind_text(st, 2, stem, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) { col(st, 0, nn, sizeof(nn)); found = lb_by_norm(db, platform, nn, out); }
        sqlite3_finalize(st);
        if (found) return 1;
    }
    return 0;
}

/* the facts of one ROM (with_lb: the database was there) */
static int lookup(const char *path, const char *sys, const char *title, int *with_lb, GameFacts *out)
{
    char lb_path[700];
    const char *lb_sys = NULL;
    sqlite3 *db;
    struct stat st;
    int found = 0, k;
    for (k = 0; k < NSYS; k++) if (!strcmp(SYS[k].ours, sys)) lb_sys = SYS[k].lb;
    memset(out, 0, sizeof(*out));
    db_path("launchbox.sqlite", lb_path, sizeof(lb_path));
    *with_lb = stat(lb_path, &st) == 0;
    if (lb_sys && *with_lb && sqlite3_open_v2(lb_path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
        found = lb_lookup(db, lb_sys, path, title, out);
        sqlite3_close(db);
    }
    return found;
}

static void facts_write(const char *txt, const GameFacts *g, int with_lb)
{
    char part[1100];
    const char *c;
    FILE *f;
    snprintf(part, sizeof(part), "%s.part", txt);
    if (!(f = fopen(part, "w"))) return;
    fprintf(f, "lb=%d\ndeveloper=%s\npublisher=%s\ngenre=%s\nyear=%s\nplayers=%s\ndesc=", with_lb, g->developer, g->publisher, g->genre, g->year, g->players);
    for (c = g->desc; *c; c++) { if (*c == '\n') fputs("\\n", f); else if (*c != '\r') fputc(*c, f); }   /* (one line: its breaks as \n) */
    fputc('\n', f);
    fclose(f);
    rename(part, txt);
}

/* 1 read (with_lb: LaunchBox was there when it was looked up) */
static int facts_read(const char *txt, GameFacts *g, int *with_lb)
{
    static char line[13000];
    FILE *f = fopen(txt, "r");
    if (!f) return 0;
    memset(g, 0, sizeof(*g));
    *with_lb = 0;
    while (fgets(line, sizeof(line), f)) {
        char *v = strchr(line, '=');
        size_t l;
        if (!v) continue;
        *v++ = 0;
        l = strlen(v);
        while (l && (v[l - 1] == '\n' || v[l - 1] == '\r')) v[--l] = 0;
        if (!strcmp(line, "lb")) *with_lb = atoi(v);
        else if (!strcmp(line, "developer")) snprintf(g->developer, sizeof(g->developer), "%s", v);
        else if (!strcmp(line, "publisher")) snprintf(g->publisher, sizeof(g->publisher), "%s", v);
        else if (!strcmp(line, "genre")) snprintf(g->genre, sizeof(g->genre), "%s", v);
        else if (!strcmp(line, "year")) snprintf(g->year, sizeof(g->year), "%s", v);
        else if (!strcmp(line, "players")) snprintf(g->players, sizeof(g->players), "%s", v);
        else if (!strcmp(line, "desc")) {   /* (its breaks back from \n) */
            size_t o = 0;
            for (; *v && o + 1 < sizeof(g->desc); v++) {
                if (v[0] == '\\' && v[1] == 'n') { g->desc[o++] = '\n'; v++; }
                else g->desc[o++] = *v;
            }
            g->desc[o] = 0;
        }
    }
    fclose(f);
    return 1;
}

/* ---- the downloads (each in a child, once; a failure tried again 10 minutes later) ----------------- */

static int may_try(const char *failed)
{
    struct stat st;
    return stat(failed, &st) != 0 || time(NULL) - st.st_mtime > RETRY_S;
}

static void lb_start(void)
{
    char dir[600];
    pid_t pid;
    cache_dir(dir, sizeof(dir));
    mkdirs(dir);
    pid = fork();
    if (pid == 0) {   /* the zip streamed through bsdtar into the builder: never kept whole */
        char cmd[400], part[700], db[700], failed[700];
        /* a stop (the console's restart, a package's upgrade) ends it as it is: no failure marked, the
         * next console starting it again (it had the console's SIGTERM handling, and marked a cut-off
         * download as failed: 2026-10-31) */
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGCHLD, SIG_DFL);
        FILE *in;
        int ok;
        const char *url = getenv("PSPEXP_LAUNCHBOX_URL");   /* (tests: a copy, file:///...) */
        snprintf(cmd, sizeof(cmd), "curl -fsSL --retry 3 --retry-delay 5 --max-time 3600 '%s' | bsdtar -xOf - Metadata.xml Files.xml", url && *url ? url : LB_URL);
        snprintf(part, sizeof(part), "%s/launchbox.sqlite.part", dir);
        snprintf(db, sizeof(db), "%s/launchbox.sqlite", dir);
        snprintf(failed, sizeof(failed), "%s/launchbox.failed", dir);
        in = popen(cmd, "r");
        ok = in && lb_build(in, part) == 0;
        {
            int st = in ? pclose(in) : -1;
            if (st != 0) ok = 0;
            fprintf(stderr, "[gamedb] LaunchBox Games Database: %s (download and unpacking %s)\n", ok ? "made" : "not made", st == 0 ? "fine" : "failed");
        }
        if (ok && rename(part, db) == 0) unlink(failed);
        else { FILE *f; unlink(part); if ((f = fopen(failed, "w"))) fclose(f); }
        _exit(ok ? 0 : 1);
    }
    if (pid > 0) g_lb_pid = pid;
}

int gamedb_facts(const char *path, const char *sys, const char *title, int allow, GameFacts *out)
{
    char lb[700], lb_failed[700], txt[1000], none[1000];
    struct stat st, sn;
    int with_lb = 0, have_lb;
    pid_t pid;
    db_path("launchbox.sqlite", lb, sizeof(lb)); db_path("launchbox.failed", lb_failed, sizeof(lb_failed));
    have_lb = stat(lb, &st) == 0;
    facts_paths(path, sys, txt, sizeof(txt), none, sizeof(none));
    if (facts_read(txt, out, &with_lb) && with_lb) return 1;
    if (!have_lb) {   /* the database first: once (a failure tried again a day later) */
        if (allow && !g_lb_pid && may_try(lb_failed)) lb_start();
        return g_lb_pid ? 0 : -1;
    }
    if (stat(none, &sn) == 0 && sn.st_mtime >= st.st_mtime) return -1;   /* (not found in this copy) */
    if (g_look_pid) return 0;   /* (one at a time: asked again when it ends) */
    {
        char dir[1000], *sl;
        snprintf(dir, sizeof(dir), "%s", txt);
        if ((sl = strrchr(dir, '/'))) { *sl = 0; mkdirs(dir); }
    }
    pid = fork();
    if (pid == 0) {
        GameFacts g;
        int wl = 0;
        if (lookup(path, sys, title, &wl, &g)) { facts_write(txt, &g, wl); unlink(none); }
        else { FILE *f = fopen(none, "w"); if (f) fclose(f); unlink(txt); }
        _exit(0);
    }
    if (pid > 0) g_look_pid = pid;
    return 0;
}

int gamedb_poll(void)
{
    int done = 0;
    if (g_lb_pid > 0 && waitpid(g_lb_pid, NULL, WNOHANG) == g_lb_pid) { g_lb_pid = 0; done = 1; }
    if (g_look_pid > 0 && waitpid(g_look_pid, NULL, WNOHANG) == g_look_pid) { g_look_pid = 0; done = 1; }
    return done;
}

int gamedb_busy(void) { return g_lb_pid ? 1 : g_look_pid ? 2 : 0; }
