/*
 * pspexp-term launcher view (see launcher.h). Layout at 480 x 272:
 *
 *   status    y 4..24     clock and date; Wi-Fi and battery
 *   rail      y 28..86    one tile per category (Recent, Favourites, Games,
 *                         each emulated system with ROMs, Apps, System)
 *   header    y 88..108   the category's name and size
 *   cover     x 16, y 112 128 x 128: box art or a generated cover
 *   list      x 160..464  the category's entries; the focused row expands
 *                         with a detail line
 *   hints     y 250..268  what the buttons do
 *
 * The categories are built from the library (launcher_lib.c), never from
 * folders. Every movement is eased by time; a still screen sends nothing.
 */
#include "launcher.h"
#include "launcher_lib.h"
#include "launcher_gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#define SCREEN_W 480
#define SCREEN_H 272

/* palette */
#define C_TEXT   0xf1f4f9
#define C_SUB    0xa9b2c3
#define C_MUTED  0x7d869a
#define C_TILE   0x1b2131
#define C_ACCENT 0x8b9bff

/* layout */
#define RAIL_Y      56      /* tile centre */
#define TILE        46
#define TILE_SEL    58
#define TILE_GAP    12
#define HEAD_Y      90
#define LIST_X      160
#define LIST_W      300
#define LIST_Y      112
#define LIST_H      128
#define ROW         22
#define ROW_EXTRA   17
#define COVER_X     16
#define COVER_Y     112
#define HINT_Y      252

/* ---- categories ------------------------------------------------------------------------ */

enum { CK_RECENT, CK_FAVS, CK_GAMES, CK_SYSTEM, CK_APPS, CK_SETTINGS };

typedef struct Cat {
    char name[64], icon[32], noun[16];
    uint32_t col;
    int kind, sys;
    LEntry **it; int n;
    int sel; float scroll;
} Cat;

static PspExpContext *g_ctx;
static Library L;
static Cat g_cat[40];
static int g_ncat, g_cur;
static LEntry g_actions[8];
static int g_nactions;
static const char *g_wifi_cmd;    /* pspexp-wifi-mode, when installed */
static int g_nofetch;             /* box art downloads off */

static void action(LEntry *e, const char *id, const char *title, const char *icon, const char *detail)
{
    memset(e, 0, sizeof(*e));
    e->type = LT_ACTION; e->sys = -1;
    snprintf(e->id, sizeof(e->id), "a:%s", id);
    snprintf(e->title, sizeof(e->title), "%s", title);
    snprintf(e->icon, sizeof(e->icon), "%s", icon);
    snprintf(e->tags, sizeof(e->tags), "%s", detail);
}

static uint32_t system_colour(const char *id)
{
    static const uint32_t pal[] = { 0x38bdf8, 0xa78bfa, 0xfb7185, 0x34d399, 0xfbbf24, 0x60a5fa, 0xf472b6, 0x2dd4bf, 0xfb923c, 0xc084fc };
    return pal[gfx_hash(id) % (sizeof(pal) / sizeof(pal[0]))];
}

static Cat *cat_add(const char *name, const char *icon, uint32_t col, int kind, const char *noun)
{
    Cat *c = &g_cat[g_ncat++];
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    snprintf(c->icon, sizeof(c->icon), "%s", icon);
    snprintf(c->noun, sizeof(c->noun), "%s", noun);
    c->col = col; c->kind = kind; c->sys = -1;
    return c;
}

static void cat_push(Cat *c, LEntry *e)
{
    c->it = (LEntry **)realloc(c->it, (size_t)(c->n + 1) * sizeof(LEntry *));
    c->it[c->n++] = e;
}

static void cats_free(void)
{
    int i;
    for (i = 0; i < g_ncat; i++) free(g_cat[i].it);
    g_ncat = 0;
}

/* (re)builds the rail from the library; keeps the current category and selection */
static void cats_build(void)
{
    char keep[64] = "", keep_id[160] = "";
    int i, s, keep_sel = 0;
    float keep_scroll = 0;
    Cat *c;
    if (g_ncat) {
        snprintf(keep, sizeof(keep), "%s", g_cat[g_cur].name);
        keep_sel = g_cat[g_cur].sel;
        keep_scroll = g_cat[g_cur].scroll;
        if (g_cat[g_cur].n) snprintf(keep_id, sizeof(keep_id), "%s", g_cat[g_cur].it[g_cat[g_cur].sel]->id);
    }
    cats_free();
    if (L.nrecent) {
        c = cat_add("Recent", "history", 0xf5a524, CK_RECENT, "item");
        for (i = 0; i < L.nrecent; i++) { LEntry *e = lib_find(&L, L.recent[i]); if (e) cat_push(c, e); }
        if (!c->n) g_ncat--;
    }
    if (L.nfavs) {
        c = cat_add("Favourites", "star", 0xf472b6, CK_FAVS, "item");
        for (i = 0; i < L.nfavs; i++) { LEntry *e = lib_find(&L, L.favs[i]); if (e) cat_push(c, e); }
        if (!c->n) g_ncat--;
    }
    c = cat_add("Games", "gamepad-2", 0x818cf8, CK_GAMES, "game");
    for (i = 0; i < L.n; i++) if (L.e[i].type == LT_GAME) cat_push(c, &L.e[i]);
    for (s = 0; s < L.nsys && g_ncat < (int)(sizeof(g_cat) / sizeof(g_cat[0])) - 2; s++) {
        if (!L.sys[s].nroms) continue;
        c = cat_add(L.sys[s].name, L.sys[s].icon[0] ? L.sys[s].icon : "gamepad", system_colour(L.sys[s].id), CK_SYSTEM, "game");
        c->sys = s;
        for (i = 0; i < L.sys[s].nroms; i++) cat_push(c, &L.sys[s].roms_list[i]);
    }
    c = cat_add("Apps", "layout-grid", 0x2dd4bf, CK_APPS, "app");
    for (i = 0; i < L.n; i++) if (L.e[i].type == LT_APP || L.e[i].type == LT_TOOL) cat_push(c, &L.e[i]);
    c = cat_add("System", "settings", 0x94a3b8, CK_SETTINGS, "item");
    g_nactions = 0;
    action(&g_actions[g_nactions++], "rescan", "Refresh library", "refresh-cw", "Look for new packages, entries and ROMs");
    if (g_wifi_cmd) action(&g_actions[g_nactions++], "wifi", "Wi-Fi", "radio-tower", "PSP direct: the GameShell is the PSP's access point");
    action(&g_actions[g_nactions++], "boxart", "Box art", g_nofetch ? "image-off" : "image", "Download covers for ROMs from libretro-thumbnails");
    action(&g_actions[g_nactions++], "console", "Console", "square-terminal", "This board's shell on the PSP");
    action(&g_actions[g_nactions++], "reboot", "Restart", "rotate-ccw", "Restart this board");
    action(&g_actions[g_nactions++], "poweroff", "Power off", "power", "Shut this board down");
    action(&g_actions[g_nactions++], "about", "About", "info", "This board, the PSP and the connection");
    for (i = 0; i < g_nactions; i++) cat_push(c, &g_actions[i]);
    /* back to where we were */
    g_cur = -1;
    for (i = 0; i < g_ncat; i++) if (keep[0] && !strcmp(g_cat[i].name, keep)) { g_cur = i; break; }
    if (g_cur < 0) { g_cur = 0; for (i = 0; i < g_ncat; i++) if (g_cat[i].kind == CK_GAMES) g_cur = i; keep_sel = 0; keep_scroll = 0; }
    c = &g_cat[g_cur];
    c->sel = keep_sel < c->n ? keep_sel : (c->n ? c->n - 1 : 0);
    c->scroll = keep_scroll;
    for (i = 0; i < c->n; i++) if (keep_id[0] && !strcmp(c->it[i]->id, keep_id)) c->sel = i;
}

static LEntry *cur_entry(void)
{
    Cat *c;
    if (!g_ncat) return NULL;
    c = &g_cat[g_cur];
    return c->n ? c->it[c->sel] : NULL;
}

/* ---- state ------------------------------------------------------------------------------ */

enum { M_BROWSE, M_SEARCH, M_DETAILS, M_ABOUT };
static int g_mode;
static float g_overlay_a;             /* search / details / about fade-in */
static uint64_t g_about_at;
static void about_collect(void);

static int g_dirty = 1, g_anim;
static uint64_t g_now, g_open_at;   /* this frame's time; when the details card or About page opened */

/* scrolling text: keeps frames coming while it moves */
static int mtext(int font, int x, int y, const char *s, uint32_t rgb, int alpha, int max_w, uint64_t since)
{
    return gfx_text_marquee(font, x, y, s, rgb, alpha, max_w, g_now - since, &g_anim);
}
static uint64_t g_last_frame, g_sel_since;
static float g_rail_x = 16, g_lift[40], g_list_anim = 1, g_list_dir, g_expand[1024], g_cover_fade = 1;
static char g_cover_prev[160], g_cover_cur[160];
static char g_toast[160], g_toast_icon[32];
static uint64_t g_toast_until;
static float g_toast_a;
static char g_launch_cmd[1200], g_launch_cwd[512];
static int g_launch_pending, g_launch_aware, g_running, g_console_req;
static LEntry g_running_e;
static uint64_t g_confirm_until;
static char g_confirm[32];
static int g_bat = -2, g_chg, g_wifi = -2, g_minute = -1;
static uint64_t g_status_at;
static int g_nofetch;

static void toast(const char *icon, const char *s, uint64_t now)
{
    snprintf(g_toast, sizeof(g_toast), "%s", s);
    snprintf(g_toast_icon, sizeof(g_toast_icon), "%s", icon ? icon : "");
    g_toast_until = now + 2600000u;
    g_dirty = 1;
}

/* ---- box art: libretro thumbnails, fetched in the background -------------------------------- */

static pid_t g_fetch_pid;
static char g_fetch_id[160];

static void art_paths(const LEntry *e, char *png, size_t n, char *miss, size_t m)
{
    const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
    const char *base = strrchr(e->exec, '/'), *dot;
    char stem[200];
    size_t i;
    base = base ? base + 1 : e->exec;
    dot = strrchr(base, '.');
    snprintf(stem, sizeof(stem), "%.*s", dot ? (int)(dot - base) : (int)strlen(base), base);
    for (i = 0; stem[i]; i++) if (strchr("&*/:`<>?\\|\"", stem[i])) stem[i] = '_';   /* libretro-thumbnails naming */
    snprintf(png, n, "%s/.cache/pspexp-launcher/boxart/%s/%s.png", home, L.sys[e->sys].id, stem);
    if (miss) snprintf(miss, m, "%s/.cache/pspexp-launcher/boxart/%s/%s.miss", home, L.sys[e->sys].id, stem);
}

static void urlenc(const char *s, char *out, size_t n)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~", c)) out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

static void fetch_start(const LEntry *e)
{
    char png[700], miss[700], dir[700], sysenc[300], nameenc[600], url[1200], stem[400], *slash;
    const char *base;
    pid_t pid;
    art_paths(e, png, sizeof(png), miss, sizeof(miss));
    snprintf(dir, sizeof(dir), "%s", png);
    if ((slash = strrchr(dir, '/'))) *slash = 0;
    base = strrchr(png, '/') + 1;
    urlenc(L.sys[e->sys].thumbs, sysenc, sizeof(sysenc));
    snprintf(stem, sizeof(stem), "%.*s", (int)(strlen(base) - 4), base);
    urlenc(stem, nameenc, sizeof(nameenc));
    snprintf(url, sizeof(url), "https://thumbnails.libretro.com/%s/Named_Boxarts/%s.png", sysenc, nameenc);
    pid = fork();
    if (pid == 0) {
        setenv("D", dir, 1); setenv("P", png, 1); setenv("M", miss, 1); setenv("U", url, 1);
        execl("/bin/sh", "sh", "-c",
              /* curl where there is one; OpenWrt has wget (uclient-fetch) */
              "mkdir -p \"$D\" && if command -v curl >/dev/null; then curl -fsSL --max-time 20 -o \"$P.part\" \"$U\"; "
              "else wget -q -T 20 -O \"$P.part\" \"$U\"; fi && [ -s \"$P.part\" ] && mv \"$P.part\" \"$P\" || { rm -f \"$P.part\"; touch \"$M\"; }",
              (char *)NULL);
        _exit(127);
    }
    if (pid > 0) { g_fetch_pid = pid; snprintf(g_fetch_id, sizeof(g_fetch_id), "%s", e->id); }
}

static void fetch_poll(uint64_t now)
{
    LEntry *e;
    if (g_fetch_pid > 0) {
        int st;
        if (waitpid(g_fetch_pid, &st, WNOHANG) == g_fetch_pid) {
            g_fetch_pid = 0;
            if ((e = cur_entry()) && !strcmp(e->id, g_fetch_id)) g_dirty = 1;   /* the cover picks the art up */
        }
        return;
    }
    if (g_nofetch || g_running || !(e = cur_entry()) || e->type != LT_ROM || e->sys < 0 || !L.sys[e->sys].thumbs[0]) return;
    if (now - g_sel_since < 500000u) return;   /* only once the selection rests */
    {
        char png[700], miss[700];
        struct stat st;
        art_paths(e, png, sizeof(png), miss, sizeof(miss));
        if (stat(png, &st) == 0 || stat(miss, &st) == 0) return;
        fetch_start(e);
    }
}

/* ---- covers ------------------------------------------------------------------------------ */

static const char *type_icon(const LEntry *e)
{
    if (e->icon[0]) return e->icon;
    switch (e->type) {
    case LT_ROM: return e->sys >= 0 && L.sys[e->sys].icon[0] ? L.sys[e->sys].icon : "gamepad";
    case LT_APP: return "app-window";
    case LT_TOOL: return "wand-sparkles";
    default: return "gamepad-2";
    }
}

static const char *type_badge(const LEntry *e)
{
    switch (e->type) {
    case LT_ROM: return e->sys >= 0 ? L.sys[e->sys].shortname : "ROM";
    case LT_APP: return "APP";
    case LT_TOOL: return "TOOL";
    case LT_ACTION: return "SYSTEM";
    default: return "GAME";
    }
}

static void draw_cover(const LEntry *e, int x, int y, int alpha)
{
    CoverSpec cs;
    char png[700] = "";
    memset(&cs, 0, sizeof(cs));
    cs.key = e->id; cs.title = e->title; cs.badge = type_badge(e); cs.icon = type_icon(e);
    if (e->art[0]) cs.art = e->art;
    else if (e->type == LT_ROM && e->sys >= 0) { struct stat st; art_paths(e, png, sizeof(png), NULL, 0); if (stat(png, &st) == 0) cs.art = png; }
    gfx_cover(&cs, x, y, alpha);
}

/* ---- settings ---------------------------------------------------------------------------------- */

static pid_t g_bg_pid;
static char g_bg_ok[96], g_bg_fail[96], g_bg_icon[32];

/* runs a command in the background; a toast says how it went */
static void bg_run(const char *cmd, const char *icon, const char *ok, const char *fail)
{
    pid_t pid;
    if (g_bg_pid > 0) return;
    pid = fork();
    if (pid == 0) {
        int fd = open("/tmp/pspexp-launcher.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return;
    g_bg_pid = pid;
    snprintf(g_bg_ok, sizeof(g_bg_ok), "%s", ok);
    snprintf(g_bg_fail, sizeof(g_bg_fail), "%s", fail);
    snprintf(g_bg_icon, sizeof(g_bg_icon), "%s", icon);
}

static void toast(const char *icon, const char *s, uint64_t now);

static void bg_poll(uint64_t now)
{
    int st;
    if (g_bg_pid <= 0 || waitpid(g_bg_pid, &st, WNOHANG) != g_bg_pid) return;
    g_bg_pid = 0;
    toast(g_bg_icon, WIFEXITED(st) && WEXITSTATUS(st) == 0 ? g_bg_ok : g_bg_fail, now);
}

static int wifi_mode_psp(void)
{
    FILE *f = fopen("/etc/pspexp/wifi-mode", "r");
    char m[16] = "";
    if (f) { if (!fgets(m, sizeof(m), f)) m[0] = 0; fclose(f); }
    return !strncmp(m, "psp", 3);
}

/* what a System row shows on its right */
static const char *action_value(const LEntry *e)
{
    const char *a = e->id + 2;
    if (!strcmp(a, "wifi")) return wifi_mode_psp() ? "PSP direct" : "Home network";
    if (!strcmp(a, "boxart")) return g_nofetch ? "Off" : "On";
    return NULL;
}

/* ---- launching and actions ------------------------------------------------------------------- */

static void run_action(LEntry *e, uint64_t now)
{
    const char *a = e->id + 2;
    if (!strcmp(a, "rescan")) {
        char s[96];
        int n = lib_reload(&L);
        cats_build();
        snprintf(s, sizeof(s), "%d game%s, app%s and ROMs", n, n == 1 ? "" : "s", n == 1 ? "" : "s");
        toast("refresh-cw", s, now);
    } else if (!strcmp(a, "console")) {
        g_console_req = 1;
    } else if (!strcmp(a, "boxart")) {
        g_nofetch = !g_nofetch;
        lib_setting_set(&L, "boxart", !g_nofetch);
        snprintf(e->icon, sizeof(e->icon), "%s", g_nofetch ? "image-off" : "image");
        toast(e->icon, g_nofetch ? "Box art downloads off" : "Box art downloads on", now);
    } else if (!strcmp(a, "wifi")) {
        int to_psp = !wifi_mode_psp();
        char cmd[400];
        if (now > g_confirm_until || strcmp(g_confirm, a)) {
            snprintf(g_confirm, sizeof(g_confirm), "%s", a);
            g_confirm_until = now + 4000000u;
            toast("radio-tower", to_psp ? "Press X again: Wi-Fi straight to the PSP" : "Press X again: back to the home Wi-Fi (keep the PSP on USB)", now);
            return;
        }
        g_confirm[0] = 0;
        snprintf(cmd, sizeof(cmd), "%s%s %s", geteuid() == 0 ? "" : "sudo -n ", g_wifi_cmd, to_psp ? "psp" : "home");
        bg_run(cmd, "radio-tower", to_psp ? "Wi-Fi: PSP direct" : "Wi-Fi: home network", "Wi-Fi switch failed (see /tmp/pspexp-launcher.log)");
        toast("radio-tower", "Switching Wi-Fi...", now);
    } else if (!strcmp(a, "reboot") || !strcmp(a, "poweroff")) {
        int reboot = !strcmp(a, "reboot");
        if (now > g_confirm_until || strcmp(g_confirm, a)) {
            snprintf(g_confirm, sizeof(g_confirm), "%s", a);
            g_confirm_until = now + 3000000u;
            toast(reboot ? "rotate-ccw" : "power", reboot ? "Press X again to restart" : "Press X again to power off", now);
            return;
        }
        g_confirm[0] = 0;
        /* as root (OpenWrt on the Omega2+) directly; else through the sudo rule
         * the installer adds (/etc/sudoers.d/pspexp) */
        if (geteuid() == 0) snprintf(g_launch_cmd, sizeof(g_launch_cmd), "exec %s", reboot ? "reboot" : "poweroff");
        else snprintf(g_launch_cmd, sizeof(g_launch_cmd), "exec sudo -n /usr/sbin/%s", reboot ? "reboot" : "poweroff");
        snprintf(g_launch_cwd, sizeof(g_launch_cwd), "/");
        g_launch_aware = 1;
        g_launch_pending = 1;
        toast("power", reboot ? "Restarting..." : "Powering off...", now);
    } else if (!strcmp(a, "about")) {
        about_collect();
        g_about_at = now;
        g_open_at = now;
        g_mode = M_ABOUT;
        g_overlay_a = 0;
        g_dirty = 1;
    }
}

static void launch(LEntry *e, uint64_t now)
{
    if (e->type == LT_ACTION) { run_action(e, now); return; }
    lib_command(&L, e, g_launch_cmd, sizeof(g_launch_cmd));
    snprintf(g_launch_cwd, sizeof(g_launch_cwd), "%s", e->dir[0] ? e->dir : "/");
    g_launch_aware = e->pspexp || strstr(g_launch_cmd, "pspexp-") != NULL;
    g_launch_pending = 1;
    g_running = 1;
    g_running_e = *e;
    lib_recent_push(&L, e->id);
    g_dirty = 1;
}

int launcher_take_launch(char *cmd, size_t cmd_len, char *cwd, size_t cwd_len, int *pspexp_aware)
{
    if (!g_launch_pending) return 0;
    g_launch_pending = 0;
    snprintf(cmd, cmd_len, "%s", g_launch_cmd);
    snprintf(cwd, cwd_len, "%s", g_launch_cwd);
    *pspexp_aware = g_launch_aware;
    return 1;
}

int launcher_take_console(void) { int r = g_console_req; g_console_req = 0; return r; }

void launcher_child_exited(int status)
{
    char s[160];
    if (g_running && status != 0) {
        snprintf(s, sizeof(s), "%s stopped (status %d)", g_running_e.title, status);
        toast("info", s, pspexp_time_us());
    }
    g_running = 0;
    cats_build();
    g_dirty = 1;
}

void launcher_invalidate(void) { g_dirty = 1; }

/* ---- about: this board and the PSP --------------------------------------------------------------- */

static char g_link[160] = "not connected";
void launcher_set_link(const char *desc) { snprintf(g_link, sizeof(g_link), "%s", desc ? desc : ""); g_dirty = 1; }

typedef struct About {
    char board[96], soc[96], cpu[96], mem[64], os[96], kernel[96];
    char disks[3][96]; int ndisks;   /* one line per storage device */
    char host[64], net[160], power[64], temp[32], uptime[48], library[96];
} About;
static About g_about;

static int read_line(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    size_t l;
    if (!f) return -1;
    if (!fgets(out, (int)n, f)) { fclose(f); return -1; }
    fclose(f);
    l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == '\r' || out[l - 1] == ' ')) out[--l] = 0;
    return 0;
}

/* KEY="value" from os-release style files */
static int read_quoted(const char *path, const char *key, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    char line[256];
    size_t kl = strlen(key);
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            char *v = line + kl + 1, *e;
            if (*v == '"' || *v == '\'') v++;
            e = v + strlen(v);
            while (e > v && (e[-1] == '\n' || e[-1] == '"' || e[-1] == '\'')) *--e = 0;
            snprintf(out, n, "%s", v);
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static void cpuinfo_field(const char *key, char *out, size_t n)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[256];
    size_t kl = strlen(key);
    out[0] = 0;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (!strncasecmp(line, key, kl)) {
            char *c = strchr(line, ':');
            if (c) { c++; while (*c == ' ' || *c == '\t') c++; snprintf(out, n, "%s", c); }
            break;
        }
    }
    fclose(f);
    { size_t l = strlen(out); while (l && (out[l - 1] == '\n' || out[l - 1] == ' ')) out[--l] = 0; }
}

/* the chip, from the device tree's compatible list (friendlier than /proc/cpuinfo) */
static void soc_name(char *out, size_t n)
{
    static const struct { const char *compat, *name; } SOCS[] = {
        { "allwinner,sun8i-r16", "Allwinner R16" }, { "allwinner,sun8i-a33", "Allwinner A33" },
        { "allwinner,sun8i-h3", "Allwinner H3" }, { "allwinner,sun50i-h6", "Allwinner H6" },
        { "brcm,bcm2712", "Broadcom BCM2712" }, { "brcm,bcm2711", "Broadcom BCM2711" },
        { "brcm,bcm2837", "Broadcom BCM2837" }, { "brcm,bcm2836", "Broadcom BCM2836" }, { "brcm,bcm2835", "Broadcom BCM2835" },
        { "mediatek,mt7628an-soc", "MediaTek MT7688" }, { "mediatek,mt7628", "MediaTek MT7688" },
        { "rockchip,rk3399", "Rockchip RK3399" }, { "rockchip,rk3566", "Rockchip RK3566" },
    };
    char buf[512];
    FILE *f = fopen("/proc/device-tree/compatible", "rb");
    size_t len = 0, i, k;
    out[0] = 0;
    if (f) { len = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); }
    buf[len] = 0;
    for (k = 0; k < sizeof(SOCS) / sizeof(SOCS[0]) && !out[0]; k++)   /* first match in table order: most specific */
        for (i = 0; i < len; i += strlen(buf + i) + 1)
            if (!strcmp(buf + i, SOCS[k].compat)) { snprintf(out, n, "%s", SOCS[k].name); break; }
    if (!out[0]) cpuinfo_field("system type", out, n);   /* OpenWrt on MIPS */
    if (!out[0]) cpuinfo_field("Hardware", out, n);
    if (!out[0]) cpuinfo_field("model name", out, n);
}

static void human_bytes(unsigned long long b, char *out, size_t n)
{
    if (b >= 1000ull * 1000 * 1000) snprintf(out, n, "%.1f GB", (double)b / 1e9);
    else snprintf(out, n, "%llu MB", b / 1000000ull);
}

/* what kind of storage a device is, by its name */
/* the storage kind of a mounted file system. The device name alone can
 * mislead ("/dev/root" on a GameShell is its SD card), so block devices are
 * looked up by number in sysfs; MMC says whether it is an SD card or eMMC. */
static const char *disk_kind(const char *dev, const char *mnt, dev_t num)
{
    char link[96], path[512], type[16];
    ssize_t n;
    FILE *f;
    snprintf(link, sizeof(link), "/sys/dev/block/%u:%u", major(num), minor(num));
    n = readlink(link, path, sizeof(path) - 32);
    if (n > 0) {
        path[n] = 0;
        if (strstr(path, "/mmc")) {
            /* partition: the card is one directory up */
            char tp[600];
            type[0] = 0;
            snprintf(tp, sizeof(tp), "%s/device/type", link);
            if (!(f = fopen(tp, "r"))) { snprintf(tp, sizeof(tp), "%s/../device/type", link); f = fopen(tp, "r"); }
            if (f) { if (!fgets(type, sizeof(type), f)) type[0] = 0; fclose(f); }
            return !strncmp(type, "MMC", 3) ? "Internal flash" : "SD card";
        }
        if (strstr(path, "/nvme")) return "SSD";
        if (strstr(path, "/usb")) return "USB disk";
        if (strstr(path, "/mtd") || strstr(path, "/ubi")) return "Internal flash";
    }
    if (strstr(dev, "mmcblk")) return "SD card";
    if (strstr(dev, "nvme")) return "SSD";
    if (!strncmp(dev, "/dev/sd", 7)) return "USB disk";
    if (strstr(dev, "overlay") || strstr(dev, "mtd") || strstr(dev, "ubi") || !strcmp(dev, "/dev/root")) return "Internal flash";
    return !strcmp(mnt, "/") ? "System" : "Disk";
}

/* one line per mounted storage device: the root and anything under /mnt,
 * /media, /opt (not /boot, not the kernel's own file systems) */
static void about_disks(About *a)
{
    FILE *f = fopen("/proc/mounts", "r");
    char dev[128], mnt[256], type[32], line[512];
    dev_t seen[8];
    int nseen = 0, k;
    a->ndisks = 0;
    if (!f) return;
    /* the root first */
    while (fgets(line, sizeof(line), f)) {
        struct stat st;
        struct statvfs sv;
        char tot[32], fr[32];
        if (sscanf(line, "%127s %255s %31s", dev, mnt, type) != 3) continue;
        if (strcmp(mnt, "/") && strncmp(mnt, "/mnt/", 5) && strncmp(mnt, "/media/", 7) && strncmp(mnt, "/opt", 4)) continue;
        if (!strcmp(type, "tmpfs") || !strcmp(type, "squashfs") || !strcmp(type, "autofs")) continue;
        if (stat(mnt, &st) != 0 || statvfs(mnt, &sv) != 0 || sv.f_blocks == 0) continue;
        /* small system partitions (the Omega's 512 KB settings flash) are not storage */
        if (strcmp(mnt, "/") && (unsigned long long)sv.f_blocks * sv.f_frsize < 16ull * 1024 * 1024) continue;
        for (k = 0; k < nseen; k++) if (seen[k] == st.st_dev) break;
        if (k < nseen || nseen == 8) continue;
        seen[nseen++] = st.st_dev;
        if (a->ndisks == 3) continue;
        human_bytes((unsigned long long)sv.f_blocks * sv.f_frsize, tot, sizeof(tot));
        human_bytes((unsigned long long)sv.f_bavail * sv.f_frsize, fr, sizeof(fr));
        snprintf(a->disks[a->ndisks], sizeof(a->disks[0]), "%s: %s, %s free", disk_kind(dev, mnt, st.st_dev), tot, fr);
        if (!strcmp(mnt, "/") && a->ndisks > 0) {   /* keep the root on the first line */
            char t[96];
            memcpy(t, a->disks[a->ndisks], sizeof(t));
            memmove(a->disks[1], a->disks[0], sizeof(a->disks[0]) * (size_t)a->ndisks);
            memcpy(a->disks[0], t, sizeof(t));
        }
        a->ndisks++;
    }
    fclose(f);
}

static void about_collect(void)
{
    About *a = &g_about;
    struct utsname u;
    char tmp[128], t2[64];
    int cores = 0, bat, i;
    memset(a, 0, sizeof(*a));
    if (read_line("/proc/device-tree/model", a->board, sizeof(a->board)) < 0 || !a->board[0]) {
        cpuinfo_field("machine", a->board, sizeof(a->board));   /* OpenWrt: "Onion Omega2+" */
        if (!a->board[0]) cpuinfo_field("Model", a->board, sizeof(a->board));
        if (!a->board[0]) snprintf(a->board, sizeof(a->board), "Linux computer");
    }
    soc_name(a->soc, sizeof(a->soc));
    { FILE *f = fopen("/proc/cpuinfo", "r"); char l[256]; if (f) { while (fgets(l, sizeof(l), f)) if (!strncmp(l, "processor", 9)) cores++; fclose(f); } }
    tmp[0] = 0;
    if (read_line("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", tmp, sizeof(tmp)) == 0 && atol(tmp) > 0)
        snprintf(t2, sizeof(t2), ", %.1f GHz", atol(tmp) / 1e6);
    else t2[0] = 0;
    uname(&u);
    if (!cores) cores = 1;
    snprintf(a->cpu, sizeof(a->cpu), "%d core%s %s%s", cores, cores == 1 ? "" : "s", u.machine, t2);
    {
        FILE *f = fopen("/proc/meminfo", "r");
        char l[128]; unsigned long long tot = 0, avail = 0;
        if (f) {
            while (fgets(l, sizeof(l), f)) {
                if (!strncmp(l, "MemTotal:", 9)) tot = strtoull(l + 9, NULL, 10) * 1024ull;
                else if (!strncmp(l, "MemAvailable:", 13)) avail = strtoull(l + 13, NULL, 10) * 1024ull;
            }
            fclose(f);
        }
        human_bytes(tot, tmp, sizeof(tmp)); human_bytes(avail, t2, sizeof(t2));
        snprintf(a->mem, sizeof(a->mem), "%s, %s free", tmp, t2);
    }
    about_disks(a);
    if (read_quoted("/etc/os-release", "PRETTY_NAME", a->os, sizeof(a->os)) < 0)
        read_quoted("/etc/openwrt_release", "DISTRIB_DESCRIPTION", a->os, sizeof(a->os));
    snprintf(a->kernel, sizeof(a->kernel), "Linux %s", u.release);
    gethostname(a->host, sizeof(a->host) - 1);
    {   /* IPv4 addresses, one per interface */
        struct ifaddrs *ifs, *it;
        size_t o = 0;
        if (getifaddrs(&ifs) == 0) {
            for (it = ifs; it; it = it->ifa_next) {
                char ip[INET_ADDRSTRLEN];
                if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET || (it->ifa_flags & IFF_LOOPBACK)) continue;
                inet_ntop(AF_INET, &((struct sockaddr_in *)it->ifa_addr)->sin_addr, ip, sizeof(ip));
                if (o + strlen(ip) + strlen(it->ifa_name) + 5 < sizeof(a->net))
                    o += (size_t)snprintf(a->net + o, sizeof(a->net) - o, "%s%s %s", o ? ", " : "", it->ifa_name, ip);
            }
            freeifaddrs(ifs);
        }
        if (!o) snprintf(a->net, sizeof(a->net), "no network");
    }
    bat = g_bat;
    if (bat >= 0) snprintf(a->power, sizeof(a->power), "battery %d%%%s", bat, g_chg ? ", charging" : "");
    else snprintf(a->power, sizeof(a->power), "mains / USB");
    for (i = 0; i < 4 && !a->temp[0]; i++) {
        char p[80];
        snprintf(p, sizeof(p), "/sys/class/thermal/thermal_zone%d/temp", i);
        if (read_line(p, tmp, sizeof(tmp)) == 0 && atol(tmp) > 0) snprintf(a->temp, sizeof(a->temp), "%.0f C", atol(tmp) / 1000.0);
    }
    if (read_line("/proc/uptime", tmp, sizeof(tmp)) == 0) {
        long up = atol(tmp);
        if (up >= 86400) snprintf(a->uptime, sizeof(a->uptime), "%ld d %ld h", up / 86400, up % 86400 / 3600);
        else if (up >= 3600) snprintf(a->uptime, sizeof(a->uptime), "%ld h %ld min", up / 3600, up % 3600 / 60);
        else snprintf(a->uptime, sizeof(a->uptime), "%ld min", up / 60);
    }
    {
        int roms = 0, s2;
        for (s2 = 0; s2 < L.nsys; s2++) roms += L.sys[s2].nroms;
        snprintf(a->library, sizeof(a->library), "%d games and apps, %d systems, %d ROMs", L.n, L.nsys, roms);
    }
}

/* ---- search and details: state and logic ------------------------------------------------------ */


/* search: an on-screen keyboard over live results from the whole library */
#define KB_COLS 13
#define KB_ROWS 3
static const char *KB[KB_ROWS][KB_COLS] = {
    { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M" },
    { "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z" },
    { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "SPC", "DEL", "GO" },
};
static char g_q[40];
static int g_kx, g_ky, g_sfocus, g_ssel, g_nres, g_caret;
static float g_sscroll;
static LEntry **g_res;

static int word_starts(const char *title, const char *q)
{
    size_t n = strlen(q);
    const char *t = title;
    while (*t) {
        if (!strncasecmp(t, q, n)) return t == title ? 3 : 2;
        while (*t && *t != ' ' && *t != '-' && *t != ':') t++;
        while (*t == ' ' || *t == '-' || *t == ':') t++;
    }
    return 0;
}

static int contains_ci(const char *h, const char *q)
{
    size_t n = strlen(q);
    for (; *h; h++) if (!strncasecmp(h, q, n)) return 1;
    return 0;
}

typedef struct Hit { LEntry *e; int score; } Hit;
static int cmp_hit(const void *a, const void *b)
{
    const Hit *x = (const Hit *)a, *y = (const Hit *)b;
    if (x->score != y->score) return y->score - x->score;
    return strcasecmp(x->e->title, y->e->title);
}

static void search_update(void)
{
    Hit *h = NULL;
    int n = 0, cap = 0, i, s, k;
    char q[40];
    snprintf(q, sizeof(q), "%s", g_q);
    { size_t l = strlen(q); while (l && q[l - 1] == ' ') q[--l] = 0; }
    free(g_res); g_res = NULL; g_nres = 0; g_ssel = 0; g_sscroll = 0;
    if (!q[0]) return;
    for (s = -1; s < L.nsys; s++) {
        LEntry *arr = s < 0 ? L.e : L.sys[s].roms_list;
        int cnt = s < 0 ? L.n : L.sys[s].nroms;
        for (i = 0; i < cnt; i++) {
            int sc = word_starts(arr[i].title, q);
            if (!sc && contains_ci(arr[i].title, q)) sc = 1;
            if (!sc) continue;
            if (arr[i].fav) sc += 1;
            if (n == cap) { cap = cap ? cap * 2 : 64; h = (Hit *)realloc(h, (size_t)cap * sizeof(Hit)); }
            h[n].e = &arr[i]; h[n].score = sc; n++;
        }
    }
    if (n) qsort(h, (size_t)n, sizeof(Hit), cmp_hit);
    if (n > 200) n = 200;
    g_res = (LEntry **)malloc((size_t)(n ? n : 1) * sizeof(LEntry *));
    for (k = 0; k < n; k++) g_res[k] = h[k].e;
    g_nres = n;
    free(h);
}

static void search_open(uint64_t now)
{
    g_mode = M_SEARCH; g_overlay_a = 0;
    g_sfocus = 0;
    (void)now;
    search_update();
    g_dirty = 1;
}

static void launch(LEntry *e, uint64_t now);

static void search_type(const char *key)
{
    size_t l = strlen(g_q);
    if (!strcmp(key, "DEL")) { if (l) g_q[l - 1] = 0; }
    else if (!strcmp(key, "SPC")) { if (l && l + 1 < sizeof(g_q) && g_q[l - 1] != ' ') { g_q[l] = ' '; g_q[l + 1] = 0; } }
    else if (l + 1 < sizeof(g_q)) { g_q[l] = (char)(key[0] >= 'A' && key[0] <= 'Z' ? key[0] - 'A' + 'a' : key[0]); g_q[l + 1] = 0; }
    search_update();
}

static void toast(const char *icon, const char *s, uint64_t now);
static void cats_build(void);

static void search_input(uint32_t pressed, int up, int down, int left, int right, uint64_t now)
{
    if (g_sfocus == 0) {
        if (left) g_kx = (g_kx + KB_COLS - 1) % KB_COLS;
        if (right) g_kx = (g_kx + 1) % KB_COLS;
        if (up) g_ky = (g_ky + KB_ROWS - 1) % KB_ROWS;
        if (down) g_ky = (g_ky + 1) % KB_ROWS;
        if (pressed & PSPEXP_BUTTON_CROSS) {
            const char *k = KB[g_ky][g_kx];
            if (!strcmp(k, "GO")) { if (g_nres) g_sfocus = 1; }
            else search_type(k);
        }
        if (pressed & PSPEXP_BUTTON_SQUARE) search_type("DEL");
        if (pressed & PSPEXP_BUTTON_TRIANGLE) search_type("SPC");
        if ((pressed & PSPEXP_BUTTON_START) && g_nres) g_sfocus = 1;
        if (pressed & PSPEXP_BUTTON_CIRCLE) { g_mode = M_BROWSE; }
    } else {
        if ((up || down) && g_nres) g_sel_since = now;
        if (up && g_nres) g_ssel = (pressed & PSPEXP_BUTTON_UP) ? (g_ssel + g_nres - 1) % g_nres : (g_ssel > 0 ? g_ssel - 1 : 0);
        if (down && g_nres) g_ssel = (pressed & PSPEXP_BUTTON_DOWN) ? (g_ssel + 1) % g_nres : (g_ssel + 1 < g_nres ? g_ssel + 1 : g_ssel);
        if ((pressed & PSPEXP_BUTTON_CROSS) && g_nres) { LEntry *e = g_res[g_ssel]; g_mode = M_BROWSE; launch(e, now); }
        if ((pressed & PSPEXP_BUTTON_SQUARE) && g_nres) {
            char id[160];
            int on;
            snprintf(id, sizeof(id), "%s", g_res[g_ssel]->id);
            on = lib_fav_toggle(&L, id);
            cats_build();
            toast("star", on ? "Added to favourites" : "Removed from favourites", now);
        }
        if (pressed & (PSPEXP_BUTTON_CIRCLE | PSPEXP_BUTTON_START)) g_sfocus = 0;
    }
    g_dirty = 1;
}

/* details: a card over the list with everything known about the entry */
static LEntry *g_det;

static void details_input(uint32_t pressed, uint64_t now)
{
    if (pressed & PSPEXP_BUTTON_CROSS) { LEntry *e = g_det; g_mode = M_BROWSE; launch(e, now); }
    else if (pressed & PSPEXP_BUTTON_SQUARE) {
        char id[160];
        int on;
        snprintf(id, sizeof(id), "%s", g_det->id);
        on = lib_fav_toggle(&L, id);
        cats_build();
        g_det = lib_find(&L, id);
        if (!g_det) g_mode = M_BROWSE;
        toast("star", on ? "Added to favourites" : "Removed from favourites", now);
    }
    else if (pressed & (PSPEXP_BUTTON_CIRCLE | PSPEXP_BUTTON_TRIANGLE)) g_mode = M_BROWSE;
    g_dirty = 1;
}

/* ---- input -------------------------------------------------------------------------------- */

static void select_cat(int c, int dir, uint64_t now)
{
    if (c < 0) c = g_ncat - 1;
    if (c >= g_ncat) c = 0;
    if (c == g_cur) return;
    g_cur = c;
    g_list_anim = 0; g_list_dir = (float)dir;
    memset(g_expand, 0, sizeof(g_expand));
    g_sel_since = now;
    g_dirty = 1;
}

static void move_sel(int d, int wrap, uint64_t now)
{
    Cat *c = &g_cat[g_cur];
    int s = c->sel + d;
    if (!c->n) return;
    if (wrap && (s < 0 || s >= c->n) && (d == 1 || d == -1)) s = (s + c->n) % c->n;
    s = s < 0 ? 0 : s >= c->n ? c->n - 1 : s;
    if (s != c->sel) { c->sel = s; g_sel_since = now; g_dirty = 1; }
}

void launcher_input(uint32_t b, uint32_t pressed, int up, int down, int left, int right, uint64_t now)
{
    LEntry *e;
    (void)b;
    if (g_running || !g_ncat) return;
    if (g_mode == M_SEARCH) { search_input(pressed, up, down, left, right, now); return; }
    if (g_mode == M_DETAILS) { details_input(pressed, now); return; }
    if (g_mode == M_ABOUT) {
        if (pressed & (PSPEXP_BUTTON_CIRCLE | PSPEXP_BUTTON_CROSS | PSPEXP_BUTTON_TRIANGLE | PSPEXP_BUTTON_START)) { g_mode = M_BROWSE; g_dirty = 1; }
        return;
    }
    if (pressed & PSPEXP_BUTTON_START) { search_open(now); return; }
    if (left) select_cat(g_cur - 1, -1, now);
    if (right) select_cat(g_cur + 1, 1, now);
    if (up) move_sel(-1, (pressed & PSPEXP_BUTTON_UP) != 0, now);   /* wraps on a fresh press, stops while repeating */
    if (down) move_sel(1, (pressed & PSPEXP_BUTTON_DOWN) != 0, now);
    if (pressed & PSPEXP_BUTTON_LTRIGGER) move_sel(-5, 0, now);
    if (pressed & PSPEXP_BUTTON_RTRIGGER) move_sel(5, 0, now);
    if (pressed & PSPEXP_BUTTON_CIRCLE) move_sel(-100000, 0, now);   /* back to the top */
    e = cur_entry();
    if (!e) return;
    if (pressed & PSPEXP_BUTTON_CROSS) launch(e, now);
    if ((pressed & PSPEXP_BUTTON_SQUARE) && e->type != LT_ACTION) {
        char id[160];
        int on;
        snprintf(id, sizeof(id), "%s", e->id);
        on = lib_fav_toggle(&L, id);
        cats_build();
        toast("star", on ? "Added to favourites" : "Removed from favourites", now);
    }
    if (pressed & PSPEXP_BUTTON_TRIANGLE) {
        if (e->type == LT_ACTION) toast(e->icon, e->tags, now);
        else { g_det = e; g_mode = M_DETAILS; g_overlay_a = 0; g_open_at = now; g_dirty = 1; }
    }
}

/* ---- status sources ------------------------------------------------------------------------ */

static int read_int(const char *p, int *v) { FILE *f = fopen(p, "r"); int ok; if (!f) return -1; ok = fscanf(f, "%d", v) == 1; fclose(f); return ok ? 0 : -1; }

static void status_poll(uint64_t now)
{
    static const char *dirs[] = { "/sys/class/power_supply/axp20x-battery", "/sys/class/power_supply/battery", "/sys/class/power_supply/BAT0", NULL };
    int bat = -1, chg = 0, wifi = -1, i;
    char p[160];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    if (tmv.tm_min != g_minute) { g_minute = tmv.tm_min; g_dirty = 1; }
    if (now - g_status_at < 3000000u && g_bat != -2) return;
    g_status_at = now;
    for (i = 0; dirs[i]; i++) {
        snprintf(p, sizeof(p), "%s/capacity", dirs[i]);
        if (read_int(p, &bat) == 0) {
            FILE *f; char st[32];
            snprintf(p, sizeof(p), "%s/status", dirs[i]);
            if ((f = fopen(p, "r"))) { if (fgets(st, sizeof(st), f) && !strncmp(st, "Charging", 8)) chg = 1; fclose(f); }
            break;
        }
        bat = -1;
    }
    {
        FILE *f = fopen("/proc/net/wireless", "r");
        char line[256];
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                char name[32]; int s; float link;
                if (sscanf(line, " %31[^:]: %d %f", name, &s, &link) == 3) { wifi = link >= 55 ? 3 : link >= 35 ? 2 : link > 0 ? 1 : 0; break; }
            }
            fclose(f);
        }
        if (wifi < 0) {   /* no wireless extensions in the kernel: up or not, strength unknown */
            FILE *o = fopen("/sys/class/net/wlan0/operstate", "r");
            char st[16] = "";
            if (o) { if (fgets(st, sizeof(st), o) && !strncmp(st, "up", 2)) wifi = 2; fclose(o); }
        }
    }
    if (bat != g_bat || chg != g_chg || wifi != g_wifi) { g_bat = bat; g_chg = chg; g_wifi = wifi; g_dirty = 1; }
}

/* ---- animation ----------------------------------------------------------------------------- */

static float ease(float cur, float target, float rate, float dt, float snap)
{
    float k = 1.0f - expf(-rate * dt), v = cur + (target - cur) * k;
    if (fabsf(target - v) < snap) v = target;
    if (v != target) g_anim = 1;
    return v;
}

static void library_changed(uint64_t now)
{
    lib_reload(&L);
    cats_build();
    g_det = NULL;
    if (g_mode == M_DETAILS) g_mode = M_BROWSE;
    if (g_mode == M_SEARCH) search_update();
    toast("package", "Library updated", now);
}

int launcher_needs_frame(uint64_t now)
{
    static uint64_t last_check;
    if (!g_running && now - last_check > 3000000u) {   /* packages installed or removed, ROMs added */
        last_check = now;
        if (lib_changed(&L)) library_changed(now);
    }
    bg_poll(now);
    fetch_poll(now);
    status_poll(now);
    if (g_confirm[0] && now > g_confirm_until) g_confirm[0] = 0;
    if (g_mode == M_SEARCH && (int)((now / 530000u) & 1) != g_caret) { g_caret = (int)((now / 530000u) & 1); g_dirty = 1; }
    if (g_mode == M_ABOUT && now - g_about_at > 3000000u) { about_collect(); g_about_at = now; g_dirty = 1; }
    if (g_toast[0] && now >= g_toast_until && g_toast_a <= 0.01f) { g_toast[0] = 0; g_dirty = 1; }
    return g_dirty || g_anim || g_toast[0] != 0;
}

/* ---- drawing ------------------------------------------------------------------------------ */

static void draw_status(void)
{
    char s[64];
    time_t t = time(NULL);
    struct tm tmv;
    int x, w;
    localtime_r(&t, &tmv);
    strftime(s, sizeof(s), "%H:%M", &tmv);
    w = gfx_text(FONT_TITLE, 16, 4, s, C_TEXT, 255, 0);
    strftime(s, sizeof(s), "%a %d %b", &tmv);
    gfx_text(FONT_SMALL, 16 + w + 8, 8, s, C_MUTED, 255, 0);
    x = SCREEN_W - 16;
    if (g_bat >= 0) {
        const char *ic = g_chg ? "battery-charging" : g_bat > 70 ? "battery-full" : g_bat > 35 ? "battery-medium" : g_bat > 12 ? "battery-low" : "battery-warning";
        snprintf(s, sizeof(s), "%d%%", g_bat);
        x -= gfx_text_width(FONT_SMALL, s);
        gfx_text(FONT_SMALL, x, 8, s, C_SUB, 255, 0);
        x -= 20;
        gfx_icon(ic, 16, x, 6, g_bat <= 12 && !g_chg ? 0xf87171 : C_SUB, 255);
        x -= 10;
    }
    if (g_wifi >= 0) {
        const char *ic = g_wifi >= 3 ? "wifi" : g_wifi == 2 ? "wifi-high" : g_wifi == 1 ? "wifi-low" : "wifi-zero";
        x -= 16;
        gfx_icon(ic, 16, x, 6, C_SUB, 255);
    }
}

static void draw_rail(float dt)
{
    int i, pitch = TILE + TILE_GAP, total = g_ncat * pitch - TILE_GAP + (TILE_SEL - TILE);
    float target;
    /* left-aligned with the cover column; scrolls when it overflows, two tiles of context on the left */
    if (16 + total <= SCREEN_W - 16) target = 16;
    else {
        target = 16.0f - (float)((g_cur - 2) * pitch);
        if (target > 16) target = 16;
        if (target < (float)(SCREEN_W - 16 - total)) target = (float)(SCREEN_W - 16 - total);
    }
    g_rail_x = ease(g_rail_x, target, 14.0f, dt, 0.3f);
    for (i = 0; i < g_ncat; i++) {
        Cat *c = &g_cat[i];
        float lift = g_lift[i] = ease(g_lift[i], i == g_cur ? 1.0f : 0.0f, 16.0f, dt, 0.01f);
        int size = TILE + (int)lroundf(lift * (TILE_SEL - TILE));
        /* tiles after the selected one make room for its extra size */
        float left = g_rail_x + (float)(i * pitch);
        int x, y;
        {
            int k; float extra = 0;
            for (k = 0; k < i; k++) extra += g_lift[k] * (TILE_SEL - TILE);
            left += extra;
        }
        x = (int)lroundf(left);
        y = RAIL_Y - size / 2 - (int)lroundf(lift * 2);
        if (x + size < -20 || x > SCREEN_W + 20) continue;
        if (lift > 0.02f) gfx_shadow(NINE_SHADOW14, x, y + 5, size, size, c->col, (int)(lift * 140));
        gfx_nine(NINE_R14, x, y, size, size, C_TILE, 235);
        if (lift > 0.02f) gfx_nine(NINE_R14, x, y, size, size, c->col, (int)(lift * 255));
        if (c->kind == CK_SYSTEM) {   /* systems: their short name, set like a logo */
            const char *t = L.sys[c->sys].shortname;
            int f = gfx_text_width(FONT_TITLE, t) <= size - 12 ? FONT_TITLE : FONT_BODY, tw = gfx_text_width(f, t);
            int ty = y + (size - gfx_line_height(f)) / 2;
            gfx_text(f, x + (size - tw) / 2, ty, t, c->col, (int)((1 - lift) * 235), 0);
            if (lift > 0.02f) gfx_text(f, x + (size - tw) / 2, ty, t, 0xffffff, (int)(lift * 255), 0);
        } else {
            gfx_icon(c->icon, 24, x + (size - 24) / 2, y + (size - 24) / 2, c->col, (int)((1 - lift) * 235));
            if (lift > 0.02f) gfx_icon(c->icon, 24, x + (size - 24) / 2, y + (size - 24) / 2, 0xffffff, (int)(lift * 255));
        }
    }
}

static void human_size(long long b, char *out, size_t n)
{
    if (b >= 1048576) snprintf(out, n, "%.1f MB", (double)b / 1048576.0);
    else snprintf(out, n, "%lld KB", (b + 1023) / 1024);
}

static void detail_line(const LEntry *e, char *s, size_t n)
{
    char sz[32];
    switch (e->type) {
    case LT_ROM:
        human_size(e->size, sz, sizeof(sz));
        if (e->tags[0]) snprintf(s, n, "%s  /  %s  /  %s", L.sys[e->sys].shortname, e->tags, sz);
        else snprintf(s, n, "%s  /  %s", L.sys[e->sys].name, sz);
        break;
    case LT_APP: snprintf(s, n, "App%s", e->pspexp ? "  /  PSP native" : ""); break;
    case LT_TOOL: snprintf(s, n, "Tool"); break;
    case LT_ACTION:
        if (!strcmp(e->id, "a:wifi")) snprintf(s, n, "%s", wifi_mode_psp() ? "The GameShell is the PSP's access point" : "On the home network; X switches to PSP direct");
        else if (!strcmp(e->id, "a:boxart")) snprintf(s, n, "%s", g_nofetch ? "Generated covers only" : "Covers for ROMs from libretro-thumbnails");
        else snprintf(s, n, "%s", e->tags);
        break;
    default: snprintf(s, n, "Game%s%s%s", e->pspexp ? "  /  PSP native" : "", e->tags[0] ? "  /  " : "", e->tags); break;
    }
}

static void draw_list(float dt)
{
    Cat *c = &g_cat[g_cur];
    float y, target;
    int i, alpha, dx, lh_t = gfx_line_height(FONT_TITLE), lh_b = gfx_line_height(FONT_BODY);
    char s[200];
    LEntry *e = cur_entry();
    g_list_anim = ease(g_list_anim, 1.0f, 16.0f, dt, 0.01f);
    alpha = (int)(g_list_anim * 255);
    dx = (int)lroundf((1 - g_list_anim) * 24 * g_list_dir);

    /* header: the category and its size */
    gfx_text(FONT_TITLE, 16 + dx / 2, HEAD_Y - 2, c->name, C_TEXT, alpha, 300);
    if (c->kind != CK_SETTINGS) {
        snprintf(s, sizeof(s), "%d %s%s", c->n, c->noun, c->n == 1 ? "" : "s");
        gfx_text(FONT_SMALL, LIST_X + LIST_W - gfx_text_width(FONT_SMALL, s), HEAD_Y + 2, s, C_MUTED, alpha, 0);
    }

    /* cover, cross-faded between selections */
    if (e && strcmp(e->id, g_cover_cur)) {
        snprintf(g_cover_prev, sizeof(g_cover_prev), "%s", g_cover_cur);
        snprintf(g_cover_cur, sizeof(g_cover_cur), "%s", e->id);
        g_cover_fade = g_cover_prev[0] ? 0 : 1;
    }
    g_cover_fade = ease(g_cover_fade, 1.0f, 24.0f, dt, 0.02f);
    if (e) {
        gfx_shadow(NINE_SHADOW14, COVER_X, COVER_Y + 6, COVER_SIZE, COVER_SIZE, 0x000000, 160);
        if (g_cover_fade < 1 && g_cover_prev[0]) {
            LEntry *p = lib_find(&L, g_cover_prev);
            if (!p) { int k; for (k = 0; k < g_nactions; k++) if (!strcmp(g_actions[k].id, g_cover_prev)) p = &g_actions[k]; }
            if (p) draw_cover(p, COVER_X, COVER_Y, 255);
        }
        draw_cover(e, COVER_X, COVER_Y, (int)(g_cover_fade * 255));
        if (e->fav) {
            gfx_nine(NINE_R6, COVER_X + COVER_SIZE - 28, COVER_Y + 8, 20, 20, 0x000000, 120);
            gfx_icon("star", 12, COVER_X + COVER_SIZE - 24, COVER_Y + 12, 0xfbbf24, 255);
        }
    }

    if (!c->n) {
        gfx_text(FONT_TITLE, LIST_X + 10 + dx, LIST_Y + 18, c->kind == CK_GAMES ? "No games yet" : "Nothing here yet", C_SUB, alpha, 0);
        gfx_text(FONT_SMALL, LIST_X + 10 + dx, LIST_Y + 42, "Installed games and apps appear here", C_MUTED, alpha, 0);
        return;
    }

    /* rows: the focused one grows to hold its detail line; scrolling keeps it in view */
    for (i = 0; i < c->n && i < 1024; i++) g_expand[i] = ease(g_expand[i], i == c->sel ? 1.0f : 0.0f, 20.0f, dt, 0.01f);
    target = (float)((c->sel - 1) * ROW);   /* one whole row of context above the focus */
    if (target > (float)(c->n * ROW + ROW_EXTRA - LIST_H)) target = (float)(c->n * ROW + ROW_EXTRA - LIST_H);
    if (target < 0) target = 0;
    c->scroll = ease(c->scroll, target, 16.0f, dt, 0.3f);

    gfx_set_clip(LIST_X - 8, LIST_Y - 2, LIST_W + 16, LIST_H + 4);
    y = (float)LIST_Y - c->scroll;
    for (i = 0; i < c->n; i++) {
        float ex = i < 1024 ? g_expand[i] : 0;
        int h = ROW + (int)lroundf(ex * ROW_EXTRA), ry = (int)lroundf(y), sel = i == c->sel;
        if (ry + h >= LIST_Y - 2 && ry <= LIST_Y + LIST_H + 2) {
            LEntry *it = c->it[i];
            int tx = LIST_X + 6 + dx, right = LIST_X + LIST_W - 6 + dx;
            int font = sel ? FONT_TITLE : FONT_BODY, lh = sel ? lh_t : lh_b;
            if (ex > 0.02f) {
                int a = (int)(ex * alpha);
                gfx_nine(NINE_R10, LIST_X - 6 + dx, ry, LIST_W + 12, h, 0xffffff, a * 24 / 255);
                gfx_rect(LIST_X - 6 + dx, ry + 7, 3, h - 14, c->col, a);
            }
            if (it->type == LT_ACTION) {
                const char *v = action_value(it);
                gfx_icon(it->icon, 16, tx, ry + (ROW - 16) / 2 + (sel ? 1 : 0), sel ? 0xffffff : C_SUB, alpha);
                tx += 24;
                if (v) {
                    right -= gfx_text_width(FONT_SMALL, v);
                    gfx_text(FONT_SMALL, right, ry + (ROW - gfx_line_height(FONT_SMALL)) / 2 + 1, v, sel ? C_TEXT : C_MUTED, alpha, 0);
                    right -= 8;
                }
            }
            /* right side: favourite mark; the system in mixed lists */
            if (it->fav) { right -= 12; gfx_icon("star", 12, right, ry + (ROW - 12) / 2 + 1, 0xfbbf24, alpha); right -= 6; }
            if ((c->kind == CK_RECENT || c->kind == CK_FAVS) && it->type != LT_ACTION) {
                const char *b = type_badge(it);
                int bw = gfx_text_width(FONT_BADGE, b);
                right -= bw;
                gfx_text(FONT_BADGE, right, ry + (ROW - gfx_line_height(FONT_BADGE)) / 2 + 1, b, C_MUTED, alpha, 0);
                right -= 8;
            }
            if (sel) mtext(font, tx, ry + (ROW - lh) / 2 + 1, it->title, C_TEXT, alpha, right - tx - 4, g_sel_since);
            else gfx_text(font, tx, ry + (ROW - lh) / 2, it->title, C_SUB, alpha, right - tx - 4);
            if (ex > 0.3f) {
                detail_line(it, s, sizeof(s));
                mtext(FONT_SMALL, tx, ry + ROW - 1, s, C_MUTED, (int)((ex - 0.3f) / 0.7f * alpha), LIST_X + LIST_W - 12 - tx + dx, g_sel_since);
            }
        }
        y += (float)h;
    }
    gfx_set_clip(0, 0, SCREEN_W, SCREEN_H);

    /* scroll bar */
    if (c->n * ROW + ROW_EXTRA > LIST_H) {
        int full = c->n * ROW + ROW_EXTRA, th = LIST_H * LIST_H / full, ty = (int)(c->scroll * LIST_H / full);
        if (th < 12) th = 12;
        gfx_nine(NINE_R6, LIST_X + LIST_W + 10, LIST_Y, 3, LIST_H, 0xffffff, 18);
        gfx_nine(NINE_R6, LIST_X + LIST_W + 10, LIST_Y + ty, 3, th, 0xffffff, 90);
    }
}

static void draw_hints(void)
{
    int x = 16;
    LEntry *e = cur_entry();
    const char *go = !e ? "Open" : e->type == LT_ACTION ? "Select" : e->type == LT_ROM || e->type == LT_GAME ? "Play" : "Open";
    gfx_button('x', x, HINT_Y); x += 18; x += gfx_text(FONT_SMALL, x, HINT_Y - 1, go, C_SUB, 255, 0) + 14;
    if (e && e->type != LT_ACTION) {
        gfx_button('s', x, HINT_Y); x += 18; x += gfx_text(FONT_SMALL, x, HINT_Y - 1, e->fav ? "Unfavourite" : "Favourite", C_SUB, 255, 0) + 14;
    }
    gfx_button('t', x, HINT_Y); x += 18; gfx_text(FONT_SMALL, x, HINT_Y - 1, "Details", C_SUB, 255, 0);
    {
        const char *r = "Search";
        int w = gfx_text_width(FONT_SMALL, r), bw = gfx_text_width(FONT_BADGE, "START") + 10;
        x = SCREEN_W - 16 - w;
        gfx_text(FONT_SMALL, x, HINT_Y - 1, r, C_SUB, 255, 0);
        x -= bw + 6;
        gfx_nine(NINE_R6, x, HINT_Y, bw, 14, 0x2a3142, 255);
        gfx_text(FONT_BADGE, x + 5, HINT_Y + (14 - gfx_line_height(FONT_BADGE)) / 2, "START", C_SUB, 255, 0);
    }
}

/* a small key cap with a label, for hint rows */
static int hint_cap(int x, const char *cap, const char *label, uint32_t col)
{
    int bw = gfx_text_width(FONT_BADGE, cap) + 10;
    gfx_nine(NINE_R6, x, HINT_Y, bw, 14, 0x2a3142, 255);
    gfx_text(FONT_BADGE, x + 5, HINT_Y + (14 - gfx_line_height(FONT_BADGE)) / 2, cap, C_SUB, 255, 0);
    x += bw + 5;
    return x + gfx_text(FONT_SMALL, x, HINT_Y - 1, label, col, 255, 0) + 14;
}

static int hint_btn(int x, char b, const char *label)
{
    gfx_button(b, x, HINT_Y);
    x += 18;
    return x + gfx_text(FONT_SMALL, x, HINT_Y - 1, label, C_SUB, 255, 0) + 14;
}

/* ---- the search screen ---------------------------------------------------------------------- */

#define S_FIELD_Y 12
#define S_RES_Y   52
#define S_ROWS    4
#define S_KB_Y    150
#define KEY_W     32
#define KEY_H     25
#define KEY_GAP   2

static void draw_search(float dt)
{
    int i, a, fx = 16, fw = SCREEN_W - 32, x, y, kb_a;
    char s[64];
    float target;
    g_overlay_a = ease(g_overlay_a, 1.0f, 16.0f, dt, 0.01f);
    a = (int)(g_overlay_a * 255);
    /* the field */
    gfx_shadow(NINE_SHADOW10, fx, S_FIELD_Y + 3, fw, 30, 0x000000, a * 120 / 255);
    gfx_nine(NINE_R10, fx, S_FIELD_Y, fw, 30, 0x1d2334, a);
    gfx_icon("search", 16, fx + 11, S_FIELD_Y + 7, g_q[0] ? C_ACCENT : C_MUTED, a);
    x = fx + 36;
    if (g_q[0]) x += gfx_text(FONT_TITLE, x, S_FIELD_Y + (30 - gfx_line_height(FONT_TITLE)) / 2, g_q, C_TEXT, a, fw - 120);
    else gfx_text(FONT_BODY, x, S_FIELD_Y + (30 - gfx_line_height(FONT_BODY)) / 2, "Search games, apps and ROMs", C_MUTED, a, 0);
    if (g_sfocus == 0 && !g_caret) gfx_rect(x + 1, S_FIELD_Y + 7, 2, 16, C_ACCENT, a);
    if (g_q[0]) {
        snprintf(s, sizeof(s), g_nres == 1 ? "1 result" : "%d results", g_nres);
        gfx_text(FONT_SMALL, fx + fw - 12 - gfx_text_width(FONT_SMALL, s), S_FIELD_Y + (30 - gfx_line_height(FONT_SMALL)) / 2, s, C_MUTED, a, 0);
    }
    /* results */
    if (!g_q[0]) {
        gfx_icon("text-search", 24, SCREEN_W / 2 - 12, S_RES_Y + 14, C_MUTED, a * 200 / 255);
        { const char *m = "Type with the keyboard below"; gfx_text(FONT_SMALL, (SCREEN_W - gfx_text_width(FONT_SMALL, m)) / 2, S_RES_Y + 46, m, C_MUTED, a, 0); }
    } else if (!g_nres) {
        gfx_icon("search-x", 24, SCREEN_W / 2 - 12, S_RES_Y + 14, C_MUTED, a * 200 / 255);
        { const char *m = "No matches"; gfx_text(FONT_BODY, (SCREEN_W - gfx_text_width(FONT_BODY, m)) / 2, S_RES_Y + 46, m, C_SUB, a, 0); }
    } else {
        target = (float)(g_ssel - 1);
        if (target > (float)(g_nres - S_ROWS)) target = (float)(g_nres - S_ROWS);
        if (target < 0) target = 0;
        g_sscroll = ease(g_sscroll, target, 16.0f, dt, 0.02f);
        gfx_set_clip(fx - 4, S_RES_Y - 2, fw + 8, S_ROWS * ROW + 4);
        for (i = 0; i < g_nres; i++) {
            LEntry *e = g_res[i];
            int ry = S_RES_Y + (int)lroundf((i - g_sscroll) * ROW), sel = i == g_ssel, right = fx + fw - 10;
            const char *b = type_badge(e);
            if (ry < S_RES_Y - ROW || ry > S_RES_Y + S_ROWS * ROW) continue;
            if (sel) {
                gfx_nine(NINE_R10, fx, ry, fw, ROW, 0xffffff, g_sfocus ? 26 : 12);
                if (g_sfocus) gfx_rect(fx, ry + 6, 3, ROW - 12, C_ACCENT, a);
            }
            gfx_icon(e->type == LT_ROM ? "gamepad" : e->type == LT_APP ? "app-window" : e->type == LT_TOOL ? "wand-sparkles" : "gamepad-2", 14, fx + 12, ry + (ROW - 14) / 2,
                     sel && g_sfocus ? 0xffffff : C_MUTED, a);
            right -= gfx_text_width(FONT_BADGE, b);
            gfx_text(FONT_BADGE, right, ry + (ROW - gfx_line_height(FONT_BADGE)) / 2 + 1, b, C_MUTED, a, 0);
            if (e->fav) { right -= 18; gfx_icon("star", 12, right, ry + (ROW - 12) / 2, 0xfbbf24, a); }
            if (sel && g_sfocus) mtext(FONT_BODY, fx + 34, ry + (ROW - gfx_line_height(FONT_BODY)) / 2, e->title, C_TEXT, a, right - fx - 44, g_sel_since);
            else gfx_text(FONT_BODY, fx + 34, ry + (ROW - gfx_line_height(FONT_BODY)) / 2, e->title, C_SUB, a, right - fx - 44);
        }
        gfx_set_clip(0, 0, SCREEN_W, SCREEN_H);
    }
    /* keyboard */
    kb_a = g_sfocus ? a * 110 / 255 : a;
    x = (SCREEN_W - (KB_COLS * KEY_W + (KB_COLS - 1) * KEY_GAP)) / 2;
    for (y = 0; y < KB_ROWS; y++)
        for (i = 0; i < KB_COLS; i++) {
            const char *k = KB[y][i];
            int kx = x + i * (KEY_W + KEY_GAP), ky = S_KB_Y + y * (KEY_H + KEY_GAP), foc = !g_sfocus && y == g_ky && i == g_kx;
            uint32_t fg = foc ? 0xffffff : C_TEXT;
            if (foc) gfx_shadow(NINE_SHADOW10, kx, ky + 2, KEY_W, KEY_H, C_ACCENT, kb_a * 120 / 255);
            gfx_nine(NINE_R6, kx, ky, KEY_W, KEY_H, foc ? 0x6f7ff5 : 0x222a3b, foc ? kb_a : kb_a * 235 / 255);
            if (!strcmp(k, "SPC")) gfx_icon("space", 14, kx + (KEY_W - 14) / 2, ky + (KEY_H - 14) / 2, fg, kb_a);
            else if (!strcmp(k, "DEL")) gfx_icon("delete", 14, kx + (KEY_W - 14) / 2, ky + (KEY_H - 14) / 2, fg, kb_a);
            else if (!strcmp(k, "GO")) gfx_icon("corner-down-left", 14, kx + (KEY_W - 14) / 2, ky + (KEY_H - 14) / 2, g_nres ? fg : C_MUTED, kb_a);
            else gfx_text(FONT_BODY, kx + (KEY_W - gfx_text_width(FONT_BODY, k)) / 2, ky + (KEY_H - gfx_line_height(FONT_BODY)) / 2, k, fg, kb_a, 0);
        }
    /* hints */
    x = 16;
    if (!g_sfocus) {
        x = hint_btn(x, 'x', "Type");
        x = hint_btn(x, 's', "Delete");
        x = hint_btn(x, 't', "Space");
        x = hint_btn(x, 'o', "Close");
        if (g_nres) hint_cap(SCREEN_W - 16 - gfx_text_width(FONT_BADGE, "START") - 15 - gfx_text_width(FONT_SMALL, "Results"), "START", "Results", C_SUB);
    } else {
        LEntry *e = g_nres ? g_res[g_ssel] : NULL;
        x = hint_btn(x, 'x', e && (e->type == LT_ROM || e->type == LT_GAME) ? "Play" : "Open");
        x = hint_btn(x, 's', e && e->fav ? "Unfavourite" : "Favourite");
        x = hint_btn(x, 'o', "Keyboard");
    }
    (void)x;
}

/* ---- the details card -------------------------------------------------------------------------- */

static void info_row(int x, int y, const char *icon, const char *text, int w, int a)
{
    gfx_icon(icon, 14, x, y + 1, C_MUTED, a);
    mtext(FONT_SMALL, x + 22, y + (16 - gfx_line_height(FONT_SMALL)) / 2, text, C_SUB, a, w - 22, g_open_at);
}

static void draw_details(float dt)
{
    LEntry *e = g_det;
    int a, cx = 24, cy = 24, cw = SCREEN_W - 48, ch = 216, tx, ty, tw, i;
    char s[400];
    if (!e) return;
    g_overlay_a = ease(g_overlay_a, 1.0f, 18.0f, dt, 0.01f);
    a = (int)(g_overlay_a * 255);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, 0x05070c, a * 170 / 255);
    cy += (int)lroundf((1 - g_overlay_a) * 10);
    gfx_shadow(NINE_SHADOW14, cx, cy + 6, cw, ch, 0x000000, a * 200 / 255);
    gfx_nine(NINE_R14, cx, cy, cw, ch, 0x1a2030, a * 250 / 255);
    draw_cover(e, cx + 16, cy + 16, a);
    tx = cx + 16 + COVER_SIZE + 18; ty = cy + 16; tw = cx + cw - 16 - tx;
    mtext(FONT_H1, tx, ty, e->title, C_TEXT, a, tw, g_open_at);
    ty += gfx_line_height(FONT_H1) + 2;
    switch (e->type) {
    case LT_ROM: snprintf(s, sizeof(s), "%s game", L.sys[e->sys].name); break;
    case LT_APP: snprintf(s, sizeof(s), "App"); break;
    case LT_TOOL: snprintf(s, sizeof(s), "Tool"); break;
    default: snprintf(s, sizeof(s), "Game"); break;
    }
    mtext(FONT_BODY, tx, ty, e->desc[0] ? e->desc : s, C_SUB, a, tw, g_open_at);
    ty += 26;
    if (e->type == LT_ROM) {
        const char *base = strrchr(e->exec, '/');
        char sz[32];
        human_size(e->size, sz, sizeof(sz));
        if (e->tags[0]) { info_row(tx, ty, "tag", e->tags, tw, a); ty += 20; }
        info_row(tx, ty, "hard-drive", sz, tw, a); ty += 20;
        info_row(tx, ty, "file-text", base ? base + 1 : e->exec, tw, a); ty += 20;
        { const char *x = L.sys[e->sys].exec, *sp = strchr(x, ' '); snprintf(s, sizeof(s), "%.*s", sp ? (int)(sp - x) : (int)strlen(x), x); }
        info_row(tx, ty, "cpu", s, tw, a); ty += 20;
    } else {
        info_row(tx, ty, e->pspexp ? "zap" : "monitor", e->pspexp ? "Drawn by the PSP; hold START + SELECT to quit" : "Shown through the mirror", tw, a); ty += 20;
        if (e->tags[0]) { info_row(tx, ty, "tag", e->tags, tw, a); ty += 20; }
        info_row(tx, ty, "folder-open", e->dir[0] ? e->dir : "/", tw, a); ty += 20;
        if (e->pkg[0]) snprintf(s, sizeof(s), "Package %s", e->pkg);
        else snprintf(s, sizeof(s), "library/%s.entry", e->id + 2);
        info_row(tx, ty, "package", s, tw, a); ty += 20;
    }
    for (i = 0; i < L.nrecent; i++) if (!strcmp(L.recent[i], e->id)) break;
    if (i < L.nrecent) { snprintf(s, sizeof(s), i == 0 ? "Played last" : "Played recently (#%d)", i + 1); info_row(tx, ty, "history", s, tw, a); ty += 20; }
    if (e->fav) { info_row(tx, ty, "star", "Favourite", tw, a); }
    /* buttons along the card's foot */
    {
        int bx = tx, by = cy + ch - 30;
        gfx_button('x', bx, by); bx += 18; bx += gfx_text(FONT_SMALL, bx, by - 1, e->type == LT_ROM || e->type == LT_GAME ? "Play" : "Open", C_SUB, a, 0) + 14;
        gfx_button('s', bx, by); bx += 18; bx += gfx_text(FONT_SMALL, bx, by - 1, e->fav ? "Unfavourite" : "Favourite", C_SUB, a, 0) + 14;
        gfx_button('o', bx, by); bx += 18; gfx_text(FONT_SMALL, bx, by - 1, "Close", C_SUB, a, 0);
    }
}

static void draw_toast(float dt, uint64_t now)
{
    int w, x, y = HINT_Y - 36, a, ic = g_toast_icon[0] ? 20 : 0;
    g_toast_a = ease(g_toast_a, g_toast[0] && now < g_toast_until ? 1.0f : 0.0f, 14.0f, dt, 0.01f);
    if (!g_toast[0] || g_toast_a <= 0.01f) return;
    a = (int)(g_toast_a * 255);
    w = gfx_text_width(FONT_BODY, g_toast) + 28 + ic;
    if (w > SCREEN_W - 40) w = SCREEN_W - 40;
    x = (SCREEN_W - w) / 2;
    y += (int)lroundf((1 - g_toast_a) * 8);
    gfx_shadow(NINE_SHADOW10, x, y + 3, w, 26, 0x000000, a * 170 / 255);
    gfx_nine(NINE_R10, x, y, w, 26, 0x2a3142, a * 245 / 255);
    if (ic) gfx_icon(g_toast_icon, 14, x + 12, y + 6, C_ACCENT, a);
    mtext(FONT_BODY, x + 14 + ic, y + (26 - gfx_line_height(FONT_BODY)) / 2, g_toast, C_TEXT, a, w - 28 - ic, g_toast_until - 2600000u);
}

static void draw_running(void)
{
    int w, cx = (SCREEN_W - COVER_SIZE) / 2;
    const char *sub = "is running", *h = "Hold START + SELECT in the game to quit it";
    gfx_shadow(NINE_SHADOW14, cx, 40 + 8, COVER_SIZE, COVER_SIZE, 0x000000, 180);
    draw_cover(&g_running_e, cx, 40, 255);
    w = gfx_text_width(FONT_H1, g_running_e.title);
    if (w > SCREEN_W - 40) w = SCREEN_W - 40;
    gfx_text(FONT_H1, (SCREEN_W - w) / 2, 180, g_running_e.title, C_TEXT, 255, SCREEN_W - 40);
    w = gfx_text_width(FONT_BODY, sub);
    gfx_text(FONT_BODY, (SCREEN_W - w) / 2, 212, sub, C_SUB, 255, 0);
    w = gfx_text_width(FONT_SMALL, h);
    gfx_text(FONT_SMALL, (SCREEN_W - w) / 2, HINT_Y - 1, h, C_MUTED, 255, 0);
}

/* ---- the about page --------------------------------------------------------------------------------- */

static int about_row(int x, int y, int w, const char *icon, const char *label, const char *value, int a)
{
    if (!value || !value[0]) return y;
    gfx_icon(icon, 14, x, y + 3, C_MUTED, a);
    gfx_text(FONT_BADGE, x + 22, y, label, C_MUTED, a, w - 22);
    mtext(FONT_BODY, x + 22, y + 11, value, C_TEXT, a, w - 22, g_open_at);
    return y + 28;
}

static void draw_about(float dt)
{
    About *b = &g_about;
    const PspExpHello *h = g_ctx ? pspexp_caps(g_ctx) : NULL;
    const PspExpStats *st = g_ctx ? pspexp_stats(g_ctx) : NULL;
    int a, cw = (SCREEN_W - 16 * 2 - 12) / 2, cx1 = 16, cx2 = 16 + cw + 12, cy = 42, ch = 200, y;
    char psp[96] = "", mem[96] = "", rtt[48] = "", host[128];
    g_overlay_a = ease(g_overlay_a, 1.0f, 16.0f, dt, 0.01f);
    a = (int)(g_overlay_a * 255);
    snprintf(host, sizeof(host), "%s", b->host);
    gfx_icon("info", 20, 16, 12, C_ACCENT, a);
    gfx_text(FONT_TITLE, 44, 11, "About this console", C_TEXT, a, 0);
    gfx_text(FONT_SMALL, SCREEN_W - 16 - gfx_text_width(FONT_SMALL, host), 15, host, C_MUTED, a, 0);

    gfx_shadow(NINE_SHADOW10, cx1, cy + 4, cw, ch, 0x000000, a * 120 / 255);
    gfx_nine(NINE_R10, cx1, cy, cw, ch, 0x1a2030, a * 240 / 255);
    gfx_text(FONT_BADGE, cx1 + 12, cy + 9, "THIS BOARD", C_ACCENT, a, 0);
    y = cy + 26;
    y = about_row(cx1 + 12, y, cw - 24, "cpu", "BOARD", b->board, a);
    {
        char chip[160];
        snprintf(chip, sizeof(chip), "%s%s%s", b->soc, b->soc[0] ? ", " : "", b->cpu);
        y = about_row(cx1 + 12, y, cw - 24, "zap", "CHIP", chip, a);
    }
    y = about_row(cx1 + 12, y, cw - 24, "layers", "MEMORY", b->mem, a);
    if (b->ndisks) {   /* one line per device */
        int k;
        gfx_icon("hard-drive", 14, cx1 + 12, y + 3, C_MUTED, a);
        gfx_text(FONT_BADGE, cx1 + 34, y, "STORAGE", C_MUTED, a, cw - 46);
        for (k = 0; k < b->ndisks; k++)
            mtext(FONT_BODY, cx1 + 34, y + 11 + k * 17, b->disks[k], C_TEXT, a, cw - 46, g_open_at);
        y += 28 + (b->ndisks - 1) * 17;
    }
    {
        char sys[200];
        snprintf(sys, sizeof(sys), "%s, %s", b->os[0] ? b->os : "Linux", b->kernel);
        about_row(cx1 + 12, y, cw - 24, "square-terminal", "SYSTEM", sys, a);
    }

    gfx_shadow(NINE_SHADOW10, cx2, cy + 4, cw, ch, 0x000000, a * 120 / 255);
    gfx_nine(NINE_R10, cx2, cy, cw, ch, 0x1a2030, a * 240 / 255);
    gfx_text(FONT_BADGE, cx2 + 12, cy + 9, "PSP AND CONNECTION", C_ACCENT, a, 0);
    if (h && h->protocol) {
        snprintf(psp, sizeof(psp), "runtime %u, %ux%u at %u Hz", h->runtime_version, h->display_width, h->display_height, h->refresh_hz);
        snprintf(mem, sizeof(mem), "%u KB VRAM, %u KB RAM", (unsigned)(h->vram_budget / 1024), (unsigned)(h->ram_budget / 1024));
    }
    if (st && st->last_present_latency_us) snprintf(rtt, sizeof(rtt), "%.1f ms per frame, send to shown", st->last_present_latency_us / 1000.0);
    y = cy + 26;
    y = about_row(cx2 + 12, y, cw - 24, "link", "LINK", g_link, a);
    y = about_row(cx2 + 12, y, cw - 24, "clock", "LATENCY", rtt, a);
    y = about_row(cx2 + 12, y, cw - 24, "gamepad-2", "PSP", psp, a);
    y = about_row(cx2 + 12, y, cw - 24, "layers", "PSP MEMORY", mem, a);
    y = about_row(cx2 + 12, y, cw - 24, "globe", "NETWORK", b->net, a);
    {
        char pw[140];
        snprintf(pw, sizeof(pw), "%s%s%s%s%s", b->power, b->temp[0] ? ", " : "", b->temp, b->uptime[0] ? ", up " : "", b->uptime);
        about_row(cx2 + 12, y, cw - 24, g_bat >= 0 ? "battery-full" : "zap", "POWER", pw, a);
    }

    gfx_text(FONT_SMALL, SCREEN_W - 16 - gfx_text_width(FONT_SMALL, b->library), HINT_Y - 1, b->library, C_MUTED, a, SCREEN_W - 100);
    {
        int x = 16;
        gfx_button('o', x, HINT_Y); x += 18; gfx_text(FONT_SMALL, x, HINT_Y - 1, "Back", C_SUB, 255, 0);
    }
}

void launcher_render(PspExpContext *ctx, uint64_t now, int psp_connected)
{
    float dt = g_last_frame ? (float)(now - g_last_frame) / 1e6f : 0.016f;
    (void)psp_connected;
    if (dt > 0.1f) dt = 0.1f;
    g_last_frame = now;
    g_now = now;
    g_anim = 0;
    g_ctx = ctx;
    gfx_frame(ctx);
    pspexp_begin_frame(ctx);
    pspexp_set_clip(ctx, 0, 0, SCREEN_W, SCREEN_H);
    gfx_backdrop();
    if (g_running) draw_running();
    else if (g_mode == M_SEARCH) draw_search(dt);
    else if (g_mode == M_ABOUT) draw_about(dt);
    else if (g_ncat) {
        draw_status();
        draw_rail(dt);
        draw_list(dt);
        draw_hints();
        if (g_mode == M_DETAILS) draw_details(dt);
    }
    draw_toast(dt, now);
    pspexp_present(ctx);
    g_dirty = 0;
}

/* ---- init ------------------------------------------------------------------------------- */

int launcher_init(PspExpContext *ctx, const char *assets)
{
    int i;
    g_ctx = ctx;
    g_wifi_cmd = getenv("PSPEXP_WIFI_MODE_CMD");
    if (!g_wifi_cmd && access("/usr/local/bin/pspexp-wifi-mode", X_OK) == 0) g_wifi_cmd = "/usr/local/bin/pspexp-wifi-mode";
    if (gfx_init(ctx, assets) < 0) { fprintf(stderr, "[launcher] no fonts in %s\n", assets); return -1; }
    lib_load(&L);
    g_nofetch = getenv("PSPEXP_LAUNCHER_NO_FETCH") != NULL || !lib_setting_int(&L, "boxart", 1);
    cats_build();
    for (i = 0; i < g_ncat; i++) if (g_cat[i].kind == CK_GAMES) g_cur = i;   /* start on Games */
    /* screenshots and tests: open a page without replaying keys, which a slow
     * link can drop (a lost press in System could land on Power off) */
    if (getenv("PSPEXP_LAUNCHER_OPEN") && !strcmp(getenv("PSPEXP_LAUNCHER_OPEN"), "about")) {
        about_collect();
        g_mode = M_ABOUT;
        g_dirty = 1;
    }
    fprintf(stderr, "[launcher] library %s: %d entries, %d systems, %d categories\n", L.home, L.n, L.nsys, g_ncat);
    return 0;
}
