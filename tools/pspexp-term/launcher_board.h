/*
 * The board's own settings from the launcher (System > Time zone, Wi-Fi
 * network), with each board's tools:
 *
 *   GameShell, Raspberry Pi   timedatectl, NetworkManager (nmcli), through
 *                             the installer's sudo rule when not root
 *   Omega2+ (OpenWrt, root)   uci (system zone), Onion's ubus wifi-scan and
 *                             wifisetup
 *
 * This file builds the shell commands and parses what they print; the
 * launcher runs them in the background. A Wi-Fi password only ever travels in
 * the environment variable PSPEXP_WIFI_PSK of the command that uses it: it is
 * never logged or stored by PSPExp (the board's own network tool stores it).
 */
#ifndef PSPEXP_LAUNCHER_BOARD_H
#define PSPEXP_LAUNCHER_BOARD_H

#include <stddef.h>

typedef struct BoardZone {
    const char *iana;    /* Europe/Berlin */
    const char *posix;   /* CET-1CEST,M3.5.0,M10.5.0/3 (OpenWrt, musl's TZ) */
    const char *label;   /* Berlin, Vienna, Zurich */
    const char *offset;  /* UTC+1 */
} BoardZone;

int  board_zone_count(void);
const BoardZone *board_zone(int i);
int  board_zone_find(const char *iana);          /* index, -1 */
int  board_openwrt(void);
/* the zone set on the board (IANA name), "" when unknown */
void board_tz_current(char *out, size_t n);
/* the command that sets zone z */
void board_tz_command(const BoardZone *z, char *cmd, size_t n);

enum { BOARD_WIFI_NONE, BOARD_WIFI_NM, BOARD_WIFI_ONION };
int  board_wifi_kind(void);

typedef struct BoardNet {
    char ssid[64];
    char encr[16];       /* Omega: psk2, psk, wep, none; NetworkManager: its SECURITY text */
    int  signal;         /* 0-100 */
    int  secure;         /* needs a password */
} BoardNet;

/* the network the board is on, "" when none */
void board_wifi_current(int kind, char *ssid, size_t n);
/* the command whose output board_wifi_parse reads */
void board_wifi_scan_command(int kind, char *cmd, size_t n);
/* networks from a scan: named ones, strongest first, one per name */
int  board_wifi_parse(int kind, const char *text, BoardNet *out, int max);
/* the command that joins net (the password in $PSPEXP_WIFI_PSK) */
void board_wifi_connect_command(int kind, const BoardNet *net, char *cmd, size_t n);

#endif
