/*
 * pspexp-term's music player: the sound side of the file manager's player
 * screen (files.c draws it).
 *
 * MP3 goes to the PSP as it is, whole frames, and the PSP decodes it on its
 * Media Engine (AUDIO_CODED_STREAM, PSPEXP_FEATURE_AUDIO_MP3): a few KB a
 * second over the link and next to no board CPU. 16-bit WAV is sent as PCM.
 * Anything else (Ogg, FLAC, Opus, M4A, MOD...) and MP3 for a runtime without
 * the feature: decoded on the board by ffmpeg, when it is installed, and sent
 * as PCM.
 *
 * Everything runs on the thread that drives the link (player_pump, from
 * pspexp-term's main loop).
 */
#ifndef PSPEXP_TERM_PLAYER_H
#define PSPEXP_TERM_PLAYER_H

#include <stddef.h>
#include "../../host/libpspexp/pspexp.h"

typedef struct PlayerInfo {
    char   title[160], artist[160], album[160];
    char   format[96];       /* "MP3 192 kbps 44.1 kHz" */
    char   decoder[48];      /* "decoded on the PSP (Media Engine)" */
    char   cover[300];       /* a picture file for the cover, or "" */
    double pos, dur;         /* seconds; dur 0 when unknown */
    int    playing, paused, ended;
} PlayerInfo;

/* is this a file the player takes (by its name) */
int  player_handles(const char *name);
/* stop whatever plays and start path; -1 and err when it cannot be played */
int  player_start(PspExpContext *ctx, const char *path, char *err, size_t en);
void player_stop(void);
void player_toggle_pause(void);
void player_seek(double delta_s);
/* each main-loop turn: feeds the PSP ahead of its clock */
void player_pump(PspExpContext *ctx);
/* 1 while a track is loaded (playing, paused or just ended) */
int  player_info(PlayerInfo *out);

#endif
