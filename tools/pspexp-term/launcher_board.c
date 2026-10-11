/* The board's own settings from the launcher: see launcher_board.h. */
#include "launcher_board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* Common zones: the IANA name for timedatectl and OpenWrt's zonename, the
 * POSIX rule for OpenWrt's timezone and musl (which reads only TZ). */
static const BoardZone ZONES[] = {
    { "UTC", "UTC0", "UTC", "UTC+0" },
    { "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0", "London, Dublin", "UTC+0" },
    { "Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0", "Lisbon", "UTC+0" },
    { "Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3", "Paris, Brussels, Amsterdam", "UTC+1" },
    { "Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3", "Berlin, Vienna, Zurich", "UTC+1" },
    { "Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3", "Madrid", "UTC+1" },
    { "Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3", "Rome", "UTC+1" },
    { "Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3", "Stockholm, Oslo, Copenhagen", "UTC+1" },
    { "Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3", "Warsaw, Prague, Budapest", "UTC+1" },
    { "Africa/Lagos", "WAT-1", "Lagos", "UTC+1" },
    { "Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4", "Athens", "UTC+2" },
    { "Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4", "Helsinki, Kyiv, Bucharest", "UTC+2" },
    { "Africa/Johannesburg", "SAST-2", "Johannesburg", "UTC+2" },
    { "Europe/Istanbul", "<+03>-3", "Istanbul", "UTC+3" },
    { "Europe/Moscow", "MSK-3", "Moscow", "UTC+3" },
    { "Asia/Dubai", "<+04>-4", "Dubai", "UTC+4" },
    { "Asia/Kolkata", "IST-5:30", "India", "UTC+5:30" },
    { "Asia/Bangkok", "<+07>-7", "Bangkok, Jakarta", "UTC+7" },
    { "Asia/Singapore", "<+08>-8", "Singapore", "UTC+8" },
    { "Asia/Shanghai", "CST-8", "China", "UTC+8" },
    { "Asia/Tokyo", "JST-9", "Tokyo", "UTC+9" },
    { "Asia/Seoul", "KST-9", "Seoul", "UTC+9" },
    { "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3", "Sydney, Melbourne", "UTC+10" },
    { "Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3", "Auckland", "UTC+12" },
    { "America/Sao_Paulo", "<-03>3", "Sao Paulo", "UTC-3" },
    { "America/New_York", "EST5EDT,M3.2.0,M11.1.0", "New York, Toronto", "UTC-5" },
    { "America/Chicago", "CST6CDT,M3.2.0,M11.1.0", "Chicago", "UTC-6" },
    { "America/Mexico_City", "CST6", "Mexico City", "UTC-6" },
    { "America/Denver", "MST7MDT,M3.2.0,M11.1.0", "Denver", "UTC-7" },
    { "America/Phoenix", "MST7", "Phoenix", "UTC-7" },
    { "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0", "Los Angeles, Vancouver", "UTC-8" },
    { "America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0", "Anchorage", "UTC-9" },
    { "Pacific/Honolulu", "HST10", "Honolulu", "UTC-10" },
};

int board_zone_count(void) { return (int)(sizeof(ZONES) / sizeof(ZONES[0])); }
const BoardZone *board_zone(int i) { return i >= 0 && i < board_zone_count() ? &ZONES[i] : NULL; }

int board_zone_find(const char *iana)
{
    int i;
    if (!iana || !iana[0]) return -1;
    for (i = 0; i < board_zone_count(); i++) if (!strcmp(ZONES[i].iana, iana)) return i;
    return -1;
}

int board_openwrt(void)
{
    struct stat st;
    return stat("/etc/openwrt_release", &st) == 0;
}

/* "  option zonename 'Europe/Berlin'" in /etc/config/system */
static void openwrt_zonename(char *out, size_t n)
{
    FILE *f = fopen("/etc/config/system", "r");
    char line[256];
    out[0] = 0;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "option zonename");
        if (p) {
            char *q = strchr(p + 15, '\''), *e;
            if (!q) q = strchr(p + 15, '"');
            if (q && (e = strchr(q + 1, *q))) { snprintf(out, n, "%.*s", (int)(e - q - 1), q + 1); break; }
        }
    }
    fclose(f);
}

void board_tz_current(char *out, size_t n)
{
    char link[256];
    ssize_t k;
    FILE *f;
    out[0] = 0;
    if (board_openwrt()) { openwrt_zonename(out, n); return; }
    k = readlink("/etc/localtime", link, sizeof(link) - 1);
    if (k > 0) {
        char *z;
        link[k] = 0;
        if ((z = strstr(link, "zoneinfo/"))) { snprintf(out, n, "%s", z + 9); return; }
    }
    if ((f = fopen("/etc/timezone", "r"))) {
        if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = 0;
        fclose(f);
    }
}

void board_tz_command(const BoardZone *z, char *cmd, size_t n)
{
    if (board_openwrt())
        snprintf(cmd, n, "uci set system.@system[0].zonename='%s' && uci set system.@system[0].timezone='%s' && "
                 "uci commit system && /etc/init.d/system reload", z->iana, z->posix);
    else
        snprintf(cmd, n, "%stimedatectl set-timezone '%s'", geteuid() == 0 ? "" : "sudo -n ", z->iana);
}

/* ---- Wi-Fi ------------------------------------------------------------------------ */

int board_wifi_kind(void)
{
    if (board_openwrt()) return access("/usr/bin/wifisetup", X_OK) == 0 ? BOARD_WIFI_ONION : BOARD_WIFI_NONE;
    if (access("/usr/bin/nmcli", X_OK) == 0) return BOARD_WIFI_NM;
    return BOARD_WIFI_NONE;
}

/* shell-quote s into out ('...' with ' as '\'') */
static void squote(const char *s, char *out, size_t n)
{
    size_t o = 0;
    if (n < 3) { if (n) out[0] = 0; return; }
    out[o++] = '\'';
    for (; *s && o + 5 < n; s++) {
        if (*s == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
        else out[o++] = *s;
    }
    out[o++] = '\'';
    out[o] = 0;
}

/* nmcli -t fields: ':' separates, '\' escapes */
static const char *nm_field(const char *p, char *out, size_t n)
{
    size_t o = 0;
    while (*p && *p != ':' && *p != '\n') {
        if (*p == '\\' && p[1]) p++;
        if (o + 1 < n) out[o++] = *p;
        p++;
    }
    out[o] = 0;
    return *p == ':' ? p + 1 : p;
}

void board_wifi_current(int kind, char *ssid, size_t n)
{
    FILE *p = NULL;
    char line[256];
    ssid[0] = 0;
    if (kind == BOARD_WIFI_NM) p = popen("nmcli -t -f ACTIVE,SSID dev wifi 2>/dev/null", "r");
    else if (kind == BOARD_WIFI_ONION) p = popen("uci -q get wireless.sta.ssid; uci -q get wireless.sta.disabled", "r");
    if (!p) return;
    if (kind == BOARD_WIFI_NM) {
        while (fgets(line, sizeof(line), p)) {
            char act[8];
            const char *rest = nm_field(line, act, sizeof(act));
            if (!strcmp(act, "yes")) { nm_field(rest, ssid, n); break; }
        }
    } else {
        char dis[8] = "";
        if (fgets(line, sizeof(line), p)) { line[strcspn(line, "\r\n")] = 0; snprintf(ssid, n, "%s", line); }
        if (fgets(dis, sizeof(dis), p) && dis[0] == '1') ssid[0] = 0;
    }
    pclose(p);
}

void board_wifi_scan_command(int kind, char *cmd, size_t n)
{
    if (kind == BOARD_WIFI_NM)
        snprintf(cmd, n, "{ %snmcli -t -f SSID,SIGNAL,SECURITY dev wifi list --rescan yes 2>/dev/null || nmcli -t -f SSID,SIGNAL,SECURITY dev wifi list; }; "
                 "for c in $(nmcli -t -f UUID,TYPE connection show | sed -n 's/:802-11-wireless$//p'); do "
                 "printf 'S\\t%%s\\t%%s\\n' \"$c\" \"$(nmcli -t -g 802-11-wireless.ssid connection show \"$c\")\"; done",
                 geteuid() == 0 ? "" : "sudo -n ");
    else if (kind == BOARD_WIFI_ONION)
        snprintf(cmd, n, "ubus call onion wifi-scan '{\"device\":\"ra0\"}'; "
                 "uci -q show wireless | sed -n \"s/^wireless\\.@wifi-config\\[[0-9]*\\]\\.ssid='\\(.*\\)'$/\\1/p\" | "
                 "while read -r s; do printf 'S\\t-\\t%%s\\n' \"$s\"; done");
    else snprintf(cmd, n, "true");
}

static int net_cmp(const void *a, const void *b) { return ((const BoardNet *)b)->signal - ((const BoardNet *)a)->signal; }

static void net_add(BoardNet *out, int *cnt, int max, const BoardNet *nw)
{
    int i;
    if (!nw->ssid[0]) return;   /* hidden networks: not offered */
    for (i = 0; i < *cnt; i++)
        if (!strcmp(out[i].ssid, nw->ssid)) { if (nw->signal > out[i].signal) out[i] = *nw; return; }
    if (*cnt < max) out[(*cnt)++] = *nw;
}

/* the value of "key": "value" inside one JSON object (between s and e) */
static int json_str(const char *s, const char *e, const char *key, char *out, size_t n)
{
    char pat[48];
    const char *p, *q;
    size_t o = 0;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    for (p = s; p && p < e; p++) {
        p = strstr(p, pat);
        if (!p || p >= e) return -1;
        q = p + strlen(pat);
        while (q < e && (*q == ' ' || *q == ':' || *q == '\t')) q++;
        if (q >= e || *q != '"') continue;
        for (q++; q < e && *q != '"'; q++) {
            if (*q == '\\' && q + 1 < e) q++;
            if (o + 1 < n) out[o++] = *q;
        }
        out[o] = 0;
        return 0;
    }
    return -1;
}

/* where the saved networks start: the first line "S<tab>" */
static const char *saved_start(const char *text)
{
    const char *p = text;
    while (p && *p) {
        if (p[0] == 'S' && p[1] == '\t') return p;
        p = strchr(p, '\n');
        if (p) p++;
    }
    return text + strlen(text);
}

int board_wifi_parse(int kind, const char *text, BoardNet *out, int max)
{
    int cnt = 0, i;
    const char *saved = saved_start(text), *sp;
    if (kind == BOARD_WIFI_NM) {
        const char *line = text;
        while (line && *line && line < saved) {
            BoardNet nw;
            char sig[8];
            const char *p;
            memset(&nw, 0, sizeof(nw));
            p = nm_field(line, nw.ssid, sizeof(nw.ssid));
            p = nm_field(p, sig, sizeof(sig));
            nm_field(p, nw.encr, sizeof(nw.encr));
            nw.signal = atoi(sig);
            nw.secure = nw.encr[0] && strcmp(nw.encr, "--");
            net_add(out, &cnt, max, &nw);
            line = strchr(line, '\n');
            if (line) line++;
        }
    } else if (kind == BOARD_WIFI_ONION) {
        const char *p = strstr(text, "\"results\"");
        while (p && (p = strchr(p, '{')) && p < saved) {
            const char *e = strchr(p, '}');
            BoardNet nw;
            char sig[8] = "0";
            if (!e) break;
            memset(&nw, 0, sizeof(nw));
            json_str(p, e, "ssid", nw.ssid, sizeof(nw.ssid));
            json_str(p, e, "encryption", nw.encr, sizeof(nw.encr));
            json_str(p, e, "signalStrength", sig, sizeof(sig));
            nw.signal = atoi(sig);
            nw.secure = nw.encr[0] && strcmp(nw.encr, "none");
            net_add(out, &cnt, max, &nw);
            p = e + 1;
        }
    }
    /* the saved ones: "S<tab>id<tab>ssid" */
    for (sp = saved; *sp; ) {
        const char *t1 = strchr(sp, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL, *nl = strchr(sp, '\n');
        if (!nl) nl = sp + strlen(sp);
        if (sp[0] == 'S' && t1 == sp + 1 && t2 && t2 < nl) {
            char id[40], ssid[64];
            size_t il = (size_t)(t2 - t1 - 1), sl = (size_t)(nl - t2 - 1);
            if (il >= sizeof(id)) il = sizeof(id) - 1;
            if (sl >= sizeof(ssid)) sl = sizeof(ssid) - 1;
            memcpy(id, t1 + 1, il); id[il] = 0;
            memcpy(ssid, t2 + 1, sl); ssid[sl] = 0;
            while (sl && ssid[sl - 1] == '\r') ssid[--sl] = 0;
            for (i = 0; i < cnt; i++)
                if (!out[i].saved && !strcmp(out[i].ssid, ssid)) {
                    out[i].saved = 1;
                    if (strcmp(id, "-")) snprintf(out[i].id, sizeof(out[i].id), "%s", id);
                    break;
                }
        }
        sp = *nl ? nl + 1 : nl;
    }
    if (cnt > 1) qsort(out, (size_t)cnt, sizeof(BoardNet), net_cmp);
    return cnt;
}

void board_wifi_connect_command(int kind, const BoardNet *net, int with_password, char *cmd, size_t n)
{
    char q[200];
    squote(net->ssid, q, sizeof(q));
    if (!net->secure) with_password = 0;
    if (kind == BOARD_WIFI_NM) {
        const char *su = geteuid() == 0 ? "" : "sudo -n ";
        if (net->saved && net->id[0] && !with_password) {
            char qi[100];
            squote(net->id, qi, sizeof(qi));
            snprintf(cmd, n, "%snmcli connection up uuid %s", su, qi);
        } else {
            /* the password reaches nmcli on its standard input (--ask), from the shell's own printf
             * (a builtin: no process), never among a program's arguments, which every user of the
             * board could read in the process list while it joined; its prompt (one dot a character,
             * so the password's length) goes nowhere, its errors to the log */
            char feed[96] = "";
            if (with_password) snprintf(feed, sizeof(feed), "printf '%%s\\n' \"$PSPEXP_WIFI_PSK\" | ");
            if (net->saved)   /* a new password for a known network: keep its profile whatever happens */
                snprintf(cmd, n, "%s%snmcli%s dev wifi connect %s%s", feed, su, with_password ? " --ask" : "", q, with_password ? " >/dev/null" : "");
            else   /* a failed join (a wrong password) leaves a profile NetworkManager
                    * cannot update, so every retry would fail too: drop it on failure */
                snprintf(cmd, n, "%s%snmcli%s dev wifi connect %s%s || { %snmcli connection delete id %s >/dev/null 2>&1; false; }",
                         feed, su, with_password ? " --ask" : "", q, with_password ? " >/dev/null" : "", su, q);
        }
    } else if (kind == BOARD_WIFI_ONION && net->saved && !with_password) {
        /* known to wifisetup: first in line, and wifimanager joins it */
        snprintf(cmd, n, "wifisetup priority -ssid %s -move top", q);
    } else if (kind == BOARD_WIFI_ONION) {
        /* (Onion's wifisetup takes the password as an argument; on the Omega everything runs as
         * root and nobody else logs in) */
        const char *encr = net->secure ? (net->encr[0] ? net->encr : "psk2") : "none";
        /* add, or edit when the network is already known; then first in line */
        snprintf(cmd, n, "{ wifisetup add -ssid %s -encr %s -password \"$PSPEXP_WIFI_PSK\" || "
                 "wifisetup edit -ssid %s -encr %s -password \"$PSPEXP_WIFI_PSK\"; } && wifisetup priority -ssid %s -move top",
                 q, encr, q, encr, q);
    } else snprintf(cmd, n, "false");
}
