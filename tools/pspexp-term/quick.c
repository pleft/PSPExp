/* Quick controls: see quick.h */
#include "quick.h"
#include "launcher.h"
#include "launcher_ui.h"
#include "notify.h"
#include <stdio.h>
#include <string.h>

enum { Q_NOTIFY = 1, Q_TERMINAL, Q_INFO, Q_NETWORK, Q_SOUND, Q_RESTART, Q_POWER, Q_RESTART_YES, Q_POWER_YES, Q_CANCEL };

static UiMenu g_q;
static int g_taken;
static const char *g_group;

static const char *battery_icon(int pct, int chg)
{
    return chg ? "battery-charging" : pct > 70 ? "battery-full" : pct > 35 ? "battery-medium" : pct > 12 ? "battery-low" : "battery-warning";
}

void quick_open(void)
{
    static const char *const BARS[4] = { "off", "weak", "good", "strong" };
    char n[16] = "", label[80], v[24];
    int unread = notify_unread();
    LauncherStatus st;
    launcher_status_info(&st);
    if (unread) snprintf(n, sizeof(n), "%d", unread > 99 ? 99 : unread);
    ui_menu_begin(&g_q, "Quick controls");
    ui_menu_add(&g_q, Q_NOTIFY, unread ? "bell-dot" : "bell", "Notifications", n[0] ? n : NULL, 0, 0);
    ui_menu_add(&g_q, Q_TERMINAL, "square-terminal", "Terminal", NULL, 0, 1);
    /* how things stand (the status line keeps the clock and the PSP's battery) */
    ui_menu_add(&g_q, Q_INFO, !strcmp(st.link, "USB") ? "usb" : !strcmp(st.link, "Wi-Fi") ? "wifi" : "unplug", "PSP link", st.link, 1, 0);
    if (st.wired) ui_menu_add(&g_q, st.net_settings ? Q_NETWORK : Q_INFO, "ethernet-port", "Network", "Ethernet", 0, st.net_settings);
    else if (st.wifi >= 0) {
        const char *ic = st.wifi >= 3 ? "wifi" : st.wifi == 2 ? "wifi-high" : st.wifi == 1 ? "wifi-low" : "wifi-off";
        if (st.wifi > 0 && st.ssid[0]) snprintf(label, sizeof(label), "Wi-Fi: %s", st.ssid);
        else snprintf(label, sizeof(label), "Wi-Fi");
        ui_menu_add(&g_q, st.net_settings ? Q_NETWORK : Q_INFO, ic, label, BARS[st.wifi > 3 ? 3 : st.wifi], 0, st.net_settings);
    }
    if (st.bt) {
        if (st.bt_vol >= 0) snprintf(v, sizeof(v), "%d%%", st.bt_vol); else v[0] = 0;
        ui_menu_add(&g_q, Q_SOUND, "headphones", "Headphones", v[0] ? v : NULL, 0, 1);
    } else ui_menu_add(&g_q, Q_SOUND, "volume-2", "Sound", "the PSP", 0, 1);
    if (st.bat >= 0) {   /* the board's own (the GameShell's): the PSP's is in the status line */
        snprintf(label, sizeof(label), "%s battery", st.board);
        snprintf(v, sizeof(v), "%d%%%s", st.bat, st.chg ? ", charging" : "");
        ui_menu_add(&g_q, Q_INFO, battery_icon(st.bat, st.chg), label, v, 0, 0);
    }
    ui_menu_add(&g_q, Q_RESTART, "refresh-cw", "Restart PSPExp", NULL, 1, 1);
    ui_menu_add(&g_q, Q_POWER, "power", "Power off the board", NULL, 0, 1);
}

int quick_is_open(void) { return g_q.open; }

void quick_input(uint32_t pressed, int up, int down)
{
    switch (ui_menu_input(&g_q, pressed, up, down)) {
    case Q_NOTIFY: g_taken = QK_NOTIFY; break;
    case Q_TERMINAL: g_taken = QK_TERMINAL; break;
    case Q_INFO: { int sel = g_q.sel; quick_open(); g_q.sel = sel; break; }   /* (it only says: open again, read anew) */
    case Q_NETWORK: g_taken = QK_SETTINGS; g_group = "Network"; break;
    case Q_SOUND: g_taken = QK_SETTINGS; g_group = "Sound"; break;
    case Q_RESTART:   /* asked again */
        ui_menu_begin(&g_q, "Restart PSPExp?");
        ui_menu_add(&g_q, Q_RESTART_YES, "refresh-cw", "Restart", NULL, 0, 0);
        ui_menu_add(&g_q, Q_CANCEL, "x", "Cancel", NULL, 0, 0);
        break;
    case Q_POWER:
        ui_menu_begin(&g_q, "Power off the board?");
        ui_menu_add(&g_q, Q_POWER_YES, "power", "Power off", NULL, 0, 0);
        ui_menu_add(&g_q, Q_CANCEL, "x", "Cancel", NULL, 0, 0);
        break;
    case Q_RESTART_YES: g_taken = QK_RESTART; break;
    case Q_POWER_YES: g_taken = QK_POWEROFF; break;
    default: break;
    }
}

void quick_draw(void) { ui_menu_draw(&g_q); }

int quick_take(void) { int t = g_taken; g_taken = QK_NONE; return t; }

const char *quick_settings_group(void) { return g_group ? g_group : "Settings"; }
