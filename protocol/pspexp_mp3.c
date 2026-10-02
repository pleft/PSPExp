#include "pspexp_mp3.h"

int pspexp_mp3_header(const uint8_t *p, size_t n, PspExpMp3Frame *out)
{
    static const uint16_t KBPS1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const uint16_t KBPS2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
    static const uint16_t RATE1[3] = { 44100, 48000, 32000 };
    int ver, br, sr, pad;
    if (n < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return -1;
    switch ((p[1] >> 3) & 3) { case 3: ver = 10; break; case 2: ver = 20; break; case 0: ver = 25; break; default: return -1; }
    if (((p[1] >> 1) & 3) != 1) return -1;   /* layer III only */
    br = (p[2] >> 4) & 15;
    sr = (p[2] >> 2) & 3;
    if (br == 0 || br == 15 || sr == 3) return -1;
    pad = (p[2] >> 1) & 1;
    out->version = ver;
    out->bitrate_kbps = ver == 10 ? KBPS1[br] : KBPS2[br];
    out->sample_rate = RATE1[sr] / (ver == 10 ? 1 : ver == 20 ? 2 : 4);
    out->channels = ((p[3] >> 6) & 3) == 3 ? 1 : 2;
    out->samples = ver == 10 ? 1152 : 576;
    out->bytes = (ver == 10 ? 144000 : 72000) * out->bitrate_kbps / out->sample_rate + pad;
    return 0;
}
