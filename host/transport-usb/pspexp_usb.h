/*
 * Host USB transport: libusb to the PSPExp kernel driver on the PSP.
 * See protocol/pspexp_usb_link.h for the link rules both ends follow.
 *
 * A background thread keeps a read posted at all times so the PSP's IN
 * endpoint is always drained; recv() pops from the resulting queue. Without
 * that, a host busy sending and a PSP blocked on writing an INPUT sample
 * would deadlock each other.
 */
#ifndef PSPEXP_USB_H
#define PSPEXP_USB_H

#include "../libpspexp/pspexp_transport.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PspExpUsbOptions {
    uint16_t vid, pid;
    int transfer_timeout_ms;   /* per bulk write (default 3000) */
    int verbose;
} PspExpUsbOptions;

typedef struct PspExpUsbStats {
    uint64_t bytes_sent, bytes_received;
    uint32_t transfers_out, transfers_in, splits, read_timeouts;
    int      packet_size;      /* 512 hi-speed, 64 full-speed */
} PspExpUsbStats;

void pspexp_usb_options_default(PspExpUsbOptions *o);
/* Open the first matching device and synchronise the link. NULL on failure
 * with a message in `err`. */
PspExpTransport *pspexp_usb_open(const PspExpUsbOptions *o, char *err, size_t errlen);
/* Poll for a device until one opens or `timeout_ms` (-1 = forever) elapses. */
PspExpTransport *pspexp_usb_wait(const PspExpUsbOptions *o, int timeout_ms, char *err, size_t errlen);
const PspExpUsbStats *pspexp_usb_stats(PspExpTransport *t);

#ifdef __cplusplus
}
#endif
#endif /* PSPEXP_USB_H */
