/*
 * Tracker modules (MOD, XM, S3M, IT and the rest libopenmpt reads) for
 * Media: decoded here by libopenmpt, which also tells which pattern, row and
 * channel volumes each piece of sound comes from, for Media's tracker view.
 *
 * libopenmpt is opened at run time (dlopen "libopenmpt.so.0"): Debian's
 * ffmpeg depends on it, so the card images have it; a board without it (the
 * Omega) plays modules through ffmpeg as before, without the view.
 */
#ifndef PSPEXP_TERM_MODPLAY_H
#define PSPEXP_TERM_MODPLAY_H

#include <stddef.h>
#include <stdint.h>

#define MOD_CHANNELS 64   /* VU kept for this many */

typedef struct ModPlay ModPlay;

typedef struct ModState {   /* where the module is at one moment */
    int order, orders, pattern, row, rows, speed, channels;
    double tempo;
    float vu[MOD_CHANNELS];
} ModState;

/* is libopenmpt there (opened once) */
int  mod_available(void);
/* a module from memory (the whole file); NULL and the reason in err */
ModPlay *mod_open(const void *data, size_t len, char *err, size_t en);
void mod_close(ModPlay *m);
/* stereo 16-bit frames at rate; fewer (0) at its end */
size_t mod_read(ModPlay *m, int rate, int16_t *out, size_t frames);
void mod_seek(ModPlay *m, double seconds);
double mod_duration(ModPlay *m);
/* "title", "artist", "type" ("xm"), "type_long", "tracker" */
void mod_meta(ModPlay *m, const char *key, char *out, size_t n);
/* where it is now (after the last read) */
void mod_state(ModPlay *m, ModState *s);
/* a pattern cell as text width wide (C-5 01 v64 A0F...), and its colours,
 * one letter per character: n note, i instrument, u/v volume, e/f effect */
void mod_cell(ModPlay *m, int pattern, int row, int channel, int width, char *text, char *hl, size_t n);
int  mod_pattern_rows(ModPlay *m, int pattern);

#endif
