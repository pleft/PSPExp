/*
 * Sound played on the board instead of the PSP: when the console has routed
 * it to a Bluetooth device (pspexp-bt, docs/Launcher_and_Boards.md §11), the
 * stream every program sends with pspexp_audio_stream goes to a pw-play on
 * the board, and the link carries none.
 *
 * The route is a file the console keeps (/tmp/pspexp-audio-route-UID: "pw
 * NODE NAME" or "psp ..."); a program looks at it at most twice a second, so the
 * sound follows the earbuds going into their case or coming out within one
 * check. pw-play reads from a socket, not a pipe: written with MSG_NOSIGNAL,
 * a pw-play that died is an error here, never a SIGPIPE that ends the game.
 */
#include "pspexp_local_audio.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   /* macOS: no route file there anyway */
#endif

extern char **environ;

void pspexp_la_init(PspExpLocalAudio *la)
{
    memset(la, 0, sizeof(*la));
    la->fd = -1;
    la->pid = -1;
    la->vol_pid = -1;
    la->vol_fd = -1;
    la->vol_now = -1;
}

void pspexp_la_close(PspExpLocalAudio *la)
{
    if (la->fd >= 0) close(la->fd);
    if (la->pid > 0) { kill(la->pid, SIGTERM); waitpid(la->pid, NULL, 0); }
    la->fd = -1;
    la->pid = -1;
}

/* a processor under 1.5 GHz (the GameShell's 1.2 GHz Cortex-A7): 20 ms made
 * pw-play starve there now and then (2026-10-03), so it gets 60 */
static int slow_board(void)
{
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
    long khz = 0;
    if (f) { if (fscanf(f, "%ld", &khz) != 1) khz = 0; fclose(f); }
    return khz > 0 && khz < 1500000;
}

static int la_open(PspExpLocalAudio *la, uint32_t rate)
{
    int sv[2];
    char r[16], uid_dir[64], lat[32];
    /* --latency: pw-play's default 100 ms also set the Bluetooth sink's cycle
     * (2048 samples at 48 kHz); 20 ms asks for 512 (measured with pw-top,
     * 2026-10-03), the delay games noticed; PSPEXP_BT_LATENCY to try others */
    char *argv[] = { "pw-play", "--raw", "--format", "s16", "--rate", r, "--channels", "2", "--latency", lat, "--target", la->node, "-", NULL };
    posix_spawn_file_actions_t fa;
    pid_t pid;
    snprintf(r, sizeof(r), "%u", (unsigned)rate);
    snprintf(lat, sizeof(lat), "%s", getenv("PSPEXP_BT_LATENCY") ? getenv("PSPEXP_BT_LATENCY") : slow_board() ? "60ms" : "20ms");
    if (!getenv("XDG_RUNTIME_DIR")) {   /* a system service has none: PipeWire's socket is the user's */
        snprintf(uid_dir, sizeof(uid_dir), "/run/user/%u", (unsigned)getuid());
        setenv("XDG_RUNTIME_DIR", uid_dir, 0);
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return -1;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 0);
    posix_spawn_file_actions_addclose(&fa, sv[0]);
    posix_spawn_file_actions_addclose(&fa, sv[1]);
    if (posix_spawnp(&pid, "pw-play", &fa, NULL, argv, environ) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        close(sv[0]); close(sv[1]);
        return -1;
    }
    posix_spawn_file_actions_destroy(&fa);
    close(sv[1]);
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    la->fd = sv[0];
    la->pid = pid;
    la->rate = rate;
    return 0;
}

/* the route file, read when it changed: 1 to the board's output (node set), 0 to the PSP */
static int la_route(PspExpLocalAudio *la)
{
    char path[64], line[200];
    struct stat st;
    FILE *f;
    snprintf(path, sizeof(path), "/tmp/pspexp-audio-route-%u", (unsigned)getuid());
    if (stat(path, &st) < 0) { la->want = 0; la->mtime = 0; return 0; }
    if (st.st_mtime == la->mtime && st.st_size == la->size) return la->want;
    la->mtime = st.st_mtime; la->size = st.st_size;
    la->want = 0;
    if ((f = fopen(path, "r")) != NULL) {
        if (fgets(line, sizeof(line), f) && !strncmp(line, "pw ", 3)) {
            char node[sizeof(la->node)];
            snprintf(node, sizeof(node), "%s", line + 3);
            node[strcspn(node, " \r\n")] = 0;   /* "pw NODE NAME" */
            if (node[0]) {
                if (strcmp(node, la->node)) { pspexp_la_close(la); snprintf(la->node, sizeof(la->node), "%s", node); }
                la->want = 1;
            }
        }
        fclose(f);
    }
    return la->want;
}

int pspexp_la_active(PspExpLocalAudio *la, uint32_t rate, uint64_t now_us)
{
    if (!rate) return 0;
    if (!la->checked_us || now_us - la->checked_us >= 500000u) {
        la->checked_us = now_us;
        if (!la_route(la)) { if (la->fd >= 0) pspexp_la_close(la); return 0; }
        if (la->fd >= 0 && la->pid > 0 && waitpid(la->pid, NULL, WNOHANG) == la->pid) { la->pid = -1; pspexp_la_close(la); }   /* it ended */
        if (la->fd < 0 && now_us - la->failed_us >= 3000000u && la_open(la, rate) < 0) la->failed_us = now_us;
    }
    return la->want && la->fd >= 0;
}

int pspexp_la_write(PspExpLocalAudio *la, const int16_t *stereo, uint32_t frames)
{
    const char *p = (const char *)stereo;
    size_t left = (size_t)frames * 4u;
    while (left) {
        ssize_t n = send(la->fd, p, left, MSG_NOSIGNAL);
        if (n > 0) { p += n; left -= (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {   /* full: the rest is dropped, as the PSP does */
            static uint32_t told;
            la->dropped += (uint32_t)(left / 4u);
            if (la->dropped - told >= 4410u) { told = la->dropped; fprintf(stderr, "[pspexp] Bluetooth sound: %u frames dropped so far (pw-play's input full)\n", (unsigned)la->dropped); }
            return 0;
        }
        if (n < 0 && errno == EINTR) continue;
        pspexp_la_close(la);   /* pw-play went away: opened again at the next check */
        return -1;
    }
    return 0;
}

#define VOL_UP   0x100000u   /* PSPEXP_BUTTON_VOLUP / VOLDOWN */
#define VOL_DOWN 0x200000u
#define VOL_STEP 5           /* percent a press */

static void vol_spawn(PspExpLocalAudio *la)
{
    char d[16];
    char *argv[] = { "pspexp-bt", "volume", d, NULL };
    posix_spawn_file_actions_t fa;
    int pf[2];
    snprintf(d, sizeof(d), "%+d", la->vol_pending);
    if (pipe(pf) < 0) return;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pf[1], 1);   /* it prints the new volume */
    posix_spawn_file_actions_addclose(&fa, pf[0]);
    posix_spawn_file_actions_addclose(&fa, pf[1]);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (posix_spawnp(&la->vol_pid, "pspexp-bt", &fa, NULL, argv, environ) != 0) { la->vol_pid = -1; close(pf[0]); }
    else { la->vol_fd = pf[0]; fcntl(la->vol_fd, F_SETFD, FD_CLOEXEC); }
    close(pf[1]);
    posix_spawn_file_actions_destroy(&fa);
    la->vol_pending = 0;
}

void pspexp_la_buttons(PspExpLocalAudio *la, uint32_t buttons, uint64_t now_us)
{
    uint32_t vol = buttons & (VOL_UP | VOL_DOWN), pressed = vol & ~la->vol_buttons;
    la->vol_buttons = vol;
    if (la->vol_pid > 0 && waitpid(la->vol_pid, NULL, WNOHANG) == la->vol_pid) {   /* done: its answer, for the bar */
        char b[16];
        ssize_t n = la->vol_fd >= 0 ? read(la->vol_fd, b, sizeof(b) - 1) : -1;
        if (n > 0) { b[n] = 0; if (b[0] >= '0' && b[0] <= '9') { la->vol_now = atoi(b); la->vol_report = 1; } }
        if (la->vol_fd >= 0) close(la->vol_fd);
        la->vol_fd = -1;
        la->vol_pid = -1;
    }
    if (pressed) la_route(la);   /* (a program that plays nothing has not looked at the route) */
    if (!la->want) { la->vol_pending = 0; return; }   /* the PSP's own sound: its own volume */
    if (pressed) la->vol_next_us = now_us + 400000u;   /* held: again after 0.4 s, then every 0.15 s */
    else if (vol && now_us >= la->vol_next_us) { la->vol_next_us = now_us + 150000u; pressed = vol; }
    if (pressed & VOL_UP) la->vol_pending += VOL_STEP;
    if (pressed & VOL_DOWN) la->vol_pending -= VOL_STEP;
    if (la->vol_pending && la->vol_pid <= 0) vol_spawn(la);   /* one at a time: the presses meanwhile add up */
}

void pspexp_la_reset(PspExpLocalAudio *la)
{
    /* what pw-play holds cannot be dropped: a new one, at once */
    if (la->fd < 0) return;
    pspexp_la_close(la);
    if (la_open(la, la->rate) < 0) la->failed_us = la->checked_us;
}
