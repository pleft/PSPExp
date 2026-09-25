#include "pspexp_tcp_psp.h"
#include "../../protocol/pspexp_usb_link.h"
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <psputility.h>
#include <psputility_netparam.h>
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
static char g_status[160] = "tcp: off";
static char g_ip[32] = "";
static unsigned char g_syncbuf[PSPEXP_USB_SYNC_LEN];
static int g_synclen;
static const unsigned char SYNC[PSPEXP_USB_SYNC_LEN] = PSPEXP_USB_SYNC_BYTES;

/* access point connection progress, from the apctl handler */
static volatile int g_ap_state, g_ap_error, g_ap_event;
static int g_ap_tries;
static unsigned int g_ap_since;
/* Saved connection profiles, in the order they are tried: a network named
 * "PSPExp" (a PSPExp host's own access point) first, then the rest. */
static int g_profiles[10], g_nprofiles, g_profile_idx;
static char g_profile_ssid[40];

static void scan_profiles(void)
{
    int i, n = 0, first = -1;
    netData d;
    for (i = 1; i <= 10 && n < 10; i++) {
        if (sceUtilityCheckNetParam(i) != 0) continue;
        memset(&d, 0, sizeof(d));
        if (sceUtilityGetNetParam(i, PSP_NETPARAM_SSID, &d) == 0 && strstr(d.asString, "PSPExp")) first = n;
        g_profiles[n++] = i;
    }
    if (first > 0) { int t = g_profiles[0]; g_profiles[0] = g_profiles[first]; g_profiles[first] = t; }
    g_nprofiles = n;
    g_profile_idx = 0;
}

static int current_profile(void) { return g_nprofiles ? g_profiles[g_profile_idx % g_nprofiles] : 1; }

static void profile_ssid(void)
{
    netData d;
    memset(&d, 0, sizeof(d));
    g_profile_ssid[0] = 0;
    if (sceUtilityGetNetParam(current_profile(), PSP_NETPARAM_SSID, &d) == 0) {
        strncpy(g_profile_ssid, d.asString, sizeof(g_profile_ssid) - 1);
        g_profile_ssid[sizeof(g_profile_ssid) - 1] = 0;
    }
}

/* join the current profile; the next retry moves on to the next profile */
static int ap_join(void)
{
    profile_ssid();
    return sceNetApctlConnect(current_profile());
}

static const char *const AP_STATE_NAMES[] = { "disconnected", "scanning", "joining", "getting IP", "got IP", "EAP auth", "key exchange" };

static void (*g_trace)(const char *);
void pspexp_tcp_psp_set_trace(void (*fn)(const char *msg)) { g_trace = fn; }
static void set_status(const char *s) { strncpy(g_status, s, sizeof(g_status) - 1); g_status[sizeof(g_status) - 1] = 0; if (g_trace) g_trace(g_status); }
static void tracef(const char *fmt, int a, int b) { char buf[96]; if (!g_trace) return; snprintf(buf, sizeof(buf), fmt, a, b); g_trace(buf); }

static void ap_handler(int old_state, int new_state, int event, int error, void *arg)
{
    (void)old_state; (void)arg;
    g_ap_state = new_state; g_ap_event = event;
    if (error) g_ap_error = error;
}

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
    sceNetApctlAddHandler(ap_handler, NULL);
    g_ap_tries = 1;
    g_ap_since = sceKernelGetSystemTimeLow();
    scan_profiles();
    rc = ap_join();   /* "PSPExp" first if saved, else profile 1; instant under PPSSPP */
    if (rc < 0) { snprintf(g_status, sizeof(g_status), "tcp: apctl connect %08X (no Wi-Fi profile?)", rc); g_state = ST_FAILED; return -1; }
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
    if (g_listen < 0) { set_status("tcp: socket failed, retrying"); return; }   /* stays in ST_CONNECTING: poll retries */
    sceNetInetSetsockopt(g_listen, SOL_SOCKET, SO_NONBLOCK, &one, sizeof(one));
    /* after a Wi-Fi drop the old socket may still hold the port for a while */
    sceNetInetSetsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (sceNetInetBind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) < 0 || sceNetInetListen(g_listen, 1) < 0) {
        static unsigned int fails;
        char buf[96];
        snprintf(buf, sizeof(buf), "tcp: bind/listen failed (errno %d), retrying", sceNetInetGetErrno());
        set_status(buf);
        sceNetInetClose(g_listen); g_listen = -1;
        fails++;
        sceKernelDelayThread(500000);   /* poll() tries again (state stays ST_CONNECTING) */
        return;
    }
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info) == 0) strncpy(g_ip, info.ip, sizeof(g_ip) - 1);
    g_state = ST_LISTENING;
    snprintf(g_status, sizeof(g_status), "tcp: listening on %s:%d (%s)", g_ip[0] ? g_ip : "?", PORT, g_profile_ssid);
}

/* The access point can drop us at any time (weak signal, router): once a
 * second, outside the connecting phase, look at the apctl state and, when
 * the IP is gone, drop the sockets and join again. */
static void check_access_point(void)
{
    static unsigned int last;
    unsigned int now = sceKernelGetSystemTimeLow();
    int st = 0;
    if (g_state != ST_LISTENING && g_state != ST_SYNCING && g_state != ST_CONNECTED) return;
    if (now - last < 1000000u) return;
    last = now;
    if (sceNetApctlGetState(&st) < 0 || st == PSP_NET_APCTL_STATE_GOT_IP) return;
    tracef("tcp: access point lost (state %d, try %d), rejoining", st, g_ap_tries + 1);
    if (g_client >= 0) { sceNetInetClose(g_client); g_client = -1; }
    if (g_listen >= 0) { sceNetInetClose(g_listen); g_listen = -1; }
    g_synclen = 0;
    sceNetApctlDisconnect();
    g_ap_error = 0;
    g_ap_tries++;
    g_ap_since = now;
    scan_profiles();          /* start again from the preferred network */
    ap_join();
    g_state = ST_CONNECTING;
    set_status("tcp: wifi lost, rejoining");
}

void pspexp_tcp_psp_poll(void)
{
    check_access_point();
    switch (g_state) {
    case ST_CONNECTING: {
        int st = 0;
        unsigned int now = sceKernelGetSystemTimeLow();
        if (sceNetApctlGetState(&st) < 0) st = g_ap_state;
        if (st == PSP_NET_APCTL_STATE_GOT_IP) { start_listening(); break; }
        /* show where it is, and retry when it fell back to disconnected
         * (a failed join or key exchange) or got stuck for 30 s */
        {
            char buf[160];
            const char *name = st >= 0 && st <= 6 ? AP_STATE_NAMES[st] : "?";
            if (g_ap_error) snprintf(buf, sizeof(buf), "tcp: %s: %s, try %d, error %08X", g_profile_ssid, name, g_ap_tries, (unsigned)g_ap_error);
            else snprintf(buf, sizeof(buf), "tcp: %s: %s, try %d", g_profile_ssid, name, g_ap_tries);
            if (strcmp(buf, g_status)) set_status(buf);
        }
        if ((st == PSP_NET_APCTL_STATE_DISCONNECTED && now - g_ap_since > 3000000u) || now - g_ap_since > 30000000u) {
            sceNetApctlDisconnect();
            g_ap_tries++;
            g_ap_since = now;
            g_ap_error = 0;
            g_profile_idx++;          /* the next saved network, e.g. home Wi-Fi when the host's AP is off */
            ap_join();
        }
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
    check_access_point();
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
                if (++spins > 1500) { close_client(); set_status("tcp: send timeout (host gone?)"); return -2; }
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
