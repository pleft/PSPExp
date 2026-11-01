/* Button sounds: see sfx.h */
#include "sfx.h"
#include <math.h>
#include <string.h>

#define SFX_RATE 44100            /* the console's output rate (the PSP's channels have no rate of their own) */
#define SFX_ID   7000             /* + which: their ids on the PSP (apart from the launcher's textures) */
#define SFX_MAX  (SFX_RATE / 10)  /* 100 ms at most */

static int16_t g_pcm[SFX_COUNT][SFX_MAX];
static int g_len[SFX_COUNT], g_made;

/* a soft tone from f0 to f1 Hz, len ms, its start at `at` ms: a 1.5 ms rise, then an exponential
 * fall (tau ms), peak `amp` of full scale; added into s */
static void tone(int which, double at, double len, double f0, double f1, double tau, double amp)
{
    int i, a = (int)(at * SFX_RATE / 1000), n = (int)(len * SFX_RATE / 1000);
    double ph = 0;
    for (i = 0; i < n && a + i < SFX_MAX; i++) {
        double t = (double)i / SFX_RATE * 1000.0, f = f0 + (f1 - f0) * i / n;
        double env = (t < 1.5 ? t / 1.5 : 1.0) * exp(-t / tau);
        double v = g_pcm[which][a + i] + sin(ph) * env * amp * 32767.0;
        ph += 2 * M_PI * f / SFX_RATE;
        g_pcm[which][a + i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        if (a + i + 1 > g_len[which]) g_len[which] = a + i + 1;
    }
}

static void make(void)
{
    memset(g_pcm, 0, sizeof(g_pcm));
    tone(SFX_MOVE, 0, 14, 2100, 1900, 3.0, 0.16);          /* a tick */
    tone(SFX_SWITCH, 0, 22, 1500, 1500, 5.0, 0.14);        /* two soft ticks: a category, a tab, a view */
    tone(SFX_SWITCH, 28, 22, 1800, 1800, 5.0, 0.11);
    tone(SFX_OPEN, 0, 30, 880, 880, 9.0, 0.16);            /* up: in, chosen */
    tone(SFX_OPEN, 32, 45, 1320, 1320, 12.0, 0.15);
    tone(SFX_BACK, 0, 28, 1320, 1320, 8.0, 0.13);          /* down: back */
    tone(SFX_BACK, 30, 40, 880, 880, 11.0, 0.13);
    tone(SFX_PANEL, 0, 45, 620, 980, 12.0, 0.16);          /* a soft rising pop: a panel, a menu */
    g_made = 1;
}

void sfx_play(PspExpContext *ctx, int which)
{
    int i;
    if (which < 0 || which >= SFX_COUNT || pspexp_audio_local(ctx)) return;
    if (!g_made) make();
    /* uploaded when the PSP holds none: libpspexp sends its uploads again itself on a new link (the
     * working set), so uploading them again at each connection gave the PSP each one twice, five
     * DUPLICATE_ID errors a reconnect (the test suite's "no link errors", 2026-11-01) */
    for (i = 0; i < SFX_COUNT; i++)
        if (pspexp_resource_state(ctx, (uint16_t)(SFX_ID + i), NULL) == PSPEXP_RES_NONE)
            pspexp_upload_sound(ctx, (uint16_t)(SFX_ID + i), g_pcm[i], (uint32_t)g_len[i], 1);
    pspexp_play_sound(ctx, (uint16_t)(SFX_ID + which), 255, 0, 0);
}
