/*
 * PSPExp USB kernel driver. Registers a vendor-specific device with two bulk
 * endpoints and exposes a byte stream in each direction to user mode.
 * Structure follows PSPLink's usbhostfs (BSD), reduced to what PSPExp needs:
 * see protocol/pspexp_usb_link.h for the link rules.
 */
#include <pspkernel.h>
#include <pspsdk.h>
#include <pspusb.h>
#include <pspusbbus.h>
#include <pspctrl.h>
#include <string.h>
#include "../../protocol/pspexp_usb_link.h"
#include "pspexp_usb_driver.h"

#ifndef PSPEXP_USB_EMBEDDED   /* (built into another kernel module: psp/capture, PSPExp Capture) */
PSP_MODULE_INFO("PSPExpUSB", PSP_MODULE_KERNEL, 1, 0);
PSP_NO_CREATE_MAIN_THREAD();
#endif

enum {
    EV_ATTACH  = 1,
    EV_DETACH  = 2,
    EV_RX_DONE = 4,
    EV_RX_FREE = 8,
    EV_STOP    = 16
};
enum {
    EVC_CONNECTED = 1,   /* consumer flag: link synchronised */
    EVC_DATA      = 2    /* consumer flag: bytes waiting */
};
enum { EVT_DONE = 1 };   /* transfer flag: tx complete */
enum { EVS_DONE = 1 };   /* sync reply flag */

#define RX_BUFFERS   3
#define RX_FREE     (-1)
#define RX_PENDING  (-2)
#define TX_TIMEOUT_US 3000000

/* ---- descriptors ------------------------------------------------------- */

static struct DeviceDescriptor devdesc_hi = {
    18, 0x01, 0x200, 0, 0, 0, 64, 0, 0, 0x100, 0, 0, 0, 1
};
static struct ConfigDescriptor confdesc_hi = { 9, 2, 9 + 9 + 2 * 7, 1, 1, 0, 0xC0, 0 };
static struct InterfaceDescriptor interdesc_hi = { 9, 4, 0, 0, 2, 0xFF, 0x01, 0xFF, 1 };
static struct EndpointDescriptor endpdesc_hi[2] = {
    { 7, 5, PSPEXP_USB_EP_IN,  2, PSPEXP_USB_PACKET_HS, 0 },
    { 7, 5, PSPEXP_USB_EP_OUT, 2, PSPEXP_USB_PACKET_HS, 0 },
};
static struct DeviceDescriptor devdesc_full = {
    18, 0x01, 0x200, 0, 0, 0, 64, 0, 0, 0x100, 0, 0, 0, 1
};
static struct ConfigDescriptor confdesc_full = { 9, 2, 9 + 9 + 2 * 7, 1, 1, 0, 0xC0, 0 };
static struct InterfaceDescriptor interdesc_full = { 9, 4, 0, 0, 2, 0xFF, 0x01, 0xFF, 1 };
static struct EndpointDescriptor endpdesc_full[2] = {
    { 7, 5, PSPEXP_USB_EP_IN,  2, PSPEXP_USB_PACKET_FS, 0 },
    { 7, 5, PSPEXP_USB_EP_OUT, 2, PSPEXP_USB_PACKET_FS, 0 },
};
static unsigned char strp[] = { 0x8, 0x3, 'P', 0, 'X', 0, 0, 0 };
static struct UsbEndpoint endp[3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 } };
static struct UsbInterface intp = { 0xFFFFFFFF, 0, 1 };
static struct UsbData usbdata[2];

#define EP_IN_BLOCK  (&endp[1])   /* PSP -> host */
#define EP_OUT_BLOCK (&endp[2])   /* host -> PSP */

/* ---- state -------------------------------------------------------------- */

static SceUID g_thid = -1;
static SceUID g_ev = -1;      /* driver thread events */
static SceUID g_evc = -1;     /* consumer events */
static SceUID g_evt = -1;     /* tx completion */
static SceUID g_evs = -1;     /* sync reply completion */
static SceUID g_tx_sema = -1;

static struct UsbdDeviceReq g_rx_req;
static struct UsbdDeviceReq g_tx_req;
static struct UsbdDeviceReq g_sync_req;

/* each receive: a whole chunk; a host that only says hello (PSPExp Capture, built in) asks for
 * less, its buffers being kernel memory a game's capture needs */
#ifndef PSPEXP_USB_RX_SIZE
#define PSPEXP_USB_RX_SIZE PSPEXP_USB_CHUNK
#endif
static unsigned char g_rx_buf[RX_BUFFERS][PSPEXP_USB_RX_SIZE] __attribute__((aligned(64)));
static volatile int g_rx_len[RX_BUFFERS];    /* RX_FREE, RX_PENDING or byte count */
static volatile int g_rx_head;               /* next buffer the consumer reads */
static volatile int g_rx_tail;               /* next buffer to submit */
static volatile int g_rx_off;                /* consumer offset within head */
static volatile int g_rx_submitted = -1;
/* the copy of a write whose data is not 64-byte aligned (an embedded user can make it smaller:
 * kernel memory is scarce while a game runs) */
#ifndef PSPEXP_USB_TXBUF
#define PSPEXP_USB_TXBUF PSPEXP_USB_CHUNK
#endif
static unsigned char g_tx_buf[PSPEXP_USB_TXBUF] __attribute__((aligned(64)));
static unsigned char g_sync_buf[64] __attribute__((aligned(64)));
static const unsigned char g_sync_pattern[PSPEXP_USB_SYNC_LEN] = PSPEXP_USB_SYNC_BYTES;

static volatile int g_attached;
static volatile int g_connected;
static volatile int g_packet = PSPEXP_USB_PACKET_HS;
static volatile unsigned int g_generation;
static volatile int g_started;
static PspExpUsbStats g_stats;

/* ---- bus callbacks ------------------------------------------------------ */

static int usb_request(int arg1, int arg2, struct DeviceRequest *req) { return 0; }
static int func28(int arg1, int arg2, int arg3) { return 0; }

static int usb_attach(int speed, void *arg2, void *arg3)
{
    g_packet = (speed == 2) ? PSPEXP_USB_PACKET_HS : PSPEXP_USB_PACKET_FS;
    sceKernelSetEventFlag(g_ev, EV_ATTACH);
    return 0;
}

static int usb_detach(int arg1, int arg2, int arg3)
{
    sceKernelSetEventFlag(g_ev, EV_DETACH);
    return 0;
}

static int rx_done(struct UsbdDeviceReq *req, int arg2, int arg3)
{
    sceKernelSetEventFlag(g_ev, EV_RX_DONE);
    return 0;
}

static int tx_done(struct UsbdDeviceReq *req, int arg2, int arg3)
{
    sceKernelSetEventFlag(g_evt, EVT_DONE);
    return 0;
}

static int sync_done(struct UsbdDeviceReq *req, int arg2, int arg3)
{
    sceKernelSetEventFlag(g_evs, EVS_DONE);
    return 0;
}

/* ---- receive ring ------------------------------------------------------- */

static void rx_reset(void)
{
    int i, intc = pspSdkDisableInterrupts();
    for (i = 0; i < RX_BUFFERS; i++) g_rx_len[i] = RX_FREE;
    g_rx_head = g_rx_tail = g_rx_off = 0;
    g_rx_submitted = -1;
    g_stats.rx_queued = 0;
    pspSdkEnableInterrupts(intc);
    sceKernelClearEventFlag(g_evc, ~EVC_DATA);
}

static void rx_submit_next(void)
{
    int idx;
    if (!g_attached || g_rx_submitted >= 0) return;
    idx = g_rx_tail;
    if (g_rx_len[idx] != RX_FREE) return;     /* consumer is behind: bus NAKs until it catches up */
    g_rx_len[idx] = RX_PENDING;
    sceKernelDcacheInvalidateRange(g_rx_buf[idx], PSPEXP_USB_RX_SIZE);
    memset(&g_rx_req, 0, sizeof(g_rx_req));
    g_rx_req.endp = EP_OUT_BLOCK;
    g_rx_req.data = g_rx_buf[idx];
    g_rx_req.size = PSPEXP_USB_RX_SIZE;
    g_rx_req.func = rx_done;
    if (sceUsbbdReqRecv(&g_rx_req) < 0) {
        g_rx_len[idx] = RX_FREE;
        g_stats.rx_errors++;
        return;
    }
    g_rx_submitted = idx;
    g_rx_tail = (idx + 1) % RX_BUFFERS;
}

static void send_sync_reply(void)
{
    memcpy(g_sync_buf, g_sync_pattern, PSPEXP_USB_SYNC_LEN);
    sceKernelDcacheWritebackRange(g_sync_buf, sizeof(g_sync_buf));
    memset(&g_sync_req, 0, sizeof(g_sync_req));
    g_sync_req.endp = EP_IN_BLOCK;
    g_sync_req.data = g_sync_buf;
    g_sync_req.size = PSPEXP_USB_SYNC_LEN;
    g_sync_req.func = sync_done;
    sceKernelClearEventFlag(g_evs, ~EVS_DONE);
    if (sceUsbbdReqSend(&g_sync_req) == 0) {
        SceUInt timeout = 1000000;
        if (sceKernelWaitEventFlag(g_evs, EVS_DONE, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, NULL, &timeout) < 0)
            sceUsbbdReqCancelAll(EP_IN_BLOCK);
    }
}

static void handle_rx_done(void)
{
    int idx = g_rx_submitted;
    int len;
    if (idx < 0) return;
    g_rx_submitted = -1;
    if (g_rx_req.retcode != 0) {
        g_rx_len[idx] = RX_FREE;
        if (g_rx_req.retcode != -3) g_stats.rx_errors++;
        /* Keep the ring in submission order. */
        g_rx_tail = idx;
        rx_submit_next();
        return;
    }
    len = g_rx_req.recvsize;
    g_stats.rx_transfers++;
    if (len == PSPEXP_USB_SYNC_LEN && memcmp(g_rx_buf[idx], g_sync_pattern, PSPEXP_USB_SYNC_LEN) == 0) {
        /* Host (re)connected: drop everything, answer, announce. */
        sceUsbbdReqCancelAll(EP_IN_BLOCK);
        rx_reset();
        g_stats.resyncs++;
        send_sync_reply();
        /* the new link announced only once its reply is out: the runtime's HELLO, sent on the new
         * generation, went out on the same endpoint while the reply waited, and the two broke each
         * other (no reply on the board, which synced again, cancelling the HELLO: forever) */
        g_generation++;
        g_connected = 1;
        sceKernelSetEventFlag(g_evc, EVC_CONNECTED);
        rx_submit_next();
        return;
    }
    if (len <= 0) {
        g_rx_len[idx] = RX_FREE;
        g_rx_tail = idx;
    } else {
        g_stats.rx_bytes += (u32)len;
        g_rx_len[idx] = len;
        {
            int intc = pspSdkDisableInterrupts();
            g_stats.rx_queued += (u32)len;
            pspSdkEnableInterrupts(intc);
        }
        sceKernelSetEventFlag(g_evc, EVC_DATA);
    }
    rx_submit_next();
}

static int usb_thread(SceSize size, void *argp)
{
    for (;;) {
        u32 result = 0;
        if (sceKernelWaitEventFlag(g_ev, EV_ATTACH | EV_DETACH | EV_RX_DONE | EV_RX_FREE | EV_STOP,
                                   PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, &result, NULL) < 0)
            break;
        if (result & EV_STOP) break;
        if (result & EV_DETACH) {
            g_attached = 0;
            g_connected = 0;
            g_stats.detaches++;
            sceKernelClearEventFlag(g_evc, ~EVC_CONNECTED);
            sceUsbbdReqCancelAll(EP_IN_BLOCK);
            sceUsbbdReqCancelAll(EP_OUT_BLOCK);
            rx_reset();
            sceKernelSetEventFlag(g_evt, EVT_DONE);   /* wake a blocked writer */
        }
        if (result & EV_ATTACH) {
            g_attached = 1;
            g_stats.attaches++;
            g_stats.packet_size = (u32)g_packet;
            rx_reset();
            rx_submit_next();
        }
        if (result & EV_RX_DONE) handle_rx_done();
        if (result & EV_RX_FREE) rx_submit_next();
    }
    sceKernelExitDeleteThread(0);
    return 0;
}

/* ---- driver start/stop -------------------------------------------------- */

static struct UsbDriver g_driver;

static int start_func(int size, void *p)
{
    memset(usbdata, 0, sizeof(usbdata));
    memcpy(usbdata[0].devdesc, &devdesc_hi, sizeof(devdesc_hi));
    usbdata[0].config.pconfdesc = &usbdata[0].confdesc;
    usbdata[0].config.pinterfaces = &usbdata[0].interfaces;
    usbdata[0].config.pinterdesc = &usbdata[0].interdesc;
    usbdata[0].config.pendp = usbdata[0].endp;
    memcpy(usbdata[0].confdesc.desc, &confdesc_hi, sizeof(confdesc_hi));
    usbdata[0].confdesc.pinterfaces = &usbdata[0].interfaces;
    usbdata[0].interfaces.pinterdesc[0] = &usbdata[0].interdesc;
    usbdata[0].interfaces.intcount = 1;
    memcpy(usbdata[0].interdesc.desc, &interdesc_hi, sizeof(interdesc_hi));
    usbdata[0].interdesc.pendp = usbdata[0].endp;
    memcpy(usbdata[0].endp[0].desc, &endpdesc_hi[0], sizeof(endpdesc_hi[0]));
    memcpy(usbdata[0].endp[1].desc, &endpdesc_hi[1], sizeof(endpdesc_hi[1]));

    memcpy(usbdata[1].devdesc, &devdesc_full, sizeof(devdesc_full));
    usbdata[1].config.pconfdesc = &usbdata[1].confdesc;
    usbdata[1].config.pinterfaces = &usbdata[1].interfaces;
    usbdata[1].config.pinterdesc = &usbdata[1].interdesc;
    usbdata[1].config.pendp = usbdata[1].endp;
    memcpy(usbdata[1].confdesc.desc, &confdesc_full, sizeof(confdesc_full));
    usbdata[1].confdesc.pinterfaces = &usbdata[1].interfaces;
    usbdata[1].interfaces.pinterdesc[0] = &usbdata[1].interdesc;
    usbdata[1].interfaces.intcount = 1;
    memcpy(usbdata[1].interdesc.desc, &interdesc_full, sizeof(interdesc_full));
    usbdata[1].interdesc.pendp = usbdata[1].endp;
    memcpy(usbdata[1].endp[0].desc, &endpdesc_full[0], sizeof(endpdesc_full[0]));
    memcpy(usbdata[1].endp[1].desc, &endpdesc_full[1], sizeof(endpdesc_full[1]));

    g_driver.devp_hi = usbdata[0].devdesc;
    g_driver.confp_hi = &usbdata[0].config;
    g_driver.devp = usbdata[1].devdesc;
    g_driver.confp = &usbdata[1].config;

    g_ev = sceKernelCreateEventFlag("PSPExpUsbEv", 0x200, 0, NULL);
    g_evc = sceKernelCreateEventFlag("PSPExpUsbEvC", 0x200, 0, NULL);
    g_evt = sceKernelCreateEventFlag("PSPExpUsbEvT", 0x200, 0, NULL);
    g_evs = sceKernelCreateEventFlag("PSPExpUsbEvS", 0x200, 0, NULL);
    g_tx_sema = sceKernelCreateSema("PSPExpUsbTx", 0, 1, 1, NULL);
    if (g_ev < 0 || g_evc < 0 || g_evt < 0 || g_evs < 0 || g_tx_sema < 0) return -1;
    g_attached = 0;
    g_connected = 0;
    rx_reset();
    g_thid = sceKernelCreateThread("PSPExpUsb", usb_thread, 8, 0x4000, 0, NULL);
    if (g_thid < 0) return -1;
    if (sceKernelStartThread(g_thid, 0, NULL) < 0) return -1;
    g_started = 1;
    return 0;
}

static int stop_func(int size, void *p)
{
    g_started = 0;
    g_attached = 0;
    g_connected = 0;
    if (g_thid >= 0) { sceKernelSetEventFlag(g_ev, EV_STOP); sceKernelDelayThread(10000); sceKernelTerminateDeleteThread(g_thid); g_thid = -1; }
    if (g_ev >= 0) { sceKernelDeleteEventFlag(g_ev); g_ev = -1; }
    if (g_evc >= 0) { sceKernelDeleteEventFlag(g_evc); g_evc = -1; }
    if (g_evt >= 0) { sceKernelDeleteEventFlag(g_evt); g_evt = -1; }
    if (g_evs >= 0) { sceKernelDeleteEventFlag(g_evs); g_evs = -1; }
    if (g_tx_sema >= 0) { sceKernelDeleteSema(g_tx_sema); g_tx_sema = -1; }
    return 0;
}

static struct UsbDriver g_driver = {
    PSPEXP_USB_DRIVER_NAME,
    3,
    endp,
    &intp,
    NULL, NULL, NULL, NULL,
    (struct StringDescriptor *)strp,
    usb_request, func28, usb_attach, usb_detach,
    0,
    start_func,
    stop_func,
    NULL
};

/* ---- exported API ------------------------------------------------------- */

int pspexpUsbStart(void)
{
    int k1 = pspSdkSetK1(0);
    int ret;
    do {
        ret = sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, 0);
        if (ret < 0) break;
        ret = sceUsbStart(PSPEXP_USB_DRIVER_NAME, 0, 0);
        if (ret < 0) break;
        ret = sceUsbActivate(PSPEXP_USB_PID);
    } while (0);
    pspSdkSetK1(k1);
    return ret;
}

int pspexpUsbStop(void)
{
    int k1 = pspSdkSetK1(0);
    sceUsbDeactivate(PSPEXP_USB_PID);
    sceUsbStop(PSPEXP_USB_DRIVER_NAME, 0, 0);
    sceUsbStop(PSP_USBBUS_DRIVERNAME, 0, 0);
    pspSdkSetK1(k1);
    return 0;
}

int pspexpUsbAttached(void) { return g_attached; }
int pspexpUsbConnected(void) { return g_connected; }
unsigned int pspexpUsbGeneration(void) { return g_generation; }

int pspexpUsbWaitConnect(unsigned int timeout_us)
{
    int k1 = pspSdkSetK1(0);
    SceUInt t = timeout_us;
    int ret;
    if (g_evc < 0) { pspSdkSetK1(k1); return -1; }
    ret = sceKernelWaitEventFlag(g_evc, EVC_CONNECTED, PSP_EVENT_WAITOR, NULL, timeout_us ? &t : NULL);
    pspSdkSetK1(k1);
    return ret < 0 ? -1 : 0;
}

int pspexpUsbRead(void *buf, int len, unsigned int timeout_us)
{
    int k1 = pspSdkSetK1(0);
    int idx, avail, n, intc;
    SceUInt t = timeout_us;

    if (!g_connected || g_evc < 0 || len <= 0) { pspSdkSetK1(k1); return -1; }
    if (sceKernelWaitEventFlag(g_evc, EVC_DATA, PSP_EVENT_WAITOR, NULL, timeout_us ? &t : NULL) < 0) {
        pspSdkSetK1(k1);
        return g_connected ? 0 : -1;
    }
    idx = g_rx_head;
    avail = g_rx_len[idx];
    if (avail < 0) {                 /* woken but nothing full (reset raced) */
        sceKernelClearEventFlag(g_evc, ~EVC_DATA);
        pspSdkSetK1(k1);
        return 0;
    }
    n = avail - g_rx_off;
    if (n > len) n = len;
    memcpy(buf, g_rx_buf[idx] + g_rx_off, n);   /* head is ours until freed */
    intc = pspSdkDisableInterrupts();
    g_rx_off += n;
    g_stats.rx_queued -= (u32)n;
    if (g_rx_off >= avail) {
        g_rx_len[idx] = RX_FREE;
        g_rx_off = 0;
        g_rx_head = (idx + 1) % RX_BUFFERS;
        if (g_rx_len[g_rx_head] < 0) sceKernelClearEventFlag(g_evc, ~EVC_DATA);
        pspSdkEnableInterrupts(intc);
        sceKernelSetEventFlag(g_ev, EV_RX_FREE);
    } else {
        pspSdkEnableInterrupts(intc);
    }
    pspSdkSetK1(k1);
    return n;
}

static int tx_one(const unsigned char *data, int len)
{
    const unsigned char *src = data;
    SceUInt timeout = TX_TIMEOUT_US;
    int ret;
    if (((u32)data & 63) != 0) {
        memcpy(g_tx_buf, data, len);
        src = g_tx_buf;
    }
    sceKernelDcacheWritebackRange((void *)src, len);
    memset(&g_tx_req, 0, sizeof(g_tx_req));
    g_tx_req.endp = EP_IN_BLOCK;
    g_tx_req.data = (void *)src;
    g_tx_req.size = len;
    g_tx_req.func = tx_done;
    sceKernelClearEventFlag(g_evt, ~EVT_DONE);
    ret = sceUsbbdReqSend(&g_tx_req);
    if (ret < 0) { g_stats.tx_errors++; return -1; }
    ret = sceKernelWaitEventFlag(g_evt, EVT_DONE, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, NULL, &timeout);
    if (ret < 0) {
        sceUsbbdReqCancelAll(EP_IN_BLOCK);
        g_stats.tx_timeouts++;
        return -2;
    }
    if (g_tx_req.retcode != 0 || !g_connected) { g_stats.tx_errors++; return -1; }
    g_stats.tx_bytes += (u32)g_tx_req.recvsize;
    g_stats.tx_transfers++;
    return len;
}

/* The size of each transfer, and whether one that ends on a packet boundary is split so both
 * parts end short (what the runtime's host reads need). PSPExp Capture, built in, sets its own:
 * a stream the receiver reads without pause, where fewer, larger transfers may cost the PSP less. */
static int g_tx_chunk = PSPEXP_USB_CHUNK, g_tx_split = 1;
#ifdef PSPEXP_USB_EMBEDDED
static void pspexpUsbSetTx(int chunk, int split) { g_tx_chunk = chunk; g_tx_split = split; }
#endif

int pspexpUsbWrite(const void *data, int len)
{
    int k1 = pspSdkSetK1(0);
    const unsigned char *p = (const unsigned char *)data;
    int remaining = len, ret = len;

    if (!g_connected || g_tx_sema < 0) { pspSdkSetK1(k1); return -1; }
    if (sceKernelWaitSema(g_tx_sema, 1, NULL) < 0) { pspSdkSetK1(k1); return -1; }
    while (remaining > 0) {
        int n = remaining > g_tx_chunk ? g_tx_chunk : remaining;
        int r;
        if (((u32)p & 63) && n > (int)PSPEXP_USB_TXBUF) n = PSPEXP_USB_TXBUF;   /* (copied through g_tx_buf) */
        if (g_tx_split && n < g_tx_chunk && (n % g_packet) == 0) {
            /* Ends on a packet boundary: split so both transfers end short. */
            r = tx_one(p, n - PSPEXP_USB_SPLIT);
            if (r >= 0) r = tx_one(p + n - PSPEXP_USB_SPLIT, PSPEXP_USB_SPLIT);
        } else {
            r = tx_one(p, n);
        }
        if (r < 0) { ret = r; break; }
        p += n;
        remaining -= n;
    }
    sceKernelSignalSema(g_tx_sema, 1);
    pspSdkSetK1(k1);
    return ret;
}

int pspexpUsbGetStats(PspExpUsbStats *out)
{
    int k1 = pspSdkSetK1(0);
    memcpy(out, &g_stats, sizeof(*out));
    pspSdkSetK1(k1);
    return 0;
}

/* HOME is visible to kernel mode only; the runtime polls this so HOME held
 * leaves PSPExp even when the system's quit dialog does not come up. */
int pspexpUsbHomeHeld(void)
{
    int k1 = pspSdkSetK1(0);
    SceCtrlData pad;
    int held = 0;
    if (sceCtrlPeekBufferPositive(&pad, 1) > 0) held = (pad.Buttons & PSP_CTRL_HOME) != 0;
    pspSdkSetK1(k1);
    return held;
}

/* The buttons as kernel mode sees them: with the volume buttons (and HOME,
 * NOTE), which user mode's reads leave out. The volume buttons set the
 * sound on a board's Bluetooth device (libpspexp, pspexp-bt). */
unsigned int pspexpUsbButtons(void)
{
    int k1 = pspSdkSetK1(0);
    SceCtrlData pad;
    unsigned int b = 0;
    if (sceCtrlPeekBufferPositive(&pad, 1) > 0) b = pad.Buttons;
    pspSdkSetK1(k1);
    return b;
}

/* The system clock (UTC), in microseconds since 0001-01-01: setting it is
 * kernel-only. For CLOCK_SET: a PSP whose clock battery ran flat starts years
 * behind, and the board it plays for knows the time. */
int sceRtcSetCurrentTick(u64 *tick);
int pspexpUsbSetClock(unsigned int tick_lo, unsigned int tick_hi)
{
    int k1 = pspSdkSetK1(0), r;
    u64 tick = ((u64)tick_hi << 32) | tick_lo;
    r = sceRtcSetCurrentTick(&tick);
    pspSdkSetK1(k1);
    return r;
}

/* The time-zone settings, as Settings > Date & Time keeps them in the system
 * registry: /CONFIG/DATE time_zone_offset (minutes east, summer time not
 * included) and summer_time (0 / 1). write: set them from *tz and *dst;
 * else read them into *tz and *dst. 0 or < 0. */
#include <pspreg.h>
static int reg_date(int write, int *tz, int *dst)
{
    struct RegParam reg;
    REGHANDLE h, hd;
    int r;
    memset(&reg, 0, sizeof(reg));
    reg.regtype = 1;
    strcpy(reg.name, "/system");
    reg.namelen = strlen(reg.name);
    reg.unk2 = 1; reg.unk3 = 1;
    if ((r = sceRegOpenRegistry(&reg, 2, &h)) < 0) return r;
    if ((r = sceRegOpenCategory(h, "/CONFIG/DATE", 2, &hd)) < 0) { sceRegCloseRegistry(h); return r; }
    if (write) {
        r = sceRegSetKeyValue(hd, "time_zone_offset", tz, 4);
        if (r >= 0) r = sceRegSetKeyValue(hd, "summer_time", dst, 4);
        if (r >= 0) r = sceRegFlushCategory(hd);
    } else {
        r = sceRegGetKeyValueByName(hd, "time_zone_offset", tz, 4);
        if (r >= 0) r = sceRegGetKeyValueByName(hd, "summer_time", dst, 4);
    }
    sceRegCloseCategory(hd);
    if (write && r >= 0) sceRegFlushRegistry(h);
    sceRegCloseRegistry(h);
    return r < 0 ? r : 0;
}

int pspexpUsbSetZone(int tz_minutes, int dst)
{
    int k1 = pspSdkSetK1(0), r;
    r = reg_date(1, &tz_minutes, &dst);
    pspSdkSetK1(k1);
    return r;
}

/* the registry's values (what Settings shows), into two ints of the caller */
int pspexpUsbGetZone(int *tz_minutes, int *dst)
{
    int k1 = pspSdkSetK1(0), r, t = 0, d = 0;
    r = reg_date(0, &t, &d);
    pspSdkSetK1(k1);
    if (r >= 0) { *tz_minutes = t; *dst = d; }
    return r;
}

#ifndef PSPEXP_USB_EMBEDDED
int module_start(SceSize args, void *argp)
{
    memset(&g_stats, 0, sizeof(g_stats));
    return sceUsbbdRegister(&g_driver) < 0 ? 1 : 0;
}

int module_stop(SceSize args, void *argp)
{
    sceUsbbdUnregister(&g_driver);
    return 0;
}
#endif
