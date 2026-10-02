/*
 * PSPExp runtime for real hardware: the shared runtime core over the USB
 * driver, rendering through the GE executor. Samples the controller at
 * 60 Hz, answers the transport benchmark, and shows a status page while no
 * host is connected (the GE owns the display while one is).
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psppower.h>
#include <stdio.h>
#include <string.h>
#include "../../runtime/pspexp_runtime.h"
#include "../executor-ge/pspexp_ge.h"
#include "pspexp_audio_psp.h"
#include "pspexp_fs_psp.h"
#include "../transport-usb/pspexp_usb_driver.h"
#include "../transport-tcp/pspexp_tcp_psp.h"
#include "../../protocol/pspexp_usb_link.h"

/* The link in use: USB when the cable is synchronised, else TCP. */
enum { LINK_NONE, LINK_USB, LINK_TCP };
static int g_link = LINK_NONE;
static int g_tcp_available;

/* Status trace appended to pspexp-rt.log next to the EBOOT (readable from
 * the host when running under PPSSPP; a few lines per session on hardware). */
static char g_trace_path[256];
static void trace(const char *msg)
{
    char line[200];
    int n = snprintf(line, sizeof(line), "%u [pspexp-rt] %s\n", (unsigned)(sceKernelGetSystemTimeLow() / 1000), msg);
    SceUID fd;
    if (n <= 0 || !g_trace_path[0]) return;
    fd = sceIoOpen(g_trace_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, line, (SceSize)n);
    sceIoClose(fd);
}
static void trace_init(const char *argv0)
{
    const char *slash = argv0 ? strrchr(argv0, '/') : NULL;
    if (!slash) { strcpy(g_trace_path, "ms0:/pspexp-rt.log"); return; }
    snprintf(g_trace_path, sizeof(g_trace_path), "%.*s/pspexp-rt.log", (int)(slash - argv0), argv0);
}

/* A working connection: the boot loader's attempt count for this slot goes
 * back to 0 (docs/Package_Manager.md §6.1: `active` next to the EBOOT, "slot=X
 * tries=N"; after 3 attempts without this, the loader boots the other slot). */
static void slot_confirm(void)
{
    static int done;
    char path[288], text[64];
    const char *slash = strrchr(g_trace_path, '/'), *sl;
    SceUID fd;
    int n;
    if (done || !slash) return;
    done = 1;
    snprintf(path, sizeof(path), "%.*s/active", (int)(slash - g_trace_path), g_trace_path);
    if ((fd = sceIoOpen(path, PSP_O_RDONLY, 0)) < 0) return;   /* before slots: nothing to confirm */
    n = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    text[n] = 0;
    if (strstr(text, "tries=0\n") || !(sl = strstr(text, "slot=")) || (sl[5] != 'a' && sl[5] != 'b')) return;
    snprintf(text, sizeof(text), "slot=%c\ntries=0\n", sl[5]);
    if ((fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777)) < 0) return;
    sceIoWrite(fd, text, strlen(text));
    sceIoClose(fd);
    trace("slot confirmed: attempts back to 0");
}

static int link_connected(void)
{
    if (g_link == LINK_USB) return pspexpUsbConnected();
    if (g_link == LINK_TCP) return pspexp_tcp_psp_connected();
    return 0;
}
static unsigned link_generation(void)
{
    /* USB and TCP generations are folded so a switch between links looks like a new one */
    return (pspexpUsbGeneration() << 16) ^ pspexp_tcp_psp_generation() ^ (g_link == LINK_TCP ? 0x8000u : 0u);
}
static int link_read(void *buf, int len, unsigned timeout_us)
{
    if (g_link == LINK_USB) return pspexpUsbRead(buf, len, timeout_us);
    if (g_link == LINK_TCP) return pspexp_tcp_psp_read(buf, len, timeout_us);
    return -1;
}
static int link_write(const void *data, int len)
{
    if (g_link == LINK_USB) return pspexpUsbWrite(data, len);
    if (g_link == LINK_TCP) return pspexp_tcp_psp_write(data, len);
    return -1;
}
/* Pick a link: USB first, then TCP; waits briefly. */
static void link_wait(void)
{
    static char last_status[96];
    if (pspexpUsbConnected()) { g_link = LINK_USB; return; }
    if (g_tcp_available) {
        pspexp_tcp_psp_poll();
        if (strcmp(last_status, pspexp_tcp_psp_status()) != 0) {
            strncpy(last_status, pspexp_tcp_psp_status(), sizeof(last_status) - 1);
            trace(last_status);
        }
        if (pspexp_tcp_psp_connected()) { g_link = LINK_TCP; return; }
    }
    g_link = LINK_NONE;
    /* Idle: give lower-priority threads (network) time, and do not spin when
     * the USB driver is absent (emulator). */
    if (pspexpUsbAttached()) pspexpUsbWaitConnect(50000);
    else sceKernelDelayThread(20000);
}

PSP_MODULE_INFO("PSPExpRuntime", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define printf pspDebugScreenPrintf

static int g_running = 1;
static char g_log[4][100];
static int g_log_n;
static PspExpGeExecutor g_ge;

static int exit_callback(int arg1, int arg2, void *common) { g_running = 0; return 0; }
static int callback_thread(SceSize args, void *argp)
{
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static uint32_t op_now(void *u) { return sceKernelGetSystemTimeLow(); }

#include "../../runtime/pspexp_bmp.h"
static void op_screenshot(void *u, uint16_t index)
{
    static uint16_t row16[PSPEXP_DISPLAY_WIDTH * PSPEXP_DISPLAY_HEIGHT] __attribute__((aligned(64)));   /* whole cache lines: the GE writes it */
    static uint8_t row[PSPEXP_DISPLAY_WIDTH * 3 + 4];
    uint8_t hdr[54];
    char path[256];
    const char *slash = strrchr(g_trace_path, '/');
    SceUID fd;
    int y;
    (void)u;
    if (!slash) return;
    snprintf(path, sizeof(path), "%.*s/pspexp-shot-%u.bmp", (int)(slash - g_trace_path), g_trace_path, (unsigned)index);
    pspexp_ge_read_display(&g_ge, row16);
    fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) { trace("screenshot: cannot open file"); return; }
    pspexp_bmp_header(hdr, PSPEXP_DISPLAY_WIDTH, PSPEXP_DISPLAY_HEIGHT);
    sceIoWrite(fd, hdr, sizeof(hdr));
    memset(row, 0, sizeof(row));
    for (y = PSPEXP_DISPLAY_HEIGHT - 1; y >= 0; y--) {   /* bottom-up */
        pspexp_bmp_row_5650(row, row16 + (size_t)y * PSPEXP_DISPLAY_WIDTH, PSPEXP_DISPLAY_WIDTH);
        sceIoWrite(fd, row, pspexp_bmp_stride(PSPEXP_DISPLAY_WIDTH));
    }
    sceIoClose(fd);
    trace(g_ge.in_frame ? "screenshot written (in a frame: read by the CPU)" : "screenshot written");
}
/* the battery and power supply (POWER_QUERY), from the power service; its error codes are large
 * negative numbers (0x8xxxxxxx), kept out as "unknown" */
static int op_power_get(void *u, PspExpPowerStatus *p)
{
    int v;
    (void)u;
    p->flags = 0;
    if (scePowerIsPowerOnline() > 0) p->flags |= PSPEXP_POWER_EXTERNAL;
    if (scePowerIsBatteryExist() > 0) {
        p->flags |= PSPEXP_POWER_BATTERY;
        if (scePowerIsBatteryCharging() > 0) p->flags |= PSPEXP_POWER_CHARGING;
        if (scePowerIsLowBattery() > 0) p->flags |= PSPEXP_POWER_LOW;
        if ((v = scePowerGetBatteryLifePercent()) >= 0 && v <= 100) p->percent = (int16_t)v;
        if ((v = scePowerGetBatteryLifeTime()) >= 0 && v < 32767) p->minutes_left = (int16_t)v;
        if ((v = scePowerGetBatteryElec()) > -32768 && v < 32767) p->current_ma = (int16_t)v;
        if ((v = scePowerGetBatteryTemp()) >= 0 && v < 100) p->temp_c = (int16_t)v;
        if ((v = scePowerGetBatteryVolt()) > 0 && v < 65535) p->voltage_mv = (uint16_t)v;
    }
    return 0;
}

/* the clock (CLOCK_SET): read here, set through the USB driver (kernel mode) */
#include <psprtc.h>
#include <psputility_sysparam.h>
#define TICKS_TO_1970 62135596800ull   /* seconds from 0001-01-01 to 1970-01-01 */
static int op_clock_get(void *u, uint64_t *utc, int *tz, int *dst)
{
    u64 tick = 0;
    (void)u;
    if (sceRtcGetCurrentTick(&tick) < 0) return -1;
    *utc = tick / 1000000ull - TICKS_TO_1970;
    if (pspexpUsbGetZone(tz, dst) < 0) {   /* the registry, as set; else what the system read at boot */
        if (sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_TIMEZONE, tz) < 0) *tz = 0;
        if (sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_DAYLIGHTSAVINGS, dst) < 0) *dst = 0;
    }
    return 0;
}
static int op_clock_set(void *u, uint64_t utc)
{
    u64 tick = (utc + TICKS_TO_1970) * 1000000ull;
    int r;
    char b[64];
    (void)u;
    r = pspexpUsbSetClock((unsigned)tick, (unsigned)(tick >> 32));
    snprintf(b, sizeof(b), "clock set from the host: 0x%08X", (unsigned)r);
    trace(b);
    return r < 0 ? -1 : 0;
}
static int op_zone_set(void *u, int tz, int dst)
{
    int r = pspexpUsbSetZone(tz, dst);
    char b[80];
    (void)u;
    snprintf(b, sizeof(b), "time zone set from the host: %d min, summer %d: 0x%08X", tz, dst, (unsigned)r);
    trace(b);
    return r < 0 ? -1 : 0;
}
static void op_log(void *u, const char *msg)
{
    strncpy(g_log[g_log_n % 4], msg, sizeof(g_log[0]) - 1);
    g_log[g_log_n % 4][sizeof(g_log[0]) - 1] = 0;
    g_log_n++;
    if (!strncmp(msg, "connected", 9) || !strncmp(msg, "maintenance", 11) || !strncmp(msg, "3D:", 3) ||
        !strncmp(msg, "us/bytes", 8)) trace(msg);   /* once per connection, or every 600 frames: kept in the log file too */
}

static void draw_status(PspExpRuntime *rt, unsigned gen, int connected)
{
    PspExpUsbStats us;
    int i;
    pspexpUsbGetStats(&us);
    pspDebugScreenSetXY(0, 0);
    printf("PSPExp runtime v0.2  %s  (hold HOME 2 s to exit)          \n",
           connected ? (g_link == LINK_TCP ? "CONNECTED (TCP)" : "CONNECTED (USB)")
                     : (pspexpUsbAttached() ? "attached, waiting for host sync" : "waiting for USB cable or TCP"));
    printf("%-78s\n", g_tcp_available ? pspexp_tcp_psp_status() : "tcp: unavailable");
    printf("usb: packet %u  attach %u detach %u sync %u  rx %u B / %u xfers  tx %u B / %u xfers  tmo %u err %u/%u   \n",
           (unsigned)us.packet_size, (unsigned)us.attaches, (unsigned)us.detaches, (unsigned)us.resyncs,
           (unsigned)us.rx_bytes, (unsigned)us.rx_transfers, (unsigned)us.tx_bytes, (unsigned)us.tx_transfers,
           (unsigned)us.tx_timeouts, (unsigned)us.tx_errors, (unsigned)us.rx_errors);
    printf("link gen %u  hello_ack %d  msgs in %u out %u  bytes in %llu out %llu           \n",
           gen, rt->connected, (unsigned)rt->stats.messages_in, (unsigned)rt->stats.messages_out,
           (unsigned long long)rt->stats.bytes_in, (unsigned long long)rt->stats.bytes_out);
    printf("frames %u  batches %u  sprites %u  rects %u  last frame %u B exec %u us          \n",
           (unsigned)rt->stats.frames, (unsigned)rt->stats.batches, (unsigned)rt->stats.sprites, (unsigned)rt->stats.rects,
           (unsigned)rt->stats.last_frame_bytes, (unsigned)rt->stats.last_execute_us);
    printf("ge: frames %u batches %u dropped %u texchg %u rects %u  last ge %u us  vram frag %u     \n",
           (unsigned)g_ge.stats.frames, (unsigned)g_ge.stats.batches, (unsigned)g_ge.stats.dropped_batches,
           (unsigned)g_ge.stats.texture_changes, (unsigned)g_ge.stats.rects, (unsigned)g_ge.stats.last_ge_us,
           (unsigned)g_ge.stats.vram_alloc_failures);
    printf("resources: uploads %u releases %u errors %u misses %u  vram %u/%u KiB ram %u/%u KiB   \n",
           (unsigned)rt->stats.uploads, (unsigned)rt->stats.releases, (unsigned)rt->stats.resource_errors, (unsigned)rt->stats.misses,
           (unsigned)(rt->cache.vram_used / 1024), (unsigned)(rt->cache.vram_budget / 1024),
           (unsigned)(rt->cache.ram_used / 1024), (unsigned)(rt->cache.ram_budget / 1024));
    printf("bench: active %d  %u msgs  %u B  uplink left %u  protocol errors %u              \n",
           rt->bench_active, (unsigned)rt->bench_msgs, (unsigned)rt->bench_bytes, (unsigned)rt->uplink_remaining,
           (unsigned)rt->stats.protocol_errors);
    printf("audio: %s  sounds %u plays %u  stream %u frames  buffered %u  underruns %u dropped %u     \n",
           rt->audio_active ? "on" : "off", (unsigned)rt->stats.sounds, (unsigned)rt->stats.plays,
           (unsigned)rt->stats.stream_frames, (unsigned)rt->mixer.buffered, (unsigned)rt->mixer.underruns,
           (unsigned)rt->mixer.dropped);
    printf("input seq %u                                              \n", (unsigned)rt->input_seq);
    printf("\nlog:\n");
    for (i = 0; i < 4; i++) {
        int idx = (g_log_n + i) % 4;
        printf("  %-64s\n", g_log[idx]);
    }
}

/* Back to the status page: the debug screen takes the display over from the GE. */
static void show_status_page(void)
{
    pspDebugScreenInit();
}

int main(int argc, char *argv[])
{
    PspExpRuntime rt;
    PspExpRuntimeOps ops;
    PspExpRuntimeConfig cfg;
    static unsigned char rxbuf[PSPEXP_USB_CHUNK] __attribute__((aligned(64)));
    unsigned gen = 0;
    int connected = 0;
    int rc;
    uint32_t last_input = 0, last_status = 0;
    SceCtrlData pad;

    pspDebugScreenInit();
    {
        SceUID thid = sceKernelCreateThread("cb", callback_thread, 0x11, 0xFA0, 0, NULL);
        if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
    }
    scePowerSetClockFrequency(333, 333, 166);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    printf("PSPExp runtime starting\n");
    rc = pspexpUsbStart();
    printf("pspexpUsbStart: 0x%08X\n", rc);
    trace_init(argc > 0 ? argv[0] : NULL);
    trace("runtime starting");
    pspexp_tcp_psp_set_trace(trace);
    { char b[64]; snprintf(b, sizeof(b), "pspexpUsbStart: 0x%08X", (unsigned)rc); trace(b); }
    g_tcp_available = pspexp_tcp_psp_init() == 0;
    printf("%s\n", pspexp_tcp_psp_status());
    trace(pspexp_tcp_psp_status());

    memset(&ops, 0, sizeof(ops));
    pspexp_ge_init(&g_ge);
    pspexp_ge_ops(&g_ge, &ops);
    pspexp_audio_psp_ops(&ops);
    pspexp_fs_psp_ops(&ops);   /* the memory stick as a drive of the board's file manager */
    ops.now_us = op_now;
    ops.screenshot = op_screenshot;
    ops.clock_get = op_clock_get;
    ops.clock_set = op_clock_set;
    ops.zone_set = op_zone_set;
    ops.power_get = op_power_get;
    ops.log = op_log;
    show_status_page();   /* GU init took the display; status page while idle */

    pspexp_runtime_default_config(&cfg, PSPEXP_EXECUTOR_PSP);
    cfg.ram_budget = 12u * 1024u * 1024u;   /* a game's working set; the runtime itself is small */
    cfg.usb_chunk = PSPEXP_USB_CHUNK;
    cfg.in_capacity = 512u * 1024u;
    cfg.out_capacity = 64u * 1024u;
    if (pspexp_runtime_init(&rt, &ops, &cfg) < 0) {
        printf("runtime init failed\n");
        sceKernelDelayThread(3000000);
        sceKernelExitGame();
    }
    pspexp_ge_bind(&g_ge, &rt);

    while (g_running) {
        uint32_t now = sceKernelGetSystemTimeLow();
        static uint32_t last_tick;

        /* Keep Auto Sleep and the backlight timer from firing: the PSP counts
         * only button presses as activity, and it often sits untouched while
         * the host drives it (or while it waits for one). */
        if (now - last_tick > 1000000) { scePowerTick(PSP_POWER_TICK_ALL); last_tick = now; }

        /* HOME held for 2 s leaves PSPExp, whatever state the link is in:
         * the system's quit dialog did not always come up (seen while a host
         * had died mid-session). A short press still opens that dialog. */
        {
            static uint32_t last_home_poll, home_since;
            if (now - last_home_poll > 50000) {
                last_home_poll = now;
                if (pspexpUsbHomeHeld() > 0) {
                    if (!home_since) home_since = now | 1;
                    else if (now - home_since > 2000000) { trace("HOME held: leaving"); break; }
                } else home_since = 0;
            }
        }

        if (!link_connected()) {
            if (connected) { connected = 0; pspexp_runtime_disconnect(&rt); show_status_page(); }
            if (now - last_status > 250000) { draw_status(&rt, gen, 0); last_status = now; }
            link_wait();
            continue;
        }
        if (!connected || gen != link_generation()) {
            gen = link_generation();
            connected = 1;
            pspexp_runtime_connect(&rt);      /* queues HELLO */
            trace(g_link == LINK_TCP ? "link up (tcp), HELLO queued" : "link up (usb), HELLO queued");
        }

        /* Output first so HELLO / replies go out promptly. */
        for (;;) {
            size_t len;
            const void *out = pspexp_runtime_out_peek(&rt, &len);
            if (len == 0) break;
            if (link_write(out, (int)len) < 0) { trace("link write failed"); connected = 0; pspexp_runtime_disconnect(&rt); show_status_page(); break; }
            pspexp_runtime_out_consume(&rt);
            rt.stats.messages_out++;
        }
        if (!connected) continue;

        rc = link_read(rxbuf, sizeof(rxbuf), 2000);
        if (rc > 0) {
            if (pspexp_runtime_feed(&rt, rxbuf, (size_t)rc) < 0) {
                trace("inbound stream corrupt");
                /* Stream corrupt: drop state and wait for the host to resync. */
                pspexp_runtime_disconnect(&rt);
                show_status_page();
                connected = 0;
                continue;
            }
        } else if (rc < 0) {
            trace("link read failed");
            connected = 0;
            pspexp_runtime_disconnect(&rt);
            show_status_page();
            continue;
        }

        if (rt.connected) slot_confirm();
        now = sceKernelGetSystemTimeLow();
        if (now - last_input >= 16666) {
            last_input = now;
            sceCtrlPeekBufferPositive(&pad, 1);
            if (rt.connected) pspexp_runtime_push_input(&rt, pad.Buttons, pad.Lx, pad.Ly);
        }
        /* Keep the status page until the first frame; after that the GE owns the screen. */
        if (rt.stats.frames == 0 && now - last_status > 250000) { draw_status(&rt, gen, 1); last_status = now; }
    }

    pspexp_runtime_free(&rt);
    pspexp_ge_term(&g_ge);
    pspexpUsbStop();
    sceKernelExitGame();
    return 0;
}
