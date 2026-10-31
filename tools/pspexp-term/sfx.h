/*
 * Button sounds (2026-10-31): a soft click as you move and press buttons in the launcher, the apps
 * and Files (not the terminal, not a game). Each sound is made here once, uploaded to the PSP as a
 * cached sound effect at each connection, and played by its mixer over the music (a few bytes a
 * press, no delay). Settings > Sound > Button sounds turns them off; with the sound on Bluetooth
 * headphones they are not played (they would come from the PSP's speaker).
 */
#ifndef PSPEXP_TERM_SFX_H
#define PSPEXP_TERM_SFX_H

#include "../../host/libpspexp/pspexp.h"

enum { SFX_MOVE, SFX_SWITCH, SFX_OPEN, SFX_BACK, SFX_PANEL, SFX_COUNT };

/* a new connection: the sounds are uploaded again before the next one plays */
void sfx_connected(void);
void sfx_play(PspExpContext *ctx, int which);

#endif
