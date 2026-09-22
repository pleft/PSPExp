/*
 * Software audio mixer shared by every executor (section 8): up to
 * PSPEXP_AUDIO_MAX_VOICES cached-sound voices plus one streamed PCM channel
 * with a jitter buffer, rendered as s16 stereo at the negotiated rate.
 * Plain C99. The caller serialises access (an executor's audio thread
 * renders; the runtime thread plays, stops and feeds the stream).
 */
#ifndef PSPEXP_MIXER_H
#define PSPEXP_MIXER_H

#include "../protocol/pspexp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PspExpVoice {
    const int16_t *pcm;
    uint32_t frames, pos;
    uint16_t sound_id;
    uint8_t  channels, volume, loop, active;
    int8_t   pan;
} PspExpVoice;

typedef struct PspExpMixer {
    uint32_t rate;
    PspExpVoice voices[PSPEXP_AUDIO_MAX_VOICES];
    int16_t *ring;               /* stereo frames */
    uint32_t ring_frames, head, tail, buffered;
    uint32_t target_frames;      /* jitter buffer depth before the stream starts */
    int      stream_playing;
    uint32_t underruns, received, dropped, voices_played;
    uint32_t adapt_phase, stretched, skipped;   /* rate adaptation counters */
} PspExpMixer;

int  pspexp_mixer_init(PspExpMixer *m, uint32_t rate, uint32_t ring_frames, uint32_t target_frames);
void pspexp_mixer_free(PspExpMixer *m);
/* Start a voice. Returns the voice index or -1 when all are busy (the
 * oldest non-looping voice is then replaced). */
int  pspexp_mixer_play(PspExpMixer *m, uint16_t sound_id, const int16_t *pcm, uint32_t frames,
                       uint8_t channels, uint8_t volume, int8_t pan, uint8_t loop);
void pspexp_mixer_stop_sound(PspExpMixer *m, uint16_t sound_id);   /* 0xFFFF = all */
/* Append stereo frames to the stream; returns frames accepted. */
uint32_t pspexp_mixer_stream_write(PspExpMixer *m, const int16_t *stereo, uint32_t frames);
void pspexp_mixer_stream_reset(PspExpMixer *m);
/* Render `frames` stereo frames (voices + stream) into `out`. */
void pspexp_mixer_render(PspExpMixer *m, int16_t *out, uint32_t frames);
void pspexp_mixer_status(const PspExpMixer *m, PspExpAudioStatus *out);

#ifdef __cplusplus
}
#endif
#endif /* PSPEXP_MIXER_H */
