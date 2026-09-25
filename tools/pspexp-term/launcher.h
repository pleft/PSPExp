/*
 * pspexp-term launcher view: the GameShell's menu tree shown as a launcher
 * made for the PSP's 480x272 (docs/GameShell_Launcher_and_Shim.md, section 3).
 *
 * The content model is launchergo's, so every existing GameShell package and
 * skin shows up unchanged: menu roots hold categories (folders) and entries
 * (.sh scripts); a folder with plugin.json is a built-in page, one with
 * action.config an emulator whose ROM folder is listed; the skin holds one
 * PNG per entry. Everything is drawn through libpspexp: a CLUT4 glyph atlas
 * rasterised from the skin's TTF, the icons as 4444 textures, cards as
 * alpha-blended rectangles. A static screen sends nothing.
 */
#ifndef PSPEXP_TERM_LAUNCHER_H
#define PSPEXP_TERM_LAUNCHER_H

#include <stdint.h>
#include <stddef.h>
#include "../../host/libpspexp/pspexp.h"

/* roots: menu directories separated by ':' (launchergo/Menu/GameShell, apps/Menu);
 * skin: the skin directory (holding Menu/GameShell PNG icons and truetype/). 0 or -1. */
int launcher_init(PspExpContext *ctx, const char *roots, const char *skin);

/* Buttons: b = current mask, pressed = newly pressed, up..right = fired with
 * auto-repeat (1 on press and on repeats). */
void launcher_input(uint32_t b, uint32_t pressed, int up, int down, int left, int right, uint64_t now);

/* 1 when something changed or an animation is running: render a frame. */
int launcher_needs_frame(uint64_t now);
void launcher_render(PspExpContext *ctx, uint64_t now, int psp_connected);
void launcher_invalidate(void);

/* A launch requested by the last input, once: the shell command to run and
 * the directory to run it in. `pspexp_aware` = the script hands the PSP over
 * itself (pspexp-run / pspexp-exec), so the console need not mirror it. */
int launcher_take_launch(char *cmd, size_t cmd_len, char *cwd, size_t cwd_len, int *pspexp_aware);
void launcher_child_exited(int status);

#endif
