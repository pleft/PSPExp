/*
 * PSPExp Capture: a kernel plugin for custom firmware (PRO/ARK: ms0:/seplugins/game.txt) that
 * sends a native PSP game's picture over USB to a PSPExp receiver (tools/pspexp-capture), so a
 * board or a computer can record it: its picture and, from stage 2, its sound.
 *
 * A watcher thread looks at the frame being shown once per screen refresh (sceDisplayGetFrameBuf)
 * and, when the game has put up a new one, has it copied out band by band by the DMA engine
 * (copy_thread) while a sender thread writes the bands to the USB link; frames that come while
 * one is on its way are skipped. It
 * also counts the game's own frame changes (its frame rate, with and without a receiver), for
 * measuring what the capture costs it. Its log: ms0:/seplugins/pspexp_capture.log. The game's exit
 * is caught so the plugin stops before the system restarts (cap_shutdown). What was tried and
 * measured: docs/PSPExp_Technical_Architecture.md section 12b.
 *
 * The USB link is PSPExp's own (psp/transport-usb, protocol/pspexp_usb_link.h), built in here
 * rather than loaded as a module: one file to install, and nothing else of PSPExp needed on the
 * PSP while a game runs.
 *
 * Each frame on the link: a 32-byte header, then height rows of width pixels in the game's own
 * format (width: VRAM's stride, 512; the screen is its first 480):
 *   "PXCF", u16 width, u16 height, u16 format (0 565, 1 5551, 2 4444, 3 8888), u16 reserved,
 *   u32 sequence, u32 frame changes so far, u32 time shown (us), u32 payload bytes, u32 reserved[2]
 * and each block of sound the game put out on one of its channels, as it gave it (16-bit, mono or
 * stereo interleaved), mixed by the receiver:
 *   "PXCA", u8 kind (0 a channel, 1 output2, 2 SRC), u8 channel, u16 channels (1 or 2), u32 rate,
 *   u16 left volume, u16 right volume (0x8000 = full), u32 samples, u32 time (us), u32 payload
 *   bytes, u32 sequence
 */
#define PSPEXP_USB_EMBEDDED
#define PSPEXP_USB_RX_SIZE 4096   /* it receives only the receiver's hello and short commands */
#include "../transport-usb/main.c"

#include <pspdisplay.h>
#include <pspdisplay_kernel.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <pspdmac.h>
#include <pspsysmem_kernel.h>
#include <psputilsforkernel.h>
#include <psppower.h>
#include <pspaudio.h>
#include <pspctrl.h>
#include <psploadexec_kernel.h>
#include <string.h>
#include <stdarg.h>

PSP_MODULE_INFO("PSPExpCapture", PSP_MODULE_KERNEL, 1, 0);
PSP_NO_CREATE_MAIN_THREAD();

#define CAP_W 480
#define CAP_H 272
#define LOG_PATH "ms0:/seplugins/pspexp_capture.log"


/* the log's own small formatter (the kernel's libc has no vsnprintf): %s %d %u %x (as %08x) */
static int fmt_num(char *p, u32 v, int base, int width)
{
    char t[12];
    int n = 0, i;
    do { t[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    while (n < width) t[n++] = '0';
    for (i = 0; i < n; i++) p[i] = t[n - 1 - i];
    return n;
}

static void cap_log(const char *fmt, ...)
{
    char line[160];
    int n = 0;
    va_list ap;
    SceUID fd;
    va_start(ap, fmt);
    for (; *fmt && n < (int)sizeof(line) - 14; fmt++) {
        if (*fmt != '%') { line[n++] = *fmt; continue; }
        switch (*++fmt) {
        case 's': { const char *a = va_arg(ap, const char *); while (*a && n < (int)sizeof(line) - 14) line[n++] = *a++; break; }
        case 'd': { int v = va_arg(ap, int); if (v < 0) { line[n++] = '-'; v = -v; } n += fmt_num(line + n, (u32)v, 10, 1); break; }
        case 'u': n += fmt_num(line + n, va_arg(ap, u32), 10, 1); break;
        case 'x': n += fmt_num(line + n, va_arg(ap, u32), 16, 8); break;
        default: line[n++] = *fmt; break;
        }
        if (!*fmt) break;
    }
    va_end(ap);
    line[n++] = '\n';
    fd = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
}

static u32 now_us(void)
{
    return (u32)sceKernelGetSystemTimeLow();
}

static void put16(unsigned char *p, u32 v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put32(unsigned char *p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }
static u32 get32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

/* The frame: right after the game puts up a new one, the DMA engine copies it out of VRAM in bands
 * of rows, as it is (the game's own format, rows of its stride), into a ring of small buffers in
 * kernel memory, and the sender writes each band to the link while the next is copied; the CPU
 * does nothing to the pixels (the receiver converts them). The copy must end before the game
 * flips again (it then draws into this buffer): the ring keeps it at the link's pace, ≈22 ms for
 * a 32-bit frame; a frame flipped before its copy ended is counted as late in the log.
 *
 * What came before (2026-10-23, Burnout Legends): rows copied while sending tore (v1); a whole
 * 32-bit snapshot (544 KB) fits nowhere that is free: in the game's memory it hung loading a saved
 * profile (the PSP's save screen takes its memory there), in the 4 MB volatile area it froze on
 * the splash screen; a 16-bit copy in kernel memory, packed by the VFPU and sent while copied,
 * took every frame but cost a race a third of its frame rate (30 fps alone, 20 captured: 27 ms of
 * every frame); the USB engine reading VRAM itself hung the plugin at the first frame. */
#define BAND_BYTES (32 * 1024)       /* 16 rows of a 512-wide 32-bit frame */
#define NBANDS 6                     /* 192 KB of the kernel's ~450 free while a game runs */
static unsigned char *g_bands;       /* 64-byte aligned */
static volatile u32 g_band_len[NBANDS];
static SceUID g_band_free = -1, g_band_full = -1, g_copy_go = -1;
static SceUID g_block = -1;
static volatile int g_busy;            /* a frame is being copied and sent */
static const unsigned char *g_frame_src;
static int g_frame_bw, g_frame_fmt;
static u32 g_frame_seq, g_frame_changes, g_frame_time;
static SceUID g_send_ev = -1;          /* wakes the sender */
static SceUID g_tx_lock = -1;          /* one packet at a time on the link: the sender's, a reply */
#define SEND_FRAME 1
#define SEND_AUDIO 2
#define SEND_QUIT  4
static volatile int g_quit;   /* the game is ending */
static void catch_exit(void);
static void audio_drop_all(void);
static int audio_send(void);
static SceUID g_cap_th = -1, g_send_th = -1, g_copy_th = -1, g_power_th = -1, g_power_cb = -1;
static int g_power_slot = -1;
static int g_usb_up;
static volatile u32 g_changes, g_sent, g_skipped, g_copy_us, g_dma_us, g_send_us, g_late;
static volatile int g_dead_gen = -1;     /* the link the receiver stopped reading (its generation) */

static int snap_alloc(void)
{
    SceUID b;
    cap_log("kernel memory: %u KB free, largest %u KB", (u32)sceKernelPartitionTotalFreeMemSize(1) / 1024,
            (u32)sceKernelPartitionMaxFreeMemSize(1) / 1024);
    b = sceKernelAllocPartitionMemory(1, "PSPExpCapture", PSP_SMEM_High, NBANDS * BAND_BYTES + 64, NULL);
    if (b < 0) return -1;
    g_block = b;
    g_bands = (unsigned char *)(((u32)sceKernelGetBlockHeadAddr(b) + 63) & ~63u);
    sceKernelDcacheInvalidateRange(g_bands, NBANDS * BAND_BYTES);   /* (only the DMA engines touch it) */
    g_band_free = sceKernelCreateSema("PSPExpCaptureFree", 0, NBANDS, NBANDS, NULL);
    g_band_full = sceKernelCreateSema("PSPExpCaptureFull", 0, 0, NBANDS, NULL);
    g_copy_go = sceKernelCreateSema("PSPExpCaptureCopy", 0, 0, 1, NULL);
    return NBANDS * BAND_BYTES / 1024;
}

/* a semaphore taken, waiting in steps so a game's end is noticed; <0 then */
static int take(SceUID sema)
{
    while (!g_quit) {
        SceUInt t = 200000;
        if (sceKernelWaitSema(sema, 1, &t) >= 0) return 0;
    }
    return -1;
}

/* the copier: each frame the watcher hands it, band by band into the ring */
static int copy_thread(SceSize args, void *argp)
{
    while (take(g_copy_go) == 0) {
        int bpp = g_frame_fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2;
        int stride = g_frame_bw * bpp, rows = BAND_BYTES / stride, y, k = 0;
        u32 t0 = now_us(), changes = g_changes;
        for (y = 0; y < CAP_H; y += rows, k++) {
            int n = CAP_H - y < rows ? CAP_H - y : rows, slot = k % NBANDS;
            u32 t;
            if (take(g_band_free) < 0) return 0;
            t = now_us();
            sceDmacMemcpy(g_bands + slot * BAND_BYTES, g_frame_src + y * stride, n * stride);
            g_dma_us += now_us() - t;
            g_band_len[slot] = n * stride;
            sceKernelSignalSema(g_band_full, 1);
        }
        g_copy_us += now_us() - t0;
        if (g_changes != changes) g_late++;   /* the game flipped before the copy ended: maybe torn */
    }
    return 0;
}

/* ---- the sound ----
 * Each block a game puts out (sceAudio: its eight channels, output2, the SRC channel) is caught on
 * its way in (the syscalls redirected, as the exit below) and copied, with its channel, volume and
 * time, into a ring the sender empties onto the link; the receiver mixes the channels, as the
 * PSP's hardware does. The ring is the kernel's memory; a block that does not fit is dropped (a
 * gap in the clip, counted in the log). Caught from the plugin's start, before the game reserves
 * its channels (their sizes and formats are learnt there). */
#define AUDIO_RING (96 * 1024)
static unsigned char *g_aring;
static volatile u32 g_ahead, g_atail;  /* bytes written and sent so far (the ring: their difference) */
static volatile u32 g_aseq, g_ablocks, g_abytes, g_adropped, g_aunknown, g_akinds;
static struct { u16 samples, chans; } g_ch[8];
static u16 g_o2_samples;
static u16 g_src_samples, g_src_chans;
static u32 g_src_rate;

static void ring_put(const void *p, u32 n)
{
    u32 at = g_ahead % AUDIO_RING, first = AUDIO_RING - at < n ? AUDIO_RING - at : n;
    memcpy(g_aring + at, p, first);
    if (n > first) memcpy(g_aring, (const unsigned char *)p + first, n - first);
    g_ahead += n;
}

/* one block into the ring (in the game's call, after the hardware took it) */
static void audio_put(int kind, int ch, int chans, u32 rate, int lvol, int rvol, u32 samples, const void *buf)
{
    unsigned char h[32];
    u32 n = samples * chans * 2, intr;
    int k1;
    if (!g_aring || !buf || !pspexpUsbConnected() || g_quit) return;
    if (!samples) { g_aunknown++; return; }
    k1 = pspSdkSetK1(0);
    memcpy(h, "PXCA", 4);
    h[4] = kind; h[5] = ch; put16(h + 6, chans); put32(h + 8, rate);
    put16(h + 12, lvol); put16(h + 14, rvol); put32(h + 16, samples); put32(h + 20, now_us());
    put32(h + 24, n);
    intr = pspSdkDisableInterrupts();   /* (several game threads put sound out) */
    if (AUDIO_RING - (g_ahead - g_atail) < 32 + n) {
        g_adropped++;
    } else {
        put32(h + 28, g_aseq++);
        ring_put(h, 32);
        ring_put(buf, n);
        g_ablocks++; g_abytes += n; g_akinds |= 1u << kind;
    }
    pspSdkEnableInterrupts(intr);
    sceKernelSetEventFlag(g_send_ev, SEND_AUDIO);
    pspSdkSetK1(k1);
}

/* the ring onto the link (the sender's); <0: the receiver is not reading */
static int audio_send(void)
{
    u32 head = g_ahead, tail = g_atail, n = head - tail, at, first;
    if (!n) return 0;
    at = tail % AUDIO_RING;
    first = AUDIO_RING - at < n ? AUDIO_RING - at : n;
    if (pspexpUsbWrite(g_aring + at, first) < 0) return -1;
    if (n > first && pspexpUsbWrite(g_aring, n - first) < 0) return -1;
    g_atail = head;
    return 0;
}

static void audio_drop_all(void) { g_atail = g_ahead; }

static int (*o_out)(int, int, void *), (*o_out_b)(int, int, void *);
static int (*o_pan)(int, int, int, void *), (*o_pan_b)(int, int, int, void *);
static int (*o_reserve)(int, int, int), (*o_datalen)(int, int), (*o_config)(int, int);
static int (*o_o2_reserve)(int), (*o_o2_length)(int), (*o_o2_out)(int, void *);
static int (*o_src_reserve)(int, int, int), (*o_src_out)(int, void *);

static void ch_put(int ch, int l, int r, void *buf)
{
    if (ch >= 0 && ch < 8) audio_put(0, ch, g_ch[ch].chans, 44100, l, r, g_ch[ch].samples, buf);
}
static int h_out(int ch, int vol, void *buf) { int r = o_out(ch, vol, buf); if (r >= 0) ch_put(ch, vol, vol, buf); return r; }
static int h_out_b(int ch, int vol, void *buf) { int r = o_out_b(ch, vol, buf); if (r >= 0) ch_put(ch, vol, vol, buf); return r; }
static int h_pan(int ch, int l, int rv, void *buf) { int r = o_pan(ch, l, rv, buf); if (r >= 0) ch_put(ch, l, rv, buf); return r; }
static int h_pan_b(int ch, int l, int rv, void *buf) { int r = o_pan_b(ch, l, rv, buf); if (r >= 0) ch_put(ch, l, rv, buf); return r; }
static int h_reserve(int ch, int samples, int format)
{
    int r = o_reserve(ch, samples, format);
    if (r >= 0 && r < 8) { g_ch[r].samples = samples; g_ch[r].chans = format == PSP_AUDIO_FORMAT_MONO ? 1 : 2; }
    return r;
}
static int h_datalen(int ch, int samples) { int r = o_datalen(ch, samples); if (r >= 0 && ch >= 0 && ch < 8) g_ch[ch].samples = samples; return r; }
static int h_config(int ch, int format)
{
    int r = o_config(ch, format);
    if (r >= 0 && ch >= 0 && ch < 8) g_ch[ch].chans = format == PSP_AUDIO_FORMAT_MONO ? 1 : 2;
    return r;
}
static int h_o2_reserve(int samples) { int r = o_o2_reserve(samples); if (r >= 0) g_o2_samples = samples; return r; }
static int h_o2_length(int samples) { int r = o_o2_length(samples); if (r >= 0) g_o2_samples = samples; return r; }
static int h_o2_out(int vol, void *buf) { int r = o_o2_out(vol, buf); if (r >= 0) audio_put(1, 0, 2, 44100, vol, vol, g_o2_samples, buf); return r; }
static int h_src_reserve(int samples, int rate, int chans)
{
    int r = o_src_reserve(samples, rate, chans);
    if (r >= 0) { g_src_samples = samples; g_src_rate = rate ? rate : 44100; g_src_chans = chans == 1 ? 1 : 2; }
    return r;
}
static int h_src_out(int vol, void *buf)
{
    int r = o_src_out(vol, buf);
    if (r >= 0) audio_put(2, 0, g_src_chans, g_src_rate, vol, vol, g_src_samples, buf);
    return r;
}

u32 sctrlHENFindFunction(char *modname, char *libname, u32 nid);
void sctrlHENPatchSyscall(void *addr, void *newaddr);

/* a user syscall of the firmware's, through the plugin: its own address kept in *orig */
static int redirect(const char *mod, const char *lib, u32 nid, void *hook, void *orig)
{
    void *f = (void *)sctrlHENFindFunction((char *)mod, (char *)lib, nid);
    if (!f) return 0;
    *(void **)orig = f;
    sctrlHENPatchSyscall(f, hook);
    return 1;
}

static void catch_audio(void)
{
    int n = 0;
    SceUID b = sceKernelAllocPartitionMemory(1, "PSPExpCaptureSound", PSP_SMEM_High, AUDIO_RING + 64, NULL);
    if (b < 0) { cap_log("sound: no memory for its ring (%x)", (u32)b); return; }
    g_aring = (unsigned char *)(((u32)sceKernelGetBlockHeadAddr(b) + 63) & ~63u);
    n += redirect("sceAudio_Driver", "sceAudio", 0x5EC81C55, h_reserve, &o_reserve);      /* sceAudioChReserve */
    n += redirect("sceAudio_Driver", "sceAudio", 0xCB2E439E, h_datalen, &o_datalen);      /* ...SetChannelDataLen */
    n += redirect("sceAudio_Driver", "sceAudio", 0x95FD0C2D, h_config, &o_config);        /* ...ChangeChannelConfig */
    n += redirect("sceAudio_Driver", "sceAudio", 0x8C1009B2, h_out, &o_out);              /* sceAudioOutput */
    n += redirect("sceAudio_Driver", "sceAudio", 0x136CAF51, h_out_b, &o_out_b);          /* ...OutputBlocking */
    n += redirect("sceAudio_Driver", "sceAudio", 0xE2D56B2D, h_pan, &o_pan);              /* ...OutputPanned */
    n += redirect("sceAudio_Driver", "sceAudio", 0x13F592BC, h_pan_b, &o_pan_b);          /* ...OutputPannedBlocking */
    n += redirect("sceAudio_Driver", "sceAudio", 0x01562BA3, h_o2_reserve, &o_o2_reserve); /* sceAudioOutput2Reserve */
    n += redirect("sceAudio_Driver", "sceAudio", 0x63F2889C, h_o2_length, &o_o2_length);  /* ...ChangeLength */
    n += redirect("sceAudio_Driver", "sceAudio", 0x2D53F36E, h_o2_out, &o_o2_out);        /* ...OutputBlocking */
    n += redirect("sceAudio_Driver", "sceAudio", 0x38553111, h_src_reserve, &o_src_reserve); /* sceAudioSRCChReserve */
    n += redirect("sceAudio_Driver", "sceAudio", 0xE0727056, h_src_out, &o_src_out);      /* ...SRCOutputBlocking */
    cap_log("sound: %d of 12 calls caught, a %u KB ring", n, AUDIO_RING / 1024);
}

/* ---- control from the receiver: testing without hands on the PSP ----
 * The receiver can send commands on the same link: buttons pressed for the game (its pad reads
 * caught as the sound calls are), a file written into ms0:/seplugins/ (a new plugin), the log
 * sent back, the game restarted from its ISO (the new plugin loads with it). Each command:
 *   "PXCC", u32 command, u32 payload bytes, then the payload
 * Replies, as the frames: a 32-byte header ("PXCR" result or "PXCL" the log; u32 command at 4,
 * i32 result at 8, u32 payload bytes at 24), then the payload. */
#define CMD_PAD 1        /* u32 buttons (PSP_CTRL_*), u32 ms (0: until the next) */
#define CMD_PUT 2        /* char name[32] (a file in ms0:/seplugins/), then its bytes */
#define CMD_LOG 3
#define CMD_RESTART 4
#define CMD_PAUSE 5      /* u32 1: no frames copied (the game's own frame rate, for comparing), 0: again */
static volatile int g_paused;
static volatile u32 g_pad_buttons, g_pad_until, g_pad_reads;
static SceUID g_cmd_th = -1;

static void pad_apply(SceCtrlData *pad, int n, int negative)
{
    u32 b = g_pad_buttons;
    int i;
    g_pad_reads++;
    if (!b || n <= 0) return;
    if (g_pad_until && (int)(now_us() - g_pad_until) >= 0) { g_pad_buttons = 0; return; }
    for (i = 0; i < n; i++) {
        if (negative) pad[i].Buttons &= ~b;
        else pad[i].Buttons |= b;
    }
}
static int (*o_rbp)(SceCtrlData *, int), (*o_pbp)(SceCtrlData *, int), (*o_rbn)(SceCtrlData *, int), (*o_pbn)(SceCtrlData *, int);
static int h_rbp(SceCtrlData *p, int n) { int r = o_rbp(p, n); pad_apply(p, r, 0); return r; }
static int h_pbp(SceCtrlData *p, int n) { int r = o_pbp(p, n); pad_apply(p, r, 0); return r; }
static int h_rbn(SceCtrlData *p, int n) { int r = o_rbn(p, n); pad_apply(p, r, 1); return r; }
static int h_pbn(SceCtrlData *p, int n) { int r = o_pbn(p, n); pad_apply(p, r, 1); return r; }

static void catch_pad(void)
{
    int n = 0;
    n += redirect("sceController_Service", "sceCtrl", 0x1F803938, h_rbp, &o_rbp);   /* sceCtrlReadBufferPositive */
    n += redirect("sceController_Service", "sceCtrl", 0x3A622550, h_pbp, &o_pbp);   /* ...PeekBufferPositive */
    n += redirect("sceController_Service", "sceCtrl", 0x60B81F86, h_rbn, &o_rbn);   /* ...ReadBufferNegative */
    n += redirect("sceController_Service", "sceCtrl", 0xC152080A, h_pbn, &o_pbn);   /* ...PeekBufferNegative */
    cap_log("buttons: %d of 4 calls caught", n);
}

static unsigned char g_cmd_buf[4096] __attribute__((aligned(64)));

/* exactly n bytes of a command; <0 when the receiver went or the game ends */
static int cmd_read(void *buf, int n)
{
    int got = 0, r;
    while (got < n) {
        if (g_quit) return -1;
        r = pspexpUsbRead((unsigned char *)buf + got, n - got, 500000);
        if (r < 0) return -1;
        got += r;
    }
    return 0;
}

static void cmd_reply(const char *magic, u32 cmd, int result, const void *data, u32 len)
{
    unsigned char h[32];
    memset(h, 0, 32);
    memcpy(h, magic, 4);
    put32(h + 4, cmd); put32(h + 8, (u32)result); put32(h + 20, now_us()); put32(h + 24, len);
    sceKernelWaitSema(g_tx_lock, 1, NULL);
    if (pspexpUsbWrite(h, 32) >= 0 && len) pspexpUsbWrite(data, len);
    sceKernelSignalSema(g_tx_lock, 1);
}

/* a file into ms0:/seplugins/: written beside, then put in place */
static int cmd_put(u32 len)
{
    char name[33], path[80], tmp[84];
    SceUID fd;
    u32 left;
    int i, r = 0;
    if (len < 32 || cmd_read(name, 32) < 0) return -1;
    name[32] = 0;
    for (i = 0; name[i]; i++)   /* a plain name only */
        if (name[i] == '/' || name[i] == ':' || name[i] == '\\' || (name[i] == '.' && name[i + 1] == '.')) return -2;
    if (!name[0]) return -2;
    memcpy(path, "ms0:/seplugins/", 15); memcpy(path + 15, name, i + 1);   /* (memcpy: the kernel's libc has no stpcpy, which gcc makes of strcpy+strcat) */
    memcpy(tmp, path, 15 + i); memcpy(tmp + 15 + i, ".new", 5);
    fd = sceIoOpen(tmp, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    for (left = len - 32; left > 0; ) {
        int n = left > sizeof(g_cmd_buf) ? (int)sizeof(g_cmd_buf) : (int)left;
        if (cmd_read(g_cmd_buf, n) < 0) { if (fd >= 0) sceIoClose(fd); return -1; }
        if (fd >= 0 && sceIoWrite(fd, g_cmd_buf, n) != n) r = -3;
        left -= n;
    }
    if (fd < 0) return fd;
    sceIoClose(fd);
    if (r < 0) return r;
    sceIoRemove(path);
    r = sceIoRename(tmp, path);
    cap_log("written: %s (%u bytes): %x", path, len - 32, (u32)r);
    return r;
}

static void cmd_log(void)
{
    SceUID fd = sceIoOpen(LOG_PATH, PSP_O_RDONLY, 0);
    int n = fd >= 0 ? sceIoRead(fd, g_cmd_buf, sizeof(g_cmd_buf)) : 0;
    int size = fd >= 0 ? (int)sceIoLseek32(fd, 0, PSP_SEEK_END) : 0;
    if (fd >= 0) sceIoClose(fd);
    if (n < 0) n = 0;
    if (size > n) {   /* the log's end: the newest lines */
        fd = sceIoOpen(LOG_PATH, PSP_O_RDONLY, 0);
        if (fd >= 0) { sceIoLseek32(fd, size - (int)sizeof(g_cmd_buf), PSP_SEEK_SET); n = sceIoRead(fd, g_cmd_buf, sizeof(g_cmd_buf)); sceIoClose(fd); }
    }
    cmd_reply("PXCL", CMD_LOG, size, g_cmd_buf, n > 0 ? (u32)n : 0);
}

u32 sctrlKernelLoadExecVSHWithApitype(int apitype, const char *file, struct SceKernelLoadExecVSHParam *param);
char *sctrlSEGetUmdFile(void);
void sctrlSESetUmdFile(char *file);
int sctrlSEGetBootConfFileIndex(void);
void sctrlSESetBootConfFileIndex(int index);
static void cap_shutdown(const char *why);

/* the running game started again from its ISO, as PRO's own menu starts one (Vshctrl
 * virtual_pbp.c): the ISO and the driver mode stay as they are */
static int cmd_restart(void)
{
    static char iso[256];
    static SceKernelLoadExecVSHParam param;
    static const char boot[] = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    char *f = sctrlSEGetUmdFile();
    int mode = sctrlSEGetBootConfFileIndex();
    if (!f || !f[0]) return -1;   /* not from an ISO */
    strncpy(iso, f, sizeof(iso) - 1);
    cap_log("restarting %s (mode %d)", iso, mode);
    cmd_reply("PXCR", CMD_RESTART, 0, NULL, 0);
    cap_shutdown("the game restarts");
    sctrlSESetUmdFile(iso);
    sctrlSESetBootConfFileIndex(mode);
    memset(&param, 0, sizeof(param));
    param.size = sizeof(param);
    param.argp = (void *)boot;
    param.args = sizeof(boot);
    param.key = mode == 1 ? "game" : "umdemu";   /* (1: March33) */
    return (int)sctrlKernelLoadExecVSHWithApitype(0x120, boot, &param);
}

static int cmd_thread(SceSize args, void *argp)
{
    unsigned char h[12];
    while (!g_quit) {
        u32 cmd, len;
        if (!pspexpUsbConnected()) { sceKernelDelayThread(100000); continue; }
        if (cmd_read(h, 4) < 0) { sceKernelDelayThread(100000); continue; }
        if (memcmp(h, "PXCC", 4) != 0) continue;   /* (out of step: the next one) */
        if (cmd_read(h + 4, 8) < 0) continue;
        cmd = get32(h + 4); len = get32(h + 8);
        if (cmd == CMD_PAD && len >= 8 && cmd_read(g_cmd_buf, 8) == 0) {
            u32 ms = get32(g_cmd_buf + 4);
            g_pad_until = ms ? now_us() + ms * 1000 : 0;
            g_pad_buttons = get32(g_cmd_buf);
        } else if (cmd == CMD_PUT) {
            cmd_reply("PXCR", CMD_PUT, cmd_put(len), NULL, 0);
        } else if (cmd == CMD_LOG) {
            cmd_log();
        } else if (cmd == CMD_PAUSE && len >= 4 && cmd_read(g_cmd_buf, 4) == 0) {
            g_paused = get32(g_cmd_buf) != 0;
            cap_log(g_paused ? "paused" : "capturing again");
        } else if (cmd == CMD_RESTART) {
            cmd_reply("PXCR", CMD_RESTART, cmd_restart(), NULL, 0);
        }
    }
    return 0;
}

/* the sender: the sound so far, then the frame when there is one, over the link (the driver cuts
 * it in 64 KB pieces). A write that times out
 * means the receiver went (the cable stays plugged in): nothing more is sent until a receiver
 * syncs again (a new link generation), instead of waiting out a 3 s timeout per frame. */
static unsigned char g_hdr[64] __attribute__((aligned(64)));

static int send_thread(SceSize args, void *argp)
{
    int r, gen;
    for (;;) {
        u32 bits = 0;
        sceKernelWaitEventFlag(g_send_ev, SEND_FRAME | SEND_AUDIO | SEND_QUIT, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &bits, NULL);
        if (g_quit) break;
        gen = (int)g_generation;   /* (a send that fails is this link's: a receiver syncing meanwhile is a new one) */
        if (!pspexpUsbConnected() || gen == g_dead_gen) { audio_drop_all(); g_busy = 0; continue; }
        sceKernelWaitSema(g_tx_lock, 1, NULL);
        r = audio_send();
        sceKernelSignalSema(g_tx_lock, 1);
        if (r < 0) { g_dead_gen = gen; cap_log("receiver not reading: paused until one connects"); g_busy = 0; continue; }
        if (g_busy) {   /* the frame's header, then its bands as the copier fills them */
            unsigned char *h = g_hdr;
            int bpp = g_frame_fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2;
            int rows = BAND_BYTES / (g_frame_bw * bpp), y, k = 0;
            u32 ts = now_us();
            memset(h, 0, 32);
            memcpy(h, "PXCF", 4);
            put16(h + 4, g_frame_bw); put16(h + 6, CAP_H); put16(h + 8, g_frame_fmt);   /* (width: the stride) */
            put32(h + 12, g_frame_seq); put32(h + 16, g_frame_changes); put32(h + 20, g_frame_time);
            put32(h + 24, (u32)g_frame_bw * CAP_H * bpp);
            sceKernelWaitSema(g_tx_lock, 1, NULL);   /* (a command's reply never inside a frame) */
            r = pspexpUsbWrite(h, 32) >= 0;
            for (y = 0; y < CAP_H; y += rows, k++) {   /* every band taken, sent or not, so the copier goes on */
                int slot = k % NBANDS;
                if (take(g_band_full) < 0) break;
                if (r) r = pspexpUsbWrite(g_bands + slot * BAND_BYTES, g_band_len[slot]) >= 0;
                sceKernelSignalSema(g_band_free, 1);
            }
            sceKernelSignalSema(g_tx_lock, 1);
            if (r) { g_sent++; g_send_us += now_us() - ts; }
            else { g_dead_gen = gen; cap_log("receiver not reading: paused until one connects"); }
            g_busy = 0;
        }
    }
    return 0;
}

/* the PSP's power events in the log (a PSP found switched off after a capture, 2026-10-23: the log
 * stopped 30 s after the receiver did, nothing wrong before it; asleep, or switched off?) */
static int power_cb(int unknown, int flags, void *arg)
{
    if (flags & PSP_POWER_CB_POWER_SWITCH) cap_log("power: the power switch");
    if (flags & PSP_POWER_CB_HOLD_SWITCH) cap_log("power: the hold switch");
    if (flags & PSP_POWER_CB_STANDBY) cap_log("power: standby (switching off)");
    if (flags & PSP_POWER_CB_SUSPENDING) cap_log("power: going to sleep");
    if (flags & PSP_POWER_CB_RESUME_COMPLETE) cap_log("power: woke up");
    if (flags & PSP_POWER_CB_BATTERY_LOW) cap_log("power: battery low");
    return 0;
}

static int power_thread(SceSize args, void *argp)
{
    int slot, r = -1;
    g_power_cb = sceKernelCreateCallback("PSPExpCapturePower", power_cb, NULL);
    for (slot = 15; slot >= 0 && r < 0; slot--)   /* (-1, any free slot, is refused in kernel mode: 80000004) */
        if ((r = scePowerRegisterCallback(slot, g_power_cb)) >= 0) g_power_slot = slot;
    cap_log("power callback: %x in slot %d", (u32)r, g_power_slot);
    while (!g_quit) sceKernelDelayThreadCB(200000);
    return 0;
}

/* the watcher: every screen refresh, the frame shown; a new one counted (the game's frame rate)
 * and, when none is on its way and a receiver is there, handed to the copier and the sender */
static int capture_thread(SceSize args, void *argp)
{
    void *top = NULL, *last = NULL;
    int bw = 0, fmt = 0, r, was_connected = 0, part;
    u32 seq = 0, t0, c0 = 0, s0 = 0, k0 = 0, o0 = 0, q0 = 0, u0 = 0, d0 = 0, ab0 = 0, ad0 = 0, au0 = 0, se0 = 0, l0 = 0;

    sceKernelDelayThread(2000000);   /* the game first: its modules, its first frames */
    if (g_quit) return 0;
    part = snap_alloc();
    cap_log("frame ring: %d KB of kernel memory", part);
    if (part < 0) return 0;
    g_copy_th = sceKernelCreateThread("PSPExpCaptureCopy", copy_thread, 0x19, 0x2000, 0, NULL);
    if (g_copy_th >= 0) sceKernelStartThread(g_copy_th, 0, NULL);
    {   /* the sender just below the watcher (at 0x60, below the game's threads, a race got 14 frames
         * of 28 a second sent; 0x1A, 14-23; above the game's threads, 0x11 with the watcher at
         * 0x10, no more frames and the game itself slower, 20-25 fps: 2026-10-23) */
        g_send_th = sceKernelCreateThread("PSPExpCaptureSend", send_thread, 0x1A, 0x4000, 0, NULL);
        if (g_send_th >= 0) sceKernelStartThread(g_send_th, 0, NULL);
    }
    {
        g_power_th = sceKernelCreateThread("PSPExpCapturePower", power_thread, 0x60, 0x1000, 0, NULL);
        if (g_power_th >= 0) sceKernelStartThread(g_power_th, 0, NULL);
    }
    cap_log("battery %d%%", scePowerGetBatteryLifePercent());
    memset(&g_stats, 0, sizeof(g_stats));
    r = sceUsbbdRegister(&g_driver);
    cap_log("USB driver registered: %x", r);
    r = pspexpUsbStart();
    cap_log("USB started: %x", r);
    if (r < 0) return 0;
    g_usb_up = 1;
    g_cmd_th = sceKernelCreateThread("PSPExpCaptureCmd", cmd_thread, 0x30, 0x2000, 0, NULL);
    if (g_cmd_th >= 0) sceKernelStartThread(g_cmd_th, 0, NULL);

    t0 = now_us();
    while (!g_quit) {
        sceDisplayWaitVblankStart();
        if (sceDisplayGetFrameBuf(&top, &bw, &fmt, 0) < 0 || !top) continue;
        if (top != last) {
            g_changes++; last = top;
            if (pspexpUsbConnected()) {
                if (!was_connected) { cap_log("receiver connected"); was_connected = 1; }
                if (g_paused || (int)g_generation == g_dead_gen) ;   /* (nobody reading: nothing copied) */
                else if (!g_busy) {   /* to the copier and the sender */
                    g_frame_src = (const unsigned char *)((u32)top & ~0x40000000u);
                    g_frame_bw = bw; g_frame_fmt = fmt;
                    g_frame_seq = seq++; g_frame_changes = g_changes; g_frame_time = now_us();
                    g_busy = 1;
                    sceKernelSignalSema(g_copy_go, 1);
                    sceKernelSetEventFlag(g_send_ev, SEND_FRAME);
                } else g_skipped++;
            } else if (was_connected) { cap_log("receiver gone"); was_connected = 0; }
        }
        if (now_us() - t0 >= 5000000) {   /* every 5 s: the game's frame rate, and what went out */
            u32 ms = (now_us() - t0) / 1000;   /* (32-bit: no 64-bit division in the kernel) */
            u32 g10 = (g_changes - c0) * 10000 / ms, s10 = (g_sent - s0) * 10000 / ms;
            u32 copies = seq - q0, copy_us = copies ? (g_copy_us - u0) / copies : 0;   /* the game's cost, per frame copied */
            u32 dma_us = copies ? (g_dma_us - d0) / copies : 0, sends = g_sent - s0, send_us = sends ? (g_send_us - se0) / sends : 0;
            cap_log("game %u.%u fps, sent %u.%u fps, skipped %u, late %u, copy %u us (DMA %u), send %u us, USB timeouts %u, format %d, width %d, %s",
                    g10 / 10, g10 % 10, s10 / 10, s10 % 10, g_skipped - k0, g_late - l0, copy_us, dma_us, send_us, g_stats.tx_timeouts - o0, fmt, bw,
                    was_connected ? "connected" : "no receiver");
            cap_log("sound: %u KB/s, dropped %u, unknown channel %u, kinds %x (1 channels, 2 output2, 4 SRC)",
                    (g_abytes - ab0) / ms, g_adropped - ad0, g_aunknown - au0, g_akinds);
            t0 = now_us(); c0 = g_changes; s0 = g_sent; k0 = g_skipped; o0 = g_stats.tx_timeouts; q0 = seq; u0 = g_copy_us; d0 = g_dma_us;
            ab0 = g_abytes; ad0 = g_adropped; au0 = g_aunknown; se0 = g_send_us; l0 = g_late;
        }
    }
    return 0;
}

int module_start(SceSize args, void *argp)
{
    sceIoRemove(LOG_PATH ".old");   /* the last run's log kept beside (after a hang, what it did last) */
    sceIoRename(LOG_PATH, LOG_PATH ".old");
    cap_log("PSPExp Capture: stage 2 (picture and sound; a DMA ring, 32-bit as the game draws; control from the receiver)");
    g_send_ev = sceKernelCreateEventFlag("PSPExpCaptureSend", 0, 0, NULL);
    g_tx_lock = sceKernelCreateSema("PSPExpCaptureTx", 0, 1, 1, NULL);
    catch_exit();
    catch_audio();   /* now, before the game reserves its channels */
    catch_pad();
    /* the watcher above most of the game's threads, to copy a frame the moment it is shown; it does
     * little: one look per refresh, and the DMA engine copies while it waits */
    g_cap_th = sceKernelCreateThread("PSPExpCapture", capture_thread, 0x18, 0x4000, 0, NULL);
    if (g_cap_th >= 0) sceKernelStartThread(g_cap_th, 0, NULL);
    return 0;
}

/* a thread's end awaited (half a second), else ended */
static void end_thread(SceUID *th)
{
    SceUInt t = 500000;
    if (*th < 0 || *th == sceKernelGetThreadId()) return;   /* (not the one stopping everything) */
    if (sceKernelWaitThreadEnd(*th, &t) < 0) sceKernelTerminateThread(*th);
    sceKernelDeleteThread(*th);
    *th = -1;
}

/* Everything of the plugin stopped and given back, once. A game's exit restarts the PSP's system
 * without stopping plugins first (module_stop is never called: no line of it in the log); with
 * the plugin's USB link and threads still running then, the PSP switched itself off on "Please
 * wait..." every time (v1-v7, 2026-10-23). So the game's exit call is caught (below) and this
 * runs first. */
static void cap_shutdown(const char *why)
{
    static int done;
    if (done) return;
    done = 1;
    cap_log("stopping: %s", why);
    g_quit = 1;
    if (g_send_ev >= 0) sceKernelSetEventFlag(g_send_ev, SEND_QUIT);
    end_thread(&g_cap_th);
    end_thread(&g_send_th);
    end_thread(&g_copy_th);
    end_thread(&g_cmd_th);
    if (g_power_slot >= 0) scePowerUnregisterCallback(g_power_slot);
    end_thread(&g_power_th);
    if (g_power_cb >= 0) sceKernelDeleteCallback(g_power_cb);
    if (g_usb_up) pspexpUsbStop();
    sceUsbbdUnregister(&g_driver);
    if (g_send_ev >= 0) sceKernelDeleteEventFlag(g_send_ev);
    if (g_tx_lock >= 0) sceKernelDeleteSema(g_tx_lock);
    if (g_band_free >= 0) sceKernelDeleteSema(g_band_free);
    if (g_band_full >= 0) sceKernelDeleteSema(g_band_full);
    if (g_copy_go >= 0) sceKernelDeleteSema(g_copy_go);
    if (g_block >= 0) sceKernelFreePartitionMemory(g_block);
    cap_log("stopped");
}

/* the game's exit calls (LoadExecForUser: what a game calls from HOME's Exit), through the plugin */
static int (*g_exit_game)(void);
static int (*g_exit_game_status)(int);
static void exit_shutdown(void)   /* (in the game's own call: kernel rights for the files and USB) */
{
    int k1 = pspSdkSetK1(0);
    cap_shutdown("the game exits");
    pspSdkSetK1(k1);
}
static int exit_game(void) { exit_shutdown(); return g_exit_game(); }
static int exit_game_status(int status) { exit_shutdown(); return g_exit_game_status(status); }

static void catch_exit(void)
{
    g_exit_game = (void *)sctrlHENFindFunction("sceLoadExec", "LoadExecForUser", 0x05572A5F);   /* sceKernelExitGame */
    g_exit_game_status = (void *)sctrlHENFindFunction("sceLoadExec", "LoadExecForUser", 0x2AC9954B);   /* ...WithStatus */
    if (g_exit_game) sctrlHENPatchSyscall((void *)g_exit_game, exit_game);
    if (g_exit_game_status) sctrlHENPatchSyscall((void *)g_exit_game_status, exit_game_status);
    cap_log("game exit caught: %x %x", (u32)g_exit_game, (u32)g_exit_game_status);
}

int module_stop(SceSize args, void *argp)
{
    cap_shutdown("the plugin is unloaded");
    return 0;
}
