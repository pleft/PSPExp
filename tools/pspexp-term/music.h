/*
 * The console's Music app (Apps > Music): what plays, the folders to choose
 * from and the queue, with the music going on behind the launcher and the
 * other apps. The sound itself is player.c's; this keeps the queue (a folder's
 * tracks, in order or shuffled, repeated or not), moves on when a track ends
 * whatever is on the screen, and draws the app (appview.h: MUSIC_APP).
 *
 * Files hands songs to it (apps.c: the app that opens a file by its name), the
 * launcher shows what plays (its top bar, a "Now playing" pill on the home
 * screen) and the PSP's note button pauses and resumes it from anywhere.
 */
#ifndef PSPEXP_TERM_MUSIC_H
#define PSPEXP_TERM_MUSIC_H

#include <stddef.h>
#include "files_ops.h"
#include "../../host/libpspexp/pspexp.h"

/* the context the sound goes through (pspexp-term's link) */
void music_init(PspExpContext *ctx);
/* play dir's playable entries (of the n given: folders and others are left out) from entries[start] */
int  music_play_list(const char *dir, const FEntry *entries, int n, int start);
/* each main-loop turn: the next track when one ends */
void music_tick(uint64_t now);
/* the note button and the launcher: pause or resume (a track that ended: from its start) */
void music_toggle(void);
void music_stop(void);
/* 1 while a track is loaded; its title (or file name) and whether it is paused */
int  music_now(char *title, size_t n, int *paused);

#endif
