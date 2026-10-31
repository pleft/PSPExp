/* Shared launcher pieces: see launcher_ui.h */
#include "launcher_ui.h"
#include "launcher_gfx.h"
#include "../../host/libpspexp/pspexp.h"

#include <stdio.h>
#include <string.h>

int ui_hint_cap(int x, const char *cap, const char *label, uint32_t col)
{
    int bw = gfx_text_width(FONT_BADGE, cap) + 10;
    gfx_nine(NINE_R6, x, UI_HINT_Y, bw, 14, 0x1e2c4a, 255);
    gfx_text(FONT_BADGE, x + 5, UI_HINT_Y + (14 - gfx_line_height(FONT_BADGE)) / 2, cap, UI_SUB, 255, 0);
    x += bw + 5;
    return x + gfx_text(FONT_SMALL, x, UI_HINT_Y - 1, label, col, 255, 0) + 14;
}

int ui_hint_btn(int x, char b, const char *label)
{
    gfx_button(b, x, UI_HINT_Y);
    x += 18;
    return x + gfx_text(FONT_SMALL, x, UI_HINT_Y - 1, label, UI_SUB, 255, 0) + 14;
}

int ui_app_bar(const char *icon, const char *title)
{
    gfx_rect(0, 0, UI_SCREEN_W, UI_BAR_H, 0x0a1120, 235);
    gfx_rect(0, UI_BAR_H, UI_SCREEN_W, 1, 0x283a5c, 255);
    gfx_icon(icon, 16, 12, 6, UI_ACCENT, 255);
    gfx_text(FONT_TITLE, 34, 4, title, UI_TEXT, 255, 0);
    return UI_SCREEN_W - 12;
}

int ui_hint_home(void) { return ui_hint_cap(12, "HOME", "Close", UI_SUB); }

void ui_hint_start(const char *label)
{
    int bw = gfx_text_width(FONT_BADGE, "START") + 10, x = UI_SCREEN_W - 12 - gfx_text_width(FONT_SMALL, label) - 5 - bw;
    ui_hint_cap(x, "START", label, UI_SUB);
}

/* ---- the menu --------------------------------------------------------------------- */

void ui_menu_begin(UiMenu *m, const char *title)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->title, sizeof(m->title), "%s", title ? title : "");
    m->open = 1;
}

void ui_menu_add(UiMenu *m, int id, const char *icon, const char *label, const char *value, int sep, int more)
{
    UiMenuItem *it;
    if (m->n >= UI_MENU_MAX) return;
    it = &m->it[m->n++];
    it->id = id; it->sep = sep && m->n > 1; it->more = more;
    snprintf(it->icon, sizeof(it->icon), "%s", icon ? icon : "");
    snprintf(it->label, sizeof(it->label), "%s", label ? label : "");
    snprintf(it->value, sizeof(it->value), "%s", value ? value : "");
}

int ui_menu_input(UiMenu *m, uint32_t pressed, int up, int down)
{
    if (!m->open || !m->n) { m->open = 0; return UIM_CLOSE; }
    if (up) m->sel = (m->sel + m->n - 1) % m->n;
    if (down) m->sel = (m->sel + 1) % m->n;
    if (pressed & (PSPEXP_BUTTON_CIRCLE | PSPEXP_BUTTON_START)) { m->open = 0; return UIM_CLOSE; }
    if (pressed & PSPEXP_BUTTON_CROSS) { m->open = 0; return m->it[m->sel].id; }
    return UIM_NONE;
}

void ui_menu_draw(const UiMenu *m)
{
    int i, seps = 0, w = 270, h, x, y, ry;
    if (!m->open) return;
    for (i = 0; i < m->n; i++) seps += m->it[i].sep;
    h = 30 + m->n * 20 + seps * 7;
    x = (UI_SCREEN_W - w) / 2; y = (UI_SCREEN_H - h) / 2 - 8;
    gfx_shadow(NINE_SHADOW14, x, y, w, h, 0x000000, 160);
    gfx_nine(NINE_R14, x, y, w, h, 0x1b2a46, 250);
    gfx_text(FONT_BODY, x + 14, y + 8, m->title, UI_TEXT, 255, w - 28);
    ry = y + 28;
    for (i = 0; i < m->n; i++) {
        const UiMenuItem *it = &m->it[i];
        int on = i == m->sel, right = x + w - 14;
        if (it->sep) { gfx_rect(x + 12, ry + 1, w - 24, 1, 0x34466b, 255); ry += 7; }
        if (on) gfx_nine(NINE_R6, x + 6, ry - 2, w - 12, 19, UI_ACCENT, 90);
        if (it->icon[0]) gfx_icon(it->icon, 13, x + 14, ry + 1, on ? 0xffffff : UI_ACCENT, 255);
        if (it->more) { gfx_icon("chevron-right", 13, right - 13, ry + 1, on ? 0xffffff : UI_SUB, 255); right -= 18; }
        if (it->value[0]) { int vw = gfx_text_width(FONT_SMALL, it->value); gfx_text(FONT_SMALL, right - vw, ry, it->value, on ? 0xffffff : UI_SUB, 255, 0); right -= vw + 8; }
        gfx_text(FONT_SMALL, x + 36, ry, it->label, on ? 0xffffff : UI_TEXT, 255, right - x - 36);
        ry += 20;
    }
    i = ui_hint_btn(12, 'x', "Choose");
    ui_hint_btn(i, 'o', "Close");
}

/* ---- the text keyboard ------------------------------------------------------------ */

#define TK_COLS 13
#define TK_ROWS 3
#define TK_NCTRL 5
#define KEY_W 32
#define KEY_H 25
#define KEY_GAP 2
static const char *TK[3][TK_ROWS] = {
    { "abcdefghijklm", "nopqrstuvwxyz", "0123456789.-_" },
    { "ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "0123456789.-_" },
    { "!@#$%^&*()+=?", "[]{}<>/\\|~`'\"", ",;:0123456789" },
};

void ui_kbd_reset(UiKbd *k, const char *initial, int masked, const char *done)
{
    memset(k, 0, sizeof(*k));
    if (initial) snprintf(k->buf, sizeof(k->buf), "%s", initial);
    k->masked = masked;
    k->done = done ? done : "OK";
}

int ui_kbd_input(UiKbd *k, uint32_t pressed, int up, int down, int left, int right)
{
    int cols = k->y < TK_ROWS ? (int)strlen(TK[k->page][k->y]) : TK_NCTRL;
    size_t l = strlen(k->buf);
    if (left) k->x = (k->x + cols - 1) % cols;
    if (right) k->x = (k->x + 1) % cols;
    if (up || down) {
        k->y = (k->y + (up ? TK_ROWS : 1)) % (TK_ROWS + 1);
        cols = k->y < TK_ROWS ? (int)strlen(TK[k->page][k->y]) : TK_NCTRL;
        if (k->x >= cols) k->x = cols - 1;
    }
    if (pressed & PSPEXP_BUTTON_CROSS) {
        if (k->y < TK_ROWS) {
            if (l + 1 < sizeof(k->buf)) { k->buf[l] = TK[k->page][k->y][k->x]; k->buf[l + 1] = 0; }
        } else if (k->x < 3) k->page = k->x;
        else if (k->x == 3) { if (l + 1 < sizeof(k->buf)) { k->buf[l] = ' '; k->buf[l + 1] = 0; } }
        else if (l) return UIK_DONE;
    }
    if ((pressed & PSPEXP_BUTTON_SQUARE) && l) k->buf[l - 1] = 0;
    if ((pressed & PSPEXP_BUTTON_TRIANGLE) && k->masked) k->show = !k->show;
    if ((pressed & PSPEXP_BUTTON_START) && l) return UIK_DONE;
    if (pressed & PSPEXP_BUTTON_CIRCLE) return UIK_CANCEL;
    return UIK_NONE;
}

void ui_kbd_draw(const UiKbd *k, const char *icon, const char *title, const char *placeholder, int caret, int a)
{
    static const char *ctrl[TK_NCTRL] = { "abc", "ABC", "#+=", "space", NULL };
    int i, y, x0, fx = 16, fw = UI_SCREEN_W - 32, kx, ky, x;
    char shown[256];
    size_t l = strlen(k->buf);
    gfx_icon(icon, 18, 16, 13, UI_ACCENT, a);
    gfx_text(FONT_TITLE, 42, 11, title, UI_TEXT, a, UI_SCREEN_W - 58);
    /* the field */
    for (i = 0; i < (int)l && i < (int)sizeof(shown) - 1; i++) shown[i] = k->masked && !k->show ? '*' : k->buf[i];
    shown[i] = 0;
    gfx_nine(NINE_R10, fx, 44, fw, 30, 0x172540, a);
    x = fx + 12;
    if (l) x += gfx_text(FONT_TITLE, x, 44 + (30 - gfx_line_height(FONT_TITLE)) / 2, shown, UI_TEXT, a, fw - 60);
    else gfx_text(FONT_BODY, x, 44 + (30 - gfx_line_height(FONT_BODY)) / 2, placeholder, UI_MUTED, a, 0);
    if (!caret) gfx_rect(x + 1, 51, 2, 16, UI_ACCENT, a);
    if (k->masked) gfx_icon(k->show ? "eye" : "eye-off", 14, fx + fw - 24, 52, UI_MUTED, a);
    /* the keys */
    x0 = (UI_SCREEN_W - (TK_COLS * KEY_W + (TK_COLS - 1) * KEY_GAP)) / 2;
    for (y = 0; y < TK_ROWS; y++) {
        const char *row = TK[k->page][y];
        int n = (int)strlen(row);
        for (i = 0; i < n; i++) {
            char c[2] = { row[i], 0 };
            int foc = y == k->y && i == k->x;
            kx = x0 + i * (KEY_W + KEY_GAP); ky = 92 + y * (KEY_H + KEY_GAP);
            if (foc) gfx_shadow(NINE_SHADOW10, kx, ky + 2, KEY_W, KEY_H, UI_ACCENT, a * 120 / 255);
            gfx_nine(NINE_R6, kx, ky, KEY_W, KEY_H, foc ? 0x3d8bff : 0x222a3b, foc ? a : a * 235 / 255);
            gfx_text(FONT_BODY, kx + (KEY_W - gfx_text_width(FONT_BODY, c)) / 2, ky + (KEY_H - gfx_line_height(FONT_BODY)) / 2, c, foc ? 0xffffff : UI_TEXT, a, 0);
        }
    }
    {   /* the control row: page keys, space, done */
        int total = TK_COLS * KEY_W + (TK_COLS - 1) * KEY_GAP, w = (total - (TK_NCTRL - 1) * KEY_GAP) / TK_NCTRL;
        ky = 92 + TK_ROWS * (KEY_H + KEY_GAP);
        for (i = 0; i < TK_NCTRL; i++) {
            int foc = k->y == TK_ROWS && i == k->x, on = i < 3 && i == k->page;
            const char *c = i == TK_NCTRL - 1 ? k->done : ctrl[i];
            kx = x0 + i * (w + KEY_GAP);
            if (foc) gfx_shadow(NINE_SHADOW10, kx, ky + 2, w, KEY_H, UI_ACCENT, a * 120 / 255);
            gfx_nine(NINE_R6, kx, ky, w, KEY_H, foc ? 0x3d8bff : on ? 0x3a4560 : i == 4 ? 0x1f5f4a : 0x222a3b, foc ? a : a * 235 / 255);
            gfx_text(FONT_SMALL, kx + (w - gfx_text_width(FONT_SMALL, c)) / 2, ky + (KEY_H - gfx_line_height(FONT_SMALL)) / 2, c, foc ? 0xffffff : UI_TEXT, a, 0);
        }
    }
    x = 16;
    x = ui_hint_btn(x, 'x', "Type");
    x = ui_hint_btn(x, 's', "Delete");
    if (k->masked) x = ui_hint_btn(x, 't', k->show ? "Hide" : "Show");
    x = ui_hint_btn(x, 'o', "Cancel");
    if (l) ui_hint_cap(UI_SCREEN_W - 16 - gfx_text_width(FONT_BADGE, "START") - 15 - gfx_text_width(FONT_SMALL, k->done), "START", k->done, UI_SUB);
    (void)x;
}
