/*
 * pspexp-term launcher view (see launcher.h). Layout at 480 x 272:
 *
 *   status    y 4..24     clock and date; Wi-Fi and battery
 *   rail      x 0..46     one tile per category down the left (Recent,
 *                         Favourites, Games, each emulated system with ROMs,
 *                         Apps, System); up and down move along it
 *   header    y 30        the category's name and size
 *   shelf     y 60..160   the category's entries left to right, left and
 *                         right move along it: games and ROMs as discs (the
 *                         cover in a UMD-like shell, gfx_disc), apps and
 *                         settings as icon tiles; the selection larger, lifted
 *                         and glowing
 *   details   y 174..246  the selection's title, description and detail line
 *   hints     y 250..268  what the buttons do
 *
 * The categories are built from the library (launcher_lib.c), never from
 * folders. Every movement is eased by time; a still screen sends nothing.
 */
#include "launcher.h"
#include "notify.h"
#include "music.h"
#include "btaudio.h"
#include "launcher_lib.h"
#include "launcher_board.h"
#include "launcher_ui.h"
#ifdef __APPLE__
#include "launcher_mac.h"
#endif
#include "launcher_gfx.h"
#include "psp_plugin.h"   /* PSP_PLUGIN_*: Settings > This PSP > PSPExp plugin */
#include "pspexp_version.h"   /* PSPEXP_VERSION: generated at build time (cmake/pspexp_version.cmake) */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include "../../host/libpspexp/pspexp_disc.h"
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
#define C_SUB    0xa9b9d3
#define C_MUTED  0x7d8fae
#define C_TILE   0x16223c
#define C_ACCENT 0x5aa9ff

/* layout */
/* the left rail: one tile per category, scrolled to keep the current one in view */
#define RAIL_W      46
#define RAIL_TOP    34
#define RAIL_PITCH  40
#define RAIL_TILE   32
#define RAIL_SHOWN  5
/* the shelf: discs (games, ROMs) or tiles (apps, settings) left to right */
#define SHELF_X0    106     /* first item's centre */
#define SHELF_PITCH 84
#define SHELF_SHOWN 5
#define SHELF_Y     104     /* item centre */
#define SHELF_LABEL 150
#define DETAIL_X    58
#define DETAIL_Y    174
#define ROW         22
#define HINT_Y      252

/* ---- categories ------------------------------------------------------------------------ */

/* CK_ALL: Games' first tab (every game); CK_GAMES: the board's own games; CK_SYSTEM: a system's ROMs
 * (the PSP's too) */
enum { CK_ALL, CK_GAMES, CK_SYSTEM, CK_APPS, CK_SETTINGS };

/* the rail's sections, each its colour on the rail and in its highlights (2026-10-30) */
#define COL_GAMES    0x818cf8
#define COL_APPS     0x2dd4bf
#define COL_SETTINGS 0x5aa9ff

typedef struct Cat {
    char name[64], icon[32], noun[16];
    uint32_t col;
    int kind, sys;
    int hidden;             /* not on the rail itself: the tabs after a section's first */
    int tab;                /* a tab (L / R) of Games (1) or Settings (2): the rail shows its section for it, and up / down move along the rail */
    LEntry **it; int n;
    int sel; float scroll;
} Cat;

static PspExpContext *g_ctx;
static Library L;
static Cat g_cat[40];
static int g_ncat, g_cur;
#define MAX_ACTIONS 20
#define ACTION(...) do { if (g_nactions < MAX_ACTIONS) action(&g_actions[g_nactions++], __VA_ARGS__); } while (0)
static LEntry g_actions[MAX_ACTIONS];   /* System's entries (up to 16 now: count them when adding one) */
static int g_nactions;
static const char *g_wifi_cmd;    /* pspexp-wifi-mode, when installed */
static int g_nofetch;             /* box art downloads off */
static int g_psp_clock = 1, g_clock_req;   /* set the PSP's clock from the board; asked for now */
static int g_clips_keep, g_clips_change;   /* the recorder keeps the last seconds (off by default: it costs); just changed */
static int g_clip_secs = 30, g_clip_quality;   /* Settings > Recording: the clip's length (15, 30, 60), the quality (0 high, 1 standard, 2 small) */
static const int CLIP_CRF[3] = { 18, 23, 28 };
static const char *const CLIP_QUALITY[3] = { "High", "Standard", "Small" };
static int g_core_updated, g_console_restart_req;   /* PSPExp's own files were upgraded; the user asked to restart it */
static int g_psp_update, g_psp_update_req;          /* a newer PSP runtime is on the board; the user asked to install it */
static int g_psp_plugin = PSP_PLUGIN_UNKNOWN, g_plugin_req = -1;   /* the PSPExp plugin on the PSP's stick; on (1) or off (0) asked for */

static void action(LEntry *e, const char *id, const char *title, const char *icon, const char *detail)
{
    memset(e, 0, sizeof(*e));
    e->type = LT_ACTION; e->sys = -1;
    snprintf(e->id, sizeof(e->id), "a:%s", id);
    snprintf(e->title, sizeof(e->title), "%s", title);
    snprintf(e->icon, sizeof(e->icon), "%s", icon);
    snprintf(e->tags, sizeof(e->tags), "%s", detail);
}

/* apps built into the console, first in Apps: the terminal and the file manager */
#define NBUILTIN 6
static LEntry g_builtin[NBUILTIN];
static char g_assets[400];   /* the launcher's assets (fonts, icons, assets/systems) */
static const uint32_t GRAD_TERMINAL[2] = { 0x1e293b, 0x0d9488 }, GRAD_FILES[2] = { 0xf59e0b, 0xdb2777 },
                      GRAD_MONITOR[2] = { 0x0f766e, 0x2563eb }, GRAD_STORE[2] = { 0x7c3aed, 0xe11d48 }, GRAD_MUSIC[2] = { 0xdb2777, 0xf59e0b };
static char g_app_req[16];   /* a built-in app to show (appview.h), taken by the console */

static int is_builtin(const LEntry *e) { return e->type != LT_ACTION && !strncmp(e->id, "a:", 2); }

/* The board this runs on: a small icon in its own colour (status bar, About: the GameShell a D-pad,
 * the Omega2+ a microchip, a Pi 400/500 a keyboard, another Pi a circuit board, a Mac a laptop) and
 * its name, for Games' tab of its own games ("Raspberry Pi 400") */
static struct { const char *icon; uint32_t colour; int done; char name[48]; } g_board;
static void board_detect(void);

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

static int cmp_entry_title(const void *a, const void *b)
{
    return strcasecmp((*(LEntry *const *)a)->title, (*(LEntry *const *)b)->title);
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
static int ssh_available(void);

static int psp_not_a_game(const LEntry *e);
static int is_psp_sys(int s);

/* where the launcher opens (no home screen since 2026-10-30): Games, All */
static int start_cat(void)
{
    int i;
    for (i = 0; i < g_ncat; i++) if (g_cat[i].kind == CK_ALL) return i;
    return 0;
}

/* where an entry was in what was played lately: 0 the last; 1000 not there */
static int recent_rank(const LEntry *e)
{
    int i;
    for (i = 0; i < L.nrecent; i++) if (!strcmp(L.recent[i], e->id)) return i;
    return 1000;
}

/* All's order: the favourites first, then by when last played, then the rest A to Z; within the
 * favourites the same (the last played of them first) */
static int cmp_all(const void *a, const void *b)
{
    const LEntry *x = *(LEntry *const *)a, *y = *(LEntry *const *)b;
    int rx = recent_rank(x), ry = recent_rank(y);
    if (x->fav != y->fav) return x->fav ? -1 : 1;
    if (rx != ry) return rx < ry ? -1 : 1;
    return strcasecmp(x->title, y->title);
}

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
    action(&g_builtin[0], "console", "Terminal", "square-terminal", "This board's shell on the PSP: up to four, themes, copy and paste");
    action(&g_builtin[1], "files", "Files", "folder-open", "The board's drives and the PSP's memory stick: copy, move, view");
    action(&g_builtin[2], "monitor", "System Monitor", "activity", "This board's CPU, memory, temperature, storage, network and programs");
    action(&g_builtin[3], "store", "Packages", "package", "Games, emulators and apps for this board: install, update, remove");
    action(&g_builtin[4], "music", "Media", "clapperboard", "Your music and films; music plays on behind everything");
    action(&g_builtin[5], "gallery", "Gallery", "images", "Your screenshots, recordings and clips, a page for each game");
    for (i = 0; i < NBUILTIN; i++) {
        int k;
        g_builtin[i].type = LT_APP;
        for (k = 0; k < L.nfavs; k++) if (!strcmp(L.favs[k], g_builtin[i].id)) g_builtin[i].fav = 1;
    }
    /* the rail: Games, Apps, Settings (2026-10-30: no home screen, no Favourites, PSP or Emulators
     * section). Games is one shelf of discs with tabs on L / R: All (every game: the favourites
     * first, then by when last played), PSP (the PSP's own games: psp.system, there with none too:
     * where they go), the board's own games (the tab named after the board), then each system with
     * ROMs (made after Settings, below: tabs keep the order they are made in) */
    board_detect();
    c = cat_add("All", "gamepad-2", COL_GAMES, CK_ALL, "game");
    c->tab = 1;
    for (i = 0; i < L.n; i++) if (L.e[i].type == LT_GAME) cat_push(c, &L.e[i]);   /* (All) */
    for (s = 0; s < L.nsys; s++)
        for (i = 0; i < L.sys[s].nroms; i++) if (!is_psp_sys(s) || !psp_not_a_game(&L.sys[s].roms_list[i])) cat_push(c, &L.sys[s].roms_list[i]);
    if (c->n > 1) qsort(c->it, (size_t)c->n, sizeof(c->it[0]), cmp_all);
    for (s = 0; s < L.nsys; s++) if (is_psp_sys(s)) {
        c = cat_add("PSP", L.sys[s].icon[0] ? L.sys[s].icon : "disc-3", COL_GAMES, CK_SYSTEM, "game");
        c->sys = s; c->tab = c->hidden = 1;
        for (i = 0; i < L.sys[s].nroms; i++) if (!psp_not_a_game(&L.sys[s].roms_list[i])) cat_push(c, &L.sys[s].roms_list[i]);   /* (a PS1 disc in the PSP folder: left out) */
        break;
    }
    c = cat_add(g_board.name, g_board.icon, COL_GAMES, CK_GAMES, "game");
    c->tab = c->hidden = 1;
    for (i = 0; i < L.n; i++) if (L.e[i].type == LT_GAME) cat_push(c, &L.e[i]);
    c = cat_add("Apps", "layout-grid", COL_APPS, CK_APPS, "app");
    for (i = 0; i < NBUILTIN; i++) cat_push(c, &g_builtin[i]);
    for (i = 0; i < L.n; i++) if (L.e[i].type == LT_APP || L.e[i].type == LT_TOOL) cat_push(c, &L.e[i]);
    if (c->n > 1) qsort(c->it, (size_t)c->n, sizeof(c->it[0]), cmp_entry_title);   /* the built-in and installed apps together, A to Z */
    g_nactions = 0;
    ACTION("rescan", "Refresh library", "refresh-cw", "Look for new packages, entries and ROMs");
    if (g_wifi_cmd) ACTION("wifi", "Wi-Fi", "radio-tower", "PSP direct: the GameShell is the PSP's access point");
    if (board_wifi_kind() != BOARD_WIFI_NONE)
        ACTION("wifinet", "Wi-Fi network", "wifi", "Join a Wi-Fi network from here");
    if (bt_available()) ACTION("sound", "Sound output", "headphones", "The PSP, or Bluetooth headphones or a speaker paired with this board");
    ACTION("tz", "Time zone", "globe", "The board's time zone, for the clock");
    ACTION("pspclock", "PSP clock", "clock", "Set the PSP's clock from this board when they differ");
    ACTION("boxart", "Box art", g_nofetch ? "image-off" : "image", "Download covers for ROMs from libretro-thumbnails");
    ACTION("clips", "Clips", "history", "♪ + R saves the last seconds as a video: the board records all the time");
    ACTION("cliplen", "Clip length", "clock", "How much ♪ + R saves");
    ACTION("quality", "Quality", "gauge", "The videos' quality, and so their size");
    if (g_psp_plugin != PSP_PLUGIN_UNKNOWN) ACTION("plugin", "PSPExp plugin", "puzzle", "For the PSP's own games: the PSP tab's, and recording them");
    if (g_psp_update) ACTION("psp-update", "Update the PSP", "download", "A newer PSPExp for the PSP, or its plugin: put it on its memory stick");
    if (g_core_updated) ACTION("console-restart", "Restart PSPExp", "refresh-cw", "PSPExp was updated: start the new version");
    ACTION("reboot", "Restart", "rotate-ccw", "Restart this board");
    ACTION("poweroff", "Power off", "power", "Shut this board down");
    if (ssh_available()) ACTION("ssh", "SSH", "terminal", "Log in to this board from a computer");
    ACTION("about", "About", "info", "This board, the PSP and the connection");
    /* Settings: its groups as tabs (L / R), as Games' (2026-10-30; each opened a page of its own
     * before): Ready first when something waits (an update for the PSP, a restart), then the rest */
    {
        static const struct { const char *name, *icon, *ids[5]; } GROUPS[] = {
            { "Network", "wifi", { "a:wifinet", "a:wifi", NULL } },
            { "Sound", "headphones", { "a:sound", NULL } },
            { "Recording", "film", { "a:clips", "a:cliplen", "a:quality", NULL } },   /* (the Gallery is in Apps) */
            { "Clock", "clock", { "a:tz", NULL } },
            { "Library", "refresh-cw", { "a:rescan", "a:boxart", NULL } },
            { "This PSP", "gamepad-directional", { "a:plugin", "a:pspclock", NULL } },   /* (the PSP's own: its plugin, recording and the PSP tab's) */
            { "This board", "circuit-board", { "a:about", "a:ssh", "a:reboot", "a:poweroff", NULL } },
        };
        int g, k, first = 1;
        for (g = -1; g < (int)(sizeof(GROUPS) / sizeof(GROUPS[0])); g++) {
            static const char *const READY[] = { "a:psp-update", "a:console-restart", NULL };
            const char *const *ids = g < 0 ? READY : GROUPS[g].ids;
            c = NULL;
            for (k = 0; ids[k]; k++)   /* the group's settings in its own order (those this board has) */
                for (i = 0; i < g_nactions; i++) if (!strcmp(g_actions[i].id, ids[k])) {
                    if (!c) {   /* (a tab only with something in it; the rail's gear for each) */
                        c = cat_add(g < 0 ? "Ready" : GROUPS[g].name, "settings", COL_SETTINGS, CK_SETTINGS, "setting");
                        c->tab = 2; c->hidden = !first; first = 0;
                    }
                    cat_push(c, &g_actions[i]);
                }
        }
    }
    for (s = 0; s < L.nsys && g_ncat < (int)(sizeof(g_cat) / sizeof(g_cat[0])); s++) {
        if (!L.sys[s].nroms || is_psp_sys(s)) continue;
        c = cat_add(L.sys[s].name, L.sys[s].icon[0] ? L.sys[s].icon : "gamepad", COL_GAMES, CK_SYSTEM, "game");
        c->sys = s; c->tab = c->hidden = 1;
        for (i = 0; i < L.sys[s].nroms; i++) cat_push(c, &L.sys[s].roms_list[i]);
    }
    /* back to where we were */
    g_cur = -1;
    for (i = 0; i < g_ncat; i++) if (keep[0] && !strcmp(g_cat[i].name, keep)) { g_cur = i; break; }
    if (g_cur < 0) { g_cur = start_cat(); keep_sel = 0; keep_scroll = 0; }
    c = &g_cat[g_cur];
    c->sel = keep_sel < c->n ? keep_sel : (c->n ? c->n - 1 : 0);
    c->scroll = keep_scroll;
    if (keep_id[0]) {   /* the same entry; when it's gone (Update the PSP, once done), the one above it, never whatever slid into its place (Restart) */
        int found = 0;
        for (i = 0; i < c->n; i++) if (!strcmp(c->it[i]->id, keep_id)) { c->sel = i; found = 1; }
        if (!found && keep_sel > 0 && keep_sel - 1 < c->n) c->sel = keep_sel - 1;
    }
}

static LEntry *cur_entry(void)
{
    Cat *c;
    if (!g_ncat) return NULL;
    c = &g_cat[g_cur];
    return c->n ? c->it[c->sel] : NULL;
}



/* a section's icon: a system on the rail (PSP) its console's picture, as its Emulators tile had
 * (assets/systems), else the font's */
static void cat_icon(const Cat *c, int px, int x, int y, uint32_t rgb, int alpha)
{
    char p[600];
    if (c->kind == CK_SYSTEM && c->sys >= 0 && L.sys[c->sys].thumbs[0]) {
        snprintf(p, sizeof(p), "%s/systems/%s.png", g_assets, L.sys[c->sys].thumbs);
        if (gfx_picon(p, px, x, y, rgb, alpha)) return;
    }
    gfx_icon(c->icon, px, x, y, rgb, alpha);
}

/* the rail's sections (the categories not hidden): how many, the k-th, and where a category
 * sits on it (a Games tab: where Games is) */
static int rail_n(void) { int i, n = 0; for (i = 0; i < g_ncat; i++) if (!g_cat[i].hidden) n++; return n; }
static int rail_cat(int k) { int i; for (i = 0; i < g_ncat; i++) if (!g_cat[i].hidden && k-- == 0) return i; return 0; }
static int rail_pos(int cat)
{
    int i, k = 0;
    if (g_cat[cat].hidden) { for (i = 0; i < g_ncat; i++) if (g_cat[i].tab == g_cat[cat].tab && !g_cat[i].hidden) { cat = i; break; } }   /* (a tab: where its section is) */
    for (i = 0; i < cat; i++) if (!g_cat[i].hidden) k++;
    return k;
}

/* ---- state ------------------------------------------------------------------------------ */

enum { M_BROWSE, M_SEARCH, M_DETAILS, M_ABOUT, M_PICK, M_TEXT };
static int g_mode = M_BROWSE;
static int g_last_tab[3] = { -1, -1, -1 };   /* Games' (1) and Settings' (2) tab last shown: the rail goes back to it */
static char g_np_title[160]; static int g_np_on, g_np_paused;   /* what Music plays, as last drawn */
static float g_overlay_a;             /* search / details / about fade-in */
static uint64_t g_about_at;
static void about_collect(void);

static int g_dirty = 1, g_anim;
static uint64_t g_now, g_open_at;   /* this frame's time; when the details card or About page opened */

/* the last button press: 30 s after it the launcher goes still (scrolling text at its start, no
 * blinking caret), so an idle PSP gets no frames and its own Power Save settings can take over
 * (the runtime holds it awake only while frames or sound keep coming) */
static uint64_t g_input_at;
#define LAUNCHER_STILL_US 30000000u
static int launcher_still(uint64_t now) { return now - g_input_at > LAUNCHER_STILL_US; }

/* scrolling text: keeps frames coming while it moves (not once the launcher is still) */
static int mtext(int font, int x, int y, const char *s, uint32_t rgb, int alpha, int max_w, uint64_t since)
{
    int ignored = 0;
    if (launcher_still(g_now)) return gfx_text_marquee(font, x, y, s, rgb, alpha, max_w, 0, &ignored);
    return gfx_text_marquee(font, x, y, s, rgb, alpha, max_w, g_now - since, &g_anim);
}
static uint64_t g_last_frame, g_sel_since;
static float g_rail_top, g_lift[40], g_list_anim = 1, g_list_dir, g_expand[1024];
static char g_launch_cmd[1200], g_launch_cwd[512];
static int g_launch_pending, g_launch_aware, g_running, g_console_req, g_files_req;
static char g_files_path[512];   /* the folder Files opens on (Settings, a path asked for), else its own */
static char g_native_path[700];  /* a PSP game chosen on the shelf, for the console to start on the PSP */
static int g_spin_x, g_spin_y, g_spin_drawn;   /* the "Starting" frame's spinner square (draw_running) */
static LEntry g_running_e;
/* the controls sheet: shown when a game with control= lines is chosen, X starts it, O goes back */
static int g_sheet, g_sheet_n;
static LEntry g_sheet_e;
static char g_sheet_ctl[1200];
static uint64_t g_confirm_until;
static char g_confirm[32];
static UiMenu g_menu;   /* START's (menu_open, pick_menu_open) */
enum { MN_GO = 1, MN_FAV, MN_DETAILS, MN_SEARCH, MN_NOTIFY, MN_RESTART, MN_POWER, MN_RESTART_YES, MN_POWER_YES, MN_CANCEL,
       MN_PICK_GO, MN_PICK_SQUARE, MN_PICK_SCAN };
static int g_bat = -2, g_chg, g_wifi = -2, g_minute = -1;
static int g_snd_on, g_snd_vol = -1;   /* the sound on Bluetooth, its volume (btaudio.h) */
static int g_wired;   /* the default route goes over a cable (Ethernet, USB networking) */
static uint64_t g_status_at;
static int g_nofetch;

/* the answer to a press ("Added to favourites", "Press X again to power off"): a hint, drawn by
 * notify.c over every view and not kept; what is worth keeping goes to notify() */
static void toast(const char *icon, const char *s, uint64_t now) { (void)now; notify_hint(icon, s); }

/* ---- box art: libretro thumbnails, fetched in the background -------------------------------- */

static pid_t g_fetch_pid;
static char g_fetch_id[160];
static char g_fetch_miss[700];   /* a PSP icon's ".noicon", while its child looks */

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
    /* ".nomatch": a miss of the title matching (older ".miss" files were
     * exact-name misses and are ignored, so those ROMs get another try) */
    if (miss) snprintf(miss, m, "%s/.cache/pspexp-launcher/boxart/%s/%s.nomatch", home, L.sys[e->sys].id, stem);
}

/* a PSP game's own icon (ICON0.PNG, read from its disc on the board), its shelf cover where
 * there is no box art; ".noicon": none could be read (a PS1 game in the folder, a damaged file) */
static int is_psp_sys(int s) { return s >= 0 && !strcmp(L.sys[s].id, "psp"); }
static int is_psp_rom(const LEntry *e) { return e->type == LT_ROM && e->sys >= 0 && !strcmp(L.sys[e->sys].id, "psp"); }
static void icon_paths(const LEntry *e, char *png, size_t n, char *miss, size_t m)
{
    char art[700], *slash;
    art_paths(e, art, sizeof(art), NULL, 0);
    slash = strrchr(art, '/');
    snprintf(png, n, "%.*s/icon0/%s", (int)(slash - art), art, slash + 1);
    if (miss) snprintf(miss, m, "%.*s.noicon", (int)(strlen(png) - 4), png);
}

/* a PSP game's ".noicon", made no earlier than the file it is about: one made while the file was
 * still being copied (Ace Combat X on the GameShell, 2026-10-25: looked at 5 minutes before its copy
 * ended, and left off the PSP section for good) is no verdict on the file as it is now */
static int noicon_current(const LEntry *e, const char *miss)
{
    struct stat m, f;
    if (stat(miss, &m) != 0) return 0;
    return stat(e->exec, &f) != 0 || m.st_mtime >= f.st_mtime;
}

/* a file in the PSP folder found to be no PSP game (its icon looked for: no PSP_GAME on it) */
static int psp_not_a_game(const LEntry *e)
{
    char png[700], miss[700];
    if (!is_psp_rom(e)) return 0;
    icon_paths(e, png, sizeof(png), miss, sizeof(miss));
    return noicon_current(e, miss);
}

/* the picture for a ROM's cover: its box art; a PSP game's own icon first (the XMB's picture of
 * it, wide as the disc's window, on the disc itself: box art only for one without); NULL none yet */
static const char *rom_art(const LEntry *e, char *png, size_t n)
{
    struct stat st;
    if (is_psp_rom(e)) { icon_paths(e, png, n, NULL, 0); if (stat(png, &st) == 0) return png; }
    art_paths(e, png, n, NULL, 0);
    return stat(png, &st) == 0 ? png : NULL;
}

/* The thumbnails' names per system: its directory listing, fetched once into
 * the cache (.names.html) and matched locally, so a ROM named any way finds
 * its cover (lib_thumbs_match). */
static struct { char **names; int n, loaded; } g_thumbs[32];

static void thumbs_paths(int sys, char *html, size_t n, char *miss, size_t m)
{
    const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
    snprintf(html, n, "%s/.cache/pspexp-launcher/boxart/%s/.names.html", home, L.sys[sys].id);
    snprintf(miss, m, "%s/.cache/pspexp-launcher/boxart/%s/.names.miss", home, L.sys[sys].id);
}

/* 1 when the system's names are in memory, 0 while they still have to be
 * fetched, -1 when there are none (the listing could not be fetched) */
static int thumbs_ready(int sys)
{
    char html[700], miss[700];
    struct stat st;
    FILE *f;
    if (sys < 0 || sys >= 32) return -1;
    if (g_thumbs[sys].loaded) return g_thumbs[sys].n > 0 ? 1 : -1;
    thumbs_paths(sys, html, sizeof(html), miss, sizeof(miss));
    if (stat(html, &st) != 0) return stat(miss, &st) == 0 ? -1 : 0;
    if ((f = fopen(html, "rb"))) {
        char *buf = (char *)malloc((size_t)st.st_size + 1);
        if (buf) {
            size_t got = fread(buf, 1, (size_t)st.st_size, f);
            g_thumbs[sys].n = lib_thumbs_parse(buf, got, &g_thumbs[sys].names);
            free(buf);
        }
        fclose(f);
    }
    g_thumbs[sys].loaded = 1;
    return g_thumbs[sys].n > 0 ? 1 : -1;
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

/* download URL to OUT in the background (MISS is touched when it fails) */
static void fetch_url(const char *url, const char *out, const char *miss, const char *id)
{
    char dir[700], *slash;
    pid_t pid;
    snprintf(dir, sizeof(dir), "%s", out);
    if ((slash = strrchr(dir, '/'))) *slash = 0;
    pid = fork();
    if (pid == 0) {
        setenv("D", dir, 1); setenv("P", out, 1); setenv("M", miss, 1); setenv("U", url, 1);
        execl("/bin/sh", "sh", "-c",
              /* curl where there is one; OpenWrt has wget (uclient-fetch) */
              "mkdir -p \"$D\" && if command -v curl >/dev/null; then curl -fsSL --max-time 30 -o \"$P.part\" \"$U\"; "
              "else wget -q -T 30 -O \"$P.part\" \"$U\"; fi && [ -s \"$P.part\" ] && mv \"$P.part\" \"$P\" || { rm -f \"$P.part\"; touch \"$M\"; }",
              (char *)NULL);
        _exit(127);
    }
    if (pid > 0) { g_fetch_pid = pid; snprintf(g_fetch_id, sizeof(g_fetch_id), "%s", id); }
}

static void fetch_start(const LEntry *e)
{
    char png[700], miss[700], sysenc[300], nameenc[600], url[1200];
    int ready = thumbs_ready(e->sys);
    const char *name = NULL;
    urlenc(L.sys[e->sys].thumbs, sysenc, sizeof(sysenc));
    if (ready == 0) {   /* first the system's list of covers */
        char html[700], hmiss[700];
        thumbs_paths(e->sys, html, sizeof(html), hmiss, sizeof(hmiss));
        snprintf(url, sizeof(url), "https://thumbnails.libretro.com/%s/Named_Boxarts/", sysenc);
        fetch_url(url, html, hmiss, "");
        return;
    }
    art_paths(e, png, sizeof(png), miss, sizeof(miss));
    if (ready > 0) {
        int k = lib_thumbs_match(g_thumbs[e->sys].names, g_thumbs[e->sys].n, e->title, e->tags);
        if (k < 0) {   /* no cover of that title: remember, do not ask again */
            FILE *f;
            char dir[700], *slash;
            snprintf(dir, sizeof(dir), "%s", miss);
            if ((slash = strrchr(dir, '/'))) { *slash = 0; mkdir(dir, 0755); }
            if ((f = fopen(miss, "w"))) fclose(f);
            return;
        }
        name = g_thumbs[e->sys].names[k];
    } else {   /* no list: try the file's own name, as libretro-thumbnails spells it */
        const char *base = strrchr(png, '/') + 1;
        static char stem[400];
        snprintf(stem, sizeof(stem), "%.*s", (int)(strlen(base) - 4), base);
        name = stem;
    }
    {
        char safe[400];
        size_t i;
        snprintf(safe, sizeof(safe), "%s", name);
        for (i = 0; safe[i]; i++) if (strchr("&*/:`<>?\\|\"", safe[i])) safe[i] = '_';   /* libretro-thumbnails naming */
        urlenc(safe, nameenc, sizeof(nameenc));
    }
    snprintf(url, sizeof(url), "https://thumbnails.libretro.com/%s/Named_Boxarts/%s.png", sysenc, nameenc);
    fetch_url(url, png, miss, e->id);
}

static void fetch_poll(uint64_t now)
{
    LEntry *e;
    if (g_fetch_pid > 0) {
        int st;
        if (waitpid(g_fetch_pid, &st, WNOHANG) == g_fetch_pid) {
            struct stat ms;
            g_fetch_pid = 0;
            if (g_fetch_miss[0] && stat(g_fetch_miss, &ms) == 0) { g_fetch_miss[0] = 0; cats_build(); g_dirty = 1; }   /* (no PSP game: off the shelf now) */
            g_fetch_miss[0] = 0;
            if ((e = cur_entry()) && !strcmp(e->id, g_fetch_id)) g_dirty = 1;   /* the cover picks the art up */
            if (!g_fetch_id[0]) g_sel_since = 0;   /* a system's cover list arrived: match the selection now */
        }
        return;
    }
    if (g_running || !(e = cur_entry()) || e->type != LT_ROM || e->sys < 0) return;
    if (now - g_sel_since < 500000u) return;   /* only once the selection rests */
    if (is_psp_rom(e)) {   /* its own icon first, from its disc (offline, 1 s; a zip's deep one up to 45 s), in a child */
        char png[700], miss[700];
        struct stat st;
        icon_paths(e, png, sizeof(png), miss, sizeof(miss));
        if (stat(png, &st) == 0 || noicon_current(e, miss)) {   /* this one done: the next of the shelf without one, so it fills in while open */
            int s = e->sys, i;
            for (i = 0; i < L.sys[s].nroms; i++) {
                LEntry *o = &L.sys[s].roms_list[i];
                icon_paths(o, png, sizeof(png), miss, sizeof(miss));
                if (stat(png, &st) != 0 && !noicon_current(o, miss)) { e = o; break; }
            }
            if (i == L.sys[s].nroms) { icon_paths(e, png, sizeof(png), miss, sizeof(miss)); if (stat(png, &st) == 0) return; }   /* (all done) */
        }
        if (stat(png, &st) != 0 && !noicon_current(e, miss)) {
            pid_t pid;
            char dir[700], *slash;
            snprintf(dir, sizeof(dir), "%s", png);
            if ((slash = strrchr(dir, '/'))) {   /* (every level: a fresh home has no ~/.cache/pspexp-launcher yet) */
                char *q;
                *slash = 0;
                for (q = dir + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0755); *q = '/'; }
                mkdir(dir, 0755);
            }
            pid = fork();
            if (pid == 0) {
                PspExpDiscInfo in;
                if (pspexp_disc_info(e->exec, &in, png, NULL, 0) < 0 || stat(png, &st) != 0) { FILE *f = fopen(miss, "w"); if (f) fclose(f); }
                _exit(0);
            }
            if (pid > 0) { g_fetch_pid = pid; snprintf(g_fetch_id, sizeof(g_fetch_id), "%s", e->id); snprintf(g_fetch_miss, sizeof(g_fetch_miss), "%s", miss); }
            return;
        }
    }
    if (g_nofetch || !L.sys[e->sys].thumbs[0]) return;
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
    if (is_builtin(e)) {
        cs.hero = 1;
        cs.grad = !strcmp(e->id, "a:files") ? GRAD_FILES : !strcmp(e->id, "a:monitor") ? GRAD_MONITOR : !strcmp(e->id, "a:store") ? GRAD_STORE : !strcmp(e->id, "a:music") ? GRAD_MUSIC : GRAD_TERMINAL;
    }
    if (e->art[0]) cs.art = e->art;
    else if (e->type == LT_ROM && e->sys >= 0) cs.art = rom_art(e, png, sizeof(png));
    gfx_cover(&cs, x, y, alpha);
}

/* ---- settings ---------------------------------------------------------------------------------- */

static pid_t g_bg_pid;
static char g_bg_ok[96], g_bg_fail[96], g_bg_icon[32], g_bg_warn[160];

/* runs a command in the background; a toast says how it went */
static int bg_run_env(const char *cmd, const char *icon, const char *ok, const char *fail, const char *env, const char *val)
{
    pid_t pid;
    if (g_bg_pid > 0) return 0;   /* one at a time: the caller says so */
    pid = fork();
    if (pid == 0) {
        int fd = open("/tmp/pspexp-launcher.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        if (env && val) setenv(env, val, 1);   /* a secret: in the command's environment only */
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return 0;
    g_bg_pid = pid;
    snprintf(g_bg_ok, sizeof(g_bg_ok), "%s", ok);
    snprintf(g_bg_fail, sizeof(g_bg_fail), "%s", fail);
    snprintf(g_bg_icon, sizeof(g_bg_icon), "%s", icon);
    g_bg_warn[0] = 0;
    return 1;
}

/* a warning that goes with the started job's success (bg_run first) */
static void bg_warn(const char *s)
{
    if (g_bg_pid > 0) snprintf(g_bg_warn, sizeof(g_bg_warn), "%s", s);
}

static int bg_run(const char *cmd, const char *icon, const char *ok, const char *fail)
{
    return bg_run_env(cmd, icon, ok, fail, NULL, NULL);
}

static void toast(const char *icon, const char *s, uint64_t now);
static void board_refresh(void);

static void bg_poll(uint64_t now)
{
    int st;
    if (g_bg_pid <= 0 || waitpid(g_bg_pid, &st, WNOHANG) != g_bg_pid) return;
    g_bg_pid = 0;
    board_refresh();
    bt_route_soon();   /* a sound device may have been chosen */   /* a setting may have changed: the rows show the new values */
    {   /* a job's result (joined a network, paired headphones, set the time zone): kept */
        int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
        char key[48];
        snprintf(key, sizeof(key), "job:%s", g_bg_icon);
        (void)now;
        notify(!ok ? NOTE_FAIL : g_bg_warn[0] ? NOTE_WARN : NOTE_DONE, g_bg_icon, ok ? g_bg_ok : g_bg_fail, ok && g_bg_warn[0] ? g_bg_warn : NULL, NULL, key);
    }
}

static int wifi_mode_psp(void)
{
    FILE *f = fopen("/etc/pspexp/wifi-mode", "r");
    char m[16] = "";
    if (f) { if (!fgets(m, sizeof(m), f)) m[0] = 0; fclose(f); }
    return !strncmp(m, "psp", 3);
}

/* ---- board settings: time zone, Wi-Fi network ------------------------------------------------ */

static char g_cur_ssid[64], g_cur_tz[64];
static int g_board_known;
static int g_ssh_on, g_ssh_default;   /* SSH server on; the user's password still a card image's published one */

/* SSH: on a Debian board with an SSH server, through the root helper (pspexp-pkg-apt ssh) */
static int ssh_available(void)
{
    return access("/usr/sbin/sshd", X_OK) == 0 && access("/usr/sbin/pspexp-pkg-apt", X_OK) == 0;
}

static void ssh_refresh(void)
{
    FILE *p;
    char line[64] = "";
    if (!ssh_available() || !(p = popen("sudo -n /usr/sbin/pspexp-pkg-apt ssh status 2>/dev/null", "r"))) return;
    if (!fgets(line, sizeof(line), p)) line[0] = 0;
    pclose(p);
    g_ssh_on = !strncmp(line, "on", 2);
    g_ssh_default = strstr(line, "default") != NULL;
}

/* the board's current network and zone, read now (popen: not per frame) */
static void board_refresh(void)
{
    int k;
    board_wifi_current(board_wifi_kind(), g_cur_ssid, sizeof(g_cur_ssid));
    board_tz_current(g_cur_tz, sizeof(g_cur_tz));
    ssh_refresh();
    k = board_zone_find(g_cur_tz);
    if (k >= 0) {   /* the launcher's own clock follows at once (musl reads only TZ) */
        setenv("TZ", board_zone(k)->posix, 1);
        tzset();
    }
    g_board_known = 1;
}

/* what a System row shows on its right */
static const char *action_value(const LEntry *e)
{
    const char *a = e->id + 2;
    if (!g_board_known) board_refresh();
    if (!strcmp(a, "wifinet")) return wifi_mode_psp() ? "PSP direct mode" : g_cur_ssid[0] ? g_cur_ssid : "Not connected";
    if (!strcmp(a, "tz")) { int k = board_zone_find(g_cur_tz); return k >= 0 ? board_zone(k)->label : g_cur_tz[0] ? g_cur_tz : "UTC"; }
    if (!strcmp(a, "wifi")) return wifi_mode_psp() ? "PSP direct" : "Home network";
    if (!strcmp(a, "boxart")) return g_nofetch ? "Off" : "On";
    if (!strcmp(a, "clips")) return g_clips_keep ? "On" : "Off";
    if (!strcmp(a, "cliplen")) { static char v[16]; snprintf(v, sizeof(v), "%d seconds", g_clip_secs); return v; }
    if (!strcmp(a, "quality")) return CLIP_QUALITY[g_clip_quality];
    if (!strcmp(a, "plugin")) return g_psp_plugin == PSP_PLUGIN_ON ? "On" : g_psp_plugin == PSP_PLUGIN_OFF ? "Off" : "Not on the PSP";
    if (!strcmp(a, "sound")) { static char v[96]; bt_route_label(v, sizeof(v)); return v; }
    if (!strcmp(a, "pspclock")) return g_psp_clock ? "From the board" : "Off";
    if (!strcmp(a, "ssh")) return g_ssh_on ? "On" : "Off";
    return NULL;
}

/* ---- launching and actions ------------------------------------------------------------------- */

enum { PICK_TZ, PICK_WIFI, PICK_BT };
static void pick_open(int kind, uint64_t now);
static void ssh_action(uint64_t now);

static int g_test_mode;

static void run_action(LEntry *e, uint64_t now)
{
    const char *a = e->id + 2;
    if (g_test_mode && (!strcmp(a, "reboot") || !strcmp(a, "poweroff") || !strcmp(a, "wifi"))) {
        toast("lock", "Not during a test run", now);   /* a test never takes the board away */
        return;
    }
    if (!strcmp(a, "rescan")) {
        char s[96];
        int n = lib_reload(&L);
        cats_build();
        snprintf(s, sizeof(s), "%d game%s, app%s and ROMs", n, n == 1 ? "" : "s", n == 1 ? "" : "s");
        toast("refresh-cw", s, now);
    } else if (!strcmp(a, "console")) {
        g_console_req = 1;
    } else if (!strcmp(a, "files")) {
        g_files_req = 1;
    } else if (!strcmp(a, "monitor") || !strcmp(a, "store") || !strcmp(a, "music") || !strcmp(a, "gallery")) {
        snprintf(g_app_req, sizeof(g_app_req), "%s", a);
    } else if (!strcmp(a, "psp-update")) {
        g_psp_update_req = 1;
    } else if (!strcmp(a, "console-restart")) {
        g_console_restart_req = 1;
    } else if (!strcmp(a, "pspclock")) {
        g_psp_clock = !g_psp_clock;
        lib_setting_set(&L, "psp_clock", g_psp_clock);
        toast("clock", g_psp_clock ? "The PSP's clock follows this board" : "The PSP keeps its own clock", now);
        if (g_psp_clock) g_clock_req = 1;   /* now, not at the next connection */
    } else if (!strcmp(a, "clips")) {
        g_clips_keep = !g_clips_keep;
        g_clips_change = 1;
        lib_setting_set(&L, "clips", g_clips_keep);
        {
            char m[80];
            snprintf(m, sizeof(m), g_clips_keep ? "Clips on: ♪ + R saves the last %d seconds" : "Clips off: ♪ + L recordings only", g_clip_secs);
            toast("history", m, now);
        }
    } else if (!strcmp(a, "cliplen")) {   /* 15, 30, 60, round */
        char m[80];
        g_clip_secs = g_clip_secs == 15 ? 30 : g_clip_secs == 30 ? 60 : 15;
        g_clips_change = 1;
        lib_setting_set(&L, "clip_secs", g_clip_secs);
        snprintf(m, sizeof(m), "♪ + R saves the last %d seconds", g_clip_secs);
        toast("clock", m, now);
    } else if (!strcmp(a, "quality")) {
        char m[80];
        g_clip_quality = (g_clip_quality + 1) % 3;
        g_clips_change = 1;
        lib_setting_set(&L, "clip_quality", g_clip_quality);
        snprintf(m, sizeof(m), "Quality: %s", CLIP_QUALITY[g_clip_quality]);
        toast("gauge", m, now);
    } else if (!strcmp(a, "plugin")) {   /* the console writes game.txt (and puts the plugin there first when missing) */
        g_plugin_req = g_psp_plugin != PSP_PLUGIN_ON;
        toast("puzzle", g_plugin_req ? "Switching the PSPExp plugin on" : "Switching the PSPExp plugin off", now);
    } else if (!strcmp(a, "boxart")) {
        g_nofetch = !g_nofetch;
        lib_setting_set(&L, "boxart", !g_nofetch);
        snprintf(e->icon, sizeof(e->icon), "%s", g_nofetch ? "image-off" : "image");
        toast(e->icon, g_nofetch ? "Box art downloads off" : "Box art downloads on", now);
    } else if (!strcmp(a, "ssh")) {
        ssh_action(now);
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
    } else if (!strcmp(a, "tz")) {
        pick_open(PICK_TZ, now);
    } else if (!strcmp(a, "wifinet")) {
        if (wifi_mode_psp()) toast("wifi", "The Wi-Fi is the PSP's access point: switch it to the home network first", now);
        else pick_open(PICK_WIFI, now);
    } else if (!strcmp(a, "sound")) {
        pick_open(PICK_BT, now);
    } else if (!strcmp(a, "about")) {
        about_collect();
        g_about_at = now;
        g_open_at = now;
        g_mode = M_ABOUT;
        g_overlay_a = 0;
        g_dirty = 1;
    }
}


static void start_entry(LEntry *e, uint64_t now);
static void launch(LEntry *e, uint64_t now)
{
    if (e->type == LT_ACTION) { run_action(e, now); return; }
    if (is_builtin(e)) { lib_recent_push(&L, e->id); run_action(e, now); cats_build(); return; }
    if ((g_sheet_n = lib_controls(&L, e, g_sheet_ctl, sizeof(g_sheet_ctl))) > 0) {   /* its controls first */
        g_sheet = 1;
        g_sheet_e = *e;
        g_dirty = 1;
        return;
    }
    start_entry(e, now);
}

static void start_entry(LEntry *e, uint64_t now)
{
    (void)now;
    if (e->type == LT_ROM && e->sys >= 0 && !strncmp(L.sys[e->sys].exec, "@psp-boot", 9)) {   /* a PSP game: the PSP runs it, not the board */
        snprintf(g_native_path, sizeof(g_native_path), "%s", e->exec);
        lib_recent_push(&L, e->id);
        cats_build();   /* (All: played last, it moves up, after the favourites) */
        g_dirty = 1;
        return;
    }
    lib_command(&L, e, g_launch_cmd, sizeof(g_launch_cmd));
    snprintf(g_launch_cwd, sizeof(g_launch_cwd), "%s", e->dir[0] ? e->dir : "/");
    g_launch_aware = e->pspexp || strstr(g_launch_cmd, "pspexp-") != NULL;
    g_launch_pending = 1;
    g_running = 1;
    g_spin_drawn = 0;   /* until its "Starting" frame is drawn */
    g_running_e = *e;
    lib_recent_push(&L, e->id);
    cats_build();
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

static void status_poll(uint64_t now);
void launcher_status(uint64_t now, int *bat, int *chg, int *online)
{
    status_poll(now);
    *bat = g_bat; *chg = g_chg;
    *online = wifi_mode_psp() ? -1 : g_wired || g_wifi > 0;   /* -1: the board is the PSP's own access point */
}

int launcher_take_console(void) { int r = g_console_req; g_console_req = 0; return r; }
int launcher_take_files(void) { int r = g_files_req; g_files_req = 0; return r; }
int launcher_take_native(char *path, size_t n)
{
    if (!g_native_path[0]) return 0;
    snprintf(path, n, "%s", g_native_path);
    g_native_path[0] = 0;
    return 1;
}
const char *launcher_files_path(void)
{
    static char p[512];
    if (!g_files_path[0]) return NULL;
    snprintf(p, sizeof(p), "%s", g_files_path);
    g_files_path[0] = 0;
    return p;
}
int launcher_take_app(char *id, size_t n) { if (!g_app_req[0]) return 0; snprintf(id, n, "%s", g_app_req); g_app_req[0] = 0; return 1; }

int launcher_open_file(const char *path)
{
    LEntry e;
    if (lib_rom_entry(&L, path, &e) < 0) return 0;
    launch(&e, pspexp_time_us());
    return 1;
}

const char *launcher_running_title(void) { return g_running ? g_running_e.title : NULL; }

void launcher_set_test_mode(int on) { g_test_mode = on; }

/* 1 the first time KEY is asked for on this board (kept in the library's settings), 0 after */
int launcher_first_time(const char *key)
{
    if (lib_setting_int(&L, key, 0)) return 0;
    lib_setting_set(&L, key, 1);
    return 1;
}

int launcher_category(const char *name)
{
    int i;
    for (i = 0; i < g_ncat; i++)
        if (!strcmp(g_cat[i].name, name)) { g_cur = i; if (g_cat[i].tab) g_last_tab[g_cat[i].tab] = i; g_sel_since = pspexp_time_us(); g_dirty = 1; return 1; }
    if (!strcmp(name, "Games") || !strcmp(name, "Favourites") || !strcmp(name, "Emulators"))   /* (the sections before 2026-10-30: tests, notifications) */
        return launcher_category("All");
    if (!strcmp(name, "Settings"))   /* (its first tab) */
        for (i = 0; i < g_ncat; i++) if (g_cat[i].kind == CK_SETTINGS && !g_cat[i].hidden) return launcher_category(g_cat[i].name);
    return 0;
}

int launcher_select(const char *title)
{
    Cat *c;
    int i;
    if (!g_ncat) return 0;
    c = &g_cat[g_cur];
    for (i = 0; i < c->n; i++)
        if (!strcmp(c->it[i]->title, title)) { c->sel = i; g_sel_since = pspexp_time_us(); g_dirty = 1; return 1; }
    return 0;
}
int  launcher_psp_clock(void) { return g_psp_clock; }
void launcher_set_core_updated(void)
{
    if (g_core_updated) return;
    g_core_updated = 1;
    cats_build();   /* System gains Restart PSPExp */
    notify(NOTE_ATTN, "package", "PSPExp was updated", "Settings > Restart PSPExp starts the new version", "category:Settings", "core-updated");
}
int  launcher_take_console_restart(void) { int r = g_console_restart_req; g_console_restart_req = 0; return r; }
void launcher_set_psp_update(int available) { if (g_psp_update != available) { g_psp_update = available; cats_build(); } }
int  launcher_take_psp_update(void) { int r = g_psp_update_req; g_psp_update_req = 0; return r; }
void launcher_set_psp_plugin(int state)
{
    int shown = g_psp_plugin != PSP_PLUGIN_UNKNOWN;
    g_psp_plugin = state;
    if (shown != (state != PSP_PLUGIN_UNKNOWN)) cats_build();   /* (Recording gains or loses it) */
}
int  launcher_take_plugin_switch(void) { int r = g_plugin_req; g_plugin_req = -1; return r; }
int  launcher_take_clock_request(void) { int r = g_clock_req; g_clock_req = 0; return r; }
int  launcher_clips_keep(void) { return g_clips_keep; }
int  launcher_clip_secs(void) { return g_clip_secs; }
int  launcher_clip_crf(void) { return CLIP_CRF[g_clip_quality]; }

#ifndef __APPLE__
/* from into to, what to has already left where it is (a folder in both: merged in turn) */
static void merge_into(const char *from, const char *to)
{
    DIR *d = opendir(from);
    struct dirent *e;
    char a[1024], b[1024];
    struct stat st;
    if (!d) return;
    mkdir(to, 0755);
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(a, sizeof(a), "%s/%s", from, e->d_name);
        snprintf(b, sizeof(b), "%s/%s", to, e->d_name);
        if (lstat(b, &st) < 0) { if (rename(a, b) < 0) fprintf(stderr, "[launcher] gallery: %s not moved (%s)\n", a, strerror(errno)); }
        else if (S_ISDIR(st.st_mode) && stat(a, &st) == 0 && S_ISDIR(st.st_mode)) merge_into(a, b);
    }
    closedir(d);
    rmdir(from);   /* (only when all of it went) */
}
#endif

/* the Gallery's folder (the recorder's -s, the Gallery app's), made if not there: ~/Gallery (a Mac:
 * ~/Movies/PSPExp, the Movies folder's). Before 2026-10-29 it was ~/Captures: moved here once */
void launcher_gallery_dir(char *out, size_t n)
{
    const char *h = getenv("HOME");
#ifdef __APPLE__
    snprintf(out, n, "%s/Movies", h && *h ? h : "/tmp");
    mkdir(out, 0755);
    snprintf(out, n, "%s/Movies/PSPExp", h && *h ? h : "/tmp");
#else
    static int moved;
    snprintf(out, n, "%s/Gallery", h && *h ? h : "/tmp");
    if (!moved) {
        char old[1024];
        struct stat st;
        moved = 1;
        snprintf(old, sizeof(old), "%s/Captures", h && *h ? h : "/tmp");
        if (stat(old, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (stat(out, &st) < 0 && rename(old, out) == 0) fprintf(stderr, "[launcher] gallery: %s is now %s\n", old, out);
            else merge_into(old, out);
        }
    }
#endif
    mkdir(out, 0755);
}
int  launcher_take_clips_change(void) { int r = g_clips_change; g_clips_change = 0; return r; }

void launcher_state(char *out, size_t n)
{
    static const char *modes[] = { "browse", "search", "details", "about", "pick", "text" };
    size_t l = strlen(out);
    LEntry *e = cur_entry();
    int i;
    l += (size_t)snprintf(out + l, n > l ? n - l : 0, "launcher.mode=%s\nlauncher.category=%s\nlauncher.selected=%s\nlauncher.items=",
                          g_sheet ? "controls" : g_mode >= 0 && g_mode < 6 ? modes[g_mode] : "?", g_ncat ? g_cat[g_cur].name : "", e ? e->title : "");
    for (i = 0; g_ncat && i < g_cat[g_cur].n && l < n; i++) l += (size_t)snprintf(out + l, n - l, "%s%s", i ? "|" : "", g_cat[g_cur].it[i]->title);
    if (l < n) l += (size_t)snprintf(out + l, n - l, "\nlauncher.categories=");
    for (i = 0; i < g_ncat && l < n; i++) l += (size_t)snprintf(out + l, n - l, "%s%s", i ? "|" : "", g_cat[i].name);
    if (l < n) l += (size_t)snprintf(out + l, n - l, "\nlauncher.systems=");   /* the sections that are emulated systems (with ROMs) */
    for (i = 0, e = NULL; i < g_ncat && l < n; i++) if (g_cat[i].kind == CK_SYSTEM) l += (size_t)snprintf(out + l, n - l, "%s%s", e ? "|" : "", g_cat[i].name), e = (LEntry *)1;
    if (l < n) snprintf(out + l, n - l, "\nlauncher.toast=%s\n", notify_showing());   /* (the banner or the hint shown: tests read it) */
}

int launcher_system_for(const char *path, char *name, size_t n)
{
    LEntry e;
    int i = lib_rom_entry(&L, path, &e);
    if (i < 0) return 0;
    snprintf(name, n, "%s", L.sys[i].shortname[0] ? L.sys[i].shortname : L.sys[i].name);
    return 1;
}

void launcher_child_exited(int status, const char *message)
{
    char s[160];
    char key[64];
    snprintf(key, sizeof(key), "exit:%s", g_running_e.title);
    if (g_running && message && message[0])   /* what the program said (PSPEXP_MESSAGE) */
        notify(NOTE_FAIL, g_running_e.icon[0] ? g_running_e.icon : "circle-x", g_running_e.title, message, NULL, key);
    else if (g_running && status != 0) {
        snprintf(s, sizeof(s), "Stopped with status %d", status);
        notify(NOTE_FAIL, "circle-x", g_running_e.title, s, NULL, key);
    }
    g_running = 0;
    g_spin_drawn = 0;
    cats_build();
    g_dirty = 1;
}

void launcher_invalidate(void) { g_dirty = 1; }

/* ---- about: this board and the PSP --------------------------------------------------------------- */

static char g_link[160] = "not connected";
void launcher_set_link(const char *desc) { snprintf(g_link, sizeof(g_link), "%s", desc ? desc : ""); g_dirty = 1; }

static char g_psp_slot, g_psp_pending, g_psp_ver[64];
void launcher_set_psp_runtime(char slot, char pending, const char *version)
{
    g_psp_slot = slot; g_psp_pending = pending;
    snprintf(g_psp_ver, sizeof(g_psp_ver), "%s", version ? version : "");
    g_dirty = 1;
}

/* a runtime set's version, "2026.10.01.224358+g4330aa0" (scripts/packages/runtime-slot), shown as
 * "2026.10.01 (4330aa0)"; anything else as it is */
static void short_version(const char *v, char *out, size_t n)
{
    const char *g = strstr(v, "+g");
    if (strlen(v) >= 10 && v[4] == '.' && v[7] == '.' && g) snprintf(out, n, "%.10s (%s)", v, g + 2);
    else snprintf(out, n, "%s", v);
}

/* how an entry draws, as the details card says it: what it uses and who renders it, in the
 * shelf's words (PSP renderer / Board GPU renderer); "" not said */
static const char *graphics_label(const char *g)
{
    if (!strcmp(g, "sdl")) return "SDL, PSP renderer";
    if (!strcmp(g, "opengl")) return "OpenGL, PSP renderer";
    if (!strcmp(g, "opengl-shaders")) return "OpenGL with shaders, PSP renderer";
    if (!strcmp(g, "board-gpu")) return "OpenGL 3, Board GPU renderer";
    if (!strcmp(g, "native")) return "the PSP's own game, run by the PSP";
    return "";
}

/* the PSP's battery as one line (About), "" when there is nothing to say; the reading's charge
 * and state for the status bar's icon */
static int psp_battery(char *out, size_t n, PspExpPowerStatus *p)
{
    char t[64];
    out[0] = 0;
    if (!g_ctx || pspexp_link_state(g_ctx) != PSPEXP_LINK_CONNECTED) return 0;
    if (!pspexp_power_supported(g_ctx)) { snprintf(out, n, "needs a newer PSP runtime (Settings > Update the PSP)"); return 0; }
    if (pspexp_power_status(g_ctx, p, NULL) < 0) { snprintf(out, n, "reading..."); return 0; }
    if (!(p->flags & PSPEXP_POWER_BATTERY)) { snprintf(out, n, "no battery: on the adapter"); return 0; }
    snprintf(out, n, p->percent >= 0 ? "%d%%" : "charge unknown", p->percent);
    if (p->flags & PSPEXP_POWER_CHARGING) snprintf(t, sizeof(t), ", charging");
    else if (p->flags & PSPEXP_POWER_EXTERNAL) snprintf(t, sizeof(t), ", on the adapter");
    else if (p->minutes_left > 0) snprintf(t, sizeof(t), ", about %d h %02d min left", p->minutes_left / 60, p->minutes_left % 60);
    else t[0] = 0;
    strncat(out, t, n - strlen(out) - 1);
    if (p->current_ma) { snprintf(t, sizeof(t), ", %d mA %s", abs(p->current_ma), p->current_ma < 0 ? "drawn" : "in"); strncat(out, t, n - strlen(out) - 1); }
    if (p->temp_c >= 0) { snprintf(t, sizeof(t), ", %d C", p->temp_c); strncat(out, t, n - strlen(out) - 1); }
    return p->percent >= 0;
}

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

/* memory, counted as it is sold: in 1024s (24 GB, not 25.8); GB from 1000 MB,
 * since the kernel keeps some (a 1 GB GameShell has 1000 MB left) */
static void human_mem(unsigned long long b, char *out, size_t n)
{
    if (b >= 1000ull * 1024 * 1024) snprintf(out, n, "%.1f GB", (double)b / (1024.0 * 1024 * 1024));
    else snprintf(out, n, "%llu MB", b / (1024ull * 1024));
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
#ifdef __APPLE__
    (void)cores; (void)u; (void)soc_name; (void)read_quoted;   /* Linux only */
    mac_model(a->board, sizeof(a->board));
    mac_chip(a->soc, sizeof(a->soc));
    mac_cores(a->cpu, sizeof(a->cpu));
    {
        unsigned long long tot, avail;
        mac_memory(&tot, &avail);
        human_mem(tot, tmp, sizeof(tmp)); human_mem(avail, t2, sizeof(t2));
        snprintf(a->mem, sizeof(a->mem), "%s, %s free", tmp, t2);
    }
    about_disks(a);
    mac_os(a->os, sizeof(a->os), a->kernel, sizeof(a->kernel));
#else
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
        human_mem(tot, tmp, sizeof(tmp)); human_mem(avail, t2, sizeof(t2));
        snprintf(a->mem, sizeof(a->mem), "%s, %s free", tmp, t2);
    }
    about_disks(a);
    if (read_quoted("/etc/os-release", "PRETTY_NAME", a->os, sizeof(a->os)) < 0)
        read_quoted("/etc/openwrt_release", "DISTRIB_DESCRIPTION", a->os, sizeof(a->os));
    snprintf(a->kernel, sizeof(a->kernel), "Linux %s", u.release);
#endif
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
#ifdef __APPLE__
    snprintf(tmp, sizeof(tmp), "%ld", mac_uptime());
    if (atol(tmp) >= 0) {
#else
    if (read_line("/proc/uptime", tmp, sizeof(tmp)) == 0) {
#endif
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
static void board_refresh(void);
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
        if ((pressed & PSPEXP_BUTTON_START) && g_nres) g_sfocus = 1;
        if (pressed & PSPEXP_BUTTON_CIRCLE) { g_mode = M_BROWSE; }   /* (back where it was opened) */
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

/* ---- board settings: the picker (time zones, networks) and the password ------------------------- */

static int g_pick_kind, g_pick_sel, g_pick_n, g_scanning;
static float g_pick_scroll;
static pid_t g_scan_pid;
static BoardNet g_nets[40];
static BoardNet g_join;               /* the network the password is for */
static UiKbd g_kbd;                   /* the password being typed: wiped after use */

/* Bluetooth devices (pspexp-bt devices): row 0 of the picker is the PSP itself */
typedef struct BtDev { char addr[20], name[64]; int paired, connected, audio; } BtDev;
static BtDev g_bt[24];
static char g_bt_chosen[24];

static int bt_cmp(const void *a, const void *b)
{
    const BtDev *x = a, *y = b;
    if (x->audio != y->audio) return y->audio - x->audio;     /* sound devices first */
    if (x->paired != y->paired) return y->paired - x->paired;
    return strcmp(x->name, y->name);
}

static int bt_parse(const char *text)
{
    int n = 0;
    const char *p = text;
    while (*p && n < (int)(sizeof(g_bt) / sizeof(g_bt[0]))) {
        const char *e = strchr(p, '\n');
        char line[200];
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l < sizeof(line)) {
            BtDev *d = &g_bt[n];
            char *f[5], *q;
            int k = 0;
            memcpy(line, p, l); line[l] = 0;
            for (q = line, f[k++] = q; *q && k < 5; q++) if (*q == '|') { *q = 0; f[k++] = q + 1; }
            if (k == 5) {
                snprintf(d->addr, sizeof(d->addr), "%s", f[0]);
                snprintf(d->name, sizeof(d->name), "%s", f[1]);
                d->paired = atoi(f[2]); d->connected = atoi(f[3]); d->audio = atoi(f[4]);
                n++;
            }
        }
        p = e ? e + 1 : p + l;
    }
    qsort(g_bt, (size_t)n, sizeof(BtDev), bt_cmp);
    return n;
}

static void scan_start(void)
{
    char cmd[1024];
    pid_t pid;
    if (g_scan_pid > 0) return;
    if (g_pick_kind == PICK_BT) snprintf(cmd, sizeof(cmd), "pspexp-bt devices 8");
    else board_wifi_scan_command(board_wifi_kind(), cmd, sizeof(cmd));
    pid = fork();
    if (pid == 0) {
        int fd = open("/tmp/pspexp-wifi-scan.txt", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { dup2(fd, 1); close(fd); }
        fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 2); close(fd); }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) { g_scan_pid = pid; g_scanning = 1; g_pick_n = 0; }
}

static void scan_poll(void)
{
    int st;
    if (g_scan_pid <= 0 || waitpid(g_scan_pid, &st, WNOHANG) != g_scan_pid) return;
    g_scan_pid = 0;
    g_scanning = 0;
    {
        FILE *f = fopen("/tmp/pspexp-wifi-scan.txt", "r");
        static char buf[32768];
        size_t n = 0;
        if (f) { n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); }
        buf[n] = 0;
        unlink("/tmp/pspexp-wifi-scan.txt");
        if (g_pick_kind == PICK_BT) g_pick_n = 1 + bt_parse(buf);
        else g_pick_n = board_wifi_parse(board_wifi_kind(), buf, g_nets, (int)(sizeof(g_nets) / sizeof(g_nets[0])));
    }
    g_pick_sel = 0;
    if (g_pick_kind == PICK_BT) { int i; for (i = 1; i < g_pick_n; i++) if (!strcmp(g_bt[i - 1].addr, g_bt_chosen)) g_pick_sel = i; }
    else { int i; for (i = 0; i < g_pick_n; i++) if (!strcmp(g_nets[i].ssid, g_cur_ssid)) g_pick_sel = i; }
    g_dirty = 1;
}

static void pick_open(int kind, uint64_t now)
{
    g_pick_kind = kind;
    g_pick_scroll = 0;
    board_refresh();
    if (kind == PICK_TZ) {
        int k = board_zone_find(g_cur_tz);
        g_pick_n = board_zone_count();
        g_pick_sel = k >= 0 ? k : 0;
    } else {
        g_pick_sel = 0;
        if (kind == PICK_BT) bt_chosen(g_bt_chosen, sizeof(g_bt_chosen));
        scan_start();
    }
    g_mode = M_PICK; g_overlay_a = 0; g_open_at = now; g_sel_since = now;
    g_dirty = 1;
}

/* SSH: off -> on, on -> off. The password stays the user's choice; while it is still the card
 * image's published one, the result warns that anyone on the network could log in with it */
static void ssh_action(uint64_t now)
{
    if (g_bg_pid > 0) { toast("terminal", "A change is still under way: a moment", now); return; }
    ssh_refresh();
    if (g_ssh_on) {
        bg_run("sudo -n /usr/sbin/pspexp-pkg-apt ssh off </dev/null", "terminal", "SSH off", "Could not turn SSH off (see /tmp/pspexp-launcher.log)");
        toast("terminal", "Turning SSH off...", now);
    } else {
        char ok[160], host[64] = "board";
        const char *user = getenv("USER");
        gethostname(host, sizeof(host));
        snprintf(ok, sizeof(ok), "SSH on: ssh %s@%s.local", user && *user ? user : "user", host);
        bg_run("sudo -n /usr/sbin/pspexp-pkg-apt ssh on </dev/null", "terminal", ok, "Could not turn SSH on (see /tmp/pspexp-launcher.log)");
        if (g_ssh_default)
            bg_warn("The card's password still: anyone on your network can log in. Change it: passwd in Apps > Terminal");
        toast("terminal", "Turning SSH on...", now);
    }
    g_dirty = 1;
}

/* with_password: the keyboard's password; otherwise a saved or open network */
static void wifi_join(int with_password, uint64_t now)
{
    char cmd[1024], ok[128], fail[128];
    board_wifi_connect_command(board_wifi_kind(), &g_join, with_password, cmd, sizeof(cmd));
    snprintf(ok, sizeof(ok), "Connected to %s", g_join.ssid);
    snprintf(fail, sizeof(fail), "Could not join %s%s", g_join.ssid,
             !g_join.secure ? "" : with_password ? " (the password?)" : " (square: a new password)");
    bg_run_env(cmd, "wifi", ok, fail, "PSPEXP_WIFI_PSK", with_password ? g_kbd.buf : NULL);
    memset(g_kbd.buf, 0, sizeof(g_kbd.buf));   /* the password lives on only in the board's network tool */
    toast("wifi", "Joining the network...", now);
    g_mode = M_BROWSE;
    g_dirty = 1;
}

/* START in a picker: the row's (join, use, set; a new password, forget), then Scan again */
static void pick_menu_open(void)
{
    ui_menu_begin(&g_menu, g_pick_kind == PICK_TZ ? "Time zone" : g_pick_kind == PICK_BT ? "Sound" : "Wi-Fi");
    if (g_pick_n && !g_scanning) {
        ui_menu_add(&g_menu, MN_PICK_GO, "check", g_pick_kind == PICK_TZ ? "Set" : g_pick_kind == PICK_BT ? "Use" : "Join", NULL, 0, 0);
        if (g_pick_kind == PICK_WIFI && g_nets[g_pick_sel].secure && g_nets[g_pick_sel].saved) ui_menu_add(&g_menu, MN_PICK_SQUARE, "pencil", "New password", NULL, 0, 0);
        if (g_pick_kind == PICK_BT && g_pick_sel > 0 && g_pick_sel - 1 < g_pick_n - 1 && g_bt[g_pick_sel - 1].paired) ui_menu_add(&g_menu, MN_PICK_SQUARE, "trash-2", "Forget", NULL, 0, 0);
    }
    if (g_pick_kind != PICK_TZ) ui_menu_add(&g_menu, MN_PICK_SCAN, "refresh-cw", "Scan again", NULL, g_menu.n > 0, 0);
    if (!g_menu.n) g_menu.open = 0;
    g_dirty = 1;
}

static void pick_input(uint32_t pressed, int up, int down, uint64_t now)
{
    if (up && g_pick_n) { g_pick_sel = (pressed & PSPEXP_BUTTON_UP) ? (g_pick_sel + g_pick_n - 1) % g_pick_n : (g_pick_sel > 0 ? g_pick_sel - 1 : 0); g_sel_since = now; }
    if (down && g_pick_n) { g_pick_sel = (pressed & PSPEXP_BUTTON_DOWN) ? (g_pick_sel + 1) % g_pick_n : (g_pick_sel + 1 < g_pick_n ? g_pick_sel + 1 : g_pick_sel); g_sel_since = now; }
    if (pressed & PSPEXP_BUTTON_LTRIGGER) g_pick_sel = g_pick_sel > 6 ? g_pick_sel - 6 : 0;
    if ((pressed & PSPEXP_BUTTON_RTRIGGER) && g_pick_n) g_pick_sel = g_pick_sel + 6 < g_pick_n ? g_pick_sel + 6 : g_pick_n - 1;
    if (pressed & PSPEXP_BUTTON_CIRCLE) { g_mode = M_BROWSE; g_dirty = 1; return; }
    if (pressed & PSPEXP_BUTTON_START) { pick_menu_open(); return; }   /* the menu: the row's, then Scan again */
    if (g_pick_kind == PICK_BT && g_pick_n && !g_scanning && (pressed & (PSPEXP_BUTTON_CROSS | PSPEXP_BUTTON_SQUARE))) {
        char cmd[160], ok[128], fail[160];
        if (g_pick_sel == 0) {   /* the PSP */
            if (!(pressed & PSPEXP_BUTTON_CROSS)) return;
            bg_run("pspexp-bt psp", "volume-2", "Sound: the PSP", "Could not switch the sound");
            g_bt_chosen[0] = 0;
        } else {
            const BtDev *d = &g_bt[g_pick_sel - 1];
            if (pressed & PSPEXP_BUTTON_SQUARE) {
                if (!d->paired) return;
                snprintf(cmd, sizeof(cmd), "pspexp-bt forget '%s'", d->addr);
                snprintf(ok, sizeof(ok), "Forgot %s", d->name);
                bg_run(cmd, "bluetooth-off", ok, "Could not forget it");
                if (!strcmp(d->addr, g_bt_chosen)) g_bt_chosen[0] = 0;
            } else {
                snprintf(cmd, sizeof(cmd), "pspexp-bt use '%s'", d->addr);
                snprintf(ok, sizeof(ok), "Sound: %s", d->name);
                snprintf(fail, sizeof(fail), "Could not connect %s%s", d->name, d->paired ? "" : " (in pairing mode?)");
                bg_run(cmd, "bluetooth-connected", ok, fail);
                toast("bluetooth", d->paired ? "Connecting..." : "Pairing...", now);
            }
        }
        g_mode = M_BROWSE;
        g_dirty = 1;
        return;
    }
    if ((pressed & PSPEXP_BUTTON_CROSS) && g_pick_n && !g_scanning) {
        if (g_pick_kind == PICK_TZ) {
            const BoardZone *z = board_zone(g_pick_sel);
            char cmd[400], ok[128];
            board_tz_command(z, cmd, sizeof(cmd));
            snprintf(ok, sizeof(ok), "Time zone: %s (%s)", z->label, z->offset);
            bg_run(cmd, "clock", ok, "Could not set the time zone (see /tmp/pspexp-launcher.log)");
            setenv("TZ", z->posix, 1);   /* the clock follows now */
            tzset();
            g_minute = -1;
            g_mode = M_BROWSE;
        } else {
            g_join = g_nets[g_pick_sel];
            if (g_join.secure && !g_join.saved) { ui_kbd_reset(&g_kbd, NULL, 1, "Join"); g_mode = M_TEXT; g_overlay_a = 0; }
            else wifi_join(0, now);   /* saved: what the board keeps; or open */
        }
    }
    /* square: a (new) password for a secured network, saved or not */
    if (g_pick_kind == PICK_WIFI && (pressed & PSPEXP_BUTTON_SQUARE) && g_pick_n && !g_scanning && g_nets[g_pick_sel].secure && g_nets[g_pick_sel].saved) {   /* (not saved: cross asks anyway) */
        g_join = g_nets[g_pick_sel];
        ui_kbd_reset(&g_kbd, NULL, 1, "Join"); g_mode = M_TEXT; g_overlay_a = 0;
    }
    g_dirty = 1;
}

static void text_input(uint32_t pressed, int up, int down, int left, int right, uint64_t now)
{
    int r = ui_kbd_input(&g_kbd, pressed, up, down, left, right);
    if (r == UIK_DONE) { wifi_join(1, now); return; }
    if (r == UIK_CANCEL) { memset(g_kbd.buf, 0, sizeof(g_kbd.buf)); g_mode = M_PICK; }
    g_dirty = 1;
}

/* ---- input -------------------------------------------------------------------------------- */

static void show_cat(int c, int dir, uint64_t now)
{
    if (c == g_cur) return;
    g_cur = c;
    if (g_cat[c].tab) g_last_tab[g_cat[c].tab] = c;
    g_list_anim = 0; g_list_dir = (float)dir;
    memset(g_expand, 0, sizeof(g_expand));
    g_sel_since = now;
    g_dirty = 1;
}

/* up and down the rail: k is a position on it, wrapping */
static void select_cat(int k, int dir, uint64_t now)
{
    int n = rail_n();
    if (!n) return;
    k = ((k % n) + n) % n;
    {
        int t = g_cat[rail_cat(k)].tab, last = t ? g_last_tab[t] : -1;
        if (last >= 0 && last < g_ncat && g_cat[last].tab == t) show_cat(last, dir, now);   /* (a section with tabs: the tab it was on) */
        else show_cat(rail_cat(k), dir, now);
    }
}

/* a section's tabs (Games', Settings'), L / R: the next (d 1) or previous (-1), wrapping */
static void tab_step(int d, uint64_t now)
{
    int i, k, t = g_cat[g_cur].tab;
    for (k = 1; k < g_ncat; k++) {
        i = ((g_cur + d * k) % g_ncat + g_ncat) % g_ncat;
        if (g_cat[i].tab == t) { show_cat(i, d, now); return; }
    }
}

/* a setting's kind (2026-10-30): a switch (drawn as one under its tile), a choice (its value in a
 * pill; cross the next), a page (cross opens it: a list, About), or a thing done */
enum { ROW_DO, ROW_SWITCH, ROW_CHOICE, ROW_PAGE };
static int row_kind(const LEntry *e)
{
    static const char *const SW[] = { "a:clips", "a:boxart", "a:pspclock", "a:plugin", "a:ssh", NULL },
                             *const CH[] = { "a:cliplen", "a:quality", NULL },
                             *const PG[] = { "a:wifinet", "a:sound", "a:tz", "a:about", NULL };
    int i;
    for (i = 0; SW[i]; i++) if (!strcmp(e->id, SW[i])) return ROW_SWITCH;
    for (i = 0; CH[i]; i++) if (!strcmp(e->id, CH[i])) return ROW_CHOICE;
    for (i = 0; PG[i]; i++) if (!strcmp(e->id, PG[i])) return ROW_PAGE;
    return ROW_DO;
}
static int switch_on(const LEntry *e)
{
    const char *v = action_value(e);
    return v && strcmp(v, "Off") && strcmp(v, "Not on the PSP");
}
/* a choice's value short, for its pill ("15 s"), and what cross makes it (d 1: the next) */
static void choice_text(const LEntry *e, int d, char *out, size_t n)
{
    if (!strcmp(e->id, "a:cliplen")) { int v = g_clip_secs; if (d) v = v == 15 ? 30 : v == 30 ? 60 : 15; snprintf(out, n, "%d s", v); }
    else if (!strcmp(e->id, "a:quality")) snprintf(out, n, "%s", CLIP_QUALITY[(g_clip_quality + d) % 3]);
    else snprintf(out, n, "%s", action_value(e) ? action_value(e) : "");
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

/* ---- START: the menu (the highlighted entry's actions, then Search and Notifications, Restart
 * PSPExp and Power off, each asked again) ---- */

/* favourites are games (pinned first in All since 2026-10-30: there is no Favourites section) */
static int can_fav(const LEntry *e) { return e && (e->type == LT_GAME || e->type == LT_ROM); }
static void fav_toggle(LEntry *e, uint64_t now)
{
    char id[160];
    int on;
    snprintf(id, sizeof(id), "%s", e->id);
    on = lib_fav_toggle(&L, id);
    cats_build();
    toast("star", on ? "Added to favourites" : "Removed from favourites", now);
}
/* triangle: a game's, a ROM's, an app's Details card; what a setting or a built-in app is, in a toast */
static void show_more(LEntry *e, uint64_t now)
{
    if (e->type == LT_ACTION || is_builtin(e)) toast(e->icon, e->tags, now);
    else { g_det = e; g_mode = M_DETAILS; g_overlay_a = 0; g_open_at = now; g_dirty = 1; }
}

static void menu_open(uint64_t now)
{
    LEntry *e = g_mode == M_BROWSE ? cur_entry() : NULL;
    (void)now;
    ui_menu_begin(&g_menu, e ? e->title : "PSPExp");
    if (e) {
        int game = e->type == LT_ROM || e->type == LT_GAME;
        ui_menu_add(&g_menu, MN_GO, game ? "play" : e->type == LT_ACTION ? "check" : "folder-open", game ? "Play" : e->type == LT_ACTION ? "Select" : "Open", NULL, 0, 0);
        if (can_fav(e)) {
            ui_menu_add(&g_menu, MN_FAV, "star", e->fav ? "Remove from favourites" : "Add to favourites", NULL, 0, 0);
            ui_menu_add(&g_menu, MN_DETAILS, "info", "Details", NULL, 0, 0);
        }
    }
    ui_menu_add(&g_menu, MN_SEARCH, "search", "Search", NULL, e != NULL, 0);
    ui_menu_add(&g_menu, MN_NOTIFY, "bell", "Notifications", NULL, 0, 0);
    ui_menu_add(&g_menu, MN_RESTART, "refresh-cw", "Restart PSPExp", NULL, 1, 1);
    ui_menu_add(&g_menu, MN_POWER, "power", "Power off the board", NULL, 0, 1);
    g_dirty = 1;
}

static void menu_choose(int id, uint64_t now)
{
    LEntry *e = g_mode == M_BROWSE ? cur_entry() : NULL;
    g_dirty = 1;
    switch (id) {
    case MN_GO: if (e) launch(e, now); break;
    case MN_FAV: if (e) fav_toggle(e, now); break;
    case MN_DETAILS: if (e) show_more(e, now); break;
    case MN_SEARCH: search_open(now); break;
    case MN_NOTIFY: snprintf(g_app_req, sizeof(g_app_req), "notifications"); break;
    case MN_RESTART:   /* asked again, in the menu */
        ui_menu_begin(&g_menu, "Restart PSPExp?");
        ui_menu_add(&g_menu, MN_RESTART_YES, "refresh-cw", "Restart", NULL, 0, 0);
        ui_menu_add(&g_menu, MN_CANCEL, "x", "Cancel", NULL, 0, 0);
        break;
    case MN_POWER:
        ui_menu_begin(&g_menu, "Power off the board?");
        ui_menu_add(&g_menu, MN_POWER_YES, "power", "Power off", NULL, 0, 0);
        ui_menu_add(&g_menu, MN_CANCEL, "x", "Cancel", NULL, 0, 0);
        break;
    case MN_PICK_GO: pick_input(PSPEXP_BUTTON_CROSS, 0, 0, now); break;
    case MN_PICK_SQUARE: pick_input(PSPEXP_BUTTON_SQUARE, 0, 0, now); break;
    case MN_PICK_SCAN: scan_start(); break;
    case MN_RESTART_YES: g_console_restart_req = 1; toast("refresh-cw", "Restarting PSPExp...", now); break;
    case MN_POWER_YES: {   /* the Settings action, its "again" already given */
        int i;
        for (i = 0; i < g_nactions; i++) if (!strcmp(g_actions[i].id, "a:poweroff")) {
            snprintf(g_confirm, sizeof(g_confirm), "poweroff"); g_confirm_until = now + 1000000u;
            run_action(&g_actions[i], now);
        }
        break;
    }
    default: break;
    }
}

void launcher_input(uint32_t b, uint32_t pressed, int up, int down, int left, int right, uint64_t now)
{
    LEntry *e;
    if (b || pressed || up || down || left || right) { if (launcher_still(now)) g_dirty = 1; g_input_at = now; }   /* (scrolling starts again) */
    if (g_running || !g_ncat) return;
    if (g_sheet) {
        if (pressed & PSPEXP_BUTTON_CROSS) { g_sheet = 0; start_entry(&g_sheet_e, now); }
        else if (pressed & PSPEXP_BUTTON_CIRCLE) { g_sheet = 0; g_dirty = 1; }
        return;
    }
    if (g_menu.open) {   /* START's menu over a section */
        int id = ui_menu_input(&g_menu, pressed, up, down);
        if (id >= 0) menu_choose(id, now);
        g_dirty = 1;
        return;
    }
    if (g_mode == M_SEARCH) { search_input(pressed, up, down, left, right, now); return; }
    if (g_mode == M_DETAILS) { details_input(pressed, now); return; }
    if (g_mode == M_PICK) { pick_input(pressed, up, down, now); return; }
    if (g_mode == M_TEXT) { text_input(pressed, up, down, left, right, now); return; }
    if (g_mode == M_ABOUT) {
        if (pressed & (PSPEXP_BUTTON_CIRCLE | PSPEXP_BUTTON_CROSS | PSPEXP_BUTTON_TRIANGLE | PSPEXP_BUTTON_START)) { g_mode = M_BROWSE; g_dirty = 1; }
        return;
    }
    if (pressed & PSPEXP_BUTTON_START) { menu_open(now); return; }
    if (pressed & PSPEXP_BUTTON_SELECT) { snprintf(g_app_req, sizeof(g_app_req), "notifications"); return; }   /* the notification centre (also in START's menu) */
    /* up and down the rail (Games, Apps, Settings), left and right along the shelf; L / R the tabs
     * (Games', Settings'; Apps: five along) */
    {
        const Cat *c = &g_cat[g_cur];
        if (up) select_cat(rail_pos(g_cur) - 1, -1, now);
        if (down) select_cat(rail_pos(g_cur) + 1, 1, now);
        if (left) move_sel(-1, (pressed & PSPEXP_BUTTON_LEFT) != 0, now);   /* wraps on a fresh press, stops while repeating */
        if (right) move_sel(1, (pressed & PSPEXP_BUTTON_RIGHT) != 0, now);
        if (pressed & PSPEXP_BUTTON_LTRIGGER) { if (c->tab) tab_step(-1, now); else move_sel(-5, 0, now); }
        if (pressed & PSPEXP_BUTTON_RTRIGGER) { if (c->tab) tab_step(1, now); else move_sel(5, 0, now); }
    }
    /* (circle: nothing; it went back from a Settings group's page or to the home screen) */
    e = cur_entry();
    if (!e) return;
    if (pressed & PSPEXP_BUTTON_CROSS) launch(e, now);
    if ((pressed & PSPEXP_BUTTON_SQUARE) && can_fav(e)) fav_toggle(e, now);
    if (pressed & PSPEXP_BUTTON_TRIANGLE) show_more(e, now);
}

/* ---- status sources ------------------------------------------------------------------------ */

static int read_int(const char *p, int *v) { FILE *f = fopen(p, "r"); int ok; if (!f) return -1; ok = fscanf(f, "%d", v) == 1; fclose(f); return ok ? 0 : -1; }

static void status_poll(uint64_t now)
{
    static const char *dirs[] = { "/sys/class/power_supply/axp20x-battery", "/sys/class/power_supply/battery", "/sys/class/power_supply/BAT0", NULL };
    int bat = -1, chg = 0, wifi = -1, wired = 0, i;
    char p[160];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    if (tmv.tm_min != g_minute) { g_minute = tmv.tm_min; g_dirty = 1; }
    {   /* Bluetooth sound became possible (the controller came up after the console): Settings > Sound */
        static uint64_t at;
        static int had = -1;
        if (!at || now - at > 10000000u) {
            int has = bt_available();
            at = now;
            if (had >= 0 && has != had) { cats_build(); g_dirty = 1; }
            had = has;
        }
    }
    {   /* the Bluetooth sound and its volume: two small files, a few times a second */
        static uint64_t at;
        if (!at || now - at > 300000u) {
            int v, on = bt_indicator(&v);
            at = now;
            if (on != g_snd_on || v != g_snd_vol) { g_snd_on = on; g_snd_vol = v; g_dirty = 1; }
        }
    }
    {   /* the PSP's battery: a new charge or state (the charger in or out) redraws the bar; an idle
         * launcher draws nothing otherwise */
        static int shown_pct = -2, shown_flags = -1;
        PspExpPowerStatus ps;
        int pct = -1, fl = -1;
        if (g_ctx && pspexp_power_status(g_ctx, &ps, NULL) == 0) { pct = ps.percent; fl = ps.flags; }
        if (pct != shown_pct || fl != shown_flags) { shown_pct = pct; shown_flags = fl; g_dirty = 1; }
    }
    if (now - g_status_at < 3000000u && g_bat != -2) return;
    g_status_at = now;
#ifdef __APPLE__
    (void)dirs; (void)i; (void)p; (void)read_int;
    bat = mac_battery(&chg);
    {   /* no routing table to read: en0 is a MacBook's Wi-Fi, any other en with an address a cable or adapter */
        struct ifaddrs *ifs, *it;
        if (getifaddrs(&ifs) == 0) {
            for (it = ifs; it; it = it->ifa_next) {
                if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET || strncmp(it->ifa_name, "en", 2)) continue;
                if (!strcmp(it->ifa_name, "en0")) wifi = 2;   /* connected, strength unknown */
                else wired = 1;
            }
            freeifaddrs(ifs);
        }
        if (wired) wifi = -1;
    }
#else
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
    /* the network: the interface of the default route; a cable, or Wi-Fi
     * with bars from the kernel's link quality (the Omega2+'s driver leaves
     * that empty: its own RSSI, asked every 15 s, never during a game) */
    {
        char iface[32] = "", line[256];
        FILE *f = fopen("/proc/net/route", "r");
        wired = 0;
        if (f) {   /* the default route with the lowest metric whose interface has its link: a USB
                    * network's route left after the cable came out had shown a cable, not Wi-Fi */
            long best = -1;
            while (fgets(line, sizeof(line), f)) {
                char name[32], cpath[96], c = '1'; unsigned long dest, gw, flags, refcnt, use, metric;
                FILE *cf;
                if (sscanf(line, "%31s %lx %lx %lx %lu %lu %lu", name, &dest, &gw, &flags, &refcnt, &use, &metric) != 7 || dest != 0) continue;
                snprintf(cpath, sizeof(cpath), "/sys/class/net/%s/carrier", name);
                if ((cf = fopen(cpath, "r"))) { c = (char)fgetc(cf); fclose(cf); }
                if (c != '1') continue;
                if (best < 0 || (long)metric < best) { best = (long)metric; snprintf(iface, sizeof(iface), "%s", name); }
            }
            fclose(f);
        }
        if (iface[0]) {
            char w[128];
            struct stat st;
            snprintf(w, sizeof(w), "/sys/class/net/%s/wireless", iface);
            int wireless = stat(w, &st) == 0 || !strncmp(iface, "wlan", 4) || !strncmp(iface, "apcli", 5) || !strncmp(iface, "wl", 2);
            if (!wireless) wired = 1;
            else {
                if ((f = fopen("/proc/net/wireless", "r"))) {
                    while (fgets(line, sizeof(line), f)) {
                        char name[32]; int s; float link;
                        if (sscanf(line, " %31[^:]: %d %f", name, &s, &link) == 3 && !strcmp(name, iface) && link > 0) {
                            wifi = link >= 55 ? 3 : link >= 35 ? 2 : 1;
                            break;
                        }
                    }
                    fclose(f);
                }
                if (wifi < 0 && !strncmp(iface, "apcli", 5)) {   /* MediaTek (Omega2+): iwpriv's RSSI */
                    static uint64_t asked; static int last = 2;
                    if (!g_running && (!asked || now - asked > 15000000u)) {
                        FILE *pp;
                        char cmd[96];
                        asked = now;
                        snprintf(cmd, sizeof(cmd), "iwpriv %s stat 2>/dev/null", iface);
                        if ((pp = popen(cmd, "r"))) {
                            while (fgets(line, sizeof(line), pp)) {
                                int rssi;
                                if (!strncmp(line, "RSSI", 4) && strchr(line, '=') && sscanf(strchr(line, '=') + 1, "%d", &rssi) == 1 && rssi < 0)
                                    last = rssi >= -60 ? 3 : rssi >= -70 ? 2 : 1;   /* connected: at least one bar (0 is Wi-Fi off) */
                            }
                            pclose(pp);
                        }
                    }
                    wifi = last;
                }
                if (wifi < 0) wifi = 2;   /* connected, strength unknown */
            }
        } else {   /* no route: Wi-Fi off if there is a wireless interface at all */
            struct stat st;
            if (stat("/sys/class/net/wlan0", &st) == 0 || stat("/sys/class/net/apcli0", &st) == 0) wifi = 0;
        }
    }
#endif
    if (bat != g_bat || chg != g_chg || wifi != g_wifi || wired != g_wired) { g_bat = bat; g_chg = chg; g_wifi = wifi; g_wired = wired; g_dirty = 1; }
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
        char mark[700];
        last_check = now;
        /* pspexp-pkg is publishing an install: never read a half-published one
         * (docs/Package_Manager.md §5.3); the next check sees it whole */
        snprintf(mark, sizeof(mark), "%s/state/publishing", L.root);
        if (access(mark, F_OK) != 0 && lib_changed(&L)) library_changed(now);
    }
    bg_poll(now);
    scan_poll();
    {   /* the music: its title and pause shown in the top bar and on the home screen */
        char t[160] = "";
        int paused = 0, on = music_now(t, sizeof(t), &paused);
        if (on != g_np_on || paused != g_np_paused || strcmp(t, g_np_title)) {
            g_np_on = on; g_np_paused = paused; snprintf(g_np_title, sizeof(g_np_title), "%s", t);
            g_dirty = 1;
        }
    }
    fetch_poll(now);
    status_poll(now);
    if (g_confirm[0] && now > g_confirm_until) g_confirm[0] = 0;
    if ((g_mode == M_SEARCH || g_mode == M_TEXT) && !launcher_still(now) && (int)((now / 530000u) & 1) != g_caret) { g_caret = (int)((now / 530000u) & 1); g_dirty = 1; }
    if (g_mode == M_PICK && g_scanning && (int)((now / 300000u) & 3) != g_caret) { g_caret = (int)((now / 300000u) & 3); g_dirty = 1; }
    if (g_mode == M_ABOUT && now - g_about_at > 3000000u) { about_collect(); g_about_at = now; g_dirty = 1; }
    return g_dirty || g_anim;
}

/* ---- drawing ------------------------------------------------------------------------------ */


static void board_detect(void)
{
    char model[128] = "";
    FILE *f;
    struct utsname u;
    if (g_board.done) return;
    g_board.done = 1;
    g_board.icon = "computer"; g_board.colour = C_SUB;
    snprintf(g_board.name, sizeof(g_board.name), "This board");
    if ((f = fopen("/proc/device-tree/model", "r"))) { if (!fgets(model, sizeof(model), f)) model[0] = 0; fclose(f); }
    if (!model[0] && (f = fopen("/proc/cpuinfo", "r"))) {   /* OpenWrt: "machine : Onion Omega2+" */
        char l[256];
        while (fgets(l, sizeof(l), f)) if (!strncmp(l, "machine", 7) && strchr(l, ':')) { snprintf(model, sizeof(model), "%s", strchr(l, ':') + 1); break; }
        fclose(f);
    }
    if (strstr(model, "Clockwork") || strstr(model, "clockwork")) { g_board.icon = "gamepad-directional"; g_board.colour = 0xf59e0b; snprintf(g_board.name, sizeof(g_board.name), "GameShell"); }
    else if (strstr(model, "Omega")) { g_board.icon = "microchip"; g_board.colour = 0x34d399; snprintf(g_board.name, sizeof(g_board.name), "Omega2+"); }
    else if (strstr(model, "Raspberry")) {
        char *r;
        g_board.icon = (strstr(model, " 400") || strstr(model, " 500")) ? "keyboard" : "circuit-board";
        g_board.colour = 0xf43f5e;
        snprintf(g_board.name, sizeof(g_board.name), "%s", strstr(model, "Raspberry"));
        if ((r = strstr(g_board.name, " Rev"))) *r = 0;              /* "Raspberry Pi 400 Rev 1.0": "Raspberry Pi 400" */
        if ((r = strstr(g_board.name, " Model B"))) memmove(r, r + 8, strlen(r + 8) + 1);   /* "Raspberry Pi 4 Model B": "Raspberry Pi 4" */
    } else if (uname(&u) == 0 && !strcmp(u.sysname, "Darwin")) { g_board.icon = "laptop"; g_board.colour = 0xa5b4fc; snprintf(g_board.name, sizeof(g_board.name), "Mac"); }
}

/* the top bar (2026-10-30: only what tells something): the clock, the bell while there is news;
 * on the right the music (loaded), Bluetooth sound, the board's network, the board's battery when
 * it has one (its icon before it), the PSP's battery */
static void draw_status(void)
{
    char s[64];
    time_t t = time(NULL);
    struct tm tmv;
    int x, w;
    PspExpPowerStatus p;
    char b[160];
    localtime_r(&t, &tmv);
    strftime(s, sizeof(s), "%H:%M", &tmv);
    w = gfx_text(FONT_TITLE, 16, 4, s, C_TEXT, 255, 0);
    x = 16 + w + 12;
    {
        int unread = notify_unread();
        if (unread) {
            gfx_icon("bell-dot", 16, x, 6, C_ACCENT, 255);
            snprintf(s, sizeof(s), "%d", unread > 99 ? 99 : unread);
            gfx_text(FONT_BADGE, x + 19, 9, s, C_ACCENT, 255, 0);
        }
    }
    x = SCREEN_W - 16;
    if (psp_battery(b, sizeof(b), &p)) {   /* the PSP's, once its runtime has reported it */
        int chg = (p.flags & (PSPEXP_POWER_CHARGING | PSPEXP_POWER_EXTERNAL)) != 0;
        const char *ic = chg ? "battery-charging" : p.percent > 70 ? "battery-full" : p.percent > 35 ? "battery-medium" : p.percent > 12 ? "battery-low" : "battery-warning";
        snprintf(s, sizeof(s), "%d%%", p.percent);
        x -= gfx_text_width(FONT_SMALL, s);
        gfx_text(FONT_SMALL, x, 8, s, C_SUB, 255, 0);
        x -= 20;
        gfx_icon(ic, 16, x, 6, p.percent <= 12 && !chg ? 0xf87171 : C_SUB, 255);
        x -= 12;
    }
    if (g_bat >= 0) {   /* the board's own battery (the GameShell's), its icon before it */
        const char *ic = g_chg ? "battery-charging" : g_bat > 70 ? "battery-full" : g_bat > 35 ? "battery-medium" : g_bat > 12 ? "battery-low" : "battery-warning";
        board_detect();
        snprintf(s, sizeof(s), "%d%%", g_bat);
        x -= gfx_text_width(FONT_SMALL, s);
        gfx_text(FONT_SMALL, x, 8, s, C_SUB, 255, 0);
        x -= 20;
        gfx_icon(ic, 16, x, 6, g_bat <= 12 && !g_chg ? 0xf87171 : C_SUB, 255);
        x -= 18;
        gfx_icon(g_board.icon, 16, x, 6, g_board.colour, 255);
        x -= 12;
    }
    if (g_wired) { x -= 16; gfx_icon("ethernet-port", 16, x, 6, C_SUB, 255); x -= 10; }
    else if (g_wifi >= 0) {
        const char *ic = g_wifi >= 3 ? "wifi" : g_wifi == 2 ? "wifi-high" : g_wifi == 1 ? "wifi-low" : "wifi-off";
        x -= 16; gfx_icon(ic, 16, x, 6, C_SUB, 255); x -= 10;
    }
    if (g_snd_on) {   /* the sound on a Bluetooth device: its volume (the PSP's volume buttons set it) */
        if (g_snd_vol >= 0) {
            snprintf(s, sizeof(s), "%d%%", g_snd_vol);
            x -= gfx_text_width(FONT_SMALL, s);
            gfx_text(FONT_SMALL, x, 8, s, C_SUB, 255, 0);
            x -= 4;
        }
        x -= 16; gfx_icon("headphones", 16, x, 6, C_SUB, 255); x -= 10;
    }
    if (g_np_on) { x -= 16; gfx_icon(g_np_paused ? "pause" : "music", 16, x, 6, g_np_paused ? C_SUB : C_ACCENT, 255); }   /* music loaded: a note, a pause sign while paused */
}

static void draw_rail(float dt)
{
    static float on = 1.0f;   /* 1: up / down move along it (a section's page), lit; 0: a sub-page, dimmed */
    int i, k, first, n = rail_n(), here = rail_pos(g_cur);
    float dim;
    first = here - RAIL_SHOWN / 2;
    if (first > n - RAIL_SHOWN) first = n - RAIL_SHOWN;
    if (first < 0) first = 0;
    g_rail_top = ease(g_rail_top, (float)first, 14.0f, dt, 0.01f);
    on = ease(on, g_cat[g_cur].hidden && !g_cat[g_cur].tab ? 0.0f : 1.0f, 12.0f, dt, 0.01f);
    if (on > 0.01f && on < 0.99f) g_dirty = 1;   /* (still easing) */
    dim = 0.35f + 0.65f * on;
    if (on > 0.02f) gfx_nine(NINE_R10, 3, RAIL_TOP - 4, RAIL_W - 6, RAIL_SHOWN * RAIL_PITCH + 6, 0xffffff, (int)(on * 12));   /* (lit: a faint panel) */
    gfx_rect(RAIL_W, RAIL_TOP, 1, RAIL_SHOWN * RAIL_PITCH, 0xffffff, (int)(8 + 14 * on));
    gfx_set_clip(0, RAIL_TOP - 4, RAIL_W, RAIL_SHOWN * RAIL_PITCH + 6);
    for (i = 0, k = 0; i < g_ncat; i++) {
        Cat *c = &g_cat[i];
        float lift;
        int size, cy, x, y;
        if (c->hidden) continue;
        lift = g_lift[k] = ease(g_lift[k], k == here ? 1.0f : 0.0f, 16.0f, dt, 0.01f);
        size = RAIL_TILE + (int)lroundf(lift * 2);
        cy = RAIL_TOP + RAIL_PITCH / 2 + (int)lroundf((k - g_rail_top) * RAIL_PITCH) - 2;
        x = (RAIL_W - size) / 2; y = cy - size / 2;
        k++;
        if (cy < RAIL_TOP - RAIL_PITCH || cy > RAIL_TOP + (RAIL_SHOWN + 1) * RAIL_PITCH) continue;
        if (lift > 0.02f) {   /* one blue highlight everywhere (2026-10-30: the sections' tints looked muddy over the waves) */
            gfx_shadow(NINE_SHADOW10, x, y, size, size, 0x40a0ff, (int)(lift * 200 * on));
            gfx_nine(NINE_R10, x, y, size, size, 0x14305c, (int)(lift * 255 * dim));
        }
        cat_icon(c, 18, (RAIL_W - 18) / 2, cy - 9, c->col, (int)((1 - lift) * 220 * dim));
        if (lift > 0.02f) cat_icon(c, 18, (RAIL_W - 18) / 2, cy - 9, 0xffffff, (int)(lift * 255 * dim));
    }
    gfx_set_clip(0, 0, SCREEN_W, SCREEN_H);
    /* more above or below */
    if (g_rail_top > 0.05f) gfx_icon("chevron-up", 12, (RAIL_W - 12) / 2, RAIL_TOP - 12, C_MUTED, 200);
    if (g_rail_top < (float)(n - RAIL_SHOWN) - 0.05f) gfx_icon("chevron-down", 12, (RAIL_W - 12) / 2, RAIL_TOP + RAIL_SHOWN * RAIL_PITCH, C_MUTED, 200);
}

static void human_size(long long b, char *out, size_t n)
{
    if (b >= 1048576) snprintf(out, n, "%.1f MB", (double)b / 1048576.0);
    else snprintf(out, n, "%lld KB", (b + 1023) / 1024);
}

/* the line under a game's or an app's description, in plain words (2026-10-30): what it is and
 * when it was played; sizes, dump tags, paths and who draws it are in Details */
static void detail_line(const LEntry *e, char *s, size_t n)
{
    int r = recent_rank(e);
    const char *played = r == 0 ? "  ·  played last" : r < 1000 ? "  ·  played lately" : "";
    switch (e->type) {
    case LT_ROM:
        if (is_psp_sys(e->sys) && e->tags[0]) {   /* a PSP game: its region ("Europe, En,Fr,De": Europe) */
            char reg[64];
            snprintf(reg, sizeof(reg), "%s", e->tags);
            reg[strcspn(reg, ",")] = 0;
            snprintf(s, n, "PSP  ·  %s%s", reg, played);
        } else snprintf(s, n, "%s%s", L.sys[e->sys].name, played);
        break;
    case LT_GAME:
        board_detect();
        snprintf(s, n, "%s%s", g_board.name, played);
        break;
    case LT_APP:
        if (is_builtin(e)) snprintf(s, n, "Built into PSPExp");
        else snprintf(s, n, "App");
        break;
    case LT_TOOL:
        snprintf(s, n, "Tool");
        break;
    case LT_ACTION:
        if (!strcmp(e->id, "a:wifi")) snprintf(s, n, "%s", wifi_mode_psp() ? "The GameShell is the PSP's access point" : "On the home network; X switches to PSP direct");
        else if (!strcmp(e->id, "a:pspclock")) snprintf(s, n, "%s", g_psp_clock ? "When the PSP connects, a wrong clock is set from this board" : "The PSP keeps its own clock");
        else if (!strcmp(e->id, "a:boxart")) snprintf(s, n, "%s", g_nofetch ? "Generated covers only" : "Covers for ROMs from libretro-thumbnails");
        else if (!strcmp(e->id, "a:clips")) snprintf(s, n, g_clips_keep ? "♪ + R saves the last %d seconds: the board records all the time"
                                                                     : "Turn on for ♪ + R: the last %d seconds as a video", g_clip_secs);
        else if (!strcmp(e->id, "a:cliplen")) snprintf(s, n, "♪ + R saves the last %d seconds%s", g_clip_secs, g_clips_keep ? "" : " (with Clips on)");
        else if (!strcmp(e->id, "a:quality")) snprintf(s, n, "%s", g_clip_quality == 0 ? "The sharpest: up to 17 MB a minute in a busy game"
                                                                   : g_clip_quality == 1 ? "Nearly as sharp: up to 10 MB a minute in a busy game"
                                                                   : "Softer: up to 5 MB a minute in a busy game");
        else if (!strcmp(e->id, "a:plugin")) snprintf(s, n, "%s", g_psp_plugin == PSP_PLUGIN_ON ? "The PSP tab's games start, and PSP games record"
                                                                  : g_psp_plugin == PSP_PLUGIN_OFF ? "Turn on to start the PSP tab's games and record PSP games"
                                                                  : "Not on the PSP's memory stick: cross puts it there and turns it on");
        else snprintf(s, n, "%s", e->tags);
        break;
    default: snprintf(s, n, "%s", e->tags); break;
    }
}

/* a tab's name: All, PSP, the board's, a system's (its short one when the name is long:
 * "Nintendo Entertainment System" is NES) */
static const char *tab_label(const Cat *c)
{
    if (c->kind == CK_SYSTEM && c->sys >= 0 && L.sys[c->sys].shortname[0] && gfx_text_width(FONT_SMALL, c->name) > 110) return L.sys[c->sys].shortname;
    return c->name;
}

/* games and ROMs are discs; apps, tools and settings tiles */
static int is_disc(const LEntry *e) { return e->type == LT_GAME || e->type == LT_ROM; }

static void shelf_item(const LEntry *e, int cx, int cy, float lift, int alpha)
{
    if (is_disc(e)) {
        CoverSpec cs;
        char png[700] = "";
        memset(&cs, 0, sizeof(cs));
        cs.key = e->id; cs.title = e->title; cs.badge = type_badge(e); cs.icon = type_icon(e);
        cs.round = 1;
        if (e->art[0]) cs.art = e->art;
        else if (e->type == LT_ROM && e->sys >= 0) cs.art = rom_art(e, png, sizeof(png));
        /* the small disc fades into the large one, which is lifted */
        if (lift < 0.98f) gfx_disc(&cs, cx, cy, 0, (int)((1 - lift) * alpha));
        if (lift > 0.02f) gfx_disc(&cs, cx, cy - (int)lroundf(lift * 4), 1, (int)(lift * alpha));
    } else {
        int size = 54 + (int)lroundf(lift * 8), x = cx - size / 2, y = cy - size / 2 - (int)lroundf(lift * 3);
        int ip = 26 + (int)lroundf(lift * 2);
        const char *ic = e->icon[0] ? e->icon : type_icon(e);
        if (lift > 0.02f) gfx_shadow(NINE_SHADOW14, x, y, size, size, 0x40a0ff, (int)(lift * 230 * alpha / 255));
        gfx_shadow(NINE_SHADOW10, x, y + 4, size, size, 0x000000, 110 * alpha / 255);
        gfx_nine(NINE_R14, x, y, size, size, 0x16223c, alpha);
        if (lift > 0.02f) gfx_nine(NINE_R14, x, y, size, size, 0x1e3a6e, (int)(lift * alpha));
        gfx_icon(ic, ip, cx - ip / 2, y + (size - ip) / 2, lift > 0.5f ? 0xffffff : 0xb9c8e0, alpha);
        if (e->type == LT_ACTION && row_kind(e) == ROW_SWITCH && action_value(e)) {   /* a setting on / off: a dot in its corner while on, a ring while off */
            int dx = x + size - 13, dyy = y + 7;
            const char *v = action_value(e);
            if (!strcmp(v, "On") || !strcmp(v, "From the board")) gfx_dot(7, dx, dyy, lift > 0.5f ? 0xffffff : C_ACCENT, alpha);
            else { gfx_dot(7, dx, dyy, C_MUTED, alpha); gfx_dot(5, dx + 1, dyy + 1, lift > 0.5f ? 0x1e3a6e : 0x16223c, alpha); }
        }
    }
}

#define C_STAR 0xfbbf24   /* a favourite's solid star, before its name */

/* a section's tabs over the shelf (Games', Settings'): L, the tabs (the one shown in blue; the
 * strip scrolls to keep it in view), R; Games' count on the right */
static void draw_tabs(const Cat *cur, int y, int alpha)
{
    int idx[48], n = 0, i, at = 0, first = 0, x, limit, cw;
    char cnt[48];
    for (i = 0; i < g_ncat && n < 48; i++) if (g_cat[i].tab == cur->tab) { if (&g_cat[i] == cur) at = n; idx[n++] = i; }
    if (cur->tab == 1) {
        snprintf(cnt, sizeof(cnt), "%d %s%s", cur->n, cur->noun, cur->n == 1 ? "" : "s");
        cw = gfx_text_width(FONT_SMALL, cnt);
        gfx_text(FONT_SMALL, SCREEN_W - 16 - cw, y + 2, cnt, C_MUTED, alpha, 0);
    } else cw = -12;
    limit = SCREEN_W - 16 - cw - 12 - 20;   /* (R's badge before the count) */
    for (;;) {   /* the first tab shown: the one shown must fit */
        int w = DETAIL_X + 21 + (first ? 14 : 0);
        for (i = first; i <= at; i++) w += gfx_text_width(FONT_SMALL, tab_label(&g_cat[idx[i]])) + 18 + 2;
        if (w <= limit || first >= at) break;
        first++;
    }
    x = DETAIL_X;
    gfx_nine(NINE_R6, x, y + 2, 16, 14, 0x1e2c4a, alpha);
    gfx_text(FONT_BADGE, x + 5, y + 3, "L", C_SUB, alpha, 0);
    x += 21;
    if (first) { gfx_icon("chevron-left", 12, x - 2, y + 3, C_MUTED, alpha); x += 14; }
    for (i = first; i < n; i++) {
        const char *t = tab_label(&g_cat[idx[i]]);
        int w = gfx_text_width(FONT_SMALL, t) + 18, on = i == at;
        if (x + w > limit) { gfx_icon("chevron-right", 12, x, y + 3, C_MUTED, alpha); x += 14; break; }
        if (on) gfx_nine(NINE_R6, x, y, w, 18, C_ACCENT, alpha * 200 / 255);
        gfx_text(FONT_SMALL, x + 9, y + 1, t, on ? 0xffffff : C_SUB, alpha, 0);
        x += w + 2;
    }
    gfx_nine(NINE_R6, x + 2, y + 2, 16, 14, 0x1e2c4a, alpha);
    gfx_text(FONT_BADGE, x + 7, y + 3, "R", C_SUB, alpha, 0);
}

/* Settings' panel (2026-10-30, from mockups): right of the setting's name, what it is set to and what
 * cross does: a choice shows all its values side by side, the current one filled; on / off a
 * switch with its word; a page its state and a chevron; a thing done a button with its name
 * (Ready's green). Its width, for the name beside it */
static int pill(int right, int cy, const char *t, uint32_t edge, uint32_t fill, uint32_t ink, int chevron, int alpha)
{
    int w = gfx_text_width(FONT_SMALL, t) + 22 + (chevron ? 12 : 0), x = right - w;
    gfx_nine(NINE_R10, x, cy - 11, w, 22, edge, alpha);
    if (fill != edge) gfx_nine(NINE_R10, x + 1, cy - 10, w - 2, 20, fill, alpha);
    gfx_text(FONT_SMALL, x + 11, cy - gfx_line_height(FONT_SMALL) / 2, t, ink, alpha, w - 22);
    if (chevron) gfx_icon("chevron-right", 11, right - 19, cy - 5, ink, alpha);
    return w;
}

static int settings_control(const LEntry *e, int right, int cy, int alpha)
{
    int k = row_kind(e);
    const char *v = action_value(e);
    if (k == ROW_CHOICE) {   /* every value, the current one filled */
        const char *opts[3];
        char a[3][16];
        int n = 3, cur = 0, i, ws[3], w = 0, x;
        if (!strcmp(e->id, "a:cliplen")) { snprintf(a[0], 16, "15 s"); snprintf(a[1], 16, "30 s"); snprintf(a[2], 16, "60 s"); cur = g_clip_secs == 15 ? 0 : g_clip_secs == 60 ? 2 : 1; }
        else { for (i = 0; i < 3; i++) snprintf(a[i], 16, "%s", CLIP_QUALITY[i]); cur = g_clip_quality; }
        for (i = 0; i < n; i++) { opts[i] = a[i]; ws[i] = gfx_text_width(FONT_SMALL, opts[i]) + 18; w += ws[i]; }
        x = right - w - 2;
        gfx_nine(NINE_R10, x - 2, cy - 11, w + 4, 22, 0x30425f, alpha);
        gfx_nine(NINE_R10, x - 1, cy - 10, w + 2, 20, 0x16223c, alpha);
        for (i = 0; i < n; i++) {
            if (i == cur) gfx_nine(NINE_R6, x, cy - 9, ws[i], 18, C_ACCENT, alpha);
            gfx_text(i == cur ? FONT_BODY : FONT_SMALL, x + (ws[i] - gfx_text_width(i == cur ? FONT_BODY : FONT_SMALL, opts[i])) / 2,
                     cy - gfx_line_height(i == cur ? FONT_BODY : FONT_SMALL) / 2, opts[i], i == cur ? 0xffffff : C_SUB, alpha, 0);
            x += ws[i];
        }
        return w + 4;
    }
    if (k == ROW_SWITCH && v && (!strcmp(v, "On") || !strcmp(v, "Off") || !strcmp(v, "From the board"))) {   /* a switch and its word */
        int on = switch_on(e), tw = gfx_text_width(FONT_BODY, on ? "On" : "Off");
        gfx_nine(NINE_R6, right - 34, cy - 9, 34, 18, on ? C_ACCENT : 0x3a4660, alpha);
        gfx_dot(14, on ? right - 16 : right - 32, cy - 7, 0xffffff, alpha);
        gfx_text(FONT_BODY, right - 34 - 8 - tw, cy - gfx_line_height(FONT_BODY) / 2, on ? "On" : "Off", on ? 0xffffff : C_SUB, alpha, 0);
        return 34 + 8 + tw;
    }
    if (!strcmp(e->id, "a:psp-update")) return pill(right, cy, "Install", 0x22c55e, 0x22c55e, 0xffffff, 1, alpha);   /* (Ready: green) */
    if (!strcmp(e->id, "a:console-restart")) return pill(right, cy, "Restart PSPExp", 0x22c55e, 0x22c55e, 0xffffff, 1, alpha);
    if (k == ROW_SWITCH) return pill(right, cy, "Install", C_ACCENT, 0x16223c, C_TEXT, 1, alpha);   /* (the plugin, not on the PSP yet) */
    if (v) return pill(right, cy, v, 0x30425f, 0x16223c, C_TEXT, 1, alpha);   /* a page, or the GameShell's Wi-Fi: where it stands */
    if (k == ROW_PAGE) return pill(right, cy, "Open", 0x30425f, 0x16223c, C_TEXT, 1, alpha);
    return pill(right, cy, !strcmp(e->id, "a:rescan") ? "Refresh" : e->title, 0x8a9bbd, 0x16223c, C_TEXT, 1, alpha);   /* a thing done: its name */
}

static void draw_list(float dt)
{
    Cat *c = &g_cat[g_cur];
    int i, alpha, dy, first, last;
    float target;
    char s[200];
    LEntry *e = cur_entry();
    g_list_anim = ease(g_list_anim, 1.0f, 16.0f, dt, 0.01f);
    alpha = (int)(g_list_anim * 255);
    dy = (int)lroundf((1 - g_list_anim) * 16 * g_list_dir);

    /* header: Games' tabs; a Settings group under Settings; else the section and its size */
    if (c->tab) draw_tabs(c, 29, 255);   /* (the strip stays: only the shelf slides) */
    else if (c->hidden) {
        int x = DETAIL_X + gfx_text(FONT_BODY, DETAIL_X, 32 + dy / 2, "Settings", C_MUTED, alpha, 0) + 2;
        gfx_icon("chevron-right", 14, x, 34 + dy / 2, C_MUTED, alpha);
        gfx_text(FONT_TITLE, x + 16, 30 + dy / 2, c->name, C_TEXT, alpha, 260);
    } else gfx_text(FONT_TITLE, DETAIL_X, 30 + dy / 2, c->name, C_TEXT, alpha, 300);
    if (!c->tab && c->kind != CK_SETTINGS) {
        snprintf(s, sizeof(s), "%d %s%s", c->n, c->noun, c->n == 1 ? "" : "s");
        gfx_text(FONT_SMALL, SCREEN_W - 16 - gfx_text_width(FONT_SMALL, s), 34 + dy / 2, s, C_MUTED, alpha, 0);
    }

    if (!c->n) {
        int psp = c->kind == CK_SYSTEM && is_psp_sys(c->sys);
        gfx_text(FONT_TITLE, DETAIL_X, SHELF_Y - 10 + dy, psp ? "No PSP games yet" : c->tab == 1 ? "No games yet" : "Nothing here yet", C_SUB, alpha, 0);
        gfx_text(FONT_SMALL, DETAIL_X, SHELF_Y + 14 + dy, psp ? "Your ISO, CSO and ZIP games go in roms/psp on the board"
                 : c->tab == 1 ? "Apps > Packages installs games and emulators; ROMs go in roms on the board" : "Installed games and apps appear here", C_MUTED, alpha, 0);
        return;
    }

    /* the shelf scrolls to keep the selection in view, one item of context each side */
    target = (float)(c->sel - (SHELF_SHOWN - 2));
    if (target > (float)(c->sel - 1)) target = (float)(c->sel - 1);
    if (c->scroll > (float)(c->sel - 1)) target = (float)(c->sel - 1);
    else if (c->scroll < (float)(c->sel - (SHELF_SHOWN - 2))) target = (float)(c->sel - (SHELF_SHOWN - 2));
    else target = c->scroll;
    if (target > (float)(c->n - SHELF_SHOWN)) target = (float)(c->n - SHELF_SHOWN);
    if (target < 0) target = 0;
    c->scroll = ease(c->scroll, target, 14.0f, dt, 0.01f);
    for (i = 0; i < c->n && i < 1024; i++) g_expand[i] = ease(g_expand[i], i == c->sel ? 1.0f : 0.0f, 18.0f, dt, 0.01f);
    first = (int)floorf(c->scroll) - 1; if (first < 0) first = 0;
    last = (int)ceilf(c->scroll) + SHELF_SHOWN + 1; if (last > c->n) last = c->n;

    gfx_set_clip(RAIL_W + 1, 26, SCREEN_W - RAIL_W - 1, DETAIL_Y - 30);
    /* the others first, the selection last: its glow over its neighbours */
    for (i = first; i < last; i++) if (i != c->sel) {
        int cx = SHELF_X0 + (int)lroundf((i - c->scroll) * SHELF_PITCH);
        shelf_item(c->it[i], cx, SHELF_Y + dy, i < 1024 ? g_expand[i] : 0, alpha);
    }
    if (c->sel >= first && c->sel < last) shelf_item(c->it[c->sel], SHELF_X0 + (int)lroundf((c->sel - c->scroll) * SHELF_PITCH), SHELF_Y + dy, g_expand[c->sel], alpha);
    /* titles under the items */
    for (i = first; i < last; i++) {
        int cx = SHELF_X0 + (int)lroundf((i - c->scroll) * SHELF_PITCH), sel = i == c->sel, f = sel ? FONT_BODY : FONT_SMALL;
        const char *t = c->it[i]->title;
        int st = c->it[i]->fav ? (sel ? 13 : 11) : 0;   /* a favourite: its solid star before the name */
        int tw = gfx_text_width(f, t), mw = SHELF_PITCH - 6 - (st ? st + 3 : 0);
        if (tw > mw) tw = mw;
        if (st) gfx_star(st, cx - (tw + st + 3) / 2, SHELF_LABEL + dy + (gfx_line_height(f) - st) / 2, C_STAR, alpha);
        gfx_text(f, cx - (tw - (st ? st + 3 : 0)) / 2, SHELF_LABEL + dy, t, sel ? C_TEXT : C_SUB, alpha, mw);
    }
    gfx_set_clip(0, 0, SCREEN_W, SCREEN_H);
    if (c->scroll > 0.05f) gfx_icon("chevron-left", 14, RAIL_W + 3, SHELF_Y - 7, C_MUTED, 200);
    if (c->scroll < (float)(c->n - SHELF_SHOWN) - 0.05f) gfx_icon("chevron-right", 14, SCREEN_W - 17, SHELF_Y - 7, C_MUTED, 200);

    /* the selection's details, where the list's expanded row was (Settings: its control on the right) */
    if (e) {
        int tx = DETAIL_X, py = DETAIL_Y, cw = 0;
        if (c->kind == CK_SETTINGS) cw = settings_control(e, SCREEN_W - 16, py + gfx_line_height(FONT_TITLE) / 2 + 1, alpha) + 12;
        if (e->fav) { gfx_star(15, tx, py + (gfx_line_height(FONT_TITLE) - 15) / 2, C_STAR, alpha); tx += 20; }
        mtext(FONT_TITLE, tx, py, e->title, C_TEXT, alpha, SCREEN_W - 16 - tx - cw, g_sel_since);
        {
            const char *d = e->desc[0] ? e->desc : is_builtin(e) ? e->tags : "";   /* (a built-in app's is in its tags) */
            if (d[0]) gfx_text_wrapped(FONT_SMALL, DETAIL_X, py + 22, d, C_SUB, alpha, SCREEN_W - 16 - DETAIL_X, 14, 2);
            detail_line(e, s, sizeof(s));
            mtext(FONT_SMALL, DETAIL_X, d[0] ? py + 54 : py + 24, s, c->kind == CK_SETTINGS ? C_SUB : 0x7fb2f5, alpha, SCREEN_W - 16 - DETAIL_X, g_sel_since);
        }
    }
}

/* the ribbons over the backdrop, still: moving them meant the board redrawing and sending a frame
 * ten times a second while the launcher was in use (a still screen sends nothing); the user's
 * choice was the look without that cost */
static void waves(float dt) { (void)dt; gfx_waves(0); }

/* a hint at the right of the hint line, ending at x: a button's name in a badge, then what it
 * does; the x where it starts */
static int hint_right(int x, const char *badge, const char *label)
{
    int bw = gfx_text_width(FONT_BADGE, badge) + 10;
    x -= gfx_text_width(FONT_SMALL, label);
    gfx_text(FONT_SMALL, x, HINT_Y - 1, label, C_SUB, 255, 0);
    x -= bw + 6;
    gfx_nine(NINE_R6, x, HINT_Y, bw, 14, 0x1e2c4a, 255);
    gfx_text(FONT_BADGE, x + 5, HINT_Y + (14 - gfx_line_height(FONT_BADGE)) / 2, badge, C_SUB, 255, 0);
    return x;
}

static int hint_cap(int x, const char *cap, const char *label, uint32_t col);
static int hint_btn(int x, char b, const char *label);

static void draw_hints(void)
{
    int x = 16;
    LEntry *e = cur_entry();
    if (g_menu.open) return;   /* (START's menu draws its own) */
    if (g_cat[g_cur].kind == CK_SETTINGS) {   /* what cross does to it: "Turn off", "Next: 30 s", "Open" */
        if (e) {
            int k = row_kind(e);
            char t[64];
            if (k == ROW_SWITCH && action_value(e) && !strcmp(action_value(e), "Not on the PSP")) snprintf(t, sizeof(t), "Install");
            else if (k == ROW_SWITCH) snprintf(t, sizeof(t), "%s", switch_on(e) ? "Turn off" : "Turn on");
            else if (k == ROW_CHOICE) { char v[32]; choice_text(e, 1, v, sizeof(v)); snprintf(t, sizeof(t), "Next: %s", v); }
            else snprintf(t, sizeof(t), "%s", k == ROW_PAGE ? "Open" : "Select");
            hint_btn(x, 'x', t);
        }
        hint_right(SCREEN_W - 16, "START", "Menu");
        return;
    }
    /* three at most (2026-10-30): cross, Details, START; square (favourites) and SELECT (the
     * notifications) work as before and are in START's menu */
    const char *go = !e ? "Open" : e->type == LT_ACTION ? "Select" : e->type == LT_ROM || e->type == LT_GAME ? "Play" : "Open";
    if (e) x = hint_btn(x, 'x', go);   /* (an empty section: nothing to open) */
    if (e && e->type != LT_ACTION && !is_builtin(e)) x = hint_btn(x, 't', "Details");   /* (a built-in app's triangle only repeats what it is) */
    hint_right(SCREEN_W - 16, "START", "Menu");
}

/* a small key cap with a label, for hint rows */
static int hint_cap(int x, const char *cap, const char *label, uint32_t col)
{
    int bw = gfx_text_width(FONT_BADGE, cap) + 10;
    gfx_nine(NINE_R6, x, HINT_Y, bw, 14, 0x1e2c4a, 255);
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
    gfx_nine(NINE_R10, fx, S_FIELD_Y, fw, 30, 0x172540, a);
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
            gfx_nine(NINE_R6, kx, ky, KEY_W, KEY_H, foc ? 0x3d8bff : 0x222a3b, foc ? kb_a : kb_a * 235 / 255);
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
    gfx_nine(NINE_R14, cx, cy, cw, ch, 0x14203a, a * 250 / 255);
    if (is_disc(e)) {   /* games and ROMs in their disc, as on the shelf */
        CoverSpec cs;
        char png[700] = "";
        memset(&cs, 0, sizeof(cs));
        cs.key = e->id; cs.title = e->title; cs.badge = type_badge(e); cs.icon = type_icon(e); cs.round = 1;
        if (e->art[0]) cs.art = e->art;
        else if (e->type == LT_ROM && e->sys >= 0) cs.art = rom_art(e, png, sizeof(png));
        gfx_disc(&cs, cx + 16 + COVER_SIZE / 2, cy + 16 + COVER_SIZE / 2, 1, a);
    } else draw_cover(e, cx + 16, cy + 16, a);
    tx = cx + 16 + COVER_SIZE + 18; ty = cy + 16; tw = cx + cw - 16 - tx;
    mtext(FONT_H1, tx, ty, e->title, C_TEXT, a, tw, g_open_at);
    ty += gfx_line_height(FONT_H1) + 2;
    switch (e->type) {
    case LT_ROM: snprintf(s, sizeof(s), "%s game", L.sys[e->sys].name); break;
    case LT_APP: snprintf(s, sizeof(s), "App"); break;
    case LT_TOOL: snprintf(s, sizeof(s), "Tool"); break;
    default: snprintf(s, sizeof(s), "Game"); break;
    }
    {   /* the description, over the lines left above the rows below it (three to five) */
        int lh = gfx_line_height(FONT_BODY) + 1;
        int rows = e->type == LT_ROM ? 3 + (e->tags[0] != 0) : 2 + (e->tags[0] != 0) + (e->pkg[0] == 0);
        int lines = (cy + ch - 30 - 6 - ty - rows * 20) / lh;
        lines = lines < 3 ? 3 : lines > 5 ? 5 : lines;
        ty += gfx_text_wrapped(FONT_BODY, tx, ty, e->desc[0] ? e->desc : s, C_SUB, a, tw, lh, lines) * lh + 8;
    }
    if (e->type == LT_ROM) {
        const char *base = strrchr(e->exec, '/');
        char sz[32];
        human_size(e->size, sz, sizeof(sz));
        if (e->tags[0]) { info_row(tx, ty, "tag", e->tags, tw, a); ty += 20; }
        info_row(tx, ty, "hard-drive", sz, tw, a); ty += 20;
        info_row(tx, ty, "file-text", base ? base + 1 : e->exec, tw, a); ty += 20;
        {   /* the player: pspexp-retro and its core, by the core's file name; else the command's first word */
            const char *x = L.sys[e->sys].exec, *r = strstr(x, "pspexp-retro "), *sp;
            const char *gl = graphics_label(L.sys[e->sys].graphics);
            if (r) {
                char core[200], *cb;
                snprintf(core, sizeof(core), "%.*s", (int)strcspn(r + 13, " "), r + 13);
                cb = strrchr(core, '/');
                snprintf(s, sizeof(s), "%s%spspexp-retro, %s", gl, gl[0] ? ": " : "", cb ? cb + 1 : core);
            } else { sp = strchr(x, ' '); snprintf(s, sizeof(s), "%s%s%.*s", gl, gl[0] ? ": " : "", sp ? (int)(sp - x) : (int)strlen(x), x); }
        }
        info_row(tx, ty, "cpu", s, tw, a); ty += 20;
    } else {
        if (!e->pspexp) snprintf(s, sizeof(s), "Shown through the mirror");
        else if (graphics_label(e->graphics)[0]) snprintf(s, sizeof(s), "%s", graphics_label(e->graphics));
        else snprintf(s, sizeof(s), "PSP renderer; HOME quits");
        info_row(tx, ty, !e->pspexp ? "monitor" : !strncmp(e->graphics, "opengl", 6) || !strcmp(e->graphics, "board-gpu") ? "box" : !strcmp(e->graphics, "sdl") ? "image" : "zap", s, tw, a); ty += 20;
        if (e->tags[0]) { info_row(tx, ty, "tag", e->tags, tw, a); ty += 20; }
        if (!e->pkg[0]) { info_row(tx, ty, "folder-open", e->dir[0] ? e->dir : "/", tw, a); ty += 20; }   /* (a package's: its own folder) */
        if (e->pkg[0]) snprintf(s, sizeof(s), "Package %s", e->pkg);
        else snprintf(s, sizeof(s), "library/%s.entry", e->id + 2);
        info_row(tx, ty, "package", s, tw, a); ty += 20;
    }
    for (i = 0; i < L.nrecent; i++) if (!strcmp(L.recent[i], e->id)) break;
    /* (rows that would reach the buttons along the foot are left out: a long description) */
    if (i < L.nrecent && ty + 16 <= cy + ch - 34) { snprintf(s, sizeof(s), i == 0 ? "Played last" : "Played recently (#%d)", i + 1); info_row(tx, ty, "history", s, tw, a); ty += 20; }
    if (e->fav && ty + 16 <= cy + ch - 34) { info_row(tx, ty, "star", "Favourite", tw, a); }
    /* buttons along the card's foot */
    {
        int bx = tx, by = cy + ch - 30;
        gfx_button('x', bx, by); bx += 18; bx += gfx_text(FONT_SMALL, bx, by - 1, e->type == LT_ROM || e->type == LT_GAME ? "Play" : "Open", C_SUB, a, 0) + 14;
        gfx_button('s', bx, by); bx += 18; bx += gfx_text(FONT_SMALL, bx, by - 1, e->fav ? "Unfavourite" : "Favourite", C_SUB, a, 0) + 14;
        gfx_button('o', bx, by); bx += 18; gfx_text(FONT_SMALL, bx, by - 1, "Close", C_SUB, a, 0);
    }
}

/* a button's mark on the controls sheet: the face buttons' own, the others' names in a badge;
 * its width */
static int sheet_button(const char *b, int x, int y)
{
    static const char *const names[][2] = {
        { "l", "L" }, { "r", "R" }, { "start", "START" }, { "select", "SELECT" }, { "nub", "NUB" },
        { "dpad", "D-PAD" }, { "up", "UP" }, { "down", "DOWN" }, { "left", "LEFT" }, { "right", "RIGHT" },
    };
    const char *t = b;
    int i, bw;
    if (!strcmp(b, "cross")) { gfx_button('x', x, y); return 14; }
    if (!strcmp(b, "circle")) { gfx_button('o', x, y); return 14; }
    if (!strcmp(b, "triangle")) { gfx_button('t', x, y); return 14; }
    if (!strcmp(b, "square")) { gfx_button('s', x, y); return 14; }
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) if (!strcmp(b, names[i][0])) t = names[i][1];
    bw = gfx_text_width(FONT_BADGE, t) + 10;
    gfx_nine(NINE_R6, x, y, bw, 14, 0x1e2c4a, 255);
    gfx_text(FONT_BADGE, x + 5, y + (14 - gfx_line_height(FONT_BADGE)) / 2, t, C_SUB, 255, 0);
    return bw;
}

/* the controls sheet: the game's cover and title at the left, its control= lines at the right
 * ("cross square: Jump", the buttons' marks then what they do), X to play */
#define SHEET_X   164
#define SHEET_KEY 82    /* the buttons' column; a line with more buttons pushes its text along */
static void draw_sheet(void)
{
    const char *p = g_sheet_ctl;
    int y = 44, x, pitch = g_sheet_n > 10 ? 17 : 19;
    gfx_shadow(NINE_SHADOW14, 16, 28 + 8, COVER_SIZE, COVER_SIZE, 0x000000, 180);
    draw_cover(&g_sheet_e, 16, 28, 255);
    gfx_text(FONT_TITLE, 16, 166, g_sheet_e.title, C_TEXT, 255, COVER_SIZE + 8);
    gfx_icon("gamepad-2", 18, SHEET_X, 13, C_ACCENT, 255);
    gfx_text(FONT_TITLE, SHEET_X + 26, 11, "Controls", C_TEXT, 255, 0);
    while (*p && y + 14 < HINT_Y - 6) {
        const char *nl = strchr(p, '\n'), *colon = strchr(p, ':');
        char b[24], text[200];
        const char *q = p;
        if (!nl) nl = p + strlen(p);
        if (!colon || colon > nl) { p = *nl ? nl + 1 : nl; continue; }
        x = SHEET_X;
        while (q < colon) {   /* the buttons, space-separated */
            int n = 0;
            while (q < colon && *q == ' ') q++;
            while (q < colon && *q != ' ' && n < (int)sizeof(b) - 1) b[n++] = (char)tolower((unsigned char)*q++);
            b[n] = 0;
            if (n) x += sheet_button(b, x, y) + 3;
        }
        q = colon + 1;
        while (*q == ' ') q++;
        snprintf(text, sizeof(text), "%.*s", (int)(nl - q), q);
        x = x + 5 > SHEET_X + SHEET_KEY ? x + 5 : SHEET_X + SHEET_KEY;
        gfx_text(FONT_SMALL, x, y - 1, text, C_SUB, 255, SCREEN_W - 16 - x);
        y += pitch;
        p = *nl ? nl + 1 : nl;
    }
    x = 16;
    gfx_button('x', x, HINT_Y); x += 18; x += gfx_text(FONT_SMALL, x, HINT_Y - 1, "Play", C_SUB, 255, 0) + 14;
    gfx_button('o', x, HINT_Y); x += 18; gfx_text(FONT_SMALL, x, HINT_Y - 1, "Back", C_SUB, 255, 0);
    hint_right(SCREEN_W - 16, "HOME", "quits the game");
}

/* the program being started: the last frame the PSP shows before it, which keeps it and turns a
 * spinner on the pill's square (PspExpDisconnect, launcher_handover_spinner) until the program draws */
#define SPIN_BG 0x16223c
static void draw_running(void)
{
    int w, cx = (SCREEN_W - COVER_SIZE) / 2, s = PSPEXP_SPINNER_SIZE;
    const char *sub = "Starting", *h = "Press HOME in the game to quit it";
    gfx_shadow(NINE_SHADOW14, cx, 40 + 8, COVER_SIZE, COVER_SIZE, 0x000000, 180);
    draw_cover(&g_running_e, cx, 40, 255);
    w = gfx_text_width(FONT_H1, g_running_e.title);
    if (w > SCREEN_W - 40) w = SCREEN_W - 40;
    gfx_text(FONT_H1, (SCREEN_W - w) / 2, 180, g_running_e.title, C_TEXT, 255, SCREEN_W - 40);
    {   /* a pill: the spinner's square, then "Starting" */
        int tw = gfx_text_width(FONT_BODY, sub), pw = 6 + s + 8 + tw + 14, ph = s + 8;
        int px = (SCREEN_W - pw) / 2, py = 206;
        gfx_nine(NINE_R14, px, py, pw, ph, SPIN_BG, 255);
        g_spin_x = px + 6 + s / 2; g_spin_y = py + ph / 2;
        gfx_rect(g_spin_x - s / 2, g_spin_y - s / 2, s, s, SPIN_BG, 255);
        gfx_text(FONT_BODY, px + 6 + s + 8, g_spin_y - gfx_line_height(FONT_BODY) / 2, sub, C_SUB, 255, 0);
        g_spin_drawn = 1;
    }
    w = gfx_text_width(FONT_SMALL, h);
    gfx_text(FONT_SMALL, (SCREEN_W - w) / 2, HINT_Y - 1, h, C_MUTED, 255, 0);
}

int launcher_handover_spinner(int *x, int *y, uint32_t *bg, uint32_t *fg)
{
    if (!g_running || !g_spin_drawn) return 0;
    *x = g_spin_x; *y = g_spin_y; *bg = SPIN_BG; *fg = C_ACCENT;
    return 1;
}

/* ---- the about page --------------------------------------------------------------------------------- */

static int g_about_pitch = 28;   /* a row's height: 26 when the board's card has more storage lines */
static int about_row(int x, int y, int w, const char *icon, const char *label, const char *value, int a)
{
    if (!value || !value[0]) return y;
    gfx_icon(icon, 14, x, y + 3, C_MUTED, a);
    gfx_text(FONT_BADGE, x + 22, y, label, C_MUTED, a, w - 22);
    mtext(FONT_BODY, x + 22, y + 11, value, C_TEXT, a, w - 22, g_open_at);
    return y + g_about_pitch;
}

/* the picker: time zones or networks, a list over the backdrop */
#define P_LIST_Y 48
#define P_ROWS   8
static void draw_pick(float dt)
{
    int a, i, fx = 16, fw = SCREEN_W - 32, x;
    float target;
    const char *title = g_pick_kind == PICK_TZ ? "Time zone" : g_pick_kind == PICK_BT ? "Sound output" : "Wi-Fi networks";
    g_overlay_a = ease(g_overlay_a, 1.0f, 16.0f, dt, 0.01f);
    a = (int)(g_overlay_a * 255);
    gfx_icon(g_pick_kind == PICK_TZ ? "clock" : g_pick_kind == PICK_BT ? "headphones" : "wifi", 20, 16, 12, C_ACCENT, a);
    gfx_text(FONT_TITLE, 44, 11, title, C_TEXT, a, 0);
    if (g_pick_kind == PICK_WIFI && g_cur_ssid[0]) {
        char s[96];
        snprintf(s, sizeof(s), "on %s", g_cur_ssid);
        gfx_text(FONT_SMALL, SCREEN_W - 16 - gfx_text_width(FONT_SMALL, s), 15, s, C_MUTED, a, 0);
    }
    if (g_pick_kind != PICK_TZ && g_scanning) {
        static const char *dots[] = { "Scanning", "Scanning.", "Scanning..", "Scanning..." };
        gfx_icon(g_pick_kind == PICK_BT ? "bluetooth" : "wifi", 24, SCREEN_W / 2 - 12, 110, C_MUTED, a * 200 / 255);
        gfx_text(FONT_BODY, SCREEN_W / 2 - gfx_text_width(FONT_BODY, "Scanning...") / 2, 144, dots[g_caret & 3], C_SUB, a, 0);
    } else if (!g_pick_n) {
        const char *m = "No networks found";
        gfx_icon("wifi-off", 24, SCREEN_W / 2 - 12, 110, C_MUTED, a * 200 / 255);
        gfx_text(FONT_BODY, (SCREEN_W - gfx_text_width(FONT_BODY, m)) / 2, 144, m, C_SUB, a, 0);
    } else {
        target = (float)(g_pick_sel - P_ROWS / 2);
        if (target > (float)(g_pick_n - P_ROWS)) target = (float)(g_pick_n - P_ROWS);
        if (target < 0) target = 0;
        g_pick_scroll = ease(g_pick_scroll, target, 16.0f, dt, 0.02f);
        gfx_set_clip(fx - 4, P_LIST_Y - 2, fw + 8, P_ROWS * ROW + 4);
        for (i = 0; i < g_pick_n; i++) {
            int ry = P_LIST_Y + (int)lroundf((i - g_pick_scroll) * ROW), sel = i == g_pick_sel, right = fx + fw - 12;
            const char *label, *value = NULL, *icon;
            int current;
            if (ry < P_LIST_Y - ROW || ry > P_LIST_Y + P_ROWS * ROW) continue;
            if (g_pick_kind == PICK_TZ) {
                const BoardZone *z = board_zone(i);
                label = z->label; value = z->offset; current = !strcmp(z->iana, g_cur_tz); icon = current ? "check" : "clock";
            } else if (g_pick_kind == PICK_BT) {
                if (i == 0) { label = "The PSP"; current = !g_bt_chosen[0]; icon = current ? "check" : "volume-2"; value = "its speaker or headphone socket"; }
                else {
                    const BtDev *d = &g_bt[i - 1];
                    label = d->name; current = !strcmp(d->addr, g_bt_chosen);
                    icon = current ? "check" : d->connected ? "bluetooth-connected" : d->audio ? "headphones" : "bluetooth";
                    value = d->connected ? "connected" : d->paired ? "paired" : !d->audio ? "not audio?" : NULL;
                }
            } else {
                const BoardNet *nw = &g_nets[i];
                label = nw->ssid; current = !strcmp(nw->ssid, g_cur_ssid);
                icon = nw->signal >= 67 ? "wifi" : nw->signal >= 34 ? "wifi-high" : nw->signal > 0 ? "wifi-low" : "wifi-zero";
                if (nw->secure) { right -= 14; gfx_icon("lock", 12, right, ry + (ROW - 12) / 2, C_MUTED, a); right -= 6; }
                if (nw->saved) value = "saved";
            }
            if (sel) {
                gfx_nine(NINE_R10, fx, ry, fw, ROW, 0xffffff, 26);
                gfx_rect(fx, ry + 6, 3, ROW - 12, C_ACCENT, a);
            }
            gfx_icon(icon, 14, fx + 12, ry + (ROW - 14) / 2, current ? C_ACCENT : sel ? 0xffffff : C_MUTED, a);
            if (value) { right -= gfx_text_width(FONT_SMALL, value); gfx_text(FONT_SMALL, right, ry + (ROW - gfx_line_height(FONT_SMALL)) / 2, value, C_MUTED, a, 0); right -= 10; }
            if (sel) mtext(FONT_BODY, fx + 34, ry + (ROW - gfx_line_height(FONT_BODY)) / 2, label, C_TEXT, a, right - fx - 40, g_sel_since);
            else gfx_text(FONT_BODY, fx + 34, ry + (ROW - gfx_line_height(FONT_BODY)) / 2, label, current ? C_TEXT : C_SUB, a, right - fx - 40);
        }
        gfx_set_clip(0, 0, SCREEN_W, SCREEN_H);
    }
    x = 16;
    if (g_menu.open) return;   /* (the menu draws its own hints) */
    if (!g_scanning || g_pick_kind == PICK_TZ) x = hint_btn(x, 'x', g_pick_kind == PICK_TZ ? "Set" : g_pick_kind == PICK_BT ? "Use" : "Join");   /* (cross waits for the scan) */
    if (g_pick_kind == PICK_BT && g_pick_sel > 0 && g_pick_sel - 1 < g_pick_n - 1 && g_bt[g_pick_sel - 1].paired && !g_scanning) x = hint_btn(x, 's', "Forget");
    if (g_pick_kind == PICK_WIFI && g_pick_n && !g_scanning && g_nets[g_pick_sel].secure && g_nets[g_pick_sel].saved)
        x = hint_btn(x, 's', "New password");
    x = hint_cap(x, "START", "Menu", C_SUB);
    x = hint_btn(x, 'o', "Back");
    (void)x;
}

/* the password: the shared keyboard (launcher_ui.c) */
static void draw_text(float dt)
{
    char title[128];
    g_overlay_a = ease(g_overlay_a, 1.0f, 16.0f, dt, 0.01f);
    snprintf(title, sizeof(title), "Password for %s", g_join.ssid);
    ui_kbd_draw(&g_kbd, "lock", title, "Type the password", g_caret, (int)(g_overlay_a * 255));
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
    gfx_nine(NINE_R10, cx1, cy, cw, ch, 0x14203a, a * 240 / 255);
    gfx_text(FONT_BADGE, cx1 + 12, cy + 9, "THIS BOARD", C_ACCENT, a, 0);
    y = cy + 26;
    g_about_pitch = b->ndisks > 1 ? 26 : 28;   /* (six rows and the storage lines in the card's 200) */
    board_detect();
    y = about_row(cx1 + 12, y, cw - 24, g_board.icon, "BOARD", b->board, a);
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
        y += g_about_pitch + (b->ndisks - 1) * 17;
    }
    {
        char sys[200];
        snprintf(sys, sizeof(sys), "%s, %s", b->os[0] ? b->os : "Linux", b->kernel);
        y = about_row(cx1 + 12, y, cw - 24, "square-terminal", "SYSTEM", sys, a);
    }
    /* this launcher's version: the board's side (the console, the launcher, the SDL layer); the PSP's
     * side has its own row in the other card */
    y = about_row(cx1 + 12, y, cw - 24, "package", "LAUNCHER", PSPEXP_VERSION, a);
    g_about_pitch = 28;

    gfx_shadow(NINE_SHADOW10, cx2, cy + 4, cw, ch, 0x000000, a * 120 / 255);
    gfx_nine(NINE_R10, cx2, cy, cw, ch, 0x14203a, a * 240 / 255);
    gfx_text(FONT_BADGE, cx2 + 12, cy + 9, "PSP AND CONNECTION", C_ACCENT, a, 0);
    if (h && h->protocol) {
        char v[96];
        short_version(g_psp_ver, v, sizeof(v));
        /* the runtime: its slot and version (read from the stick on connecting), and an update waiting */
        if (g_psp_slot == 'a' || g_psp_slot == 'b')
            snprintf(psp, sizeof(psp), "slot %c, %s%s", g_psp_slot, v[0] ? v : "version unknown",
                     g_psp_pending ? "; an update waits: restart PSPExp on the PSP" : "");
        else if (g_psp_slot == '-') snprintf(psp, sizeof(psp), "runtime %u, installed without slots", h->runtime_version);
        else snprintf(psp, sizeof(psp), "runtime %u", h->runtime_version);
        snprintf(mem, sizeof(mem), "%u KB VRAM, %u KB RAM", (unsigned)(h->vram_budget / 1024), (unsigned)(h->ram_budget / 1024));
    }
    if (st && st->last_present_latency_us) snprintf(rtt, sizeof(rtt), "%.1f ms a frame", st->last_present_latency_us / 1000.0);
    y = cy + 26;
    {
        char link[220], bat[160];
        PspExpPowerStatus p;
        int pct;
        snprintf(link, sizeof(link), "%s%s%s", g_link, rtt[0] ? ", " : "", rtt);
        y = about_row(cx2 + 12, y, cw - 24, "link", "LINK", link, a);
        y = about_row(cx2 + 12, y, cw - 24, "gamepad-2", "PSPEXP ON THE PSP", psp, a);
        pct = psp_battery(bat, sizeof(bat), &p) ? p.percent : -1;
        y = about_row(cx2 + 12, y, cw - 24, pct < 0 ? "battery" : (p.flags & (PSPEXP_POWER_CHARGING | PSPEXP_POWER_EXTERNAL)) ? "battery-charging" : pct > 70 ? "battery-full" : pct > 35 ? "battery-medium" : pct > 12 ? "battery-low" : "battery-warning",
                      "PSP BATTERY", bat, a);
    }
    y = about_row(cx2 + 12, y, cw - 24, "layers", "PSP MEMORY", mem, a);
    y = about_row(cx2 + 12, y, cw - 24, "globe", "NETWORK", b->net, a);
    {
        char pw[140];
        snprintf(pw, sizeof(pw), "%s%s%s%s%s", b->power, b->temp[0] ? ", " : "", b->temp, b->uptime[0] ? ", up " : "", b->uptime);
        about_row(cx2 + 12, y, cw - 24, g_bat >= 0 ? "battery-full" : "zap", "BOARD POWER", pw, a);
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
    else if (g_sheet) draw_sheet();
    else if (g_mode == M_SEARCH) draw_search(dt);
    else if (g_mode == M_ABOUT) draw_about(dt);
    else if (g_mode == M_PICK) draw_pick(dt);
    else if (g_mode == M_TEXT) draw_text(dt);
    else if (g_ncat) {
        waves(dt);
        draw_status();
        draw_rail(dt);
        draw_list(dt);
        draw_hints();
        if (g_mode == M_DETAILS) draw_details(dt);
    }
    if (g_menu.open && (g_mode == M_BROWSE || g_mode == M_PICK)) ui_menu_draw(&g_menu);   /* START's menu, with its own hints */
    notify_draw(now);
    pspexp_present(ctx);
    g_dirty = 0;
}

/* ---- init ------------------------------------------------------------------------------- */

int launcher_init(PspExpContext *ctx, const char *assets)
{
    g_ctx = ctx;
    snprintf(g_assets, sizeof(g_assets), "%s", assets);
    g_wifi_cmd = getenv("PSPEXP_WIFI_MODE_CMD");
    if (!g_wifi_cmd && access("/usr/local/bin/pspexp-wifi-mode", X_OK) == 0) g_wifi_cmd = "/usr/local/bin/pspexp-wifi-mode";
    if (gfx_init(ctx, assets) < 0) { fprintf(stderr, "[launcher] no fonts in %s\n", assets); return -1; }
    lib_load(&L);
    g_nofetch = getenv("PSPEXP_LAUNCHER_NO_FETCH") != NULL || !lib_setting_int(&L, "boxart", 1);
    g_psp_clock = lib_setting_int(&L, "psp_clock", 1);
    g_clips_keep = lib_setting_int(&L, "clips", 0);
    g_clip_secs = lib_setting_int(&L, "clip_secs", 30);
    if (g_clip_secs != 15 && g_clip_secs != 60) g_clip_secs = 30;
    g_clip_quality = lib_setting_int(&L, "clip_quality", 0);
    if (g_clip_quality < 0 || g_clip_quality > 2) g_clip_quality = 0;
    cats_build();
    g_cur = start_cat();   /* start on Games, All */
    /* screenshots and tests: open a page without replaying keys, which a slow
     * link can drop (a lost press in System could land on Power off) */
    {
        const char *o = getenv("PSPEXP_LAUNCHER_OPEN");
        if (o && !strcmp(o, "about")) { about_collect(); g_mode = M_ABOUT; g_dirty = 1; }
        if (o && !strcmp(o, "tz")) pick_open(PICK_TZ, 0);
        if (o && !strncmp(o, "terms:", 6)) snprintf(g_app_req, sizeof(g_app_req), "store");   /* (store.c shows them) */
        if (o && !strcmp(o, "wifi") && board_wifi_kind() != BOARD_WIFI_NONE) pick_open(PICK_WIFI, 0);
        if (o && !strncmp(o, "password:", 9)) {   /* the keyboard for a network, typing nothing */
            snprintf(g_join.ssid, sizeof(g_join.ssid), "%s", o + 9);
            g_join.secure = 1;
            ui_kbd_reset(&g_kbd, NULL, 1, "Join");
            g_mode = M_TEXT; g_dirty = 1;
        }
    }
    fprintf(stderr, "[launcher] library %s: %d entries, %d systems, %d categories\n", L.home, L.n, L.nsys, g_ncat);
    return 0;
}
