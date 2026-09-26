/*
 * pspexp-term launcher view: the PSP's home screen when a GameShell (or any
 * Linux host) runs pspexp-term --launcher (docs/GameShell_Launcher_and_Shim.md).
 *
 * What it shows comes from its own library (launcher_lib.h): installed
 * PSPExp packages in /opt/pspexp and hand-made entry files, one per game
 * or app, one per emulated system. It draws with
 * its own look (launcher_gfx.h): Inter text, Lucide icons, generated covers
 * or box art, all rendered on the host and cached on the PSP.
 */
#ifndef PSPEXP_TERM_LAUNCHER_H
#define PSPEXP_TERM_LAUNCHER_H

#include <stdint.h>
#include <stddef.h>
#include "../../host/libpspexp/pspexp.h"

/* assets: the directory holding Inter-*.ttf and lucide.ttf. 0 or -1. */
int launcher_init(PspExpContext *ctx, const char *assets);

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
/* 1 once when the user picked System > Console: show the terminal view */
int launcher_take_console(void);
/* how the console reaches the PSP right now ("USB", "Wi-Fi 192.168.50.2"),
 * for the About page */
void launcher_set_link(const char *desc);

#endif
