/*
 * The package manager (Apps > Packages): what this board's repositories offer
 * next to what is installed, and installs, updates and removals through
 * pspexp-pkg, in the background.
 *
 * On the Debian boards it shows the signed recipe catalogue through
 * pspexp-pkg (docs/Package_Manager.md): `pspexp-pkg available` for the list,
 * `install` / `remove` / `update` / `recover` for the work, apt's progress
 * read from its status lines. Sections: Games, Emulators, Apps (L / R).
 *
 * Elsewhere (the Omega2+) it reads folder repositories, as before:
 * Repositories: $PSPEXP_ROOT/repo (a folder on the board; the Mac fills it
 * with `pspexp-pkg-build publish`, a card image can carry one), and one more
 * per line of ~/.config/pspexp-launcher/sources: a folder, or an http(s)
 * address whose <board>/ holds the same files (fetched with curl or wget).
 * A repository is an `index` (id|version|bytes|file|title|description per
 * line), the packages, and <id>.png covers.
 *
 *   up / down   a package      cross     install / update (reinstall when current)
 *   triangle    remove (asks)  start     read the repositories again
 *   circle      close
 */
#include "appview.h"
#include "launcher_gfx.h"
#include "launcher_ui.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define PKG_MAX 128
#define SRC_MAX 8
#define STORE_LOG "/tmp/pspexp-store.log"

typedef struct Pkg {
    char id[48], title[96], desc[200];
    char avail[40], inst[40];      /* versions: offered, installed ("" none) */
    long long bytes;
    char file[700];                /* the package: a path, or an address to fetch */
    char cover[700];               /* its picture: a path ("" none) */
    int remote;                    /* file is an address */
    char cat[12];                  /* catalogue: game | system | app | tool */
    char st[12];                   /* catalogue: installed | update | new | broken | local */
} Pkg;

static Pkg g_pk[PKG_MAX];
static int g_n, g_sel, g_top, g_dirty = 1, g_exit;
static int g_nsrc, g_nremote;
static char g_err[200];            /* why a repository could not be read */
enum { S_LIST, S_CONFIRM, S_WORKING, S_RESULT };
static int g_mode;
static pid_t g_pid;                /* the background job */
static char g_job[160], g_result_title[96], g_result[400];
static int g_job_install;
static int g_catmode;              /* the signed catalogue and pspexp-pkg drive it */
#define CATALOGUE "/usr/share/pspexp/catalogue"
enum { TAB_GAMES, TAB_EMULATORS, TAB_APPS, NTABS };
static int g_tab;
static int g_vis[PKG_MAX], g_nvis;  /* the packages of the section shown (g_sel indexes this) */
static int g_prog;                 /* the job's progress, percent (-1 unknown) */
static char g_phase[160];          /* and what it is doing */

static const char *root_dir(void) { const char *r = getenv("PSPEXP_ROOT"); return r && *r ? r : "/opt/pspexp"; }

/* the kind of board, for a repository on the web: its builds differ */
static const char *board_kind(void)
{
    static char k[16];
    struct utsname u;
    if (k[0]) return k;
    if (uname(&u) != 0) return "gameshell";
    if (!strncmp(u.machine, "mips", 4)) snprintf(k, sizeof(k), "omega");
    else if (!strcmp(u.machine, "aarch64")) snprintf(k, sizeof(k), "pi");
    else snprintf(k, sizeof(k), "gameshell");
    return k;
}

static const char *cache_dir(void)
{
    static char d[600];
    const char *h = getenv("HOME");
    snprintf(d, sizeof(d), "%s/.cache/pspexp-store", h && *h ? h : "/tmp");
    return d;
}

/* "1.10" > "1.9"; "20260301" > "20250101"; a git hash: equal or not */
static int vercmp(const char *a, const char *b)
{
    while (*a || *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            unsigned long long x = strtoull(a, (char **)&a, 10), y = strtoull(b, (char **)&b, 10);
            if (x != y) return x < y ? -1 : 1;
        } else {
            if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
            a++; b++;
        }
    }
    return 0;
}

static Pkg *find(const char *id)
{
    int i;
    for (i = 0; i < g_n; i++) if (!strcmp(g_pk[i].id, id)) return &g_pk[i];
    if (g_n >= PKG_MAX) return NULL;
    memset(&g_pk[g_n], 0, sizeof(Pkg));
    snprintf(g_pk[g_n].id, sizeof(g_pk[g_n].id), "%s", id);
    return &g_pk[g_n++];
}

/* one repository's index: dir holds it (a fetched one is cached there); base is where its files are */
static void read_index(const char *dir, const char *base, int remote)
{
    char path[700], line[1024];
    FILE *f;
    snprintf(path, sizeof(path), "%s/index", dir);
    if (!(f = fopen(path, "r"))) return;
    g_nsrc++;
    while (fgets(line, sizeof(line), f)) {
        char *fld[6], *p = line;
        int k;
        Pkg *pk;
        line[strcspn(line, "\r\n")] = 0;
        for (k = 0; k < 6; k++) { fld[k] = p; p = p ? strchr(p, '|') : NULL; if (p) *p++ = 0; }
        if (!fld[0][0] || !fld[1] || !fld[3] || !(pk = find(fld[0]))) continue;
        if (pk->avail[0] && vercmp(fld[1], pk->avail) <= 0) continue;   /* another repository has it newer */
        snprintf(pk->avail, sizeof(pk->avail), "%s", fld[1]);
        pk->bytes = atoll(fld[2]);
        snprintf(pk->file, sizeof(pk->file), "%s/%s", base, fld[3]);
        pk->remote = remote;
        if (fld[4]) snprintf(pk->title, sizeof(pk->title), "%s", fld[4]);
        if (fld[5]) snprintf(pk->desc, sizeof(pk->desc), "%s", fld[5]);
        snprintf(path, sizeof(path), "%s/%s.png", dir, fld[0]);
        if (access(path, R_OK) == 0) snprintf(pk->cover, sizeof(pk->cover), "%s", path);
    }
    fclose(f);
}

/* the installed packages: <root>/apps/<id>/package */
static void read_installed(void)
{
    char path[700], line[400];
    FILE *pl = NULL;
    snprintf(path, sizeof(path), "ls -1 '%s/apps' 2>/dev/null", root_dir());
    if (!(pl = popen(path, "r"))) return;
    while (fgets(line, sizeof(line), pl)) {
        char id[64], pf[700], kv[400];
        FILE *f;
        Pkg *pk;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '.') continue;
        snprintf(id, sizeof(id), "%s", line);
        snprintf(pf, sizeof(pf), "%s/apps/%s/package", root_dir(), id);
        if (!(f = fopen(pf, "r"))) continue;
        if (!(pk = find(id))) { fclose(f); continue; }
        while (fgets(kv, sizeof(kv), f)) {
            kv[strcspn(kv, "\r\n")] = 0;
            if (!strncmp(kv, "version=", 8)) snprintf(pk->inst, sizeof(pk->inst), "%s", kv + 8);
            else if (!strncmp(kv, "title=", 6) && !pk->title[0]) snprintf(pk->title, sizeof(pk->title), "%s", kv + 6);
            else if (!strncmp(kv, "description=", 12) && !pk->desc[0]) snprintf(pk->desc, sizeof(pk->desc), "%s", kv + 12);
        }
        if (!pk->inst[0]) snprintf(pk->inst, sizeof(pk->inst), "?");
        fclose(f);
    }
    pclose(pl);
}

static int cmp_pkg(const void *a, const void *b) { return strcasecmp(((const Pkg *)a)->title, ((const Pkg *)b)->title); }

static int tab_of(const Pkg *p)
{
    if (!strcmp(p->cat, "game")) return TAB_GAMES;
    if (!strcmp(p->cat, "system")) return TAB_EMULATORS;
    return TAB_APPS;
}

static void filter(void)
{
    int i, keep = g_nvis && g_sel < g_nvis ? g_vis[g_sel] : -1;
    g_nvis = 0;
    for (i = 0; i < g_n; i++) if (!g_catmode || tab_of(&g_pk[i]) == g_tab) g_vis[g_nvis++] = i;
    g_sel = 0;
    for (i = 0; i < g_nvis; i++) if (g_vis[i] == keep) g_sel = i;
    g_top = 0;
    g_dirty = 1;
}

/* the catalogue's recipes with their state (pspexp-pkg available), and installed
 * titles it does not have (older archive installs: removable only) */
static void load_catalogue(void)
{
    char line[1024], keep[48] = "";
    FILE *f;
    int i;
    if (g_nvis && g_sel < g_nvis) snprintf(keep, sizeof(keep), "%s", g_pk[g_vis[g_sel]].id);
    g_n = 0; g_nsrc = 1;
    if ((f = popen("pspexp-pkg available 2>/dev/null", "r"))) {
        while (fgets(line, sizeof(line), f) && g_n < PKG_MAX) {
            char *fld[6], *p = line;
            int k;
            Pkg *pk;
            line[strcspn(line, "\r\n")] = 0;
            for (k = 0; k < 6; k++) { fld[k] = p; p = p ? strchr(p, '|') : NULL; if (p) *p++ = 0; }
            if (!fld[0][0] || !fld[5] || !(pk = find(fld[0]))) continue;
            snprintf(pk->avail, sizeof(pk->avail), "%s", fld[1]);
            snprintf(pk->st, sizeof(pk->st), "%s", fld[2]);
            snprintf(pk->cat, sizeof(pk->cat), "%s", fld[3]);
            snprintf(pk->title, sizeof(pk->title), "%s", fld[4]);
            snprintf(pk->desc, sizeof(pk->desc), "%s", fld[5]);
            snprintf(line, sizeof(line), CATALOGUE "/recipes/%s/cover.png", pk->id);
            if (access(line, R_OK) == 0) snprintf(pk->cover, sizeof(pk->cover), "%s", line);
        }
        pclose(f);
    }
    read_installed();   /* adds installed titles the catalogue lacks; the rest only get their installed version */
    for (i = 0; i < g_n; i++) {
        Pkg *pk = &g_pk[i];
        if (!pk->st[0]) {   /* not in the catalogue */
            char sysdir[800];
            snprintf(pk->st, sizeof(pk->st), "local");
            snprintf(sysdir, sizeof(sysdir), "%s/apps/%s/systems", root_dir(), pk->id);
            snprintf(pk->cat, sizeof(pk->cat), "%s", access(sysdir, F_OK) == 0 ? "system" : "game");
        }
        if (!pk->title[0]) snprintf(pk->title, sizeof(pk->title), "%s", pk->id);
        if (!pk->cover[0]) {   /* no catalogue cover: the installed package's own picture, if it has one */
            char cmd[900], line2[800];
            FILE *pf;
            snprintf(cmd, sizeof(cmd), "ls '%s/apps/%s'/*.png 2>/dev/null | head -1", root_dir(), pk->id);
            if ((pf = popen(cmd, "r"))) {
                if (fgets(line2, sizeof(line2), pf)) { line2[strcspn(line2, "\r\n")] = 0; snprintf(pk->cover, sizeof(pk->cover), "%s", line2); }
                pclose(pf);
            }
        }
    }
    qsort(g_pk, (size_t)g_n, sizeof(Pkg), cmp_pkg);
    g_nvis = 0;
    filter();
    for (i = 0; i < g_nvis; i++) if (!strcmp(g_pk[g_vis[i]].id, keep)) g_sel = i;
}

/* every repository, and what is installed. Web ones: read from the cache, which fetch() fills */
static void load(void)
{
    if (g_catmode) { load_catalogue(); return; }
    char dir[700], line[700], keep[48] = "";
    FILE *f;
    int i;
    if (g_sel < g_nvis) snprintf(keep, sizeof(keep), "%s", g_pk[g_vis[g_sel]].id);
    g_n = 0; g_nsrc = 0; g_nremote = 0;
    snprintf(dir, sizeof(dir), "%s/repo", root_dir());
    read_index(dir, dir, 0);
    snprintf(dir, sizeof(dir), "%s/.config/pspexp-launcher/sources", getenv("HOME") ? getenv("HOME") : "");
    if ((f = fopen(dir, "r"))) {
        int k = 0;
        while (fgets(line, sizeof(line), f) && k < SRC_MAX) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0] || line[0] == '#') continue;
            if (!strncmp(line, "http://", 7) || !strncmp(line, "https://", 8)) {
                char base[800], cdir[700];
                snprintf(cdir, sizeof(cdir), "%s/%d", cache_dir(), k++);
                snprintf(base, sizeof(base), "%s/%s", line, board_kind());
                read_index(cdir, base, 1);
                g_nremote++;
            } else read_index(line, line, 0);
        }
        fclose(f);
    }
    read_installed();
    for (i = 0; i < g_n; i++) if (!g_pk[i].title[0]) snprintf(g_pk[i].title, sizeof(g_pk[i].title), "%s", g_pk[i].id);
    qsort(g_pk, (size_t)g_n, sizeof(Pkg), cmp_pkg);
    g_sel = 0;
    for (i = 0; i < g_n; i++) if (!strcmp(g_pk[i].id, keep)) g_sel = i;
    g_nvis = 0;
    for (i = 0; i < g_n; i++) g_vis[g_nvis++] = i;
    g_dirty = 1;
}

/* a shell job in the background, its output in STORE_LOG */
static int run_bg(const char *cmd)
{
    pid_t pid;
    unlink(STORE_LOG);
    if ((pid = fork()) < 0) return -1;
    if (pid == 0) {
        int fd = open(STORE_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644), i;
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        for (i = 3; i < 1024; i++) close(i);   /* not the PSP link */
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    g_pid = pid;
    return 0;
}

static void q(char *out, size_t n, const char *s)   /* a single-quoted shell word */
{
    size_t o = 0;
    if (o < n - 1) out[o++] = '\'';
    for (; *s && o < n - 6; s++) { if (*s == '\'') { memcpy(out + o, "'\\''", 4); o += 4; } else out[o++] = *s; }
    out[o++] = '\''; out[o] = 0;
}

/* the web repositories' index and covers into the cache (the list shows when they arrive) */
static void fetch(void)
{
    char path[700], line[700], cmd[8000], qa[1600], qd[800];
    FILE *f;
    int k = 0;
    size_t l = 0;
    snprintf(path, sizeof(path), "%s/.config/pspexp-launcher/sources", getenv("HOME") ? getenv("HOME") : "");
    if (!(f = fopen(path, "r"))) return;
    cmd[0] = 0;
    l += (size_t)snprintf(cmd + l, sizeof(cmd) - l, "get() { if command -v curl >/dev/null; then curl -fsSL -o \"$2\" \"$1\"; else wget -q -O \"$2\" \"$1\"; fi; }; ");
    while (fgets(line, sizeof(line), f) && k < SRC_MAX && l < sizeof(cmd) - 2000) {
        char base[800], d[700];
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "http://", 7) && strncmp(line, "https://", 8)) continue;
        snprintf(base, sizeof(base), "%s/%s", line, board_kind());
        snprintf(d, sizeof(d), "%s/%d", cache_dir(), k++);
        q(qa, sizeof(qa), base); q(qd, sizeof(qd), d);
        l += (size_t)snprintf(cmd + l, sizeof(cmd) - l,
            "mkdir -p %s && get %s/index %s/index.new && mv %s/index.new %s/index && "
            "for id in $(cut -d'|' -f1 %s/index); do [ -f %s/$id.png ] || get %s/$id.png %s/$id.png || rm -f %s/$id.png; done; ",
            qd, qa, qd, qd, qd, qd, qd, qa, qd, qd);
    }
    fclose(f);
    if (!k) return;
    snprintf(g_job, sizeof(g_job), "Reading the repositories");
    g_job_install = -1;
    if (run_bg(cmd) == 0) g_mode = S_WORKING;
}

/* a default route: the board can reach the repositories */
static int online(void)
{
    FILE *f = fopen("/proc/net/route", "r");
    char line[256];
    int yes = 0;
    if (!f) return 1;   /* not Linux: assume so */
    while (fgets(line, sizeof(line), f)) { char ifc[32], dst[16]; if (sscanf(line, "%31s %15s", ifc, dst) == 2 && !strcmp(dst, "00000000")) yes = 1; }
    fclose(f);
    return yes;
}

static void need_network(const char *title)
{
    snprintf(g_result_title, sizeof(g_result_title), "%s", title);
    snprintf(g_result, sizeof(g_result), "This needs a network: join Wi-Fi in System > Wi-Fi network, then try again.");
    g_mode = S_RESULT;
}

static void catalogue_job(const char *verb, const Pkg *p)
{
    char cmd[300];
    const char *what = !strcmp(verb, "update") ? "Reading the catalogue" : !strcmp(verb, "remove") ? "Removing" :
                       !strcmp(verb, "recover") ? "Repairing" : p && strcmp(p->st, "new") ? "Updating" : "Installing";
    if (strcmp(verb, "remove") && strcmp(verb, "recover") && !online()) { need_network(p ? p->title : "Packages"); return; }
    if (p) snprintf(cmd, sizeof(cmd), "PSPEXP_PKG_WAIT=5 pspexp-pkg %s %s", verb, p->id);
    else snprintf(cmd, sizeof(cmd), "PSPEXP_PKG_WAIT=5 pspexp-pkg %s", verb);
    snprintf(g_job, sizeof(g_job), "%s%s%s", what, p ? " " : "", p ? p->title : "");
    snprintf(g_result_title, sizeof(g_result_title), "%s", p ? p->title : "Packages");
    g_job_install = !strcmp(verb, "update") ? -1 : !strcmp(verb, "remove") ? 0 : 1;
    g_prog = -1; g_phase[0] = 0;
    if (run_bg(cmd) == 0) g_mode = S_WORKING;
}

static void start_install(Pkg *p)
{
    if (g_catmode) {   /* broken: finish or retry through recover, else install / update */
        char j[800];
        snprintf(j, sizeof(j), "%s/state/txn", root_dir());
        catalogue_job(!strcmp(p->st, "broken") && access(j, F_OK) == 0 ? "recover" : "install", p);
        return;
    }
    char cmd[3000], qf[1600], qc[800];
    if (!p->avail[0]) return;
    if (p->remote) {
        char local[800];
        snprintf(local, sizeof(local), "%s/pkg-%s.tar.gz", cache_dir(), p->id);
        q(qf, sizeof(qf), p->file); q(qc, sizeof(qc), local);
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s' && (if command -v curl >/dev/null; then curl -fsSL -o %s %s; else wget -q -O %s %s; fi) && pspexp-pkg install %s; r=$?; rm -f %s; exit $r",
                 cache_dir(), qc, qf, qc, qf, qc, qc);
    } else {
        q(qf, sizeof(qf), p->file);
        snprintf(cmd, sizeof(cmd), "pspexp-pkg install %s", qf);
    }
    snprintf(g_job, sizeof(g_job), "%s %s", p->inst[0] ? "Updating" : "Installing", p->title);
    snprintf(g_result_title, sizeof(g_result_title), "%s", p->title);
    g_job_install = 1;
    if (run_bg(cmd) == 0) g_mode = S_WORKING;
}

static void start_remove(Pkg *p)
{
    if (g_catmode) { catalogue_job("remove", p); return; }
    char cmd[200];
    snprintf(cmd, sizeof(cmd), "pspexp-pkg remove %s", p->id);
    snprintf(g_job, sizeof(g_job), "Removing %s", p->title);
    snprintf(g_result_title, sizeof(g_result_title), "%s", p->title);
    g_job_install = 0;
    if (run_bg(cmd) == 0) g_mode = S_WORKING;
}

static void job_done(int status)
{
    char out[600] = "";
    FILE *f = fopen(STORE_LOG, "r");
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (f) { size_t n = fread(out, 1, sizeof(out) - 1, f); out[n] = 0; fclose(f); }
    while (strlen(out) && out[strlen(out) - 1] == '\n') out[strlen(out) - 1] = 0;
    g_pid = 0;
    if (g_job_install < 0) {
        g_mode = S_LIST; load();
        if (!ok) snprintf(g_err, sizeof(g_err), "%s", g_catmode ? "The catalogue could not be read: is the board online?" : "A web repository could not be read");
        else g_err[0] = 0;
        return;
    }
    {
        const char *last = strrchr(out, '\n'), *m = last ? last + 1 : out;
        if (!strncmp(m, "pspexp-pkg: ", 12)) m += 12;
        if (ok) snprintf(g_result, sizeof(g_result), "%s. The launcher shows the change in a moment.", g_job_install ? "Installed" : "Removed");
        else if (strstr(out, "busy")) snprintf(g_result, sizeof(g_result), "Another package operation is running; try again in a moment.");
        else snprintf(g_result, sizeof(g_result), "It did not work: %s", m[0] ? m : "pspexp-pkg failed");
    }
    g_mode = S_RESULT;
    load();
}

static void st_open(void)
{
    g_exit = 0; g_err[0] = 0; g_mode = S_LIST; g_top = 0;
    g_catmode = access(CATALOGUE "/serial", R_OK) == 0;
    load();
    if (!g_catmode) fetch();   /* web repositories, when there are any */
}

static int st_take_exit(void) { int e = g_exit; g_exit = 0; return e; }

static void st_input(uint32_t b, uint32_t pressed, int up, int down, int left, int right, uint64_t now)
{
    Pkg *p = g_sel < g_nvis ? &g_pk[g_vis[g_sel]] : NULL;
    (void)b; (void)now;
    if (!pressed && !up && !down && !left && !right) return;
    g_dirty = 1;
    switch (g_mode) {
    case S_WORKING: return;   /* it finishes by itself */
    case S_RESULT: if (pressed & (PSPEXP_BUTTON_CROSS | PSPEXP_BUTTON_CIRCLE)) g_mode = S_LIST; return;
    case S_CONFIRM:
        if ((pressed & PSPEXP_BUTTON_CROSS) && p) start_remove(p);
        else if (pressed & PSPEXP_BUTTON_CIRCLE) g_mode = S_LIST;
        return;
    }
    if (up && g_sel > 0) g_sel--;
    if (down && g_sel < g_nvis - 1) g_sel++;
    if (left) g_sel = g_sel > 5 ? g_sel - 5 : 0;
    if (right) g_sel = g_sel + 5 < g_nvis ? g_sel + 5 : (g_nvis ? g_nvis - 1 : 0);
    if (g_catmode && (pressed & (PSPEXP_BUTTON_LTRIGGER | PSPEXP_BUTTON_RTRIGGER))) {
        g_tab = (g_tab + (pressed & PSPEXP_BUTTON_RTRIGGER ? 1 : NTABS - 1)) % NTABS;
        filter();
        return;
    }
    if ((pressed & PSPEXP_BUTTON_CROSS) && p && (p->avail[0] || !strcmp(p->st, "broken")) && strcmp(p->st, "local")) start_install(p);
    if ((pressed & PSPEXP_BUTTON_TRIANGLE) && p && (p->inst[0] || !strcmp(p->st, "broken") || !strcmp(p->st, "local"))) g_mode = S_CONFIRM;
    if (pressed & PSPEXP_BUTTON_START) { if (g_catmode) catalogue_job("update", NULL); else { load(); fetch(); } }
    if (pressed & (PSPEXP_BUTTON_CIRCLE | PSPEXP_BUTTON_SELECT)) g_exit = 1;
}

static int st_needs_frame(uint64_t now)
{
    static uint64_t spin;
    if (g_pid > 0) {
        int st;
        if (waitpid(g_pid, &st, WNOHANG) == g_pid) { job_done(st); g_dirty = 1; }
        else if (now - spin > 120000u) {   /* the spinner turns; apt says how far it is */
            FILE *f = fopen(STORE_LOG, "r");
            spin = now; g_dirty = 1;
            if (f) {
                char line[400], last[400] = "";
                while (fgets(line, sizeof(line), f)) if (!strncmp(line, "dlstatus:", 9) || !strncmp(line, "pmstatus:", 9)) snprintf(last, sizeof(last), "%s", line);
                fclose(f);
                if (last[0]) {   /* dlstatus:N:PCT:text  pmstatus:pkg:PCT:text */
                    char *a = strchr(last, ':'), *b = a ? strchr(a + 1, ':') : NULL, *c = b ? strchr(b + 1, ':') : NULL;
                    if (c) {
                        g_prog = atoi(b + 1);
                        c[strcspn(c, "\r\n")] = 0;
                        snprintf(g_phase, sizeof(g_phase), "%s", c + 1);
                    }
                }
            }
        }
    }
    return g_dirty;
}

/* ---- drawing ------------------------------------------------------------------------ */

enum { ST_CURRENT, ST_UPDATE, ST_NEW, ST_LOCAL, ST_BROKEN };
static int status_of(const Pkg *p)
{
    if (g_catmode) {
        if (!strcmp(p->st, "installed")) return ST_CURRENT;
        if (!strcmp(p->st, "update")) return ST_UPDATE;
        if (!strcmp(p->st, "broken")) return ST_BROKEN;
        if (!strcmp(p->st, "local")) return ST_LOCAL;
        return ST_NEW;
    }
    if (!p->inst[0]) return ST_NEW;
    if (!p->avail[0]) return ST_LOCAL;
    return vercmp(p->avail, p->inst) > 0 || (strcmp(p->avail, p->inst) && !isdigit((unsigned char)p->avail[0])) ? ST_UPDATE : ST_CURRENT;
}

static void chip(int x, int y, const char *text, uint32_t bg, uint32_t fg)
{
    int w = gfx_text_width(FONT_BADGE, text) + 10;
    gfx_nine(NINE_R6, x - w, y, w, 14, bg, 255);
    gfx_text(FONT_BADGE, x - w + 5, y + 1, text, fg, 255, 0);
}

/* a box with a title and text, wrapped at spaces over up to three lines */
static void box(const char *title, const char *text, int h)
{
    int w = 320, bx = (UI_SCREEN_W - w) / 2, by = (UI_SCREEN_H - h) / 2 - 8, ln;
    const char *p = text;
    gfx_shadow(NINE_SHADOW14, bx, by, w, h, 0x000000, 160);
    gfx_nine(NINE_R14, bx, by, w, h, 0x232a3b, 250);
    gfx_text(FONT_BODY, bx + 16, by + 12, title, UI_TEXT, 255, w - 32);
    for (ln = 0; ln < 3 && *p; ln++) {
        char line[200];
        size_t l = 0, cut = 0;
        while (p[l] && l < sizeof(line) - 1) {
            line[l] = p[l]; line[l + 1] = 0;
            if (gfx_text_width(FONT_SMALL, line) > w - 32) break;
            if (p[l] == ' ') cut = l;
            l++;
        }
        if (p[l] && cut) l = cut;
        line[l] = 0;
        gfx_text(FONT_SMALL, bx + 16, by + 34 + ln * 14, line, UI_SUB, 255, w - 32);
        p += l;
        while (*p == ' ') p++;
    }
}

static void st_render(PspExpContext *ctx, uint64_t now)
{
    const int LX = 164, LW = 308, ROWH = 30, ROWS = 7;
    int i, x, upd = 0, inst = 0, broken = 0;
    char s[240], a[32];
    const Pkg *p = g_sel < g_nvis ? &g_pk[g_vis[g_sel]] : NULL;
    gfx_frame(ctx);
    pspexp_begin_frame(ctx);
    pspexp_set_clip(ctx, 0, 0, UI_SCREEN_W, UI_SCREEN_H);
    gfx_backdrop();
    gfx_icon("package", 16, 12, 8, UI_ACCENT, 255);
    gfx_text(FONT_TITLE, 34, 5, "Packages", UI_TEXT, 255, 0);
    if (g_catmode) {   /* the sections, as tabs (L / R) */
        static const char *tab[NTABS] = { "Games", "Emulators", "Apps" };
        int tx = 150;
        for (i = 0; i < NTABS; i++) {
            int w = gfx_text_width(FONT_SMALL, tab[i]) + 18, on = i == g_tab;
            if (on) gfx_nine(NINE_R6, tx, 7, w, 18, UI_ACCENT, 200);
            gfx_text(FONT_SMALL, tx + 9, 8, tab[i], on ? 0xffffff : UI_SUB, 255, 0);
            tx += w + 4;
        }
    }
    for (i = 0; i < g_n; i++) {
        int st = status_of(&g_pk[i]);
        inst += st == ST_CURRENT || st == ST_UPDATE || st == ST_LOCAL;
        upd += st == ST_UPDATE; broken += st == ST_BROKEN;
    }
    snprintf(s, sizeof(s), "%d installed", inst);
    if (upd) { snprintf(a, sizeof(a), ", %d update%s", upd, upd == 1 ? "" : "s"); strncat(s, a, sizeof(s) - strlen(s) - 1); }
    if (broken) { snprintf(a, sizeof(a), ", %d broken", broken); strncat(s, a, sizeof(s) - strlen(s) - 1); }
    gfx_text(FONT_SMALL, UI_SCREEN_W - 12 - gfx_text_width(FONT_SMALL, s), 9, s, broken ? 0xff9a9a : upd ? 0xffc766 : UI_MUTED, 255, 0);
    if (!g_n) {
        if (g_catmode) snprintf(s, sizeof(s), "The catalogue is empty. Join a network (System > Wi-Fi network) and press START to read it.");
        else snprintf(s, sizeof(s), "No packages yet. Put a repository in %s/repo (on the Mac: pspexp-pkg-build publish), or add a source to ~/.config/pspexp-launcher/sources.", root_dir());
        box(g_nsrc ? "No packages" : "No repository", s, 110);
    } else if (!p) {
        gfx_text(FONT_BODY, LX + 12, 40, "Nothing in this section yet", UI_MUTED, 255, 0);
    } else {
        CoverSpec cs;
        /* the highlighted one: cover and details */
        memset(&cs, 0, sizeof(cs));
        snprintf(s, sizeof(s), "pkg:%s", p->id);
        cs.key = s; cs.title = p->title; cs.badge = !strcmp(p->cat, "system") ? "SYSTEM" : !strcmp(p->cat, "game") ? "GAME" : "PACKAGE";
        cs.icon = !strcmp(p->cat, "system") ? "gamepad-2" : "package";
        if (p->cover[0]) cs.art = p->cover;
        gfx_cover(&cs, 16, 36, 255);
        {
            char v[120];
            int y = 170;
            if (g_catmode && !strcmp(p->st, "local")) snprintf(v, sizeof(v), "%s", p->inst);
            else if (g_catmode) {   /* the recipe's version: rN installed, N offered */
                const char *iv = p->inst[0] == 'r' ? p->inst + 1 : p->inst;
                if (p->inst[0] && p->avail[0] && strcmp(iv, p->avail)) snprintf(v, sizeof(v), "recipe %s -> %s", iv, p->avail);
                else snprintf(v, sizeof(v), "recipe %s", p->avail[0] ? p->avail : iv[0] ? iv : "-");
            } else if (p->inst[0] && p->avail[0] && strcmp(p->inst, p->avail)) snprintf(v, sizeof(v), "%s -> %s", p->inst, p->avail);
            else snprintf(v, sizeof(v), "%s", p->inst[0] ? p->inst : p->avail);
            gfx_text(FONT_BADGE, 16, y, "version", UI_MUTED, 255, 0);
            gfx_text(FONT_BADGE, 60, y, v, UI_SUB, 255, 84);
            if (p->bytes) {
                char sz[32];
                if (p->bytes >= 1048576) snprintf(sz, sizeof(sz), "%.1f MB", p->bytes / 1048576.0); else snprintf(sz, sizeof(sz), "%lld KB", p->bytes / 1024);
                gfx_text(FONT_BADGE, 16, y + 14, "size", UI_MUTED, 255, 0);
                gfx_text(FONT_BADGE, 60, y + 14, sz, UI_SUB, 255, 84);
            }
            gfx_text(FONT_BADGE, 16, y + 28, "from", UI_MUTED, 255, 0);
            gfx_text(FONT_BADGE, 60, y + 28, g_catmode ? (!strcmp(p->st, "local") ? "an older package" : "apt, by recipe") :
                     !p->avail[0] ? "installed only" : p->remote ? "the web" : "this board", UI_SUB, 255, 84);
        }
        /* the list: the section shown */
        if (g_sel < g_top) g_top = g_sel;
        if (g_sel >= g_top + ROWS) g_top = g_sel - ROWS + 1;
        for (i = 0; i < ROWS && g_top + i < g_nvis; i++) {
            const Pkg *k = &g_pk[g_vis[g_top + i]];
            int y = 34 + i * ROWH, sel = g_top + i == g_sel, st = status_of(k);
            if (sel) gfx_nine(NINE_R10, LX, y, LW, ROWH - 2, 0xffffff, 26);
            if (sel) gfx_rect(LX, y + 6, 3, ROWH - 14, UI_ACCENT, 255);
            gfx_text(FONT_BODY, LX + 12, y + 1, k->title, sel ? 0xffffff : UI_TEXT, 255, LW - 110);
            gfx_text(FONT_BADGE, LX + 12, y + 16, k->desc, UI_MUTED, 255, LW - 24);
            if (st == ST_CURRENT) chip(LX + LW - 8, y + 2, "Installed", 0x1f5f4a, 0xd1fae5);
            else if (st == ST_UPDATE) chip(LX + LW - 8, y + 2, "Update", 0x7c5a12, 0xfff3c4);
            else if (st == ST_BROKEN) chip(LX + LW - 8, y + 2, "Broken", 0x7f1d1d, 0xfecaca);
            else if (st == ST_LOCAL) chip(LX + LW - 8, y + 2, "Installed", 0x2a3142, UI_SUB);
            else chip(LX + LW - 8, y + 2, "Get", 0x3a4560, 0xffffff);
        }
    }
    if (g_err[0]) gfx_text(FONT_BADGE, 16, 238, g_err, 0xff9a9a, 255, UI_SCREEN_W - 32);
    x = 12;
    if (p && g_mode == S_LIST) {
        int st = status_of(p);
        if (st == ST_BROKEN) x = ui_hint_btn(x, 'x', "Retry");
        else if (st != ST_LOCAL && p->avail[0]) x = ui_hint_btn(x, 'x', st == ST_NEW ? "Install" : st == ST_UPDATE ? "Update" : "Reinstall");
        if (p->inst[0] || st == ST_BROKEN || st == ST_LOCAL) x = ui_hint_btn(x, 't', "Remove");
    }
    if (g_catmode) x = ui_hint_cap(x, "L/R", "Section", UI_SUB);
    x = ui_hint_cap(x, "START", "Refresh", UI_SUB);
    x = ui_hint_btn(x, 'o', "Close");
    if (g_mode == S_CONFIRM && p) {
        snprintf(s, sizeof(s), "Remove %s from this board? Its saves and settings in your home folder stay.", p->title);
        box("Remove", s, 96);   /* the buttons sit below two lines of text */
        gfx_button('x', (UI_SCREEN_W - 320) / 2 + 16, (UI_SCREEN_H - 96) / 2 - 8 + 72); gfx_text(FONT_SMALL, (UI_SCREEN_W - 320) / 2 + 34, (UI_SCREEN_H - 96) / 2 - 8 + 71, "Remove", UI_TEXT, 255, 0);
        gfx_button('o', (UI_SCREEN_W - 320) / 2 + 120, (UI_SCREEN_H - 96) / 2 - 8 + 72); gfx_text(FONT_SMALL, (UI_SCREEN_W - 320) / 2 + 138, (UI_SCREEN_H - 96) / 2 - 8 + 71, "Keep", UI_TEXT, 255, 0);
    } else if (g_mode == S_WORKING) {
        int bx = (UI_SCREEN_W - 320) / 2, by = (UI_SCREEN_H - 84) / 2 - 8;
        box(g_job, g_phase[0] ? g_phase : "This runs in the background; a moment.", 84);
        gfx_icon("loader", 14, bx + 320 - 30, by + 12, UI_ACCENT, 128 + (int)((now / 120000u) % 2) * 127);
        gfx_nine(NINE_R6, bx + 16, by + 60, 288, 8, 0x151a26, 255);
        if (g_prog > 0) gfx_nine(NINE_R6, bx + 16, by + 60, 288 * (g_prog > 100 ? 100 : g_prog) / 100, 8, UI_ACCENT, 255);
    } else if (g_mode == S_RESULT) {
        box(g_result_title, g_result, 90);
        gfx_button('x', (UI_SCREEN_W + 320) / 2 - 60, (UI_SCREEN_H - 90) / 2 - 8 + 68); gfx_text(FONT_SMALL, (UI_SCREEN_W + 320) / 2 - 42, (UI_SCREEN_H - 90) / 2 - 8 + 67, "OK", UI_TEXT, 255, 0);
    }
    pspexp_present(ctx);
    g_dirty = 0;
}

static void st_state(char *out, size_t n)
{
    static const char *mode[] = { "list", "confirm", "working", "result" };
    static const char *stn[] = { "installed", "update", "new", "local", "broken" };
    static const char *tabn[NTABS] = { "games", "emulators", "apps" };
    size_t l = strlen(out);
    int i;
    if (l < n) l += (size_t)snprintf(out + l, n - l, "store.mode=%s\nstore.catalogue=%d\nstore.section=%s\nstore.sources=%d\nstore.selected=%s\nstore.progress=%d\nstore.phase=%s\nstore.result=%s\nstore.packages=",
                                     mode[g_mode], g_catmode, g_catmode ? tabn[g_tab] : "all", g_nsrc, g_sel < g_nvis ? g_pk[g_vis[g_sel]].id : "",
                                     g_prog, g_phase, g_mode == S_RESULT ? g_result : "");
    for (i = 0; i < g_n && l < n; i++) l += (size_t)snprintf(out + l, n - l, "%s%s:%s", i ? "|" : "", g_pk[i].id, stn[status_of(&g_pk[i])]);
    if (l < n) snprintf(out + l, n - l, "\n");
}

const AppView STORE_APP = { "store", st_open, st_input, st_needs_frame, st_render, st_take_exit, st_state };
