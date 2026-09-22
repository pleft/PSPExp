#include "pspexp_reader.h"
#include <string.h>
#include <stdlib.h>

long pspexp_message_parse(const void *buf, size_t avail, PspExpMessage *out)
{
    const PspExpMessageHeader *h = (const PspExpMessageHeader *)buf;
    size_t total;
    if (avail < PSPEXP_HEADER_SIZE) {
        /* Reject early when the first bytes cannot start a message. */
        if (avail >= 2 && (((const uint8_t *)buf)[0] | (((const uint8_t *)buf)[1] << 8)) != PSPEXP_MAGIC)
            return -1;
        return 0;
    }
    if (h->magic != PSPEXP_MAGIC || h->version != PSPEXP_PROTOCOL_VERSION ||
        (h->length & (PSPEXP_ALIGN - 1)) != 0 || h->length > PSPEXP_MAX_PAYLOAD ||
        h->channel >= PSPEXP_CH_COUNT)
        return -1;
    total = PSPEXP_HEADER_SIZE + h->length;
    if (avail < total) return 0;
    out->header = h;
    out->payload = (const uint8_t *)buf + PSPEXP_HEADER_SIZE;
    out->length = h->length;
    return (long)total;
}

static void *aligned_alloc16(size_t cap, void **raw)
{
    uint8_t *p = (uint8_t *)malloc(cap + PSPEXP_ALIGN);
    if (!p) return NULL;
    *raw = p;
    return (void *)(((uintptr_t)p + PSPEXP_ALIGN) & ~(uintptr_t)(PSPEXP_ALIGN - 1));
}

void pspexp_stream_init(PspExpStream *s, void *buf, size_t cap)
{
    memset(s, 0, sizeof(*s));
    s->buf = (uint8_t *)buf;
    s->cap = cap;
}

int pspexp_stream_init_dynamic(PspExpStream *s, size_t initial_cap)
{
    void *raw = NULL, *buf;
    if (initial_cap < 4096) initial_cap = 4096;
    buf = aligned_alloc16(initial_cap, &raw);
    if (!buf) return -1;
    pspexp_stream_init(s, buf, initial_cap);
    s->dynamic = 1;
    s->raw = raw;
    return 0;
}

void pspexp_stream_free(PspExpStream *s)
{
    if (s->dynamic && s->raw) free(s->raw);
    s->buf = NULL; s->raw = NULL; s->cap = 0; s->head = s->tail = 0;
}

void pspexp_stream_reset(PspExpStream *s)
{
    s->head = s->tail = 0;
}

static void compact(PspExpStream *s)
{
    if (s->head == 0) return;
    if (s->head == s->tail) { s->head = s->tail = 0; return; }
    memmove(s->buf, s->buf + s->head, s->tail - s->head);
    s->tail -= s->head;
    s->head = 0;
}

static int make_room(PspExpStream *s, size_t len)
{
    size_t pending = s->tail - s->head;
    size_t need = pending + len;
    void *nraw = NULL, *nbuf;
    size_t ncap;
    if (s->cap - s->tail >= len) return 0;
    if (need <= s->cap) { compact(s); return 0; }
    if (!s->dynamic) return -1;
    ncap = s->cap ? s->cap : 4096;
    while (ncap < need) ncap *= 2;
    nbuf = aligned_alloc16(ncap, &nraw);
    if (!nbuf) return -1;
    memcpy(nbuf, s->buf + s->head, pending);
    free(s->raw);
    s->buf = (uint8_t *)nbuf; s->raw = nraw; s->cap = ncap;
    s->head = 0; s->tail = pending;
    return 0;
}

int pspexp_stream_feed(PspExpStream *s, const void *data, size_t len)
{
    if (len == 0) return 0;
    if (make_room(s, len) < 0) return -1;
    memcpy(s->buf + s->tail, data, len);
    s->tail += len;
    s->bytes_in += len;
    return 0;
}

size_t pspexp_stream_writable(PspExpStream *s, void **where)
{
    if (s->cap - s->tail < s->cap / 4) compact(s);
    *where = s->buf + s->tail;
    return s->cap - s->tail;
}

void pspexp_stream_commit(PspExpStream *s, size_t len)
{
    s->tail += len;
    s->bytes_in += len;
}

int pspexp_stream_next(PspExpStream *s, PspExpMessage *m)
{
    long n = pspexp_message_parse(s->buf + s->head, s->tail - s->head, m);
    if (n < 0) return -1;
    if (n == 0) {
        /* Nothing complete: if the pending message cannot fit even after
         * compaction, a fixed stream is stuck. */
        if (!s->dynamic && s->tail - s->head >= PSPEXP_HEADER_SIZE) {
            const PspExpMessageHeader *h = (const PspExpMessageHeader *)(s->buf + s->head);
            if (PSPEXP_HEADER_SIZE + (size_t)h->length > s->cap) return -1;
        }
        return 0;
    }
    s->head += (size_t)n;
    s->messages++;
    if (s->head == s->tail) s->head = s->tail = 0;
    return 1;
}
