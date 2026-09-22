/*
 * Host TCP transport (section 14): the same byte stream as USB, over a
 * socket. Reaches the PSP over Wi-Fi, or the PSP runtime under PPSSPP on
 * this machine. The PSP side listens; the host connects and sends the
 * sync packet exactly as over USB.
 */
#ifndef PSPEXP_TCP_H
#define PSPEXP_TCP_H

#include "../libpspexp/pspexp_transport.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSPEXP_TCP_PORT 3838

typedef struct PspExpTcpOptions {
    const char *host;      /* default "127.0.0.1" */
    uint16_t port;         /* default PSPEXP_TCP_PORT */
    int connect_timeout_ms;
    int verbose;
} PspExpTcpOptions;

void pspexp_tcp_options_default(PspExpTcpOptions *o);
PspExpTransport *pspexp_tcp_open(const PspExpTcpOptions *o, char *err, size_t errlen);
/* Retry until connected or timeout_ms (-1 = forever). */
PspExpTransport *pspexp_tcp_wait(const PspExpTcpOptions *o, int timeout_ms, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
#endif
