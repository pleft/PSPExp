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
#define PSPEXP_USB_TXBUF 16384    /* (its unaligned writes, the sound ring's, go in pieces this size) */
#include "pspexp_usb_link.h"
#define PSPEXP_USB_DEVICE_PID PSPEXP_CAPTURE_USB_PID          /* (its own product: a board's console, waiting */
#define PSPEXP_USB_DEVICE_NAME PSPEXP_CAPTURE_USB_DRIVER_NAME /* for PSPExp, leaves it to the receiver) */
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
#include <pspge.h>
#include <psploadexec_kernel.h>
#include <string.h>
#include <stdarg.h>

/* Never stopped or unloaded by the system: a game's exit stops and unloads the modules it can,
 * and anything still pointing into this one then jumps into freed memory (the PSP switched
 * itself off on exits: v1-v7 with the threads still running, Castlevania: The Dracula X
 * Chronicles later with its exit calls still redirected here, 2026-10-24). The system's
 * restart wipes it instead, as it does psp-cast, a plugin of the same kind (Lucaspcaldeira/
 * psp-cast: PSP_MODULE_INFO attribute 0x1007). */
PSP_MODULE_INFO("PSPExpCapture", PSP_MODULE_KERNEL | PSP_MODULE_NO_STOP | PSP_MODULE_SINGLE_LOAD | PSP_MODULE_SINGLE_START, 1, 0);
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

/* The log: in memory (the newest 8 KB), sent to the receiver between frames ("PXCM"), which keeps
 * it: the plugin never touches the memory stick. A developer build (make DEV=1) also writes it to
 * ms0:/seplugins/pspexp_capture.log, and then only between 2 s after the game's start and the HOME
 * menu or the game's exit; before and after, the lines wait in memory. psp-cast found an sceIoOpen on ms0
 * during the start (module_start or right after) "hung the PSP, always, after exiting a game";
 * this plugin opened and renamed its log in module_start from v10 on, and every exit of
 * Castlevania: The Dracula X Chronicles switched the PSP off afterwards, with the USB stopped or
 * not used at all (2026-10-24); the exits that went well (Burnout Legends, v8-v9) were from before. */
static volatile int g_log_ready, g_log_hold;
#define MLOG 4096
static char g_mlog[MLOG];
static volatile u32 g_mlog_head, g_mlog_sent;   /* bytes logged and sent so far (the ring: their difference) */
#ifdef PSPEXP_CAPTURE_DEV
static char g_log_pend[2048];
static int g_log_pend_n;
#endif

static void cap_log(const char *fmt, ...)
{
    char line[240];
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
    {   /* into the ring for the receiver (the oldest dropped when it is full) */
        u32 intr = pspSdkDisableInterrupts(), i;
        for (i = 0; i < (u32)n; i++) g_mlog[(g_mlog_head + i) % MLOG] = line[i];
        g_mlog_head += n;
        if (g_mlog_head - g_mlog_sent > MLOG) g_mlog_sent = g_mlog_head - MLOG;
        pspSdkEnableInterrupts(intr);
    }
#ifdef PSPEXP_CAPTURE_DEV
    if (!g_log_ready || g_log_hold) {   /* (kept in memory: see above) */
        if (g_log_pend_n + n <= (int)sizeof(g_log_pend)) { memcpy(g_log_pend + g_log_pend_n, line, n); g_log_pend_n += n; }
        return;
    }
    fd = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        if (g_log_pend_n) { sceIoWrite(fd, g_log_pend, g_log_pend_n); g_log_pend_n = 0; }
        sceIoWrite(fd, line, n);
        sceIoClose(fd);
    }
#else
    (void)fd;
#endif
}

static u32 now_us(void)
{
    return (u32)sceKernelGetSystemTimeLow();
}

static void put16(unsigned char *p, u32 v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put32(unsigned char *p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }
static u32 get32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

/* The frame: right after the game puts up a new one, the DMA engine copies it whole out of VRAM,
 * as it is (the game's own format, rows of its stride), into a buffer in main memory (the USB
 * engine reads nothing else), and the sender writes it to the link; the CPU does nothing to the
 * pixels (the receiver converts them). The copy takes a few ms, long before the game draws into
 * that buffer again (after its next flip); one flipped before its copy ended is counted as late.
 *
 * Where the buffers live (the frame's 544 KB, the sound's 64 KB): in the game's own memory
 * (partition 2), where most games leave megabytes free, taken only with UMARGIN still free, and
 * given back at once whenever the game or its system needs memory: a PSP system screen opening
 * (save, keyboard, messages...), a system module loading, or the game's own memory request or
 * module load failing (the plugin then releases its buffers and retries it for the game; see
 * mem_release). Not in the kernel's memory: ≈450 KB free while a game runs, which the system's
 * screens, modules and the game's exit need; holding ≈520 KB of it left Bomberman's exit waiting
 * forever and Gran Turismo frozen (2026-10-24). A game's memory once hung Burnout Legends
 * loading a profile (2026-10-23), from before the plugin gave it back for system screens. */
#define FRAME_MAX (512 * CAP_H * 4)
#define UMARGIN (4 * 1024 * 1024)
static unsigned char *g_fbuf;          /* 64-byte aligned */
static SceUID g_fblock = -1, g_copy_go = -1, g_copied = -1;
static volatile u32 g_frame_bytes;
static volatile int g_frame_mode;      /* the frame under way: 1 and 3 whole, 2 in bands (g_mem_held when handed) */
static volatile int g_mem_held;        /* the buffers taken (frame and sound): 1 the game's memory, 2 the kernel's bands, 3 the volatile memory */
/* The fallback, for games that leave too little of their memory free (Jeanne d'Arc 497 KB, Crisis
 * Core 15-40 KB, Phantasy Star Portable 2 1.4 MB, 2026-10-24): bands of 32 KB, as many as the
 * frame needs (9 for a 16-bit one) and fit with KMARGIN of the kernel's memory left free (≈500 KB
 * is free in Jeanne d'Arc), and the sound's 16 KB; the DMA copies the frame band by band, while
 * the sender sends the bands before. A band copied after the swap's first moments can catch the
 * game drawing that buffer again: with 5 bands (KMARGIN 300 KB), the last 4 of Jeanne d'Arc's
 * frames, copied 1.5-6 ms after the swap as the sending freed bands, came black or half drawn
 * in some scenes; with 2, the copy followed the sending, ≈27 ms a frame. The margin: Bomberman's
 * exit hung with 43 KB left (2026-10-24); the bands are given back for the HOME menu (an exit
 * may follow) and every system screen. */
#define BAND 32768
#define MAXBAND 17   /* (a 32-bit frame) */
#define KMARGIN (192 * 1024)
static SceUID g_band_free = -1, g_band_full = -1;
static volatile u32 g_band_len[MAXBAND];
static int g_nband;                    /* the bands held */
static volatile u32 g_mem_releases;
static volatile int g_busy;            /* a frame on its way (copied by the copier, sent by the sender) */
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
static SceUID g_cap_th = -1, g_send_th = -1, g_copy_th = -1;
static int g_usb_up;
static volatile u32 g_changes, g_sent, g_skipped, g_copy_us, g_dma_us, g_send_us, g_late;
static volatile int g_dead_gen = -1;     /* the link the receiver stopped reading (its generation) */
static volatile int g_paused;            /* pause: no frames copied (for comparing) */
static int g_frames_on = 1;              /* the frames part (F, see read_parts) */
static u32 g_last_hand;                  /* when the watcher last handed a frame on */
static volatile u32 g_static;            /* frames not sent: the same as the last one sent */

/* ---- on the PSP's screen: a red dot while recording, a green one for a second for a clip ----
 * Drawn into the frame the game shows, in its bottom-right corner, once that frame has been
 * copied: the PSP's screen is drawn top to bottom over ≈16 ms and the copy takes a few, so the dot
 * shows from the frame's first showing and is never in the recording (the frame checks below
 * skip its corner). The pixels under it are kept: a buffer the game shows again without drawing
 * it anew has them back before it is looked at or copied, and so when the dot goes. Replaces the
 * beeps and the WLAN light of the first version (2026-10-24). */
#define OV_X 456
#define OV_Y 248
#define OV_N 16
#define OV_RED 0xE02020
#define OV_GREEN 0x30C040
static volatile int g_recording;         /* note+L: a recording under way */
static volatile u32 g_clip_dot_until;    /* note+R: the green dot until then (0: none) */
static volatile u32 g_ov_copying;        /* a frame handed to the copier: its dot drawn once copied */
static volatile u32 g_ov_shown;          /* the frame shown now */
static unsigned char g_ov_mask[OV_N * OV_N];   /* 0 nothing, 1 the dark ring, 2 the colour */
static struct { u32 top, color; int bw, fmt; u32 saved[OV_N * OV_N]; } g_ov[3];

static void ov_init(void)
{
    int x, y;
    for (y = 0; y < OV_N; y++)
        for (x = 0; x < OV_N; x++) {
            int cx = 2 * x + 1 - OV_N, cy = 2 * y + 1 - OV_N, d = cx * cx + cy * cy;   /* (half pixels: the dot centred) */
            g_ov_mask[y * OV_N + x] = d <= 144 ? 2 : d <= 196 ? 1 : 0;   /* a 12-pixel dot in a 14-pixel ring */
        }
}

static u32 ov_pixel(int fmt, u32 rgb)
{
    u32 r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
    if (fmt == PSP_DISPLAY_PIXEL_FORMAT_565) return r >> 3 | (g >> 2) << 5 | (b >> 3) << 11;
    if (fmt == PSP_DISPLAY_PIXEL_FORMAT_5551) return r >> 3 | (g >> 3) << 5 | (b >> 3) << 10 | 0x8000;
    if (fmt == PSP_DISPLAY_PIXEL_FORMAT_4444) return r >> 4 | (g >> 4) << 4 | (b >> 4) << 8 | 0xF000;
    return 0xFF000000 | b << 16 | g << 8 | r;
}

static u32 ov_addr(u32 top, int bw, int fmt, int k)   /* (uncached: VRAM as the screen reads it) */
{
    return ((top & 0x1FFFFFFF) | 0x40000000) + ((u32)(OV_Y + k / OV_N) * (u32)bw + OV_X + k % OV_N) * (fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2);
}
static u32 ov_get(u32 top, int bw, int fmt, int k)
{
    u32 a = ov_addr(top, bw, fmt, k);
    return fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? *(volatile u32 *)a : *(volatile u16 *)a;
}
static void ov_set(u32 top, int bw, int fmt, int k, u32 v)
{
    u32 a = ov_addr(top, bw, fmt, k);
    if (fmt == PSP_DISPLAY_PIXEL_FORMAT_8888) *(volatile u32 *)a = v; else *(volatile u16 *)a = (u16)v;
}

static int ov_in(u32 x, u32 y) { return y >= OV_Y && y < OV_Y + OV_N && x + 1 >= OV_X && x < OV_X + OV_N; }   /* (x+1: a word's two 16-bit pixels) */

static u32 ov_wanted(void)
{
    if (g_clip_dot_until && (int)(now_us() - g_clip_dot_until) < 0) return OV_GREEN;
    return g_recording ? OV_RED : 0;
}

static int ov_find(u32 key) { int i; for (i = 0; i < 3; i++) if (g_ov[i].top == key) return i; return -1; }

/* the dot still there as drawn (the game has not drawn over it since) */
static int ov_still(int i)
{
    return ov_get(g_ov[i].top, g_ov[i].bw, g_ov[i].fmt, (OV_N / 2) * OV_N + OV_N / 2) == ov_pixel(g_ov[i].fmt, g_ov[i].color)
        && ov_get(g_ov[i].top, g_ov[i].bw, g_ov[i].fmt, (OV_N / 2) * OV_N + 1) == ov_pixel(g_ov[i].fmt, 0);
}

/* the pixels under the dot back, if it is still there; the record dropped */
static void ov_restore(int i)
{
    u32 intr = pspSdkDisableInterrupts();
    int k;
    if (g_ov[i].top && ov_still(i))
        for (k = 0; k < OV_N * OV_N; k++) if (g_ov_mask[k]) ov_set(g_ov[i].top, g_ov[i].bw, g_ov[i].fmt, k, g_ov[i].saved[k]);
    g_ov[i].top = 0;
    pspSdkEnableInterrupts(intr);
}

static void ov_draw(u32 key, int bw, int fmt, u32 color)
{
    u32 intr, fill = ov_pixel(fmt, color), ring = ov_pixel(fmt, 0);
    int i = ov_find(key), k;
    if (i >= 0) ov_restore(i);
    for (i = 0; i < 3 && g_ov[i].top; i++) ;
    if (i == 3) { static int next; i = next++ % 3; }   /* (an older buffer's record dropped: the game draws it anew) */
    intr = pspSdkDisableInterrupts();
    g_ov[i].top = key; g_ov[i].bw = bw; g_ov[i].fmt = fmt; g_ov[i].color = color;
    for (k = 0; k < OV_N * OV_N; k++)
        if (g_ov_mask[k]) { g_ov[i].saved[k] = ov_get(key, bw, fmt, k); ov_set(key, bw, fmt, k, g_ov_mask[k] == 2 ? fill : ring); }
    pspSdkEnableInterrupts(intr);
}

/* a frame shown anew (the watcher): a dot from an earlier showing taken off before the frame is
 * looked at or copied */
static void ov_new_frame(u32 key)
{
    int i = ov_find(key);
    g_ov_shown = key;
    if (i >= 0) ov_restore(i);
}

/* every refresh (the watcher): the dot on the frame shown as it should be, but on one the copier
 * has yet to copy */
static void ov_tick(int bw, int fmt)
{
    u32 key = g_ov_shown, want = ov_wanted();
    int i;
    if (g_clip_dot_until && (int)(now_us() - g_clip_dot_until) >= 0) g_clip_dot_until = 0;
    if (!key || key == g_ov_copying || bw < OV_X + OV_N) return;
    i = ov_find(key);
    if (!want) { if (i >= 0) ov_restore(i); return; }
    if (i < 0 || g_ov[i].color != want || !ov_still(i)) ov_draw(key, bw, fmt, want);
}

/* the copier, its frame copied: the dot on it, if it is still the one shown */
static void ov_after_copy(u32 key, int bw, int fmt)
{
    u32 want = ov_wanted();
    if (want && key == g_ov_shown && bw >= OV_X + OV_N) ov_draw(key, bw, fmt, want);
    g_ov_copying = 0;
}

/* The frame the same as the last one looked at? 1,024 words sampled across it (≈0.2 ms): a game's
 * loading screen swaps identical frames, and sending them all took the processor time its loading
 * needed (Gran Turismo: black for minutes at 60 frames a second, normal without the plugin,
 * 2026-10-24). Such frames are not sent, but for one a second; the receiver holds the last one. */
static int frame_unchanged(const void *top, int bw, int fmt)
{
    static u32 last_a, last_b;
    const u32 *v = (const u32 *)(((u32)top & 0x1FFFFFFF) | 0x40000000);   /* (uncached: what the GE wrote) */
    u32 bpp = fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2, words = (u32)bw * CAP_H * bpp / 4, step = words / 1024, i, a = 0, b = 0;
    for (i = 0; i < words; i += step) {
        u32 w, px = i * 4 / bpp;
        if (ov_in(px % (u32)bw, px / (u32)bw)) continue;   /* (the dot's corner) */
        w = v[i]; a += w; b = (b << 1 | b >> 31) ^ w;
    }
    if (a == last_a && b == last_b) return 1;
    last_a = a; last_b = b;
    return 0;
}
static int g_link_on = 1;                /* the USB link part (L) */
static volatile int g_util_active, g_util_closing;   /* a system screen open (the plugin's memory given back) */
/* Experiments (exp, from the receiver), to tell what costs the game its speed: 1 the copy alone
 * (nothing sent), 2 the sending alone (nothing copied: the ring's stale bytes go out). */
static volatile int g_exp;

/* a thread's processor time so far (us; the PSP counts it per thread) */
static u32 cpu_us(SceUID th)
{
    SceKernelThreadRunStatus st;
    if (th < 0) return 0;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    if (sceKernelReferThreadRunStatus(th, &st) < 0) return 0;
    return st.runClocks.low;
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

/* VRAM as the game uses it: its GE addresses, and a fingerprint of every 32 KB of the 2 MB
 * (a sum of every 16th word, and how many of those are not zero); two of these a few seconds
 * apart show the parts the game never touches */
static u32 g_vram_info[8 + 128];
static void vram_probe(const void *top)
{
    const u32 *v = (const u32 *)0x44000000;   /* (uncached) */
    int b, i;
    g_vram_info[0] = sceGeGetCmd(0x9C); g_vram_info[1] = sceGeGetCmd(0x9D);   /* FBP, FBW */
    g_vram_info[2] = sceGeGetCmd(0x9E); g_vram_info[3] = sceGeGetCmd(0x9F);   /* ZBP, ZBW */
    g_vram_info[4] = sceGeGetCmd(0xA0); g_vram_info[5] = sceGeGetCmd(0xA8);   /* TBP0, TBW0 */
    g_vram_info[6] = sceGeGetCmd(0xD2); g_vram_info[7] = (u32)top;
    for (b = 0; b < 64; b++) {
        u32 sum = 0, nz = 0;
        for (i = 0; i < 8192; i += 16) { u32 w = v[b * 8192 + i]; sum += w; nz += w != 0; }
        g_vram_info[8 + b * 2] = sum; g_vram_info[9 + b * 2] = nz;
    }
}
static void *volatile g_top;   /* the frame shown now (the watcher's) */

/* A copy out of VRAM by the DMA engine, never on top of one of the game's own: the engine copies
 * one thing at a time (≈225 MB/s), and games copy with it too (films' frames into VRAM...).
 * Jeanne d'Arc switched the PSP off the moment its frames were copied band by band with
 * sceDmacMemcpy, nine copies spread over each frame; copied by the processor instead, it played
 * on (2026-10-24). So sceDmacTryMemcpy, which refuses while the engine is busy: tried again
 * every 50 us; still busy after 3 ms (or another error), the processor copies, reading VRAM
 * through its cache (psp-cast's way: the lines dropped first, as the GE writes VRAM behind the
 * cache; written back, so nothing the game wrote there itself is lost). */
#define DMAC_BUSY 0x80000021
static volatile u32 g_dma_busy, g_cpu_copies;
static void vram_copy(void *dst, const void *src, u32 n)
{
    u32 t0 = now_us();
    int r;
    while ((r = sceDmacTryMemcpy(dst, src, n)) == (int)DMAC_BUSY && now_us() - t0 < 3000) {
        if (!g_dma_busy++) cap_log("the DMA engine was busy (a copy of the game's): waited for it");
        sceKernelDelayThread(50);
    }
    if (r >= 0) return;
    if (!g_cpu_copies++) cap_log("a copy by the processor (the DMA engine: %x)", (u32)r);
    sceKernelDcacheWritebackInvalidateRange((void *)src, n);
    memcpy(dst, src, n);
    sceKernelDcacheWritebackInvalidateRange(dst, n);   /* (only the engines touch it otherwise) */
}

/* The copy against the frame in VRAM now, 1,024 words across it (≈0.3 ms, both read uncached):
 * 0 when the game drew into the frame while it was copied. Some games draw into the frame shown:
 * Jeanne d'Arc's illustrated scenes, cleared and drawn again within ≈5 ms of its swap, came with
 * a black staircase below the rows drawn so far in ≈1 frame of 20 (2026-10-24). */
static int copy_matches(int banded, u32 stride, u32 rows, u32 first)
{
    u32 bytes = stride * CAP_H, step = bytes / 4 / 1024 * 4, o;
    const u32 *v = (const u32 *)(((u32)g_frame_src & 0x1FFFFFFF) | 0x40000000);
    for (o = first * rows * stride; o < bytes; o += step) {   /* (from band first on: in its slot still) */
        u32 b = banded ? o / (rows * stride) % (u32)g_nband * BAND + o % (rows * stride) : o, px = o / (stride / (u32)g_frame_bw);
        if (ov_in(px % (u32)g_frame_bw, px / (u32)g_frame_bw)) continue;   /* (the dot's corner) */
        if (v[o / 4] != *(const volatile u32 *)((((u32)g_fbuf + b) & 0x1FFFFFFF) | 0xA0000000)) return 0;
    }
    return 1;
}

/* the copier: each frame the watcher hands it, whole into the buffer in one DMA copy (or band by
 * band, the fallback); a frame held whole before any of it goes is checked against VRAM and,
 * the game having drawn into it meanwhile, copied again 3 ms later (twice at most, and not once
 * the game has swapped frames). A frame larger than the bands (Crisis Core's 32-bit one: 9 of
 * 17) goes as it is copied, its last bands ≈10 ms after the swap: those are checked once copied,
 * only counted (in a game drawing into the frame it shows, they would come half drawn). */
static volatile u32 g_redrawn, g_torn, g_late_drawn;
static int copy_thread(SceSize args, void *argp)
{
    while (take(g_copy_go) == 0) {
        int bpp = g_frame_fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2, tries = 0;
        u32 bytes = (u32)g_frame_bw * CAP_H * bpp, t0 = now_us(), changes = g_changes;
        u32 stride = (u32)g_frame_bw * bpp, rows = BAND / stride, nbands = (CAP_H + rows - 1) / rows, y, k;
        int banded = g_frame_mode == 2, whole = !banded || (int)nbands <= g_nband;
        for (;;) {
            if (banded) {   /* the fallback: band by band, as the sender frees them */
                for (y = 0, k = 0; y < CAP_H; y += rows, k++) {
                    u32 n = CAP_H - y < rows ? CAP_H - y : rows, slot = k % g_nband;
                    if (!tries && take(g_band_free) < 0) return 0;
                    if (g_exp != 2) vram_copy(g_fbuf + slot * BAND, g_frame_src + y * stride, n * stride);
                    g_band_len[slot] = n * stride;
                    if (!whole) sceKernelSignalSema(g_band_full, 1);
                }
            } else if (g_exp != 2 && g_fbuf && bytes <= FRAME_MAX) vram_copy(g_fbuf, g_frame_src, bytes);
            if (!whole) {
                if (g_exp != 2 && !copy_matches(1, stride, rows, nbands - (u32)g_nband)) g_late_drawn++;
                break;
            }
            if (g_exp == 2 || !g_fbuf || copy_matches(banded, stride, rows, 0)) break;
            if (!tries) g_redrawn++;
            if (++tries > 2) { g_torn++; break; }
            sceKernelDelayThread(3000);
            if (g_changes != changes) { g_torn++; break; }   /* (swapped meanwhile: what it has) */
        }
        if (banded && whole) sceKernelSignalSema(g_band_full, (int)nbands);
        g_frame_bytes = bytes;
        g_dma_us += now_us() - t0;
        g_copy_us += now_us() - t0;
        if (g_changes != changes) g_late++;   /* the game flipped before the copy ended: maybe torn */
        ov_after_copy((u32)g_frame_src & 0x1FFFFFFF, g_frame_bw, g_frame_fmt);   /* (the dot, now that the frame is copied) */
        if (!banded) sceKernelSignalSema(g_copied, 1);
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
#define AUDIO_RING (64 * 1024)   /* (in the game's memory with the frame's: see mem_acquire) */
#define AUDIO_RING_K (16 * 1024) /* (the fallback's, in the kernel's memory) */
static u32 g_aring_size = AUDIO_RING;
static unsigned char *g_aring;
static volatile u32 g_ahead, g_atail;  /* bytes written and sent so far (the ring: their difference) */
static volatile u32 g_aseq, g_ablocks, g_abytes, g_adropped, g_aunknown, g_akinds;
static struct { u16 samples, chans; } g_ch[8];
static u16 g_o2_samples;
static u16 g_src_samples, g_src_chans;
static u32 g_src_rate;

static SceUID g_ablock = -1;

static void ring_put(const void *p, u32 n)
{
    u32 at = g_ahead % g_aring_size, first = g_aring_size - at < n ? g_aring_size - at : n;
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
    if (!g_aring) {   /* (given back meanwhile: a system screen) */
    } else if (g_aring_size - (g_ahead - g_atail) < 32 + n) {
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
    if (!n || !g_aring) return 0;
    at = tail % g_aring_size;
    first = g_aring_size - at < n ? g_aring_size - at : n;
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
    cap_log("sound: %d of 12 calls caught", n);
}

/* ---- the plugin's memory: taken from the game's spare memory, given back on demand ---- */
static u32 g_mem_released_at, g_mem_tried_at, g_started_at;

/* The volatile memory: 4 MB at 0x08400000 that a game may lock for itself (sceKernelVolatileMemLock
 * or scePowerVolatileMemLock; Lock waits while another holds it, TryLock refuses). Its lock is
 * taken by the plugin when the game leaves it free and the kernel's bands cannot hold the whole
 * frame (Crisis Core's 32-bit one needs 17 bands, 9 fit: 2026-10-24), and given back the moment
 * the game asks for it, its call then going through. Only with all four of a game's locking
 * calls caught: one the plugin could not answer would leave the game waiting forever. */
static int (*g_vtrylock)(int, void **, int *), (*g_vunlock)(int);
static int g_vcaught;
static u32 g_vrefused;   /* (the last refusal logged) */

static int vol_acquire(u32 freem)
{
    void *p = NULL;
    int size = 0, r;
    if (g_vcaught != 4 || !g_vtrylock || !g_vunlock) return 0;
    r = g_vtrylock(0, &p, &size);
    if (r < 0) {
        if ((u32)r != g_vrefused) { g_vrefused = (u32)r; cap_log("the volatile memory not free (%x): the game's", (u32)r); }
        return 0;
    }
    if (!p || size < (int)(FRAME_MAX + AUDIO_RING + 64)) { g_vunlock(0); return 0; }
    g_fbuf = (unsigned char *)(((u32)p + 63) & ~63u);
    sceKernelDcacheWritebackInvalidateRange(g_fbuf, FRAME_MAX + AUDIO_RING);
    g_aring_size = AUDIO_RING;
    g_atail = g_ahead;
    g_aring = g_fbuf + FRAME_MAX;
    g_mem_held = 3;
    cap_log("buffers taken from the volatile memory (%u KB at %x; the game has %u KB of its own free): %u KB",
            (u32)size / 1024, (u32)p, freem / 1024, (FRAME_MAX + AUDIO_RING) / 1024);
    return 1;
}

/* the frame's and the sound's buffers: in the game's memory if it keeps UMARGIN free besides;
 * else the kernel's bands when they hold the whole frame, the volatile memory, or as many bands
 * as fit; 1 when held */
static int mem_acquire(void)
{
    u32 need = FRAME_MAX + 64 + AUDIO_RING + 64, freem = sceKernelPartitionTotalFreeMemSize(2), kfree, kmax;
    int nb, want = MAXBAND, bw = 0, fmt = 0;
    void *top = NULL;
    SceUID f, a;
    if (g_mem_held) return 1;
    if (g_busy) return 0;   /* (a frame still finishing in buffers given back) */
    if (g_fblock >= 0) { sceKernelFreePartitionMemory(g_fblock); g_fblock = -1; g_fbuf = NULL; }   /* (kept by a release that came mid-frame) */
    if (freem >= need + UMARGIN && sceKernelPartitionMaxFreeMemSize(2) >= FRAME_MAX + 64) {
        f = sceKernelAllocPartitionMemory(2, "PSPExpCaptureFrame", PSP_SMEM_High, FRAME_MAX + 64, NULL);
        if (f < 0) return 0;
        a = sceKernelAllocPartitionMemory(2, "PSPExpCaptureSound", PSP_SMEM_High, AUDIO_RING + 64, NULL);
        if (a < 0) { sceKernelFreePartitionMemory(f); return 0; }
        g_aring_size = AUDIO_RING;
        g_fblock = f; g_ablock = a;
        g_fbuf = (unsigned char *)(((u32)sceKernelGetBlockHeadAddr(f) + 63) & ~63u);
        sceKernelDcacheWritebackInvalidateRange(g_fbuf, FRAME_MAX);   /* (only the DMA engines touch it) */
        g_atail = g_ahead;
        g_aring = (unsigned char *)(((u32)sceKernelGetBlockHeadAddr(a) + 63) & ~63u);
        g_mem_held = 1;
        cap_log("buffers taken from the game's memory: %u KB (%u KB were free)", need / 1024, freem / 1024);
        return 1;
    }
    kfree = sceKernelPartitionTotalFreeMemSize(1);
    kmax = sceKernelPartitionMaxFreeMemSize(1);
    nb = kfree > KMARGIN + AUDIO_RING_K + 128 ? (int)((kfree - KMARGIN - AUDIO_RING_K - 128) / BAND) : 0;
    if (sceDisplayGetFrameBuf(&top, &bw, &fmt, 0) >= 0 && bw > 0) {   /* (no more than the frame shown needs) */
        u32 rows = BAND / ((u32)bw * (fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2));
        if (rows) want = (CAP_H + rows - 1) / rows;
    }
    if (nb > want) nb = want;
    if (nb > MAXBAND) nb = MAXBAND;
    while (nb >= 2 && (u32)nb * BAND + 64 > kmax) nb--;
    if (nb < want && vol_acquire(freem)) return 1;
    if (nb < 2) return 0;
    f = sceKernelAllocPartitionMemory(1, "PSPExpCaptureBands", PSP_SMEM_High, nb * BAND + 64, NULL);
    if (f < 0) return 0;
    a = sceKernelAllocPartitionMemory(1, "PSPExpCaptureSound", PSP_SMEM_High, AUDIO_RING_K + 64, NULL);
    if (a < 0) { sceKernelFreePartitionMemory(f); return 0; }
    g_fblock = f; g_ablock = a;
    g_fbuf = (unsigned char *)(((u32)sceKernelGetBlockHeadAddr(f) + 63) & ~63u);
    sceKernelDcacheWritebackInvalidateRange(g_fbuf, nb * BAND);
    g_nband = nb;   /* (every band free: no frame is on its way) */
    while (sceKernelPollSema(g_band_free, 1) >= 0) ;
    while (sceKernelPollSema(g_band_full, 1) >= 0) ;
    sceKernelSignalSema(g_band_free, nb);
    g_aring_size = AUDIO_RING_K;
    g_atail = g_ahead;
    g_aring = (unsigned char *)(((u32)sceKernelGetBlockHeadAddr(a) + 63) & ~63u);
    g_mem_held = 2;
    cap_log("buffers taken from the kernel's memory (the game has %u KB free): %d bands of 32 KB of %d and the sound's 16 KB (%u KB were free)",
            freem / 1024, nb, want, kfree / 1024);
    return 1;
}

/* the buffers given back (the frame under way finished first, the sound stopped): the game, or
 * its system, needs the memory. The sound ring's lines are written back and dropped from the
 * cache first: written later, they would land in memory that is no longer the plugin's. */
static void mem_release(const char *why)
{
    int k1, i, locked, held = g_mem_held;
    u32 intr, rsize = g_aring_size;
    unsigned char *ring;
    SceUInt t = 300000;
    if (!held) return;
    k1 = pspSdkSetK1(0);
    g_mem_held = 0;   /* (the watcher hands no more frames) */
    for (i = 0; i < (held == 3 ? 700 : 60) && g_busy; i++) sceKernelDelayThread(5000);   /* (the volatile memory goes back at once: its frame's sending ends first, within the link's 3 s timeout) */
    locked = g_tx_lock >= 0 && sceKernelWaitSema(g_tx_lock, 1, &t) >= 0;
    intr = pspSdkDisableInterrupts();
    ring = g_aring;
    g_aring = NULL;
    pspSdkEnableInterrupts(intr);
    if (locked) sceKernelSignalSema(g_tx_lock, 1);
    if (ring) sceKernelDcacheWritebackInvalidateRange(ring, rsize);
    if (g_ablock >= 0) { sceKernelFreePartitionMemory(g_ablock); g_ablock = -1; }
    if (held == 3) {
        if (g_busy) cap_log("the volatile memory given back with a frame still on its way");
        if (g_fbuf) sceKernelDcacheWritebackInvalidateRange(g_fbuf, FRAME_MAX);
        g_fbuf = NULL;
        g_vunlock(0);
    } else if (!g_busy && g_fblock >= 0) { sceKernelFreePartitionMemory(g_fblock); g_fblock = -1; g_fbuf = NULL; }
    g_mem_releases++;
    g_mem_released_at = now_us();
    cap_log("buffers given back (%s): %u KB free in the game's memory", why, (u32)sceKernelPartitionTotalFreeMemSize(2) / 1024);
    pspSdkSetK1(k1);
}

/* the game's own memory requests and module loads: one that fails while the plugin holds memory
 * is retried once the plugin has given its buffers back, so the game never sees a failure the
 * plugin caused */
static SceUID (*o_alloc)(SceUID, const char *, int, SceSize, void *);
static SceUID (*o_loadmod)(const char *, int, void *);
static SceUID h_alloc(SceUID part, const char *name, int type, SceSize size, void *addr)
{
    SceUID r = o_alloc(part, name, type, size, addr);
    if (r < 0 && g_mem_held && !g_quit) { mem_release("the game asked for memory"); r = o_alloc(part, name, type, size, addr); }
    return r;
}
static SceUID h_loadmod(const char *path, int flags, void *opt)
{
    SceUID r = o_loadmod(path, flags, opt);
    if (r < 0 && g_mem_held && !g_quit) { mem_release("the game loaded a module"); r = o_loadmod(path, flags, opt); }
    return r;
}

/* the other ways a game takes memory from its partition: pools, threads (their stacks), message
 * pipes; Jeanne d'Arc and Crisis Core froze or crashed right after the plugin took its buffers,
 * with no failed request of the two above (2026-10-24) */
static int (*o_vpl)(const char *, int, int, unsigned int, void *), (*o_pipe)(const char *, int, int, unsigned int, void *);
static int (*o_fpl)(const char *, int, int, unsigned int, unsigned int, void *);
static int (*o_thread)(const char *, void *, int, int, int, void *);
#define RETRY(call, why) do { r = call; if (r < 0 && g_mem_held && !g_quit) { mem_release(why); r = call; } } while (0)
static int h_vpl(const char *n, int part, int attr, unsigned int size, void *o) { int r; RETRY(o_vpl(n, part, attr, size, o), "the game made a memory pool"); return r; }
static int h_pipe(const char *n, int part, int attr, unsigned int size, void *o) { int r; RETRY(o_pipe(n, part, attr, size, o), "the game made a message pipe"); return r; }
static int h_fpl(const char *n, int part, int attr, unsigned int bs, unsigned int nb, void *o) { int r; RETRY(o_fpl(n, part, attr, bs, nb, o), "the game made a memory pool"); return r; }
static int h_thread(const char *n, void *e, int prio, int stack, int attr, void *o) { int r; RETRY(o_thread(n, e, prio, stack, attr, o), "the game made a thread"); return r; }

/* the game locking the volatile memory: the plugin's hold on it given back first */
static int (*o_vlock)(int, void **, int *), (*o_vtry)(int, void **, int *), (*o_plock)(int, void **, int *), (*o_ptry)(int, void **, int *);
static int h_vlock(int t, void **p, int *s) { if (g_mem_held == 3) mem_release("the game locks the volatile memory"); return o_vlock(t, p, s); }
static int h_vtry(int t, void **p, int *s) { if (g_mem_held == 3) mem_release("the game locks the volatile memory"); return o_vtry(t, p, s); }
static int h_plock(int t, void **p, int *s) { if (g_mem_held == 3) mem_release("the game locks the volatile memory"); return o_plock(t, p, s); }
static int h_ptry(int t, void **p, int *s) { if (g_mem_held == 3) mem_release("the game locks the volatile memory"); return o_ptry(t, p, s); }

static void catch_mem(void)
{
    int n = redirect("sceSystemMemoryManager", "SysMemUserForUser", 0x237DBD4F, h_alloc, &o_alloc)   /* sceKernelAllocPartitionMemory */
          + redirect("sceModuleManager", "ModuleMgrForUser", 0x977DE386, h_loadmod, &o_loadmod)       /* sceKernelLoadModule */
          + redirect("sceThreadManager", "ThreadManForUser", 0x56C039B5, h_vpl, &o_vpl)                /* sceKernelCreateVpl */
          + redirect("sceThreadManager", "ThreadManForUser", 0xC07BB470, h_fpl, &o_fpl)                /* sceKernelCreateFpl */
          + redirect("sceThreadManager", "ThreadManForUser", 0x446D8DE6, h_thread, &o_thread)          /* sceKernelCreateThread */
          + redirect("sceThreadManager", "ThreadManForUser", 0x7C0DC2A0, h_pipe, &o_pipe);             /* sceKernelCreateMsgPipe */
    cap_log("the game's memory requests: %d of 6 calls caught", n);
    g_vcaught = redirect("sceSystemMemoryManager", "sceSuspendForUser", 0x3E0271D3, h_vlock, &o_vlock)   /* sceKernelVolatileMemLock */
              + redirect("sceSystemMemoryManager", "sceSuspendForUser", 0xA14F40B2, h_vtry, &o_vtry)    /* ...TryLock */
              + redirect("scePower_Service", "scePower", 0x23C31FFE, h_plock, &o_plock)                 /* scePowerVolatileMemLock */
              + redirect("scePower_Service", "scePower", 0xFA97A599, h_ptry, &o_ptry);                  /* ...TryLock */
    g_vtrylock = (void *)sctrlHENFindFunction("sceSystemMemoryManager", "sceSuspendForKernel", 0xA14F40B2);
    g_vunlock = (void *)sctrlHENFindFunction("sceSystemMemoryManager", "sceSuspendForKernel", 0xA569E425);
    cap_log("the volatile memory: %d of 4 calls caught, its lock %s", g_vcaught, g_vtrylock && g_vunlock ? "found" : "not found");
}

/* ---- the PSP's own screens: the plugin gives its memory back meanwhile ----
 * The save and load screens, the keyboard, messages and the like are the system's, loaded while a
 * game runs, and they need memory the plugin otherwise holds (its frame ring and sound ring,
 * ≈350 KB of the kernel's): Castlevania: The Dracula X Chronicles stopped answering its buttons
 * at its name entry, then switched the PSP off when exited (2026-10-24), as Burnout Legends hung
 * loading a profile when the plugin's buffer sat in the game's memory. While one of those screens
 * is open nothing is recorded (the clip holds the last frame), and the memory is taken back 1.5 s
 * after it closes. */
static volatile u32 g_util_end;

static void mem_release(const char *why);
static void util_open(void)
{
    if (g_quit) return;
    g_util_closing = 0;
    g_util_active = 1;   /* (the watcher hands no more frames, nor takes the memory back) */
    mem_release("a system screen opens");
}

static void util_closed(void)
{
    if (g_quit) return;
    g_util_end = now_us();
    g_util_closing = 1;
}

/* the watcher's, at a refresh: a screen closed for 1.5 s is over (the memory is taken back by
 * the watcher, when the game's margin allows) */
static void util_check(void)
{
    if (g_quit || !g_util_active || !g_util_closing || now_us() - g_util_end < 1500000) return;
    g_util_closing = 0;
    g_util_active = 0;
    cap_log("the system screen closed");
}

static int (*o_sd_in)(void *), (*o_msg_in)(void *), (*o_osk_in)(void *), (*o_net_in)(void *), (*o_gs_in)(void *),
           (*o_html_in)(void *), (*o_shot_in)(void *), (*o_gdi_in)(void *);
static int (*o_sd_out)(void), (*o_msg_out)(void), (*o_osk_out)(void), (*o_net_out)(void), (*o_gs_out)(void),
           (*o_html_out)(void), (*o_shot_out)(void), (*o_gdi_out)(void);
#define UTIL_PAIR(name) \
    static int h_##name##_in(void *p) { int r; util_open(); r = o_##name##_in(p); if (r < 0) util_closed(); return r; } \
    static int h_##name##_out(void) { int r = o_##name##_out(); util_closed(); return r; }
UTIL_PAIR(sd) UTIL_PAIR(msg) UTIL_PAIR(osk) UTIL_PAIR(net) UTIL_PAIR(gs) UTIL_PAIR(html) UTIL_PAIR(shot) UTIL_PAIR(gdi)

/* the game's system-module loads (sceUtilityLoad*Module: films, sound codecs, network, USB), each
 * logged with its result and the kernel's free memory: they load into the memory the plugin
 * holds (Gran Turismo stayed on black screens with the plugin on, 2026-10-24: to be seen) */
static int (*o_lm)(int), (*o_lav)(int), (*o_lnet)(int), (*o_lusb)(int);
static int mod_load(const char *what, int (*f)(int), int id)
{
    int r = f(id), k1 = pspSdkSetK1(0);
    cap_log("%s module %x loaded: %x (%u KB free, largest %u KB)", what, (u32)id, (u32)r,
            (u32)sceKernelPartitionTotalFreeMemSize(1) / 1024, (u32)sceKernelPartitionMaxFreeMemSize(1) / 1024);
    pspSdkSetK1(k1);
    return r;
}
static int h_lm(int id) { return mod_load("a", o_lm, id); }
static int h_lav(int id) { return mod_load("an AV", o_lav, id); }
static int h_lnet(int id) { return mod_load("a network", o_lnet, id); }
static int h_lusb(int id) { return mod_load("a USB", o_lusb, id); }

static void catch_util(void)
{
    static const struct { u32 in, out; void *hin, *hout, *oin, *oout; } u[] = {
        {0x50C4CD57, 0x9790B33C, h_sd_in, h_sd_out, &o_sd_in, &o_sd_out},          /* sceUtilitySavedata */
        {0x2AD8E239, 0x67AF3428, h_msg_in, h_msg_out, &o_msg_in, &o_msg_out},      /* ...MsgDialog */
        {0xF6269B82, 0x3DFAEBA9, h_osk_in, h_osk_out, &o_osk_in, &o_osk_out},      /* ...Osk */
        {0x4DB1E739, 0xF88155F6, h_net_in, h_net_out, &o_net_in, &o_net_out},      /* ...Netconf */
        {0xC492F751, 0xEFC6F80F, h_gs_in, h_gs_out, &o_gs_in, &o_gs_out},          /* ...GameSharing */
        {0xCDC3AA41, 0xF5CE1134, h_html_in, h_html_out, &o_html_in, &o_html_out},  /* ...HtmlViewer */
        {0x0251B134, 0xF9E0008C, h_shot_in, h_shot_out, &o_shot_in, &o_shot_out},  /* ...Screenshot */
        {0x24AC31EB, 0x32E32DCB, h_gdi_in, h_gdi_out, &o_gdi_in, &o_gdi_out},      /* ...GamedataInstall */
    };
    int i, n = 0;
    for (i = 0; i < (int)(sizeof(u) / sizeof(u[0])); i++) {
        n += redirect("sceUtility_Driver", "sceUtility", u[i].in, u[i].hin, u[i].oin);
        n += redirect("sceUtility_Driver", "sceUtility", u[i].out, u[i].hout, u[i].oout);
    }
    n += redirect("sceUtility_Driver", "sceUtility", 0x2A2B3DE0, h_lm, &o_lm);       /* sceUtilityLoadModule */
    n += redirect("sceUtility_Driver", "sceUtility", 0xC629AF26, h_lav, &o_lav);     /* ...LoadAvModule */
    n += redirect("sceUtility_Driver", "sceUtility", 0x1579A159, h_lnet, &o_lnet);   /* ...LoadNetModule */
    n += redirect("sceUtility_Driver", "sceUtility", 0x0D5BC6D2, h_lusb, &o_lusb);   /* ...LoadUsbModule */
    cap_log("system screens and module loads: %d of 20 calls caught", n);
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
#define CMD_VRAM 7       /* reply: the GE's buffer addresses and VRAM's 32 KB fingerprints */
#define CMD_EXP 10       /* u32 the experiment (g_exp) */
#define CMD_COPYPRIO 11  /* u32 the copier's priority */
#define CMD_TX 13        /* u32 the transfer size, u32 split (see pspexpUsbSetTx) */
static volatile u32 g_pad_buttons, g_pad_until, g_pad_reads;
static SceUID g_cmd_th = -1;

static volatile u32 g_pad_mask;   /* the game's buttons hidden from it: those pressed with the note button (see note_check) */
static void pad_apply(SceCtrlData *pad, int n, int negative)
{
    u32 b = g_pad_buttons, m = g_pad_mask;
    int i;
    g_pad_reads++;
    if (n <= 0) return;
    if (b && g_pad_until && (int)(now_us() - g_pad_until) >= 0) { g_pad_buttons = 0; b = 0; }
    if (!b && !m) return;
    for (i = 0; i < n; i++) {
        if (negative) pad[i].Buttons = (pad[i].Buttons | m) & ~b;
        else pad[i].Buttons = (pad[i].Buttons & ~m) | b;
    }
}
static int (*o_rbp)(SceCtrlData *, int), (*o_pbp)(SceCtrlData *, int), (*o_rbn)(SceCtrlData *, int), (*o_pbn)(SceCtrlData *, int);
static int h_rbp(SceCtrlData *p, int n) { int r = o_rbp(p, n); pad_apply(p, r, 0); return r; }
static int h_pbp(SceCtrlData *p, int n) { int r = o_pbp(p, n); pad_apply(p, r, 0); return r; }
static int h_rbn(SceCtrlData *p, int n) { int r = o_rbn(p, n); pad_apply(p, r, 1); return r; }
static int h_pbn(SceCtrlData *p, int n) { int r = o_pbn(p, n); pad_apply(p, r, 1); return r; }

static int g_pad_on;   /* the buttons part (B): the game's reads caught, the note button with them */
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
    {   /* (a link held for over a second: the reply is dropped, the commands go on) */
        SceUInt t = 1000000;
        if (sceKernelWaitSema(g_tx_lock, 1, &t) < 0) { cap_log("reply %u dropped: the link is held", cmd); return; }
    }
    if (pspexpUsbWrite(h, 32) >= 0 && len) pspexpUsbWrite(data, len);
    sceKernelSignalSema(g_tx_lock, 1);
}

#ifdef PSPEXP_CAPTURE_DEV
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
#endif

static void cmd_log(int old)   /* (the newest lines in memory; a developer build: the file, or the last run's) */
{
#ifndef PSPEXP_CAPTURE_DEV
    static char buf[4096];
    u32 intr = pspSdkDisableInterrupts(), head = g_mlog_head, m = head < sizeof(buf) ? head : sizeof(buf), i;
    for (i = 0; i < m; i++) buf[i] = g_mlog[(head - m + i) % MLOG];
    pspSdkEnableInterrupts(intr);
    (void)old;
    cmd_reply("PXCL", CMD_LOG, (int)head, buf, m);
#else
    const char *path = old ? LOG_PATH ".old" : LOG_PATH;
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    int n = fd >= 0 ? sceIoRead(fd, g_cmd_buf, sizeof(g_cmd_buf)) : 0;
    int size = fd >= 0 ? (int)sceIoLseek32(fd, 0, PSP_SEEK_END) : 0;
    if (fd >= 0) sceIoClose(fd);
    if (n < 0) n = 0;
    if (size > n) {   /* the log's end: the newest lines */
        fd = sceIoOpen(path, PSP_O_RDONLY, 0);
        if (fd >= 0) { sceIoLseek32(fd, size - (int)sizeof(g_cmd_buf), PSP_SEEK_SET); n = sceIoRead(fd, g_cmd_buf, sizeof(g_cmd_buf)); sceIoClose(fd); }
    }
    cmd_reply("PXCL", CMD_LOG, size, g_cmd_buf, n > 0 ? (u32)n : 0);
#endif
}

u32 sctrlKernelLoadExecVSHWithApitype(int apitype, const char *file, struct SceKernelLoadExecVSHParam *param);
char *sctrlSEGetUmdFile(void);
void sctrlSESetUmdFile(char *file);
int sctrlSEGetBootConfFileIndex(void);
void sctrlSESetBootConfFileIndex(int index);
static void cap_shutdown(const char *why);
static void usb_quiesce(const char *why);

#ifdef PSPEXP_CAPTURE_DEV
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
    usb_quiesce("the game restarts");   /* (as the restarts did all along: the USB stopped from the plugin's thread first) */
    sctrlSESetUmdFile(iso);
    sctrlSESetBootConfFileIndex(mode);
    memset(&param, 0, sizeof(param));
    param.size = sizeof(param);
    param.argp = (void *)boot;
    param.args = sizeof(boot);
    param.key = mode == 1 ? "game" : "umdemu";   /* (1: March33) */
    return (int)sctrlKernelLoadExecVSHWithApitype(0x120, boot, &param);
}
#endif

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
#ifdef PSPEXP_CAPTURE_DEV
        } else if (cmd == CMD_PUT) {   /* (developer builds: a new plugin over the cable) */
            cmd_reply("PXCR", CMD_PUT, cmd_put(len), NULL, 0);
#endif
        } else if (cmd == CMD_LOG) {
            int old = 0;
            if (len >= 4 && cmd_read(g_cmd_buf, 4) == 0) old = get32(g_cmd_buf) != 0;
            cmd_log(old);
        } else if (cmd == CMD_PAUSE && len >= 4 && cmd_read(g_cmd_buf, 4) == 0) {
            g_paused = get32(g_cmd_buf) != 0;
            cap_log(g_paused ? "paused" : "capturing again");
        } else if (cmd == CMD_EXP && len >= 4 && cmd_read(g_cmd_buf, 4) == 0) {
            g_exp = (int)get32(g_cmd_buf);
            cap_log("experiment %d (1 the copy alone, 2 the sending alone)", g_exp);
        } else if (cmd == CMD_COPYPRIO && len >= 4 && cmd_read(g_cmd_buf, 4) == 0) {
            int pr = (int)get32(g_cmd_buf);
            cap_log("copier priority %x: %x", pr, (u32)sceKernelChangeThreadPriority(g_copy_th, pr));
        } else if (cmd == CMD_TX && len >= 8 && cmd_read(g_cmd_buf, 8) == 0) {
            u32 c = get32(g_cmd_buf);
            if (c >= 4096 && c <= 262144) { pspexpUsbSetTx((int)c, get32(g_cmd_buf + 4) != 0); cap_log("transfers of up to %u KB, split %u", c / 1024, get32(g_cmd_buf + 4)); }
        } else if (cmd == CMD_VRAM) {
            vram_probe(g_top);
            cmd_reply("PXCR", CMD_VRAM, 0, g_vram_info, sizeof(g_vram_info));
#ifdef PSPEXP_CAPTURE_DEV
        } else if (cmd == CMD_RESTART) {
            cmd_reply("PXCR", CMD_RESTART, cmd_restart(), NULL, 0);
#endif
        }
    }
    return 0;
}

/* the log's new lines to the receiver ("PXCM", the frames' 32-byte header; the sender's, under
 * g_tx_lock); <0: the receiver is not reading */
static int log_send(void)
{
    static unsigned char h[32] __attribute__((aligned(64)));
    static char chunk[MLOG] __attribute__((aligned(64)));
    u32 intr = pspSdkDisableInterrupts(), head = g_mlog_head, from = g_mlog_sent, n = head - from, i;
    for (i = 0; i < n; i++) chunk[i] = g_mlog[(from + i) % MLOG];
    pspSdkEnableInterrupts(intr);
    if (!n) return 0;
    memset(h, 0, 32);
    memcpy(h, "PXCM", 4);
    put32(h + 20, now_us()); put32(h + 24, n);
    if (pspexpUsbWrite(h, 32) < 0 || pspexpUsbWrite(chunk, n) < 0) return -1;
    g_mlog_sent = head;
    return 0;
}

/* ---- the note button (the PSP-1000's sound button): a clip, a recording ----
 * Note+R: the receiver saves the last 30 s as a clip. Note+L: a recording starts, or stops.
 * Pairs pressed briefly, as the note button held alone for over a second mutes the PSP's sound.
 * While it is down the game sees none of its buttons (nor those held with it, until let go). The
 * receiver keeps the clips; a dot on the screen says the press was heard: green for a second for
 * a clip, red while recording (ov_tick). Only with a receiver reading, and not over the HOME menu
 * or a system screen. */
#define EV_CLIP 1
#define EV_REC_START 2
#define EV_REC_STOP 3
static volatile u32 g_ev_type[4], g_ev_time[4], g_ev_head, g_ev_sent;   /* (the watcher's, sent by the sender) */

static void event_put(u32 type)
{
    u32 i = g_ev_head;
    if (i - g_ev_sent >= 4) return;
    g_ev_type[i % 4] = type;
    g_ev_time[i % 4] = now_us();
    g_ev_head = i + 1;
    sceKernelSetEventFlag(g_send_ev, SEND_AUDIO);
}

/* the events to the receiver ("PXCE": the type at 4, the time at 20; the sender's, under g_tx_lock) */
static int event_send(void)
{
    static unsigned char h[32] __attribute__((aligned(64)));
    while (g_ev_sent != g_ev_head) {
        u32 i = g_ev_sent % 4;
        memset(h, 0, 32);
        memcpy(h, "PXCE", 4);
        put32(h + 4, g_ev_type[i]); put32(h + 20, g_ev_time[i]);
        if (pspexpUsbWrite(h, 32) < 0) return -1;
        g_ev_sent++;
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
        if (r >= 0) r = log_send();
        if (r >= 0) r = event_send();
        sceKernelSignalSema(g_tx_lock, 1);
        if (r < 0) { g_dead_gen = gen; cap_log("receiver not reading: paused until one connects"); g_busy = 0; continue; }
        if (g_busy == 1) {   /* the frame, once the copier has it: its header, then the whole of it */
            unsigned char *h = g_hdr;
            u32 ts = now_us(), bytes;
            int bpp = g_frame_fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2;
            if (g_frame_mode != 2 && take(g_copied) < 0) break;
            bytes = (u32)g_frame_bw * CAP_H * bpp;
            memset(h, 0, 32);
            memcpy(h, "PXCF", 4);
            put16(h + 4, g_frame_bw); put16(h + 6, CAP_H); put16(h + 8, g_frame_fmt);   /* (width: the stride) */
            put32(h + 12, g_frame_seq); put32(h + 16, g_frame_changes); put32(h + 20, g_frame_time);
            put32(h + 24, bytes);
            sceKernelWaitSema(g_tx_lock, 1, NULL);   /* (a command's reply never inside a frame) */
            if (g_frame_mode == 2) {   /* the fallback: each band as the copier fills it, then its slot freed */
                u32 rows = BAND / ((u32)g_frame_bw * bpp), y, k = 0;
                r = g_exp == 1 || pspexpUsbWrite(h, 32) >= 0;
                for (y = 0; y < CAP_H; y += rows, k++) {   /* (every band taken, sent or not, so the copier goes on) */
                    u32 slot = k % g_nband;
                    if (take(g_band_full) < 0) break;
                    if (r && g_exp != 1) r = pspexpUsbWrite(g_fbuf + slot * BAND, g_band_len[slot]) >= 0;
                    sceKernelSignalSema(g_band_free, 1);
                }
            } else r = g_exp == 1 || !g_fbuf || (pspexpUsbWrite(h, 32) >= 0 && pspexpUsbWrite(g_fbuf, bytes) >= 0);
            sceKernelSignalSema(g_tx_lock, 1);
            if (r) { g_sent++; g_send_us += now_us() - ts; }
            else { g_dead_gen = gen; cap_log("receiver not reading: paused until one connects"); }
            g_busy = 0;
        }
    }
    return 0;
}

/* ---- the HOME menu: the USB stopped before a game can exit ----
 * Every exit with frames still streaming switched the PSP off (Castlevania: The Dracula X
 * Chronicles, six times, 2026-10-24), whether the plugin then stopped its USB at once, cancelled
 * its transfers first, or left everything running; every exit with the receiver no longer reading
 * went well (Burnout Legends). Stopping the USB from the plugin's own thread while a game simply
 * runs has always gone well (the restart command, dozens of times, frames streaming). So the
 * watcher looks at the HOME menu every refresh (sceImposeGetStatus, its top bit; PRO resolves the
 * NID for 6.60. Not bit 0, which RemoteJoyLite toggles the menu by: on 6.60 PRO-C the status was 1
 * at a game's start, 0x00050102 while it ran and 0x80050102 with the HOME menu open, 2026-10-24)
 * and, when it opens,
 * lets the frame under way finish, then stops the USB; the receiver sees the PSP go. Back in the
 * game (the menu closed for half a second), the USB starts again and the receiver reconnects. A
 * game exiting through its own menu, without HOME, is not covered (few PSP games have one). */
static int (*g_impose_status)(void);
static volatile int g_home_paused;
static u32 g_home_closed_at, g_home_pressed_at, g_home_status = 0xFFFFFFFF;

/* frames and sound stopped, the frame under way finished, the USB driver stopped (the watcher's
 * or the command thread's, never inside a game's exit) */
static void usb_quiesce(const char *why)
{
    u32 t0 = now_us();
    int i, locked;
    SceUInt t = 300000;
    g_home_paused = 1;   /* (the watcher hands no more frames) */
    g_log_hold = 1;      /* (and nothing goes to the memory stick: an exit may follow) */
    for (i = 0; i < 60 && g_busy; i++) sceKernelDelayThread(5000);   /* (its last bands sent, not cancelled) */
    locked = g_tx_lock >= 0 && sceKernelWaitSema(g_tx_lock, 1, &t) >= 0;
    if (g_usb_up) { pspexpUsbStop(); sceUsbbdUnregister(&g_driver); g_usb_up = 0; }
    if (locked) sceKernelSignalSema(g_tx_lock, 1);
    cap_log("USB stopped (%s) in %u us, frame done %d", why, now_us() - t0, !g_busy);
}

/* the USB again after the HOME menu: each step on its own, the bus already running not counted as
 * a failure (as psp-cast found: the system may have left it on). In Gran Turismo every restart
 * failed with 80243001 when it was all one call, and the receiver never came back (2026-10-24). */
static void usb_resume(void)
{
    int k1 = pspSdkSetK1(0);
    int reg = sceUsbbdRegister(&g_driver), bus = sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, 0);
    int drv = sceUsbStart(PSPEXP_USB_DEVICE_NAME, 0, 0), act = drv >= 0 ? sceUsbActivate(PSPEXP_USB_DEVICE_PID) : -1;
    pspSdkSetK1(k1);
    if (drv >= 0 && act >= 0) { g_usb_up = 1; pspexpUsbSetTx(128 * 1024, 0); }
    g_home_paused = 0;
    g_log_hold = 0;
    cap_log("USB started again: register %x, bus %x, driver %x, activate %x", (u32)reg, (u32)bus, (u32)drv, (u32)act);
}

static void home_check(void)
{
    u32 st = g_impose_status ? (u32)g_impose_status() : 0;
    SceCtrlData pad;
    int home = 0;
    /* the HOME button itself as well (kernel code sees it): early in a game the status stays at 1
     * and its top bit did not follow the menu (a crash test, 2026-10-24) */
    if (sceCtrlPeekBufferPositive(&pad, 1) > 0 && (pad.Buttons & PSP_CTRL_HOME)) home = 1;
    if (st != g_home_status) { cap_log("HOME menu status %x", st); g_home_status = st; }
    if (home) g_home_pressed_at = now_us() | 1;
    if ((st & 0x80000000u) || home || (g_home_pressed_at && now_us() - g_home_pressed_at < 5000000)) {
        /* (the HOME menu open, or HOME pressed in the last 5 s: in Gran Turismo the status' top bit
         * did not show the menu, 2026-10-24) */
        g_home_closed_at = 0;
        if (!g_home_paused) {
            usb_quiesce(home ? "HOME pressed" : "the HOME menu opened");
            mem_release("the HOME menu");   /* (an exit may follow: the kernel's memory all its own) */
        }
    } else if (g_home_paused && !g_util_active) {
        if (!g_home_closed_at) g_home_closed_at = now_us() | 1;
        else if (now_us() - g_home_closed_at > 500000) { g_home_closed_at = 0; usb_resume(); }
    }
}


/* the note button, each screen refresh (see event_put): the pairs, the buttons hidden from the
 * game, the mute put back (the dot on the screen: ov_tick). The note button pressed mutes the
 * PSP's sound (the HOME menu's driver sees the buttons before any mask); used for a pair, the mute
 * is put back as it was, at once, and for half a second after the button is let go (in case it
 * toggles on the release); pressed alone it mutes as it always does, when let go. */
#define GAME_BUTTONS 0xF3F9   /* select, start, the pad, L, R, the four shapes */
#define IMPOSE_MUTE 0x8
static int (*g_impose_get)(int), (*g_impose_set)(int, int);
static void note_check(int reading)
{
    static u32 was, keep_until;
    static int before = -1, wanted = -1, pair;
    SceCtrlData pad;
    u32 b;
    int mute;
    if (sceCtrlPeekBufferPositive(&pad, 1) <= 0) return;
    b = pad.Buttons;
    mute = g_impose_get && g_impose_set ? g_impose_get(IMPOSE_MUTE) : -1;
    if (b & PSP_CTRL_NOTE) {
        u32 pressed = b & ~was;
        g_pad_mask = GAME_BUTTONS;
        if (!(was & PSP_CTRL_NOTE)) { pair = 0; wanted = -1; }
        if (mute >= 0 && before >= 0 && mute != before) { wanted = mute; g_impose_set(IMPOSE_MUTE, before); }   /* (the system's toggle, kept for a press alone) */
        if (reading && !g_home_paused && !g_util_active) {
            if (pressed & PSP_CTRL_RTRIGGER) { pair = 1; event_put(EV_CLIP); g_clip_dot_until = now_us() + 1200000; cap_log("note+R: a clip"); }
            if (pressed & PSP_CTRL_LTRIGGER) {
                pair = 1;
                g_recording = !g_recording;
                event_put(g_recording ? EV_REC_START : EV_REC_STOP);
                cap_log("note+L: the recording %s", g_recording ? "starts" : "stops");
            }
        }
    } else {
        g_pad_mask &= b;   /* (those held with it hidden until let go) */
        if (was & PSP_CTRL_NOTE) {   /* let go */
            if (pair) keep_until = now_us() + 500000;
            else if (wanted >= 0 && g_impose_set) g_impose_set(IMPOSE_MUTE, wanted);
            wanted = -1;
        }
        if (keep_until && (int)(now_us() - keep_until) < 0) { if (mute >= 0 && mute != before) g_impose_set(IMPOSE_MUTE, before); }
        else { keep_until = 0; before = mute; }
    }
    was = b;
    if (!reading) g_recording = 0;
}

/* the watcher: every screen refresh, the frame shown; a new one counted (the game's frame rate)
 * and, when none is on its way and a receiver is there, handed to the copier and the sender */
static int capture_thread(SceSize args, void *argp)
{
    void *top = NULL, *last = NULL;
    int bw = 0, fmt = 0, r, was_connected = 0;
    u32 seq = 0, t0, c0 = 0, s0 = 0, k0 = 0, o0 = 0, q0 = 0, u0 = 0, d0 = 0, ab0 = 0, ad0 = 0, au0 = 0, se0 = 0, l0 = 0;

    sceKernelDelayThread(2000000);   /* the game first: its modules, its first frames */
    if (g_quit) return 0;
    {   /* PSPExp itself (game.txt's plugins load for homebrew too): its own USB driver has the
         * plugin's name and its board waits for it. Started inside PSPExp, the plugin took the USB
         * and PSPExp could not reach its board (2026-10-24): it keeps out entirely, nothing started
         * (its catches stay idle: they act only while it holds buffers or the note button is down). */
        void *(*find)(const char *) = (void *)sctrlHENFindFunction("sceLoaderCore", "LoadCoreForKernel", 0xCF8A41B1);   /* sceKernelFindModuleByName */
        if (find && (find("PSPExpBoot") || find("PSPExpRuntime") || find("PSPExpUSB"))) {
            g_log_ready = 1;
            cap_log("PSPExp itself: the plugin keeps out of it");
            return 0;
        }
    }
#ifdef PSPEXP_CAPTURE_DEV
    sceIoRemove(LOG_PATH ".old");   /* the last run's log kept beside (after a hang, what it did last) */
    sceIoRename(LOG_PATH, LOG_PATH ".old");
#endif
    g_log_ready = 1;
    cap_log("kernel memory: %u KB free; the game's: %u KB free", (u32)sceKernelPartitionTotalFreeMemSize(1) / 1024,
            (u32)sceKernelPartitionTotalFreeMemSize(2) / 1024);
    g_copy_go = sceKernelCreateSema("PSPExpCaptureCopy", 0, 0, 1, NULL);
    g_copied = sceKernelCreateSema("PSPExpCaptureCopied", 0, 0, 1, NULL);
    g_band_free = sceKernelCreateSema("PSPExpCaptureFree", 0, 0, MAXBAND, NULL);   /* (filled when the bands are taken) */
    g_band_full = sceKernelCreateSema("PSPExpCaptureFull", 0, 0, MAXBAND, NULL);
    if (g_frames_on) {
        g_copy_th = sceKernelCreateThread("PSPExpCaptureCopy", copy_thread, 0x19, 0x2000, 0, NULL);
        if (g_copy_th >= 0) sceKernelStartThread(g_copy_th, 0, NULL);
    }
    {   /* the sender just below the watcher (at 0x60, below the game's threads, a race got 14 frames
         * of 28 a second sent; 0x1A, 14-23; above the game's threads, 0x11 with the watcher at
         * 0x10, no more frames and the game itself slower, 20-25 fps: 2026-10-23) */
        g_send_th = sceKernelCreateThread("PSPExpCaptureSend", send_thread, 0x1A, 0x4000, 0, NULL);
        if (g_send_th >= 0) sceKernelStartThread(g_send_th, 0, NULL);
    }
    cap_log("battery %d%%", scePowerGetBatteryLifePercent());
    if (!g_link_on) {   /* (no USB: the watcher only counts the game's frames) */
        t0 = now_us();
        while (!g_quit) {
            sceDisplayWaitVblankStart();
            if (sceDisplayGetFrameBuf(&top, &bw, &fmt, 0) >= 0 && top != last) { g_changes++; last = top; }
            if (now_us() - t0 >= 5000000) { cap_log("game %u frames in 5 s (no link)", g_changes - c0); c0 = g_changes; t0 = now_us(); }
        }
        return 0;
    }
    memset(&g_stats, 0, sizeof(g_stats));
    r = sceUsbbdRegister(&g_driver);
    cap_log("USB driver registered: %x", r);
    r = pspexpUsbStart();
    cap_log("USB started: %x", r);
    if (r < 0) return 0;
    g_usb_up = 1;
    pspexpUsbSetTx(128 * 1024, 0);   /* (a stream read without pause: no short-ending splits) */
    g_impose_status = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", 0x1B6E3400);   /* sceImposeGetStatus */
    g_impose_get = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", 0x531C9778);      /* sceImposeGetParam */
    g_impose_set = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", 0x810FB7FB);      /* sceImposeSetParam */
    ov_init();
    cap_log("the note button: mute setting %s", g_impose_get && g_impose_set ? "found" : "not found");
    cap_log("HOME menu watched: %x", (u32)g_impose_status);
    g_cmd_th = sceKernelCreateThread("PSPExpCaptureCmd", cmd_thread, 0x30, 0x2000, 0, NULL);
    if (g_cmd_th >= 0) sceKernelStartThread(g_cmd_th, 0, NULL);

    t0 = now_us();
    while (!g_quit) {
        sceDisplayWaitVblankStart();
        util_check();
        home_check();
        if (g_pad_on) note_check(pspexpUsbConnected() && (int)g_generation != g_dead_gen);
        if (!g_mem_held && !g_util_active && !g_home_paused && !g_quit && pspexpUsbConnected()
            && now_us() - g_mem_released_at > 5000000 && now_us() - g_mem_tried_at > 1000000
            && now_us() - g_started_at > 20000000) {   /* (the buffers, when the game's margin allows; never in its first 20 s, while it sets up its pools and threads) */
            g_mem_tried_at = now_us();
            mem_acquire();
        }
        if (sceDisplayGetFrameBuf(&top, &bw, &fmt, 0) < 0 || !top) continue;
        if (top != last) {
            g_changes++; last = top; g_top = top;
            ov_new_frame((u32)top & 0x1FFFFFFF);   /* (a dot from an earlier showing off it first) */
            if (pspexpUsbConnected()) {
                if (!was_connected) { cap_log("receiver connected"); was_connected = 1; }
                if (!g_frames_on || !g_mem_held || g_paused || g_util_active || g_home_paused || (int)g_generation == g_dead_gen) ;   /* (nobody reading: nothing copied) */
                else if (now_us() - g_last_hand < 30000) ;
                else if (frame_unchanged(top, bw, fmt) && now_us() - g_last_hand < 1000000) g_static++;   /* (30 a second at most: clips are 30 fps, and every frame sent costs the game processor time; Gran Turismo's black loading screens at 60 starved its loading, 2026-10-24) */
                else if (!g_busy) {   /* to the copier and the sender */
                    g_last_hand = now_us();
                    g_frame_src = (const unsigned char *)((u32)top & ~0x40000000u);
                    g_frame_bw = bw; g_frame_fmt = fmt;
                    g_frame_seq = seq++; g_frame_changes = g_changes; g_frame_time = now_us();
                    g_frame_mode = g_mem_held;
                    g_ov_copying = (u32)top & 0x1FFFFFFF;
                    g_busy = 1;
                    sceKernelSignalSema(g_copy_go, 1);
                    sceKernelSetEventFlag(g_send_ev, SEND_FRAME);
                } else g_skipped++;
            } else if (was_connected) { cap_log("receiver gone"); was_connected = 0; }
        }
        if (g_pad_on) ov_tick(bw, fmt);   /* the dot on the frame shown */
        if (g_home_paused) t0 = now_us();   /* (no log while the HOME menu is open: an exit may follow) */
        if (now_us() - t0 >= 5000000) {   /* every 5 s: the game's frame rate, and what went out */
            u32 ms = (now_us() - t0) / 1000;   /* (32-bit: no 64-bit division in the kernel) */
            u32 g10 = (g_changes - c0) * 10000 / ms, s10 = (g_sent - s0) * 10000 / ms;
            u32 copies = seq - q0, copy_us = copies ? (g_copy_us - u0) / copies : 0;   /* the game's cost, per frame copied */
            u32 dma_us = copies ? (g_dma_us - d0) / copies : 0, sends = g_sent - s0, send_us = sends ? (g_send_us - se0) / sends : 0;
            cap_log("unchanged frames not sent: %u; kernel memory %u KB free; the game's %u KB free, largest %u KB", g_static,
                    (u32)sceKernelPartitionTotalFreeMemSize(1) / 1024, (u32)sceKernelPartitionTotalFreeMemSize(2) / 1024,
                    (u32)sceKernelPartitionMaxFreeMemSize(2) / 1024);
            sceKernelSetEventFlag(g_send_ev, SEND_AUDIO);   /* (the log goes out even with no frame or sound to send) */
            cap_log("game %u.%u fps, sent %u.%u fps, skipped %u, late %u, copy %u us (DMA %u), send %u us, USB timeouts %u, format %d, width %d, %s",
                    g10 / 10, g10 % 10, s10 / 10, s10 % 10, g_skipped - k0, g_late - l0, copy_us, dma_us, send_us, g_stats.tx_timeouts - o0, fmt, bw,
                    was_connected ? "connected" : "no receiver");
            {   /* the processor time each of the plugin's threads really used (against their wall time) */
                static u32 cc0, cs0, cw0;
                u32 cc = cpu_us(g_copy_th), cs = cpu_us(g_send_th), cw = cpu_us(g_cap_th);
                cap_log("CPU in 5 s: copier %u ms, sender %u ms, watcher %u ms; exp %d; buffers %s, given back %u times",
                        (cc - cc0) / 1000, (cs - cs0) / 1000, (cw - cw0) / 1000, g_exp, g_mem_held == 3 ? "held (the volatile memory)" : g_mem_held == 2 ? "held (the kernel's bands)" : g_mem_held ? "held (the game's memory)" : "not held", g_mem_releases);
                cap_log("copies so far: the DMA engine busy %u times, by the processor %u; drawn into while copied %u (copied again; %u sent so), larger than the bands and drawn into %u",
                        g_dma_busy, g_cpu_copies, g_redrawn, g_torn, g_late_drawn);   /* (a log line holds 240 characters) */
                cc0 = cc; cs0 = cs; cw0 = cw;
            }
            cap_log("sound: %u KB/s, dropped %u, unknown channel %u, kinds %x (1 channels, 2 output2, 4 SRC)",
                    (g_abytes - ab0) / ms, g_adropped - ad0, g_aunknown - au0, g_akinds);
            t0 = now_us(); c0 = g_changes; s0 = g_sent; k0 = g_skipped; o0 = g_stats.tx_timeouts; q0 = seq; u0 = g_copy_us; d0 = g_dma_us;
            ab0 = g_abytes; ad0 = g_adropped; au0 = g_aunknown; se0 = g_send_us; l0 = g_late;
        }
    }
    return 0;
}

/* Which parts run (for finding which one a fault comes from; the file it was read from at the
 * start is gone: no memory-stick access while a game starts, see cap_log). Letters: S the sound's calls, B the buttons', U the system screens', X the exit's, F the
 * frames (their ring and copy), L the USB link (and its commands and the HOME menu's pause); Z
 * nothing at all, the plugin only loaded; no file, all of them but Z. (Castlevania: The Dracula X Chronicles' exit switched the PSP off with
 * the USB already stopped and every thread at rest, 2026-10-24: one of these, then.) */
#ifndef PSPEXP_CAPTURE_PARTS
#define PSPEXP_CAPTURE_PARTS "SBUXFL"   /* (a developer build can leave some out: make DEV=1 PARTS=...) */
#endif
static char g_parts[16] = PSPEXP_CAPTURE_PARTS;
static int part_on(char c) { int i; for (i = 0; g_parts[i]; i++) if (g_parts[i] == c) return 1; return 0; }

int module_start(SceSize args, void *argp)
{
    g_started_at = now_us();
    cap_log("PSPExp Capture: stage 2 (picture and sound; a DMA ring, 32-bit as the game draws; control from the receiver); parts: %s", g_parts[0] ? g_parts : "none");
    if (part_on('Z')) return 0;   /* (loaded, nothing else) */
    g_send_ev = sceKernelCreateEventFlag("PSPExpCaptureSend", 0, 0, NULL);
    g_tx_lock = sceKernelCreateSema("PSPExpCaptureTx", 0, 1, 1, NULL);
    if (part_on('X')) catch_exit();
    if (part_on('S')) catch_audio();   /* now, before the game reserves its channels */
    if (part_on('B')) { catch_pad(); g_pad_on = 1; }
    if (part_on('U')) catch_util();
    catch_mem();
    g_frames_on = part_on('F');
    g_link_on = part_on('L');
    /* the watcher above most of the game's threads, to copy a frame the moment it is shown; it does
     * little: one look per refresh, and the DMA engine copies while it waits */
    g_cap_th = sceKernelCreateThread("PSPExpCapture", capture_thread, 0x18, 0x4000, 0, NULL);
    if (g_cap_th >= 0) sceKernelStartThread(g_cap_th, 0, NULL);
    return 0;
}

/* Everything of the plugin stopped and given back, once. A game's exit restarts the PSP's system
 * without stopping plugins first (module_stop is never called: no line of it in the log); with
 * the plugin's USB link and threads still running then, the PSP switched itself off on "Please
 * wait..." every time (v1-v7, 2026-10-23). So the game's exit call is caught (below) and this
 * runs first. */
/* A game ending (its exit, the restart command): the plugin leaves everything as it is for the
 * system's restart, which wipes it (see exit_note). Only the log is told. */
static void cap_shutdown(const char *why)
{
    cap_log("the game ends (%s): left running for the system's restart", why);
}

/* the game's exit calls (LoadExecForUser: what a game calls from HOME's Exit), through the plugin
 * only to be logged, with their thread. The plugin does nothing else at a game's exit: its code
 * stays loaded (PSP_MODULE_NO_STOP) and keeps running, the USB too, until the system's restart
 * wipes it, as psp-cast does. Stopping the USB there while the receiver still read frames is what
 * switched the PSP off on Castlevania: The Dracula X Chronicles' exits (six times, 2026-10-24:
 * the driver stopped at once, or its transfers cancelled first, the log's last line before the
 * PSP went off); every exit that went well (Burnout Legends) had the receiver no longer reading.
 * The stop that v8 added (2026-10-23) was for a plugin the system could unload: its USB driver
 * then called into freed memory. */
static int (*g_exit_game)(void);
static int (*g_exit_game_status)(int);
static volatile u32 g_exit_calls;
static void exit_note(void)
{
    int k1 = pspSdkSetK1(0);
    g_log_hold = 1;      /* (nothing to the memory stick from here on) */
    g_quit = 1;
    cap_log("exit call %u, thread %x, USB %s", ++g_exit_calls, (u32)sceKernelGetThreadId(), g_usb_up ? "running" : "stopped");   /* (from here every thread of the plugin stops touching anything: no log, no copy, no USB) */
    pspSdkSetK1(k1);
}
static int exit_game(void) { exit_note(); return g_exit_game(); }
static int exit_game_status(int status) { exit_note(); return g_exit_game_status(status); }

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
