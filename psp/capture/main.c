/*
 * PSPExp Capture: a kernel plugin for custom firmware (PRO/ARK: ms0:/seplugins/game.txt) that
 * sends a native PSP game's picture over USB to a PSPExp receiver (tools/pspexp-capture), so a
 * board or a computer can record it. Stage 1 (the prototype): the picture only, no sound yet.
 *
 * A watcher thread looks at the frame being shown once per screen refresh (sceDisplayGetFrameBuf)
 * and, when the game has put up a new one, copies it at once as 16 bits (snap_frame); a sender
 * thread writes the copy to the USB link, and frames that come while it is busy are skipped. It
 * also counts the game's own frame changes (its frame rate, with and without a receiver), for
 * measuring what the capture costs it. Its log: ms0:/seplugins/pspexp_capture.log. The game's exit
 * is caught so the plugin stops before the system restarts (cap_shutdown). What was tried and
 * measured: docs/PSPExp_Technical_Architecture.md section 12b.
 *
 * The USB link is PSPExp's own (psp/transport-usb, protocol/pspexp_usb_link.h), built in here
 * rather than loaded as a module: one file to install, and nothing else of PSPExp needed on the
 * PSP while a game runs.
 *
 * Each frame on the link: a 32-byte header, then height rows of width pixels in 16 bits (the
 * game's own 16-bit format, or 565 for a 32-bit game; stride removed):
 *   "PXCF", u16 width, u16 height, u16 format (0 565, 1 5551, 2 4444, 3 8888), u16 reserved,
 *   u32 sequence, u32 frame changes so far, u32 time shown (us), u32 payload bytes, u32 reserved[2]
 */
#define PSPEXP_USB_EMBEDDED
#define PSPEXP_USB_RX_SIZE 512   /* it only ever receives the receiver's 16-byte hello */
#include "../transport-usb/main.c"

#include <pspdisplay.h>
#include <pspdisplay_kernel.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <pspdmac.h>
#include <pspsysmem_kernel.h>
#include <psputilsforkernel.h>
#include <psppower.h>
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

/* The snapshot: right after the game puts up a new frame, it is copied out as 480x272 in 16 bits
 * (32-bit frames turned into 565), before the game can draw into it again; the sender then takes
 * its time with the copy. Copied in bands of a few rows: the DMA engine brings each band into a
 * small buffer, the CPU packs it into the frame. v1 copied the shown buffer row by row while
 * sending, slower than a frame: the game swapped and drew into it meanwhile (torn and half-drawn
 * frames, 2026-10-23).
 *
 * All of it in the kernel's memory (315 KB in one piece while a game runs): a whole 32-bit frame
 * (544 KB) fits nowhere that is free. In the game's memory (v2-v4) Burnout Legends hung loading a
 * saved profile (the PSP's save screen takes its memory there too); in the 4 MB volatile area
 * (v5) it froze on its splash screen (the game takes that area as it starts). 2026-10-23. */
#define BAND_BYTES (16 * 1024)                       /* 8 rows of a 512-wide 32-bit frame */
#define FRAME_BYTES (CAP_W * CAP_H * 2)
static unsigned char *g_frame, *g_band;              /* 64-byte aligned */
static SceUID g_block = -1;
static volatile int g_busy;            /* the sender has the frame */
static int g_frame_fmt;
static u32 g_frame_seq, g_frame_changes, g_frame_time;
static SceUID g_snap_sema = -1;
static volatile int g_quit;   /* the game is ending */
static void catch_exit(void);
static SceUID g_cap_th = -1, g_send_th = -1, g_power_th = -1, g_power_cb = -1;
static int g_power_slot = -1;
static int g_usb_up;
static volatile u32 g_changes, g_sent, g_skipped, g_copy_us, g_dma_us;

static int snap_alloc(void)
{
    SceUID b;
    cap_log("kernel memory: %u KB free, largest %u KB", (u32)sceKernelPartitionTotalFreeMemSize(1) / 1024,
            (u32)sceKernelPartitionMaxFreeMemSize(1) / 1024);
    b = sceKernelAllocPartitionMemory(1, "PSPExpCapture", PSP_SMEM_High, FRAME_BYTES + BAND_BYTES + 64, NULL);
    if (b < 0) return -1;
    g_block = b;
    g_frame = (unsigned char *)(((u32)sceKernelGetBlockHeadAddr(b) + 63) & ~63u);
    g_band = g_frame + FRAME_BYTES;
    return (FRAME_BYTES + BAND_BYTES) / 1024;
}

/* 8888 to 565 by the VFPU, four pixels an instruction (vt5650.q; red in the low bits, as the PSP
 * keeps 565): half the bytes on the link. In C, two pixels at a time, a frame took 12-16 ms of the
 * watcher's time (v6-v7, 2026-10-23). src 16-byte aligned (the band), n a multiple of 4; the
 * thread needs PSP_THREAD_ATTR_VFPU. */
static void pack_565(unsigned char *dst, const unsigned char *src, int n)
{
    int i;
    for (i = 0; i < n; i += 4, src += 16, dst += 8)
        __asm__ volatile("lv.q C000, 0(%1)\n"
                         "vt5650.q C100, C000\n"
                         "sv.s S100, 0(%0)\n"
                         "sv.s S101, 4(%0)\n" : : "r"(dst), "r"(src) : "memory");
}

/* the shown frame into g_frame, band by band; the format it is in now */
static int snap_frame(void *top, int bw, int fmt)
{
    int bpp = fmt == PSP_DISPLAY_PIXEL_FORMAT_8888 ? 4 : 2;
    int stride = bw * bpp, rows = BAND_BYTES / stride, y, i;
    const unsigned char *src = (const unsigned char *)((u32)top & ~0x40000000u);
    for (y = 0; y < CAP_H; y += rows) {
        int n = CAP_H - y < rows ? CAP_H - y : rows;
        u32 t = now_us();
        sceDmacMemcpy(g_band, src + y * stride, n * stride);
        g_dma_us += now_us() - t;
        sceKernelDcacheInvalidateRange(g_band, n * stride);   /* the CPU reads what the DMA wrote */
        for (i = 0; i < n; i++) {
            if (bpp == 4) pack_565(g_frame + (y + i) * CAP_W * 2, g_band + i * stride, CAP_W);
            else memcpy(g_frame + (y + i) * CAP_W * 2, g_band + i * stride, CAP_W * 2);
        }
    }
    return bpp == 4 ? PSP_DISPLAY_PIXEL_FORMAT_565 : fmt;
}

/* the sender: the frame over the link (the driver cuts it in 64 KB pieces). A write that times out
 * means the receiver went (the cable stays plugged in): nothing more is sent until a receiver
 * syncs again (a new link generation), instead of waiting out a 3 s timeout per frame. */
static unsigned char g_hdr[64] __attribute__((aligned(64)));

static int send_thread(SceSize args, void *argp)
{
    int dead_gen = -1;
    for (;;) {
        sceKernelWaitSema(g_snap_sema, 1, NULL);
        if (g_quit) break;
        if (pspexpUsbConnected() && (int)g_generation != dead_gen) {
            unsigned char *h = g_hdr;
            memset(h, 0, 32);
            memcpy(h, "PXCF", 4);
            put16(h + 4, CAP_W); put16(h + 6, CAP_H); put16(h + 8, g_frame_fmt);
            put32(h + 12, g_frame_seq); put32(h + 16, g_frame_changes); put32(h + 20, g_frame_time); put32(h + 24, FRAME_BYTES);
            if (pspexpUsbWrite(h, 32) >= 0 && pspexpUsbWrite(g_frame, FRAME_BYTES) >= 0) g_sent++;
            else { dead_gen = (int)g_generation; cap_log("receiver not reading: paused until one connects"); }
        }
        g_busy = 0;
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
 * and, when the sender is free and a receiver is there, snapshotted for it */
static int capture_thread(SceSize args, void *argp)
{
    void *top = NULL, *last = NULL;
    int bw = 0, fmt = 0, r, was_connected = 0, part;
    u32 seq = 0, t0, c0 = 0, s0 = 0, k0 = 0, o0 = 0, q0 = 0, u0 = 0, d0 = 0;

    sceKernelDelayThread(2000000);   /* the game first: its modules, its first frames */
    if (g_quit) return 0;
    sceIoRemove(LOG_PATH);
    cap_log("PSPExp Capture: stage 1");
    catch_exit();
    part = snap_alloc();
    cap_log("frame buffer: %d KB of kernel memory", part);
    if (part < 0) return 0;
    g_snap_sema = sceKernelCreateSema("PSPExpCaptureSnap", 0, 0, 1, NULL);
    {   /* the sender just below the watcher: it only waits on the USB (the frame is ready), but at
         * 0x60, below the game's threads, it got the CPU too rarely in a race (14 frames of 28 a
         * second, the link at 3.6 MB/s, 2026-10-23) */
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

    t0 = now_us();
    while (!g_quit) {
        sceDisplayWaitVblankStart();
        if (sceDisplayGetFrameBuf(&top, &bw, &fmt, 0) < 0 || !top) continue;
        if (top != last) {
            g_changes++; last = top;
            if (pspexpUsbConnected()) {
                if (!was_connected) { cap_log("receiver connected"); was_connected = 1; }
                if (!g_busy) {
                    u32 t = now_us();
                    g_frame_fmt = snap_frame(top, bw, fmt);
                    g_copy_us += now_us() - t;
                    g_frame_seq = seq++; g_frame_changes = g_changes; g_frame_time = t;
                    g_busy = 1;
                    sceKernelSignalSema(g_snap_sema, 1);
                } else g_skipped++;
            } else if (was_connected) { cap_log("receiver gone"); was_connected = 0; }
        }
        if (now_us() - t0 >= 5000000) {   /* every 5 s: the game's frame rate, and what went out */
            u32 ms = (now_us() - t0) / 1000;   /* (32-bit: no 64-bit division in the kernel) */
            u32 g10 = (g_changes - c0) * 10000 / ms, s10 = (g_sent - s0) * 10000 / ms;
            u32 copies = seq - q0, copy_us = copies ? (g_copy_us - u0) / copies : 0;   /* the game's cost, per frame copied */
            u32 dma_us = copies ? (g_dma_us - d0) / copies : 0;
            cap_log("game %u.%u fps, sent %u.%u fps, skipped %u, copy %u us (DMA %u), USB timeouts %u, format %d, width %d, %s",
                    g10 / 10, g10 % 10, s10 / 10, s10 % 10, g_skipped - k0, copy_us, dma_us, g_stats.tx_timeouts - o0, fmt, bw,
                    was_connected ? "connected" : "no receiver");
            t0 = now_us(); c0 = g_changes; s0 = g_sent; k0 = g_skipped; o0 = g_stats.tx_timeouts; q0 = seq; u0 = g_copy_us; d0 = g_dma_us;
        }
    }
    return 0;
}

int module_start(SceSize args, void *argp)
{
        /* the watcher above the game's threads (0x10-0x30 usually), to snapshot a frame the moment it is
     * shown; it does little: one look per refresh, and the DMA engine copies while it waits */
    g_cap_th = sceKernelCreateThread("PSPExpCapture", capture_thread, 0x18, 0x4000, PSP_THREAD_ATTR_VFPU, NULL);
    if (g_cap_th >= 0) sceKernelStartThread(g_cap_th, 0, NULL);
    return 0;
}

/* a thread's end awaited (half a second), else ended */
static void end_thread(SceUID *th)
{
    SceUInt t = 500000;
    if (*th < 0) return;
    if (sceKernelWaitThreadEnd(*th, &t) < 0) sceKernelTerminateThread(*th);
    sceKernelDeleteThread(*th);
    *th = -1;
}

/* the custom firmware's (systemctrl.S) */
u32 sctrlHENFindFunction(char *modname, char *libname, u32 nid);
void sctrlHENPatchSyscall(void *addr, void *newaddr);

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
    if (g_snap_sema >= 0) sceKernelSignalSema(g_snap_sema, 1);
    end_thread(&g_cap_th);
    end_thread(&g_send_th);
    if (g_power_slot >= 0) scePowerUnregisterCallback(g_power_slot);
    end_thread(&g_power_th);
    if (g_power_cb >= 0) sceKernelDeleteCallback(g_power_cb);
    if (g_usb_up) pspexpUsbStop();
    sceUsbbdUnregister(&g_driver);
    if (g_snap_sema >= 0) sceKernelDeleteSema(g_snap_sema);
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
