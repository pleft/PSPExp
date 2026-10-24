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
#include <pspge.h>
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
#define RING_BYTES (256 * 1024)      /* of the kernel's ~450 KB free while a game runs */
#define MAX_BANDS 8
static int BAND_BYTES = 128 * 1024, NBANDS = 2;   /* (fewer, larger USB transfers cost the PSP less: 128 KB, 25 fps in a race against 23 at 32 KB) */   /* (the band size can be changed between frames: band) */
static volatile int g_band_req;      /* a new band size asked for */
static unsigned char *g_bands;       /* 64-byte aligned */
static volatile u32 g_band_len[MAX_BANDS];
static SceUID g_band_free = -1, g_band_full = -1, g_copy_go = -1;
static SceUID g_block = -1;
static volatile int g_busy;            /* a frame on its way: 1 copied by the copier, 2 made by the GE */
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
static volatile int g_util_active, g_util_closing;   /* a system screen open (the plugin's memory given back) */
/* Experiments (exp, from the receiver), to tell what costs the game its speed: 1 the copy alone
 * (nothing sent), 2 the sending alone (nothing copied: the ring's stale bytes go out). */
static volatile int g_exp;

/* the idle meter: a thread below everything, busy while switched on; its processor time is the
 * time nobody else wanted (how much the game leaves free) */
static volatile int g_idle_on;
static SceUID g_idle_th = -1;
static int idle_thread(SceSize args, void *argp)
{
    volatile u32 spin;
    while (!g_quit) {
        if (!g_idle_on) { sceKernelDelayThread(100000); continue; }
        for (spin = 0; spin < 10000; spin++) ;
    }
    return 0;
}

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

static int ring_alloc(void)
{
    SceUID b = sceKernelAllocPartitionMemory(1, "PSPExpCapture", PSP_SMEM_High, RING_BYTES + 64, NULL);
    if (b < 0) return b;
    g_block = b;
    g_bands = (unsigned char *)(((u32)sceKernelGetBlockHeadAddr(b) + 63) & ~63u);
    sceKernelDcacheInvalidateRange(g_bands, RING_BYTES);   /* (only the DMA engines touch it) */
    return 0;
}

static int snap_alloc(void)
{
    cap_log("kernel memory: %u KB free, largest %u KB", (u32)sceKernelPartitionTotalFreeMemSize(1) / 1024,
            (u32)sceKernelPartitionMaxFreeMemSize(1) / 1024);
    if (ring_alloc() < 0) return -1;
    g_band_free = sceKernelCreateSema("PSPExpCaptureFree", 0, NBANDS, MAX_BANDS, NULL);
    g_band_full = sceKernelCreateSema("PSPExpCaptureFull", 0, 0, MAX_BANDS, NULL);
    g_copy_go = sceKernelCreateSema("PSPExpCaptureCopy", 0, 0, 1, NULL);
    return RING_BYTES / 1024;
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

/* ---- the GE makes the frame 16-bit (ge on) ----
 * What costs a game its speed is the sending: the PSP's USB driver spends processor time on every
 * byte (a race: 30 fps alone; copying every frame out of VRAM, nothing sent, 29.9; sending,
 * nothing copied, 24.5; both, 22-23; larger transfers, 25: 2026-10-24). So fewer bytes, with no
 * processor work: when the game flips, two short GE lists are queued, each with
 * a context of its own (the GE's state saved when it starts and restored when it ends, as the
 * PSP's own dialogs draw over a game; PPSSPP GPUCommon does the same): the first redraws the
 * frame as 565 into the game's depth buffer, the second has the GE's transfer engine copy it out,
 * 480 wide, into kernel memory. Between two frames the depth buffer holds nothing (the next frame
 * clears it before drawing), and it is exactly a 16-bit frame's size (VRAM is otherwise full in a
 * race: frames, depth, textures). The GE takes about 2 ms; the USB sends 255 KB, not 557. */
static volatile int g_gemode;
static PspGeContext g_ge_ctx_r __attribute__((aligned(16))), g_ge_ctx_t __attribute__((aligned(16)));
static u32 g_ge_list_r[64] __attribute__((aligned(16))), g_ge_list_t[16] __attribute__((aligned(16)));
static struct { u16 u, v; s16 x, y, z; } __attribute__((packed)) g_ge_vtx[16] __attribute__((aligned(16)));
static volatile int g_ge_qid = -1, g_frame_ge;
static volatile u32 g_ge_frames, g_ge_skipped, g_ge_fail, g_fb_calls, g_fb_intr, g_ge_step, g_ge_wakes;
static volatile int g_ge_err;
static volatile u32 g_ge_trace;   /* the first frames after "ge" step by step in the log (a crash shows where) */
#define GE16_BYTES (CAP_W * CAP_H * 2)

#define GE(c, a) (((u32)(c) << 24) | ((u32)(a) & 0xFFFFFF))

/* the lists for the frame at top (its stride, its format) via the depth buffer at VRAM + z into
 * g_bands; the transfer's queue id, or <0 */
static int ge_queue(const void *top, int bw, int fmt, u32 z)
{
    u32 src = (u32)top & 0x1FFFFFFF, vtx = (u32)g_ge_vtx & 0x1FFFFFFF, zb = 0x04000000 + z;
    u32 dst = (u32)g_bands & 0x1FFFFFFF, *l = g_ge_list_r;
    int i, n = 0, qt, qr;
    PspGeListArgs args;
    for (i = 0; i < CAP_W; i += 64, n += 2) {   /* strips 64 wide (the texture cache's liking) */
        int x1 = i + 64 < CAP_W ? i + 64 : CAP_W;
        g_ge_vtx[n].u = i; g_ge_vtx[n].v = 0; g_ge_vtx[n].x = i; g_ge_vtx[n].y = 0; g_ge_vtx[n].z = 0;
        g_ge_vtx[n + 1].u = x1; g_ge_vtx[n + 1].v = CAP_H; g_ge_vtx[n + 1].x = x1; g_ge_vtx[n + 1].y = CAP_H; g_ge_vtx[n + 1].z = 0;
    }
    *l++ = GE(0x10, (vtx >> 8) & 0x0F0000);            /* BASE */
    *l++ = GE(0x12, 2 | (2 << 7) | (1 << 23));         /* VERTEXTYPE: 16-bit UV and XYZ, through */
    *l++ = GE(0x9C, z);                                /* FRAMEBUFPTR: the depth buffer */
    *l++ = GE(0x9D, 512);                              /* FRAMEBUFWIDTH */
    *l++ = GE(0xD2, 0);                                /* FRAMEBUFPIXFORMAT: 565 */
    *l++ = GE(0x15, 0); *l++ = GE(0x16, (CAP_W - 1) | ((CAP_H - 1) << 10));   /* REGION */
    *l++ = GE(0xD4, 0); *l++ = GE(0xD5, (CAP_W - 1) | ((CAP_H - 1) << 10));   /* SCISSOR */
    *l++ = GE(0x4C, 0); *l++ = GE(0x4D, 0);            /* OFFSETX, OFFSETY */
    *l++ = GE(0x17, 0); *l++ = GE(0x1D, 0); *l++ = GE(0x1F, 0); *l++ = GE(0x20, 0);   /* no lighting, cull, fog, dither */
    *l++ = GE(0x21, 0); *l++ = GE(0x22, 0); *l++ = GE(0x23, 0); *l++ = GE(0x24, 0);   /* no blend, alpha, depth, stencil tests */
    *l++ = GE(0x25, 0); *l++ = GE(0x27, 0); *l++ = GE(0x28, 0); *l++ = GE(0xD3, 0);   /* no antialias, colour test, logic op, clear mode */
    *l++ = GE(0xE7, 1); *l++ = GE(0xE8, 0); *l++ = GE(0xE9, 0);                       /* no depth writes; all colour bits */
    *l++ = GE(0x1E, 1);                                /* TEXTUREMAPENABLE */
    *l++ = GE(0xC0, 0); *l++ = GE(0xC2, 0); *l++ = GE(0xC3, fmt);   /* UV mapping; plain, one level; the frame's format */
    *l++ = GE(0xA0, src); *l++ = GE(0xA8, bw | ((src >> 8) & 0x0F0000));   /* TEXADDR0, TEXBUFWIDTH0 */
    *l++ = GE(0xB8, 9 | (9 << 8));                     /* TEXSIZE0: 512x512 (the frame inside) */
    *l++ = GE(0xC6, 0); *l++ = GE(0xC7, 1 | (1 << 8)); *l++ = GE(0xC8, 0);   /* nearest, clamped, level 0 */
    *l++ = GE(0xC9, 3);                                /* TEXFUNC: replace */
    *l++ = GE(0xCB, 0);                                /* TEXFLUSH */
    *l++ = GE(0x01, vtx);                              /* VADDR */
    *l++ = GE(0x04, (6 << 16) | n);                    /* PRIM: sprites */
    *l++ = GE(0x0F, 0); *l++ = GE(0x0C, 0);            /* FINISH, END */
    l = g_ge_list_t;                                   /* the copy out, 480 wide, into the ring's memory */
    *l++ = GE(0xB2, zb); *l++ = GE(0xB3, 512 | ((zb >> 8) & 0x0F0000));    /* TRXSBP, TRXSBW */
    *l++ = GE(0xB4, dst); *l++ = GE(0xB5, CAP_W | ((dst >> 8) & 0x0F0000));  /* TRXDBP, TRXDBW */
    *l++ = GE(0xEB, 0); *l++ = GE(0xEC, 0);            /* TRXSPOS, TRXDPOS */
    *l++ = GE(0xEE, (CAP_W - 1) | ((CAP_H - 1) << 10));   /* TRXSIZE */
    *l++ = GE(0xEA, 0);                                /* TRXKICK: 16-bit */
    *l++ = GE(0x0F, 0); *l++ = GE(0x0C, 0);
    sceKernelDcacheWritebackRange(g_ge_list_r, sizeof(g_ge_list_r));
    sceKernelDcacheWritebackRange(g_ge_list_t, sizeof(g_ge_list_t));
    sceKernelDcacheWritebackRange(g_ge_vtx, sizeof(g_ge_vtx));
    sceKernelDcacheWritebackInvalidateRange(g_bands, GE16_BYTES);   /* (nothing of ours cached over what the GE writes) */
    /* queued at the back, the drawing then the copy: they run as soon as the game's list now
     * drawing ends, which leaves the depth buffer free, and before the game's list for the frame
     * after next (queued behind them), which draws into this frame's buffer again. (At the front,
     * sceGeListEnQueueHead, is refused while a list draws, and each refusal loses one of the GE's
     * 64 list slots for good, as uOFW's ge.c shows: after 64 every enqueue failed, 2026-10-24.) */
    qr = qt = 0;
    if (g_gemode >= 4) {   /* fault finding: 4 an empty list with a context, 5 the drawing without one, 6 an empty one without */
        static u32 empty[4] __attribute__((aligned(16)));
        empty[0] = GE(0x0F, 0); empty[1] = GE(0x0C, 0);
        sceKernelDcacheWritebackRange(empty, sizeof(empty));
        memset(&args, 0, sizeof(args)); args.size = sizeof(args); args.context = &g_ge_ctx_r;
        g_ge_step = 3;
        qr = sceGeListEnQueue(g_gemode == 5 ? (void *)g_ge_list_r : (void *)empty, NULL, -1, g_gemode == 4 ? &args : NULL);
        g_ge_step = 4; g_ge_err = qr;
        if (g_ge_trace) cap_log("GE frame %u: test %d queued: %x", g_ge_frames, g_gemode, (u32)qr);
        return qr;
    }
    if (g_gemode & 1) {   /* (ge 1: only the drawing, ge 2: only the copy, ge 3: both; the steps found a crash) */
        memset(&args, 0, sizeof(args)); args.size = sizeof(args); args.context = &g_ge_ctx_r;
        g_ge_step = 3;
        qr = sceGeListEnQueue(g_ge_list_r, NULL, -1, &args);
        g_ge_step = 4; g_ge_err = qr;
        if (g_ge_trace) cap_log("GE frame %u: drawing queued: %x (top %x, z %x)", g_ge_frames, (u32)qr, (u32)top, z);
        if (qr < 0) return qr;
        qt = qr;
    }
    if (g_gemode & 2) {
        memset(&args, 0, sizeof(args)); args.size = sizeof(args); args.context = &g_ge_ctx_t;
        g_ge_step = 5;
        qt = sceGeListEnQueue(g_ge_list_t, NULL, -1, &args);
        g_ge_step = 6; if (qt < 0) g_ge_err = qt;
        if (g_ge_trace) cap_log("GE frame %u: copy queued: %x", g_ge_frames, (u32)qt);
        if (qt < 0) { if (qr > 0) sceGeListSync(qr, 0); return qt; }
    }
    return qt;
}

/* the game's flip: the frame it just finished is handed to the GE (ge on). Burnout Legends flips
 * from an interrupt (896 of 897 flips, 2026-10-24), where lists cannot be queued and no thread can
 * be woken (an event flag set there never woke one: the plugin stalled): the flip only leaves a
 * note, and the watcher, awake at every refresh, queues the lists (ge_take). Either way they run
 * before the game draws into that frame again: it is shown until the next flip, and the game's
 * list for the frame after next is queued behind them. */
static volatile int g_paused, g_ge_pending;
static const void *volatile g_ge_top;
static volatile int g_ge_bw, g_ge_fmt;
static int (*o_setfb)(void *, int, int, int);
static int h_setfb(void *top, int bw, int fmt, int sync)
{
    int r = o_setfb(top, bw, fmt, sync);
    g_fb_calls++;
    if (sceKernelIsIntrContext()) g_fb_intr++;
    if (r >= 0 && g_gemode && top && !g_paused && !g_util_active && pspexpUsbConnected() && (int)g_generation != g_dead_gen) {
        if (g_busy) g_ge_skipped++;
        else {
            g_ge_top = top; g_ge_bw = bw; g_ge_fmt = fmt;
            g_frame_time = now_us(); g_frame_changes = g_changes;
            g_busy = 2;   /* (2: the GE's; 1: the copier's, a frame the sender takes band by band) */
            g_ge_pending = 1;
        }
    }
    return r;
}

/* the noted frame's lists queued (the watcher's, at a refresh) */
static void ge_take(void)
{
    const unsigned char *top = g_ge_top;
    u32 z, tb, te;
    int q;
    g_ge_pending = 0; g_ge_wakes++; g_ge_step = 1;
    z = sceGeGetCmd(0x9E) & 0xFFFFF0;
    g_ge_step = 2;
    tb = (u32)top & 0x1FFFFF; te = tb + (u32)g_ge_bw * CAP_H * (g_ge_fmt == 3 ? 4 : 2);
    if (z + 512 * CAP_H * 2 > 0x200000 || (z < te && z + 512 * CAP_H * 2 > tb)) { g_ge_fail++; g_busy = 0; return; }   /* (no depth buffer of its own) */
    q = ge_queue(top, g_ge_bw, g_ge_fmt, z);
    if (q < 0) { g_ge_fail++; g_busy = 0; return; }
    g_ge_qid = q; g_frame_ge = 1;
    g_frame_bw = CAP_W; g_frame_fmt = PSP_DISPLAY_PIXEL_FORMAT_565;
    g_frame_seq = g_ge_frames++;
    sceKernelSetEventFlag(g_send_ev, SEND_FRAME);
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
            if (g_exp != 2) sceDmacMemcpy(g_bands + slot * BAND_BYTES, g_frame_src + y * stride, n * stride);
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

static SceUID g_ablock = -1;
static int aring_alloc(void)
{
    SceUID b = sceKernelAllocPartitionMemory(1, "PSPExpCaptureSound", PSP_SMEM_High, AUDIO_RING + 64, NULL);
    if (b < 0) return b;
    g_ablock = b;
    g_atail = g_ahead;
    g_aring = (unsigned char *)(((u32)sceKernelGetBlockHeadAddr(b) + 63) & ~63u);
    return 0;
}

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
    if (!g_aring) {   /* (given back meanwhile: a system screen) */
    } else if (AUDIO_RING - (g_ahead - g_atail) < 32 + n) {
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

/* a user syscall of the firmware's, through the plugin: its own address kept in *orig, and the
 * pair kept so the plugin can put every one back when it stops (unredirect_all) */
static struct { void *orig, *hook; } g_redirects[64];
static int g_nredirects;

static int redirect(const char *mod, const char *lib, u32 nid, void *hook, void *orig)
{
    void *f = (void *)sctrlHENFindFunction((char *)mod, (char *)lib, nid);
    if (!f || g_nredirects >= 64) return 0;
    *(void **)orig = f;
    sctrlHENPatchSyscall(f, hook);
    g_redirects[g_nredirects].orig = f; g_redirects[g_nredirects].hook = hook; g_nredirects++;
    return 1;
}

/* The firmware's own calls back in place: nothing of the plugin runs once it has stopped. A game
 * that opened a system screen on its way out (Castlevania: The Dracula X Chronicles) ran the
 * plugin's screen handling after the plugin had given back its memory, which then gave it back a
 * second time: the PSP switched off on HOME > Exit (2026-10-24). */
static void unredirect_all(void)
{
    int i;
    for (i = 0; i < g_nredirects; i++) sctrlHENPatchSyscall(g_redirects[i].hook, g_redirects[i].orig);
    g_nredirects = 0;
}

static void catch_audio(void)
{
    int n = 0;
    if (aring_alloc() < 0) { cap_log("sound: no memory for its ring"); return; }
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

/* ---- measuring the game's drawing commands (a feasibility test, 2026-10-24) ----
 * Instead of the finished picture, the game's GE commands could be recorded and drawn again on the
 * receiver (PPSSPP can replay a GE frame dump), at any resolution. Whether that is lighter than a
 * 557 KB frame depends on how much the game hands the GE each frame: this walks every command
 * list the game queues or extends (its calls caught as the sound ones) and adds up, per frame, the
 * command bytes, the vertex bytes behind each draw, the textures used for the first time (in main
 * memory: they would have to be sent once; in VRAM: drawn by the game itself) and the game's own
 * block transfers. On only while switched on (gestat); the walk's own time is counted too. */
static volatile int g_gestat;
static volatile u32 g_gs_lists, g_gs_cmds, g_gs_prims, g_gs_vtx, g_gs_idx, g_gs_tex_ram, g_gs_tex_vram, g_gs_trx, g_gs_clut, g_gs_us, g_gs_bad;
static struct { int qid; u32 pos, base, offset, vtype, tme, tbp, tbw, tsize, tpsm, cbp, trx_src, trx_size, trx_bw; } g_gl[8];
#define TEXSEEN 2048
static u32 g_texseen[TEXSEEN];

static int addr_ok(u32 a) { a &= 0x1FFFFFFF; return (a >= 0x08000000 && a < 0x0A000000) || (a >= 0x04000000 && a < 0x04200000); }
static u32 rd(u32 a) { return *(volatile u32 *)((a & 0x1FFFFFFF) | 0x80000000); }   /* (cached, as the game wrote it) */

static u32 vtx_size(u32 vt)
{
    static const u8 tcs[4] = {0, 1, 2, 4}, cs[8] = {0, 0, 0, 0, 2, 2, 2, 4}, ns[4] = {0, 1, 2, 4};
    u32 size = 0, big = 1, c, wc = ((vt >> 14) & 7) + 1;
#define ADD(n, each) do { c = (each); if (c) { size = (size + c - 1) & ~(c - 1); size += (n) * c; if (c > big) big = c; } } while (0)
    ADD(wc, tcs[(vt >> 9) & 3]);    /* weights */
    ADD(2, tcs[vt & 3]);            /* texture coordinates */
    ADD(1, cs[(vt >> 2) & 7]);      /* colour */
    ADD(3, ns[(vt >> 5) & 3]);      /* normal */
    ADD(3, ns[(vt >> 7) & 3]);      /* position */
#undef ADD
    return (size + big - 1) & ~(big - 1);
}

static void tex_used(u32 tbp, u32 tbw, u32 tsize, u32 tpsm)
{
    u32 addr = (tbp & 0xFFFFF0) | ((tbw << 8) & 0x0F000000), key = addr ^ (tsize << 4) ^ tpsm, h = (key * 2654435761u) >> 21, i;
    u32 w = 1u << (tsize & 15), hh = 1u << ((tsize >> 8) & 15);
    static const u8 bits[11] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8};
    for (i = 0; i < 16; i++, h = (h + 1) % TEXSEEN) {
        if (g_texseen[h] == key) return;
        if (!g_texseen[h]) break;
    }
    if (i == 16) return;
    g_texseen[h] = key;
    if ((addr & 0x1FFFFFFF) < 0x08000000) g_gs_tex_vram++;
    else g_gs_tex_ram += w * hh * bits[tpsm < 11 ? tpsm : 3] / 8;
}

/* the commands of list slot k from its position up to `stop` (0: until END) */
static void gs_walk(int k, u32 stop)
{
    u32 pc = g_gl[k].pos, stack[16], n = 0;
    int sp = 0;
    u32 t = now_us();
    while (n++ < 200000) {
        u32 op, cmd, arg, target;
        if (stop && pc == stop && sp == 0) break;
        if (!addr_ok(pc)) { g_gs_bad++; break; }
        op = rd(pc); cmd = op >> 24; arg = op & 0xFFFFFF;
        pc += 4; g_gs_cmds++;
        target = (((g_gl[k].base << 8) & 0x0F000000) | arg) + g_gl[k].offset;
        switch (cmd) {
        case 0x10: g_gl[k].base = arg; break;                              /* BASE */
        case 0x13: g_gl[k].offset = arg << 8; break;                       /* OFFSETADDR */
        case 0x14: g_gl[k].offset = pc - 4; break;                         /* ORIGIN */
        case 0x12: g_gl[k].vtype = arg; break;                             /* VERTEXTYPE */
        case 0x1E: g_gl[k].tme = arg & 1; break;
        case 0xA0: g_gl[k].tbp = arg; break;
        case 0xA8: g_gl[k].tbw = arg; break;
        case 0xB8: g_gl[k].tsize = arg; break;
        case 0xC3: g_gl[k].tpsm = arg & 15; break;
        case 0xC4: g_gs_clut += (arg & 0x3F) * 32; break;                  /* LOADCLUT: 32-byte blocks */
        case 0xB2: g_gl[k].trx_src = arg; break;
        case 0xB3: g_gl[k].trx_bw = arg; break;
        case 0xEE: g_gl[k].trx_size = arg; break;
        case 0xEA:                                                         /* TRANSFERSTART */
            g_gs_trx += ((g_gl[k].trx_size & 0x3FF) + 1) * (((g_gl[k].trx_size >> 10) & 0x3FF) + 1) * (arg & 1 ? 4 : 2);
            break;
        case 0x04: case 0x05: case 0x06: {                                 /* PRIM, BEZIER, SPLINE */
            u32 count = cmd == 0x04 ? arg & 0xFFFF : (arg & 0xFF) * ((arg >> 8) & 0xFF);
            u32 vt = g_gl[k].vtype, morph = ((vt >> 18) & 7) + 1, ix = (vt >> 11) & 3;
            g_gs_prims++;
            g_gs_vtx += count * vtx_size(vt) * morph;   /* (indexed: every reference, an upper bound) */
            if (ix) g_gs_idx += count * (ix == 1 ? 1 : 2);
            if (g_gl[k].tme) tex_used(g_gl[k].tbp, g_gl[k].tbw, g_gl[k].tsize, g_gl[k].tpsm);
            break;
        }
        case 0x08: pc = target; break;                                     /* JUMP */
        case 0x0A: if (sp < 16) { stack[sp++] = pc; pc = target; } break;  /* CALL */
        case 0x0B: if (sp > 0) pc = stack[--sp]; break;                    /* RET */
        case 0x0C: if (sp == 0) goto done; break;                          /* END */
        }
    }
done:
    g_gl[k].pos = pc;
    g_gs_us += now_us() - t;
}

static int gs_slot(int qid)
{
    int k;
    for (k = 0; k < 8; k++) if (g_gl[k].qid == qid) return k;
    return -1;
}

static int (*o_ge_enq)(const void *, void *, int, void *), (*o_ge_enqh)(const void *, void *, int, void *), (*o_ge_stall)(int, void *);
static int gs_enqueued(int qid, const void *list, void *stall)
{
    static int next;
    int k;
    if (qid < 0 || !g_gestat) return qid;
    k = next++ % 8;
    memset(&g_gl[k], 0, sizeof(g_gl[k]));
    g_gl[k].qid = qid; g_gl[k].pos = (u32)list;
    g_gs_lists++;
    {
        int k1 = pspSdkSetK1(0);
        gs_walk(k, stall ? (u32)stall : 0);
        pspSdkSetK1(k1);
    }
    return qid;
}
static int h_ge_enq(const void *l, void *st, int cb, void *a) { return gs_enqueued(o_ge_enq(l, st, cb, a), l, st); }
static int h_ge_enqh(const void *l, void *st, int cb, void *a) { return gs_enqueued(o_ge_enqh(l, st, cb, a), l, st); }
static int h_ge_stall(int qid, void *stall)
{
    int r = o_ge_stall(qid, stall), k;
    if (r >= 0 && g_gestat && (k = gs_slot(qid)) >= 0) {
        int k1 = pspSdkSetK1(0);
        gs_walk(k, (u32)stall);
        pspSdkSetK1(k1);
    }
    return r;
}

static void catch_ge(void)
{
    int n = 0;
    n += redirect("sceGE_Manager", "sceGe_user", 0xAB49E76A, h_ge_enq, &o_ge_enq);      /* sceGeListEnQueue */
    n += redirect("sceGE_Manager", "sceGe_user", 0x1C0D95A6, h_ge_enqh, &o_ge_enqh);    /* ...EnQueueHead */
    n += redirect("sceGE_Manager", "sceGe_user", 0xE0D68148, h_ge_stall, &o_ge_stall);  /* ...UpdateStallAddr */
    cap_log("GE lists: %d of 3 calls caught", n);
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

static void util_open(void)
{
    int k1, i;
    u32 intr;
    if (g_quit) return;
    k1 = pspSdkSetK1(0);
    g_util_closing = 0;
    if (!g_util_active) {
        g_util_active = 1;   /* (the watcher hands no more frames) */
        for (i = 0; i < 40 && g_busy; i++) sceKernelDelayThread(5000);
        {
            SceUInt t = 200000;
            int locked = g_tx_lock >= 0 && sceKernelWaitSema(g_tx_lock, 1, &t) >= 0;
            intr = pspSdkDisableInterrupts();
            g_aring = NULL;
            pspSdkEnableInterrupts(intr);
            if (locked) sceKernelSignalSema(g_tx_lock, 1);
        }
        if (g_ablock >= 0) { sceKernelFreePartitionMemory(g_ablock); g_ablock = -1; }
        if (!g_busy && g_block >= 0) { sceKernelFreePartitionMemory(g_block); g_block = -1; g_bands = NULL; }
        cap_log("a system screen opens: memory given back (%u KB free)", (u32)sceKernelPartitionTotalFreeMemSize(1) / 1024);
    }
    pspSdkSetK1(k1);
}

static void util_closed(void)
{
    if (g_quit) return;
    g_util_end = now_us();
    g_util_closing = 1;
}

/* the watcher's, at a refresh: the memory taken back once a screen has been closed for 1.5 s */
static void util_check(void)
{
    if (g_quit || !g_util_active || !g_util_closing || now_us() - g_util_end < 1500000) return;
    g_util_closing = 0;
    if (g_block < 0 && ring_alloc() < 0) return;   /* (not yet: tried again at the next refresh) */
    if (g_ablock < 0) aring_alloc();
    g_util_active = 0;
    cap_log("the system screen closed: recording again");
}

static int (*o_sd_in)(void *), (*o_msg_in)(void *), (*o_osk_in)(void *), (*o_net_in)(void *), (*o_gs_in)(void *),
           (*o_html_in)(void *), (*o_shot_in)(void *), (*o_gdi_in)(void *);
static int (*o_sd_out)(void), (*o_msg_out)(void), (*o_osk_out)(void), (*o_net_out)(void), (*o_gs_out)(void),
           (*o_html_out)(void), (*o_shot_out)(void), (*o_gdi_out)(void);
#define UTIL_PAIR(name) \
    static int h_##name##_in(void *p) { int r; util_open(); r = o_##name##_in(p); if (r < 0) util_closed(); return r; } \
    static int h_##name##_out(void) { int r = o_##name##_out(); util_closed(); return r; }
UTIL_PAIR(sd) UTIL_PAIR(msg) UTIL_PAIR(osk) UTIL_PAIR(net) UTIL_PAIR(gs) UTIL_PAIR(html) UTIL_PAIR(shot) UTIL_PAIR(gdi)

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
    cap_log("system screens: %d of 16 calls caught", n);
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
#define CMD_GESTAT 9     /* u32 1: measure the game's GE commands (gestat), 0: not */
#define CMD_EXP 10       /* u32 the experiment (g_exp) */
#define CMD_COPYPRIO 11  /* u32 the copier's priority */
#define CMD_BAND 12      /* u32 the band size (32, 64 or 128 KB) */
#define CMD_TX 13        /* u32 the transfer size, u32 split (see pspexpUsbSetTx) */
#define CMD_IDLE 14      /* u32 1: the idle meter on */
#define CMD_GE 8         /* u32 1: the GE makes each frame 16-bit (g_gemode), 0: copied as drawn */
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
    {   /* (a link held for over a second: the reply is dropped, the commands go on) */
        SceUInt t = 1000000;
        if (sceKernelWaitSema(g_tx_lock, 1, &t) < 0) { cap_log("reply %u dropped: the link is held", cmd); return; }
    }
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
        } else if (cmd == CMD_GESTAT && len >= 4 && cmd_read(g_cmd_buf, 4) == 0) {
            g_gestat = get32(g_cmd_buf) != 0;
            cap_log(g_gestat ? "measuring the GE commands" : "not measuring the GE commands");
        } else if (cmd == CMD_EXP && len >= 4 && cmd_read(g_cmd_buf, 4) == 0) {
            g_exp = (int)get32(g_cmd_buf);
            cap_log("experiment %d (1 the copy alone, 2 the sending alone)", g_exp);
        } else if (cmd == CMD_COPYPRIO && len >= 4 && cmd_read(g_cmd_buf, 4) == 0) {
            int pr = (int)get32(g_cmd_buf);
            cap_log("copier priority %x: %x", pr, (u32)sceKernelChangeThreadPriority(g_copy_th, pr));
        } else if (cmd == CMD_BAND && len >= 4 && cmd_read(g_cmd_buf, 4) == 0) {
            u32 b = get32(g_cmd_buf);
            if (b == 32768 || b == 65536 || b == 131072) g_band_req = b;
        } else if (cmd == CMD_TX && len >= 8 && cmd_read(g_cmd_buf, 8) == 0) {
            u32 c = get32(g_cmd_buf);
            if (c >= 4096 && c <= 262144) { pspexpUsbSetTx((int)c, get32(g_cmd_buf + 4) != 0); cap_log("transfers of up to %u KB, split %u", c / 1024, get32(g_cmd_buf + 4)); }
        } else if (cmd == CMD_IDLE && len >= 4 && cmd_read(g_cmd_buf, 4) == 0) {
            g_idle_on = get32(g_cmd_buf) != 0;
        } else if (cmd == CMD_VRAM) {
            vram_probe(g_top);
            cmd_reply("PXCR", CMD_VRAM, 0, g_vram_info, sizeof(g_vram_info));
        } else if (cmd == CMD_GE && len >= 4 && cmd_read(g_cmd_buf, 4) == 0) {
            g_gemode = (int)(get32(g_cmd_buf) & 7);
            g_ge_trace = 3;
            cap_log("GE mode %d (1 the drawing only, 2 the copy only, 3 both; 0 frames copied as drawn)", g_gemode);
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
        if (g_busy && g_frame_ge) {   /* the GE's 16-bit frame: once its copy out has run */
            unsigned char *h = g_hdr;
            u32 ts = now_us();
            r = sceGeListSync(g_ge_qid, 0) >= 0;
            if (g_ge_trace) { cap_log("GE frame %u: done (%d)", g_frame_seq, r); g_ge_trace--; }
            if (!(g_gemode & 2) || g_gemode >= 4) {   /* (ge 1 and the tests: nothing to send) */
                g_frame_ge = 0; g_busy = 0;
                continue;
            }
            memset(h, 0, 32);
            memcpy(h, "PXCF", 4);
            put16(h + 4, CAP_W); put16(h + 6, CAP_H); put16(h + 8, PSP_DISPLAY_PIXEL_FORMAT_565);
            put32(h + 12, g_frame_seq); put32(h + 16, g_frame_changes); put32(h + 20, g_frame_time); put32(h + 24, GE16_BYTES);
            sceKernelWaitSema(g_tx_lock, 1, NULL);
            if (r) r = pspexpUsbWrite(h, 32) >= 0 && pspexpUsbWrite(g_bands, GE16_BYTES) >= 0;
            sceKernelSignalSema(g_tx_lock, 1);
            if (r) { g_sent++; g_send_us += now_us() - ts; }
            else { g_dead_gen = gen; cap_log("receiver not reading: paused until one connects"); }
            g_frame_ge = 0;
            g_busy = 0;
        } else if (g_busy == 1) {   /* the frame's header, then its bands as the copier fills them */
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
            r = g_exp == 1 || pspexpUsbWrite(h, 32) >= 0;
            for (y = 0; y < CAP_H; y += rows, k++) {   /* every band taken, sent or not, so the copier goes on */
                int slot = k % NBANDS;
                if (take(g_band_full) < 0) break;
                if (r && g_exp != 1) r = pspexpUsbWrite(g_bands + slot * BAND_BYTES, g_band_len[slot]) >= 0;
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
    g_idle_th = sceKernelCreateThread("PSPExpCaptureIdle", idle_thread, 0x7E, 0x800, 0, NULL);
    if (g_idle_th >= 0) sceKernelStartThread(g_idle_th, 0, NULL);
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
    pspexpUsbSetTx(128 * 1024, 0);   /* (a stream read without pause: no short-ending splits) */
    g_cmd_th = sceKernelCreateThread("PSPExpCaptureCmd", cmd_thread, 0x30, 0x2000, 0, NULL);
    if (g_cmd_th >= 0) sceKernelStartThread(g_cmd_th, 0, NULL);

    t0 = now_us();
    while (!g_quit) {
        sceDisplayWaitVblankStart();
        if (g_ge_pending) ge_take();
        util_check();
        if (sceDisplayGetFrameBuf(&top, &bw, &fmt, 0) < 0 || !top) continue;
        if (top != last) {
            g_changes++; last = top; g_top = top;
            if (pspexpUsbConnected()) {
                if (!was_connected) { cap_log("receiver connected"); was_connected = 1; }
                if (g_paused || g_gemode || g_util_active || (int)g_generation == g_dead_gen) ;   /* (nobody reading: nothing copied; ge: the flip hands them) */
                else if (!g_busy && g_band_req && g_band_req != BAND_BYTES) {   /* a new band size, between frames */
                    int nb = RING_BYTES / g_band_req;
                    sceKernelDeleteSema(g_band_free);
                    g_band_free = sceKernelCreateSema("PSPExpCaptureFree", 0, nb, MAX_BANDS, NULL);
                    BAND_BYTES = g_band_req; NBANDS = nb;
                    cap_log("bands of %u KB, %d of them", BAND_BYTES / 1024, NBANDS);
                }
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
            {   /* the processor time each of the plugin's threads really used (against their wall time) */
                static u32 cc0, cs0, ci0, cw0;
                u32 cc = cpu_us(g_copy_th), cs = cpu_us(g_send_th), ci = cpu_us(g_idle_th), cw = cpu_us(g_cap_th);
                cap_log("CPU in 5 s: copier %u ms, sender %u ms, watcher %u ms, idle %u ms; exp %d, bands %u KB",
                        (cc - cc0) / 1000, (cs - cs0) / 1000, (cw - cw0) / 1000, (ci - ci0) / 1000, g_exp, BAND_BYTES / 1024);
                cc0 = cc; cs0 = cs; ci0 = ci; cw0 = cw;
            }
            if (g_gestat && g_changes - c0) {
                u32 f = g_changes - c0;
                cap_log("GE a frame: %u lists, %u commands (%u KB), %u draws, vertices %u KB, indices %u KB, new textures %u KB in RAM + %u in VRAM, transfers %u KB, CLUTs %u KB, walk %u us, bad %u",
                        g_gs_lists / f, g_gs_cmds / f, g_gs_cmds * 4 / 1024 / f, g_gs_prims / f, g_gs_vtx / 1024 / f, g_gs_idx / 1024 / f,
                        g_gs_tex_ram / 1024 / f, g_gs_tex_vram, g_gs_trx / 1024 / f, g_gs_clut / 1024 / f, g_gs_us / f, g_gs_bad);
                g_gs_lists = g_gs_cmds = g_gs_prims = g_gs_vtx = g_gs_idx = g_gs_tex_ram = g_gs_tex_vram = g_gs_trx = g_gs_clut = g_gs_us = 0;
            }
            if (g_gemode) cap_log("GE frames: %u, skipped %u, not possible %u; flips %u (%u in interrupts); woken %u, step %u, result %x, busy %d",
                                  g_ge_frames, g_ge_skipped, g_ge_fail, g_fb_calls, g_fb_intr, g_ge_wakes, g_ge_step, (u32)g_ge_err, g_busy);
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
    catch_ge();
    catch_util();
    if (redirect("sceDisplay_Service", "sceDisplay", 0x289D82FE, h_setfb, &o_setfb))   /* sceDisplaySetFrameBuf */
        cap_log("the game's flip caught");
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
static void cap_shutdown(const char *why)
{
    static int done;
    u32 t0 = now_us();
    if (done) return;
    done = 1;
    g_quit = 1;
    /* Quickly: a game's exit gives it a limited time, and a shutdown that waited for each thread
     * (up to half a second each) let the PSP force the exit meanwhile, which switched it off
     * (Castlevania: The Dracula X Chronicles, on every HOME > Exit; without the plugin it exits
     * cleanly, 2026-10-24). What must happen before the system restarts: the firmware's calls back
     * in place, the USB link stopped, the plugin's threads ended; the memory and the kernel
     * objects go with the restart. */
    unredirect_all();
    if (g_usb_up) pspexpUsbStop();
    sceUsbbdUnregister(&g_driver);
    {
        SceUID *th[] = { &g_cap_th, &g_send_th, &g_copy_th, &g_idle_th, &g_cmd_th, &g_power_th };
        int i;
        for (i = 0; i < 6; i++)
            if (*th[i] >= 0 && *th[i] != sceKernelGetThreadId()) { sceKernelTerminateDeleteThread(*th[i]); *th[i] = -1; }
    }
    if (g_power_slot >= 0) scePowerUnregisterCallback(g_power_slot);
    cap_log("stopped (%s) in %u us", why, now_us() - t0);
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
