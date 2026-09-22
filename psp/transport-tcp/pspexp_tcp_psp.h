/*
 * PSP TCP transport: listens on PSPEXP_TCP_PORT over the PSP's Wi-Fi
 * (connection profile 1) or, under PPSSPP, on the emulator's host network.
 * Same byte stream and sync handshake as the USB link.
 */
#ifndef PSPEXP_TCP_PSP_H
#define PSPEXP_TCP_PSP_H

/* Loads the network modules and starts the access-point connection in the
 * background. 0 when started, < 0 when networking is unavailable. */
int pspexp_tcp_psp_init(void);
/* Drives the state machine: apctl -> listen -> accept -> sync. Call often. */
void pspexp_tcp_psp_poll(void);
int pspexp_tcp_psp_connected(void);
unsigned int pspexp_tcp_psp_generation(void);
/* > 0 bytes, 0 nothing within timeout_us, < 0 disconnected. */
int pspexp_tcp_psp_read(void *buf, int len, unsigned int timeout_us);
int pspexp_tcp_psp_write(const void *data, int len);
/* One line for the status page. */
const char *pspexp_tcp_psp_status(void);
/* Optional trace sink for state changes and errors. */
void pspexp_tcp_psp_set_trace(void (*fn)(const char *msg));

#endif
