#include "pspexp_audio_psp.h"
#include "pspexp_capture_psp.h"
#include <pspkernel.h>
#include <pspaudio.h>
#include <pspaudiocodec.h>
#include <psputility.h>
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
        pspexp_capture_psp_audio(g_buf[cur], BLOCK_FRAMES);   /* (recording: what is about to play) */
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

/* ---- MP3 frames decoded by the firmware's codec, which runs on the Media
 * Engine: the main CPU only waits for each frame (AUDIO_CODED_STREAM) ---- */

static unsigned long g_codec[65] __attribute__((aligned(64)));   /* the codec's control block */
static int g_codec_state;   /* 0 not tried, 1 ready, -1 unavailable */
static int g_codec_format;  /* the header bits the codec was set up for: version, rate, mode */
static uint8_t g_src[2048] __attribute__((aligned(64)));         /* a frame: 1441 bytes at most */
static int16_t g_dec[1152 * 2] __attribute__((aligned(64)));

static int codec_init(void)
{
    static int loaded;
    if (!loaded) {   /* once: again, it answers "already loaded" (0x80111102) */
        int r = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
        if (r < 0 && r != (int)0x80020139 && r != (int)0x80111102) return -1;
        loaded = 1;
    }
    memset(g_codec, 0, sizeof(g_codec));
    if (sceAudiocodecCheckNeedMem(g_codec, PSP_CODEC_MP3) < 0) return -1;
    if (sceAudiocodecGetEDRAM(g_codec, PSP_CODEC_MP3) < 0) return -1;
    if (sceAudiocodecInit(g_codec, PSP_CODEC_MP3) < 0) { sceAudiocodecReleaseEDRAM(g_codec); return -1; }
    return 0;
}

static int op_audio_decode(void *u, int codec, const uint8_t *frame, uint32_t bytes, int16_t *stereo, uint32_t max_frames)
{
    uint32_t samples;
    (void)u;
    if (codec != PSPEXP_CODEC_MP3 || bytes < 4 || bytes > sizeof(g_src)) return -1;
    {   /* a new format (the next track): the codec set up again; PPSSPP's crashed on a change of rate or channels */
        int fmt = (frame[1] & 0x18) << 8 | (frame[2] & 0x0C) << 4 | (frame[3] >> 6);
        if (g_codec_state > 0 && fmt != g_codec_format) { sceAudiocodecReleaseEDRAM(g_codec); g_codec_state = 0; }
        if (!g_codec_state) { g_codec_state = codec_init() == 0 ? 1 : -1; g_codec_format = fmt; }
    }
    if (g_codec_state < 0) return -1;
    samples = ((frame[1] >> 3) & 3) == 3 ? 1152u : 576u;   /* MPEG-1, or 2 / 2.5 */
    if (samples > max_frames) return -1;
    memcpy(g_src, frame, bytes);
    sceKernelDcacheWritebackInvalidateRange(g_src, sizeof(g_src));
    g_codec[6] = (unsigned long)g_src;
    g_codec[7] = bytes;
    g_codec[8] = (unsigned long)g_dec;
    g_codec[9] = sizeof(g_dec);
    g_codec[10] = bytes;
    if (sceAudiocodecDecode(g_codec, PSP_CODEC_MP3) < 0) return -1;
    sceKernelDcacheWritebackInvalidateRange(g_dec, sizeof(g_dec));
    memcpy(stereo, g_dec, samples * 4u);
    return (int)samples;
}

void pspexp_audio_psp_ops(PspExpRuntimeOps *ops)
{
    ops->audio_decode = op_audio_decode;
    ops->audio_start = op_audio_start;
    ops->audio_stop = op_audio_stop;
    ops->audio_lock = op_audio_lock;
    ops->audio_unlock = op_audio_unlock;
}
