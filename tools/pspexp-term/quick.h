/*
 * Quick controls (2026-10-31, the launcher design proposal): what START opens over whatever is on
 * screen, the launcher or an app: Notifications, the terminal, the board's own battery when it has
 * one, Restart PSPExp and Power off the board (each asked again). Drawn by notify_draw, which every
 * view calls last; its input taken by main.c before the view's while it is open. Closing it leaves
 * the view as it was.
 */
#ifndef PSPEXP_TERM_QUICK_H
#define PSPEXP_TERM_QUICK_H

#include <stdint.h>

enum { QK_NONE, QK_NOTIFY, QK_TERMINAL, QK_RESTART, QK_POWEROFF };

void quick_open(void);
int  quick_is_open(void);
void quick_input(uint32_t pressed, int up, int down);
void quick_draw(void);
/* what was chosen, once: QK_NOTIFY... (QK_NONE: nothing) */
int  quick_take(void);

#endif
