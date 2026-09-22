/*
 * Host-side transport interface (section 14). A transport is a reliable,
 * ordered byte stream in each direction: USB bulk, TCP, or an in-process
 * loopback to a local executor. Nothing about USB leaks above this line.
 */
#ifndef PSPEXP_TRANSPORT_H
#define PSPEXP_TRANSPORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PspExpTransport {
    const char *name;
    void *user;
    /* Send all `len` bytes. 0 on success, -1 when the link is gone. */
    int  (*send)(struct PspExpTransport *t, const void *data, size_t len);
    /* Receive up to `cap` bytes, waiting at most `timeout_ms` (0 = poll).
     * Returns bytes received (>= 1), 0 on timeout, -1 when the link is gone. */
    long (*recv)(struct PspExpTransport *t, void *buf, size_t cap, int timeout_ms);
    void (*close)(struct PspExpTransport *t);
} PspExpTransport;

#ifdef __cplusplus
}
#endif
#endif /* PSPEXP_TRANSPORT_H */
