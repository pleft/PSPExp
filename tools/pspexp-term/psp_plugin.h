/*
 * The PSPExp plugin on the PSP's memory stick (docs/manual/plugin.md): the board's copy (the
 * pspexp-runtime package, DIR/plugin/pspexp_plugin.prx) put beside PSPExp by Update the PSP, and
 * switched on and off by its line in ms0:/seplugins/game.txt (Settings > Recording > PSPExp
 * plugin). PRO loads it into every game the PSP starts: recording them, and the PSP shelf's disc.
 */
#ifndef PSP_PLUGIN_H
#define PSP_PLUGIN_H

#include <stddef.h>
#include "../../host/libpspexp/pspexp.h"

#define PSP_PLUGIN_FILE "pspexp_plugin.prx"
#define PSP_PLUGIN_GAME_TXT "ms0:/seplugins/game.txt"

enum { PSP_PLUGIN_UNKNOWN = -2, PSP_PLUGIN_MISSING = -1, PSP_PLUGIN_OFF = 0, PSP_PLUGIN_ON = 1 };

/* game.txt's text: 1 when a line names the plugin and ends in 1 (PRO's "on") */
int psp_plugin_listed(const char *txt);
/* game.txt's text with the plugin's line set on or off (PATH, the plugin on the stick, appended
 * when no line names it; the other lines and their line ends kept): its length, -1 if it does not
 * fit in N, 0 when nothing is to be written (off, and no line) */
int psp_plugin_game_txt(const char *in, const char *path, int on, char *out, size_t n);

/* on the stick, for the install in INSTALL (ms0:/PSP/GAME/PSPExp, PspExpSlots.dir): MISSING (no
 * plugin file there), OFF or ON (its game.txt line) */
int psp_plugin_state(PspExpContext *ctx, const char *install);
/* 1 when the board has a plugin (BOARD_DIR/plugin/pspexp_plugin.prx) and the stick's is not the
 * same, or none; 0 the same, or the board has none; < 0 the stick could not be read */
int psp_plugin_outdated(PspExpContext *ctx, const char *board_dir, const char *install);
/* the board's plugin onto the stick (a .new first, read back, then in place): 0, or < 0 and ERR */
int psp_plugin_install(PspExpContext *ctx, const char *board_dir, const char *install, char *err, size_t n);
/* its game.txt line on or off (the file written as .new, read back, then in place): 0, or < 0 */
int psp_plugin_switch(PspExpContext *ctx, const char *install, int on, char *err, size_t n);

#endif
