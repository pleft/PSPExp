/* The console's Media app (once Music): see music.h. A film plays in Now
 * playing full screen, its controls over it for a few seconds after a key. */
#include "music.h"
#include "appview.h"
#include "notify.h"
#include "player.h"
#include "files.h"
#include "files_psp.h"
#include "launcher_gfx.h"
#include "launcher_ui.h"
#include "quick.h"
#include "launcher.h"
#include "tags.h"
#include "upnp.h"
#include "modplay.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { V_NOW, V_BROWSE, V_QUEUE, NVIEWS };
enum { R_OFF, R_ALL, R_ONE };

/* a file (dir and entry), or a media server's item (url; e.name its title); title: what lists show
 * (its tag, else the name without its extension) */
typedef struct Track { char dir[FOPS_PATH]; FEntry e; char url[1024]; int video; char title[160]; } Track;

static PspExpContext *g_ctx;
static Track *g_q;                 /* the queue: a folder's tracks, in the folder's order */
static int *g_order;               /* the order they play in (shuffled or not): indices into g_q */
static int g_qn, g_qcap, g_at = -1;   /* how many (room for); the place in g_order of the one loaded (-1 none) */
static int g_shuffle, g_repeat;
static int g_ended_seen;           /* the end of the track loaded was acted on */
static int g_view, g_dirty = 1, g_exit;
static int g_back;   /* music_set_back */
static UiMenu g_menu;              /* START's: the highlighted song's or folder's, then the play order */
static int g_info;                 /* triangle's box: what the highlighted (or playing) song is */
static char g_info_t[160], g_info_a[160], g_info_1[FOPS_PATH], g_info_2[200];
static char g_msg[300];            /* a track that would not play, shown until the next key */
static char g_note[160];           /* what square added to the queue, shown until the next key */
static char g_cover_key[300];
static unsigned g_vid_serial;      /* the film's picture last sent to the PSP */
static unsigned g_vid_shown;       /* pictures sent to the PSP, this film (tests) */
static int g_vid_loaded;           /* the film's textures are on the PSP */
static uint64_t g_hud_until;       /* a film's controls shown until then (a key brings them) */
static uint64_t g_now;
static int g_mod_ch0;              /* a module: the first channel shown (up / down: more when they do not all fit) */

/* what a song is called in the lists: its tag's title (a local file, not the PSP's memory stick:
 * each read crosses the cable), else its name without the extension */
static void display_title(const char *dir, const char *name, char *out, size_t n, char *artist, size_t an)
{
    char p[FOPS_PATH];
    const char *dot = strrchr(name, '.');
    if (artist && an) artist[0] = 0;
    if (dir[0] == '/') {
        char a[96];
        fops_join(p, sizeof(p), dir, name);
        if (tags_peek(p, out, n, a, sizeof(a))) { if (artist && an) snprintf(artist, an, "%s", a); return; }
    }
    snprintf(out, n, "%.*s", dot && dot > name ? (int)(dot - name) : (int)strlen(name), name);
}

/* ---- the queue -------------------------------------------------------------------------------- */

static const char *track_name(int k) { const Track *t = &g_q[g_order[k]]; return t->title[0] ? t->title : t->e.name; }

static void order_build(int keep_first)
{
    int i;
    for (i = 0; i < g_qn; i++) g_order[i] = i;
    if (g_shuffle && g_qn > 1) {   /* Fisher-Yates; the track playing (keep_first) stays where it is, first */
        int lo = 0;
        if (keep_first >= 0) { g_order[0] = keep_first; g_order[keep_first] = 0; lo = 1; }
        for (i = g_qn - 1; i > lo; i--) {
            int j = lo + rand() % (i - lo + 1), t = g_order[i];
            g_order[i] = g_order[j]; g_order[j] = t;
        }
        g_at = keep_first >= 0 ? 0 : g_at;
    } else if (keep_first >= 0) g_at = keep_first;
}

/* load and play the track at place k of the order; -1 (and the reason shown) when it cannot */
static int play_at(int k)
{
    char path[FOPS_PATH], err[300] = "";
    const Track *t;
    if (k < 0 || k >= g_qn) return -1;
    t = &g_q[g_order[k]];
    if (t->url[0] ? player_start_stream(g_ctx, t->url, t->e.name, t->video, err, sizeof(err)) < 0
                  : files_fetch(t->dir, &t->e, path, sizeof(path), err, sizeof(err)) < 0 || player_start(g_ctx, path, err, sizeof(err)) < 0) {
        snprintf(g_msg, sizeof(g_msg), "%s: %s", t->e.name, err[0] ? err : "it does not play");
        g_dirty = 1;
        return -1;
    }
    g_at = k;
    g_ended_seen = 0;
    g_vid_shown = 0; g_vid_serial = 0;
    g_msg[0] = 0;
    g_dirty = 1;
    return 0;
}

/* the next (d = 1) or previous (-1) track that plays; wrap: past the ends (a press of L / R does) */
static int step(int d, int wrap)
{
    int k;
    for (k = 1; k <= g_qn; k++) {
        int i = g_at + d * k;
        if (!wrap && (i < 0 || i >= g_qn)) return -1;
        i = ((i % g_qn) + g_qn) % g_qn;
        if (play_at(i) == 0) return 0;
    }
    return -1;
}

void music_init(PspExpContext *ctx) { g_ctx = ctx; }

/* room for n tracks */
static int q_room(int n)
{
    Track *q; int *o, cap = g_qcap ? g_qcap : 64;
    if (n <= g_qcap) return 0;
    while (cap < n) cap *= 2;
    if (!(q = (Track *)realloc(g_q, sizeof(Track) * (size_t)cap))) return -1;
    g_q = q;
    if (!(o = (int *)realloc(g_order, sizeof(int) * (size_t)cap))) return -1;
    g_order = o; g_qcap = cap;
    return 0;
}

#define ADD_MAX 4000     /* tracks one add may bring (a whole drive is not a queue) */
#define QUEUE_MAX 10000

/* dir's playable files in its order, then its folders' (depth: how far down still) */
static int add_tree(const char *dir, const FEntry *e, int depth)
{
    FList l = { 0 };
    char p[FOPS_PATH];
    int i, k, added = 0;
    if (!e->is_dir) {
        if (e->is_up || !player_handles(e->name) || g_qn >= QUEUE_MAX || q_room(g_qn + 1) < 0) return 0;
        snprintf(g_q[g_qn].dir, sizeof(g_q[g_qn].dir), "%s", dir);
        g_q[g_qn].e = *e;
        g_q[g_qn].url[0] = 0; g_q[g_qn].video = 0;
        display_title(g_qn < 500 ? dir : "", e->name, g_q[g_qn].title, sizeof(g_q[g_qn].title), NULL, 0);   /* (a big add: names) */
        g_order[g_qn] = g_qn;
        g_qn++;
        return 1;
    }
    if (depth <= 0) return 0;
    fops_join(p, sizeof(p), dir, e->name);
    if (fops_list(p, 0, 0, &l) < 0) return 0;
    for (k = 0; k < 2; k++)   /* the files first, then the folders */
        for (i = 0; i < l.n && added < ADD_MAX; i++)
            if (!l.e[i].is_up && (k == 0) == !l.e[i].is_dir) added += add_tree(p, &l.e[i], depth - 1);
    fops_free(&l);
    return added;
}

int music_add(const char *dir, const FEntry *entries, int n)
{
    PlayerInfo pi;
    int i, first = g_qn, added = 0, have = player_info(&pi);
    for (i = 0; i < n && added < ADD_MAX; i++) added += add_tree(dir, &entries[i], 4);
    if (added && (!have || g_at < 0 || pi.ended)) play_at(first);   /* nothing playing: the first one added */
    if (added == 1) snprintf(g_note, sizeof(g_note), "Added to the queue: %s", g_q[g_order[g_qn - 1]].e.name);
    else if (added) snprintf(g_note, sizeof(g_note), "Added %d to the queue", added);
    else snprintf(g_note, sizeof(g_note), "Nothing to play there");
    g_dirty = 1;
    return added;
}

const char *music_note(void) { return g_note; }

int music_is_current(const char *dir, const char *name)
{
    PlayerInfo pi;
    const Track *t;
    if (g_at < 0 || !player_info(&pi) || pi.ended) return 0;
    t = &g_q[g_order[g_at]];
    return !strcmp(t->dir, dir) && !strcmp(t->e.name, name);
}

int music_play_list(const char *dir, const FEntry *entries, int n, int start)
{
    int i, at = -1;
    if (start >= 0 && start < n && music_is_current(dir, entries[start].name)) { g_view = V_NOW; g_dirty = 1; return 0; }   /* playing already: just shown */
    Track *q = (Track *)malloc(sizeof(Track) * (size_t)(n > 0 ? n : 1));
    int *o = (int *)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!q || !o) { free(q); free(o); return -1; }
    free(g_q); free(g_order);
    g_q = q; g_order = o; g_qn = 0; g_qcap = n > 0 ? n : 1; g_at = -1;
    for (i = 0; i < n; i++) {
        const FEntry *f = &entries[i];
        if (f->is_dir || f->is_up || !player_handles(f->name)) continue;
        if (i == start) at = g_qn;
        snprintf(g_q[g_qn].dir, sizeof(g_q[g_qn].dir), "%s", dir);
        g_q[g_qn].e = *f;
        g_q[g_qn].url[0] = 0; g_q[g_qn].video = 0;
        display_title(g_qn < 500 ? dir : "", f->name, g_q[g_qn].title, sizeof(g_q[g_qn].title), NULL, 0);
        g_qn++;
    }
    if (!g_qn) return -1;
    order_build(at >= 0 ? at : 0);
    g_view = V_NOW;
    return play_at(g_at >= 0 ? g_at : 0);
}

int music_play_one(const char *dir, const FEntry *entries, int n, int chosen)
{
    if (chosen < 0 || chosen >= n) return -1;
    return music_play_list(dir, &entries[chosen], 1, 0);
}

void music_tick(uint64_t now)
{
    PlayerInfo pi;
    static int shown = -1;
    (void)now;
    if (!player_info(&pi) || g_at < 0) return;
    if (pi.ended && !g_ended_seen) {   /* the next one, whatever is on the screen */
        g_ended_seen = 1;
        if (g_repeat == R_ONE) play_at(g_at);
        else if (step(1, 0) < 0 && g_repeat == R_ALL && g_qn) {
            if (g_shuffle) order_build(-1);
            if (play_at(0) < 0) step(1, 0);
        }
        g_dirty = 1;
    }
    if (g_view == V_NOW && (int)(pi.pos * 4) != shown) { shown = (int)(pi.pos * 4); g_dirty = 1; }
}

void music_toggle(void)
{
    PlayerInfo pi;
    if (!player_info(&pi)) { if (g_qn && g_at >= 0) play_at(g_at); return; }
    if (pi.ended) play_at(g_at);
    else player_toggle_pause();
    g_dirty = 1;
}

void music_stop(void) { player_stop(); g_dirty = 1; }

int music_now(char *title, size_t n, int *paused)
{
    PlayerInfo pi;
    if (!player_info(&pi) || g_at < 0) return 0;
    if (title) snprintf(title, n, "%s", pi.title[0] ? pi.title : g_q[g_order[g_at]].e.name);
    if (paused) *paused = pi.paused || pi.ended;
    return 1;
}

/* ---- Browse: the places, then their folders ----------------------------------------------------- */

typedef struct Place { char path[FOPS_PATH], name[48]; const char *icon; } Place;
static Place g_pl[FOPS_DRIVES + 4];
static int g_npl;
static char g_dir[FOPS_PATH];      /* the folder shown; "" the places */
static char g_root[FOPS_PATH];     /* the place it is in (circle there goes back to the places) */
static char g_root_name[48];       /* its name, for the line on top ("Music / Album") */
static FList g_list;               /* its folders and songs (no "..": circle goes up) */
static struct LTitle { char title[160], artist[96]; } *g_lt;   /* what each song is called (tags) */
static int g_nspec;                /* rows before them: Play all, Shuffle (2 when there are songs) */
static int g_bsel, g_btop, g_qsel, g_qtop;

/* ---- media servers (upnp.c), asked on a thread: a search takes seconds, a big folder too ---------- */

#define UPNP_PLACE "upnp:"
enum { UP_OFF, UP_SERVERS, UP_FOLDER };
static int g_up;                       /* where Browse is: UP_OFF (files), the servers, a server's folder */
static UpnpServer g_srv[16];
static int g_nsrv, g_srv_at;           /* the servers found; the one being browsed */
static struct { char id[256], title[128]; int sel; } g_ups[16];   /* the folders gone into (the server's top first) */
static int g_upd;                      /* how deep */
static UpnpEntry *g_upl;               /* the folder's folders and what plays in it */
static int g_upn;
static pthread_t g_up_thr;
static volatile int g_up_busy, g_up_done;   /* a search or a listing running; finished, to be taken */
static int g_up_job;                   /* UP_SERVERS (a search) or UP_FOLDER (a listing) */
static UpnpServer g_job_srv[16]; static int g_job_nsrv;
static UpnpEntry *g_job_list; static int g_job_n; static char g_job_err[200], g_job_id[256];

static void *up_thread(void *arg)
{
    (void)arg;
    if (g_up_job == UP_SERVERS) g_job_nsrv = upnp_discover(g_job_srv, 16, 2500);
    else if (upnp_browse(&g_srv[g_srv_at], g_job_id, &g_job_list, &g_job_n, g_job_err, sizeof(g_job_err)) < 0) { g_job_list = NULL; g_job_n = -1; }
    g_up_done = 1;
    return NULL;
}

static void up_start(int job, const char *id)
{
    if (g_up_busy) return;
    g_up_job = job; g_job_err[0] = 0; g_up_done = 0;
    snprintf(g_job_id, sizeof(g_job_id), "%s", id ? id : "0");
    g_up_busy = pthread_create(&g_up_thr, NULL, up_thread, NULL) == 0;
    if (!g_up_busy) snprintf(g_msg, sizeof(g_msg), "Could not ask the network (no thread)");
    g_dirty = 1;
}

/* a search or a listing finished: what Browse shows */
static void up_take(void)
{
    if (!g_up_busy || !g_up_done) return;
    pthread_join(g_up_thr, NULL);
    g_up_busy = 0; g_dirty = 1;
    if (g_up_job == UP_SERVERS) {
        memcpy(g_srv, g_job_srv, sizeof(g_srv)); g_nsrv = g_job_nsrv;
        g_up = UP_SERVERS; g_bsel = g_btop = 0;
        return;
    }
    if (g_job_n < 0) {   /* it did not answer: back where it was */
        snprintf(g_msg, sizeof(g_msg), "%s", g_job_err);
        if (g_upd > 0) g_upd--; else g_up = UP_SERVERS;
        return;
    }
    {   /* what Media plays: folders, songs, films (not pictures or the rest) */
        int i, k = 0;
        for (i = 0; i < g_job_n; i++)
            if (g_job_list[i].kind == UPNP_FOLDER || g_job_list[i].kind == UPNP_AUDIO || g_job_list[i].kind == UPNP_VIDEO) g_job_list[k++] = g_job_list[i];
        free(g_upl); g_upl = g_job_list; g_upn = k; g_job_list = NULL;
    }
    g_up = UP_FOLDER;
    g_bsel = g_ups[g_upd].sel < g_upn ? g_ups[g_upd].sel : 0;   /* (back up a folder: on the one come from) */
    g_btop = 0;
}

static void up_into(int i)   /* a server, or a folder of it */
{
    if (g_up_busy) return;
    if (g_up == UP_SERVERS) {
        if (i < 0 || i >= g_nsrv) return;
        g_srv_at = i; g_upd = 0;
        snprintf(g_ups[0].id, sizeof(g_ups[0].id), "0");
        snprintf(g_ups[0].title, sizeof(g_ups[0].title), "%s", g_srv[i].name);
        g_ups[0].sel = 0;
        up_start(UP_FOLDER, "0");
        return;
    }
    if (i < 0 || i >= g_upn || g_upd + 1 >= (int)(sizeof(g_ups) / sizeof(g_ups[0]))) return;
    g_ups[g_upd].sel = i;
    g_upd++;
    snprintf(g_ups[g_upd].id, sizeof(g_ups[g_upd].id), "%s", g_upl[i].id);
    snprintf(g_ups[g_upd].title, sizeof(g_ups[g_upd].title), "%s", g_upl[i].title);
    g_ups[g_upd].sel = 0;
    up_start(UP_FOLDER, g_upl[i].id);
}

static void up_back(void)
{
    if (g_up_busy) return;
    if (g_up == UP_SERVERS) { g_up = UP_OFF; g_bsel = g_btop = 0; g_dirty = 1; return; }   /* back to the places */
    if (g_upd == 0) { g_up = UP_SERVERS; g_bsel = g_srv_at; g_btop = 0; g_dirty = 1; return; }
    g_upd--;
    up_start(UP_FOLDER, g_ups[g_upd].id);
}

/* the item at i as a Track */
static void up_track(Track *t, int i)
{
    memset(t, 0, sizeof(*t));
    snprintf(t->dir, sizeof(t->dir), "%s%s", UPNP_PLACE, g_srv[g_srv_at].name);
    snprintf(t->e.name, sizeof(t->e.name), "%s", g_upl[i].title);
    snprintf(t->url, sizeof(t->url), "%s", g_upl[i].url);
    snprintf(t->title, sizeof(t->title), "%s", g_upl[i].title);
    t->video = g_upl[i].kind == UPNP_VIDEO;
}

/* item i alone as the queue (as a file of the Library plays: square adds more) */
static void up_play(int i)
{
    Track *q; int *o;
    if (i < 0 || i >= g_upn || g_upl[i].kind == UPNP_FOLDER) return;
    q = (Track *)calloc(1, sizeof(Track)); o = (int *)malloc(sizeof(int));
    if (!q || !o) { free(q); free(o); return; }
    free(g_q); free(g_order);
    g_q = q; g_order = o; g_qn = 0; g_qcap = 1; g_at = -1;
    up_track(&g_q[g_qn++], i);
    order_build(0);
    g_view = V_NOW;
    play_at(0);
}

/* square on an item: at the end of the queue */
static void up_add(int i)
{
    PlayerInfo pi;
    if (i < 0 || i >= g_upn || g_upl[i].kind == UPNP_FOLDER || q_room(g_qn + 1) < 0) return;
    up_track(&g_q[g_qn], i);
    g_order[g_qn] = g_qn;
    g_qn++;
    if (!player_info(&pi) || g_at < 0 || pi.ended) play_at(g_qn - 1);
    snprintf(g_note, sizeof(g_note), "Added to the queue: %s", g_upl[i].title);
}

static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static void places_load(void)
{
    const char *home = getenv("HOME");
    char p[FOPS_PATH];
    FDrive d[FOPS_DRIVES];
    int i, nd = fops_board_drives(d, FOPS_DRIVES, NULL);
    g_npl = 0;
#define PLACE(P, N, I) do { snprintf(g_pl[g_npl].path, FOPS_PATH, "%s", (P)); snprintf(g_pl[g_npl].name, 48, "%s", (N)); g_pl[g_npl].icon = (I); g_npl++; } while (0)
    /* (Home went, 2026-10-29: Files is for the whole board; the PSP's MUSIC and VIDEO are in PSP) */
    if (home) {
        snprintf(p, sizeof(p), "%s/Music", home);
        if (is_dir(p)) PLACE(p, "Music", "music");
        snprintf(p, sizeof(p), "%s/Videos", home);
        if (is_dir(p)) PLACE(p, "Videos", "clapperboard");
    }
    launcher_gallery_dir(p, sizeof(p));   /* the recordings, clips and screenshots, a folder a game (the Gallery's) */
    if (is_dir(p)) PLACE(p, "Gallery", "images");
    for (i = 1; i < nd && g_npl < FOPS_DRIVES + 2; i++) PLACE(d[i].root, d[i].name, "hard-drive");   /* (d[0] is "/") */
    if (files_psp_available()) PLACE("ms0:/", "PSP", "smartphone");
    PLACE(UPNP_PLACE, "Media servers", "server");   /* on the network: Plex, Jellyfin, MiniDLNA, a NAS... */
#undef PLACE
}

static void dir_load(const char *dir)
{
    FList all = { 0 };
    int i;
    int songs = 0;
    fops_free(&g_list);
    memset(&g_list, 0, sizeof(g_list));
    free(g_lt); g_lt = NULL;
    g_nspec = 0;
    snprintf(g_dir, sizeof(g_dir), "%s", dir);
    g_bsel = g_btop = 0;
    if (fops_list(dir, 0, 0, &all) < 0) { snprintf(g_msg, sizeof(g_msg), "%s cannot be read", dir); return; }
    g_list.e = (FEntry *)malloc(sizeof(FEntry) * (size_t)(all.n ? all.n : 1));
    g_lt = (struct LTitle *)calloc((size_t)(all.n ? all.n : 1), sizeof(*g_lt));
    for (i = 0; g_list.e && g_lt && i < all.n; i++) {   /* folders and what plays (no "..": circle goes up) */
        const FEntry *f = &all.e[i];
        if (f->is_up) continue;
        if (f->is_dir || player_handles(f->name)) {
            if (!f->is_dir) {
                display_title(songs < 300 ? dir : "", f->name, g_lt[g_list.n].title, sizeof(g_lt[0].title), g_lt[g_list.n].artist, sizeof(g_lt[0].artist));
                songs++;
            }
            g_list.e[g_list.n++] = *f;
        }
    }
    g_list.cap = g_list.n;
    g_nspec = songs ? 2 : 0;
    fops_free(&all);
}

/* the Library's rows: Play all and Shuffle first (when the folder has songs), then its entries */
static int lib_rows(void) { return g_nspec + g_list.n; }
static const FEntry *lib_entry(int row) { return row >= g_nspec && row - g_nspec < g_list.n ? &g_list.e[row - g_nspec] : NULL; }

static void browse_up(void)
{
    char p[FOPS_PATH], child[FOPS_PATH];
    const char *sl;
    int i;
    if (!g_dir[0]) return;                                       /* the places: the top (circle never leaves an app: HOME does) */
    if (!strcmp(g_dir, g_root)) {   /* back to the places, on the one left */
        g_dir[0] = 0; places_load(); g_bsel = g_btop = 0;
        for (i = 0; i < g_npl; i++) if (!strcmp(g_pl[i].path, g_root)) g_bsel = i;
        return;
    }
    sl = strrchr(g_dir, '/');
    snprintf(child, sizeof(child), "%s", sl && sl[1] ? sl + 1 : g_dir);
    snprintf(p, sizeof(p), "%s", g_dir);
    fops_parent(p);
    dir_load(p);
    for (i = 0; i < g_list.n; i++) if (g_list.e[i].is_dir && !strcmp(g_list.e[i].name, child)) g_bsel = g_nspec + i;   /* (on the folder come from) */
}

static void browse_choose(void)
{
    char p[FOPS_PATH];
    if (!g_dir[0]) {
        if (g_bsel < g_npl && !strcmp(g_pl[g_bsel].path, UPNP_PLACE)) { up_start(UP_SERVERS, NULL); return; }   /* a search: the servers when it ends */
        if (g_bsel < g_npl) {
            snprintf(g_root, sizeof(g_root), "%s", g_pl[g_bsel].path);
            snprintf(g_root_name, sizeof(g_root_name), "%s", g_pl[g_bsel].name);
            dir_load(g_root);
        }
        return;
    }
    if (g_bsel < g_nspec) {   /* Play all, Shuffle: the folder's songs, from the first or a random one */
        int i, first = -1, n = 0, pick;
        for (i = 0; i < g_list.n; i++) if (!g_list.e[i].is_dir) { if (first < 0) first = i; n++; }
        if (first < 0) return;
        g_shuffle = g_bsel == 1;
        pick = first;
        if (g_shuffle) { int k = rand() % n; for (i = 0; i < g_list.n; i++) if (!g_list.e[i].is_dir && k-- == 0) { pick = i; break; } }
        music_play_list(g_dir, g_list.e, g_list.n, pick);
        return;
    }
    {
        const FEntry *e = lib_entry(g_bsel);
        if (!e) return;
        if (e->is_dir) { fops_join(p, sizeof(p), g_dir, e->name); dir_load(p); return; }
        music_play_list(g_dir, e, 1, 0);   /* that one alone (the folder: Play all, Shuffle; more: square) */
    }
}

/* ---- the app ------------------------------------------------------------------------------------ */

static int g_open_req;            /* music_open_at: where the next opening goes */
static char g_open_place[48];
void music_open_at(const char *place)
{
    g_open_req = 1;
    snprintf(g_open_place, sizeof(g_open_place), "%s", place ? place : "");
}

static void mu_open(void)
{
    g_exit = 0; g_menu.open = 0; g_info = 0; g_dirty = 1; g_cover_key[0] = 0;   /* (Files' viewer may have used the picture since) */
    {   /* Now playing only while something plays: a film or a song left paused opens the Library */
        PlayerInfo pi;
        g_view = g_at >= 0 && player_info(&pi) && !pi.paused && !pi.ended ? V_NOW : V_BROWSE;
    }
    if (g_view == V_BROWSE && !g_dir[0]) places_load();
    if (g_open_req) {   /* from the launcher's Media: on what plays, or in a place's folder */
        g_open_req = 0;
        if (!g_open_place[0]) { if (g_at >= 0) g_view = V_NOW; return; }
        g_view = V_BROWSE;
        g_dir[0] = 0; places_load(); g_bsel = g_btop = 0;
        {
            int i;
            for (i = 0; i < g_npl; i++) if (!strcmp(g_pl[i].name, g_open_place)) { g_bsel = i; browse_choose(); break; }
        }
    }
}

/* the queue down to the track playing (none: empty) */
static void queue_clear(void)
{
    if (g_at >= 0 && g_at < g_qn) {
        Track keep = g_q[g_order[g_at]];
        g_q[0] = keep; g_order[0] = 0; g_qn = 1; g_at = 0;
    } else { g_qn = 0; g_at = -1; }
    g_qsel = g_qtop = 0;
    snprintf(g_note, sizeof(g_note), "The queue cleared");
}

static void queue_remove(int k)   /* out of the queue (not the one playing) */
{
    int i, gone;
    if (k < 0 || k >= g_qn || k == g_at) return;
    gone = g_order[k];
    for (i = k; i < g_qn - 1; i++) g_order[i] = g_order[i + 1];
    g_qn--;
    for (i = 0; i < g_qn; i++) if (g_order[i] > gone) g_order[i]--;
    memmove(&g_q[gone], &g_q[gone + 1], sizeof(Track) * (size_t)(g_qn - gone));
    if (k < g_at) g_at--;
    if (g_qsel >= g_qn && g_qsel > 0) g_qsel--;
}

static void shuffle_toggle(void)
{
    int playing = g_at >= 0 ? g_order[g_at] : -1;
    g_shuffle = !g_shuffle;
    order_build(playing);
    if (!g_shuffle && playing >= 0) g_at = playing;
}

/* the Library's square: the song, the folder's songs (Play all, Shuffle: the folder's) at the queue's end */
static void library_add(void)
{
    if (g_up == UP_FOLDER) { up_add(g_bsel); return; }
    if (!g_dir[0]) return;
    {
        const FEntry *e = lib_entry(g_bsel);
        if (e) music_add(g_dir, e, 1);
        else if (g_bsel < g_nspec) music_add(g_dir, g_list.e, g_list.n);
    }
}

/* START's menu, for the view shown: the highlighted song's (or folder's) first, then the play order;
 * keep: the same row selected (Shuffle and Repeat change in it, the menu stays) */
enum { MM_GO = 1, MM_ADD, MM_REMOVE, MM_STOP, MM_SHUFFLE, MM_REPEAT, MM_CLEAR, MM_INFO };
static void info_open(void);
static void media_menu(int keep)
{
    static const char *rep[3] = { "Off", "All", "One" };
    PlayerInfo pi;
    int sel = g_menu.sel, item = 0;
    ui_menu_begin(&g_menu, "Media");
    if (g_view == V_BROWSE && g_up == UP_FOLDER && g_bsel < g_upn) {
        int folder = g_upl[g_bsel].kind == UPNP_FOLDER;
        snprintf(g_menu.title, sizeof(g_menu.title), "%s", g_upl[g_bsel].title);
        ui_menu_add(&g_menu, MM_GO, folder ? "folder-open" : "play", folder ? "Open" : "Play", NULL, 0, 0);
        if (!folder) ui_menu_add(&g_menu, MM_ADD, "list", "Add to the queue", NULL, 0, 0);
        item = 1;
    } else if (g_view == V_BROWSE && !g_up && g_dir[0] && g_bsel < lib_rows()) {
        const FEntry *e = lib_entry(g_bsel);
        if (!e) {   /* Play all, Shuffle */
            snprintf(g_menu.title, sizeof(g_menu.title), "%s", g_dir[0] ? strrchr(g_dir, '/') ? strrchr(g_dir, '/') + 1 : g_dir : "");
            ui_menu_add(&g_menu, MM_GO, g_bsel == 0 ? "play" : "shuffle", g_bsel == 0 ? "Play all" : "Shuffle", NULL, 0, 0);
            ui_menu_add(&g_menu, MM_ADD, "list", "Add the folder to the queue", NULL, 0, 0);
        } else {
            const struct LTitle *t = &g_lt[g_bsel - g_nspec];
            snprintf(g_menu.title, sizeof(g_menu.title), "%s", e->is_dir ? e->name : t->title[0] ? t->title : e->name);
            ui_menu_add(&g_menu, MM_GO, e->is_dir ? "folder-open" : "play", e->is_dir ? "Open" : "Play", NULL, 0, 0);
            ui_menu_add(&g_menu, MM_ADD, "list", "Add to the queue", NULL, 0, 0);
            ui_menu_add(&g_menu, MM_INFO, "info", "Info", NULL, 0, 0);
        }
        item = 1;
    } else if (g_view == V_NOW && player_info(&pi)) {
        snprintf(g_menu.title, sizeof(g_menu.title), "%s", pi.title);
        ui_menu_add(&g_menu, MM_STOP, "circle-x", "Stop", NULL, 0, 0);
        ui_menu_add(&g_menu, MM_INFO, "info", "Info", NULL, 0, 0);
        item = 1;
    } else if (g_view == V_QUEUE && g_qn && g_qsel < g_qn) {
        snprintf(g_menu.title, sizeof(g_menu.title), "%s", track_name(g_qsel));
        ui_menu_add(&g_menu, MM_GO, "play", "Play", NULL, 0, 0);
        if (g_qsel != g_at) ui_menu_add(&g_menu, MM_REMOVE, "trash-2", "Remove from the queue", NULL, 0, 0);
        ui_menu_add(&g_menu, MM_INFO, "info", "Info", NULL, 0, 0);
        item = 1;
    }
    ui_menu_add(&g_menu, MM_SHUFFLE, "shuffle", "Shuffle", g_shuffle ? "On" : "Off", item, 0);
    ui_menu_add(&g_menu, MM_REPEAT, g_repeat == R_ONE ? "repeat-1" : "repeat", "Repeat", rep[g_repeat], 0, 0);
    if (g_view == V_QUEUE && g_qn) ui_menu_add(&g_menu, MM_CLEAR, "trash-2", "Clear the queue", NULL, 0, 0);
    if (keep && sel < g_menu.n) g_menu.sel = sel;
    g_dirty = 1;
}

static void media_choose(int id)
{
    g_dirty = 1;
    switch (id) {
    case MM_GO:
        if (g_view == V_QUEUE) { if (g_qn) play_at(g_qsel); }
        else if (g_up == UP_FOLDER) { if (g_upl[g_bsel].kind == UPNP_FOLDER) up_into(g_bsel); else up_play(g_bsel); }
        else browse_choose();
        break;
    case MM_ADD: library_add(); break;
    case MM_REMOVE: queue_remove(g_qsel); break;
    case MM_STOP: music_stop(); break;
    case MM_INFO: info_open(); break;
    case MM_SHUFFLE: shuffle_toggle(); media_menu(1); break;
    case MM_REPEAT: g_repeat = (g_repeat + 1) % 3; media_menu(1); break;
    case MM_CLEAR: queue_clear(); break;
    default: break;
    }
}

/* triangle: what the highlighted song is (the Library, the Queue), or the one playing */
static void info_open(void)
{
    PlayerInfo pi;
    g_info_t[0] = g_info_a[0] = g_info_1[0] = g_info_2[0] = 0;
    if (g_view == V_BROWSE && !g_up && g_dir[0]) {
        const FEntry *e = lib_entry(g_bsel);
        if (!e) return;
        if (!e->is_dir) { snprintf(g_info_t, sizeof(g_info_t), "%s", g_lt[g_bsel - g_nspec].title[0] ? g_lt[g_bsel - g_nspec].title : e->name); snprintf(g_info_a, sizeof(g_info_a), "%s", g_lt[g_bsel - g_nspec].artist); }
        else snprintf(g_info_t, sizeof(g_info_t), "%s", e->name);
        snprintf(g_info_1, sizeof(g_info_1), "%s/%s", g_dir, e->name);
        if (!e->is_dir) { char sz[32]; fops_human(e->size, sz, sizeof(sz)); snprintf(g_info_2, sizeof(g_info_2), "%s", sz); }
    } else if (g_view == V_QUEUE && g_qn && g_qsel < g_qn) {
        const Track *t = &g_q[g_order[g_qsel]];
        snprintf(g_info_t, sizeof(g_info_t), "%s", track_name(g_qsel));
        snprintf(g_info_1, sizeof(g_info_1), "%s%s%s", t->url[0] ? t->dir : t->dir, t->url[0] ? "" : "/", t->url[0] ? "" : t->e.name);
    } else if (player_info(&pi)) {
        snprintf(g_info_t, sizeof(g_info_t), "%s", pi.title);
        snprintf(g_info_a, sizeof(g_info_a), "%s%s%s", pi.artist, pi.artist[0] && pi.album[0] ? "  /  " : "", pi.album);
        if (g_at >= 0) { const Track *t = &g_q[g_order[g_at]]; snprintf(g_info_1, sizeof(g_info_1), "%s%s%s", t->dir, t->url[0] ? "" : "/", t->url[0] ? "" : t->e.name); }
        snprintf(g_info_2, sizeof(g_info_2), "%s", pi.format);
    } else return;
    g_info = 1; g_dirty = 1;
}

/* Buttons (2026-10-31, the launcher design proposal's shared roles): L / R the tabs (Now playing,
 * Library, Queue); in Now playing and a film left / right the previous / next track, held a seek;
 * circle back to the Library, never out (HOME); triangle the menu (the song's: play, add, stop,
 * remove, its info; then the play order: square's and triangle's own until then); START Quick
 * controls; square and SELECT free. */
static void mu_input(uint32_t b, uint32_t pressed, int up, int down, int left, int right, uint64_t now)
{
    static uint64_t lr_since, lr_seek_at;
    static int lr_dir, lr_seeked, lr_swallow;
    PlayerInfo pi;
    int lr = (b & PSPEXP_BUTTON_LEFT) ? -1 : (b & PSPEXP_BUTTON_RIGHT) ? 1 : 0;
    int film = g_view == V_NOW && !g_menu.open && !g_info && player_info(&pi) && pi.video;
    if (g_view == V_NOW && !g_menu.open && !g_info) {   /* left / right: a tap the track, held a seek (5 s each 0.25 s) */
        if (lr && !lr_dir) { lr_dir = lr; lr_since = now; lr_seeked = 0; lr_swallow = film && !pi.paused && !pi.ended && now >= g_hud_until; }
        else if (lr_dir && lr == lr_dir && !lr_swallow && now - lr_since >= 400000u && now - lr_seek_at >= 250000u) {
            player_seek(lr_dir * 5); lr_seeked = 1; lr_seek_at = now; g_dirty = 1; g_hud_until = now + 3000000;
        } else if (lr_dir && lr != lr_dir) {   /* let go */
            if (!lr_seeked && !lr_swallow && g_qn) step(lr_dir, 1);
            lr_dir = 0; g_dirty = 1;
        }
    } else lr_dir = 0;
    if (!pressed && !up && !down && !left && !right) return;
    g_dirty = 1;
    {   /* over a film: the first key only brings its controls back */
        if (film && !pi.paused && !pi.ended && now >= g_hud_until && !(pressed & (PSPEXP_BUTTON_CIRCLE | PSPEXP_BUTTON_LTRIGGER | PSPEXP_BUTTON_RTRIGGER))) {
            g_hud_until = now + 3000000;
            return;
        }
        g_hud_until = now + 3000000;
    }
    if (g_msg[0] && pressed) g_msg[0] = 0;
    if (g_note[0] && pressed) g_note[0] = 0;
    if (g_info) { if (pressed) g_info = 0; return; }   /* (any button closes it) */
    if (g_menu.open) {
        int id = ui_menu_input(&g_menu, pressed, up, down);
        if (id >= 0) media_choose(id);
        return;
    }
    if (pressed & (PSPEXP_BUTTON_LTRIGGER | PSPEXP_BUTTON_RTRIGGER)) {   /* the tabs */
        if (g_view == V_NOW) music_leave();   /* (a film waits, paused) */
        g_view = (g_view + ((pressed & PSPEXP_BUTTON_RTRIGGER) ? 1 : NVIEWS - 1)) % NVIEWS;
        if (g_view == V_BROWSE && !g_dir[0]) places_load();
        return;
    }
    if (pressed & PSPEXP_BUTTON_START) { quick_open(); return; }
    if (pressed & PSPEXP_BUTTON_TRIANGLE) { media_menu(0); return; }
    switch (g_view) {
    case V_NOW:
        if (pressed & PSPEXP_BUTTON_CROSS) music_toggle();
        if (up && g_mod_ch0 > 0) g_mod_ch0--;   /* a module: its channels, when they do not all fit */
        if (down) g_mod_ch0++;                   /* (draw_module keeps it in range) */
        if ((pressed & PSPEXP_BUTTON_CIRCLE) && g_back) { music_stop(); g_exit = 1; }   /* back to the Gallery */
        else if (pressed & PSPEXP_BUTTON_CIRCLE) { music_leave(); g_view = V_BROWSE; if (!g_dir[0]) places_load(); }   /* back to the Library (a film paused) */
        break;
    case V_BROWSE: {
        int n = g_up == UP_SERVERS ? g_nsrv : g_up == UP_FOLDER ? g_upn : g_dir[0] ? lib_rows() : g_npl;
        if (g_up) {   /* a media server's lists */
            if (up && g_bsel > 0) g_bsel--;
            if (down && g_bsel < n - 1) g_bsel++;
            if (left) g_bsel = g_bsel > 6 ? g_bsel - 6 : 0;
            if (right) g_bsel = g_bsel + 6 < n ? g_bsel + 6 : (n ? n - 1 : 0);
            if ((pressed & PSPEXP_BUTTON_CROSS) && n) {
                if (g_up == UP_SERVERS || g_upl[g_bsel].kind == UPNP_FOLDER) up_into(g_bsel);
                else up_play(g_bsel);
            }
            if (pressed & PSPEXP_BUTTON_CIRCLE) up_back();
            break;
        }
        if (up && g_bsel > 0) g_bsel--;
        if (down && g_bsel < n - 1) g_bsel++;
        if (left) g_bsel = g_bsel > 6 ? g_bsel - 6 : 0;
        if (right) g_bsel = g_bsel + 6 < n ? g_bsel + 6 : (n ? n - 1 : 0);
        if (pressed & PSPEXP_BUTTON_CROSS) browse_choose();
        if (pressed & PSPEXP_BUTTON_CIRCLE) browse_up();
        break;
    }
    case V_QUEUE:
        if (up && g_qsel > 0) g_qsel--;
        if (down && g_qsel < g_qn - 1) g_qsel++;
        if (left) g_qsel = g_qsel > 6 ? g_qsel - 6 : 0;
        if (right) g_qsel = g_qsel + 6 < g_qn ? g_qsel + 6 : (g_qn ? g_qn - 1 : 0);
        if ((pressed & PSPEXP_BUTTON_CROSS) && g_qn) play_at(g_qsel);
        if (pressed & PSPEXP_BUTTON_CIRCLE) { g_view = V_BROWSE; if (!g_dir[0]) places_load(); }
        break;
    }
}

static int mu_needs_frame(uint64_t now)
{
    const uint16_t *px; int w, h; unsigned serial;
    static int hud_was;
    int hud;
    music_tick(now);
    up_take();
    g_now = now;
    if (g_up_busy && g_view == V_BROWSE) { static uint64_t last; if (now - last > 250000u) { last = now; g_dirty = 1; } }   /* the spinner */
    {   /* a film on the screen: a new picture means a frame; two frames in flight (the next one travels while
         * the PSP draws one: 272 KB a picture, about 20 a second with one, the round trip each time) */
        int film = g_view == V_NOW && player_video(&px, &w, &h, &serial);
        pspexp_set_max_in_flight(g_ctx, film ? 2 : 1);
        if (film && serial != g_vid_serial) g_dirty = 1;
    }
    {   /* a module in Now playing: its rows and levels move, about 30 frames a second */
        PlayerInfo pi; static uint64_t last;
        if (g_view == V_NOW && player_info(&pi) && pi.module && !pi.paused && !pi.ended && now - last >= 33000u) { last = now; g_dirty = 1; }
    }
    hud = now < g_hud_until;
    if (hud != hud_was) { hud_was = hud; g_dirty = 1; }
    if (g_back && !g_exit) {   /* the Gallery's video at its end: back there */
        PlayerInfo pi;
        if (player_info(&pi) && pi.ended) { music_stop(); g_exit = 1; }
    }
    return g_dirty;
}

static void fmt_time(double t, char *out, size_t n)
{
    int s = t > 0 ? (int)t : 0;
    if (s >= 3600) snprintf(out, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, n, "%d:%02d", s / 60, s % 60);
}


/* ---- a tracker module, as a tracker shows it (libopenmpt: player_module) ---------------------------- */


/* text one character a cell (cw wide), each coloured by its kind (libopenmpt's highlight letters) */
static void mono_cells(int x, int y, const char *t, const char *hl, int cw, int cur)
{
    char one[2] = { 0, 0 };
    int i;
    for (i = 0; t[i]; i++) {
        uint32_t c;
        char k = hl[i] ? hl[i] : ' ';
        if (t[i] == ' ') continue;
        switch (k) {
        case 'n': case 'm': c = 0xf1f4f9; break;   /* the note */
        case 'i': c = 0xffd479; break;             /* the instrument */
        case 'u': case 'v': c = 0x7fe3a0; break;   /* the volume column */
        case 'e': case 'f': c = 0xff9ad5; break;   /* the effect */
        default: c = 0x45516b; break;              /* an empty field's dots */
        }
        if (cur && c == 0x45516b) c = 0x7d8fae;
        one[0] = t[i];
        gfx_text(FONT_BADGE, x + i * cw + (cw - gfx_text_width(FONT_BADGE, one)) / 2, y, one, c, 255, 0);
    }
}

static void draw_module(const PlayerInfo *pi, const ModState *st, ModPlay *m, uint64_t now)
{
    enum { CW = 6, ROWH = 12, NR = 13, TOPY = 86, LX = 10, NUMW = 22 };
    const int avail = UI_SCREEN_W - LX - NUMW - 8;
    int width = 13, vis, ch, r, x0, cw_ch;
    char s[200], t1[16], t2[16];
    (void)now;
    /* how wide a channel: whole cells (C-5 01 v64 A0F) while they fit, then narrower, at least the note */
    if (st->channels * (width * CW + 8) > avail) {
        width = avail / (st->channels > 0 ? st->channels : 1) / CW - 1;
        if (width < 3) width = 3;
    }
    cw_ch = width * CW + 8;
    vis = avail / cw_ch;
    if (vis > st->channels) vis = st->channels;
    if (g_mod_ch0 > st->channels - vis) g_mod_ch0 = st->channels - vis;
    if (g_mod_ch0 < 0) g_mod_ch0 = 0;
    /* the song */
    gfx_text(FONT_BODY, LX, 28, pi->title, UI_TEXT, 255, 300);
    fmt_time(pi->pos, t1, sizeof(t1));
    if (pi->dur > 0) fmt_time(pi->dur, t2, sizeof(t2)); else snprintf(t2, sizeof(t2), "--:--");
    snprintf(s, sizeof(s), "%s / %s", t1, t2);
    gfx_text(FONT_SMALL, UI_SCREEN_W - 12 - gfx_text_width(FONT_SMALL, s), 30, s, UI_SUB, 255, 0);
    snprintf(s, sizeof(s), "Position %02d/%02d   Pattern %02d   Row %02X/%02X   %.0f BPM   Speed %d%s%s",
             st->order, st->orders, st->pattern, st->row, st->rows, st->tempo, st->speed, pi->artist[0] ? "   " : "", pi->artist);
    gfx_text(FONT_BADGE, LX, 47, s, UI_SUB, 255, UI_SCREEN_W - 2 * LX);
    if (pi->paused || pi->ended) gfx_text(FONT_SMALL, UI_SCREEN_W - 12 - gfx_text_width(FONT_SMALL, pi->ended ? "Ended" : "Paused"), 45, pi->ended ? "Ended" : "Paused", UI_ACCENT, 255, 0);
    /* each channel's number and level */
    x0 = LX + NUMW;
    for (ch = 0; ch < vis; ch++) {
        int c = g_mod_ch0 + ch, x = x0 + ch * cw_ch, bw = cw_ch - 8, lv;
        float v = c < MOD_CHANNELS ? st->vu[c] : 0;
        snprintf(s, sizeof(s), "%d", c + 1);
        gfx_text(FONT_BADGE, x, 60, s, UI_MUTED, 255, 0);
        gfx_rect(x + 14, 63, bw - 14, 6, 0x1b2438, 255);
        lv = (int)((bw - 14) * (v > 1 ? 1 : v < 0 ? 0 : v));
        if (lv > 0) gfx_rect(x + 14, 63, lv, 6, v > 0.85f ? 0xff7a6b : v > 0.6f ? 0xffd479 : 0x7fe3a0, 255);
    }
    if (st->channels > vis) {
        snprintf(s, sizeof(s), "%d-%d of %d (up / down)", g_mod_ch0 + 1, g_mod_ch0 + vis, st->channels);
        gfx_text(FONT_BADGE, UI_SCREEN_W - 12 - gfx_text_width(FONT_BADGE, s), 72, s, UI_MUTED, 255, 0);
    }
    /* the pattern: the row heard in the middle */
    gfx_rect(LX - 4, TOPY + (NR / 2) * ROWH - 1, UI_SCREEN_W - 2 * LX + 8, ROWH, UI_ACCENT, 70);
    for (r = 0; r < NR; r++) {
        int row = st->row - NR / 2 + r, y = TOPY + r * ROWH, cur = r == NR / 2;
        if (row < 0 || row >= st->rows) continue;
        snprintf(s, sizeof(s), "%02X", row);
        gfx_text(FONT_BADGE, LX, y, s, cur ? 0xffffff : row % 4 ? 0x5d6d8a : UI_SUB, 255, 0);
        for (ch = 0; ch < vis; ch++) {
            char text[64], hl[64];
            mod_cell(m, st->pattern, row, g_mod_ch0 + ch, width, text, hl, sizeof(text));
            mono_cells(x0 + ch * cw_ch, y, text, hl, CW, cur);
        }
    }
    /* where in the song */
    gfx_rect(LX, 245, UI_SCREEN_W - 2 * LX, 3, 0x2a3145, 255);
    if (pi->dur > 0) gfx_rect(LX, 245, (int)((UI_SCREEN_W - 2 * LX) * (pi->pos / pi->dur > 1 ? 1 : pi->pos / pi->dur)), 3, UI_ACCENT, 255);
}

static void draw_now(uint64_t now)
{
    PlayerInfo pi;
    char t1[16], t2[16], s[200];
    int x = 168, y = 44, w, h, bar_x = 24, bar_w = UI_SCREEN_W - 48, bar_y = 200, have = player_info(&pi);
    {   /* a module: its patterns, as a tracker shows them */
        ModState ms; ModPlay *mp;
        if (have && pi.module && player_module(&ms, &mp)) { draw_module(&pi, &ms, mp, now); return; }
    }
    if (!have && g_at < 0) {
        gfx_nine(NINE_R10, 24, 44, 128, 128, UI_TILE, 255);
        gfx_icon("music", 64, 24 + 32, 44 + 32, UI_MUTED, 255);
        gfx_text(FONT_TITLE, x, 60, "Nothing playing", UI_TEXT, 255, 0);
        gfx_text_wrapped(FONT_SMALL, x, 90, "Choose songs or films in the Library (R), or one in Files.", UI_SUB, 255, UI_SCREEN_W - x - 16, 14, 3);
        return;
    }
    if (have && pi.cover[0] && strcmp(g_cover_key, pi.cover)) {
        snprintf(g_cover_key, sizeof(g_cover_key), "%s", pi.cover);
        if (gfx_image_load(pi.cover, 128, 128, &w, &h) < 0) snprintf(g_cover_key, sizeof(g_cover_key), "-");
    }
    if (have && pi.cover[0] && !strcmp(g_cover_key, pi.cover)) {
        gfx_image_size(&w, &h);
        gfx_image_draw(24 + (128 - w) / 2, 44 + (128 - h) / 2);
    } else {
        gfx_nine(NINE_R10, 24, 44, 128, 128, UI_TILE, 255);
        gfx_icon("disc-3", 72, 24 + 28, 44 + 28, UI_ACCENT, 255);
    }
    {
        int moving;
        gfx_text_marquee(FONT_TITLE, x, y, have && pi.title[0] ? pi.title : g_at >= 0 ? track_name(g_at) : "", UI_TEXT, 255, UI_SCREEN_W - x - 16, now, &moving);
        if (moving) g_dirty = 1;
    }
    y += 26;
    if (have && pi.artist[0]) { gfx_text(FONT_BODY, x, y, pi.artist, UI_SUB, 255, UI_SCREEN_W - x - 16); y += 20; }
    if (have && pi.album[0]) { gfx_text(FONT_SMALL, x, y, pi.album, UI_MUTED, 255, UI_SCREEN_W - x - 16); y += 18; }
    if (g_qn > 1) {   /* what comes next */
        int nx = g_at + 1 < g_qn ? g_at + 1 : g_repeat == R_ALL ? 0 : -1;
        y = 130;
        if (g_repeat == R_ONE) nx = g_at;
        if (nx >= 0) {
            snprintf(s, sizeof(s), "%s", track_name(nx));
            gfx_text(FONT_BADGE, x, y, "next", UI_MUTED, 255, 0);
            gfx_text(FONT_SMALL, x + 28, y - 2, s, UI_SUB, 255, UI_SCREEN_W - x - 44);
        }
    }
    if (have) {
        y = 150;
        gfx_text(FONT_SMALL, x, y, pi.format, UI_MUTED, 255, UI_SCREEN_W - x - 16);
        gfx_icon("cpu", 12, x, y + 20, UI_ACCENT, 255);
        gfx_text(FONT_SMALL, x + 16, y + 17, pi.decoder, UI_SUB, 255, UI_SCREEN_W - x - 32);
        gfx_rect(bar_x, bar_y, bar_w, 6, 0x2a3145, 255);
        if (pi.dur > 0) gfx_rect(bar_x, bar_y, (int)(bar_w * (pi.pos / pi.dur > 1 ? 1 : pi.pos / pi.dur)), 6, UI_ACCENT, 255);
        fmt_time(pi.pos, t1, sizeof(t1));
        if (pi.dur > 0) fmt_time(pi.dur, t2, sizeof(t2)); else snprintf(t2, sizeof(t2), "--:--");
        gfx_text(FONT_SMALL, bar_x, bar_y + 10, t1, UI_SUB, 255, 0);
        gfx_text(FONT_SMALL, bar_x + bar_w - gfx_text_width(FONT_SMALL, t2), bar_y + 10, t2, UI_SUB, 255, 0);
        {
            const char *st = pi.ended ? "Ended" : pi.paused ? "Paused" : "";
            if (st[0]) gfx_text(FONT_SMALL, (UI_SCREEN_W - gfx_text_width(FONT_SMALL, st)) / 2, bar_y + 10, st, UI_ACCENT, 255, 0);
        }
    } else gfx_text(FONT_SMALL, x, 150, "Stopped", UI_ACCENT, 255, 0);
}

static void list_row(int i, int top, int sel, const char *icon, const char *text, uint32_t col)
{
    const int LX = 16, LW = UI_SCREEN_W - 32, ROWH = 24, Y0 = 48;   /* (under the title bar and the line saying where) */
    int y = Y0 + (i - top) * ROWH;
    if (sel) { gfx_nine(NINE_R10, LX, y, LW, ROWH - 2, 0xffffff, 26); gfx_rect(LX, y + 5, 3, ROWH - 12, UI_ACCENT, 255); }
    gfx_icon(icon, 16, LX + 10, y + 3, sel ? 0xffffff : UI_ACCENT, 255);
    gfx_text(FONT_BODY, LX + 34, y + 2, text, sel ? 0xffffff : col, 255, LW - 48);
}

#define ROWS 7

static void draw_browse(void)
{
    int i, n = g_dir[0] ? g_list.n : g_npl;
    if (g_up_busy || g_up) {   /* media servers */
        char where[600] = "";
        int k;
        if (g_up_busy) {
            gfx_icon("loader", 16, 32, 62, UI_ACCENT, 255);
            gfx_text(FONT_BODY, 56, 60, g_up_job == UP_SERVERS ? "Looking for media servers on the network..." : "Asking the server...", UI_SUB, 255, 0);
            return;
        }
        if (g_up == UP_SERVERS) {
            gfx_text(FONT_SMALL, 16, 26, "Media servers on the network", UI_MUTED, 255, UI_SCREEN_W - 32);
            if (!g_nsrv) { gfx_text_wrapped(FONT_BODY, 32, 60, "No media server answered. Plex, Jellyfin, Kodi, MiniDLNA or a NAS with DLNA on, on this network, appear here.", UI_MUTED, 255, UI_SCREEN_W - 64, 18, 4); return; }
            n = g_nsrv;
        } else {
            for (k = 0; k <= g_upd; k++) { size_t l = strlen(where); snprintf(where + l, sizeof(where) - l, "%s%s", k ? " / " : "", g_ups[k].title); }
            gfx_text(FONT_SMALL, 16, 31, where, UI_MUTED, 255, UI_SCREEN_W - 32);
            if (!g_upn) { gfx_text(FONT_BODY, 32, 60, "No folders, songs or films here", UI_MUTED, 255, 0); return; }
            n = g_upn;
        }
        if (g_bsel < g_btop) g_btop = g_bsel;
        if (g_bsel >= g_btop + ROWS) g_btop = g_bsel - ROWS + 1;
        for (i = g_btop; i < n && i < g_btop + ROWS; i++) {
            if (g_up == UP_SERVERS) list_row(i, g_btop, i == g_bsel, "server", g_srv[i].name, UI_TEXT);
            else list_row(i, g_btop, i == g_bsel, g_upl[i].kind == UPNP_FOLDER ? "folder" : g_upl[i].kind == UPNP_VIDEO ? "film" : "file-music",
                          g_upl[i].title, g_upl[i].kind == UPNP_FOLDER ? UI_TEXT : UI_SUB);
        }
        return;
    }
    if (g_dir[0]) n = lib_rows();
    if (g_bsel < g_btop) g_btop = g_bsel;
    if (g_bsel >= g_btop + ROWS) g_btop = g_bsel - ROWS + 1;
    {   /* where: the place's name, then the folders below it ("Music / Album") */
        char where[FOPS_PATH + 64];
        if (!g_dir[0]) snprintf(where, sizeof(where), "Your music, films and captures");
        else {
            size_t rl = strlen(g_root);
            const char *rest = !strncmp(g_dir, g_root, rl) ? g_dir + rl : "";
            while (*rest == '/') rest++;
            snprintf(where, sizeof(where), "%s%s%s", g_root_name, *rest ? " / " : "", rest);
            { char *c; for (c = where + strlen(g_root_name); *c; c++) if (*c == '/' && c[-1] != ' ') { memmove(c + 3, c + 1, strlen(c + 1) + 1); c[0] = ' '; c[1] = '/'; c[2] = ' '; c += 2; } }
        }
        gfx_text(FONT_SMALL, 16, 31, where, UI_MUTED, 255, UI_SCREEN_W - 32);
    }
    if (!n) { gfx_text(FONT_BODY, 32, 60, g_dir[0] ? "No folders or songs here" : "No places", UI_MUTED, 255, 0); return; }
    for (i = g_btop; i < n && i < g_btop + ROWS; i++) {
        if (!g_dir[0]) { list_row(i, g_btop, i == g_bsel, g_pl[i].icon, g_pl[i].name, UI_TEXT); continue; }
        if (i < g_nspec) { list_row(i, g_btop, i == g_bsel, i == 0 ? "play" : "shuffle", i == 0 ? "Play all" : "Shuffle", UI_ACCENT); continue; }
        {
            const FEntry *f = lib_entry(i);
            const struct LTitle *t = &g_lt[i - g_nspec];
            int cur = !f->is_dir && music_is_current(g_dir, f->name);
            list_row(i, g_btop, i == g_bsel, f->is_dir ? "folder" : cur ? "volume-2" : player_is_video(f->name) ? "film" : "file-music",
                     f->is_dir ? f->name : t->title[0] ? t->title : f->name, f->is_dir ? UI_TEXT : cur ? UI_ACCENT : UI_SUB);
            if (!f->is_dir && t->artist[0]) {   /* the artist, at the row's end */
                const int y = 40 + (i - g_btop) * 24 + 4, w = gfx_text_width(FONT_SMALL, t->artist), maxw = 140;
                int x = UI_SCREEN_W - 28 - (w < maxw ? w : maxw);
                gfx_text(FONT_SMALL, x, y, t->artist, i == g_bsel ? 0xffffff : UI_MUTED, 255, maxw);
            }
        }
    }
}

static void draw_queue(void)
{
    int i;
    if (!g_qn) { gfx_text(FONT_BODY, 32, 60, "The queue is empty: square adds songs in the Library.", UI_MUTED, 255, 0); return; }
    if (g_qsel < g_qtop) g_qtop = g_qsel;
    if (g_qsel >= g_qtop + ROWS) g_qtop = g_qsel - ROWS + 1;
    for (i = g_qtop; i < g_qn && i < g_qtop + ROWS; i++)
        list_row(i, g_qtop, i == g_qsel, i == g_at ? "play" : player_is_video(track_name(i)) ? "film" : "music", track_name(i), i == g_at ? UI_ACCENT : i < g_at ? UI_MUTED : UI_TEXT);
}

/* triangle's box: the song's title and artist, where it is, its size or format */
static void draw_info(void)
{
    int w = 340, h = 100, x = (UI_SCREEN_W - w) / 2, y = (UI_SCREEN_H - h) / 2 - 8;
    gfx_shadow(NINE_SHADOW14, x, y, w, h, 0x000000, 160);
    gfx_nine(NINE_R14, x, y, w, h, 0x1b2a46, 250);
    gfx_text(FONT_BODY, x + 16, y + 12, g_info_t, UI_TEXT, 255, w - 32);
    if (g_info_a[0]) gfx_text(FONT_SMALL, x + 16, y + 32, g_info_a, UI_SUB, 255, w - 32);
    gfx_text(FONT_BADGE, x + 16, y + 52, g_info_1, UI_MUTED, 255, w - 32);
    if (g_info_2[0]) gfx_text(FONT_BADGE, x + 16, y + 68, g_info_2, UI_MUTED, 255, w - 32);
    gfx_button('o', x + w - 64, y + h - 22); gfx_text(FONT_SMALL, x + w - 46, y + h - 23, "Close", UI_TEXT, 255, 0);
}

/* a film in Now playing: its picture on black, full screen; title, time and
 * buttons over it while paused or for a few seconds after a key */
static int draw_film(PspExpContext *ctx, uint64_t now)
{
    PlayerInfo pi;
    const uint16_t *px; int w, h; unsigned serial;
    if (g_view != V_NOW || g_menu.open || g_info || !player_info(&pi) || !pi.video) {
        if (g_vid_loaded && !(player_info(&pi) && pi.video)) { gfx_frame(ctx); gfx_video_free(); g_vid_loaded = 0; }   /* the film is over: its textures go */
        return 0;
    }
    gfx_frame(ctx);
    pspexp_begin_frame(ctx);
    pspexp_set_clip(ctx, 0, 0, UI_SCREEN_W, UI_SCREEN_H);
    gfx_rect(0, 0, UI_SCREEN_W, UI_SCREEN_H, 0x000000, 255);
    if (player_video(&px, &w, &h, &serial)) {
        if (serial != g_vid_serial || gfx_video_lost()) { gfx_video_upload(px, w, h); g_vid_serial = serial; g_vid_shown++; g_vid_loaded = 1; }   /* (lost: a film paused over a game's hand-over came back black) */
        gfx_video_draw((UI_SCREEN_W - w) / 2, (UI_SCREEN_H - h) / 2);
    } else gfx_text(FONT_SMALL, 16, UI_SCREEN_H / 2 - 8, "Starting the film...", UI_SUB, 255, 0);
    if (pi.paused || pi.ended || now < g_hud_until || g_msg[0]) {
        char t1[16], t2[16];
        int bar_x = 16, bar_w = UI_SCREEN_W - 32, x = 12;
        gfx_rect(0, 0, UI_SCREEN_W, 28, 0x000000, 150);
        gfx_icon("clapperboard", 16, 12, 6, UI_ACCENT, 255);
        gfx_text(FONT_BODY, 34, 4, pi.title, UI_TEXT, 255, UI_SCREEN_W - 120);
        if (g_qn > 1) { char s[32]; snprintf(s, sizeof(s), "%d / %d", g_at + 1, g_qn); gfx_text(FONT_SMALL, UI_SCREEN_W - 12 - gfx_text_width(FONT_SMALL, s), 7, s, UI_MUTED, 255, 0); }
        gfx_rect(0, UI_SCREEN_H - 52, UI_SCREEN_W, 52, 0x000000, 150);
        gfx_rect(bar_x, UI_SCREEN_H - 46, bar_w, 5, 0x2a3145, 255);
        if (pi.dur > 0) gfx_rect(bar_x, UI_SCREEN_H - 46, (int)(bar_w * (pi.pos / pi.dur > 1 ? 1 : pi.pos / pi.dur)), 5, UI_ACCENT, 255);
        fmt_time(pi.pos, t1, sizeof(t1));
        if (pi.dur > 0) fmt_time(pi.dur, t2, sizeof(t2)); else snprintf(t2, sizeof(t2), "--:--");
        gfx_text(FONT_SMALL, bar_x, UI_SCREEN_H - 38, t1, UI_SUB, 255, 0);
        gfx_text(FONT_SMALL, bar_x + bar_w - gfx_text_width(FONT_SMALL, t2), UI_SCREEN_H - 38, t2, UI_SUB, 255, 0);
        {
            const char *st = pi.ended ? "Ended" : pi.paused ? "Paused" : "";
            if (st[0]) gfx_text(FONT_SMALL, (UI_SCREEN_W - gfx_text_width(FONT_SMALL, st)) / 2, UI_SCREEN_H - 38, st, UI_ACCENT, 255, 0);
        }
        if (g_msg[0]) gfx_text(FONT_BADGE, 16, UI_SCREEN_H - 62, g_msg, 0xff9a9a, 255, UI_SCREEN_W - 32);
        x = ui_hint_home();
        x = ui_hint_btn(x, 'x', !pi.paused && !pi.ended ? "Pause" : "Play");
        x = ui_hint_cap(x, "LEFT/RIGHT", g_qn > 1 ? "Film, held: seek" : "Held: seek", UI_SUB);
        x = ui_hint_btn(x, 'o', g_back ? "Gallery" : "Library");
        x = ui_hint_btn(x, 't', "Options");
        ui_hint_start("Quick");
    }
    notify_draw(now);
    pspexp_present(ctx);
    return 1;
}

static void mu_render(PspExpContext *ctx, uint64_t now)
{
    static const char *tab[NVIEWS] = { "Now playing", "Library", "Queue" };
    int i, x, tx = 112;
    char s[64];
    g_dirty = 0;
    if (draw_film(ctx, now)) return;
    gfx_frame(ctx);
    pspexp_begin_frame(ctx);
    pspexp_set_clip(ctx, 0, 0, UI_SCREEN_W, UI_SCREEN_H);
    gfx_backdrop();
    ui_app_bar("clapperboard", "Media");
    gfx_nine(NINE_R6, tx - 20, 9, 16, 14, 0x1e2c4a, 255);   /* the views, as tabs: L and R on either side */
    gfx_text(FONT_BADGE, tx - 15, 10, "L", UI_SUB, 255, 0);
    for (i = 0; i < NVIEWS; i++) {
        int w = gfx_text_width(FONT_SMALL, tab[i]) + 18, on = i == g_view;
        if (on) gfx_nine(NINE_R6, tx, 7, w, 18, UI_ACCENT, 200);
        gfx_text(FONT_SMALL, tx + 9, 8, tab[i], on ? 0xffffff : UI_SUB, 255, 0);
        tx += w + 4;
    }
    gfx_nine(NINE_R6, tx, 9, 16, 14, 0x1e2c4a, 255);
    gfx_text(FONT_BADGE, tx + 5, 10, "R", UI_SUB, 255, 0);
    x = UI_SCREEN_W - 12;   /* the play order, top right */
    if (g_repeat != R_OFF) { x -= 16; gfx_icon(g_repeat == R_ONE ? "repeat-1" : "repeat", 16, x, 8, UI_SUB, 255); x -= 6; }
    if (g_shuffle) { x -= 16; gfx_icon("shuffle", 16, x, 8, UI_SUB, 255); x -= 6; }
    if (g_qn) { snprintf(s, sizeof(s), "%d / %d", g_at + 1, g_qn); x -= gfx_text_width(FONT_SMALL, s); gfx_text(FONT_SMALL, x, 9, s, UI_MUTED, 255, 0); }
    if (g_view == V_NOW) draw_now(now);
    else if (g_view == V_BROWSE) draw_browse();
    else draw_queue();
    if (g_msg[0]) gfx_text(FONT_BADGE, 16, 236, g_msg, 0xff9a9a, 255, UI_SCREEN_W - 32);
    else if (g_note[0]) gfx_text(FONT_BADGE, 16, 236, g_note, UI_ACCENT, 255, UI_SCREEN_W - 32);
    x = g_menu.open || g_info ? 12 : ui_hint_home();
    if (g_menu.open || g_info) {
        /* (the menu draws its own hints; the box has its button) */
    } else if (g_view == V_NOW) {
        PlayerInfo pi;
        int have = player_info(&pi);
        x = ui_hint_btn(x, 'x', have && !pi.paused && !pi.ended ? "Pause" : "Play");
        if (g_qn > 1 || have) x = ui_hint_cap(x, "LEFT/RIGHT", g_qn > 1 ? "Track, held: seek" : "Held: seek", UI_SUB);
        x = ui_hint_btn(x, 'o', "Library");
        x = ui_hint_btn(x, 't', "Options");
        ui_hint_start("Quick");
    } else if (g_view == V_BROWSE) {
        x = ui_hint_btn(x, 'x', g_dir[0] && g_bsel < g_nspec ? "Play" : "Open / play");
        if (g_dir[0] || g_up) x = ui_hint_btn(x, 'o', "Up");
        x = ui_hint_btn(x, 't', "Options");
        ui_hint_start("Quick");
    } else {
        x = ui_hint_btn(x, 'x', "Play");
        x = ui_hint_btn(x, 'o', "Library");
        x = ui_hint_btn(x, 't', "Options");
        ui_hint_start("Quick");
    }
    if (g_info) draw_info();
    if (g_menu.open) ui_menu_draw(&g_menu);
    notify_draw(now);
    pspexp_present(ctx);
}

static int mu_take_exit(void)
{
    int e = g_exit;
    g_exit = 0;
    if (e) pspexp_set_max_in_flight(g_ctx, 1);   /* (Media's two, for a film, end with it) */
    return e;
}

static void mu_state(char *out, size_t n)
{
    PlayerInfo pi;
    static const char *vn[NVIEWS] = { "now", "browse", "queue" }, *rn[3] = { "off", "all", "one" };
    int have = player_info(&pi), va;
    unsigned long vr, vd;
    player_video_stats(&vr, &vd, &va);
    size_t l = strlen(out);
    if (l < n) snprintf(out + l, n - l, "music.view=%s\nmusic.track=%d/%d\nmusic.shuffle=%d\nmusic.repeat=%s\nmusic.playing=%d\nmusic.title=%s\nmusic.pos=%.1f\nmusic.dur=%.1f\nmusic.paused=%d\nmusic.ended=%d\nmusic.folder=%s\nmusic.error=%s\nmusic.note=%s\nmusic.video=%d\nmusic.video_frames=%u\nmusic.video_read=%lu\nmusic.video_dropped=%lu\nmusic.video_ahead=%d\nmusic.format=%s\n",
             vn[g_view], g_at + 1, g_qn, g_shuffle, rn[g_repeat], have, have ? pi.title : "", have ? pi.pos : 0, have ? pi.dur : 0,
             have ? pi.paused : 0, have ? pi.ended : 0, g_dir, g_msg, g_note, have ? pi.video : 0, g_vid_shown, vr, vd, va, have ? pi.format : "");
    {   /* media servers: where Browse is, what it lists */
        static const char *un[3] = { "off", "servers", "folder" };
        int k, cnt = g_up == UP_SERVERS ? g_nsrv : g_up == UP_FOLDER ? g_upn : 0;
        l = strlen(out);
        if (l < n) snprintf(out + l, n - l, "music.upnp=%s%s\nmusic.upnp_items=", un[g_up], g_up_busy ? " busy" : "");
        for (k = 0; k < cnt; k++) {
            l = strlen(out);
            if (l < n) snprintf(out + l, n - l, "%s%s", k ? "|" : "", g_up == UP_SERVERS ? g_srv[k].name : g_upl[k].title);
        }
        l = strlen(out);
        if (l < n) snprintf(out + l, n - l, "\nmusic.browse_sel=%d\n", g_bsel);
    }
}

void music_set_back(int on)
{
    if (!on && g_back && g_vid_loaded) { gfx_frame(g_ctx); gfx_video_free(); g_vid_loaded = 0; }   /* (the film's pictures leave the PSP with it) */
    g_back = on;
}

void music_leave(void)
{
    PlayerInfo pi;
    if (player_info(&pi) && pi.video && !pi.paused && !pi.ended) player_toggle_pause();
}

const AppView MUSIC_APP = { "music", mu_open, mu_input, mu_needs_frame, mu_render, mu_take_exit, mu_state, NULL, NULL };
