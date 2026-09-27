/*
 * Pieces the launcher and the file manager share, drawn with launcher_gfx:
 * the palette, the hint row at the bottom (a button and what it does), and
 * the on-screen text keyboard (Wi-Fi passwords, file names).
 */
#ifndef PSPEXP_LAUNCHER_UI_H
#define PSPEXP_LAUNCHER_UI_H

#include <stdint.h>

#define UI_SCREEN_W 480
#define UI_SCREEN_H 272
#define UI_HINT_Y   252

#define UI_TEXT   0xf1f4f9
#define UI_SUB    0xa9b2c3
#define UI_MUTED  0x7d869a
#define UI_TILE   0x1b2131
#define UI_ACCENT 0x8b9bff

/* a button badge ('x' cross, 'o' circle, 's' square, 't' triangle) and its
 * label at x on the hint row; returns where the next one goes */
int ui_hint_btn(int x, char b, const char *label);
/* a key cap ("START", "L") and its label */
int ui_hint_cap(int x, const char *cap, const char *label, uint32_t col);

/* ---- the text keyboard: three pages of 13 x 3 keys and a control row ---- */

typedef struct UiKbd {
    char buf[256];
    int page, x, y;
    int masked, show;        /* a password: typed as dots unless shown */
    const char *done;        /* the control row's last key: "Join", "Rename"... */
} UiKbd;

enum { UIK_NONE, UIK_DONE, UIK_CANCEL };

void ui_kbd_reset(UiKbd *k, const char *initial, int masked, const char *done);
/* cross types, square deletes, triangle shows / hides a password, start
 * or the done key finishes (not with an empty text), circle cancels */
int  ui_kbd_input(UiKbd *k, uint32_t pressed, int up, int down, int left, int right);
/* the whole screen: title (and icon) at the top, the field, the keys, the
 * hints; caret: the blink phase; a: alpha */
void ui_kbd_draw(const UiKbd *k, const char *icon, const char *title, const char *placeholder, int caret, int a);

#endif
