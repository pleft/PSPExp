/*
 * Message writer: builds PSPExp messages into a byte buffer that can be
 * handed to a transport as-is. Either a caller-supplied fixed buffer (PSP
 * runtime, small replies) or a growing heap buffer (host frame builder).
 * The buffer is always 16-byte aligned so payloads are too.
 */
#ifndef PSPEXP_WRITER_H
#define PSPEXP_WRITER_H

#include "../pspexp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PspExpWriter {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    size_t   msg_start;   /* offset of the open message's header, or SIZE_MAX */
    uint32_t sequence;    /* next sequence number to assign */
    int      dynamic;
    int      error;       /* sticky: set when a write did not fit */
    void    *raw;         /* unaligned allocation, dynamic only */
} PspExpWriter;

/* Fixed buffer; `buf` must be 16-byte aligned. */
void pspexp_writer_init(PspExpWriter *w, void *buf, size_t cap);
/* Heap buffer that grows as needed. Returns 0 or -1 on allocation failure. */
int  pspexp_writer_init_dynamic(PspExpWriter *w, size_t initial_cap);
void pspexp_writer_free(PspExpWriter *w);
/* Discards buffered bytes; keeps the sequence counter. */
void pspexp_writer_reset(PspExpWriter *w);

/* Open a message; then append payload; then end. Returns 0 or -1. */
int   pspexp_writer_begin(PspExpWriter *w, uint8_t channel, uint16_t opcode, uint16_t flags);
int   pspexp_writer_append(PspExpWriter *w, const void *data, size_t len);
/* Reserve `len` zeroed bytes of payload and return a pointer to fill in place
 * (valid until the next writer call). NULL on failure. */
void *pspexp_writer_reserve(PspExpWriter *w, size_t len);
/* Pads the payload to 16 bytes and finalises the header. */
int   pspexp_writer_end(PspExpWriter *w);

/* begin + append + end in one call; payload may be NULL when len is 0. */
int pspexp_writer_message(PspExpWriter *w, uint8_t channel, uint16_t opcode, uint16_t flags,
                          const void *payload, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* PSPEXP_WRITER_H */
