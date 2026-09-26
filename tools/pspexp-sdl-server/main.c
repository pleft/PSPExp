/*
 * pspexp-sdl-server: the SDL executor behind a TCP port, standing in for a
 * PSP on Wi-Fi. A board's console or game connects to it exactly as to the
 * PSP runtime (--tcp HOST:3838, or PSPEXP=tcp:HOST:3838); the frames appear
 * in a window, and screenshot requests (pspexp-term --shot N) are saved as
 * pspexp-shot-N.bmp in the current directory: exact pictures of what a PSP
 * would show, from any board, with no emulator and no screen capture.
 *
 *   pspexp-sdl-server [--port 3838] [--scale 2]
 *   SDL_VIDEODRIVER=dummy pspexp-sdl-server      (headless: screenshots only)
 *
 * One client at a time, like the runtime; each gets a fresh executor.
 */
#include "../../host/executor-sdl/pspexp_sdl.h"
#include "../../protocol/pspexp_usb_link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static const uint8_t SYNC[PSPEXP_USB_SYNC_LEN] = PSPEXP_USB_SYNC_BYTES;

static int send_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n) {
        ssize_t w = send(fd, b, n, 0);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        b += w; n -= (size_t)w;
    }
    return 0;
}

/* waits for the client's sync packet and echoes it; returns bytes that
 * followed it in *extra (the first commands), -1 on failure */
static long handshake(int fd, uint8_t *extra, size_t cap)
{
    uint8_t buf[4096];
    size_t have = 0;
    for (;;) {
        fd_set r; struct timeval tv = { 10, 0 }; ssize_t n; size_t i;
        FD_ZERO(&r); FD_SET(fd, &r);
        if (select(fd + 1, &r, NULL, NULL, &tv) <= 0) return -1;
        n = recv(fd, buf + have, sizeof(buf) - have, 0);
        if (n <= 0) return -1;
        have += (size_t)n;
        for (i = 0; i + PSPEXP_USB_SYNC_LEN <= have; i++)
            if (!memcmp(buf + i, SYNC, PSPEXP_USB_SYNC_LEN)) {
                size_t rest = have - i - PSPEXP_USB_SYNC_LEN;
                if (send_all(fd, SYNC, PSPEXP_USB_SYNC_LEN) < 0) return -1;
                if (rest > cap) rest = cap;
                memcpy(extra, buf + i + PSPEXP_USB_SYNC_LEN, rest);
                return (long)rest;
            }
        if (have == sizeof(buf)) return -1;
    }
}

int main(int argc, char **argv)
{
    int port = 3838, scale = 2, i, one = 1, ls;
    struct sockaddr_in a;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else { fprintf(stderr, "usage: pspexp-sdl-server [--port N] [--scale N]\n"); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 1) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "[sdl-server] listening on port %d\n", port);
    for (;;) {
        struct sockaddr_in ca; socklen_t cl = sizeof(ca);
        uint8_t buf[65536];
        long n;
        PspExpSdlOptions o;
        PspExpSdlExecutor *x;
        PspExpTransport *t;
        char err[200];
        int fd = accept(ls, (struct sockaddr *)&ca, &cl);
        if (fd < 0) continue;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        fprintf(stderr, "[sdl-server] client %s\n", inet_ntoa(ca.sin_addr));
        n = handshake(fd, buf, sizeof(buf));
        if (n < 0) { fprintf(stderr, "[sdl-server] no sync\n"); close(fd); continue; }
        pspexp_sdl_options_default(&o);
        o.scale = scale; o.title = "PSPExp (SDL server)";
        x = pspexp_sdl_create(&o, err, sizeof(err));
        if (!x) { fprintf(stderr, "[sdl-server] %s\n", err); close(fd); return 1; }
        t = pspexp_sdl_transport(x);
        if (n > 0 && t->send(t, buf, (size_t)n) < 0) goto done;
        for (;;) {
            fd_set r; struct timeval tv = { 0, 2000 };
            FD_ZERO(&r); FD_SET(fd, &r);
            if (select(fd + 1, &r, NULL, NULL, &tv) > 0) {
                ssize_t k = recv(fd, buf, sizeof(buf), 0);
                if (k <= 0) break;
                if (t->send(t, buf, (size_t)k) < 0) break;
            }
            while ((n = t->recv(t, buf, sizeof(buf), 0)) > 0)
                if (send_all(fd, buf, (size_t)n) < 0) goto done;
            if (n < 0 || pspexp_sdl_quit_requested(x)) break;
        }
    done:
        fprintf(stderr, "[sdl-server] client gone\n");
        close(fd);
        pspexp_sdl_destroy(x);
    }
}
