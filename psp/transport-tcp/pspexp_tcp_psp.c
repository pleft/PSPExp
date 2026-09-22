#include "pspexp_tcp_psp.h"
#include "../../protocol/pspexp_usb_link.h"
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <psputility.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#define PORT 3838

enum { ST_OFF, ST_CONNECTING, ST_LISTENING, ST_SYNCING, ST_CONNECTED, ST_FAILED };

static int g_state = ST_OFF;
static int g_listen = -1;
static int g_client = -1;
static unsigned int g_generation;
static char g_status[96] = "tcp: off";
static char g_ip[32] = "";
static unsigned char g_syncbuf[PSPEXP_USB_SYNC_LEN];
static int g_synclen;
static const unsigned char SYNC[PSPEXP_USB_SYNC_LEN] = PSPEXP_USB_SYNC_BYTES;

static void (*g_trace)(const char *);
void pspexp_tcp_psp_set_trace(void (*fn)(const char *msg)) { g_trace = fn; }
static void set_status(const char *s) { strncpy(g_status, s, sizeof(g_status) - 1); g_status[sizeof(g_status) - 1] = 0; if (g_trace) g_trace(g_status); }
static void tracef(const char *fmt, int a, int b) { char buf[96]; if (!g_trace) return; snprintf(buf, sizeof(buf), fmt, a, b); g_trace(buf); }

int pspexp_tcp_psp_init(void)
{
    int rc;
    if (g_state != ST_OFF) return 0;
    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if (rc < 0) { snprintf(g_status, sizeof(g_status), "tcp: no net common module (%08X)", rc); g_state = ST_FAILED; return -1; }
    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    if (rc < 0) { snprintf(g_status, sizeof(g_status), "tcp: no net inet module (%08X)", rc); g_state = ST_FAILED; return -1; }
    rc = sceNetInit(0x20000, 0x20, 0x1000, 0x20, 0x1000);
    if (rc < 0) { snprintf(g_status, sizeof(g_status), "tcp: sceNetInit %08X", rc); g_state = ST_FAILED; return -1; }
    rc = sceNetInetInit();
    if (rc < 0) { snprintf(g_status, sizeof(g_status), "tcp: sceNetInetInit %08X", rc); g_state = ST_FAILED; return -1; }
    rc = sceNetApctlInit(0x8000, 48);
    if (rc < 0) { snprintf(g_status, sizeof(g_status), "tcp: sceNetApctlInit %08X", rc); g_state = ST_FAILED; return -1; }
    rc = sceNetApctlConnect(1);   /* the first saved Wi-Fi profile; instant under PPSSPP */
    if (rc < 0) { snprintf(g_status, sizeof(g_status), "tcp: apctl connect %08X (no Wi-Fi profile 1?)", rc); g_state = ST_FAILED; return -1; }
    g_state = ST_CONNECTING;
    set_status("tcp: connecting to access point");
    return 0;
}

static void close_client(void)
{
    if (g_client >= 0) { sceNetInetClose(g_client); g_client = -1; }
    g_synclen = 0;
    if (g_state == ST_CONNECTED || g_state == ST_SYNCING) g_state = ST_LISTENING;
}

static void start_listening(void)
{
    struct sockaddr_in addr;
    int one = 1;
    union SceNetApctlInfo info;
    g_listen = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0) { set_status("tcp: socket failed"); g_state = ST_FAILED; return; }
    sceNetInetSetsockopt(g_listen, SOL_SOCKET, SO_NONBLOCK, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (sceNetInetBind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) < 0 || sceNetInetListen(g_listen, 1) < 0) {
        set_status("tcp: bind/listen failed");
        sceNetInetClose(g_listen); g_listen = -1;
        g_state = ST_FAILED;
        return;
    }
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info) == 0) strncpy(g_ip, info.ip, sizeof(g_ip) - 1);
    g_state = ST_LISTENING;
    snprintf(g_status, sizeof(g_status), "tcp: listening on %s:%d", g_ip[0] ? g_ip : "?", PORT);
}

void pspexp_tcp_psp_poll(void)
{
    switch (g_state) {
    case ST_CONNECTING: {
        int st = 0;
        if (sceNetApctlGetState(&st) == 0 && st == PSP_NET_APCTL_STATE_GOT_IP) start_listening();
        break;
    }
    case ST_LISTENING: {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int c = sceNetInetAccept(g_listen, (struct sockaddr *)&peer, &plen);
        if (c >= 0) {
            int one = 1;
            sceNetInetSetsockopt(c, SOL_SOCKET, SO_NONBLOCK, &one, sizeof(one));
            g_client = c;
            g_synclen = 0;
            g_state = ST_SYNCING;
            set_status("tcp: client connected, waiting for sync");
        }
        break;
    }
    case ST_SYNCING: {
        int n = (int)sceNetInetRecv(g_client, g_syncbuf + g_synclen, PSPEXP_USB_SYNC_LEN - g_synclen, 0);
        if (n > 0) {
            g_synclen += n;
            if (g_synclen == PSPEXP_USB_SYNC_LEN) {
                if (memcmp(g_syncbuf, SYNC, PSPEXP_USB_SYNC_LEN) == 0) {
                    sceNetInetSend(g_client, SYNC, PSPEXP_USB_SYNC_LEN, 0);
                    g_generation++;
                    g_state = ST_CONNECTED;
                    snprintf(g_status, sizeof(g_status), "tcp: CONNECTED (%s:%d)", g_ip, PORT);
                } else {
                    close_client();
                }
            }
        } else if (n == 0) {
            close_client();
        }
        break;
    }
    default:
        break;
    }
}

int pspexp_tcp_psp_connected(void) { return g_state == ST_CONNECTED; }
unsigned int pspexp_tcp_psp_generation(void) { return g_generation; }
const char *pspexp_tcp_psp_status(void) { return g_status; }

int pspexp_tcp_psp_read(void *buf, int len, unsigned int timeout_us)
{
    /* Non-blocking recv, polled: sceNetInetSelect is not reliable everywhere
     * (the emulator), and a 1 ms poll costs nothing at this rate. */
    unsigned int waited = 0;
    if (g_state != ST_CONNECTED) return -1;
    for (;;) {
        int n = (int)sceNetInetRecv(g_client, buf, len, 0);
        if (n > 0) return n;
        if (n == 0) { close_client(); set_status("tcp: client left"); return -1; }
        {
            int e = sceNetInetGetErrno();
            if (e != EAGAIN && e != EWOULDBLOCK) {
                close_client();
                tracef("tcp: recv error errno %d (n %d)", e, n);
                set_status("tcp: recv error");
                return -1;
            }
        }
        if (waited >= timeout_us) return 0;
        sceKernelDelayThread(1000);
        waited += 1000;
    }
}

int pspexp_tcp_psp_write(const void *data, int len)
{
    const unsigned char *p = (const unsigned char *)data;
    int left = len;
    unsigned int spins = 0;
    if (g_state != ST_CONNECTED) return -1;
    while (left > 0) {
        int n = (int)sceNetInetSend(g_client, p, left, 0);
        if (n > 0) { p += n; left -= n; spins = 0; continue; }
        if (n < 0) {
            int e = sceNetInetGetErrno();
            if (e == EAGAIN || e == EWOULDBLOCK) {
                if (++spins > 3000) { close_client(); set_status("tcp: send timeout"); return -2; }
                sceKernelDelayThread(1000);
                continue;
            }
            tracef("tcp: send error errno %d (n %d)", e, n);
        } else {
            tracef("tcp: send returned %d of %d", n, left);
        }
        close_client();
        set_status("tcp: send error");
        return -1;
    }
    return len;
}
