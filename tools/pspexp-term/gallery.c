/*
 * The Gallery (Apps > Gallery): what the note button made, the
 * screenshots, recordings and clips, a folder for each game in ~/Gallery (a Mac: ~/Movies/PSPExp;
 * the recorder's, main.c's capture_tick). Its first page the games, each with its latest
 * picture; in one, its pictures newest first. A screenshot opens full screen; a video plays in
 * Media's player and comes back here when it ends or on circle.
 *
 *   L / R      All, Screenshots, Videos         cross     open, play
 *   circle     back (nothing on the first page)  square    delete (asks)
 *   triangle   details                           START     the menu: delete, copy to the PSP, sort
 *
 * Thumbnails: a screenshot is its own (gfx_thumb crops it); a video's is a frame ffmpeg takes at
 * 1 s, kept in ~/.cache/pspexp/gallery with the video's length, made one at a time while the
 * pages show.
 */
#include "appview.h"
#include "files_ops.h"
#include "files_psp.h"
#include "launcher.h"
#include "launcher_gfx.h"
#include "launcher_ui.h"
#include "music.h"
#include "notify.h"
#include "player.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ALBUMS_MAX 128
#define ITEMS_MAX  4000

enum { K_SHOT, K_CLIP, K_REC, K_VIDEO };
enum { T_ALL, T_SHOTS, T_VIDEOS, NTABS };
enum { V_ALBUMS, V_ITEMS, V_PICTURE };

typedef struct GItem { char name[256]; int kind; time_t when; uint64_t size; float dur; } GItem;   /* dur: -1 not read yet */
typedef struct GAlbum { char name[256]; int nshot, nvid; time_t shot_at, vid_at; char shot[256], vid[256]; uint64_t bytes; } GAlbum;

static char g_root[FOPS_PATH];
static GAlbum g_al[ALBUMS_MAX];
static int g_nal;
static int g_ashown[ALBUMS_MAX], g_nashown;   /* the albums the tab shows, newest first */
static GItem *g_it;
static int g_nit;
static int g_ishown[ITEMS_MAX], g_nishown;    /* the album's items the tab shows, in the sort's order */
static int g_album = -1;                      /* the one open (V_ITEMS, V_PICTURE) */
static int g_view, g_tab, g_oldest, g_exit, g_dirty = 1;
static int g_asel, g_atop, g_isel, g_itop;
static char g_app_req[16];
static uint64_t g_hud_until;
static int g_pic_ok;                          /* the picture shown is loaded */

/* overlays: START's menu, triangle's details, square's question, a copy's progress, a message */
static UiMenu g_menu;
enum { GM_DELETE = 1, GM_COPY, GM_SORT, GM_PLAY };
static int g_more, g_confirm;
static char g_msg[200];
static uint64_t g_msg_until;

/* ---- what is where ------------------------------------------------------------------------------- */

static int kind_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return -1;
    if (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return K_SHOT;
    if (!player_is_video(name)) return -1;
    if (!strncmp(name, "clip", 4)) return K_CLIP;
    if (!strncmp(name, "recording", 9)) return K_REC;
    return K_VIDEO;
}

static int is_video(int k) { return k != K_SHOT; }
static int tab_takes(int k) { return g_tab == T_ALL || (g_tab == T_SHOTS) == (k == K_SHOT); }

/* when it was made: the recorder's names say ("screenshot-2026-10-28-21.10.02.png"), else the file's time */
static time_t when_of(const char *name, time_t mtime)
{
    const char *p = strchr(name, '-');
    struct tm t;
    memset(&t, 0, sizeof(t));
    if (p && sscanf(p + 1, "%4d-%2d-%2d-%2d.%2d.%2d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) == 6) {
        t.tm_year -= 1900; t.tm_mon -= 1; t.tm_isdst = -1;
        return mktime(&t);
    }
    return mtime;
}

static void album_path(int a, char *out, size_t n) { fops_join(out, n, g_root, g_al[a].name); }

static void item_path(int i, char *out, size_t n)
{
    char d[FOPS_PATH];
    album_path(g_album, d, sizeof(d));
    fops_join(out, n, d, g_it[i].name);
}

static int cmp_album(const void *a, const void *b)
{
    const GAlbum *x = &g_al[*(const int *)a], *y = &g_al[*(const int *)b];
    time_t tx = g_tab == T_SHOTS ? x->shot_at : g_tab == T_VIDEOS ? x->vid_at : (x->shot_at > x->vid_at ? x->shot_at : x->vid_at);
    time_t ty = g_tab == T_SHOTS ? y->shot_at : g_tab == T_VIDEOS ? y->vid_at : (y->shot_at > y->vid_at ? y->shot_at : y->vid_at);
    return tx < ty ? 1 : tx > ty ? -1 : strcasecmp(x->name, y->name);
}

static void albums_filter(void)
{
    int i;
    g_nashown = 0;
    for (i = 0; i < g_nal; i++)
        if (g_tab == T_ALL ? g_al[i].nshot + g_al[i].nvid > 0 : g_tab == T_SHOTS ? g_al[i].nshot > 0 : g_al[i].nvid > 0) g_ashown[g_nashown++] = i;
    qsort(g_ashown, (size_t)g_nashown, sizeof(int), cmp_album);
    if (g_asel >= g_nashown) g_asel = g_nashown ? g_nashown - 1 : 0;
}

/* the games' folders and what is in each (counted, the latest of each kind kept for its cover) */
static void albums_load(void)
{
    DIR *d, *s;
    struct dirent *e, *f;
    char keep[256] = "";
    if (g_nashown && g_asel < g_nashown) snprintf(keep, sizeof(keep), "%s", g_al[g_ashown[g_asel]].name);
    g_nal = 0;
    launcher_gallery_dir(g_root, sizeof(g_root));
    if ((d = opendir(g_root))) {
        while ((e = readdir(d)) && g_nal < ALBUMS_MAX) {
            char p[FOPS_PATH];
            GAlbum *a = &g_al[g_nal];
            if (e->d_name[0] == '.') continue;
            fops_join(p, sizeof(p), g_root, e->d_name);
            if (!(s = opendir(p))) continue;
            memset(a, 0, sizeof(*a));
            snprintf(a->name, sizeof(a->name), "%s", e->d_name);
            while ((f = readdir(s))) {
                char fp[FOPS_PATH];
                struct stat st;
                int k = f->d_name[0] == '.' ? -1 : kind_of(f->d_name);
                time_t w;
                if (k < 0) continue;
                fops_join(fp, sizeof(fp), p, f->d_name);
                if (stat(fp, &st) < 0 || !S_ISREG(st.st_mode)) continue;
                w = when_of(f->d_name, st.st_mtime);
                a->bytes += (uint64_t)st.st_size;
                if (k == K_SHOT) { a->nshot++; if (w >= a->shot_at) { a->shot_at = w; snprintf(a->shot, sizeof(a->shot), "%s", f->d_name); } }
                else { a->nvid++; if (w >= a->vid_at) { a->vid_at = w; snprintf(a->vid, sizeof(a->vid), "%s", f->d_name); } }
            }
            closedir(s);
            g_nal++;
        }
        closedir(d);
    }
    albums_filter();
    if (keep[0]) { int i; for (i = 0; i < g_nashown; i++) if (!strcmp(g_al[g_ashown[i]].name, keep)) g_asel = i; }
}

static int cmp_item(const void *a, const void *b)
{
    const GItem *x = &g_it[*(const int *)a], *y = &g_it[*(const int *)b];
    int r = x->when < y->when ? 1 : x->when > y->when ? -1 : strcmp(y->name, x->name);
    return g_oldest ? -r : r;
}

static void items_filter(void)
{
    int i;
    g_nishown = 0;
    for (i = 0; i < g_nit && g_nishown < ITEMS_MAX; i++) if (tab_takes(g_it[i].kind)) g_ishown[g_nishown++] = i;
    qsort(g_ishown, (size_t)g_nishown, sizeof(int), cmp_item);
    if (g_isel >= g_nishown) g_isel = g_nishown ? g_nishown - 1 : 0;
}

static void items_load(int a)
{
    DIR *d;
    struct dirent *e;
    char p[FOPS_PATH];
    g_album = a; g_nit = 0;
    if (!g_it) g_it = (GItem *)calloc(ITEMS_MAX, sizeof(GItem));
    if (!g_it) return;
    album_path(a, p, sizeof(p));
    if ((d = opendir(p))) {
        while ((e = readdir(d)) && g_nit < ITEMS_MAX) {
            char fp[FOPS_PATH];
            struct stat st;
            int k = e->d_name[0] == '.' ? -1 : kind_of(e->d_name);
            GItem *it = &g_it[g_nit];
            if (k < 0) continue;
            fops_join(fp, sizeof(fp), p, e->d_name);
            if (stat(fp, &st) < 0 || !S_ISREG(st.st_mode)) continue;
            memset(it, 0, sizeof(*it));
            snprintf(it->name, sizeof(it->name), "%s", e->d_name);
            it->kind = k; it->when = when_of(e->d_name, st.st_mtime); it->size = (uint64_t)st.st_size; it->dur = -1;
            g_nit++;
        }
        closedir(d);
    }
    items_filter();
}

/* ---- video thumbnails: ffmpeg, one at a time ------------------------------------------------------ */

static pid_t g_tjob;
static char g_tjob_src[FOPS_PATH];
static char g_failed[32][FOPS_PATH];   /* videos ffmpeg made nothing of (this session) */
static int g_nfailed;

/* the cache's name for a video (its path, size and time: a new file of the same name, a new one) */
static void thumb_base(const char *path, char *out, size_t n)
{
    struct stat st;
    char k[FOPS_PATH + 64];
    const char *h = getenv("HOME");
    if (stat(path, &st) < 0) memset(&st, 0, sizeof(st));
    snprintf(k, sizeof(k), "%s|%lld|%lld", path, (long long)st.st_size, (long long)st.st_mtime);
    snprintf(out, n, "%s/.cache/pspexp/gallery/%08x%08x", h && *h ? h : "/tmp", gfx_hash(k), gfx_hash(k + 1));
}

static void thumb_dirs(void)
{
    char p[FOPS_PATH];
    const char *h = getenv("HOME");
    snprintf(p, sizeof(p), "%s/.cache", h && *h ? h : "/tmp"); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/.cache/pspexp", h && *h ? h : "/tmp"); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/.cache/pspexp/gallery", h && *h ? h : "/tmp"); mkdir(p, 0755);
}

static void thumb_tick(void)
{
    int st;
    if (g_tjob <= 0 || waitpid(g_tjob, &st, WNOHANG) != g_tjob) return;
    g_tjob = 0;
    {
        char b[FOPS_PATH], png[FOPS_PATH + 8];
        struct stat s;
        thumb_base(g_tjob_src, b, sizeof(b));
        snprintf(png, sizeof(png), "%s.png", b);
        if (stat(png, &s) < 0 && g_nfailed < 32) snprintf(g_failed[g_nfailed++], FOPS_PATH, "%s", g_tjob_src);
    }
    g_dirty = 1;
}

/* the video's thumbnail (its cache file) when made; else started (when none is under way): NULL */
static const char *video_thumb(const char *path, float *dur)
{
    static char png[FOPS_PATH + 8];
    char b[FOPS_PATH];
    struct stat s;
    int i;
    thumb_base(path, b, sizeof(b));
    snprintf(png, sizeof(png), "%s.png", b);
    if (stat(png, &s) == 0) {
        if (dur && *dur < 0) {
            char lp[FOPS_PATH + 8];
            FILE *f;
            snprintf(lp, sizeof(lp), "%s.len", b);
            *dur = 0;
            if ((f = fopen(lp, "r"))) { if (fscanf(f, "%f", dur) != 1) *dur = 0; fclose(f); }
        }
        return png;
    }
    for (i = 0; i < g_nfailed; i++) if (!strcmp(g_failed[i], path)) return NULL;
    if (g_tjob > 0) return NULL;
    thumb_dirs();
    snprintf(g_tjob_src, sizeof(g_tjob_src), "%s", path);
    g_tjob = fork();
    if (g_tjob == 0) {
        int fd;
        for (fd = 3; fd < 1024; fd++) close(fd);   /* (none of the console's: the PSP's link) */
        fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); }
        setpgid(0, 0);
        execlp("nice", "nice", "-n", "10", "sh", "-c",
               "ffmpeg -nostdin -v error -y -ss 1 -i \"$1\" -frames:v 1 -vf scale=256:-2 \"$2.tmp.png\";"
               " [ -s \"$2.tmp.png\" ] || ffmpeg -nostdin -v error -y -i \"$1\" -frames:v 1 -vf scale=256:-2 \"$2.tmp.png\";"
               " ffprobe -v error -show_entries format=duration -of csv=p=0 \"$1\" > \"$2.len\";"
               " [ -s \"$2.tmp.png\" ] && mv \"$2.tmp.png\" \"$2.png\"; rm -f \"$2.tmp.png\"",
               "sh", path, b, (char *)NULL);
        _exit(127);
    }
    if (g_tjob < 0) g_tjob = 0;
    return NULL;
}

static void thumb_drop(const char *path)
{
    char b[FOPS_PATH], p[FOPS_PATH + 8];
    thumb_base(path, b, sizeof(b));
    snprintf(p, sizeof(p), "%s.png", b); unlink(p);
    snprintf(p, sizeof(p), "%s.len", b); unlink(p);
    gfx_thumb_forget(path);
}

/* ---- copying to the PSP: its Photo and Video folders (the PSP's own menu shows them) ----------
 * The pictures go as they are. The videos first become what the PSP's player takes (H.264 Main
 * profile, level 3.0: the recorder's before 2026-10-29 were High, which it does not play): a
 * child turns each that is not into a copy in ~/.cache/pspexp/gallery/to-psp (one that is: a
 * link there), while the pictures are copied; then they go from there. */

static FJob g_cjob[2];                 /* 0 the pictures, 1 the videos */
static int g_cphase = -1;              /* -1 none; 0 the pictures copying; 1 the videos being made ready; 2 copying */
static pid_t g_prep;
static int g_ncopied, g_nprep;
static char g_stage[FOPS_PATH];

static void fat_name(const char *s, char *out, size_t n)
{
    size_t i;
    snprintf(out, n, "%s", s);
    for (i = 0; out[i]; i++) if (strchr("\\/:*?\"<>|", out[i])) out[i] = '_';
}

static int copying(void) { return g_cphase >= 0; }

static void copy_end(const char *msg)
{
    int k;
    if (g_prep > 0) { kill(-g_prep, SIGTERM); waitpid(g_prep, NULL, 0); g_prep = 0; }
    for (k = 0; k < 2; k++) fjob_finish(&g_cjob[k]);   /* (the names of one not run too) */
    if (g_stage[0]) {   /* the videos made for it */
        DIR *d = opendir(g_stage);
        struct dirent *e;
        char p[FOPS_PATH];
        while (d && (e = readdir(d))) if (e->d_name[0] != '.') { fops_join(p, sizeof(p), g_stage, e->d_name); unlink(p); }
        if (d) closedir(d);
    }
    if (msg) snprintf(g_msg, sizeof(g_msg), "%s", msg);
    g_msg_until = 0;
    g_cphase = -1;
}

/* the videos named in job 1, made ready in the stage folder by a child (its own group: cancelled whole) */
static int prep_start(void)
{
    const FJob *j = &g_cjob[1];
    char **argv;
    int k;
    const char *h = getenv("HOME");
    thumb_dirs();
    snprintf(g_stage, sizeof(g_stage), "%s/.cache/pspexp/gallery/to-psp", h && *h ? h : "/tmp");
    mkdir(g_stage, 0755);
    if (!(argv = calloc((size_t)j->nnames + 10, sizeof(char *)))) return -1;
    argv[0] = "nice"; argv[1] = "-n"; argv[2] = "10"; argv[3] = "sh"; argv[4] = "-c";
    argv[5] = "src=$1; st=$2; shift 2; for n; do"
              " p=$(ffprobe -v error -select_streams v:0 -show_entries stream=profile -of csv=p=0 \"$src/$n\");"
              " rm -f \"$st/$n\";"
              " case \"$p\" in Main|Baseline|'Constrained Baseline') ln \"$src/$n\" \"$st/$n\" 2>/dev/null || cp \"$src/$n\" \"$st/$n\" || exit 1;;"
              " *) ffmpeg -nostdin -v error -y -i \"$src/$n\" -c:v libx264 -preset veryfast -profile:v main -level 3.0 -crf 20 -pix_fmt yuv420p"
              " -c:a aac -b:a 128k -movflags +faststart \"$st/$n.part.mp4\" && mv \"$st/$n.part.mp4\" \"$st/$n\" || exit 1;; esac; done";
    argv[6] = "sh";
    argv[7] = (char *)j->src;
    argv[8] = g_stage;
    for (k = 0; k < j->nnames; k++) argv[9 + k] = (char *)j->names[k];   /* (argv[6] is $0: src and the stage $1 and $2, the names after) */
    g_prep = fork();
    if (g_prep == 0) {
        int fd;
        setpgid(0, 0);
        for (fd = 3; fd < 1024; fd++) close(fd);   /* (none of the console's: the PSP's link) */
        fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); }
        execvp("nice", argv);
        _exit(127);
    }
    free(argv);
    if (g_prep < 0) { g_prep = 0; return -1; }
    setpgid(g_prep, g_prep);
    g_nprep = j->nnames;
    return 0;
}

/* the shown items of album a (the one item when one >= 0) to ms0:/PICTURE/<game> and ms0:/VIDEO/<game> */
static void copy_start(int a, int one)
{
    char src[FOPS_PATH], game[256], err[160];
    int pass, i;
    if (!files_psp_available()) { snprintf(g_msg, sizeof(g_msg), "The PSP's memory stick cannot be reached now"); g_msg_until = 0; return; }
    if (copying()) return;
    album_path(a, src, sizeof(src));
    fat_name(g_al[a].name, game, sizeof(game));
    g_ncopied = 0; g_stage[0] = 0;
    memset(g_cjob, 0, sizeof(g_cjob));
    for (pass = 0; pass < 2; pass++) {   /* pictures, then videos */
        FJob *j = &g_cjob[pass];
        int n = 0, cap = 0;
        char (*names)[256] = NULL, top[FOPS_PATH];
        if (one >= 0) {
            if ((pass == 0) != (g_it[one].kind == K_SHOT)) continue;
            names = malloc(sizeof(*names)); snprintf(names[0], 256, "%s", g_it[one].name); n = 1;
        } else if (g_album == a) {
            for (i = 0; i < g_nishown; i++) {
                const GItem *it = &g_it[g_ishown[i]];
                if ((pass == 0) != (it->kind == K_SHOT)) continue;
                if (n == cap) { cap = cap ? cap * 2 : 16; names = realloc(names, (size_t)cap * sizeof(*names)); }
                snprintf(names[n++], 256, "%s", it->name);
            }
        }
        if (!n || !names) { free(names); continue; }
        j->kind = FJOB_COPY;
        snprintf(j->src, sizeof(j->src), "%s", src);
        snprintf(top, sizeof(top), "ms0:/%s", pass == 0 ? "PICTURE" : "VIDEO");
        fops_mkdir(top, err, sizeof(err));   /* (there already: fine) */
        snprintf(j->dst, sizeof(j->dst), "%s/%s", top, game);
        fops_mkdir(j->dst, err, sizeof(err));
        j->names = names; j->nnames = n;
        g_ncopied += n;
    }
    if (!g_ncopied) return;
    g_cphase = 0;
    if (g_cjob[1].nnames && prep_start() < 0) { copy_end("The videos could not be made ready for the PSP"); return; }
    if (g_cjob[0].nnames && fjob_start(&g_cjob[0]) < 0) { copy_end("The copy could not start"); return; }
    if (!g_cjob[0].nnames) g_cphase = 1;
}

static void copy_tick(void)
{
    if (!copying()) return;
    g_dirty = 1;   /* (its progress) */
    if (g_cphase == 0) {
        FJob *j = &g_cjob[0];
        if (!j->done) return;
        if (j->error[0] || j->cancel) {
            char m[300];
            snprintf(m, sizeof(m), j->cancel ? "The copy was cancelled" : "Not copied: %s", j->error);
            copy_end(m);
            return;
        }
        fjob_finish(j);
        if (!g_cjob[1].nnames) { copy_end(NULL); goto copied; }
        g_cphase = 1;
    }
    if (g_cphase == 1) {
        int st;
        if (g_prep > 0 && waitpid(g_prep, &st, WNOHANG) != g_prep) return;
        if (g_prep > 0 && !(WIFEXITED(st) && WEXITSTATUS(st) == 0)) { g_prep = 0; copy_end("The videos could not be made ready for the PSP"); return; }
        g_prep = 0;
        snprintf(g_cjob[1].src, sizeof(g_cjob[1].src), "%s", g_stage);
        if (fjob_start(&g_cjob[1]) < 0) { copy_end("The copy could not start"); return; }
        g_cphase = 2;
        return;
    }
    if (g_cphase == 2) {
        FJob *j = &g_cjob[1];
        char m[300];
        if (!j->done) return;
        if (j->error[0] || j->cancel) {
            snprintf(m, sizeof(m), j->cancel ? "The copy was cancelled" : "Not copied: %s", j->error);
            copy_end(m);
            return;
        }
        copy_end(NULL);
    }
copied:
    snprintf(g_msg, sizeof(g_msg), "Copied %d to the PSP: its Photo and Video menus show %s", g_ncopied, g_ncopied == 1 ? "it" : "them");
    g_msg_until = 0;
}

static void copy_cancel(void)
{
    if (g_cphase == 1) copy_end("The copy was cancelled");
    else if (g_cphase >= 0) g_cjob[g_cphase == 0 ? 0 : 1].cancel = 1;
}

/* ---- deleting --------------------------------------------------------------------------------- */

static int cur_item(void) { return g_nishown && g_isel < g_nishown ? g_ishown[g_isel] : -1; }
static int cur_album(void) { return g_nashown && g_asel < g_nashown ? g_ashown[g_asel] : -1; }

static const char *kind_word(int k, int many)
{
    static const char *w[4][2] = { { "screenshot", "screenshots" }, { "clip", "clips" }, { "recording", "recordings" }, { "video", "videos" } };
    return w[k][many];
}

static void delete_now(void)
{
    char p[FOPS_PATH];
    int i, gone = 0;
    if (g_view == V_ALBUMS) {
        int a = cur_album();
        if (a < 0) return;
        items_load(a);
        for (i = 0; i < g_nishown; i++) {
            item_path(g_ishown[i], p, sizeof(p));
            if (unlink(p) == 0) { gone++; if (is_video(g_it[g_ishown[i]].kind)) thumb_drop(p); else gfx_thumb_forget(p); }
        }
        album_path(a, p, sizeof(p));
        rmdir(p);   /* (only when nothing else is left in it) */
        g_album = -1;
        albums_load();
    } else {
        int i0 = cur_item();
        if (i0 < 0) return;
        item_path(i0, p, sizeof(p));
        if (unlink(p) == 0) { gone = 1; if (is_video(g_it[i0].kind)) thumb_drop(p); else gfx_thumb_forget(p); }
        else snprintf(g_msg, sizeof(g_msg), "Not deleted: %s", strerror(errno));
        items_load(g_album);
        albums_load();
        if (!g_nishown) { g_view = V_ALBUMS; g_album = -1; }
        else if (g_view == V_PICTURE) {   /* the next picture, else back to the grid */
            int k, found = -1;
            for (k = g_isel; k < g_nishown && found < 0; k++) if (g_it[g_ishown[k]].kind == K_SHOT) found = k;
            for (k = g_isel - 1; k >= 0 && found < 0; k--) if (g_it[g_ishown[k]].kind == K_SHOT) found = k;
            if (found < 0) { g_view = V_ITEMS; gfx_image_free(); }
            else { g_isel = found; g_pic_ok = 0; }
        }
    }
    if (gone && !g_msg[0]) { snprintf(g_msg, sizeof(g_msg), "Deleted %d", gone); g_msg_until = 0; }
}

/* ---- the app ------------------------------------------------------------------------------------- */

static void ga_open(void)
{
    g_exit = 0; g_menu.open = 0; g_more = 0; g_confirm = 0; g_msg[0] = 0; g_dirty = 1;
    g_view = V_ALBUMS; g_album = -1; g_atop = 0; g_tab = T_ALL;
    albums_load();
}

static void open_item(uint64_t now)
{
    int i = cur_item();
    char p[FOPS_PATH], d[FOPS_PATH];
    if (i < 0) return;
    if (g_it[i].kind == K_SHOT) { g_view = V_PICTURE; g_pic_ok = 0; g_hud_until = now + 3000000; return; }   /* (its buttons shown a moment) */
    {   /* a video: Media's player, back here after (main.c: the app that asked) */
        FEntry e;
        memset(&e, 0, sizeof(e));
        snprintf(e.name, sizeof(e.name), "%s", g_it[i].name);
        e.size = g_it[i].size;
        album_path(g_album, d, sizeof(d));
        item_path(i, p, sizeof(p));
        if (music_play_one(d, &e, 1, 0) < 0) { snprintf(g_msg, sizeof(g_msg), "This video does not play"); g_msg_until = 0; return; }
        music_set_back(1);
        snprintf(g_app_req, sizeof(g_app_req), "music");
    }
}

static void picture_step(int d)
{
    int k;
    for (k = g_isel + d; k >= 0 && k < g_nishown; k += d)
        if (g_it[g_ishown[k]].kind == K_SHOT) { g_isel = k; g_pic_ok = 0; return; }
}

static void menu_open(void)
{
    int a = g_view == V_ALBUMS ? cur_album() : g_album, i = g_view == V_ALBUMS ? -1 : cur_item();
    if (g_view == V_ALBUMS && a < 0) {
        ui_menu_begin(&g_menu, "Gallery");
    } else if (i >= 0) {
        ui_menu_begin(&g_menu, kind_word(g_it[i].kind, 0));
        g_menu.title[0] = (char)(g_menu.title[0] - 32);   /* "Screenshot" */
        if (g_view == V_ITEMS) ui_menu_add(&g_menu, GM_PLAY, g_it[i].kind == K_SHOT ? "image" : "circle-play", g_it[i].kind == K_SHOT ? "Show" : "Play", NULL, 0, 0);
        ui_menu_add(&g_menu, GM_COPY, "memory-stick", "Copy to the PSP", NULL, 0, 0);
        ui_menu_add(&g_menu, GM_DELETE, "trash-2", "Delete", NULL, 0, 0);
    } else {
        ui_menu_begin(&g_menu, g_al[a].name);
        ui_menu_add(&g_menu, GM_COPY, "memory-stick", g_tab == T_ALL ? "Copy all to the PSP" : g_tab == T_SHOTS ? "Copy its screenshots to the PSP" : "Copy its videos to the PSP", NULL, 0, 0);
        ui_menu_add(&g_menu, GM_DELETE, "trash-2", g_tab == T_ALL ? "Delete all" : g_tab == T_SHOTS ? "Delete its screenshots" : "Delete its videos", NULL, 0, 0);
    }
    if (g_view != V_PICTURE) ui_menu_add(&g_menu, GM_SORT, "arrow-up-down", "Order", g_oldest ? "oldest first" : "newest first", g_menu.n > 0, 0);
}

static void ga_input(uint32_t b, uint32_t pressed, int up, int down, int left, int right, uint64_t now)
{
    (void)b;
    if (!pressed && !up && !down && !left && !right) return;
    g_dirty = 1;
    if (g_msg[0] && pressed) g_msg[0] = 0;
    if (copying()) { if (pressed & PSPEXP_BUTTON_CIRCLE) copy_cancel(); return; }   /* a copy: circle cancels */
    if (g_more) { if (pressed) g_more = 0; return; }
    if (g_confirm) {
        if (pressed & PSPEXP_BUTTON_CROSS) { g_confirm = 0; delete_now(); }
        else if (pressed & PSPEXP_BUTTON_CIRCLE) g_confirm = 0;
        return;
    }
    if (g_menu.open) {
        int id = ui_menu_input(&g_menu, pressed, up, down);
        if (id == GM_DELETE) g_confirm = 1;
        else if (id == GM_PLAY) open_item(now);
        else if (id == GM_COPY) {
            if (g_view == V_ALBUMS) { int a = cur_album(); if (a >= 0) { int keep_sel = g_isel; items_load(a); g_isel = keep_sel; copy_start(a, -1); g_album = -1; } }
            else copy_start(g_album, cur_item());
        } else if (id == GM_SORT) { g_oldest = !g_oldest; if (g_view == V_ITEMS) { g_isel = 0; g_itop = 0; items_filter(); } }
        return;
    }
    if (g_view == V_PICTURE) {
        g_hud_until = now + 3000000;
        if (left) picture_step(-1);
        if (right) picture_step(1);
        if (pressed & PSPEXP_BUTTON_CIRCLE) { g_view = V_ITEMS; gfx_image_free(); }
        if (pressed & PSPEXP_BUTTON_SQUARE) g_confirm = 1;
        if (pressed & PSPEXP_BUTTON_TRIANGLE) g_more = 1;
        if (pressed & PSPEXP_BUTTON_START) menu_open();
        return;
    }
    if (pressed & (PSPEXP_BUTTON_LTRIGGER | PSPEXP_BUTTON_RTRIGGER)) {   /* the tabs */
        g_tab = (g_tab + ((pressed & PSPEXP_BUTTON_RTRIGGER) ? 1 : NTABS - 1)) % NTABS;
        albums_filter();
        if (g_view == V_ITEMS) {
            items_filter();
            if (!g_nishown) { g_view = V_ALBUMS; g_album = -1; }   /* (nothing of that kind in it) */
        }
        g_atop = g_itop = 0;
        return;
    }
    if (pressed & PSPEXP_BUTTON_START) { menu_open(); return; }
    if (g_view == V_ALBUMS) {
        const int C = 3;
        if (left && g_asel > 0) g_asel--;
        if (right && g_asel < g_nashown - 1) g_asel++;
        if (up && g_asel >= C) g_asel -= C;
        if (down && g_asel + C < g_nashown) g_asel += C;
        else if (down && g_asel / C < (g_nashown - 1) / C) g_asel = g_nashown - 1;
        if ((pressed & PSPEXP_BUTTON_CROSS) && cur_album() >= 0) { items_load(cur_album()); g_isel = 0; g_itop = 0; g_view = V_ITEMS; }
        if ((pressed & PSPEXP_BUTTON_SQUARE) && cur_album() >= 0) g_confirm = 1;
        if ((pressed & PSPEXP_BUTTON_TRIANGLE) && cur_album() >= 0) g_more = 1;
        /* (circle: nothing here, it never leaves an app: HOME does) */
    } else {
        const int C = 4;
        if (left && g_isel > 0) g_isel--;
        if (right && g_isel < g_nishown - 1) g_isel++;
        if (up && g_isel >= C) g_isel -= C;
        if (down && g_isel + C < g_nishown) g_isel += C;
        else if (down && g_isel / C < (g_nishown - 1) / C) g_isel = g_nishown - 1;
        if (pressed & PSPEXP_BUTTON_CROSS) open_item(now);
        if ((pressed & PSPEXP_BUTTON_SQUARE) && cur_item() >= 0) g_confirm = 1;
        if ((pressed & PSPEXP_BUTTON_TRIANGLE) && cur_item() >= 0) g_more = 1;
        if (pressed & PSPEXP_BUTTON_CIRCLE) { g_view = V_ALBUMS; g_album = -1; albums_load(); }
    }
}

static int g_pending;   /* thumbnails still to come: frames go on until they are drawn */

static int ga_needs_frame(uint64_t now)
{
    static int hud_was;
    int hud = g_view == V_PICTURE && now < g_hud_until;
    thumb_tick();
    copy_tick();
    if (hud != hud_was) { hud_was = hud; g_dirty = 1; }
    if (g_msg[0] && !g_msg_until) g_msg_until = now + 5000000u;
    if (g_msg[0] && now > g_msg_until && !copying()) { g_msg[0] = 0; g_dirty = 1; }
    return g_dirty || g_pending;
}

/* ---- drawing ------------------------------------------------------------------------------------- */

static void fmt_len(float s, char *o, size_t n)
{
    int t = (int)(s + 0.5f);
    if (t >= 3600) snprintf(o, n, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(o, n, "%d:%02d", t / 60, t % 60);
}

static void fmt_when(time_t t, char *o, size_t n)
{
    struct tm lt, nt;
    time_t now = time(NULL);
    localtime_r(&t, &lt); localtime_r(&now, &nt);
    if (lt.tm_year == nt.tm_year && lt.tm_yday == nt.tm_yday) strftime(o, n, "Today, %H:%M", &lt);
    else if (lt.tm_year == nt.tm_year) strftime(o, n, "%e %b, %H:%M", &lt);
    else strftime(o, n, "%e %b %Y", &lt);
    if (o[0] == ' ') memmove(o, o + 1, strlen(o));
}

/* the picture of an item (its own, or its video's frame) filling (x, y, w, h); a placeholder until then */
static void draw_thumb(const char *path, int kind, float *dur, int x, int y, int w, int h)
{
    const char *pic = kind == K_SHOT ? path : video_thumb(path, dur);
    int r = pic ? gfx_thumb(pic, x, y, w, h, 255) : -1;
    if (r == 0) g_pending = 1;   /* (a video's frame being made: thumb_tick draws again when it is there) */
    if (r <= 0) {
        gfx_nine(NINE_R6, x, y, w, h, 0x1b2a46, 255);
        gfx_icon(kind == K_SHOT ? "image" : "film", 20, x + (w - 20) / 2, y + (h - 20) / 2, UI_MUTED, 200);
    }
}

/* a video's mark on its thumbnail, bottom right: what it is and how long */
static void draw_badge(int kind, float dur, int x, int y, int w, int h)
{
    char s[24] = "";
    int bw;
    if (kind == K_SHOT) return;
    if (dur > 0) fmt_len(dur, s, sizeof(s));
    bw = 18 + (s[0] ? gfx_text_width(FONT_BADGE, s) + 4 : 0);
    gfx_nine(NINE_R6, x + w - bw - 4, y + h - 18, bw, 14, 0x000000, 170);
    gfx_icon(kind == K_CLIP ? "history" : "video", 10, x + w - bw + 1, y + h - 16, 0xffffff, 255);
    if (s[0]) gfx_text(FONT_BADGE, x + w - bw + 15, y + h - 18, s, 0xffffff, 255, 0);
}

static void draw_header(void)
{
    static const char *tab[NTABS] = { "All", "Screenshots", "Videos" };
    int i, tx = 132;
    ui_app_bar("images", "Gallery");
    gfx_nine(NINE_R6, tx - 20, 9, 16, 14, 0x1e2c4a, 255);   /* the kinds, as tabs: L and R on either side */
    gfx_text(FONT_BADGE, tx - 15, 10, "L", UI_SUB, 255, 0);
    for (i = 0; i < NTABS; i++) {
        int w = gfx_text_width(FONT_SMALL, tab[i]) + 18, on = i == g_tab;
        if (on) gfx_nine(NINE_R6, tx, 7, w, 18, UI_ACCENT, 200);
        gfx_text(FONT_SMALL, tx + 9, 8, tab[i], on ? 0xffffff : UI_SUB, 255, 0);
        tx += w + 4;
    }
    gfx_nine(NINE_R6, tx, 9, 16, 14, 0x1e2c4a, 255);
    gfx_text(FONT_BADGE, tx + 5, 10, "R", UI_SUB, 255, 0);
}

static void draw_empty(const char *what)
{
    int y = 92;
    gfx_icon("images", 32, (UI_SCREEN_W - 32) / 2, y, UI_MUTED, 220);
    gfx_text(FONT_BODY, (UI_SCREEN_W - gfx_text_width(FONT_BODY, what)) / 2, y + 42, what, UI_TEXT, 255, 0);
    {
        const char *l1 = "On the PSP: ♪ + L twice takes a screenshot, ♪ + L records,", *l2 = "♪ + R keeps the last seconds (Settings / Recording / Clips)";
        gfx_text(FONT_SMALL, (UI_SCREEN_W - gfx_text_width(FONT_SMALL, l1)) / 2, y + 66, l1, UI_SUB, 255, 0);
        gfx_text(FONT_SMALL, (UI_SCREEN_W - gfx_text_width(FONT_SMALL, l2)) / 2, y + 82, l2, UI_SUB, 255, 0);
    }
}

/* the games: 3 x 2 tiles, each its latest picture, its name and what is in it */
static void draw_albums(void)
{
    const int C = 3, TW = 144, TH = 72, GX = 12, X0 = 12, Y0 = 36, RH = 104;
    int i;
    if (!g_nashown) { draw_empty(g_tab == T_ALL ? "Nothing here yet" : g_tab == T_SHOTS ? "No screenshots yet" : "No videos yet"); return; }
    if (g_asel / C < g_atop) g_atop = g_asel / C;
    if (g_asel / C >= g_atop + 2) g_atop = g_asel / C - 1;
    for (i = g_atop * C; i < g_nashown && i < (g_atop + 2) * C; i++) {
        const GAlbum *a = &g_al[g_ashown[i]];
        int x = X0 + (i % C) * (TW + GX), y = Y0 + (i / C - g_atop) * RH, sel = i == g_asel;
        int shot = g_tab == T_SHOTS || (g_tab == T_ALL && (a->nvid == 0 || (a->nshot && a->shot_at >= a->vid_at)));
        char p[FOPS_PATH], d[FOPS_PATH], s[96];
        float dur = 0;
        fops_join(d, sizeof(d), g_root, a->name);
        fops_join(p, sizeof(p), d, shot ? a->shot : a->vid);
        if (sel) { gfx_shadow(NINE_SHADOW10, x - 3, y - 3, TW + 6, TH + 6, UI_ACCENT, 120); gfx_nine(NINE_R10, x - 3, y - 3, TW + 6, TH + 6, UI_ACCENT, 255); }
        draw_thumb(p, shot ? K_SHOT : K_REC, &dur, x, y, TW, TH);
        draw_badge(shot ? K_SHOT : K_REC, 0, x, y, TW, TH);   /* (a video's cover: its mark) */
        gfx_text(FONT_SMALL, x + 2, y + TH + 4, a->name, sel ? 0xffffff : UI_TEXT, 255, TW - 4);
        if (g_tab == T_SHOTS) snprintf(s, sizeof(s), "%d %s", a->nshot, kind_word(K_SHOT, a->nshot != 1));
        else if (g_tab == T_VIDEOS) snprintf(s, sizeof(s), "%d %s", a->nvid, kind_word(K_VIDEO, a->nvid != 1));
        else if (a->nshot && a->nvid) snprintf(s, sizeof(s), "%d %s, %d %s", a->nshot, kind_word(K_SHOT, a->nshot != 1), a->nvid, kind_word(K_VIDEO, a->nvid != 1));
        else snprintf(s, sizeof(s), "%d %s", a->nshot + a->nvid, a->nshot ? kind_word(K_SHOT, a->nshot != 1) : kind_word(K_VIDEO, a->nvid != 1));
        gfx_text(FONT_BADGE, x + 2, y + TH + 19, s, UI_MUTED, 255, TW - 4);
    }
    if (g_atop > 0) gfx_icon("chevron-up", 12, UI_SCREEN_W / 2 - 6, 26, UI_MUTED, 255);
    if ((g_atop + 2) * C < g_nashown) gfx_icon("chevron-down", 12, UI_SCREEN_W / 2 - 6, 240, UI_MUTED, 255);
}

/* a game's pictures: 4 a row, 3 rows; the highlighted one's kind and date on the line above */
static void draw_items(void)
{
    const int C = 4, TW = 104, TH = 58, GX = 8, X0 = 16, Y0 = 52, RH = 64;
    int i, n;
    char s[160], d[FOPS_PATH];
    const GAlbum *a = &g_al[g_album];
    gfx_text(FONT_BODY, 16, 31, a->name, UI_TEXT, 255, 250);
    if (cur_item() >= 0) {
        const GItem *it = &g_it[cur_item()];
        char w[48];
        fmt_when(it->when, w, sizeof(w));
        snprintf(s, sizeof(s), "%s, %s   %d / %d", kind_word(it->kind, 0), w, g_isel + 1, g_nishown);
        s[0] = (char)(s[0] - 32);
        gfx_text(FONT_SMALL, UI_SCREEN_W - 16 - gfx_text_width(FONT_SMALL, s), 33, s, UI_SUB, 255, 0);
    }
    if (g_isel / C < g_itop) g_itop = g_isel / C;
    if (g_isel / C >= g_itop + 3) g_itop = g_isel / C - 2;
    album_path(g_album, d, sizeof(d));
    for (n = 0, i = g_itop * C; i < g_nishown && i < (g_itop + 3) * C; i++, n++) {
        GItem *it = &g_it[g_ishown[i]];
        int x = X0 + (i % C) * (TW + GX), y = Y0 + (i / C - g_itop) * RH;
        char p[FOPS_PATH];
        fops_join(p, sizeof(p), d, it->name);
        if (i == g_isel) { gfx_shadow(NINE_SHADOW10, x - 3, y - 3, TW + 6, TH + 6, UI_ACCENT, 120); gfx_nine(NINE_R10, x - 3, y - 3, TW + 6, TH + 6, UI_ACCENT, 255); }
        draw_thumb(p, it->kind, &it->dur, x, y, TW, TH);
        draw_badge(it->kind, it->dur, x, y, TW, TH);
    }
    if (g_itop > 0) gfx_icon("chevron-up", 12, UI_SCREEN_W / 2 - 6, 43, UI_MUTED, 255);
    if ((g_itop + 3) * C < g_nishown) gfx_icon("chevron-down", 12, UI_SCREEN_W / 2 - 6, 242, UI_MUTED, 255);
}

/* a screenshot, full screen; its name and place over it for a few seconds after a key */
static void draw_picture(uint64_t now)
{
    int i = cur_item(), w, h, n = 0, at = 0, k;
    char p[FOPS_PATH], s[160], when[48];
    if (i < 0) return;
    item_path(i, p, sizeof(p));
    if (!g_pic_ok) g_pic_ok = gfx_image_load(p, UI_SCREEN_W, UI_SCREEN_H, NULL, NULL) == 0 ? 1 : -1;
    gfx_rect(0, 0, UI_SCREEN_W, UI_SCREEN_H, 0x000000, 255);
    if (g_pic_ok > 0) { gfx_image_size(&w, &h); gfx_image_draw((UI_SCREEN_W - w) / 2, (UI_SCREEN_H - h) / 2); }
    else gfx_text(FONT_SMALL, 16, UI_SCREEN_H / 2 - 8, "This picture cannot be shown", UI_SUB, 255, 0);
    if (now >= g_hud_until && !g_menu.open && !g_more && !g_confirm && !g_msg[0]) return;
    for (k = 0; k < g_nishown; k++) if (g_it[g_ishown[k]].kind == K_SHOT) { n++; if (k == g_isel) at = n; }
    gfx_rect(0, 0, UI_SCREEN_W, 26, 0x000000, 190);
    gfx_icon("image", 14, 12, 6, UI_ACCENT, 255);
    gfx_text(FONT_SMALL, 32, 5, g_al[g_album].name, UI_TEXT, 255, 230);
    fmt_when(g_it[i].when, when, sizeof(when));
    snprintf(s, sizeof(s), "%s   %d / %d", when, at, n);
    gfx_text(FONT_SMALL, UI_SCREEN_W - 12 - gfx_text_width(FONT_SMALL, s), 5, s, UI_SUB, 255, 0);
    gfx_rect(0, UI_HINT_Y - 6, UI_SCREEN_W, UI_SCREEN_H - UI_HINT_Y + 6, 0x000000, 190);   /* (the menu's hints too: on it, readable) */
    if (!g_menu.open && !g_more && !g_confirm) {
        int x = 12;
        if (n > 1) x = ui_hint_cap(x, "LEFT/RIGHT", "Previous / next", UI_SUB);
        x = ui_hint_btn(x, 's', "Delete");
        x = ui_hint_btn(x, 't', "Details");
        x = ui_hint_btn(x, 'o', "Back");
        x = ui_hint_cap(x, "START", "Menu", UI_SUB);
    }
}

static void draw_box(int h, const char *title)
{
    int w = 320, bx = (UI_SCREEN_W - w) / 2, by = (UI_SCREEN_H - h) / 2 - 6;
    gfx_shadow(NINE_SHADOW14, bx, by, w, h, 0x000000, 160);
    gfx_nine(NINE_R14, bx, by, w, h, 0x1b2a46, 250);
    gfx_text(FONT_BODY, bx + 16, by + 12, title, UI_TEXT, 255, w - 32);
}

static void draw_details(void)
{
    const int w = 320, h = 112, bx = (UI_SCREEN_W - w) / 2, by = (UI_SCREEN_H - h) / 2 - 6;
    char l1[200], l2[200], l3[FOPS_PATH + 8], sz[32], when[48];
    if (g_view == V_ALBUMS) {
        const GAlbum *a = &g_al[cur_album()];
        draw_box(h, a->name);
        fops_human(a->bytes, sz, sizeof(sz));
        snprintf(l1, sizeof(l1), "%d %s, %d %s", a->nshot, kind_word(K_SHOT, a->nshot != 1), a->nvid, kind_word(K_VIDEO, a->nvid != 1));
        snprintf(l2, sizeof(l2), "%s in all", sz);
        fops_join(l3, sizeof(l3), g_root, a->name);
    } else {
        const GItem *it = &g_it[cur_item()];
        char p[FOPS_PATH];
        draw_box(h, it->name);
        fops_human(it->size, sz, sizeof(sz));
        fmt_when(it->when, when, sizeof(when));
        item_path(cur_item(), p, sizeof(p));
        snprintf(l1, sizeof(l1), "%s, %s", kind_word(it->kind, 0), when);
        l1[0] = (char)(l1[0] - 32);
        if (it->kind == K_SHOT) snprintf(l2, sizeof(l2), "%s", sz);
        else if (it->dur > 0) { char t[24]; fmt_len(it->dur, t, sizeof(t)); snprintf(l2, sizeof(l2), "%s long, %s", t, sz); }
        else snprintf(l2, sizeof(l2), "%s", sz);
        snprintf(l3, sizeof(l3), "%s", p);
    }
    gfx_text(FONT_SMALL, bx + 16, by + 36, l1, UI_SUB, 255, w - 32);
    gfx_text(FONT_SMALL, bx + 16, by + 52, l2, UI_SUB, 255, w - 32);
    gfx_text(FONT_BADGE, bx + 16, by + 70, l3, UI_MUTED, 255, w - 32);
    gfx_button('o', bx + w - 64, by + h - 22); gfx_text(FONT_SMALL, bx + w - 46, by + h - 23, "Close", UI_TEXT, 255, 0);
}

static void draw_confirm(void)
{
    const int w = 320, h = 86, bx = (UI_SCREEN_W - w) / 2, by = (UI_SCREEN_H - h) / 2 - 6;
    char s[300];
    if (g_view == V_ALBUMS) {
        const GAlbum *a = &g_al[cur_album()];
        int n = g_tab == T_SHOTS ? a->nshot : g_tab == T_VIDEOS ? a->nvid : a->nshot + a->nvid;
        snprintf(s, sizeof(s), "Delete %s %d %s of %s?", n == 1 ? "the" : "all", n,
                 g_tab == T_SHOTS ? kind_word(K_SHOT, n != 1) : g_tab == T_VIDEOS ? kind_word(K_VIDEO, n != 1) : n == 1 ? "capture" : "captures", a->name);
    } else snprintf(s, sizeof(s), "Delete this %s?", kind_word(g_it[cur_item()].kind, 0));
    draw_box(h, s);
    gfx_text(FONT_SMALL, bx + 16, by + 34, "It is gone from this board for good.", UI_SUB, 255, w - 32);
    gfx_button('x', bx + 16, by + 60); gfx_text(FONT_SMALL, bx + 34, by + 59, "Delete", UI_TEXT, 255, 0);
    gfx_button('o', bx + 110, by + 60); gfx_text(FONT_SMALL, bx + 128, by + 59, "Keep", UI_TEXT, 255, 0);
}

static void draw_copy(void)
{
    const int w = 320, h = 92, bx = (UI_SCREEN_W - w) / 2, by = (UI_SCREEN_H - h) / 2 - 6;
    char a[32], t[32], line[96];
    if (g_cphase == 1) {   /* the videos being made ready: how many there are so far */
        DIR *d = opendir(g_stage);
        struct dirent *e;
        int n = 0;
        while (d && (e = readdir(d))) if (e->d_name[0] != '.' && !strstr(e->d_name, ".part.")) n++;
        if (d) closedir(d);
        draw_box(h, "Copying to the PSP");
        gfx_text(FONT_SMALL, bx + 16, by + 32, "Making the videos ready for the PSP's player", UI_SUB, 255, w - 32);
        gfx_nine(NINE_R6, bx + 16, by + 52, w - 32, 10, 0x151a26, 255);
        if (n > 0 && g_nprep) gfx_nine(NINE_R6, bx + 16, by + 52, (w - 32) * (n > g_nprep ? g_nprep : n) / g_nprep, 10, UI_ACCENT, 255);
        snprintf(line, sizeof(line), "%d / %d", n, g_nprep);
    } else {
        const FJob *j = &g_cjob[g_cphase == 0 ? 0 : 1];
        uint64_t done = j->bytes_done, tot = j->bytes_total;
        int pct = tot ? (int)(done * 100 / tot) : 0;
        draw_box(h, "Copying to the PSP");
        gfx_text(FONT_SMALL, bx + 16, by + 32, j->current, UI_SUB, 255, w - 32);
        gfx_nine(NINE_R6, bx + 16, by + 52, w - 32, 10, 0x151a26, 255);
        if (pct > 0) gfx_nine(NINE_R6, bx + 16, by + 52, (w - 32) * (pct > 100 ? 100 : pct) / 100, 10, UI_ACCENT, 255);
        fops_human(done, a, sizeof(a)); fops_human(tot, t, sizeof(t));
        snprintf(line, sizeof(line), "%d / %d   %s / %s", j->files_done, j->files_total, a, t);
    }
    gfx_text(FONT_BADGE, bx + 16, by + 68, line, UI_MUTED, 255, 0);
    gfx_button('o', bx + w - 80, by + 68); gfx_text(FONT_SMALL, bx + w - 62, by + 67, "Cancel", UI_TEXT, 255, 0);
}

static void ga_render(PspExpContext *ctx, uint64_t now)
{
    int x = 12;
    g_dirty = 0; g_pending = 0;
    gfx_frame(ctx);
    pspexp_begin_frame(ctx);
    pspexp_set_clip(ctx, 0, 0, UI_SCREEN_W, UI_SCREEN_H);
    if (g_view == V_PICTURE) draw_picture(now);
    else {
        gfx_backdrop();
        draw_header();
        if (g_view == V_ALBUMS) draw_albums(); else draw_items();
        if (g_msg[0]) gfx_text(FONT_SMALL, x, UI_HINT_Y - 1, g_msg, UI_ACCENT, 255, UI_SCREEN_W - 24);
        else if (!g_menu.open && !g_more && !g_confirm && !copying()) {
            if (g_view == V_ALBUMS ? cur_album() >= 0 : cur_item() >= 0) {
                x = ui_hint_btn(x, 'x', g_view == V_ALBUMS ? "Open" : g_it[cur_item()].kind == K_SHOT ? "Show" : "Play");
                x = ui_hint_btn(x, 's', "Delete");
                x = ui_hint_btn(x, 't', "Details");
            }
            if (g_view == V_ITEMS) x = ui_hint_btn(x, 'o', "Games");
            x = ui_hint_cap(x, "L/R", "Kind", UI_SUB);
            x = ui_hint_cap(x, "START", "Menu", UI_SUB);
        }
    }
    if (g_view == V_PICTURE && g_msg[0] && !g_menu.open) gfx_text(FONT_SMALL, 12, UI_HINT_Y - 1, g_msg, UI_ACCENT, 255, UI_SCREEN_W - 24);
    if (g_more && (g_view == V_ALBUMS ? cur_album() >= 0 : cur_item() >= 0)) draw_details();
    if (g_confirm && (g_view == V_ALBUMS ? cur_album() >= 0 : cur_item() >= 0)) draw_confirm();
    if (copying()) draw_copy();
    if (g_menu.open) ui_menu_draw(&g_menu);
    notify_draw(now);
    pspexp_present(ctx);
}

static int ga_take_exit(void) { int e = g_exit; g_exit = 0; return e; }

static int ga_take_app(char *id, size_t n)
{
    if (!g_app_req[0]) return 0;
    snprintf(id, n, "%s", g_app_req);
    g_app_req[0] = 0;
    return 1;
}

/* closed (HOME): its pictures leave the PSP */
static void ga_leave(void)
{
    gfx_thumb_free();
    if (g_view == V_PICTURE) { gfx_image_free(); g_view = V_ITEMS; }
}

static void ga_state(char *out, size_t n)
{
    static const char *views[] = { "albums", "items", "picture" }, *tabs[] = { "all", "screenshots", "videos" };
    size_t l = strlen(out);
    int a = g_view == V_ALBUMS ? cur_album() : g_album, i = g_view == V_ALBUMS ? -1 : cur_item();
    if (l < n) snprintf(out + l, n - l, "gallery.view=%s\ngallery.tab=%s\ngallery.albums=%d\ngallery.album=%s\ngallery.items=%d\ngallery.item=%s\ngallery.menu=%d\ngallery.confirm=%d\ngallery.copying=%d\ngallery.message=%s\ngallery.thumb_job=%d\n",
                        views[g_view], tabs[g_tab], g_nashown, a >= 0 ? g_al[a].name : "", g_view == V_ALBUMS ? 0 : g_nishown, i >= 0 ? g_it[i].name : "",
                        g_menu.open, g_confirm, g_cphase, g_msg, g_tjob > 0);
}

const AppView GALLERY_APP = { "gallery", ga_open, ga_input, ga_needs_frame, ga_render, ga_take_exit, ga_state, ga_take_app, ga_leave };
