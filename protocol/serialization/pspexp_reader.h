/*
 * Message reader: parses PSPExp messages out of a byte stream that arrives
 * in arbitrary chunks (USB transfers, TCP reads, loopback sends).
 */
#ifndef PSPEXP_READER_H
#define PSPEXP_READER_H

#include "../pspexp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PspExpMessage {
    const PspExpMessageHeader *header;
    const uint8_t *payload;   /* 16-byte aligned when the buffer is */
    uint32_t length;          /* header->length */
} PspExpMessage;

/* Parse one message from `avail` bytes at `buf`. Returns the number of bytes
 * the message occupies (> 0), 0 if more bytes are needed, or -1 if the bytes
 * cannot be a valid message (bad magic/version/length). */
long pspexp_message_parse(const void *buf, size_t avail, PspExpMessage *out);
/* Payload length check helper: -1 if the message is shorter than `need`. */
static inline int pspexp_message_expect(const PspExpMessage *m, size_t need)
{
    return m->length >= need ? 0 : -1;
}

typedef struct PspExpStream {
    uint8_t *buf;
    size_t   cap;
    size_t   head;     /* first unconsumed byte */
    size_t   tail;     /* one past the last received byte */
    int      dynamic;
    void    *raw;
    uint64_t bytes_in; /* statistics */
    uint32_t messages;
} PspExpStream;

/* Fixed buffer, 16-byte aligned, must be at least as large as the largest
 * message the peer may send. */
void pspexp_stream_init(PspExpStream *s, void *buf, size_t cap);
int  pspexp_stream_init_dynamic(PspExpStream *s, size_t initial_cap);
void pspexp_stream_free(PspExpStream *s);
void pspexp_stream_reset(PspExpStream *s);

/* Append received bytes. Returns 0, or -1 if they do not fit (fixed buffer
 * and the pending message is larger than the buffer, or allocation failed). */
int pspexp_stream_feed(PspExpStream *s, const void *data, size_t len);
/* Space currently available for a feed without moving or growing. */
size_t pspexp_stream_writable(PspExpStream *s, void **where);
/* Commit `len` bytes written directly into the pointer from _writable(). */
void pspexp_stream_commit(PspExpStream *s, size_t len);

/* Next complete message: 1 and `m` filled (valid until the next feed/next),
 * 0 if none is complete yet, -1 if the stream is corrupt (the caller should
 * drop the connection). */
int pspexp_stream_next(PspExpStream *s, PspExpMessage *m);

#ifdef __cplusplus
}
#endif
#endif /* PSPEXP_READER_H */
