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
#include "../transport-usb/pspexp_usb_driver.h"
#include "../../protocol/pspexp_usb_link.h"

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
static void op_log(void *u, const char *msg)
{
    strncpy(g_log[g_log_n % 4], msg, sizeof(g_log[0]) - 1);
    g_log[g_log_n % 4][sizeof(g_log[0]) - 1] = 0;
    g_log_n++;
}

static void draw_status(PspExpRuntime *rt, unsigned gen, int connected)
{
    PspExpUsbStats us;
    int i;
    pspexpUsbGetStats(&us);
    pspDebugScreenSetXY(0, 0);
    printf("PSPExp runtime v0.1  %s  (HOME to exit)                   \n",
           connected ? "CONNECTED" : (pspexpUsbAttached() ? "attached, waiting for host sync" : "waiting for USB cable"));
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

    memset(&ops, 0, sizeof(ops));
    pspexp_ge_init(&g_ge);
    pspexp_ge_ops(&g_ge, &ops);
    ops.now_us = op_now;
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

        if (!pspexpUsbConnected()) {
            if (connected) { connected = 0; pspexp_runtime_disconnect(&rt); show_status_page(); }
            if (now - last_status > 250000) { draw_status(&rt, gen, 0); last_status = now; }
            pspexpUsbWaitConnect(100000);
            continue;
        }
        if (!connected || gen != pspexpUsbGeneration()) {
            gen = pspexpUsbGeneration();
            connected = 1;
            pspexp_runtime_connect(&rt);      /* queues HELLO */
        }

        /* Output first so HELLO / replies go out promptly. */
        for (;;) {
            size_t len;
            const void *out = pspexp_runtime_out_peek(&rt, &len);
            if (len == 0) break;
            if (pspexpUsbWrite(out, (int)len) < 0) { connected = 0; pspexp_runtime_disconnect(&rt); show_status_page(); break; }
            pspexp_runtime_out_consume(&rt);
            rt.stats.messages_out++;
        }
        if (!connected) continue;

        rc = pspexpUsbRead(rxbuf, sizeof(rxbuf), 2000);
        if (rc > 0) {
            if (pspexp_runtime_feed(&rt, rxbuf, (size_t)rc) < 0) {
                /* Stream corrupt: drop state and wait for the host to resync. */
                pspexp_runtime_disconnect(&rt);
                show_status_page();
                connected = 0;
                continue;
            }
        } else if (rc < 0) {
            connected = 0;
            pspexp_runtime_disconnect(&rt);
            show_status_page();
            continue;
        }

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
