/*
 * A song's title and artist from its own file, for Media's lists (the player reads its tags only
 * for the track it plays, ffprobe for most formats: too slow for a folder): ID3v2 and ID3v1 (MP3),
 * FLAC's and Ogg's (Vorbis, Opus) comments, from the file's first 32 KB (ID3v1: its last 128
 * bytes). Anything else, or no title there: 0, and the caller shows the file's name.
 */
#ifndef PSPEXP_TERM_TAGS_H
#define PSPEXP_TERM_TAGS_H

#include <stddef.h>
#include <stdint.h>

/* 1 when a title was found (artist "" when none); UTF-8 */
int tags_peek(const char *path, char *title, size_t tn, char *artist, size_t an);
/* the same from bytes already read (the file's start; tail: its last 128 bytes or NULL) */
int tags_parse(const uint8_t *b, size_t n, const uint8_t *tail, char *title, size_t tn, char *artist, size_t an);

#endif
