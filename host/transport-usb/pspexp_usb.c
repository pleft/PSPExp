#include "pspexp_usb.h"
#include "../libpspexp/pspexp_time.h"
#include "../../protocol/pspexp_usb_link.h"
#include <libusb.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/time.h>

typedef struct Chunk {
    struct Chunk *next;
    size_t len, off;
    uint8_t data[1];
} Chunk;

/* One libusb context for the whole process: creating and destroying
 * contexts while a device comes and goes is where macOS reports the process
 * killed (a guarded descriptor closed twice inside libusb). */
static libusb_context *g_ctx;

static void shared_context_exit(void)
{
    /* Runs before library destructors (atexit order), so libusb's hotplug
     * thread is gone when sdl2-compat and friends tear down. Leaving it
     * alive made macOS kill the process at exit. */
    if (g_ctx) { libusb_exit(g_ctx); g_ctx = NULL; }
}

static libusb_context *shared_context(char *err, size_t errlen)
{
    if (!g_ctx) {
        int r = libusb_init(&g_ctx);
        if (r != 0) { snprintf(err, errlen, "libusb_init: %s", libusb_error_name(r)); g_ctx = NULL; }
        else atexit(shared_context_exit);
    }
    return g_ctx;
}

/* PSPEXP_USB_DEPTH=N (default 2, at most 8): N transfers queued each way
 * (libusb's async API), so the PSP never waits for this side to submit the next
 * one: small transfers from the PSP ~25% faster (4 KB: 12.5 -> 15.7 MB/s); 4
 * gains nothing over 2. 1: one blocking transfer at a time, as before. */
#define MAX_DEPTH 8

typedef struct Usb Usb;
typedef struct Slot {
    Usb *u;
    struct libusb_transfer *x;
    uint8_t *buf;
    volatile int busy;
} Slot;

struct Usb {
    PspExpTransport t;
    libusb_context *ctx;   /* the shared context, not owned */
    libusb_device_handle *h;
    int packet;
    int timeout_ms;
    int verbose;
    volatile int dead;
    volatile int stop;
    pthread_t reader;
    int reader_running;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Chunk *qhead, *qtail;
    size_t queued;
    PspExpUsbStats st;
    int depth;                    /* transfers queued each way (1: blocking) */
    Slot out[MAX_DEPTH], in[MAX_DEPTH];
    int out_next;
    volatile int in_live;         /* IN transfers still submitted */
};

static const uint8_t SYNC[PSPEXP_USB_SYNC_LEN] = PSPEXP_USB_SYNC_BYTES;

void pspexp_usb_options_default(PspExpUsbOptions *o)
{
    memset(o, 0, sizeof(*o));
    o->vid = PSPEXP_USB_VID;
    o->pid = PSPEXP_USB_PID;
    o->transfer_timeout_ms = 3000;
}

static int bulk_out(Usb *u, const uint8_t *data, int len)
{
    while (len > 0) {
        int done = 0;
        int r = libusb_bulk_transfer(u->h, PSPEXP_USB_EP_OUT, (unsigned char *)data, len, &done, (unsigned)u->timeout_ms);
        if (r == 0 || (r == LIBUSB_ERROR_TIMEOUT && done > 0)) {
            data += done; len -= done;
            u->st.bytes_sent += (uint64_t)done;
            u->st.transfers_out++;
            if (r == LIBUSB_ERROR_TIMEOUT) continue;
        } else {
            if (u->verbose) fprintf(stderr, "usb: bulk out failed: %s\n", libusb_error_name(r));
            u->dead = 1;
            return -1;
        }
    }
    return 0;
}

static void mark_dead(Usb *u)
{
    u->dead = 1;
    pthread_mutex_lock(&u->mu);
    pthread_cond_broadcast(&u->cv);
    pthread_mutex_unlock(&u->mu);
}

static void LIBUSB_CALL out_done(struct libusb_transfer *x)
{
    Slot *s = (Slot *)x->user_data;
    Usb *u = s->u;
    if (x->status == LIBUSB_TRANSFER_COMPLETED && x->actual_length == x->length) {
        u->st.bytes_sent += (uint64_t)x->actual_length;
        u->st.transfers_out++;
    } else {
        if (u->verbose) fprintf(stderr, "usb: bulk out failed: status %d\n", x->status);
        u->dead = 1;
    }
    pthread_mutex_lock(&u->mu);
    s->busy = 0;
    pthread_cond_broadcast(&u->cv);
    pthread_mutex_unlock(&u->mu);
}

/* one piece into the next free OUT slot (waits while all are in flight); the
 * reader thread runs libusb's events, so completions arrive there */
static int out_async(Usb *u, const uint8_t *data, int len)
{
    Slot *s = &u->out[u->out_next];
    pthread_mutex_lock(&u->mu);
    while (s->busy && !u->dead) pthread_cond_wait(&u->cv, &u->mu);
    pthread_mutex_unlock(&u->mu);
    if (u->dead) return -1;
    memcpy(s->buf, data, (size_t)len);
    libusb_fill_bulk_transfer(s->x, u->h, PSPEXP_USB_EP_OUT, s->buf, len, out_done, s, (unsigned)u->timeout_ms);
    s->busy = 1;
    if (libusb_submit_transfer(s->x) != 0) { s->busy = 0; u->dead = 1; return -1; }
    u->out_next = (u->out_next + 1) % u->depth;
    return 0;
}

static int send_piece(Usb *u, const uint8_t *data, int len)
{
    return u->depth > 1 ? out_async(u, data, len) : bulk_out(u, data, len);
}

static int tr_send(PspExpTransport *t, const void *data, size_t len)
{
    Usb *u = (Usb *)t->user;
    const uint8_t *p = (const uint8_t *)data;
    if (u->dead) return -1;
    while (len > 0) {
        size_t n = len > PSPEXP_USB_CHUNK ? PSPEXP_USB_CHUNK : len;
        if (n < PSPEXP_USB_CHUNK && (n % (size_t)u->packet) == 0) {
            /* Ends on a packet boundary: split so both transfers end short. */
            u->st.splits++;
            if (send_piece(u, p, (int)(n - PSPEXP_USB_SPLIT)) < 0) return -1;
            if (send_piece(u, p + n - PSPEXP_USB_SPLIT, PSPEXP_USB_SPLIT) < 0) return -1;
        } else {
            if (send_piece(u, p, (int)n) < 0) return -1;
        }
        p += n; len -= n;
    }
    return 0;
}

static void queue_push(Usb *u, const uint8_t *data, size_t len)
{
    Chunk *c = (Chunk *)malloc(sizeof(Chunk) + len);
    if (!c) return;
    c->next = NULL; c->len = len; c->off = 0;
    memcpy(c->data, data, len);
    pthread_mutex_lock(&u->mu);
    if (u->qtail) u->qtail->next = c; else u->qhead = c;
    u->qtail = c;
    u->queued += len;
    pthread_cond_signal(&u->cv);
    pthread_mutex_unlock(&u->mu);
}

static void LIBUSB_CALL in_done(struct libusb_transfer *x)
{
    Slot *s = (Slot *)x->user_data;
    Usb *u = s->u;
    if (x->status == LIBUSB_TRANSFER_COMPLETED) {
        if (x->actual_length > 0) {
            u->st.bytes_received += (uint64_t)x->actual_length;
            u->st.transfers_in++;
            queue_push(u, x->buffer, (size_t)x->actual_length);
        }
        if (!u->stop && !u->dead && libusb_submit_transfer(x) == 0) return;
    } else if (x->status != LIBUSB_TRANSFER_CANCELLED) {
        if (u->verbose) fprintf(stderr, "usb: bulk in failed: status %d\n", x->status);
        mark_dead(u);
    }
    s->busy = 0;
    u->in_live--;
}

/* depth > 1: every IN transfer queued at once, resubmitted as each completes
 * (USB keeps their order); this thread runs libusb's events for both ways */
static void reader_async(Usb *u)
{
    int i;
    for (i = 0; i < u->depth; i++) {
        Slot *s = &u->in[i];
        libusb_fill_bulk_transfer(s->x, u->h, PSPEXP_USB_EP_IN, s->buf, PSPEXP_USB_CHUNK, in_done, s, 0);
        if (libusb_submit_transfer(s->x) == 0) { s->busy = 1; u->in_live++; }
    }
    while (!u->stop && !u->dead) {
        struct timeval tv = { 0, 200000 };
        libusb_handle_events_timeout_completed(u->ctx, &tv, NULL);
    }
    for (i = 0; i < u->depth; i++) if (u->in[i].busy) libusb_cancel_transfer(u->in[i].x);
    for (i = 0; i < u->depth; i++) if (u->out[i].busy) libusb_cancel_transfer(u->out[i].x);
    for (i = 0; i < 50; i++) {   /* the cancellations complete through the event loop */
        int j, busy = u->in_live;
        struct timeval tv = { 0, 20000 };
        for (j = 0; j < u->depth; j++) busy += u->out[j].busy;
        if (!busy) break;
        libusb_handle_events_timeout_completed(u->ctx, &tv, NULL);
    }
}

static void *reader_main(void *arg)
{
    Usb *u = (Usb *)arg;
    uint8_t *buf;
    if (u->depth > 1) { reader_async(u); return NULL; }
    buf = (uint8_t *)malloc(PSPEXP_USB_CHUNK);
    if (!buf) { u->dead = 1; return NULL; }
    while (!u->stop && !u->dead) {
        int got = 0;
        int r = libusb_bulk_transfer(u->h, PSPEXP_USB_EP_IN, buf, PSPEXP_USB_CHUNK, &got, 200);
        if (r == 0 || r == LIBUSB_ERROR_TIMEOUT) {
            if (r == LIBUSB_ERROR_TIMEOUT) u->st.read_timeouts++;
            if (got > 0) {
                u->st.bytes_received += (uint64_t)got;
                u->st.transfers_in++;
                queue_push(u, buf, (size_t)got);
            }
        } else if (r == LIBUSB_ERROR_INTERRUPTED) {
            continue;
        } else {
            if (u->verbose) fprintf(stderr, "usb: bulk in failed: %s\n", libusb_error_name(r));
            u->dead = 1;
            pthread_mutex_lock(&u->mu);
            pthread_cond_broadcast(&u->cv);
            pthread_mutex_unlock(&u->mu);
            break;
        }
    }
    free(buf);
    return NULL;
}

static long tr_recv(PspExpTransport *t, void *buf, size_t cap, int timeout_ms)
{
    Usb *u = (Usb *)t->user;
    size_t copied = 0;
    uint8_t *out = (uint8_t *)buf;

    pthread_mutex_lock(&u->mu);
    if (!u->qhead && timeout_ms > 0 && !u->dead) {
        struct timeval tv;
        struct timespec ts;
        gettimeofday(&tv, NULL);
        ts.tv_sec = tv.tv_sec + timeout_ms / 1000;
        ts.tv_nsec = (long)tv.tv_usec * 1000L + (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (!u->qhead && !u->dead) {
            if (pthread_cond_timedwait(&u->cv, &u->mu, &ts) == ETIMEDOUT) break;
        }
    }
    while (u->qhead && copied < cap) {
        Chunk *c = u->qhead;
        size_t n = c->len - c->off;
        if (n > cap - copied) n = cap - copied;
        memcpy(out + copied, c->data + c->off, n);
        c->off += n; copied += n; u->queued -= n;
        if (c->off == c->len) {
            u->qhead = c->next;
            if (!u->qhead) u->qtail = NULL;
            free(c);
        }
    }
    pthread_mutex_unlock(&u->mu);
    if (copied == 0 && u->dead) return -1;
    return (long)copied;
}

static void tr_close(PspExpTransport *t)
{
    Usb *u = (Usb *)t->user;
    if (!u) return;
    u->stop = 1;
    if (u->reader_running) pthread_join(u->reader, NULL);
    {
        int i;
        for (i = 0; i < MAX_DEPTH; i++) {
            if (u->out[i].x) libusb_free_transfer(u->out[i].x);
            if (u->in[i].x) libusb_free_transfer(u->in[i].x);
            free(u->out[i].buf); free(u->in[i].buf);
        }
    }
    if (u->h) {
        libusb_release_interface(u->h, 0);
        libusb_close(u->h);
        u->h = NULL;
    }
    while (u->qhead) { Chunk *c = u->qhead; u->qhead = c->next; free(c); }
    pthread_mutex_destroy(&u->mu);
    pthread_cond_destroy(&u->cv);
    free(u);   /* `t` lives inside `u`: nothing may touch it after this */
}

static void flush_stale(Usb *u)
{
    uint8_t tmp[4096];
    int i;
    for (i = 0; i < 64; i++) {
        int got = 0;
        int r = libusb_bulk_transfer(u->h, PSPEXP_USB_EP_IN, tmp, sizeof(tmp), &got, 20);
        if (r == LIBUSB_ERROR_TIMEOUT && got == 0) break;
        if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) break;
    }
}

static int do_sync(Usb *u, char *err, size_t errlen)
{
    int attempt;
    for (attempt = 0; attempt < 3; attempt++) {
        uint8_t reply[PSPEXP_USB_CHUNK];
        int got = 0, r, i;
        uint64_t deadline;
        flush_stale(u);
        r = libusb_bulk_transfer(u->h, PSPEXP_USB_EP_OUT, (unsigned char *)SYNC, PSPEXP_USB_SYNC_LEN, &got, 1000);
        if (r != 0) { snprintf(err, errlen, "sync write failed: %s", libusb_error_name(r)); return -1; }
        deadline = pspexp_time_us() + 1500000u;
        while (pspexp_time_us() < deadline) {
            got = 0;
            r = libusb_bulk_transfer(u->h, PSPEXP_USB_EP_IN, reply, sizeof(reply), &got, 500);
            if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) { snprintf(err, errlen, "sync read failed: %s", libusb_error_name(r)); return -1; }
            /* The reply is its own transfer; tolerate stale bytes ahead of it. */
            for (i = 0; i + PSPEXP_USB_SYNC_LEN <= got; i++)
                if (memcmp(reply + i, SYNC, PSPEXP_USB_SYNC_LEN) == 0) return 0;
        }
    }
    snprintf(err, errlen, "no sync reply from the PSP (is pspexp_runtime running?)");
    return -1;
}

PspExpTransport *pspexp_usb_open(const PspExpUsbOptions *o, char *err, size_t errlen)
{
    Usb *u;
    int r, cfg = 0;
    PspExpUsbOptions opt;
    if (o) opt = *o; else pspexp_usb_options_default(&opt);

    u = (Usb *)calloc(1, sizeof(*u));
    if (!u) { snprintf(err, errlen, "out of memory"); return NULL; }
    u->timeout_ms = opt.transfer_timeout_ms > 0 ? opt.transfer_timeout_ms : 3000;
    u->verbose = opt.verbose;
    u->packet = PSPEXP_USB_PACKET_HS;
    pthread_mutex_init(&u->mu, NULL);
    pthread_cond_init(&u->cv, NULL);
    u->t.name = "usb";
    u->t.user = u;
    u->t.send = tr_send;
    u->t.recv = tr_recv;
    u->t.close = tr_close;

    u->ctx = shared_context(err, errlen);
    if (!u->ctx) { tr_close(&u->t); return NULL; }
    u->h = libusb_open_device_with_vid_pid(u->ctx, opt.vid, opt.pid);
    if (!u->h) { snprintf(err, errlen, "no USB device %04x:%04x", opt.vid, opt.pid); tr_close(&u->t); return NULL; }
    libusb_set_auto_detach_kernel_driver(u->h, 1);
    if (libusb_get_configuration(u->h, &cfg) == 0 && cfg != 1) {
        r = libusb_set_configuration(u->h, 1);
        if (r != 0 && u->verbose) fprintf(stderr, "usb: set_configuration: %s\n", libusb_error_name(r));
    }
    r = libusb_claim_interface(u->h, 0);
    if (r != 0) { snprintf(err, errlen, "claim interface: %s", libusb_error_name(r)); tr_close(&u->t); return NULL; }
    r = libusb_get_max_packet_size(libusb_get_device(u->h), PSPEXP_USB_EP_IN);
    if (r > 0) u->packet = r;
    u->st.packet_size = u->packet;
    libusb_clear_halt(u->h, PSPEXP_USB_EP_IN);
    libusb_clear_halt(u->h, PSPEXP_USB_EP_OUT);

    if (do_sync(u, err, errlen) < 0) { tr_close(&u->t); return NULL; }

    {
        const char *d = getenv("PSPEXP_USB_DEPTH");
        int i;
        u->depth = d && *d ? atoi(d) : 2;
        if (u->depth < 1) u->depth = 1;
        if (u->depth > MAX_DEPTH) u->depth = MAX_DEPTH;
        for (i = 0; u->depth > 1 && i < u->depth; i++) {
            u->out[i].u = u->in[i].u = u;
            u->out[i].x = libusb_alloc_transfer(0);
            u->in[i].x = libusb_alloc_transfer(0);
            u->out[i].buf = (uint8_t *)malloc(PSPEXP_USB_CHUNK);
            u->in[i].buf = (uint8_t *)malloc(PSPEXP_USB_CHUNK);
            if (!u->out[i].x || !u->in[i].x || !u->out[i].buf || !u->in[i].buf) {
                snprintf(err, errlen, "out of memory for USB transfers");
                tr_close(&u->t);
                return NULL;
            }
        }
        if (u->verbose && u->depth > 1) fprintf(stderr, "usb: %d transfers queued each way\n", u->depth);
    }

    if (pthread_create(&u->reader, NULL, reader_main, u) != 0) {
        snprintf(err, errlen, "cannot start reader thread");
        tr_close(&u->t);
        return NULL;
    }
    u->reader_running = 1;
    return &u->t;
}

PspExpTransport *pspexp_usb_wait(const PspExpUsbOptions *o, int timeout_ms, char *err, size_t errlen)
{
    uint64_t deadline = timeout_ms < 0 ? ~(uint64_t)0 : pspexp_time_us() + (uint64_t)timeout_ms * 1000u;
    for (;;) {
        PspExpTransport *t = pspexp_usb_open(o, err, errlen);
        if (t) return t;
        if (pspexp_time_us() >= deadline) return NULL;
        pspexp_sleep_us(250000);
    }
}

const PspExpUsbStats *pspexp_usb_stats(PspExpTransport *t)
{
    return &((Usb *)t->user)->st;
}
