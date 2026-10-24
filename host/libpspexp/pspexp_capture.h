/*
 * PSPExp Capture inside PSPExp: the PSP's runtime captures what it shows and plays and sends it
 * over PSPExp's own link (CAPTURE messages); whichever program holds the link (the console, a
 * game through the SDL layer) passes the bytes on, as they come, to the receiver
 * (tools/pspexp-capture) over this local socket, in the packets PSPExp Capture's plugin sends
 * over USB for a native game ("PXCF" frames, "PXCA" sound, "PXCE" note-button events, "PXCM" log).
 */
#ifndef PSPEXP_CAPTURE_H
#define PSPEXP_CAPTURE_H

#include <stdio.h>
#include <unistd.h>

/* the receiver's socket, one per user: /tmp/pspexp-capture-<uid>.sock */
static inline void pspexp_capture_socket_path(char *out, size_t n)
{
    snprintf(out, n, "/tmp/pspexp-capture-%u.sock", (unsigned)getuid());
}

#endif /* PSPEXP_CAPTURE_H */
