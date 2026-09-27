#include "pspexp_tcp.h"
#include "../libpspexp/pspexp_time.h"
#include "../../protocol/pspexp_usb_link.h"   /* sync packet */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

typedef struct Tcp {
    PspExpTransport t;
    int fd;
    int dead;
    int verbose;
    /* what arrived while a send was stuck: the peer may itself be blocked
     * sending to us, and would stop reading (a deadlock) if we did not take it */
    uint8_t *rx;
    size_t rx_len, rx_off, rx_cap;
} Tcp;

static const uint8_t SYNC[PSPEXP_USB_SYNC_LEN] = PSPEXP_USB_SYNC_BYTES;

void pspexp_tcp_options_default(PspExpTcpOptions *o)
{
    memset(o, 0, sizeof(*o));
    o->host = "127.0.0.1";
    o->port = PSPEXP_TCP_PORT;
    o->connect_timeout_ms = 2000;
}

/* takes what the peer has sent into c->rx; -1 when the link is gone */
static int take_incoming(Tcp *c)
{
    ssize_t n;
    if (c->rx_off && c->rx_off == c->rx_len) c->rx_off = c->rx_len = 0;
    if (c->rx_cap - c->rx_len < 16384) {
        size_t ncap = c->rx_cap ? c->rx_cap * 2 : 65536;
        uint8_t *nb = (uint8_t *)realloc(c->rx, ncap);
        if (!nb) return -1;
        c->rx = nb; c->rx_cap = ncap;
    }
    n = recv(c->fd, c->rx + c->rx_len, c->rx_cap - c->rx_len, 0);
    if (n == 0) return -1;
    if (n < 0) return (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    c->rx_len += (size_t)n;
    return 0;
}

static int send_all(Tcp *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t stuck_since = 0;
    while (len > 0) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(c->fd, p, len, MSG_NOSIGNAL);
#else
        ssize_t n = send(c->fd, p, len, 0);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* the peer is not taking data: read what it sends meanwhile
                 * (it may be waiting for us to), give up after 5 s of neither */
                fd_set w, r; struct timeval tv = { 0, 100000 };
                int k;
                uint64_t now = pspexp_time_us();
                if (!stuck_since) stuck_since = now;
                else if (now - stuck_since > 5000000u) { c->dead = 1; return -1; }
                FD_ZERO(&w); FD_SET(c->fd, &w); FD_ZERO(&r); FD_SET(c->fd, &r);
                k = select(c->fd + 1, &r, &w, NULL, &tv);
                if (k < 0 && errno != EINTR) { c->dead = 1; return -1; }
                if (k > 0 && FD_ISSET(c->fd, &r)) {
                    if (take_incoming(c) < 0) { c->dead = 1; return -1; }
                    stuck_since = 0;
                }
                continue;
            }
            c->dead = 1;
            return -1;
        }
        p += n; len -= (size_t)n;
        stuck_since = 0;
    }
    return 0;
}

static int tr_send(PspExpTransport *t, const void *data, size_t len)
{
    Tcp *c = (Tcp *)t->user;
    if (c->dead) return -1;
    return send_all(c, data, len);
}

static long tr_recv(PspExpTransport *t, void *buf, size_t cap, int timeout_ms)
{
    Tcp *c = (Tcp *)t->user;
    fd_set r;
    struct timeval tv;
    ssize_t n;
    if (c->rx_off < c->rx_len) {   /* first what came in during a send */
        size_t k = c->rx_len - c->rx_off;
        if (k > cap) k = cap;
        memcpy(buf, c->rx + c->rx_off, k);
        c->rx_off += k;
        return (long)k;
    }
    if (c->dead) return -1;
    FD_ZERO(&r); FD_SET(c->fd, &r);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(c->fd + 1, &r, NULL, NULL, &tv) <= 0) return 0;
    n = recv(c->fd, buf, cap, 0);
    if (n == 0) { c->dead = 1; return -1; }
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        c->dead = 1;
        return -1;
    }
    return (long)n;
}

static void tr_close(PspExpTransport *t)
{
    Tcp *c = (Tcp *)t->user;
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c->rx);
    free(c);   /* `t` lives inside `c` */
}

PspExpTransport *pspexp_tcp_open(const PspExpTcpOptions *o, char *err, size_t errlen)
{
    PspExpTcpOptions opt;
    Tcp *c;
    struct addrinfo hints, *res = NULL, *ai;
    char port[16];
    int fd = -1, one = 1, rc;
    uint8_t reply[64];
    uint64_t deadline;

    if (o) opt = *o; else pspexp_tcp_options_default(&opt);
    if (!opt.host) opt.host = "127.0.0.1";
    if (!opt.port) opt.port = PSPEXP_TCP_PORT;
    snprintf(port, sizeof(port), "%u", opt.port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(opt.host, port, &hints, &res);
    if (rc != 0) { snprintf(err, errlen, "resolve %s: %s", opt.host, gai_strerror(rc)); return NULL; }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        /* not inherited by programs this process starts: a child holding a
         * copy keeps the connection alive after we close ours, and the
         * runtime (one client at a time) then turns the next client away */
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        {
            struct timeval tv = { opt.connect_timeout_ms / 1000, (opt.connect_timeout_ms % 1000) * 1000 };
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) { snprintf(err, errlen, "connect %s:%u: %s", opt.host, opt.port, strerror(errno)); return NULL; }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    {   /* connected: a send that cannot go on returns soon, to read meanwhile (send_all) */
        struct timeval tv = { 0, 100000 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));   /* EPIPE instead of a signal */
#endif

    c = (Tcp *)calloc(1, sizeof(*c));
    if (!c) { close(fd); snprintf(err, errlen, "out of memory"); return NULL; }
    c->fd = fd;
    c->verbose = opt.verbose;
    c->t.name = "tcp";
    c->t.user = c;
    c->t.send = tr_send;
    c->t.recv = tr_recv;
    c->t.close = tr_close;

    /* Same handshake as USB: sync packet, expect it echoed. */
    if (send_all(c, SYNC, PSPEXP_USB_SYNC_LEN) < 0) { snprintf(err, errlen, "sync send failed"); tr_close(&c->t); return NULL; }
    deadline = pspexp_time_us() + 3000000u;
    while (pspexp_time_us() < deadline) {
        long n = tr_recv(&c->t, reply, sizeof(reply), 200);
        int i;
        if (n < 0) { snprintf(err, errlen, "connection closed during sync"); tr_close(&c->t); return NULL; }
        for (i = 0; i + PSPEXP_USB_SYNC_LEN <= n; i++)
            if (memcmp(reply + i, SYNC, PSPEXP_USB_SYNC_LEN) == 0) return &c->t;
    }
    snprintf(err, errlen, "no sync reply from %s:%u", opt.host, opt.port);
    tr_close(&c->t);
    return NULL;
}

PspExpTransport *pspexp_tcp_wait(const PspExpTcpOptions *o, int timeout_ms, char *err, size_t errlen)
{
    uint64_t deadline = timeout_ms < 0 ? ~(uint64_t)0 : pspexp_time_us() + (uint64_t)timeout_ms * 1000u;
    for (;;) {
        PspExpTransport *t = pspexp_tcp_open(o, err, errlen);
        if (t) return t;
        if (pspexp_time_us() >= deadline) return NULL;
        pspexp_sleep_us(500000);
    }
}
