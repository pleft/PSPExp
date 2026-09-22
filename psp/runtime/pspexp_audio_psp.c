#include "pspexp_audio_psp.h"
#include <pspkernel.h>
#include <pspaudio.h>
#include <string.h>

#define BLOCK_FRAMES 1024

static PspExpMixer *g_mixer;
static SceUID g_thread = -1;
static SceUID g_sema = -1;
static int g_channel = -1;
static volatile int g_running;
static int16_t g_buf[2][BLOCK_FRAMES * 2] __attribute__((aligned(64)));

static int audio_thread(SceSize args, void *argp)
{
    int cur = 0;
    (void)args; (void)argp;
    while (g_running) {
        sceKernelWaitSema(g_sema, 1, NULL);
        if (g_mixer) pspexp_mixer_render(g_mixer, g_buf[cur], BLOCK_FRAMES);
        else memset(g_buf[cur], 0, sizeof(g_buf[cur]));
        sceKernelSignalSema(g_sema, 1);
        sceAudioOutputPannedBlocking(g_channel, PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX, g_buf[cur]);
        cur ^= 1;
    }
    sceKernelExitDeleteThread(0);
    return 0;
}

static int op_audio_start(void *u, PspExpMixer *m, uint32_t rate)
{
    (void)u; (void)rate;   /* the hardware channel runs at 44.1 kHz; the host negotiated that */
    if (g_channel >= 0) return -1;
    g_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, BLOCK_FRAMES, PSP_AUDIO_FORMAT_STEREO);
    if (g_channel < 0) { g_channel = -1; return -1; }
    if (g_sema < 0) g_sema = sceKernelCreateSema("PSPExpAudio", 0, 1, 1, NULL);
    g_mixer = m;
    g_running = 1;
    g_thread = sceKernelCreateThread("PSPExpAudio", audio_thread, 16, 0x2000, 0, NULL);
    if (g_thread < 0) { sceAudioChRelease(g_channel); g_channel = -1; g_mixer = NULL; return -1; }
    sceKernelStartThread(g_thread, 0, NULL);
    return 0;
}

static void op_audio_stop(void *u)
{
    (void)u;
    if (g_channel < 0) return;
    g_running = 0;
    sceKernelDelayThread(60000);   /* let the thread finish its block */
    sceKernelWaitSema(g_sema, 1, NULL);
    g_mixer = NULL;
    sceKernelSignalSema(g_sema, 1);
    sceAudioChRelease(g_channel);
    g_channel = -1;
    g_thread = -1;
}

static void op_audio_lock(void *u) { (void)u; if (g_sema >= 0) sceKernelWaitSema(g_sema, 1, NULL); }
static void op_audio_unlock(void *u) { (void)u; if (g_sema >= 0) sceKernelSignalSema(g_sema, 1); }

void pspexp_audio_psp_ops(PspExpRuntimeOps *ops)
{
    ops->audio_start = op_audio_start;
    ops->audio_stop = op_audio_stop;
    ops->audio_lock = op_audio_lock;
    ops->audio_unlock = op_audio_unlock;
}
