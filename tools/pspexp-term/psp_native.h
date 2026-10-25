/*
 * The PSP shelf's games, started on the PSP (architecture §12c): the chosen disc made ready on the
 * board (an ISO or CSO as it is; a zip unpacked once into a cache, in the background), named for
 * the PSPExp plugin host, which serves it to the PSPExp plugin's virtual disc, then BOOT_DISC:
 * PSPExp leaves and the PSP starts the game from "pxd0:"; the plugin brings PSPExp back at its exit.
 */
#ifndef PSP_NATIVE_H
#define PSP_NATIVE_H

#include <stdint.h>
#include "../../host/libpspexp/pspexp.h"

/* a disc chosen on the shelf (its file on the board) */
void native_start(PspExpContext *ctx, const char *disc, uint64_t now);
/* each turn of the main loop: the unpacking's progress, then the start */
void native_poll(PspExpContext *ctx, uint64_t now);

#endif
