/*
 * MP3 frame headers (MPEG-1, 2 and 2.5 layer III): what the host needs to cut
 * a file into whole frames and the runtime to hand each one to the decoder.
 */
#ifndef PSPEXP_MP3_H
#define PSPEXP_MP3_H

#include <stddef.h>
#include <stdint.h>

typedef struct PspExpMp3Frame {
    int version;        /* 10 MPEG-1, 20 MPEG-2, 25 MPEG-2.5 */
    int bitrate_kbps;
    int sample_rate;
    int channels;       /* 1 or 2 */
    int bytes;          /* the whole frame, header included */
    int samples;        /* per channel: 1152 (MPEG-1) or 576 */
} PspExpMp3Frame;

/* 0 and *out filled when p starts a layer III frame header (free-format
 * frames, with no bitrate in the header, are refused); -1 otherwise */
int pspexp_mp3_header(const uint8_t *p, size_t n, PspExpMp3Frame *out);

#endif
