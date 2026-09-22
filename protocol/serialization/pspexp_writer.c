#include "pspexp_writer.h"
#include <string.h>
#include <stdlib.h>

#define NO_MESSAGE ((size_t)-1)

static void *aligned_alloc16(size_t cap, void **raw)
{
    uint8_t *p = (uint8_t *)malloc(cap + PSPEXP_ALIGN);
    if (!p) return NULL;
    *raw = p;
    return (void *)(((uintptr_t)p + PSPEXP_ALIGN) & ~(uintptr_t)(PSPEXP_ALIGN - 1));
}

void pspexp_writer_init(PspExpWriter *w, void *buf, size_t cap)
{
    memset(w, 0, sizeof(*w));
    w->buf = (uint8_t *)buf;
    w->cap = cap;
    w->msg_start = NO_MESSAGE;
}

int pspexp_writer_init_dynamic(PspExpWriter *w, size_t initial_cap)
{
    void *raw = NULL;
    void *buf;
    if (initial_cap < 256) initial_cap = 256;
    buf = aligned_alloc16(initial_cap, &raw);
    if (!buf) return -1;
    pspexp_writer_init(w, buf, initial_cap);
    w->dynamic = 1;
    w->raw = raw;
    return 0;
}

void pspexp_writer_free(PspExpWriter *w)
{
    if (w->dynamic && w->raw) free(w->raw);
    w->buf = NULL; w->raw = NULL; w->cap = w->len = 0;
}

void pspexp_writer_reset(PspExpWriter *w)
{
    w->len = 0;
    w->msg_start = NO_MESSAGE;
    w->error = 0;
}

static int ensure(PspExpWriter *w, size_t extra)
{
    size_t need = w->len + extra;
    size_t ncap;
    void *nraw = NULL, *nbuf;
    if (need <= w->cap) return 0;
    if (!w->dynamic) { w->error = 1; return -1; }
    ncap = w->cap ? w->cap : 256;
    while (ncap < need) ncap *= 2;
    nbuf = aligned_alloc16(ncap, &nraw);
    if (!nbuf) { w->error = 1; return -1; }
    memcpy(nbuf, w->buf, w->len);
    free(w->raw);
    w->buf = (uint8_t *)nbuf; w->raw = nraw; w->cap = ncap;
    return 0;
}

int pspexp_writer_begin(PspExpWriter *w, uint8_t channel, uint16_t opcode, uint16_t flags)
{
    PspExpMessageHeader h;
    if (w->msg_start != NO_MESSAGE) { w->error = 1; return -1; }
    if (ensure(w, PSPEXP_HEADER_SIZE) < 0) return -1;
    h.magic = PSPEXP_MAGIC;
    h.version = PSPEXP_PROTOCOL_VERSION;
    h.channel = channel;
    h.opcode = opcode;
    h.flags = flags;
    h.sequence = w->sequence++;
    h.length = 0;
    memcpy(w->buf + w->len, &h, sizeof(h));
    w->msg_start = w->len;
    w->len += PSPEXP_HEADER_SIZE;
    return 0;
}

int pspexp_writer_append(PspExpWriter *w, const void *data, size_t len)
{
    if (w->msg_start == NO_MESSAGE) { w->error = 1; return -1; }
    if (len == 0) return 0;
    if (ensure(w, len) < 0) return -1;
    memcpy(w->buf + w->len, data, len);
    w->len += len;
    return 0;
}

void *pspexp_writer_reserve(PspExpWriter *w, size_t len)
{
    void *p;
    if (w->msg_start == NO_MESSAGE) { w->error = 1; return NULL; }
    if (ensure(w, len) < 0) return NULL;
    p = w->buf + w->len;
    memset(p, 0, len);
    w->len += len;
    return p;
}

int pspexp_writer_end(PspExpWriter *w)
{
    size_t payload, padded, pad;
    PspExpMessageHeader *h;
    if (w->msg_start == NO_MESSAGE) { w->error = 1; return -1; }
    payload = w->len - w->msg_start - PSPEXP_HEADER_SIZE;
    padded = PSPEXP_ALIGN_UP(payload);
    pad = padded - payload;
    if (padded > PSPEXP_MAX_PAYLOAD) { w->error = 1; return -1; }
    if (pad && ensure(w, pad) < 0) return -1;
    if (pad) memset(w->buf + w->len, 0, pad);
    w->len += pad;
    h = (PspExpMessageHeader *)(w->buf + w->msg_start);
    h->length = (uint32_t)padded;
    w->msg_start = NO_MESSAGE;
    return 0;
}

int pspexp_writer_message(PspExpWriter *w, uint8_t channel, uint16_t opcode, uint16_t flags,
                          const void *payload, size_t len)
{
    if (pspexp_writer_begin(w, channel, opcode, flags) < 0) return -1;
    if (len && pspexp_writer_append(w, payload, len) < 0) return -1;
    return pspexp_writer_end(w);
}
