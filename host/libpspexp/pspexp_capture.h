/*
 * Recording inside PSPExp: the PSP's runtime captures what it shows and plays and sends it
 * over PSPExp's own link (CAPTURE messages); whichever program holds the link (the console, a
 * game through the SDL layer) passes the bytes on, as they come, to the receiver
 * (tools/pspexp-plugin-host) over this local socket, in the packets the PSPExp plugin sends
 * over USB for a native game ("PXCF" frames, "PXCA" sound, "PXCE" note-button events, "PXCM" log).
 */
#ifndef PSPEXP_CAPTURE_H
#define PSPEXP_CAPTURE_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* the receiver's socket, one per user: /tmp/pspexp-plugin-host-<uid>.sock (PSPEXP_CAPTURE_SOCKET:
 * another, for tests) */
static inline void pspexp_capture_socket_path(char *out, size_t n)
{
    const char *e = getenv("PSPEXP_CAPTURE_SOCKET");
    if (e && *e) snprintf(out, n, "%s", e);
    else snprintf(out, n, "/tmp/pspexp-plugin-host-%u.sock", (unsigned)getuid());
}

/* the PSP game the plugin's virtual disc reads (the PSP shelf: architecture §12c): the console
 * writes its image's path (an ISO or CSO; a zip unpacked first) into this file before starting
 * it, the receiver serves its sectors; PSPEXP_DISC_FILE names another file (tests) */
static inline void pspexp_capture_disc_file(char *out, size_t n)
{
    const char *e = getenv("PSPEXP_DISC_FILE");
    if (e && *e) snprintf(out, n, "%s", e);
    else snprintf(out, n, "/tmp/pspexp-disc-%u", (unsigned)getuid());
}

#endif /* PSPEXP_CAPTURE_H */
