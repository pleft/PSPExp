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
#include <psploadexec.h>
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

/* What the status page says this runtime is: the version in its slot's VERSION file (the one the
 * board's About shows), without the time of day, and the slot; "v0.2" had been printed since v0.2 */
static char g_version[64] = "(installed without slots)";
static void version_init(void)
{
    char path[300], text[160], *v, *e;
    const char *slash = strrchr(g_trace_path, '/');
    SceUID fd;
    int n;
    char slot;
    if (!slash) return;
    snprintf(path, sizeof(path), "%.*s/active", (int)(slash - g_trace_path), g_trace_path);
    if ((fd = sceIoOpen(path, PSP_O_RDONLY, 0)) < 0) return;
    n = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    text[n] = 0;
    if (!(v = strstr(text, "slot=")) || (v[5] != 'a' && v[5] != 'b')) return;
    slot = v[5];
    snprintf(g_version, sizeof(g_version), "(slot %c)", slot);
    snprintf(path, sizeof(path), "%.*s/slot-%c/VERSION", (int)(slash - g_trace_path), g_trace_path, slot);
    if ((fd = sceIoOpen(path, PSP_O_RDONLY, 0)) < 0) return;
    n = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    text[n] = 0;
    if (strncmp(text, "version=", 8)) return;
    {   /* a release's name (release=v1.0.0, the line after version=): shown instead of the date */
        char *r = strstr(text, "\nrelease=v");
        if (r) {
            r += 10;
            for (e = r; *e && *e != '\n' && *e != '\r'; e++) ;
            *e = 0;
            snprintf(g_version, sizeof(g_version), "%.20s (slot %c)", r, slot);
            return;
        }
    }
    v = text + 8;
    for (e = v; *e && *e != '\n' && *e != '\r'; e++) ;
    *e = 0;
    {   /* 2026.10.10.135826+g79a56b6 -> 2026.10.10+g79a56b6 */
        char *plus = strchr(v, '+'), *t = plus;
        int dots = 0;
        for (e = v; plus && e < plus; e++) if (*e == '.' && ++dots == 3) { t = e; break; }
        if (plus && t != plus) memmove(t, plus, strlen(plus) + 1);
    }
    snprintf(g_version, sizeof(g_version), "%.40s (slot %c)", v, slot);
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
/* The link failed (a write or a read): the session dropped, the generation it failed on kept (the
 * main loop waits for a new one), and the first few such failures logged, then one in 100: logged
 * each time, a PSP stuck so wrote its log twice a second, its memory stick's light never off */
static unsigned g_lost_gen;
static unsigned g_unanswered;   /* syncs from the board since the last session (the main loop) */
static uint32_t g_lost_at;
static void link_lost(PspExpRuntime *rt, unsigned gen, const char *what)
{
    static unsigned n;
    char b[96];
    if (n < 5 || n % 100 == 0) { snprintf(b, sizeof(b), "%s (%u)", what, n + 1); trace(b); }
    n++;
    g_lost_gen = gen;
    g_lost_at = sceKernelGetSystemTimeLow() | 1;
    pspexp_runtime_disconnect(rt);
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

static volatile int g_quit_asked;                 /* the HOME menu's quit */
static volatile int g_restart_asked;              /* the board's RESTART: this EBOOT started again */
static char g_eboot[256];                         /* (argv[0], as the loader passed it) */
static volatile unsigned g_suspends, g_resumes;     /* the power service's notices, logged by the main loop */
static volatile int g_power_off_switch;             /* the switch held up: switching off, not sleeping */
static int exit_callback(int arg1, int arg2, void *common) { g_quit_asked = 1; g_running = 0; return 0; }
static void op_restart(void *u) { (void)u; g_restart_asked = 1; g_running = 0; }
static int power_callback(int unknown, int info, void *common)
{
    if (info & PSP_POWER_CB_POWER_SWITCH) g_power_off_switch = 1;
    if (info & PSP_POWER_CB_SUSPENDING) g_suspends++;
    if (info & PSP_POWER_CB_RESUME_COMPLETE) g_resumes++;
    return 0;
}
static int callback_thread(SceSize args, void *argp)
{
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    /* sleep, wake and the power switch, for the log: a session that ends silently is then told
     * apart (a quit, the PSP switched off, or neither: a crash) */
    cbid = sceKernelCreateCallback("Power Callback", power_callback, NULL);
    scePowerRegisterCallback(-1, cbid);
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
/* ---- the PSP's own keyboard (TEXT_REQUEST) ------------------------------------------
 * The system's on-screen keyboard, opened when the board asks for typed text (a game's name
 * field: OpenMW's character creation): drawn by the main loop between the board's frames, one
 * frame a pass, the link read and written as ever; the buttons go to it, not to the board;
 * when it closes, what was typed goes back (TEXT_RESULT). */
#include <psputility.h>
static int g_kb;   /* open */
static unsigned short g_kb_title[128], g_kb_in[256], g_kb_out[256];
static SceUtilityOskData g_kb_data;
static SceUtilityOskParams g_kb_params;

static void utf8_to_16(const char *s, unsigned short *o, int cap)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p && n < cap - 1) {
        unsigned c = *p++;
        if (c >= 0xE0 && p[0] && p[1]) { c = (c & 0x0Fu) << 12 | (p[0] & 0x3Fu) << 6 | (p[1] & 0x3Fu); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = (c & 0x1Fu) << 6 | (p[0] & 0x3Fu); p++; }
        else if (c >= 0x80) c = '?';
        o[n++] = (unsigned short)c;
    }
    o[n] = 0;
}

static void utf16_to_8(const unsigned short *s, char *o, int cap)
{
    int n = 0;
    for (; *s; s++) {
        unsigned c = *s;
        if (c < 0x80) { if (n + 1 >= cap) break; o[n++] = (char)c; }
        else if (c < 0x800) { if (n + 2 >= cap) break; o[n++] = (char)(0xC0 | c >> 6); o[n++] = (char)(0x80 | (c & 0x3F)); }
        else { if (n + 3 >= cap) break; o[n++] = (char)(0xE0 | c >> 12); o[n++] = (char)(0x80 | (c >> 6 & 0x3F)); o[n++] = (char)(0x80 | (c & 0x3F)); }
    }
    o[n] = 0;
}

static int op_text_begin(void *u, const char *title, const char *text, unsigned max_chars)
{
    (void)u;
    if (g_kb) return -1;
    if (max_chars < 1) max_chars = 1;
    if (max_chars > 255) max_chars = 255;
    utf8_to_16(title && *title ? title : "PSPExp", g_kb_title, 128);
    utf8_to_16(text ? text : "", g_kb_in, 256);
    memset(g_kb_out, 0, sizeof(g_kb_out));
    memset(&g_kb_data, 0, sizeof(g_kb_data));
    g_kb_data.language = PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
    g_kb_data.lines = 1;
    g_kb_data.unk_24 = 1;
    g_kb_data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_ALL;
    g_kb_data.desc = g_kb_title;
    g_kb_data.intext = g_kb_in;
    g_kb_data.outtextlength = (int)max_chars;
    g_kb_data.outtextlimit = (int)max_chars;
    g_kb_data.outtext = g_kb_out;
    memset(&g_kb_params, 0, sizeof(g_kb_params));
    g_kb_params.base.size = sizeof(g_kb_params);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &g_kb_params.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &g_kb_params.base.buttonSwap);   /* (cross or circle confirms, as the PSP is set) */
    g_kb_params.base.graphicsThread = 17;
    g_kb_params.base.accessThread = 19;
    g_kb_params.base.fontThread = 18;
    g_kb_params.base.soundThread = 16;
    g_kb_params.datacount = 1;
    g_kb_params.data = &g_kb_data;
    if (sceUtilityOskInitStart(&g_kb_params) < 0) { trace("keyboard: could not open"); return -1; }
    g_kb = 1;
    g_ge.keyboard = 1;
    trace("keyboard open");
    return 0;
}

/* a pass of the main loop while it is open: a frame of it; when it has closed, the answer */
static void keyboard_tick(PspExpRuntime *rt)
{
    switch (sceUtilityOskGetStatus()) {
    case PSP_UTILITY_DIALOG_VISIBLE: pspexp_ge_keyboard_frame(&g_ge, 1); return;
    case PSP_UTILITY_DIALOG_QUIT: sceUtilityOskShutdownStart(); pspexp_ge_keyboard_frame(&g_ge, 0); return;
    case PSP_UTILITY_DIALOG_NONE: break;   /* closed: the answer below */
    default: pspexp_ge_keyboard_frame(&g_ge, 0); return;   /* (starting, or finishing) */
    }
    {
        char out[768];
        g_kb = 0;
        g_ge.keyboard = 0;
        if (g_kb_data.result == PSP_UTILITY_OSK_RESULT_CANCELLED) { trace("keyboard cancelled"); pspexp_runtime_text_done(rt, PSPEXP_TEXT_CANCELLED, NULL); return; }
        utf16_to_8(g_kb_out, out, sizeof(out));
        trace("keyboard: text typed");
        pspexp_runtime_text_done(rt, PSPEXP_TEXT_OK, out);
    }
}

static void op_log(void *u, const char *msg)
{
    strncpy(g_log[g_log_n % 4], msg, sizeof(g_log[0]) - 1);
    g_log[g_log_n % 4][sizeof(g_log[0]) - 1] = 0;
    g_log_n++;
    if (!strncmp(msg, "connected", 9) || !strncmp(msg, "maintenance", 11) || !strncmp(msg, "3D:", 3) ||
        !strncmp(msg, "us/bytes", 8) || !strncmp(msg, "msg:", 4)) trace(msg);   /* once per connection, or every 600 frames, or the message trace: kept in the log file too */
}

static void draw_status(PspExpRuntime *rt, unsigned gen, int connected)
{
    PspExpUsbStats us;
    int i;
    pspexpUsbGetStats(&us);
    pspDebugScreenSetXY(0, 0);
    printf("PSPExp %s  %s  (hold HOME 2 s to exit)          \n", g_version,
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

/* A hand-over (DISCONNECT with PSPEXP_DISCONNECT_HANDOVER: the launcher starting a game): the
 * last frame stays, a spinner turns on it where the host said, until a program draws again
 * (at most 30 s); the status page meanwhile would have replaced the launcher's "Starting" */
static PspExpDisconnect g_ho;
static int g_ho_on;
static uint32_t g_ho_at, g_ho_spun, g_ho_frames;
static unsigned g_ho_phase;
static void handover_take(PspExpRuntime *rt, uint32_t now)
{
    if (!rt->handover_req) return;
    rt->handover_req = 0;
    g_ho = rt->handover; g_ho_on = 1; g_ho_at = now; g_ho_spun = 0; g_ho_phase = 0;
    g_ho_frames = rt->stats.frames;   /* (counted across connections: a new frame is a change) */
    trace("handing over: the last frame kept, a spinner on it");
}
/* while handing over: the spinner turned (1); over (a program drew, or 30 s: the status page) */
static int handover_tick(PspExpRuntime *rt, uint32_t now)
{
    if (!g_ho_on) return 0;
    if (rt->stats.frames != g_ho_frames) { g_ho_on = 0; return 0; }   /* the next program's first frame */
    if (now - g_ho_at > 30000000u) { g_ho_on = 0; trace("handing over: nothing drew in 30 s"); show_status_page(); return 0; }
    if (now - g_ho_spun >= 80000u) {
        g_ho_spun = now;
        pspexp_ge_spinner(&g_ge, g_ho.x, g_ho.y, g_ho.bg, g_ho.fg, g_ho_phase++);
    }
    return 1;
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
    if (argc > 0 && argv[0]) snprintf(g_eboot, sizeof(g_eboot), "%s", argv[0]);
    version_init();
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
    ops.text_begin = op_text_begin;
    if (g_eboot[0]) ops.restart = op_restart;
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
    {   /* trace-messages next to the EBOOT: the first 64 messages of each OpenGL program's
         * connection traced, each line written before the message is handled (for a PSP that
         * stops: its last line names the message; about 10 ms a line) */
        char path[288];
        const char *slash = strrchr(g_trace_path, '/');
        SceIoStat st;
        if (slash) {
            snprintf(path, sizeof(path), "%.*s/trace-messages", (int)(slash - g_trace_path), g_trace_path);
            if (sceIoGetstat(path, &st) >= 0) { rt.trace_messages = 64; trace("message trace on (trace-messages)"); }
        }
    }

    while (g_running) {
        uint32_t now = sceKernelGetSystemTimeLow();
        static uint32_t last_tick;
        {   /* the power service's notices (its callback runs on another thread: logged here) */
            static unsigned suspends, resumes;
            static int off_logged;
            if (g_power_off_switch && !off_logged) { off_logged = 1; trace("power switch: the PSP is switching off or going to sleep"); }
            if (g_suspends != suspends) { suspends = g_suspends; trace("going to sleep"); }
            if (g_resumes != resumes) { resumes = g_resumes; off_logged = 0; g_power_off_switch = 0; trace("woke up"); }
        }

        /* Keep Auto Sleep and the backlight timer from firing while something is happening: the PSP
         * counts only button presses as activity, and a game may run untouched (a cutscene, a demo).
         * Busy: at least 5 frames in the last 5 s window, or streamed sound (a launcher at rest draws
         * one frame a minute, for its clock); waiting for a host: the first 5 minutes after the link
         * went (a board booting finds the PSP awake). Otherwise the PSP's own Power Save settings
         * apply. (Flags, not time differences alone: the 32-bit microsecond clock wraps every 71 min) */
        {
            static uint32_t win_at, win_seq, win_stream, wait_at;
            static int busy = 1, waited = 0, was = -1;
            if (connected != was) { was = connected; busy = 1; win_at = now; win_seq = rt.frame_seq; win_stream = rt.stats.stream_frames; wait_at = now; waited = 0; }
            if (now - win_at >= 5000000u) {
                busy = rt.frame_seq - win_seq >= 5 || rt.stats.stream_frames != win_stream;
                win_at = now; win_seq = rt.frame_seq; win_stream = rt.stats.stream_frames;
            }
            if (!connected && !waited && now - wait_at > 300000000u) waited = 1;
            if ((connected ? busy : !waited) && now - last_tick > 1000000) { scePowerTick(PSP_POWER_TICK_ALL); last_tick = now; }
        }

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
            handover_take(&rt, now);
            if (connected) { connected = 0; pspexp_runtime_disconnect(&rt); if (!g_ho_on) show_status_page(); }
            if (!handover_tick(&rt, now) && now - last_status > 250000) { draw_status(&rt, gen, 0); last_status = now; }
            link_wait();
            continue;
        }
        if (g_lost_at && link_generation() == g_lost_gen && now - g_lost_at < 5000000u) {
            /* The link failed: wait for the board's next sync (a new generation) rather than greet it
             * again on the old one. Re-sent at once, the HELLO met the board's new sync on the endpoint
             * and broke its reply, the board synced again, and so on until the PSP was switched off
             * (black, writing "link write failed" twice a second; 2026-10-17, after a game). What the
             * old link left is read and dropped, so the sync has a buffer to land in. After 5 s with
             * no sync, greeted again on the same link (a board that never noticed) */
            while (link_read(rxbuf, sizeof(rxbuf), 1) > 0) ;   /* (1 us: 0 waits for ever) */
            if (now - last_status > 250000) { draw_status(&rt, gen, 0); last_status = now; }
            sceKernelDelayThread(10000);
            continue;
        }
        g_lost_at = 0;
        if (!connected || gen != link_generation()) {
            /* The board synced again and again and never heard the answers (2026-10-17, after two
             * console restarts 3 s apart: a thousand failed writes, a new sync every 1.5 s, the
             * board's "no sync reply"): the way out was a replug. After 6 syncs without a session,
             * USB is stopped and started again: the board sees the PSP leave and come back, both
             * ends' endpoints fresh */
            if (!rt.connected && g_link == LINK_USB && gen != link_generation() && ++g_unanswered >= 6) {
                trace("the board's syncs go unanswered: USB restarted");
                g_unanswered = 0;
                connected = 0;
                pspexp_runtime_disconnect(&rt);
                pspexpUsbStop();
                sceKernelDelayThread(500000);
                pspexpUsbStart();
                gen = link_generation();
                continue;
            }
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
            if (link_write(out, (int)len) < 0) { link_lost(&rt, gen, "link write failed"); connected = 0; if (!g_ho_on) show_status_page(); break; }
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
            link_lost(&rt, gen, "link read failed");
            connected = 0;
            handover_take(&rt, sceKernelGetSystemTimeLow());
            if (!g_ho_on) show_status_page();
            continue;
        }

        if (rt.connected) { slot_confirm(); g_unanswered = 0; }   /* (a session: the count of unanswered syncs starts again) */
        now = sceKernelGetSystemTimeLow();
        if (now - last_input >= 16666) {
            last_input = now;
            sceCtrlPeekBufferPositive(&pad, 1);
            if (rt.connected && !g_kb)   /* (the keyboard's buttons are its own) */   /* the volume buttons and the note button: kernel mode's to read (the board's
                                 * Bluetooth volume; the note pauses and resumes the board's music) */
                pspexp_runtime_push_input(&rt, pad.Buttons | (pspexpUsbButtons() & (PSPEXP_BUTTON_VOLUP | PSPEXP_BUTTON_VOLDOWN | PSPEXP_BUTTON_NOTE)), pad.Lx, pad.Ly);
        }
        if (g_kb) { keyboard_tick(&rt); continue; }   /* (the keyboard owns the screen) */
        /* Keep the status page until the first frame; after that the GE owns the screen. While
         * handing over, the spinner instead */
        handover_take(&rt, now);
        if (!handover_tick(&rt, now) && rt.stats.frames == 0 && now - last_status > 250000) { draw_status(&rt, gen, 1); last_status = now; }
    }

    trace(g_restart_asked ? "leaving: starting again (the board's RESTART)" : g_quit_asked ? "leaving: quit from the HOME menu" : "leaving");
    pspexp_runtime_free(&rt);
    pspexp_ge_term(&g_ge);
    pspexpUsbStop();
    if (g_restart_asked) {   /* this EBOOT again: the loader boots the active slot (a runtime just installed, after an update) */
        struct SceKernelLoadExecParam p;
        memset(&p, 0, sizeof(p));
        p.size = sizeof(p);
        p.args = strlen(g_eboot) + 1;
        p.argp = g_eboot;
        sceKernelLoadExec(g_eboot, &p);
        trace("starting again failed: leaving");   /* (it returns only when it failed) */
    }
    sceKernelExitGame();
    return 0;
}
