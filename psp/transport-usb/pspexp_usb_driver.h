/*
 * PSPExp USB kernel driver: the API it exports to user mode (library
 * "PSPExpUSB"). Link user modules against libpspexpusb.a.
 */
#ifndef PSPEXP_USB_DRIVER_H
#define PSPEXP_USB_DRIVER_H

#include <psptypes.h>

typedef struct PspExpUsbStats {
    u32 rx_bytes, tx_bytes;
    u32 rx_transfers, tx_transfers;
    u32 tx_timeouts, tx_errors, rx_errors;
    u32 resyncs, attaches, detaches;
    u32 packet_size;        /* 512 hi-speed, 64 full-speed */
    u32 rx_queued;          /* bytes buffered, waiting for pspexpUsbRead */
} PspExpUsbStats;

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the bus driver and this driver, activates the device. 0 or error. */
int pspexpUsbStart(void);
int pspexpUsbStop(void);
/* Cable plugged and configured by the host. */
int pspexpUsbAttached(void);
/* Host program has synchronised the link. */
int pspexpUsbConnected(void);
/* Wait for the connected state. 0 when connected, < 0 on timeout. */
int pspexpUsbWaitConnect(unsigned int timeout_us);
/* Increments on every (re)synchronisation; the runtime re-sends HELLO when it changes. */
unsigned int pspexpUsbGeneration(void);
/* Read up to len bytes. > 0 bytes, 0 on timeout, < 0 when not connected. */
int pspexpUsbRead(void *buf, int len, unsigned int timeout_us);
/* Write all len bytes. len on success, < 0 on error (-2 = timeout). */
int pspexpUsbWrite(const void *data, int len);
int pspexpUsbGetStats(PspExpUsbStats *out);
/* 1 while HOME is pressed (read in kernel mode). */
int pspexpUsbHomeHeld(void);
unsigned int pspexpUsbButtons(void);   /* kernel mode's view: volume buttons, HOME, NOTE too */
/* Set the system clock (UTC): microseconds since 0001-01-01, in two halves. 0 or < 0. */
int pspexpUsbSetClock(unsigned int tick_lo, unsigned int tick_hi);
/* The time-zone settings (Settings > Date & Time): minutes east of UTC without
 * summer time, and summer time 0 / 1. 0 or < 0. */
int pspexpUsbSetZone(int tz_minutes, int dst);
int pspexpUsbGetZone(int *tz_minutes, int *dst);
/* The system's mute, which the note button toggles (PSPExp Capture's ♪ pairs put it back): < 0 to
 * read it (0 or 1), 0 or 1 to set it (0). -1: not available (no custom firmware's finder: PPSSPP). */
int pspexpUsbMute(int set);

#ifdef __cplusplus
}
#endif
#endif
