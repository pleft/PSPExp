/* pspexp-term's file manager view: see files.h */
#include "files.h"
#include "files_ops.h"
#include "files_psp.h"
#include "launcher_gfx.h"
#include "launcher_ui.h"
#include "launcher.h"

#include <ctype.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

/* ---- layout -------------------------------------------------------------------- */

#define PANEL_Y   6
#define PANEL_H   238
#define PANEL_W   230
#define PANEL_GAP 8
#define LIST_Y    (PANEL_Y + 34)
#define ROW_H     16
#define ROWS      12

typedef struct Panel {
    char path[FOPS_PATH];
    FList list;
    int sel, top;
    char error[160];              /* the folder could not be read */
    char mem_root[FOPS_DRIVES + 1][FOPS_PATH], mem_path[FOPS_DRIVES + 1][FOPS_PATH];   /* the folder it had on each drive */
    int nmem;
    int on_drives;                /* the highlight is on the drive tabs, not the list */
    char space[48];               /* on the PSP's stick: "1.2 GB free" */
} Panel;

static Panel g_p[2];
static int g_act;                 /* the active panel */
static int g_hidden, g_sort;      /* options */
static int g_dirty = 1, g_exit, g_term;
static char g_term_path[FOPS_PATH];
static int g_caret;

/* what is on top of the panels */
enum { O_NONE, O_DRIVES, O_ACTIONS, O_OPTIONS, O_CONFIRM, O_PROGRESS, O_MESSAGE, O_KBD, O_VIEW, O_IMAGE, O_INSTALLING };
static int g_over;
static int g_msel;                /* the menu's highlighted row (the drive list's too) */

/* a question waiting for yes / no */
enum { Q_DELETE, Q_REPLACE_COPY, Q_REPLACE_MOVE, Q_INSTALL };
static int g_q;
static char g_qtext[200];

/* the running job */
static FJob g_job;
static int g_job_on;
static char g_msg[300], g_msg_title[64];

/* the keyboard: a new name */
enum { K_RENAME, K_MKDIR };
static int g_kmode;
static UiKbd g_kbd;

/* a package being installed (pspexp-pkg install, in the background) */
static pid_t g_inst_pid;
static char g_inst_file[FOPS_PATH], g_inst_name[256];
#define INSTALL_LOG "/tmp/pspexp-files-install.log"

/* the picture viewer: which entry of the active panel, its size */
static int g_img_idx, g_img_w, g_img_h;

/* the text viewer */
static char **g_vlines; static int g_vn, g_vtop;
static char g_vtitle[256];

static void drives_refresh(void);
static Panel *act(void) { return &g_p[g_act]; }
static Panel *other(void) { return &g_p[!g_act]; }

/* ---- panels ---------------------------------------------------------------------- */

static void panel_load(Panel *p, const char *keep_name)
{
    int i;
    p->error[0] = 0;
    if (fops_list(p->path, g_hidden, g_sort, &p->list) < 0) {
        snprintf(p->error, sizeof(p->error), "%s", fops_strerror(errno));
        p->list.n = 0;
    }
    p->space[0] = 0;
    if (fops_is_remote(p->path) && !p->error[0]) {
        uint64_t fr, tot;
        if (files_psp_space(p->path, &fr, &tot) == 0 && tot) { char h[32]; fops_human(fr, h, sizeof(h)); snprintf(p->space, sizeof(p->space), "%s free", h); }
    }
    if (p->sel >= p->list.n) p->sel = p->list.n ? p->list.n - 1 : 0;
    if (keep_name && keep_name[0])
        for (i = 0; i < p->list.n; i++) if (!strcmp(p->list.e[i].name, keep_name)) { p->sel = i; break; }
    if (p->sel < 0) p->sel = 0;
    g_dirty = 1;
}

/* both panels again (after a job), keeping each one's place */
static void reload_all(void)
{
    int k;
    for (k = 0; k < 2; k++) {
        char keep[256] = "";
        Panel *p = &g_p[k];
        if (p->sel < p->list.n) snprintf(keep, sizeof(keep), "%s", p->list.e[p->sel].name);
        panel_load(p, keep);
    }
}

static void panel_cd(Panel *p, const char *path, const char *keep)
{
    char old[FOPS_PATH];
    snprintf(old, sizeof(old), "%s", p->path);
    snprintf(p->path, sizeof(p->path), "%s", path);
    p->sel = 0; p->top = 0;
    panel_load(p, keep);
    if (fops_is_remote(path) && !files_psp_available()) return;   /* the PSP not there yet: shown when it is */
    if (p->error[0] && strcmp(old, path)) {   /* could not go in: stay, say why */
        snprintf(g_msg_title, sizeof(g_msg_title), "Cannot open the folder");
        snprintf(g_msg, sizeof(g_msg), "%s", p->error);
        snprintf(p->path, sizeof(p->path), "%s", old);
        panel_load(p, NULL);
        g_over = O_MESSAGE;
    }
}

static void panel_up(Panel *p)
{
    char path[FOPS_PATH], name[256];
    if (fops_is_root(p->path)) return;
    snprintf(path, sizeof(path), "%s", p->path);
    snprintf(name, sizeof(name), "%s", fops_parent(path));
    panel_cd(p, path, name);
}

void files_open(const char *path)
{
    const char *home = getenv("HOME");
    drives_refresh();   /* before any panel: they are drawn with their drive */
    if (!g_p[0].path[0]) {   /* the first time: the other panel at home */
        snprintf(g_p[1].path, sizeof(g_p[1].path), "%s", home ? home : "/");
        panel_load(&g_p[1], NULL);
    }
    g_act = 0;
    panel_cd(&g_p[0], path && path[0] ? path : home ? home : "/", NULL);
    if (g_p[1].path[0]) panel_load(&g_p[1], NULL);
    g_over = O_NONE;
    g_dirty = 1;
}

void files_invalidate(void) { g_dirty = 1; }
int  files_take_exit(void) { int e = g_exit; g_exit = 0; return e; }
int  files_take_terminal(char *path, size_t n)
{
    if (!g_term) return 0;
    g_term = 0;
    snprintf(path, n, "%s", g_term_path);
    return 1;
}

/* the marked names of the active panel, or the highlighted one */
static char (*targets(int *n))[256]
{
    Panel *p = act();
    int i, c = 0;
    char (*names)[256];
    for (i = 0; i < p->list.n; i++) c += p->list.e[i].marked && !p->list.e[i].is_up;
    if (!c && p->sel < p->list.n && !p->list.e[p->sel].is_up) c = 1;
    *n = c;
    if (!c || !(names = calloc((size_t)c, 256))) { *n = 0; return NULL; }
    c = 0;
    for (i = 0; i < p->list.n; i++) if (p->list.e[i].marked && !p->list.e[i].is_up) snprintf(names[c++], 256, "%s", p->list.e[i].name);
    if (!c) snprintf(names[0], 256, "%s", p->list.e[p->sel].name);
    return names;
}

static void job_begin(int kind)
{
    int n;
    char (*names)[256] = targets(&n);
    if (!n) return;
    memset(&g_job, 0, sizeof(g_job));
    g_job.kind = kind;
    snprintf(g_job.src, sizeof(g_job.src), "%s", act()->path);
    snprintf(g_job.dst, sizeof(g_job.dst), "%s", other()->path);
    g_job.names = names; g_job.nnames = n;
    if (fjob_start(&g_job) < 0) {
        fjob_finish(&g_job);
        snprintf(g_msg_title, sizeof(g_msg_title), "Could not start");
        snprintf(g_msg, sizeof(g_msg), "no thread for the job");
        g_over = O_MESSAGE;
        return;
    }
    g_job_on = 1;
    g_over = O_PROGRESS;
}

/* copy / move: ask before replacing anything in the other panel */
static void transfer(int kind)
{
    int n, c;
    char first[256], (*names)[256] = targets(&n);
    if (!n) return;
    if (!strcmp(act()->path, other()->path)) {
        free(names);
        snprintf(g_msg_title, sizeof(g_msg_title), "Same folder");
        snprintf(g_msg, sizeof(g_msg), "Both panels show the same folder: open another one in the other panel first (L / R switches).");
        g_over = O_MESSAGE;
        return;
    }
    c = fops_conflicts(other()->path, names, n, first, sizeof(first));
    free(names);
    if (c) {
        g_q = kind == FJOB_COPY ? Q_REPLACE_COPY : Q_REPLACE_MOVE;
        if (c == 1) snprintf(g_qtext, sizeof(g_qtext), "\"%s\" is already there. Replace it?", first);
        else snprintf(g_qtext, sizeof(g_qtext), "%d of these are already there. Replace them?", c);
        g_over = O_CONFIRM;
        return;
    }
    job_begin(kind);
}

/* ---- files on the PSP's stick, opened here ------------------------------------------- */

#define VIEW_DIR "/tmp/pspexp-files-view"
#define VIEW_MAX (16u * 1024u * 1024u)

/* a path this board can open for the entry name of panel p: its own, or for
 * the PSP's stick a copy fetched to VIEW_DIR (the previous one removed). 0,
 * or -1 with the reason in err */
static int local_path(const Panel *p, const FEntry *e, char *out, size_t n, char *err, size_t en)
{
    static char last[FOPS_PATH];
    FJob j;
    if (!fops_is_remote(p->path)) { fops_join(out, n, p->path, e->name); return 0; }
    if (e->size > VIEW_MAX) { snprintf(err, en, "It is too large to open from the PSP (over 16 MB): copy it to the board first."); return -1; }
    fops_join(out, n, VIEW_DIR, e->name);
    if (!strcmp(out, last)) return 0;   /* fetched already (a picture shown again) */
    if (last[0]) unlink(last);
    last[0] = 0;
    mkdir(VIEW_DIR, 0700);
    memset(&j, 0, sizeof(j));
    j.kind = FJOB_COPY;
    snprintf(j.src, sizeof(j.src), "%s", p->path);
    snprintf(j.dst, sizeof(j.dst), "%s", VIEW_DIR);
    if (!(j.names = calloc(1, 256))) { snprintf(err, en, "out of memory"); return -1; }
    snprintf(j.names[0], 256, "%s", e->name);
    j.nnames = 1;
    fjob_run(&j);   /* here: the link is this thread's */
    fjob_finish(&j);
    if (j.error[0]) { snprintf(err, en, "%s", j.error); return -1; }
    snprintf(last, sizeof(last), "%s", out);
    return 0;
}

/* ---- the text viewer ------------------------------------------------------------ */

static void view_free(void)
{
    int i;
    for (i = 0; i < g_vn; i++) free(g_vlines[i]);
    free(g_vlines); g_vlines = NULL; g_vn = 0; g_vtop = 0;
}

static void view_add(const char *s, size_t n)
{
    char **nl;
    if (g_vn % 256 == 0 && !(nl = realloc(g_vlines, sizeof(char *) * (size_t)(g_vn + 256)))) return;
    else if (g_vn % 256 == 0) g_vlines = nl;
    g_vlines[g_vn] = malloc(n + 1);
    if (!g_vlines[g_vn]) return;
    memcpy(g_vlines[g_vn], s, n); g_vlines[g_vn][n] = 0;
    g_vn++;
}

/* the first 256 KB, wrapped to the screen's width; NUL bytes: not text */
static int view_open(const char *path, const char *name)
{
    enum { MAX = 256 * 1024 };
    FILE *f = fopen(path, "rb");
    char *buf, line[512];
    size_t n, i, l = 0;
    int w = 0, maxw = UI_SCREEN_W - 32;
    if (!f) return -1;
    buf = malloc(MAX);
    if (!buf) { fclose(f); return -1; }
    n = fread(buf, 1, MAX, f);
    fclose(f);
    if (memchr(buf, 0, n < 8192 ? n : 8192)) { free(buf); return 1; }   /* binary */
    view_free();
    for (i = 0; i <= n; i++) {
        char c = i < n ? buf[i] : '\n';
        if (c == '\r') continue;
        if (c == '\t') { int k; for (k = 0; k < 4 && l < sizeof(line) - 1; k++) line[l++] = ' '; w += 4 * 5; continue; }
        if (c == '\n' || l >= sizeof(line) - 1) { view_add(line, l); l = 0; w = 0; if (c == '\n') continue; }
        {   /* wrap at the screen's width (approximate: each character measured alone) */
            char one[2] = { c, 0 };
            int cw = gfx_text_width(FONT_SMALL, one);
            if (w + cw > maxw) { view_add(line, l); l = 0; w = 0; }
            line[l++] = c; w += cw;
        }
    }
    free(buf);
    snprintf(g_vtitle, sizeof(g_vtitle), "%s", name);
    g_over = O_VIEW;
    return 0;
}

static const char *icon_for(const FEntry *e);

static int is_image(const FEntry *e) { return !e->is_dir && !strcmp(icon_for(e), "file-image"); }

/* the picture at index i of the active panel, full screen */
static int image_show(int i)
{
    Panel *p = act();
    char path[FOPS_PATH], err[160];
    if (local_path(p, &p->list.e[i], path, sizeof(path), err, sizeof(err)) < 0) return -1;
    if (gfx_image_load(path, UI_SCREEN_W, UI_SCREEN_H, &g_img_w, &g_img_h) < 0) return -1;
    g_img_idx = i;
    p->sel = i;   /* back in the panel, the highlight is on the last one shown */
    g_over = O_IMAGE;
    g_dirty = 1;
    return 0;
}

/* the next (d = 1) or previous (-1) picture in the folder, wrapping */
static void image_step(int d)
{
    Panel *p = act();
    int i = g_img_idx, k;
    for (k = 0; k < p->list.n; k++) {
        i = (i + d + p->list.n) % p->list.n;
        if (is_image(&p->list.e[i]) && image_show(i) == 0) return;
    }
}

static int is_package(const FEntry *e)
{
    size_t l = strlen(e->name);
    return !e->is_dir && ((l > 7 && !strcasecmp(e->name + l - 7, ".tar.gz")) || (l > 4 && !strcasecmp(e->name + l - 4, ".tgz")));
}

static void install_start(void)
{
    pid_t pid;
    unlink(INSTALL_LOG);
    pid = fork();
    if (pid == 0) {
        int fd = open(INSTALL_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644), i;
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        for (i = 3; i < 1024; i++) close(i);   /* not the PSP link */
        execlp("pspexp-pkg", "pspexp-pkg", "install", g_inst_file, (char *)NULL);
        fprintf(stderr, "pspexp-pkg is not installed on this board\n");
        _exit(127);
    }
    if (pid < 0) {
        snprintf(g_msg_title, sizeof(g_msg_title), "Could not install");
        snprintf(g_msg, sizeof(g_msg), "%s", strerror(errno));
        g_over = O_MESSAGE;
        return;
    }
    g_inst_pid = pid;
    g_over = O_INSTALLING;
}

/* finished: what pspexp-pkg said, its last lines */
static void install_done(int status)
{
    char out[600] = "";
    FILE *f = fopen(INSTALL_LOG, "r");
    if (f) { size_t n = fread(out, 1, sizeof(out) - 1, f); out[n] = 0; fclose(f); }
    while (strlen(out) && (out[strlen(out) - 1] == '\n')) out[strlen(out) - 1] = 0;
    g_inst_pid = 0;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        snprintf(g_msg_title, sizeof(g_msg_title), "Installed");
        snprintf(g_msg, sizeof(g_msg), "%s\nThe launcher shows it in a moment.", out[0] ? out : g_inst_name);
    } else {
        const char *last = strrchr(out, '\n'), *m = last ? last + 1 : out[0] ? out : "pspexp-pkg failed", *at;
        char nice[600];
        if (!strncmp(m, "pspexp-pkg: ", 12)) m += 12;
        /* the file by its name, not its whole path */
        if ((at = strstr(m, g_inst_file))) snprintf(nice, sizeof(nice), "%.*s%s%s", (int)(at - m), m, g_inst_name, at + strlen(g_inst_file));
        else snprintf(nice, sizeof(nice), "%s", m);
        snprintf(g_msg_title, sizeof(g_msg_title), "Not installed");
        snprintf(g_msg, sizeof(g_msg), "%s", nice);
    }
    g_over = O_MESSAGE;
    reload_all();
}

/* a file is text by its name, or when its start has no NUL */
static void open_file(Panel *p, const FEntry *e)
{
    char path[FOPS_PATH], err[300];
    int r;
    if (fops_is_remote(p->path) && !is_image(e) && (is_package(e) || launcher_system_for(e->name, err, 16))) {
        snprintf(g_msg_title, sizeof(g_msg_title), "%s", e->name);
        snprintf(g_msg, sizeof(g_msg), "This is on the PSP's memory stick: copy it to the board first (triangle, then Copy), and open it there.");
        g_over = O_MESSAGE;
        return;
    }
    if (local_path(p, e, path, sizeof(path), err, sizeof(err)) < 0) {
        snprintf(g_msg_title, sizeof(g_msg_title), "%s", e->name);
        snprintf(g_msg, sizeof(g_msg), "%s", err);
        g_over = O_MESSAGE;
        return;
    }
    if (launcher_open_file(path)) return;   /* a ROM: its emulator, as from the launcher */
    if (is_package(e)) {   /* a PSPExp package: install it, after asking */
        snprintf(g_inst_file, sizeof(g_inst_file), "%s", path);
        snprintf(g_inst_name, sizeof(g_inst_name), "%s", e->name);
        snprintf(g_qtext, sizeof(g_qtext), "Install \"%s\" as a PSPExp package? An older version of it is replaced.", e->name);
        g_q = Q_INSTALL; g_over = O_CONFIRM;
        return;
    }
    if (is_image(e)) {
        if (image_show((int)(e - p->list.e)) == 0) return;
        snprintf(g_msg_title, sizeof(g_msg_title), "%s", e->name);
        snprintf(g_msg, sizeof(g_msg), "This picture could not be read (PNG, JPEG, GIF, BMP and TGA can).");
        g_over = O_MESSAGE;
        return;
    }
    r = view_open(path, e->name);
    if (r == 0) return;
    snprintf(g_msg_title, sizeof(g_msg_title), "%s", e->name);
    if (r > 0) {
        char sz[32];
        fops_human(e->size, sz, sizeof(sz));
        snprintf(g_msg, sizeof(g_msg), "Not a text file (%s), and no installed system plays it.", sz);
    } else snprintf(g_msg, sizeof(g_msg), "Cannot read it: %s", strerror(errno));
    g_over = O_MESSAGE;
}

/* ---- menus ------------------------------------------------------------------- */

typedef struct MItem { const char *icon, *label; int id; } MItem;
enum { A_COPY, A_MOVE, A_DELETE, A_RENAME, A_MKDIR, A_DETAILS,
       P_HIDDEN, P_SORT, P_SAME, P_SWAP, P_TERMINAL, P_CLOSE };
static const MItem ACTIONS[] = {
    { "copy", "Copy to the other panel", A_COPY }, { "folder-input", "Move to the other panel", A_MOVE },
    { "trash-2", "Delete", A_DELETE }, { "pencil", "Rename", A_RENAME },
    { "folder-plus", "New folder", A_MKDIR }, { "info", "Details", A_DETAILS },
};
static const MItem OPTIONS[] = {
    { "eye", "Hidden files", P_HIDDEN }, { "arrow-up-down", "Sort", P_SORT },
    { "columns-2", "This folder in the other panel", P_SAME }, { "arrow-right-left", "Swap the panels", P_SWAP },
    { "square-terminal", "Terminal here", P_TERMINAL }, { "x", "Close the file manager", P_CLOSE },
};
#define NACT ((int)(sizeof(ACTIONS) / sizeof(ACTIONS[0])))
#define NOPT ((int)(sizeof(OPTIONS) / sizeof(OPTIONS[0])))

static void details(void)
{
    Panel *p = act();
    const FEntry *e;
    char sz[32], when[64], path[FOPS_PATH];
    struct tm tmv;
    if (p->sel >= p->list.n || p->list.e[p->sel].is_up) return;
    e = &p->list.e[p->sel];
    fops_join(path, sizeof(path), p->path, e->name);
    fops_human(e->size, sz, sizeof(sz));
    localtime_r(&e->mtime, &tmv);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tmv);
    snprintf(g_msg_title, sizeof(g_msg_title), "%s", e->name);
    if (fops_is_remote(p->path))
        snprintf(g_msg, sizeof(g_msg), "%s on the PSP's memory stick\n%s\nchanged %s", e->is_dir ? "folder" : sz, path, when);
    else snprintf(g_msg, sizeof(g_msg), "%s%s\n%s\nchanged %s\npermissions %03o", e->is_dir ? "folder" : sz,
                  e->is_link ? " (a link)" : "", path, when, (unsigned)(e->mode & 0777));
    g_over = O_MESSAGE;
}

/* the drives the tabs show: this board's, then the PSP's memory stick */
static FDrive g_drv[FOPS_DRIVES + 1];
static int g_ndrv;

static void drives_refresh(void)
{
    g_ndrv = fops_board_drives(g_drv, FOPS_DRIVES, NULL);
    snprintf(g_drv[g_ndrv].root, sizeof(g_drv[g_ndrv].root), "ms0:/");
    snprintf(g_drv[g_ndrv].name, sizeof(g_drv[g_ndrv].name), "PSP");
    g_drv[g_ndrv].icon = "memory-stick";
    g_ndrv++;
}

static int drive_is_psp(int i) { return fops_is_remote(g_drv[i].root); }

/* the tab of the panel's folder */
static int drive_cur(const Panel *p)
{
    if (fops_is_remote(p->path)) return g_ndrv - 1;
    { int i = fops_drive_of(g_drv, g_ndrv - 1, p->path); return i < 0 ? 0 : i; }
}

/* the panel onto drive i, where it last was on it (its top the first time,
 * or when that folder is gone) */
static void drive_go(Panel *p, int i)
{
    int cur = drive_cur(p), k, slot = -1;
    if (i < 0 || i >= g_ndrv || i == cur) return;
    if (drive_is_psp(i) && !files_psp_available()) {
        snprintf(g_msg_title, sizeof(g_msg_title), "The PSP's memory stick");
        snprintf(g_msg, sizeof(g_msg), "The PSP does not share its memory stick yet: its PSPExp runtime is older (or the PSP is not connected). Copy the new one (dist/PSPExp) to ms0:/PSP/GAME/PSPExp/ and start it again.");
        g_over = O_MESSAGE;
        return;
    }
    /* remember this drive's folder */
    for (k = 0; k < p->nmem; k++) if (!strcmp(p->mem_root[k], g_drv[cur].root)) { slot = k; break; }
    if (slot < 0 && p->nmem < FOPS_DRIVES + 1) slot = p->nmem++;
    if (slot >= 0) { snprintf(p->mem_root[slot], FOPS_PATH, "%s", g_drv[cur].root); snprintf(p->mem_path[slot], FOPS_PATH, "%s", p->path); }
    for (k = 0; k < p->nmem; k++)
        if (!strcmp(p->mem_root[k], g_drv[i].root)) {
            panel_cd(p, p->mem_path[k], NULL);
            if (drive_cur(p) == i && !p->error[0]) return;
            g_over = O_NONE;   /* gone: the drive's top instead */
        }
    panel_cd(p, g_drv[i].root, NULL);
}

static void run_action(int id)
{
    Panel *p = act();
    g_over = O_NONE;
    switch (id) {
    case A_COPY: transfer(FJOB_COPY); break;
    case A_MOVE: transfer(FJOB_MOVE); break;
    case A_DELETE: {
        int n; char (*names)[256] = targets(&n);
        if (!n) break;
        if (n == 1) snprintf(g_qtext, sizeof(g_qtext), "Delete \"%s\"%s?", names[0], p->list.e[p->sel].is_dir && n == 1 ? " and everything in it" : "");
        else snprintf(g_qtext, sizeof(g_qtext), "Delete these %d items?", n);
        free(names);
        g_q = Q_DELETE; g_over = O_CONFIRM;
        break;
    }
    case A_RENAME:
        if (p->sel < p->list.n && !p->list.e[p->sel].is_up) { ui_kbd_reset(&g_kbd, p->list.e[p->sel].name, 0, "Rename"); g_kmode = K_RENAME; g_over = O_KBD; }
        break;
    case A_MKDIR: ui_kbd_reset(&g_kbd, "", 0, "Create"); g_kmode = K_MKDIR; g_over = O_KBD; break;
    case A_DETAILS: details(); break;
    case P_HIDDEN: g_hidden = !g_hidden; reload_all(); break;
    case P_SORT: g_sort = (g_sort + 1) % 3; reload_all(); break;
    case P_SAME: panel_cd(other(), p->path, NULL); break;
    case P_SWAP: { Panel t = g_p[0]; g_p[0] = g_p[1]; g_p[1] = t; g_act = !g_act; break; }
    case P_TERMINAL:
        if (fops_is_remote(p->path)) {
            snprintf(g_msg_title, sizeof(g_msg_title), "Terminal here");
            snprintf(g_msg, sizeof(g_msg), "The terminal works in this board's folders, not on the PSP's memory stick.");
            g_over = O_MESSAGE;
        } else { g_term = 1; snprintf(g_term_path, sizeof(g_term_path), "%s", p->path); }
        break;
    case P_CLOSE: g_exit = 1; break;
    }
    g_dirty = 1;
}

static void kbd_done(void)
{
    Panel *p = act();
    char path[FOPS_PATH], err[160];
    const char *name = g_kbd.buf;
    g_over = O_NONE;
    if (!fops_name_ok(name)) {
        snprintf(g_msg_title, sizeof(g_msg_title), "Not a usable name");
        snprintf(g_msg, sizeof(g_msg), "A name cannot contain / or be . or ..");
        g_over = O_MESSAGE;
        return;
    }
    fops_join(path, sizeof(path), p->path, name);
    if (g_kmode == K_MKDIR) {
        if (fops_mkdir(path, err, sizeof(err)) == 0) panel_load(p, name);
        else { snprintf(g_msg_title, sizeof(g_msg_title), "Could not create it"); snprintf(g_msg, sizeof(g_msg), "%s", err); g_over = O_MESSAGE; }
    } else if (p->sel < p->list.n) {
        char from[FOPS_PATH];
        fops_join(from, sizeof(from), p->path, p->list.e[p->sel].name);
        if (!strcmp(p->list.e[p->sel].name, name)) return;
        if (fops_rename(from, path, err, sizeof(err)) == 0) panel_load(p, name);
        else { snprintf(g_msg_title, sizeof(g_msg_title), "Could not rename it"); snprintf(g_msg, sizeof(g_msg), "%s", err); g_over = O_MESSAGE; }
    }
    if (strcmp(other()->path, p->path) == 0) panel_load(other(), NULL);
}

/* ---- input ------------------------------------------------------------------- */

static void move_sel(Panel *p, int d)
{
    if (!p->list.n) return;
    p->sel += d;
    if (p->sel < 0) p->sel = 0;
    if (p->sel >= p->list.n) p->sel = p->list.n - 1;
    g_dirty = 1;
}

void files_input(uint32_t b, uint32_t pressed, int up, int down, int left, int right, uint64_t now)
{
    Panel *p = act();
    (void)b; (void)now;
    if (launcher_running_title()) return;   /* a program is starting: nothing else meanwhile */
    switch (g_over) {
    case O_ACTIONS: case O_OPTIONS: {
        int n = g_over == O_ACTIONS ? NACT : NOPT;
        if (up) { g_msel = (g_msel + n - 1) % n; g_dirty = 1; }
        if (down) { g_msel = (g_msel + 1) % n; g_dirty = 1; }
        if (pressed & PSPEXP_BUTTON_CROSS) run_action(g_over == O_ACTIONS ? ACTIONS[g_msel].id : OPTIONS[g_msel].id);
        else if (pressed & (PSPEXP_BUTTON_CIRCLE | PSPEXP_BUTTON_TRIANGLE | PSPEXP_BUTTON_START)) { g_over = O_NONE; g_dirty = 1; }
        return;
    }
    case O_DRIVES:   /* the drop-down: every drive */
        if (up) g_msel = (g_msel + g_ndrv - 1) % g_ndrv;
        if (down) g_msel = (g_msel + 1) % g_ndrv;
        if (pressed & PSPEXP_BUTTON_CROSS) { g_over = O_NONE; p->on_drives = 0; drive_go(p, g_msel); }
        else if (pressed & (PSPEXP_BUTTON_CIRCLE | PSPEXP_BUTTON_TRIANGLE | PSPEXP_BUTTON_START)) g_over = O_NONE;
        g_dirty = 1;
        return;
    case O_CONFIRM:
        if (pressed & PSPEXP_BUTTON_CROSS) {
            g_over = O_NONE;
            if (g_q == Q_INSTALL) install_start();
            else job_begin(g_q == Q_DELETE ? FJOB_DELETE : g_q == Q_REPLACE_COPY ? FJOB_COPY : FJOB_MOVE);
        } else if (pressed & PSPEXP_BUTTON_CIRCLE) g_over = O_NONE;
        g_dirty = 1;
        return;
    case O_PROGRESS:
        if (pressed & PSPEXP_BUTTON_CIRCLE) g_job.cancel = 1;
        return;
    case O_INSTALLING:
        return;   /* it finishes by itself */
    case O_MESSAGE:
        if (pressed & (PSPEXP_BUTTON_CROSS | PSPEXP_BUTTON_CIRCLE)) { g_over = O_NONE; g_dirty = 1; }
        return;
    case O_KBD: {
        int r = ui_kbd_input(&g_kbd, pressed, up, down, left, right);
        if (r == UIK_DONE) kbd_done();
        else if (r == UIK_CANCEL) g_over = O_NONE;
        g_dirty = 1;
        return;
    }
    case O_IMAGE:
        if (left) image_step(-1);
        if (right) image_step(1);
        if (pressed & (PSPEXP_BUTTON_CIRCLE | PSPEXP_BUTTON_CROSS)) { gfx_image_free(); g_over = O_NONE; g_dirty = 1; }
        return;
    case O_VIEW: {
        int page = 14;
        if (up && g_vtop > 0) g_vtop--;
        if (down && g_vtop < g_vn - 1) g_vtop++;
        if (left) g_vtop = g_vtop > page ? g_vtop - page : 0;
        if (right) g_vtop = g_vtop + page < g_vn ? g_vtop + page : (g_vn ? g_vn - 1 : 0);
        if (pressed & (PSPEXP_BUTTON_CIRCLE | PSPEXP_BUTTON_CROSS)) { view_free(); g_over = O_NONE; }
        g_dirty = 1;
        return;
    }
    }
    if (pressed & PSPEXP_BUTTON_LTRIGGER) { g_act = 0; g_dirty = 1; }
    if (pressed & PSPEXP_BUTTON_RTRIGGER) { g_act = 1; g_dirty = 1; }
    p = act();
    if (p->on_drives) {   /* the drive button: cross opens the list, down / circle back to the files */
        if (pressed & PSPEXP_BUTTON_CROSS) { drives_refresh(); g_msel = drive_cur(p); g_over = O_DRIVES; }
        else if (down || (pressed & PSPEXP_BUTTON_CIRCLE)) p->on_drives = 0;
        if (pressed & PSPEXP_BUTTON_TRIANGLE) { p->on_drives = 0; g_over = O_ACTIONS; g_msel = 0; }
        if (pressed & PSPEXP_BUTTON_START) { g_over = O_OPTIONS; g_msel = 0; }
        if (pressed & PSPEXP_BUTTON_SELECT) g_exit = 1;
        g_dirty = 1;
        return;
    }
    if (up && p->sel == 0) { drives_refresh(); p->on_drives = 1; g_dirty = 1; return; }   /* above the list: the drive button */
    if (up) move_sel(p, -1);
    if (down) move_sel(p, 1);
    if (left) move_sel(p, -(ROWS - 1));
    if (right) move_sel(p, ROWS - 1);
    if (pressed & PSPEXP_BUTTON_CROSS && p->sel < p->list.n) {
        FEntry *e = &p->list.e[p->sel];
        if (e->is_up) panel_up(p);
        else if (e->is_dir) { char path[FOPS_PATH]; fops_join(path, sizeof(path), p->path, e->name); panel_cd(p, path, NULL); }
        else open_file(p, e);
    }
    if (pressed & PSPEXP_BUTTON_CIRCLE) panel_up(p);
    if ((pressed & PSPEXP_BUTTON_SQUARE) && p->sel < p->list.n) {
        if (!p->list.e[p->sel].is_up) p->list.e[p->sel].marked = !p->list.e[p->sel].marked;
        move_sel(p, 1);
    }
    if (pressed & PSPEXP_BUTTON_TRIANGLE) { g_over = O_ACTIONS; g_msel = 0; }
    if (pressed & PSPEXP_BUTTON_START) { g_over = O_OPTIONS; g_msel = 0; }
    if (pressed & PSPEXP_BUTTON_SELECT) g_exit = 1;
    g_dirty = 1;
}

int files_needs_frame(uint64_t now)
{
    {   /* a program started from here: its state shows over the panels */
        static int was;
        int is = launcher_running_title() != NULL;
        if (is != was) { was = is; g_dirty = 1; }
    }
    if (g_job_on && g_job.done) {   /* finished: collect it */
        fjob_finish(&g_job);
        g_job_on = 0;
        reload_all();
        { int k, i; for (k = 0; k < 2; k++) for (i = 0; i < g_p[k].list.n; i++) g_p[k].list.e[i].marked = 0; }
        if (g_job.error[0]) {
            snprintf(g_msg_title, sizeof(g_msg_title), "%s", strcmp(g_job.error, "cancelled") ? "It did not finish" : "Cancelled");
            snprintf(g_msg, sizeof(g_msg), "%s", g_job.error);
            g_over = O_MESSAGE;
        } else g_over = O_NONE;
        g_dirty = 1;
    }
    {   /* a panel on the PSP's stick while the PSP was away: read it again when it is back */
        static int had;
        int has = files_psp_available(), k;
        if (has && !had) for (k = 0; k < 2; k++) if (fops_is_remote(g_p[k].path) && g_p[k].error[0]) panel_load(&g_p[k], NULL);
        had = has;
    }
    if (g_inst_pid > 0) {
        int st;
        if (waitpid(g_inst_pid, &st, WNOHANG) == g_inst_pid) { install_done(st); g_dirty = 1; }
    }
    if (g_over == O_PROGRESS) return 1;
    if (g_over == O_KBD && (int)((now / 530000u) & 1) != g_caret) { g_caret = (int)((now / 530000u) & 1); g_dirty = 1; }
    return g_dirty;
}

/* ---- drawing ------------------------------------------------------------------ */

static const char *ext_of(const char *name) { const char *d = strrchr(name, '.'); return d && d != name ? d + 1 : ""; }
static int ext_in(const char *e, const char *list)   /* list: "png jpg gif" */
{
    size_t l = strlen(e);
    const char *p = list;
    if (!l) return 0;
    while ((p = strstr(p, e))) {
        if ((p == list || p[-1] == ' ') && (p[l] == ' ' || !p[l]) && !strncasecmp(p, e, l)) return 1;
        p += l;
    }
    { char low[16]; size_t i; for (i = 0; i < l && i < 15; i++) low[i] = (char)tolower((unsigned char)e[i]); low[i] = 0;
      if (strcmp(low, e)) return ext_in(low, list); }
    return 0;
}

static const char *icon_for(const FEntry *e)
{
    const char *x = ext_of(e->name);
    if (e->is_up) return "corner-left-up";
    if (e->is_dir) return e->is_link ? "folder-symlink" : "folder";
    if (ext_in(x, "png jpg jpeg gif bmp webp tga")) return "file-image";
    if (ext_in(x, "zip gz tgz xz bz2 7z tar rar zst")) return "file-archive";
    if (ext_in(x, "mp3 ogg wav flac mod xm it s3m opus m4a")) return "file-music";
    if (ext_in(x, "nes sfc smc gb gbc gba gen smd sms gg pce")) return "gamepad-2";   /* not .md: Markdown far more often */
    if (ext_in(x, "c h cpp hpp py sh js json go rs lua mk cmake")) return "file-code";
    if (ext_in(x, "txt md log conf cfg ini entry system tab csv xml html")) return "file-text";
    if (ext_in(x, "mp4 mkv avi webm mov")) return "monitor-play";
    return "file";
}

static void draw_panel(int k)
{
    Panel *p = &g_p[k];
    int x = 8 + k * (PANEL_W + PANEL_GAP), active = k == g_act, i, marked = 0;
    uint64_t total = 0;
    const char *base = strrchr(p->path, '/');
    int stick = fops_is_remote(p->path);
    char s[96];
    base = base && base[1] ? base + 1 : stick ? "" : p->path;
    gfx_nine(NINE_R10, x, PANEL_Y, PANEL_W, PANEL_H, UI_TILE, active ? 235 : 190);
    if (active) gfx_nine(NINE_R10, x, PANEL_Y, PANEL_W, 3, UI_ACCENT, 255);
    /* the folder: its name, then its path */
    {   /* the drive button: the panel's drive, its list below it on cross */
        int cur = drive_cur(p), focus = active && p->on_drives, bw = 40 + gfx_text_width(FONT_SMALL, g_drv[cur].name), tx;
        if (bw > 130) bw = 130;
        gfx_nine(NINE_R6, x + 6, PANEL_Y + 5, bw, 17, focus ? UI_ACCENT : 0xffffff, focus ? 200 : 26);
        gfx_icon(g_drv[cur].icon, 12, x + 12, PANEL_Y + 8, focus ? 0xffffff : active ? UI_ACCENT : UI_SUB, 255);
        gfx_text(FONT_SMALL, x + 28, PANEL_Y + 6, g_drv[cur].name, focus ? 0xffffff : UI_TEXT, 255, bw - 40);
        gfx_icon("chevron-down", 10, x + 6 + bw - 15, PANEL_Y + 9, focus ? 0xffffff : UI_MUTED, 255);
        tx = x + 6 + bw + 8;
        /* the folder's name after it (the path below has it too) */
        if (strcmp(p->path, g_drv[cur].root))
            gfx_text(FONT_SMALL, tx, PANEL_Y + 6, base, active ? UI_SUB : UI_MUTED, 255, x + PANEL_W - 8 - tx);
    }
    {   /* the path, its end kept when it does not fit */
        const char *shown = p->path;
        char cut[FOPS_PATH + 4];
        while (gfx_text_width(FONT_BADGE, shown) > PANEL_W - 28 && strlen(shown) > 4) {
            shown++;
            snprintf(cut, sizeof(cut), "...%s", shown);
            if (gfx_text_width(FONT_BADGE, cut) <= PANEL_W - 20) { shown = cut; break; }
        }
        gfx_text(FONT_BADGE, x + 10, PANEL_Y + 23, shown, UI_MUTED, 255, PANEL_W - 20);
    }
    if (p->error[0]) { gfx_text(FONT_SMALL, x + 10, LIST_Y + 4, p->error, 0xff8080, 255, PANEL_W - 20); return; }
    /* keep the highlight in view */
    if (p->sel < p->top) p->top = p->sel;
    if (p->sel >= p->top + ROWS) p->top = p->sel - ROWS + 1;
    gfx_set_clip(x, LIST_Y, PANEL_W, ROWS * ROW_H);
    for (i = 0; i < ROWS && p->top + i < p->list.n; i++) {
        const FEntry *e = &p->list.e[p->top + i];
        int y = LIST_Y + i * ROW_H, sel = p->top + i == p->sel;
        uint32_t tc = e->marked ? 0xffd479 : e->is_dir ? UI_TEXT : UI_SUB;
        if (sel && active && !p->on_drives) gfx_nine(NINE_R6, x + 3, y, PANEL_W - 6, ROW_H, UI_ACCENT, 70);
        else if (sel) gfx_nine(NINE_R6, x + 3, y, PANEL_W - 6, ROW_H, 0xffffff, 18);
        gfx_icon(icon_for(e), 12, x + 8, y + 2, e->marked ? 0xffd479 : e->is_dir ? UI_ACCENT : UI_MUTED, 255);
        if (!e->is_dir && !e->is_up) {
            fops_human(e->size, s, sizeof(s));
            gfx_text(FONT_BADGE, x + PANEL_W - 10 - gfx_text_width(FONT_BADGE, s), y + 3, s, UI_MUTED, 255, 0);
        }
        gfx_text(FONT_SMALL, x + 26, y + 1, e->is_up ? ".." : e->name, sel && active ? 0xffffff : tc, 255, PANEL_W - 26 - 58);
    }
    gfx_set_clip(0, 0, UI_SCREEN_W, UI_SCREEN_H);
    /* the foot: how many, marked, their size */
    for (i = 0; i < p->list.n; i++) if (p->list.e[i].marked) { marked++; if (!p->list.e[i].is_dir) total += p->list.e[i].size; }
    if (marked && total) { char sz[32]; fops_human(total, sz, sizeof(sz)); snprintf(s, sizeof(s), "%d marked, files %s", marked, sz); }
    else if (marked) snprintf(s, sizeof(s), "%d marked", marked);
    else { int n = p->list.n - (p->list.n && p->list.e[0].is_up); snprintf(s, sizeof(s), "%d %s%s%s", n, n == 1 ? "item" : "items", p->space[0] ? "   " : "", p->space); }
    gfx_text(FONT_BADGE, x + 10, PANEL_Y + PANEL_H - 14, s, marked ? 0xffd479 : UI_MUTED, 255, 0);
}

static void draw_menu(const MItem *items, int n, const char *title)
{
    int w = 250, h = 30 + n * 20, x = (UI_SCREEN_W - w) / 2, y = (UI_SCREEN_H - h) / 2 - 8, i;
    gfx_shadow(NINE_SHADOW14, x, y, w, h, 0x000000, 160);
    gfx_nine(NINE_R14, x, y, w, h, 0x232a3b, 250);
    gfx_text(FONT_BODY, x + 14, y + 8, title, UI_TEXT, 255, w - 28);
    for (i = 0; i < n; i++) {
        int ry = y + 28 + i * 20;
        char label[96];
        snprintf(label, sizeof(label), "%s", items[i].label);
        if (items[i].id == P_HIDDEN) snprintf(label, sizeof(label), "Hidden files: %s", g_hidden ? "shown" : "hidden");
        if (items[i].id == P_SORT) snprintf(label, sizeof(label), "Sort by %s", g_sort == FSORT_NAME ? "name" : g_sort == FSORT_SIZE ? "size" : "date");
        if (i == g_msel) gfx_nine(NINE_R6, x + 6, ry - 2, w - 12, 19, UI_ACCENT, 90);
        gfx_icon(items[i].icon, 13, x + 14, ry + 1, i == g_msel ? 0xffffff : UI_ACCENT, 255);
        gfx_text(FONT_SMALL, x + 36, ry, label, i == g_msel ? 0xffffff : UI_TEXT, 255, w - 48);
    }
}

/* the drive list, dropped from the active panel's drive button */
static void draw_drives(void)
{
    int x = 8 + g_act * (PANEL_W + PANEL_GAP) + 6, y = PANEL_Y + 24, w = PANEL_W - 12, rh = 30, h = 8 + g_ndrv * rh, i;
    int cur = drive_cur(act());
    gfx_shadow(NINE_SHADOW14, x, y, w, h, 0x000000, 160);
    gfx_nine(NINE_R10, x, y, w, h, 0x232a3b, 250);
    for (i = 0; i < g_ndrv; i++) {
        int ry = y + 4 + i * rh, sel = i == g_msel, avail = !drive_is_psp(i) || files_psp_available();
        char sub[FOPS_PATH + 48];
        uint32_t tc = sel ? 0xffffff : avail ? UI_TEXT : 0x5a6275;
        if (sel) gfx_nine(NINE_R6, x + 4, ry, w - 8, rh - 2, UI_ACCENT, 90);
        gfx_icon(g_drv[i].icon, 14, x + 10, ry + 7, sel ? 0xffffff : avail ? UI_ACCENT : 0x5a6275, 255);
        gfx_text(FONT_SMALL, x + 32, ry + 1, g_drv[i].name, tc, 255, w - 60);
        if (i == cur) gfx_icon("check", 12, x + w - 22, ry + 8, sel ? 0xffffff : UI_ACCENT, 255);
        /* where it is, and its free space (the board's drives: asked here, cheap) */
        if (drive_is_psp(i)) snprintf(sub, sizeof(sub), "%s", avail ? "ms0:/  the PSP's memory stick" : "not shared by the PSP right now");
        else {
            struct statvfs sv;
            char fr[32] = "";
            if (statvfs(g_drv[i].root, &sv) == 0) fops_human((uint64_t)sv.f_bavail * sv.f_frsize, fr, sizeof(fr));
            snprintf(sub, sizeof(sub), "%s%s%s%s", g_drv[i].root, fr[0] ? "  " : "", fr, fr[0] ? " free" : "");
        }
        gfx_text(FONT_BADGE, x + 32, ry + 15, sub, sel ? 0xdde3ff : UI_MUTED, 255, w - 60);
    }
}

/* a box with a title and wrapped text ('\n' starts a line) */
static void draw_box(const char *title, const char *text, int h)
{
    int w = 340, x = (UI_SCREEN_W - w) / 2, y = (UI_SCREEN_H - h) / 2 - 8, ly = y + 34;
    const char *p = text;
    gfx_shadow(NINE_SHADOW14, x, y, w, h, 0x000000, 160);
    gfx_nine(NINE_R14, x, y, w, h, 0x232a3b, 250);
    gfx_text(FONT_BODY, x + 16, y + 10, title, UI_TEXT, 255, w - 32);
    while (*p && ly < y + h - 16) {   /* word wrap */
        char line[160];
        size_t l = 0, last_space = 0;
        while (p[l] && p[l] != '\n' && l < sizeof(line) - 1) {
            line[l] = p[l]; line[l + 1] = 0;
            if (p[l] == ' ') last_space = l;
            if (gfx_text_width(FONT_SMALL, line) > w - 32 && last_space) { l = last_space; break; }
            l++;
        }
        line[l] = 0;
        gfx_text(FONT_SMALL, x + 16, ly, line, UI_SUB, 255, w - 32);
        p += l;
        while (*p == ' ' || *p == '\n') { if (*p == '\n') { p++; break; } p++; }
        ly += 15;
    }
}

void files_render(PspExpContext *ctx, uint64_t now)
{
    int x;
    (void)now;
    gfx_frame(ctx);
    pspexp_begin_frame(ctx);
    pspexp_set_clip(ctx, 0, 0, UI_SCREEN_W, UI_SCREEN_H);
    gfx_backdrop();
    if (g_over == O_KBD) {
        ui_kbd_draw(&g_kbd, g_kmode == K_MKDIR ? "folder-plus" : "pencil", g_kmode == K_MKDIR ? "New folder" : "Rename", "Type a name", g_caret, 255);
        pspexp_present(ctx);
        g_dirty = 0;
        return;
    }
    if (g_over == O_IMAGE) {
        Panel *p = act();
        int w, h, i, n = 0, at = 0;
        char info[96];
        gfx_rect(0, 0, UI_SCREEN_W, UI_SCREEN_H, 0x000000, 255);
        gfx_image_size(&w, &h);
        gfx_image_draw((UI_SCREEN_W - w) / 2, (UI_SCREEN_H - h) / 2);
        for (i = 0; i < p->list.n; i++) if (is_image(&p->list.e[i])) { n++; if (i == g_img_idx) at = n; }
        /* the name and where it is, on a band over the top */
        gfx_rect(0, 0, UI_SCREEN_W, 22, 0x000000, 150);
        gfx_icon("file-image", 12, 8, 5, UI_ACCENT, 255);
        gfx_text(FONT_SMALL, 26, 3, p->list.e[g_img_idx].name, UI_TEXT, 255, 300);
        snprintf(info, sizeof(info), "%d / %d   %d x %d", at, n, g_img_w, g_img_h);
        gfx_text(FONT_SMALL, UI_SCREEN_W - 8 - gfx_text_width(FONT_SMALL, info), 3, info, UI_SUB, 255, 0);
        gfx_rect(0, UI_HINT_Y - 4, UI_SCREEN_W, UI_SCREEN_H - UI_HINT_Y + 4, 0x000000, 150);
        x = 16;
        if (n > 1) x = ui_hint_cap(x, "LEFT/RIGHT", "Previous / next", UI_SUB);
        x = ui_hint_btn(x, 'o', "Close");
        pspexp_present(ctx);
        g_dirty = 0;
        return;
    }
    if (g_over == O_VIEW) {
        int i, rows = 14;
        char pos[48];
        gfx_icon("file-text", 16, 16, 10, UI_ACCENT, 255);
        gfx_text(FONT_TITLE, 40, 8, g_vtitle, UI_TEXT, 255, 330);
        snprintf(pos, sizeof(pos), "%d / %d", g_vn ? g_vtop + 1 : 0, g_vn);
        gfx_text(FONT_SMALL, UI_SCREEN_W - 16 - gfx_text_width(FONT_SMALL, pos), 12, pos, UI_MUTED, 255, 0);
        gfx_nine(NINE_R10, 8, 34, UI_SCREEN_W - 16, 210, 0x121724, 235);
        for (i = 0; i < rows && g_vtop + i < g_vn; i++) gfx_text(FONT_SMALL, 16, 40 + i * 14, g_vlines[g_vtop + i], UI_TEXT, 255, UI_SCREEN_W - 32);
        x = 16;
        x = ui_hint_cap(x, "UP/DOWN", "Scroll", UI_SUB);
        x = ui_hint_cap(x, "LEFT/RIGHT", "Page", UI_SUB);
        x = ui_hint_btn(x, 'o', "Close");
        pspexp_present(ctx);
        g_dirty = 0;
        return;
    }
    draw_panel(0);
    draw_panel(1);
    x = 12;
    {   /* what cross does to the highlighted entry */
        Panel *p = act();
        const char *go = "Open";
        static char play[40];
        if (p->sel < p->list.n && !p->list.e[p->sel].is_dir) {
            const FEntry *e = &p->list.e[p->sel];
            char path[FOPS_PATH], sys[16];
            fops_join(path, sizeof(path), p->path, e->name);
            if (fops_is_remote(p->path)) go = is_image(e) ? "View" : "Open";
            else if (launcher_system_for(path, sys, sizeof(sys))) { snprintf(play, sizeof(play), "Play (%s)", sys); go = play; }
            else if (is_package(e)) go = "Install";
            else if (is_image(e)) go = "View";
            else go = "Read";
        }
        if (!p->on_drives && g_over != O_DRIVES) x = ui_hint_btn(x, 'x', go);
    }
    if (g_over == O_DRIVES) {
        x = ui_hint_btn(x, 'x', "Switch");
        x = ui_hint_btn(x, 'o', "Close");
    } else if (act()->on_drives) {
        x = ui_hint_btn(x, 'x', "Drives");
        x = ui_hint_cap(x, "DOWN", "The files", UI_SUB);
    } else {
        x = ui_hint_btn(x, 'o', "Up");
        x = ui_hint_btn(x, 's', "Mark");
        x = ui_hint_btn(x, 't', "Actions");
    }
    x = ui_hint_cap(x, "START", "Options", UI_SUB);
    x = ui_hint_cap(x, "L/R", "Panel", UI_SUB);
    switch (g_over) {
    case O_DRIVES: draw_drives(); break;
    case O_ACTIONS: draw_menu(ACTIONS, NACT, "Actions"); break;
    case O_OPTIONS: draw_menu(OPTIONS, NOPT, "Options"); break;
    case O_CONFIRM:
        draw_box(g_q == Q_DELETE ? "Delete" : g_q == Q_INSTALL ? "Install" : "Replace", g_qtext, 96);
        { int bx = (UI_SCREEN_W - 340) / 2 + 16, by = (UI_SCREEN_H - 96) / 2 - 8 + 70;
          gfx_button('x', bx, by); gfx_text(FONT_SMALL, bx + 18, by - 1, g_q == Q_DELETE ? "Delete" : g_q == Q_INSTALL ? "Install" : "Replace", UI_TEXT, 255, 0);
          gfx_button('o', bx + 110, by); gfx_text(FONT_SMALL, bx + 128, by - 1, "Keep", UI_TEXT, 255, 0); }
        break;
    case O_PROGRESS: {
        int w = 340, h = 100, bx = (UI_SCREEN_W - w) / 2, by = (UI_SCREEN_H - h) / 2 - 8;
        uint64_t done = g_job.bytes_done, tot = g_job.bytes_total;
        int pct = tot ? (int)(done * 100 / tot) : (g_job.files_total ? g_job.files_done * 100 / g_job.files_total : 0);
        char a[32], t[32], line[96];
        draw_box(g_job.kind == FJOB_COPY ? "Copying" : g_job.kind == FJOB_MOVE ? "Moving" : "Deleting", g_job.current, h);
        gfx_nine(NINE_R6, bx + 16, by + 56, w - 32, 10, 0x151a26, 255);
        if (pct > 0) gfx_nine(NINE_R6, bx + 16, by + 56, (w - 32) * (pct > 100 ? 100 : pct) / 100, 10, UI_ACCENT, 255);
        fops_human(done, a, sizeof(a)); fops_human(tot, t, sizeof(t));
        snprintf(line, sizeof(line), "%d / %d files   %s / %s", g_job.files_done, g_job.files_total, a, t);
        gfx_text(FONT_BADGE, bx + 16, by + 72, line, UI_MUTED, 255, 0);
        gfx_button('o', bx + w - 80, by + 72); gfx_text(FONT_SMALL, bx + w - 62, by + 71, "Cancel", UI_TEXT, 255, 0);
        break;
    }
    case O_INSTALLING: {
        char s[320];
        snprintf(s, sizeof(s), "%s", g_inst_name);
        draw_box("Installing", s, 64);
        break;
    }
    case O_MESSAGE:
        draw_box(g_msg_title, g_msg, 110);
        { int bx = (UI_SCREEN_W - 340) / 2 + 340 - 70, by = (UI_SCREEN_H - 110) / 2 - 8 + 88;
          gfx_button('x', bx, by); gfx_text(FONT_SMALL, bx + 18, by - 1, "OK", UI_TEXT, 255, 0); }
        break;
    }
    if (launcher_running_title()) {
        char s[160];
        snprintf(s, sizeof(s), "%s is starting; the PSP shows it when it is ready.", launcher_running_title());
        draw_box("Starting", s, 70);
    }
    pspexp_present(ctx);
    g_dirty = 0;
}
