/*
 * The package manager (Apps > Packages): what this board's repositories offer
 * next to what is installed, and installs, updates and removals through
 * pspexp-pkg, in the background.
 *
 * It shows the recipe catalogue through pspexp-pkg (docs/Package_Manager.md):
 * `pspexp-pkg available` for the list, `install` / `remove` / `update` /
 * `recover` for the work, apt's progress read from its status lines.
 * Sections: Games, Emulators, Apps (L / R). The catalogue: the signed one apt's
 * helper keeps in /usr/share/pspexp/catalogue (the Debian boards), or the one the
 * Mac publishes to $PSPEXP_ROOT/catalogue with the builds it installs from (the
 * Omega2+, §9: no network needed there).
 *
 *   up / down   a package      cross     install / update (reinstall when current)
 *   triangle    remove (asks)  start     read the catalogue again
 *   circle      close
 */
#include "appview.h"
#include "launcher_gfx.h"
#include "launcher_ui.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PKG_MAX 128
#define STORE_LOG "/tmp/pspexp-store.log"

typedef struct Pkg {
    char id[48], title[96], desc[300];
    char graphics[16];             /* how it draws: sdl | opengl | opengl-shaders | board-gpu ("" not said) */
    char avail[40], inst[40];      /* versions: offered, installed ("" none) */
    char cover[700];               /* its picture: a path ("" none) */
    char cat[12];                  /* catalogue: game | system | app | tool */
    char st[12];                   /* catalogue: installed | update | new | broken | local */
} Pkg;

static Pkg g_pk[PKG_MAX];
static int g_n, g_sel, g_top, g_dirty = 1, g_exit;
static char g_err[200];
enum { S_LIST, S_CONFIRM, S_WORKING, S_RESULT };
static int g_mode;
static pid_t g_pid;                /* the background job */
static char g_job[160], g_result_title[96], g_result[400];
static int g_job_install;
static int g_catmode;              /* there is a catalogue */
static int g_local;                /* published by the Mac with its builds (the Omega): no network needed */
static char g_catdir[600];
#define CATALOGUE "/usr/share/pspexp/catalogue"
enum { TAB_GAMES, TAB_EMULATORS, TAB_APPS, NTABS };
static int g_tab;
static int g_vis[PKG_MAX], g_nvis;  /* the packages of the section shown (g_sel indexes this) */
static int g_prog;                 /* the job's progress, percent (-1 unknown) */
static char g_phase[160];          /* and what it is doing */

static const char *root_dir(void) { const char *r = getenv("PSPEXP_ROOT"); return r && *r ? r : "/opt/pspexp"; }

static Pkg *find(const char *id)
{
    int i;
    for (i = 0; i < g_n; i++) if (!strcmp(g_pk[i].id, id)) return &g_pk[i];
    if (g_n >= PKG_MAX) return NULL;
    memset(&g_pk[g_n], 0, sizeof(Pkg));
    snprintf(g_pk[g_n].id, sizeof(g_pk[g_n].id), "%s", id);
    return &g_pk[g_n++];
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
    for (i = 0; i < g_n; i++) if (tab_of(&g_pk[i]) == g_tab) g_vis[g_nvis++] = i;
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
    g_n = 0;
    if ((f = popen("pspexp-pkg available 2>/dev/null", "r"))) {
        while (fgets(line, sizeof(line), f) && g_n < PKG_MAX) {
            char *fld[7], *p = line;
            int k;
            Pkg *pk;
            line[strcspn(line, "\r\n")] = 0;
            for (k = 0; k < 7; k++) { fld[k] = p; p = p ? strchr(p, '|') : NULL; if (p) *p++ = 0; }   /* (fld[6]: older pspexp-pkg has none) */
            if (!fld[0][0] || !fld[5] || !(pk = find(fld[0]))) continue;
            snprintf(pk->avail, sizeof(pk->avail), "%s", fld[1]);
            snprintf(pk->st, sizeof(pk->st), "%s", fld[2]);
            snprintf(pk->cat, sizeof(pk->cat), "%s", fld[3]);
            snprintf(pk->title, sizeof(pk->title), "%s", fld[4]);
            snprintf(pk->desc, sizeof(pk->desc), "%s", fld[5]);
            snprintf(pk->graphics, sizeof(pk->graphics), "%s", fld[6] ? fld[6] : "");
            snprintf(line, sizeof(line), "%s/recipes/%s/cover.png", g_catdir, pk->id);
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
    snprintf(g_result, sizeof(g_result), "This needs a network: join Wi-Fi in Settings > Network, then try again.");
    g_mode = S_RESULT;
}

static void catalogue_job(const char *verb, const Pkg *p)
{
    char cmd[300];
    const char *what = !strcmp(verb, "update") ? "Reading the catalogue" : !strcmp(verb, "remove") ? "Removing" :
                       !strcmp(verb, "recover") ? "Repairing" : p && strcmp(p->st, "new") ? "Updating" : "Installing";
    if (!g_local && strcmp(verb, "remove") && strcmp(verb, "recover") && !online()) { need_network(p ? p->title : "Packages"); return; }
    if (p) snprintf(cmd, sizeof(cmd), "PSPEXP_PKG_WAIT=5 pspexp-pkg %s %s", verb, p->id);
    else snprintf(cmd, sizeof(cmd), "PSPEXP_PKG_WAIT=5 pspexp-pkg %s", verb);
    snprintf(g_job, sizeof(g_job), "%s%s%s", what, p ? " " : "", p ? p->title : "");
    snprintf(g_result_title, sizeof(g_result_title), "%s", p ? p->title : "Packages");
    g_job_install = !strcmp(verb, "update") ? -1 : !strcmp(verb, "remove") ? 0 : 1;
    g_prog = -1; g_phase[0] = 0;
    if (run_bg(cmd) == 0) g_mode = S_WORKING;
}

static void start_install(Pkg *p)
{   /* broken: finish or retry through recover, else install / update */
    char j[800];
    snprintf(j, sizeof(j), "%s/state/txn", root_dir());
    catalogue_job(!strcmp(p->st, "broken") && access(j, F_OK) == 0 ? "recover" : "install", p);
}

static void start_remove(Pkg *p) { catalogue_job("remove", p); }

static void job_done(int status)
{
    char out[600] = "";
    FILE *f = fopen(STORE_LOG, "r");
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (f) { size_t n = fread(out, 1, sizeof(out) - 1, f); out[n] = 0; fclose(f); }
    while (strlen(out) && out[strlen(out) - 1] == '\n') out[strlen(out) - 1] = 0;
    g_pid = 0;
    if (g_job_install < 0) {
        g_mode = S_LIST; load_catalogue();
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
    load_catalogue();
}

static void st_open(void)
{
    g_exit = 0; g_err[0] = 0; g_mode = S_LIST; g_top = 0;
    {   /* $PSPEXP_CATALOGUE, the one published with the board's builds, or apt's helper's */
        const char *e = getenv("PSPEXP_CATALOGUE");
        char pub[700];
        snprintf(pub, sizeof(pub), "%s/catalogue/serial", root_dir());
        g_local = !(e && *e) && access(pub, R_OK) == 0;
        if (e && *e) snprintf(g_catdir, sizeof(g_catdir), "%s", e);
        else if (g_local) snprintf(g_catdir, sizeof(g_catdir), "%s/catalogue", root_dir());
        else snprintf(g_catdir, sizeof(g_catdir), "%s", CATALOGUE);
        snprintf(pub, sizeof(pub), "%s/serial", g_catdir);
        g_catmode = access(pub, R_OK) == 0;
    }
    load_catalogue();
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
    if (pressed & (PSPEXP_BUTTON_LTRIGGER | PSPEXP_BUTTON_RTRIGGER)) {
        g_tab = (g_tab + (pressed & PSPEXP_BUTTON_RTRIGGER ? 1 : NTABS - 1)) % NTABS;
        filter();
        return;
    }
    if ((pressed & PSPEXP_BUTTON_CROSS) && p && (p->avail[0] || !strcmp(p->st, "broken")) && strcmp(p->st, "local")) start_install(p);
    /* square removes (square deletes everywhere: a letter, a Bluetooth pairing); triangle stays for showing more */
    if ((pressed & PSPEXP_BUTTON_SQUARE) && p && (p->inst[0] || !strcmp(p->st, "broken") || !strcmp(p->st, "local"))) g_mode = S_CONFIRM;
    if (pressed & PSPEXP_BUTTON_START) { if (g_local) load_catalogue(); else catalogue_job("update", NULL); }
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
    if (!strcmp(p->st, "installed")) return ST_CURRENT;
    if (!strcmp(p->st, "update")) return ST_UPDATE;
    if (!strcmp(p->st, "broken")) return ST_BROKEN;
    if (!strcmp(p->st, "local")) return ST_LOCAL;
    return ST_NEW;
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
    gfx_nine(NINE_R14, bx, by, w, h, 0x1b2a46, 250);
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
    const int LX = 164, LW = 308, ROWH = 30, ROWS = 7, OPEN = 22;
    int i, x, upd = 0, inst = 0, broken = 0;
    char s[240], a[32];
    const Pkg *p = g_sel < g_nvis ? &g_pk[g_vis[g_sel]] : NULL;
    gfx_frame(ctx);
    pspexp_begin_frame(ctx);
    pspexp_set_clip(ctx, 0, 0, UI_SCREEN_W, UI_SCREEN_H);
    gfx_backdrop();
    gfx_icon("package", 16, 12, 8, UI_ACCENT, 255);
    gfx_text(FONT_TITLE, 34, 5, "Packages", UI_TEXT, 255, 0);
    {   /* the sections, as tabs (L / R) */
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
        if (g_local || !g_catmode) snprintf(s, sizeof(s), g_local ? "The catalogue is empty." : "No catalogue yet. Join a network (Settings > Network) and press START to read it.");
        else snprintf(s, sizeof(s), "The catalogue is empty. Join a network (Settings > Network) and press START to read it.");
        box(g_catmode ? "No packages" : "No catalogue", s, 110);
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
            if (!strcmp(p->st, "local") || (g_local && p->inst[0])) snprintf(v, sizeof(v), "%s", p->inst);   /* a build's own version */
            else {   /* the recipe's version: rN installed, N offered */
                const char *iv = p->inst[0] == 'r' ? p->inst + 1 : p->inst;
                if (p->inst[0] && p->avail[0] && strcmp(iv, p->avail)) snprintf(v, sizeof(v), "recipe %s -> %s", iv, p->avail);
                else snprintf(v, sizeof(v), "recipe %s", p->avail[0] ? p->avail : iv[0] ? iv : "-");
            }
            gfx_text(FONT_BADGE, 16, y, "version", UI_MUTED, 255, 0);
            gfx_text(FONT_BADGE, 60, y, v, UI_SUB, 255, 84);
            gfx_text(FONT_BADGE, 16, y + 28, "from", UI_MUTED, 255, 0);
            gfx_text(FONT_BADGE, 60, y + 28, !strcmp(p->st, "local") ? "an older package" : g_local ? "a PSPExp build" : "apt, by recipe", UI_SUB, 255, 84);
            if (p->graphics[0]) {   /* how it draws */
                gfx_text(FONT_BADGE, 16, y + 56, "draws", UI_MUTED, 255, 0);
                gfx_text(FONT_BADGE, 60, y + 56, !strcmp(p->graphics, "sdl") ? "SDL" : !strcmp(p->graphics, "opengl") ? "OpenGL" : !strcmp(p->graphics, "board-gpu") ? "board GPU" : "OpenGL, shaders", UI_SUB, 255, 84);
            }
        }
        /* the list: the section shown */
        /* the selected row open (OPEN taller), its whole description over three lines; so one row fewer fits */
        if (g_sel < g_top) g_top = g_sel;
        if (g_sel >= g_top + ROWS - 1) g_top = g_sel - (ROWS - 1) + 1;
        for (i = 0; i < ROWS && g_top + i < g_nvis; i++) {
            const Pkg *k = &g_pk[g_vis[g_top + i]];
            int sel = g_top + i == g_sel, st = status_of(k), y = 34 + i * ROWH + (g_top + i > g_sel ? OPEN : 0), h = ROWH + (sel ? OPEN : 0);
            if (y + h > 34 + ROWS * ROWH) break;
            if (sel) gfx_nine(NINE_R10, LX, y, LW, h - 2, 0xffffff, 26);
            if (sel) gfx_rect(LX, y + 6, 3, h - 14, UI_ACCENT, 255);
            gfx_text(FONT_BODY, LX + 12, y + 1, k->title, sel ? 0xffffff : UI_TEXT, 255, LW - 110);
            if (sel) gfx_text_wrapped(FONT_BADGE, LX + 12, y + 16, k->desc, UI_SUB, 255, LW - 24, 11, 3);
            else gfx_text(FONT_BADGE, LX + 12, y + 16, k->desc, UI_MUTED, 255, LW - 24);
            if (st == ST_CURRENT) chip(LX + LW - 8, y + 2, "Installed", 0x1f5f4a, 0xd1fae5);
            else if (st == ST_UPDATE) chip(LX + LW - 8, y + 2, "Update", 0x7c5a12, 0xfff3c4);
            else if (st == ST_BROKEN) chip(LX + LW - 8, y + 2, "Broken", 0x7f1d1d, 0xfecaca);
            else if (st == ST_LOCAL) chip(LX + LW - 8, y + 2, "Installed", 0x1e2c4a, UI_SUB);
            else chip(LX + LW - 8, y + 2, "Get", 0x3a4560, 0xffffff);
        }
    }
    if (g_err[0]) gfx_text(FONT_BADGE, 16, 238, g_err, 0xff9a9a, 255, UI_SCREEN_W - 32);
    x = 12;
    if (p && g_mode == S_LIST) {
        int st = status_of(p);
        if (st == ST_BROKEN) x = ui_hint_btn(x, 'x', "Retry");
        else if (st != ST_LOCAL && p->avail[0]) x = ui_hint_btn(x, 'x', st == ST_NEW ? "Install" : st == ST_UPDATE ? "Update" : "Reinstall");
        if (p->inst[0] || st == ST_BROKEN || st == ST_LOCAL) x = ui_hint_btn(x, 's', "Remove");
    }
    x = ui_hint_cap(x, "L/R", "Section", UI_SUB);
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
    if (l < n) l += (size_t)snprintf(out + l, n - l, "store.mode=%s\nstore.catalogue=%d\nstore.local=%d\nstore.section=%s\nstore.selected=%s\nstore.progress=%d\nstore.phase=%s\nstore.result=%s\nstore.packages=",
                                     mode[g_mode], g_catmode, g_local, tabn[g_tab], g_sel < g_nvis ? g_pk[g_vis[g_sel]].id : "",
                                     g_prog, g_phase, g_mode == S_RESULT ? g_result : "");
    for (i = 0; i < g_n && l < n; i++) l += (size_t)snprintf(out + l, n - l, "%s%s:%s", i ? "|" : "", g_pk[i].id, stn[status_of(&g_pk[i])]);
    if (l < n) snprintf(out + l, n - l, "\n");
}

const AppView STORE_APP = { "store", st_open, st_input, st_needs_frame, st_render, st_take_exit, st_state };
