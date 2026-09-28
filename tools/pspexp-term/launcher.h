/*
 * pspexp-term launcher view: the PSP's home screen when a GameShell (or any
 * Linux host) runs pspexp-term --launcher (docs/Launcher_and_Boards.md).
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
/* 1 once when the user opened Apps > Terminal: show the terminal view */
int launcher_take_console(void);
/* 1 once when the user opened Apps > Files: show the file manager */
int launcher_take_files(void);
/* 1 once with the id of a built-in app the user opened (appview.h: "monitor", "store") */
int launcher_take_app(char *id, size_t n);
/* a file opened from the file manager: a ROM starts in its system's
 * emulator, as from the launcher's lists. 1 when started, 0 when no system
 * plays it */
int launcher_open_file(const char *path);
/* the short name of the system that plays path ("NES"), 0 when none */
int launcher_system_for(const char *path, char *name, size_t n);
/* the title of what is starting or running, NULL when nothing is */
const char *launcher_running_title(void);
/* tests (the control port): what is on screen, as "launcher.key=value" lines
 * appended to out; test mode refuses Restart, Power off and the Wi-Fi switch */
void launcher_state(char *out, size_t n);
void launcher_set_test_mode(int on);
/* the highlight onto the entry titled so in the section shown; 1, or 0 when none */
int  launcher_select(const char *title);
/* the rail onto the section named so; 1, or 0 when none */
int  launcher_category(const char *name);
/* System > PSP clock: set the PSP's clock from this board (1, the default) */
int  launcher_psp_clock(void);
/* PSPExp's own program file was replaced (a package upgrade): say so, offer a restart;
 * 1 once when the user chose System > Restart PSPExp */
void launcher_set_core_updated(void);
int  launcher_take_console_restart(void);
/* a newer PSP runtime is on the board: System > Update the PSP (1 once when chosen) */
void launcher_set_psp_update(int available);
int  launcher_take_psp_update(void);
/* 1 once when the user just turned it on: set it now */
int  launcher_take_clock_request(void);
/* a message at the bottom of the launcher, as its own ones */
void launcher_notice(const char *icon, const char *text);
/* how the console reaches the PSP right now ("USB", "Wi-Fi 192.168.50.2"),
 * for the About page */
void launcher_set_link(const char *desc);

#endif
