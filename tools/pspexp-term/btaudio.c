/* where the sound goes: see btaudio.h */
#include "btaudio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK_US 3000000u

static pid_t g_pid;
static uint64_t g_last;
static int g_soon;

static void route_path(char *out, size_t n) { snprintf(out, n, "/tmp/pspexp-audio-route-%u", (unsigned)getuid()); }

int bt_available(void)
{
    static int have = -1;
    if (have < 0) have = system("pspexp-bt available >/dev/null 2>&1") == 0;
    return have;
}

void bt_chosen(char *out, size_t n)
{
    char path[512], line[64] = "";
    const char *home = getenv("HOME");
    FILE *f;
    snprintf(path, sizeof(path), "%s/.config/pspexp/audio-out", home ? home : "");
    out[0] = 0;
    if (!(f = fopen(path, "r"))) return;
    if (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strcmp(line, "psp")) snprintf(out, n, "%s", line);
    }
    fclose(f);
}

static void route_read(char *line, size_t n)
{
    char path[64];
    FILE *f;
    route_path(path, sizeof(path));
    line[0] = 0;
    if ((f = fopen(path, "r")) != NULL) { if (!fgets(line, (int)n, f)) line[0] = 0; fclose(f); }
    line[strcspn(line, "\r\n")] = 0;
}

void bt_route_label(char *out, size_t n)
{
    char line[200];
    route_read(line, sizeof(line));
    if (!strncmp(line, "pw ", 3) && strchr(line + 3, ' ')) snprintf(out, n, "%s", strchr(line + 3, ' ') + 1);
    else if (!strncmp(line, "psp - ", 6)) snprintf(out, n, "PSP (%s away)", line + 6);
    else snprintf(out, n, "PSP");
}

void bt_route_soon(void) { g_soon = 1; }

void bt_route_tick(uint64_t now)
{
    char chosen[64], line[200], path[64];
    int st;
    if (g_pid > 0 && waitpid(g_pid, &st, WNOHANG) == g_pid) g_pid = 0;
    if (g_pid > 0 || (!g_soon && now - g_last < CHECK_US)) return;
    g_last = now;
    g_soon = 0;
    bt_chosen(chosen, sizeof(chosen));
    route_read(line, sizeof(line));
    route_path(path, sizeof(path));
    if (!chosen[0] && strncmp(line, "pw ", 3)) {   /* the PSP, and the file says so (or is not there): nothing to check */
        if (line[0] && strcmp(line, "psp")) { FILE *f = fopen(path, "w"); if (f) { fputs("psp\n", f); fclose(f); } }
        return;
    }
    if (!bt_available()) return;
    g_pid = fork();
    if (g_pid == 0) {   /* written whole, then renamed: a program never reads half a line */
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "pspexp-bt route > %s.new 2>/dev/null && mv -f %s.new %s", path, path, path);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (g_pid < 0) g_pid = 0;
}
