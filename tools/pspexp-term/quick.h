/*
 * Quick controls (2026-10-31, the launcher design proposal): what START opens over whatever is on
 * screen, the launcher or an app: Notifications, the terminal; how things stand, what the status
 * line no longer shows (how the PSP is connected, the board's network and its Wi-Fi's strength, the
 * sound and the Bluetooth volume, the board's own battery when it has one; the network and the sound
 * open their Settings); Restart PSPExp and Power off the board (each asked again). Drawn by notify_draw, which every
 * view calls last; its input taken by main.c before the view's while it is open. Closing it leaves
 * the view as it was.
 */
#ifndef PSPEXP_TERM_QUICK_H
#define PSPEXP_TERM_QUICK_H

#include <stdint.h>

enum { QK_NONE, QK_NOTIFY, QK_TERMINAL, QK_RESTART, QK_POWEROFF, QK_SETTINGS, QK_RECORD, QK_CLIP };

void quick_open(void);
/* the recorder as the link knows it (main.c, each turn): the receiver's PSPEXP_CAPTURE_* while the
 * PSP can be asked (0: it cannot: Start recording and Save clip not offered), a recording under way */
void quick_set_capture(int mode, int recording);
int  quick_is_open(void);
void quick_input(uint32_t pressed, int up, int down);
void quick_draw(void);
/* what was chosen, once: QK_NOTIFY... (QK_NONE: nothing) */
int  quick_take(void);
/* QK_SETTINGS: the Settings group to show ("Network", "Sound") */
const char *quick_settings_group(void);

#endif
