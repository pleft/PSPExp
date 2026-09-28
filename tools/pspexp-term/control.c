/* pspexp-term's test-control port: see control.h */
#include "control.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int g_listen = -1, g_client = -1, g_busy;
static char g_line[4096], g_path[108];
static size_t g_len;

const char *ctl_path(void)
{
    static char p[108];
    const char *e = getenv("PSPEXP_TERM_CONTROL");
    if (e && *e) snprintf(p, sizeof(p), "%s", e);
    else snprintf(p, sizeof(p), "/tmp/pspexp-term-%u.ctl", (unsigned)getuid());
    return p;
}

int ctl_open(const char *path)
{
    struct sockaddr_un a;
    int fd;
    if (strlen(path) >= sizeof(a.sun_path)) { errno = ENAMETOOLONG; return -1; }   /* a socket's path is short (104-108) */
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    unlink(path);   /* a console that ended without cleaning up */
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 2) < 0) { close(fd); return -1; }
    chmod(path, 0600);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);   /* not inherited by the games the launcher starts */
    g_listen = fd;
    snprintf(g_path, sizeof(g_path), "%s", path);
    return 0;
}

void ctl_close(void)
{
    if (g_client >= 0) close(g_client);
    if (g_listen >= 0) { close(g_listen); unlink(g_path); }
    g_client = g_listen = -1;
}

const char *ctl_take(void)
{
    if (g_listen < 0 || g_busy) return NULL;
    if (g_client < 0) {
        g_client = accept(g_listen, NULL, NULL);
        if (g_client < 0) return NULL;
        fcntl(g_client, F_SETFL, O_NONBLOCK);
        fcntl(g_client, F_SETFD, FD_CLOEXEC);
        g_len = 0;
    }
    for (;;) {
        char *nl;
        ssize_t r;
        if ((nl = memchr(g_line, '\n', g_len))) {
            static char cmd[4096];
            size_t l = (size_t)(nl - g_line);
            memcpy(cmd, g_line, l); cmd[l] = 0;
            if (l && cmd[l - 1] == '\r') cmd[l - 1] = 0;
            memmove(g_line, nl + 1, g_len - l - 1); g_len -= l + 1;
            g_busy = 1;
            return cmd;
        }
        if (g_len >= sizeof(g_line) - 1) g_len = 0;   /* a runaway line: dropped */
        r = read(g_client, g_line + g_len, sizeof(g_line) - 1 - g_len);
        if (r > 0) { g_len += (size_t)r; continue; }
        if (r == 0 || (errno != EAGAIN && errno != EINTR)) { close(g_client); g_client = -1; }
        return NULL;
    }
}

static void write_all(int fd, const char *s, size_t n)
{
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR) { struct pollfd p = { fd, POLLOUT, 0 }; poll(&p, 1, 100); continue; }
            return;
        }
        s += w; n -= (size_t)w;
    }
}

void ctl_reply(const char *text)
{
    g_busy = 0;
    if (g_client < 0) return;
    write_all(g_client, text, strlen(text));
    if (text[0] && text[strlen(text) - 1] != '\n') write_all(g_client, "\n", 1);
    write_all(g_client, "END\n", 4);
}

int ctl_client(const char *path, const char *cmd, int timeout_s)
{
    struct sockaddr_un a;
    char buf[8192], acc[65536];
    size_t n = 0;
    time_t end = time(NULL) + (timeout_s > 0 ? timeout_s : 60);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 2;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { fprintf(stderr, "no console answers on %s\n", path); close(fd); return 2; }
    write_all(fd, cmd, strlen(cmd));
    write_all(fd, "\n", 1);
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        ssize_t r;
        char *end_mark;
        if (time(NULL) > end) { fprintf(stderr, "no reply in time\n"); close(fd); return 2; }
        if (poll(&p, 1, 500) <= 0) continue;
        r = read(fd, buf, sizeof(buf));
        if (r <= 0) { fprintf(stderr, "the console closed the connection\n"); close(fd); return 2; }
        if (n + (size_t)r >= sizeof(acc)) r = (ssize_t)(sizeof(acc) - 1 - n);
        memcpy(acc + n, buf, (size_t)r); n += (size_t)r; acc[n] = 0;
        if ((end_mark = strstr(acc, "END\n")) && (end_mark == acc || end_mark[-1] == '\n')) {
            *end_mark = 0;
            fputs(acc, stdout);
            close(fd);
            return strncmp(acc, "ok", 2) ? 1 : 0;
        }
    }
}
