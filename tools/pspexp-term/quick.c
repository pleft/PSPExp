/* Quick controls: see quick.h */
#include "quick.h"
#include "launcher.h"
#include "launcher_ui.h"
#include "notify.h"
#include <stdio.h>

enum { Q_NOTIFY = 1, Q_TERMINAL, Q_BATTERY, Q_RESTART, Q_POWER, Q_RESTART_YES, Q_POWER_YES, Q_CANCEL };

static UiMenu g_q;
static int g_taken;

void quick_open(void)
{
    char n[16] = "", label[80], v[32];
    int unread = notify_unread(), pct, chg;
    if (unread) snprintf(n, sizeof(n), "%d", unread > 99 ? 99 : unread);
    ui_menu_begin(&g_q, "Quick controls");
    ui_menu_add(&g_q, Q_NOTIFY, unread ? "bell-dot" : "bell", "Notifications", n[0] ? n : NULL, 0, 0);
    ui_menu_add(&g_q, Q_TERMINAL, "square-terminal", "Terminal", NULL, 0, 1);
    if (launcher_board_battery(label, sizeof(label), &pct, &chg)) {   /* the board's own (the GameShell's): the PSP's is in the status line */
        snprintf(v, sizeof(v), "%d%%%s", pct, chg ? ", charging" : "");
        ui_menu_add(&g_q, Q_BATTERY, chg ? "battery-charging" : "battery", label, v, 0, 0);
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
    case Q_BATTERY: quick_open(); break;   /* (it only says) */
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
