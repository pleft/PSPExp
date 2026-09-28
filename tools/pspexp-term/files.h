/*
 * pspexp-term's file manager view: two panels in the spirit of Midnight
 * Commander, drawn like the launcher (launcher_gfx), for the PSP's buttons:
 *
 *   d-pad         move (left / right: a page); up from the top row: the drive
 *                 button, cross there: every drive (the board's mounts, the PSP)
 *   L / R         the left / right panel
 *   cross         open: a folder, or a file (text: the viewer)
 *   circle        the folder above
 *   square        mark / unmark, then down (for several files)
 *   triangle      actions: copy or move to the other panel, delete,
 *                 rename, new folder, details
 *   start         options: hidden files, sort, same folder in both panels,
 *                 terminal here, close
 *   select        close (held: the next view, as everywhere)
 *
 * Copy, move and delete run on a thread (files_ops.c) with progress on
 * screen and circle to cancel; deleting and replacing ask first.
 */
#ifndef PSPEXP_TERM_FILES_H
#define PSPEXP_TERM_FILES_H

#include <stddef.h>
#include <stdint.h>
#include "../../host/libpspexp/pspexp.h"

/* the launcher's graphics must be up (launcher_init) */
void files_open(const char *path);
void files_input(uint32_t b, uint32_t pressed, int up, int down, int left, int right, uint64_t now);
int  files_needs_frame(uint64_t now);
void files_render(PspExpContext *ctx, uint64_t now);
void files_invalidate(void);
/* 1 once when the user closed the file manager */
int  files_take_exit(void);
/* 1 once with a folder when the user asked for a terminal there */
int  files_take_terminal(char *path, size_t n);
/* tests (the control port): what is on screen, as "files.key=value" lines appended to out */
void files_state(char *out, size_t n);

#endif
