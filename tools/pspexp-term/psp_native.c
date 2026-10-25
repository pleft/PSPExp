/*
 * The PSP shelf's games, started on the PSP (psp_native.h).
 */
#include "psp_native.h"
#include "notify.h"
#include "psp_plugin.h"
#include "../../host/libpspexp/pspexp_disc.h"
#include "../../host/libpspexp/pspexp_capture.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define KEY "psp-native"
#define VDISC "pxd0:/disc.iso"   /* (the plugin's device: any name on it is the receiver's disc) */

static pid_t g_pid;               /* the unpacking, in a child */
static char g_img[1024], g_tmp[1100], g_title[128], g_job[160];
static uint64_t g_size, g_shown;

static void fail(const char *title, const char *detail) { notify(NOTE_FAIL, "gamepad", title, detail, NULL, KEY); }

/* the game's name, from its file: "Ace Combat X - Skies of Deception (Europe).zip" -> before " (" */
static void title_of(const char *path, char *out, size_t n)
{
    const char *b = strrchr(path, '/'), *p;
    b = b ? b + 1 : path;
    p = strstr(b, " (");
    if (!p) p = strrchr(b, '.');
    snprintf(out, n, "%.*s", p ? (int)(p - b) : (int)strlen(b), b);
}

/* the PSPExp plugin on in ms0:/seplugins/game.txt: the game's disc comes through it (without it the
 * PSP would wait for its disc for ever) */
static int plugin_on(PspExpContext *ctx)
{
    static char t[4097];
    int n = pspexp_fs_read(ctx, PSP_PLUGIN_GAME_TXT, 0, t, sizeof(t) - 1);
    if (n <= 0) return 0;
    t[n] = 0;
    return psp_plugin_listed(t);
}

static void cache_dir(char *out, size_t n)
{
    const char *h = getenv("HOME");
    snprintf(out, n, "%s/.cache/pspexp/discs", h && *h ? h : "/tmp");
}

/* room for size bytes: the other unpacked games gone, oldest first, until there is (one kept
 * at least when there is room: the game played last) */
static int make_room(const char *dir, uint64_t size)
{
    for (;;) {
        struct statvfs v;
        DIR *d;
        struct dirent *e;
        char oldest[1100] = "";
        time_t ot = 0;
        if (statvfs(dir, &v) == 0 && (uint64_t)v.f_bavail * v.f_frsize > size + (256ull << 20)) return 0;
        if (!(d = opendir(dir))) return -1;
        while ((e = readdir(d))) {
            char p[1100];
            struct stat st;
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
            if (strcmp(p, g_img) && stat(p, &st) == 0 && (!oldest[0] || st.st_mtime < ot)) { snprintf(oldest, sizeof(oldest), "%s", p); ot = st.st_mtime; }
        }
        closedir(d);
        if (!oldest[0] || unlink(oldest) != 0) return -1;
    }
}

static void boot(PspExpContext *ctx)
{
    char f[600];
    FILE *fp;
    pspexp_capture_disc_file(f, sizeof(f));
    if (!(fp = fopen(f, "w"))) { fail("The game could not be started", "The board could not name its disc for the PSP"); return; }
    fprintf(fp, "%s\n", g_img);
    fclose(fp);
    if (pspexp_boot_disc(ctx, VDISC) < 0) { fail("The game could not be started", "The PSP is not connected"); return; }
    notify(NOTE_DONE, "gamepad", g_title, "Starting on the PSP: PSPExp comes back when you exit the game", NULL, KEY);
}

void native_start(PspExpContext *ctx, const char *disc, uint64_t now)
{
    PspExpDisc *d;
    char err[200], dir[700];
    struct stat st;
    (void)now;
    if (g_pid > 0) { notify_hint("gamepad", "A game is being prepared"); return; }
    title_of(disc, g_title, sizeof(g_title));
    if (!pspexp_boot_disc_supported(ctx)) { fail("PSP games need PSPExp on the PSP updated", "Settings > Update the PSP, with PRO custom firmware on the PSP"); return; }
    if (!plugin_on(ctx)) { fail("Switch on the PSPExp plugin first", "Settings > This PSP > PSPExp plugin: PSP games from the board come through it"); return; }
    if (!(d = pspexp_disc_open(disc, err, sizeof(err)))) { fail("Not a PSP game disc", err); return; }
    g_size = pspexp_disc_size(d);
    if (strcmp(pspexp_disc_kind(d), "zip") != 0) {   /* an ISO or CSO: served as it is */
        pspexp_disc_close(d);
        snprintf(g_img, sizeof(g_img), "%s", disc);
        boot(ctx);
        return;
    }
    pspexp_disc_close(d);
    cache_dir(dir, sizeof(dir));
    {   /* ~/.cache/pspexp/discs: every level */
        char p[700], *q;
        snprintf(p, sizeof(p), "%s", dir);
        for (q = p + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(p, 0755); *q = '/'; }
        mkdir(p, 0755);
    }
    {
        const char *b = strrchr(disc, '/');
        char stem[400];
        b = b ? b + 1 : disc;
        snprintf(stem, sizeof(stem), "%.*s", (int)(strlen(b) > 4 ? strlen(b) - 4 : strlen(b)), b);
        snprintf(g_img, sizeof(g_img), "%s/%s.iso", dir, stem);
    }
    if (stat(g_img, &st) == 0 && (uint64_t)st.st_size == g_size) {   /* unpacked before */
        utimes(g_img, NULL);   /* (the newest: kept) */
        boot(ctx);
        return;
    }
    if (make_room(dir, g_size) < 0) { fail("No room on the board for the game", "It needs its unpacked size free; the cache of unpacked games is ~/.cache/pspexp/discs"); return; }
    snprintf(g_tmp, sizeof(g_tmp), "%s.part", g_img);
    g_pid = fork();
    if (g_pid == 0) {   /* the zip's ISO, front to back, into the cache */
        static uint8_t buf[64 * PSPEXP_DISC_SECTOR];
        PspExpDisc *z = pspexp_disc_open(disc, NULL, 0);
        FILE *o = fopen(g_tmp, "wb");
        uint32_t lba = 0, total = (uint32_t)(g_size / PSPEXP_DISC_SECTOR);
        int ok = z && o;
        while (ok && lba < total) {
            uint32_t n = total - lba < 64 ? total - lba : 64;
            ok = pspexp_disc_read(z, lba, n, buf) == 0 && fwrite(buf, PSPEXP_DISC_SECTOR, n, o) == n;
            lba += n;
        }
        if (o) ok = fclose(o) == 0 && ok;
        pspexp_disc_close(z);
        if (ok && rename(g_tmp, g_img) == 0) _exit(0);
        unlink(g_tmp);
        _exit(1);
    }
    if (g_pid < 0) { g_pid = 0; fail("The game could not be prepared", strerror(errno)); return; }
    g_shown = 0;
    snprintf(g_job, sizeof(g_job), "Preparing %s", g_title);
    notify_job(KEY, "gamepad", g_job, 0);
}

void native_poll(PspExpContext *ctx, uint64_t now)
{
    int st;
    if (g_pid <= 0) return;
    if (waitpid(g_pid, &st, WNOHANG) == g_pid) {
        g_pid = 0;
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) boot(ctx);
        else fail("The game could not be prepared", "Unpacking it failed (the board's disk full, or the zip damaged)");
        return;
    }
    if (now - g_shown >= 500000u) {   /* the unpacking's progress, from the file's size */
        struct stat s;
        g_shown = now;
        if (stat(g_tmp, &s) == 0 && g_size) notify_job(KEY, "gamepad", g_job, (int)((uint64_t)s.st_size * 100 / g_size));
    }
}
