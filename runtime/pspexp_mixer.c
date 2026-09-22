#include "pspexp_mixer.h"
#include <stdlib.h>
#include <string.h>

int pspexp_mixer_init(PspExpMixer *m, uint32_t rate, uint32_t ring_frames, uint32_t target_frames)
{
    memset(m, 0, sizeof(*m));
    m->rate = rate;
    m->ring_frames = ring_frames;
    m->target_frames = target_frames < ring_frames ? target_frames : ring_frames / 2;
    m->ring = (int16_t *)calloc((size_t)ring_frames * 2, sizeof(int16_t));
    return m->ring ? 0 : -1;
}

void pspexp_mixer_free(PspExpMixer *m)
{
    free(m->ring);
    m->ring = NULL;
}

int pspexp_mixer_play(PspExpMixer *m, uint16_t sound_id, const int16_t *pcm, uint32_t frames,
                      uint8_t channels, uint8_t volume, int8_t pan, uint8_t loop)
{
    int i, slot = -1;
    for (i = 0; i < PSPEXP_AUDIO_MAX_VOICES; i++)
        if (!m->voices[i].active) { slot = i; break; }
    if (slot < 0) {
        /* replace the non-looping voice closest to its end */
        uint32_t best = 0;
        for (i = 0; i < PSPEXP_AUDIO_MAX_VOICES; i++) {
            const PspExpVoice *v = &m->voices[i];
            if (v->loop) continue;
            if (slot < 0 || v->pos > best) { slot = i; best = v->pos; }
        }
        if (slot < 0) return -1;
    }
    m->voices[slot].pcm = pcm;
    m->voices[slot].frames = frames;
    m->voices[slot].pos = 0;
    m->voices[slot].sound_id = sound_id;
    m->voices[slot].channels = channels;
    m->voices[slot].volume = volume;
    m->voices[slot].pan = pan;
    m->voices[slot].loop = loop;
    m->voices[slot].active = 1;
    m->voices_played++;
    return slot;
}

void pspexp_mixer_stop_sound(PspExpMixer *m, uint16_t sound_id)
{
    int i;
    for (i = 0; i < PSPEXP_AUDIO_MAX_VOICES; i++)
        if (m->voices[i].active && (sound_id == 0xFFFF || m->voices[i].sound_id == sound_id))
            m->voices[i].active = 0;
}

uint32_t pspexp_mixer_stream_write(PspExpMixer *m, const int16_t *stereo, uint32_t frames)
{
    uint32_t space = m->ring_frames - m->buffered, n, first;
    if (!m->ring) return 0;
    if (frames > space) { m->dropped += frames - space; frames = space; }
    n = frames;
    first = m->ring_frames - m->tail;
    if (first > n) first = n;
    memcpy(m->ring + (size_t)m->tail * 2, stereo, (size_t)first * 4);
    if (n > first) memcpy(m->ring, stereo + (size_t)first * 2, (size_t)(n - first) * 4);
    m->tail = (m->tail + n) % m->ring_frames;
    m->buffered += n;
    m->received += n;
    if (!m->stream_playing && m->buffered >= m->target_frames) m->stream_playing = 1;
    return n;
}

void pspexp_mixer_stream_reset(PspExpMixer *m)
{
    m->head = m->tail = m->buffered = 0;
    m->stream_playing = 0;
}

static inline int16_t clamp16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void pspexp_mixer_render(PspExpMixer *m, int16_t *out, uint32_t frames)
{
    uint32_t f;
    int i;
    /* Rate adaptation: the host's audio clock and ours drift (and a host
     * without a sound card delivers a few percent slow). Track the buffer
     * level: below half the target, repeat every 32nd stream frame (3%
     * slower consumption); above twice the target, skip every 32nd. */
    int stretch = 0, shrink = 0;
    if (m->stream_playing) {
        if (m->buffered < m->target_frames / 2) stretch = 1;
        else if (m->buffered > m->target_frames * 2) shrink = 1;
    }
    /* Mix in 32-bit; start from the stream, then add voices. */
    for (f = 0; f < frames; f++) {
        int32_t l = 0, r = 0;
        if (m->stream_playing) {
            if (m->buffered > 0) {
                l = m->ring[(size_t)m->head * 2];
                r = m->ring[(size_t)m->head * 2 + 1];
                m->adapt_phase++;
                if (stretch && (m->adapt_phase & 31) == 0) {
                    /* hold this frame: do not advance */
                    m->stretched++;
                } else {
                    m->head = (m->head + 1) % m->ring_frames;
                    m->buffered--;
                    if (shrink && (m->adapt_phase & 31) == 0 && m->buffered > 1) {
                        m->head = (m->head + 1) % m->ring_frames;
                        m->buffered--;
                        m->skipped++;
                    }
                }
            } else {
                m->underruns++;
                m->stream_playing = 0;   /* refill to the target before resuming */
            }
        }
        for (i = 0; i < PSPEXP_AUDIO_MAX_VOICES; i++) {
            PspExpVoice *v = &m->voices[i];
            int32_t sl, sr, gl, gr;
            if (!v->active) continue;
            if (v->channels == 2) {
                sl = v->pcm[(size_t)v->pos * 2];
                sr = v->pcm[(size_t)v->pos * 2 + 1];
            } else {
                sl = sr = v->pcm[v->pos];
            }
            /* constant-power-ish pan in integer: pan -128..127 -> gains 0..255 */
            gl = v->pan <= 0 ? 255 : 255 - (v->pan * 2);
            gr = v->pan >= 0 ? 255 : 255 + (v->pan * 2);
            l += (sl * v->volume / 255) * gl / 255;
            r += (sr * v->volume / 255) * gr / 255;
            if (++v->pos >= v->frames) {
                if (v->loop) v->pos = 0; else v->active = 0;
            }
        }
        out[f * 2] = clamp16(l);
        out[f * 2 + 1] = clamp16(r);
    }
}

void pspexp_mixer_status(const PspExpMixer *m, PspExpAudioStatus *out)
{
    out->buffered_frames = m->buffered;
    out->underruns = m->underruns;
    out->received_frames = m->received;
    out->dropped_frames = m->dropped;
}
