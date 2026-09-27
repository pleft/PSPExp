/*
 * pspexp-term: the PSP as a host's console. Runs a shell in a pseudo-
 * terminal, emulates a VT100 / xterm-subset terminal (80 x 34 with the 6x8
 * font, 16 / 256 / true colours, scrollback, alternate screen for vi and
 * top) and draws it on the PSP. The PSP's controls type through an on-screen
 * keyboard. The terminal bell beeps on the PSP, and raw audio written to a
 * FIFO plays there (the host's "ears").
 *
 *   pspexp-term [--usb | --tcp host[:port] | --null | --sdl] [--shell PATH]
 *               [--cmd "command"] [--audio-fifo PATH | --no-audio-fifo]
 *               [--pidfile PATH] [--shot N]
 *
 * Controls (PSP):
 *   select        show / hide the on-screen keyboard (terminal 80x25 / 80x34)
 *   d-pad         move on the keyboard; arrow keys when it is hidden
 *   cross         type the highlighted key          circle  backspace
 *   square        space                              triangle  Enter
 *   start         Tab (completion)
 *   L (held)      Shift                              R (held)  Ctrl
 *   nub           arrow keys (history, cursor, vi) with the keyboard shown
 *   L tap         the context menu: keys and commands for the shell or for
 *                 the full-screen program (vi, less, top), terminal actions
 *                 (~/.config/pspexp-term/menu, see term_menu.h)
 *   L + triangle  Esc                                R + circle  Ctrl-C
 *   R + up/down   scroll back a line (R + left/right: a page)
 *   R + nub       scroll back like a mouse wheel (further: faster); with the
 *                 keyboard hidden the nub alone does
 *   L + select    scroll mode: the d-pad and nub scroll, nothing held
 *
 * Linux hosts: a real keyboard and mouse (the Raspberry Pi 400's own, or USB
 * ones on the Omega's dock) are read from /dev/input and type into the same
 * terminal (US layout); while the PSP is connected they are grabbed, so the
 * host's own console does not see the keys too. The first key hides the
 * on-screen keyboard. The mouse wheel scrolls back; programs that ask for the
 * mouse (xterm mouse modes 1000/1002/1006) get clicks. --no-host-input
 * turns this off, --no-grab shares the devices.
 *
 * Mirror view (Linux, --mirror [/dev/fb0]): the host's own framebuffer is
 * streamed to the PSP, scaled to fit (the ClockworkPi GameShell's 320x240
 * launcher and games), and the PSP's buttons become a virtual keypad through
 * /dev/uinput (default: the GameShell's keys: d-pad/nub = arrows, cross /
 * circle / square / triangle = A B X Y = J K U I, start = Enter, select =
 * Space, L / R = H / L). Hold start ~0.7 s = Menu (Esc); hold select ~0.7 s
 * switches between the mirror and the terminal (quick taps of select and
 * start still type Space / Enter, sent on release). No shoulder-button
 * combos: on a PSP-1000 over Wi-Fi, holding L+R covers the antenna.
 *
 * Hand-over: another PSPExp program (a game) can take the PSP while the
 * terminal keeps running: SIGUSR1 releases the PSP, SIGUSR2 takes it back
 * (scripts/board/pspexp-exec does both around a command).
 */
#define _GNU_SOURCE
#include "../../host/libpspexp/pspexp.h"
#include "../../host/libpspexp/pspexp_font.h"
#include "launcher.h"
#include "term_menu.h"
#include "term_fonts.h"
#include "term_themes.h"
#include "../../host/executor-null/pspexp_null.h"
#include "../../host/transport-tcp/pspexp_tcp.h"
#ifdef PSPEXP_HAVE_USB
#include "../../host/transport-usb/pspexp_usb.h"
#endif
#ifdef PSPEXP_HAVE_SDL
#include "../../host/executor-sdl/pspexp_sdl.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

/* ---- geometry ----------------------------------------------------------- */

#define COLS_MAX    80          /* the terminal's font decides: 480 / its width */
#define ROWS_MAX    34          /* 272 / 8, the smallest font */
#define UI_W        6           /* the keyboard's and the menu's own font (built in) */
#define UI_H        8
#define KB_Y        200         /* the on-screen keyboard's top */
#define KB_H        (272 - KB_Y)
#define SCROLLBACK  1000

static int g_cw = 6, g_ch = 8;  /* the terminal font's cell (term_fonts.h) */
static int g_font, g_theme;     /* TERM_FONTS[g_font], TERM_THEMES[g_theme] */
static int g_scroll_mode;       /* the D-pad and nub scroll, nothing held (L + select) */

#define FONT_ID     1
#define FONT_PAL_ID 2

/* ---- terminal state ------------------------------------------------------ */

enum { A_BOLD = 1, A_UNDER = 2, A_REVERSE = 4, A_FGRGB = 8, A_BGRGB = 16 };

/* ch: 32..126 ASCII, 0x80..0x9f line drawing and blocks (drawn by
 * box_glyph), 0xa0..0xff Latin-1. fg / bg: a palette index (0..255), DEF_FG /
 * DEF_BG (the theme's own), or with A_FGRGB / A_BGRGB a 5650 colour: the
 * theme is applied when drawing, so a new one recolours what is on screen */
typedef struct Cell { uint8_t ch, attr; uint16_t fg, bg; } Cell;
typedef struct Line { Cell c[COLS_MAX]; } Line;

#define DEF_FG 256
#define DEF_BG 257

typedef struct Pen { int fg, bg; uint16_t fg_rgb, bg_rgb; uint8_t attr; } Pen;   /* fg/bg: palette index, -2 = direct rgb */

typedef struct Term {
    int rows, cols;                 /* the font and the keyboard decide */
    Line screen[ROWS_MAX], alt[ROWS_MAX];
    int dec_graphics, charset_g0;   /* ESC ( 0: DEC line drawing for G0 */
    int on_alt;
    int cx, cy, wrap_pending;
    int saved_cx, saved_cy; Pen saved_pen;
    int top, bottom;                /* scroll region, inclusive */
    Pen pen;
    int cursor_visible, autowrap, app_cursor;
    int mouse_mode, mouse_sgr;      /* xterm mouse reporting: 1000 / 1002 / 1003, SGR (1006) */
    Line sb[SCROLLBACK]; int sb_count, sb_head;   /* ring of lines scrolled off the top */
    int view;                       /* lines scrolled back, 0 = live */
    /* parser */
    int state; int params[16]; int nparams; int private_mode; int param_started;
    uint32_t utf8; int utf8_left;
    int dirty;
} Term;

static Term T;
static int g_master = -1;
static pid_t g_child = -1;
static volatile sig_atomic_t g_release_req, g_acquire_req, g_child_died;

static uint16_t rgb565(uint32_t rgb) { return pspexp_color_5650((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb); }

static const TermTheme *theme(void) { return &TERM_THEMES[g_theme]; }

/* a palette index, DEF_FG / DEF_BG, through the current theme */
static uint16_t color_of(int idx, uint16_t direct)
{
    if (idx == -2) return direct;
    if (idx == DEF_FG) return rgb565(theme()->fg);
    if (idx == DEF_BG) return rgb565(theme()->bg);
    if (idx < 16) return rgb565(theme()->ansi[idx]);
    if (idx < 232) {                /* 6x6x6 cube */
        int i = idx - 16, r = i / 36, g = (i / 6) % 6, b = i % 6;
        static const uint8_t lv[6] = { 0, 95, 135, 175, 215, 255 };
        return pspexp_color_5650(lv[r], lv[g], lv[b]);
    }
    { uint8_t v = (uint8_t)(8 + (idx - 232) * 10); return pspexp_color_5650(v, v, v); }
}

static uint16_t cell_fg(const Cell *c) { return (c->attr & A_FGRGB) ? c->fg : color_of(c->fg, 0); }
static uint16_t cell_bg(const Cell *c) { return (c->attr & A_BGRGB) ? c->bg : color_of(c->bg, 0); }

static Cell blank_cell(void)
{
    Cell c;
    c.ch = ' '; c.attr = 0;
    c.fg = DEF_FG;
    if (T.pen.bg == -2) { c.bg = T.pen.bg_rgb; c.attr |= A_BGRGB; }   /* erase uses the current background */
    else c.bg = (uint16_t)T.pen.bg;
    return c;
}

static void clear_line(Line *l, int from, int to)
{
    int x;
    Cell b = blank_cell();
    for (x = from; x < to && x < COLS_MAX; x++) l->c[x] = b;
}

static Line *line(int y) { return T.on_alt ? &T.alt[y] : &T.screen[y]; }

static void push_scrollback(const Line *l)
{
    if (T.on_alt) return;
    T.sb[T.sb_head] = *l;
    T.sb_head = (T.sb_head + 1) % SCROLLBACK;
    if (T.sb_count < SCROLLBACK) T.sb_count++;
}

static const Line *sb_line(int back)   /* 1 = newest line in the scrollback */
{
    int i = (T.sb_head - back + SCROLLBACK * 2) % SCROLLBACK;
    return &T.sb[i];
}

static void scroll_up(int top, int bottom, int n)
{
    int y;
    while (n-- > 0) {
        if (top == 0 && bottom == T.rows - 1) push_scrollback(line(0));
        for (y = top; y < bottom; y++) *line(y) = *line(y + 1);
        clear_line(line(bottom), 0, T.cols);
    }
    T.dirty = 1;
}

static void scroll_down(int top, int bottom, int n)
{
    int y;
    while (n-- > 0) {
        for (y = bottom; y > top; y--) *line(y) = *line(y - 1);
        clear_line(line(top), 0, T.cols);
    }
    T.dirty = 1;
}

static void term_reset(int rows)
{
    int y;
    memset(&T.pen, 0, sizeof(T.pen));
    T.pen.fg = DEF_FG; T.pen.bg = DEF_BG;
    T.rows = rows;
    T.cx = T.cy = 0; T.wrap_pending = 0;
    T.top = 0; T.bottom = rows - 1;
    T.cursor_visible = 1; T.autowrap = 1; T.app_cursor = 0; T.on_alt = 0;
    for (y = 0; y < ROWS_MAX; y++) { clear_line(&T.screen[y], 0, COLS_MAX); clear_line(&T.alt[y], 0, COLS_MAX); }
    T.dirty = 1;
}

/* Keyboard shown / hidden: 25 / 34 rows. Content stays bottom-aligned (the
 * prompt stays put): shrinking pushes top lines into the scrollback, growing
 * pulls them back. The shell and full-screen programs get SIGWINCH. */
static void term_resize(int rows)
{
    int d, y;
    if (rows == T.rows) goto tell;
    if (rows < T.rows) {
        d = T.cy - (rows - 1);
        if (d > 0) {
            for (y = 0; y < d; y++) push_scrollback(&T.screen[y]);
            memmove(&T.screen[0], &T.screen[d], sizeof(Line) * (size_t)(T.rows - d));
            memmove(&T.alt[0], &T.alt[d], sizeof(Line) * (size_t)(T.rows - d));
            T.cy -= d;
        }
    } else if (!T.on_alt) {
        d = rows - T.rows;
        if (d > T.sb_count) d = T.sb_count;
        if (d > 0) {
            memmove(&T.screen[d], &T.screen[0], sizeof(Line) * (size_t)T.rows);
            for (y = 0; y < d; y++) T.screen[d - 1 - y] = *sb_line(y + 1);
            T.sb_head = (T.sb_head - d + SCROLLBACK) % SCROLLBACK;
            T.sb_count -= d;
            T.cy += d;
        }
        for (y = T.rows + d; y < rows; y++) clear_line(&T.screen[y], 0, T.cols);
    }
    for (y = T.rows; y < rows; y++) clear_line(&T.alt[y], 0, T.cols);
    T.rows = rows;
    T.top = 0; T.bottom = rows - 1;
    if (T.cy >= rows) T.cy = rows - 1;
    T.view = 0;
tell:
    T.dirty = 1;
    if (T.cx >= T.cols) T.cx = T.cols - 1;
    if (g_master >= 0) {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_row = (unsigned short)rows; ws.ws_col = (unsigned short)T.cols;
        ws.ws_xpixel = (unsigned short)(T.cols * g_cw); ws.ws_ypixel = (unsigned short)(rows * g_ch);
        ioctl(g_master, TIOCSWINSZ, &ws);
        if (g_child > 0) kill(-g_child, SIGWINCH);
    }
}

static void pty_write(const char *s, size_t n)
{
    while (n > 0 && g_master >= 0) {
        ssize_t w = write(g_master, s, n);
        if (w < 0) { if (errno == EINTR || errno == EAGAIN) { usleep(1000); continue; } return; }
        s += w; n -= (size_t)w;
    }
}
static void pty_puts(const char *s) { pty_write(s, strlen(s)); }

static int g_bell;

static void put_char(uint8_t ch)
{
    Cell *c;
    if (T.wrap_pending) {
        T.wrap_pending = 0;
        T.cx = 0;
        if (T.cy == T.bottom) scroll_up(T.top, T.bottom, 1);
        else if (T.cy < T.rows - 1) T.cy++;
    }
    c = &line(T.cy)->c[T.cx];
    c->ch = ch;
    c->attr = T.pen.attr;
    if (T.pen.fg == -2) { c->fg = T.pen.fg_rgb; c->attr |= A_FGRGB; }
    else c->fg = (uint16_t)((T.pen.attr & A_BOLD) && T.pen.fg >= 0 && T.pen.fg < 8 ? T.pen.fg + 8 : T.pen.fg);
    if (T.pen.bg == -2) { c->bg = T.pen.bg_rgb; c->attr |= A_BGRGB; }
    else c->bg = (uint16_t)T.pen.bg;
    if (T.cx == T.cols - 1) { if (T.autowrap) T.wrap_pending = 1; }
    else T.cx++;
    T.dirty = 1;
}

static void newline(void)
{
    T.wrap_pending = 0;
    if (T.cy == T.bottom) scroll_up(T.top, T.bottom, 1);
    else if (T.cy < T.rows - 1) T.cy++;
    T.dirty = 1;
}

static int P(int i, int def) { return (i < T.nparams && T.params[i] > 0) ? T.params[i] : def; }
static int P0(int i) { return i < T.nparams ? T.params[i] : 0; }

static void clamp_cursor(void)
{
    if (T.cx < 0) T.cx = 0;
    if (T.cx >= T.cols) T.cx = T.cols - 1;
    if (T.cy < 0) T.cy = 0;
    if (T.cy >= T.rows) T.cy = T.rows - 1;
    T.wrap_pending = 0;
}

static void sgr(void)
{
    int i;
    if (T.nparams == 0) { T.pen.fg = DEF_FG; T.pen.bg = DEF_BG; T.pen.attr = 0; return; }
    for (i = 0; i < T.nparams; i++) {
        int p = T.params[i];
        if (p == 0) { T.pen.fg = DEF_FG; T.pen.bg = DEF_BG; T.pen.attr = 0; }
        else if (p == 1) T.pen.attr |= A_BOLD;
        else if (p == 4) T.pen.attr |= A_UNDER;
        else if (p == 7) T.pen.attr |= A_REVERSE;
        else if (p == 22) T.pen.attr &= (uint8_t)~A_BOLD;
        else if (p == 24) T.pen.attr &= (uint8_t)~A_UNDER;
        else if (p == 27) T.pen.attr &= (uint8_t)~A_REVERSE;
        else if (p >= 30 && p <= 37) T.pen.fg = p - 30;
        else if (p == 39) T.pen.fg = DEF_FG;
        else if (p >= 40 && p <= 47) T.pen.bg = p - 40;
        else if (p == 49) T.pen.bg = DEF_BG;
        else if (p >= 90 && p <= 97) T.pen.fg = p - 90 + 8;
        else if (p >= 100 && p <= 107) T.pen.bg = p - 100 + 8;
        else if ((p == 38 || p == 48) && i + 1 < T.nparams) {
            int *dst = p == 38 ? &T.pen.fg : &T.pen.bg;
            uint16_t *rgb = p == 38 ? &T.pen.fg_rgb : &T.pen.bg_rgb;
            if (T.params[i + 1] == 5 && i + 2 < T.nparams) { *dst = T.params[i + 2] & 255; i += 2; }
            else if (T.params[i + 1] == 2 && i + 4 < T.nparams) {
                *rgb = pspexp_color_5650((uint8_t)T.params[i + 2], (uint8_t)T.params[i + 3], (uint8_t)T.params[i + 4]);
                *dst = -2; i += 4;
            }
        }
    }
}

static void set_mode(int on)
{
    int i;
    for (i = 0; i < T.nparams; i++) {
        int p = T.params[i];
        if (!T.private_mode) continue;   /* ANSI modes (insert, ...) are not needed by the usual tools */
        if (p == 25) T.cursor_visible = on;
        else if (p == 7) T.autowrap = on;
        else if (p == 1) T.app_cursor = on;
        else if (p == 1000 || p == 1002 || p == 1003) T.mouse_mode = on ? p : 0;
        else if (p == 1006) T.mouse_sgr = on;
        else if (p == 47 || p == 1047 || p == 1049) {
            if (on && !T.on_alt) {
                int y;
                if (p == 1049) { T.saved_cx = T.cx; T.saved_cy = T.cy; T.saved_pen = T.pen; }
                T.on_alt = 1;
                for (y = 0; y < ROWS_MAX; y++) clear_line(&T.alt[y], 0, COLS_MAX);
            } else if (!on && T.on_alt) {
                T.on_alt = 0;
                if (p == 1049) { T.cx = T.saved_cx; T.cy = T.saved_cy; T.pen = T.saved_pen; clamp_cursor(); }
            }
            T.dirty = 1;
        }
    }
}

static void csi(char f)
{
    int n, y;
    char buf[48];
    switch (f) {
    case 'A': T.cy -= P(0, 1); if (T.cy < T.top && T.cy + P(0, 1) >= T.top) T.cy = T.top; clamp_cursor(); break;
    case 'B': case 'e': T.cy += P(0, 1); if (T.cy > T.bottom && T.cy - P(0, 1) <= T.bottom) T.cy = T.bottom; clamp_cursor(); break;
    case 'C': case 'a': T.cx += P(0, 1); clamp_cursor(); break;
    case 'D': T.cx -= P(0, 1); clamp_cursor(); break;
    case 'E': T.cy += P(0, 1); T.cx = 0; clamp_cursor(); break;
    case 'F': T.cy -= P(0, 1); T.cx = 0; clamp_cursor(); break;
    case 'G': case '`': T.cx = P(0, 1) - 1; clamp_cursor(); break;
    case 'd': T.cy = P(0, 1) - 1; clamp_cursor(); break;
    case 'H': case 'f': T.cy = P(0, 1) - 1; T.cx = P(1, 1) - 1; clamp_cursor(); break;
    case 'J':
        n = P0(0);
        if (n == 0) { clear_line(line(T.cy), T.cx, T.cols); for (y = T.cy + 1; y < T.rows; y++) clear_line(line(y), 0, T.cols); }
        else if (n == 1) { clear_line(line(T.cy), 0, T.cx + 1); for (y = 0; y < T.cy; y++) clear_line(line(y), 0, T.cols); }
        else { for (y = 0; y < T.rows; y++) clear_line(line(y), 0, T.cols); }
        T.dirty = 1; break;
    case 'K':
        n = P0(0);
        if (n == 0) clear_line(line(T.cy), T.cx, T.cols);
        else if (n == 1) clear_line(line(T.cy), 0, T.cx + 1);
        else clear_line(line(T.cy), 0, T.cols);
        T.dirty = 1; break;
    case 'L': if (T.cy >= T.top && T.cy <= T.bottom) scroll_down(T.cy, T.bottom, P(0, 1)); break;
    case 'M': if (T.cy >= T.top && T.cy <= T.bottom) scroll_up(T.cy, T.bottom, P(0, 1)); break;
    case 'S': scroll_up(T.top, T.bottom, P(0, 1)); break;
    case 'T': scroll_down(T.top, T.bottom, P(0, 1)); break;
    case 'P': {
        Line *l = line(T.cy); int k = P(0, 1), x;
        if (k > T.cols - T.cx) k = T.cols - T.cx;
        for (x = T.cx; x < T.cols - k; x++) l->c[x] = l->c[x + k];
        clear_line(l, T.cols - k, T.cols); T.dirty = 1; break;
    }
    case '@': {
        Line *l = line(T.cy); int k = P(0, 1), x;
        if (k > T.cols - T.cx) k = T.cols - T.cx;
        for (x = T.cols - 1; x >= T.cx + k; x--) l->c[x] = l->c[x - k];
        clear_line(l, T.cx, T.cx + k); T.dirty = 1; break;
    }
    case 'X': clear_line(line(T.cy), T.cx, T.cx + P(0, 1)); T.dirty = 1; break;
    case 'm': sgr(); break;
    case 'r':
        T.top = P(0, 1) - 1; T.bottom = P(1, T.rows) - 1;
        if (T.top < 0) T.top = 0;
        if (T.bottom >= T.rows) T.bottom = T.rows - 1;
        if (T.top >= T.bottom) { T.top = 0; T.bottom = T.rows - 1; }
        T.cx = 0; T.cy = 0; T.wrap_pending = 0; break;
    case 's': T.saved_cx = T.cx; T.saved_cy = T.cy; break;
    case 'u': T.cx = T.saved_cx; T.cy = T.saved_cy; clamp_cursor(); break;
    case 'h': set_mode(1); break;
    case 'l': set_mode(0); break;
    case 'n':
        if (P0(0) == 6) { snprintf(buf, sizeof(buf), "\033[%d;%dR", T.cy + 1, T.cx + 1); pty_puts(buf); }
        else if (P0(0) == 5) pty_puts("\033[0n");
        break;
    case 'c': if (!T.private_mode) pty_puts("\033[?6c"); break;
    case 't': if (P0(0) == 18) { snprintf(buf, sizeof(buf), "\033[8;%d;%dt", T.rows, T.cols); pty_puts(buf); } break;
    default: break;
    }
}

/* ---- characters beyond ASCII ------------------------------------------------ */

/* the line-drawing and block characters drawn for any cell size (box_glyph),
 * cell codes 0x80 + i; arms: left, right, up, down; 1 single, 2 double */
typedef struct BoxChar { uint16_t cp; uint8_t l, r, u, d; uint8_t block; } BoxChar;
enum { BK_NONE, BK_FULL, BK_TOP, BK_BOTTOM, BK_LEFT, BK_RIGHT, BK_LIGHT, BK_MEDIUM, BK_DARK };
static const BoxChar BOX[] = {
    { 0x2500, 1,1,0,0, 0 }, { 0x2502, 0,0,1,1, 0 }, { 0x250c, 0,1,0,1, 0 }, { 0x2510, 1,0,0,1, 0 },
    { 0x2514, 0,1,1,0, 0 }, { 0x2518, 1,0,1,0, 0 }, { 0x251c, 0,1,1,1, 0 }, { 0x2524, 1,0,1,1, 0 },
    { 0x252c, 1,1,0,1, 0 }, { 0x2534, 1,1,1,0, 0 }, { 0x253c, 1,1,1,1, 0 },
    { 0x2550, 2,2,0,0, 0 }, { 0x2551, 0,0,2,2, 0 }, { 0x2554, 0,2,0,2, 0 }, { 0x2557, 2,0,0,2, 0 },
    { 0x255a, 0,2,2,0, 0 }, { 0x255d, 2,0,2,0, 0 }, { 0x2560, 0,2,2,2, 0 }, { 0x2563, 2,0,2,2, 0 },
    { 0x2566, 2,2,0,2, 0 }, { 0x2569, 2,2,2,0, 0 }, { 0x256c, 2,2,2,2, 0 },
    { 0x2588, 0,0,0,0, BK_FULL }, { 0x2580, 0,0,0,0, BK_TOP }, { 0x2584, 0,0,0,0, BK_BOTTOM },
    { 0x258c, 0,0,0,0, BK_LEFT }, { 0x2590, 0,0,0,0, BK_RIGHT }, { 0x2591, 0,0,0,0, BK_LIGHT },
    { 0x2592, 0,0,0,0, BK_MEDIUM }, { 0x2593, 0,0,0,0, BK_DARK },
};
#define NBOX ((int)(sizeof(BOX) / sizeof(BOX[0])))

/* a Unicode character -> a cell code: Latin-1 as itself, line drawing and
 * blocks (heavy and rounded ones as light), a few look-alikes, else '?' */
static uint8_t glyph_code(uint32_t cp)
{
    static const struct { uint16_t from, to; } alias[] = {
        { 0x2501, 0x2500 }, { 0x2503, 0x2502 }, { 0x250f, 0x250c }, { 0x2513, 0x2510 }, { 0x2517, 0x2514 },
        { 0x251b, 0x2518 }, { 0x2523, 0x251c }, { 0x252b, 0x2524 }, { 0x2533, 0x252c }, { 0x253b, 0x2534 },
        { 0x254b, 0x253c }, { 0x256d, 0x250c }, { 0x256e, 0x2510 }, { 0x256f, 0x2518 }, { 0x2570, 0x2514 },
        { 0x2552, 0x250c }, { 0x2553, 0x250c }, { 0x2555, 0x2510 }, { 0x2556, 0x2510 }, { 0x2558, 0x2514 },
        { 0x2559, 0x2514 }, { 0x255b, 0x2518 }, { 0x255c, 0x2518 }, { 0x2574, 0x2500 }, { 0x2576, 0x2500 },
        { 0x2575, 0x2502 }, { 0x2577, 0x2502 },
    };
    size_t i;
    if (cp >= 32 && cp < 127) return (uint8_t)cp;
    if (cp >= 0xa0 && cp <= 0xff) return (uint8_t)cp;
    for (i = 0; i < sizeof(alias) / sizeof(alias[0]); i++) if (alias[i].from == cp) { cp = alias[i].to; break; }
    for (i = 0; i < (size_t)NBOX; i++) if (BOX[i].cp == cp) return (uint8_t)(0x80 + i);
    switch (cp) {
    case 0x2018: case 0x2019: case 0x2032: return '\'';
    case 0x201c: case 0x201d: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x2022: case 0x2219: case 0x22c5: case 0x25cf: return 0xb7;   /* bullets: the middle dot */
    case 0x2026: return '.';
    case 0x2190: case 0x25c0: case 0x25c2: return '<';
    case 0x2192: case 0x25b6: case 0x25b8: return '>';
    case 0x2191: case 0x25b2: return '^';
    case 0x2193: case 0x25bc: return 'v';
    case 0x2713: case 0x2714: return 'v';
    case 0x2717: case 0x2718: return 'x';
    case 0x00a0: return ' ';
    default: return '?';
    }
}

/* DEC special graphics (ESC ( 0), what curses draws boxes with on terminals
 * without Unicode */
static uint8_t dec_graphic(uint8_t b)
{
    uint32_t cp;
    switch (b) {
    case 'j': cp = 0x2518; break; case 'k': cp = 0x2510; break; case 'l': cp = 0x250c; break;
    case 'm': cp = 0x2514; break; case 'n': cp = 0x253c; break; case 'q': cp = 0x2500; break;
    case 't': cp = 0x251c; break; case 'u': cp = 0x2524; break; case 'v': cp = 0x2534; break;
    case 'w': cp = 0x252c; break; case 'x': cp = 0x2502; break; case 'a': cp = 0x2592; break;
    case '0': cp = 0x2588; break; case '`': return '+'; case '~': return 0xb7; case 'f': return 0xb0;
    case 'g': return 0xb1; case 'y': return '<'; case 'z': return '>'; case '_': return ' ';
    default: return b;
    }
    return glyph_code(cp);
}

enum { S_GROUND, S_ESC, S_CSI, S_OSC, S_OSC_ESC, S_CHARSET };

static void term_feed(const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t b = p[i];
        switch (T.state) {
        case S_GROUND:
            if (T.utf8_left) {
                if ((b & 0xC0) == 0x80) {
                    T.utf8 = (T.utf8 << 6) | (b & 0x3F);
                    if (--T.utf8_left == 0) put_char(glyph_code(T.utf8));
                    continue;
                }
                T.utf8_left = 0;
            }
            if (b >= 0xC0 && b < 0xF8) {
                T.utf8_left = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : 1;
                T.utf8 = b & (b >= 0xF0 ? 0x07 : b >= 0xE0 ? 0x0F : 0x1F);
                continue;
            }
            if (b >= 0x80) { put_char('?'); continue; }
            if (b >= 32 && b < 127) { put_char(T.dec_graphics && b >= 0x5f ? dec_graphic(b) : b); continue; }
            switch (b) {
            case 7: g_bell = 1; break;
            case 8: if (T.cx > 0) T.cx--; T.wrap_pending = 0; T.dirty = 1; break;
            case 9: T.cx = (T.cx / 8 + 1) * 8; if (T.cx >= T.cols) T.cx = T.cols - 1; T.wrap_pending = 0; T.dirty = 1; break;
            case 10: case 11: case 12: newline(); break;
            case 13: T.cx = 0; T.wrap_pending = 0; T.dirty = 1; break;
            case 27: T.state = S_ESC; break;
            default: break;
            }
            break;
        case S_ESC:
            T.state = S_GROUND;
            switch (b) {
            case '[': T.state = S_CSI; T.nparams = 0; T.private_mode = 0; T.param_started = 0; memset(T.params, 0, sizeof(T.params)); break;
            case ']': T.state = S_OSC; break;
            case '(': T.state = S_CHARSET; T.charset_g0 = 1; break;
            case ')': case '*': case '+': T.state = S_CHARSET; T.charset_g0 = 0; break;
            case '7': T.saved_cx = T.cx; T.saved_cy = T.cy; T.saved_pen = T.pen; break;
            case '8': T.cx = T.saved_cx; T.cy = T.saved_cy; T.pen = T.saved_pen; clamp_cursor(); break;
            case 'D': newline(); break;
            case 'E': T.cx = 0; newline(); break;
            case 'M': if (T.cy == T.top) scroll_down(T.top, T.bottom, 1); else if (T.cy > 0) T.cy--; T.dirty = 1; break;
            case 'c': term_reset(T.rows); break;
            default: break;   /* ESC = / ESC > (keypad modes) and others */
            }
            break;
        case S_CHARSET: if (T.charset_g0) T.dec_graphics = b == '0'; T.state = S_GROUND; break;
        case S_CSI:
            if (b >= '0' && b <= '9') {
                if (T.nparams == 0) T.nparams = 1;
                if (T.params[T.nparams - 1] < 10000) T.params[T.nparams - 1] = T.params[T.nparams - 1] * 10 + (b - '0');
                T.param_started = 1;
            } else if (b == ';' || b == ':') {
                if (T.nparams == 0) T.nparams = 1;
                if (T.nparams < 16) T.nparams++;
            } else if (b == '?' || b == '>' || b == '=') {
                T.private_mode = 1;
            } else if (b >= 0x40 && b <= 0x7E) {
                csi((char)b);
                T.state = S_GROUND;
            } else if (b == 27) {
                T.state = S_ESC;
            }   /* intermediates (space, !, ", $ ...) ignored */
            break;
        case S_OSC:   /* window title and the like: skipped until BEL or ESC \ */
            if (b == 7) T.state = S_GROUND;
            else if (b == 27) T.state = S_OSC_ESC;
            break;
        case S_OSC_ESC: T.state = b == '\\' ? S_GROUND : S_OSC; break;
        }
    }
}

/* ---- on-screen keyboard -------------------------------------------------- */

enum { K_TEXT, K_SHIFT, K_CTRL };
typedef struct Key { const char *label, *out, *sout; uint8_t w; uint8_t kind; } Key;   /* w in 16 px units, 30 per row */

#define KROWS 5
static const Key KROW0[] = {
    {"Esc","\033",0,2,0},{"1","1","!",2,0},{"2","2","@",2,0},{"3","3","#",2,0},{"4","4","$",2,0},{"5","5","%",2,0},
    {"6","6","^",2,0},{"7","7","&",2,0},{"8","8","*",2,0},{"9","9","(",2,0},{"0","0",")",2,0},{"-","-","_",2,0},
    {"=","=","+",2,0},{"`","`","~",2,0},{"Bk","\177",0,2,0}};
static const Key KROW1[] = {
    {"Tab","\t",0,3,0},{"q","q","Q",2,0},{"w","w","W",2,0},{"e","e","E",2,0},{"r","r","R",2,0},{"t","t","T",2,0},
    {"y","y","Y",2,0},{"u","u","U",2,0},{"i","i","I",2,0},{"o","o","O",2,0},{"p","p","P",2,0},{"[","[","{",2,0},
    {"]","]","}",2,0},{"\\","\\","|",3,0}};
static const Key KROW2[] = {
    {"Ctrl",0,0,4,K_CTRL},{"a","a","A",2,0},{"s","s","S",2,0},{"d","d","D",2,0},{"f","f","F",2,0},{"g","g","G",2,0},
    {"h","h","H",2,0},{"j","j","J",2,0},{"k","k","K",2,0},{"l","l","L",2,0},{";",";",":",2,0},{"'","'","\"",2,0},
    {"Enter","\r",0,4,0}};
static const Key KROW3[] = {
    {"Shift",0,0,5,K_SHIFT},{"z","z","Z",2,0},{"x","x","X",2,0},{"c","c","C",2,0},{"v","v","V",2,0},{"b","b","B",2,0},
    {"n","n","N",2,0},{"m","m","M",2,0},{",",",","<",2,0},{".",".",">",2,0},{"/","/","?",2,0},{"^","\033[A",0,2,0},
    {"Del","\033[3~",0,3,0}};
static const Key KROW4[] = {
    {"Hm","\033[H",0,2,0},{"End","\033[F",0,2,0},{"PgU","\033[5~",0,3,0},{"PgD","\033[6~",0,3,0},{"Space"," ",0,10,0},
    {"<","\033[D",0,2,0},{"v","\033[B",0,2,0},{">","\033[C",0,2,0},{"^C","\003",0,2,0},{"^D","\004",0,2,0}};
static const Key *KROWS_[KROWS] = { KROW0, KROW1, KROW2, KROW3, KROW4 };
static const int KCOUNT[KROWS] = { 15, 14, 13, 13, 10 };
#define KEY_H (KB_H / KROWS)   /* 14 px */

static int kb_shown = 1, kb_row = 2, kb_col = 1;
static int sticky_shift, sticky_ctrl;
static int held_shift, held_ctrl;   /* L / R held: shown on the keyboard too */

static int key_x(int row, int col) { int x = 0, i; for (i = 0; i < col; i++) x += KROWS_[row][i].w * 16; return x; }

static int key_at_x(int row, int px)
{
    int x = 0, i;
    for (i = 0; i < KCOUNT[row]; i++) { int w = KROWS_[row][i].w * 16; if (px < x + w) return i; x += w; }
    return KCOUNT[row] - 1;
}

static void send_key(const Key *k, int shift, int ctrl)
{
    const char *s;
    if (k->kind == K_SHIFT) { sticky_shift = !sticky_shift; return; }
    if (k->kind == K_CTRL) { sticky_ctrl = !sticky_ctrl; return; }
    shift |= sticky_shift; ctrl |= sticky_ctrl;
    sticky_shift = sticky_ctrl = 0;
    s = (shift && k->sout) ? k->sout : k->out;
    if (ctrl && s[0] && !s[1]) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
        else if (c >= '@' && c <= '_') c = (char)(c - '@');
        else if (c == ' ') c = 0;
        pty_write(&c, 1);
        return;
    }
    if (T.app_cursor && s[0] == '\033' && s[1] == '[' && s[2] >= 'A' && s[2] <= 'D' && !s[3]) {
        char seq[4] = { '\033', 'O', s[2], 0 };   /* application cursor keys (vi, less) */
        pty_puts(seq);
        return;
    }
    pty_puts(s);
}

static void send_arrow(char d)
{
    char seq[4] = { '\033', T.app_cursor ? 'O' : '[', d, 0 };
    pty_puts(seq);
}

/* ---- rendering ------------------------------------------------------------ */

static void draw_pointer(void);
static void mirror_invalidate(void);
static void mirror_reset(void);
static void keypad_release_all(void);
static int mirror_frame(void);

static PspExpContext *ctx;
static PspExpFont font;
static int have_font, blink_on = 1;

typedef struct Batch { PspExpSpriteRec r[COLS_MAX * ROWS_MAX + 512]; int n; } Batch;
static Batch g_batch, g_tbatch;   /* the UI font's glyphs, the terminal font's */

static void glyph(int x, int y, uint8_t ch, uint16_t color)
{
    if (ch <= ' ' || ch < font.first_char || ch >= font.first_char + font.glyph_count) return;
    if (g_batch.n >= (int)(sizeof(g_batch.r) / sizeof(g_batch.r[0]))) return;
    g_batch.r[g_batch.n].x = (int16_t)x; g_batch.r[g_batch.n].y = (int16_t)y;
    g_batch.r[g_batch.n].frame = (uint16_t)(font.first_frame + (ch - font.first_char));
    g_batch.r[g_batch.n].color = color;
    g_batch.n++;
}

static void text(int x, int y, const char *s, uint16_t color) { for (; *s; s++, x += UI_W) glyph(x, y, (uint8_t)*s, color); }

/* ---- the terminal font: Spleen glyphs plus line drawing, one CLUT4 atlas ---- */

#define TFONT_ID 3
#define TF_COLS 16
static int have_tfont;

/* the frame of a cell code in the atlas: ASCII, Latin-1, then BOX[] */
static int tframe(uint8_t ch)
{
    if (ch >= 0x80 && ch < 0x80 + NBOX) return TERM_FONT_GLYPHS + (ch - 0x80);
    return term_font_index(ch);
}

/* a line-drawing or block character for a w x h cell */
static void box_glyph(const BoxChar *b, int w, int h, uint8_t *px /* w*h, 1 = on */)
{
    int cx = (w - 1) / 2, cy = (h - 1) / 2, x, y;
    memset(px, 0, (size_t)(w * h));
#define SET(X, Y) do { int X_ = (X), Y_ = (Y); if (X_ >= 0 && X_ < w && Y_ >= 0 && Y_ < h) px[Y_ * w + X_] = 1; } while (0)
    if (b->block) {
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            int on = 0;
            switch (b->block) {
            case BK_FULL: on = 1; break;
            case BK_TOP: on = y < h / 2; break;
            case BK_BOTTOM: on = y >= h / 2; break;
            case BK_LEFT: on = x < w / 2; break;
            case BK_RIGHT: on = x >= w / 2; break;
            case BK_LIGHT: on = (x % 2 == 0) && (y % 2 == 0); break;
            case BK_MEDIUM: on = (x + y) % 2 == 0; break;
            case BK_DARK: on = !((x % 2 == 0) && (y % 2 == 0)); break;
            }
            if (on) px[y * w + x] = 1;
        }
        return;
    }
    /* each arm from the centre to the edge; a double arm is two lines, one
     * pixel either side of the centre line; they meet at the centre */
    if (b->l) for (x = 0; x <= cx; x++) { if (b->l == 1) SET(x, cy); else { SET(x, cy - 1); SET(x, cy + 1); } }
    if (b->r) for (x = cx; x < w; x++) { if (b->r == 1) SET(x, cy); else { SET(x, cy - 1); SET(x, cy + 1); } }
    if (b->u) for (y = 0; y <= cy; y++) { if (b->u == 1) SET(cx, y); else { SET(cx - 1, y); SET(cx + 1, y); } }
    if (b->d) for (y = cy; y < h; y++) { if (b->d == 1) SET(cx, y); else { SET(cx - 1, y); SET(cx + 1, y); } }
    /* double corners and joins: close the inner gap */
    if ((b->l == 2 || b->r == 2) && (b->u == 2 || b->d == 2)) {
        SET(cx - 1, cy - 1); SET(cx + 1, cy - 1); SET(cx - 1, cy + 1); SET(cx + 1, cy + 1);
    }
#undef SET
}

/* uploads TERM_FONTS[g_font] as TFONT_ID (re-uploaded on a change; the
 * library replays it after reconnects) */
static void tfont_upload(void)
{
    static uint8_t pixels[128 * 256 / 2];
    static PspExpFrame frames[TERM_FONT_GLYPHS + NBOX];
    const TermFont *f = &TERM_FONTS[g_font];
    int n = TERM_FONT_GLYPHS + NBOX, i, x, y, aw = 128, ah;
    uint8_t cell[16 * 16];
    PspExpTextureDesc desc;
    ah = ((n + TF_COLS - 1) / TF_COLS) * f->h;
    ah = ah <= 64 ? 64 : ah <= 128 ? 128 : 256;
    memset(pixels, 0, sizeof(pixels));
    for (i = 0; i < n; i++) {
        int ox = (i % TF_COLS) * f->w, oy = (i / TF_COLS) * f->h;
        if (i < TERM_FONT_GLYPHS) {
            for (y = 0; y < f->h; y++) for (x = 0; x < f->w; x++) cell[y * f->w + x] = (f->bits[i * f->h + y] >> (7 - x)) & 1;
        } else box_glyph(&BOX[i - TERM_FONT_GLYPHS], f->w, f->h, cell);
        for (y = 0; y < f->h; y++) for (x = 0; x < f->w; x++)
            if (cell[y * f->w + x]) { int idx = (oy + y) * aw + ox + x; pixels[idx / 2] |= (uint8_t)(1u << ((idx & 1) * 4)); }
        frames[i].u0 = (uint16_t)ox; frames[i].v0 = (uint16_t)oy; frames[i].w = f->w; frames[i].h = f->h;
    }
    if (have_tfont) pspexp_release(ctx, TFONT_ID);
    memset(&desc, 0, sizeof(desc));
    desc.width = (uint16_t)aw; desc.height = (uint16_t)ah; desc.format = PSPEXP_TEX_T4;
    desc.layout = PSPEXP_LAYOUT_LINEAR; desc.priority = PSPEXP_PRIORITY_HIGH; desc.palette = FONT_PAL_ID;
    have_tfont = pspexp_upload_texture(ctx, TFONT_ID, &desc, pixels, (size_t)(aw * ah / 2), frames, (uint32_t)n) == 0;
    g_cw = f->w; g_ch = f->h;
}

static void tglyph(int x, int y, uint8_t ch, uint16_t color)
{
    int fr;
    if (ch == ' ' || (fr = tframe(ch)) < 0) return;
    if (g_tbatch.n >= (int)(sizeof(g_tbatch.r) / sizeof(g_tbatch.r[0]))) return;
    g_tbatch.r[g_tbatch.n].x = (int16_t)x; g_tbatch.r[g_tbatch.n].y = (int16_t)y;
    g_tbatch.r[g_tbatch.n].frame = (uint16_t)fr;
    g_tbatch.r[g_tbatch.n].color = color ? color : 0x0001;   /* 0 would mean "no tint" */
    g_tbatch.n++;
}

/* the on-screen rows: the keyboard takes the bottom 72 px when shown */
static int rows_full(void) { return 272 / g_ch > ROWS_MAX ? ROWS_MAX : 272 / g_ch; }
static int rows_kb(void) { return KB_Y / g_ch; }

static void draw_terminal(int rows)
{
    int y, x;
    uint16_t tbg = rgb565(theme()->bg);
    for (y = 0; y < rows; y++) {
        const Line *l;
        int src = y - T.view;   /* with scrollback: rows above the screen come from the history */
        if (src < 0) l = sb_line(-src); else l = line(src);
        /* backgrounds: runs of one colour other than the theme's */
        for (x = 0; x < T.cols;) {
            const Cell *c = &l->c[x];
            uint16_t bg = (c->attr & A_REVERSE) ? cell_fg(c) : cell_bg(c);
            int x1 = x + 1;
            while (x1 < T.cols) {
                const Cell *d = &l->c[x1];
                uint16_t bg2 = (d->attr & A_REVERSE) ? cell_fg(d) : cell_bg(d);
                if (bg2 != bg) break;
                x1++;
            }
            if (bg != tbg) pspexp_fill_rect(ctx, (int16_t)(x * g_cw), (int16_t)(y * g_ch), (int16_t)((x1 - x) * g_cw), (int16_t)g_ch, bg, 255);
            x = x1;
        }
        for (x = 0; x < T.cols; x++) {
            const Cell *c = &l->c[x];
            uint16_t fg = (c->attr & A_REVERSE) ? cell_bg(c) : cell_fg(c);
            tglyph(x * g_cw, y * g_ch, c->ch, fg);
            if (c->attr & A_UNDER) pspexp_fill_rect(ctx, (int16_t)(x * g_cw), (int16_t)(y * g_ch + g_ch - 1), (int16_t)g_cw, 1, fg, 255);
        }
    }
    if (T.view == 0 && T.cursor_visible && blink_on && T.cy < rows)
        pspexp_fill_rect(ctx, (int16_t)(T.cx * g_cw), (int16_t)(T.cy * g_ch + g_ch - 2), (int16_t)g_cw, 2, rgb565(theme()->cursor), 200);
}

/* while scrolled back: a label at the top right (UI font) */
static void draw_scroll_label(void)
{
    char s[64];
    if (T.view <= 0 && !g_scroll_mode) return;
    if (g_scroll_mode) snprintf(s, sizeof(s), " SCROLL MODE -%d  circle: back ", T.view);
    else snprintf(s, sizeof(s), " SCROLLBACK -%d ", T.view);
    pspexp_fill_rect(ctx, (int16_t)(477 - (int)strlen(s) * UI_W), 0, (int16_t)(strlen(s) * UI_W), UI_H, rgb565(theme()->accent), 255);
    text(477 - (int)strlen(s) * UI_W, 0, s, rgb565(theme()->accent_text));
}

static void draw_keyboard(void)
{
    int r, i;
    const TermTheme *th = theme();
    pspexp_fill_rect(ctx, 0, KB_Y, 480, KB_H, rgb565(th->panel), 255);
    for (r = 0; r < KROWS; r++) {
        int x = 0, y = KB_Y + r * KEY_H;
        for (i = 0; i < KCOUNT[r]; i++) {
            const Key *k = &KROWS_[r][i];
            int w = k->w * 16, sel = r == kb_row && i == kb_col;
            int shifted = sticky_shift || held_shift;
            int active = (k->kind == K_SHIFT && shifted) || (k->kind == K_CTRL && (sticky_ctrl || held_ctrl));
            uint16_t bg = rgb565(sel ? th->accent : active ? th->active : th->key);
            uint16_t fg = rgb565(sel ? th->accent_text : th->key_text);
            /* with Shift active the keys show what they will type */
            const char *label = (shifted && k->sout && k->kind == 0 && k->out && !k->out[1]) ? k->sout : k->label;
            int len = (int)strlen(label);
            pspexp_fill_rect(ctx, (int16_t)(x + 1), (int16_t)(y + 1), (int16_t)(w - 2), KEY_H - 2, bg, 255);
            text(x + (w - len * UI_W) / 2, y + (KEY_H - UI_H) / 2 + 1, label, fg);
            x += w;
        }
    }
}

/* ---- settings: ~/.config/pspexp-term/settings -------------------------------- */

static void settings_path(char *out, size_t n, const char *file)
{
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/.config/pspexp-term/%s", home ? home : ".", file);
}

static void settings_load(void)
{
    char path[512], line[160];
    FILE *f;
    settings_path(path, sizeof(path), "settings");
    if (!(f = fopen(path, "r"))) return;
    while (fgets(line, sizeof(line), f)) {
        char *v = strchr(line, '='), *e;
        int i;
        if (!v) continue;
        *v++ = 0;
        e = v + strlen(v); while (e > v && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        if (!strcmp(line, "theme") && (i = term_theme_find(v)) >= 0) g_theme = i;
        if (!strcmp(line, "font")) for (i = 0; i < TERM_FONT_COUNT; i++) if (!strcmp(TERM_FONTS[i].name, v)) g_font = i;
    }
    fclose(f);
}

static void settings_save(void)
{
    char path[512], dir[512];
    FILE *f;
    settings_path(dir, sizeof(dir), "");
    { char parent[512]; snprintf(parent, sizeof(parent), "%s", dir); *strrchr(parent, '/') = 0; *strrchr(parent, '/') = 0; mkdir(parent, 0755); }
    mkdir(dir, 0755);
    settings_path(path, sizeof(path), "settings");
    if (!(f = fopen(path, "w"))) return;
    fprintf(f, "theme=%s\nfont=%s\n", TERM_THEMES[g_theme].name, TERM_FONTS[g_font].name);
    fclose(f);
}

static int term_rows(void) { return kb_shown ? rows_kb() : rows_full(); }

/* a new terminal font: its atlas, the columns and rows, the shell told */
static void font_apply(int i)
{
    g_font = i;
    tfont_upload();
    T.cols = 480 / g_cw > COLS_MAX ? COLS_MAX : 480 / g_cw;
    term_resize(term_rows());
}

/* ---- the context menu (a tap of L) --------------------------------------------- */

static TermMenu g_menu;
static int g_menu_open, g_menu_sel, g_menu_top;
static int g_help_open;   /* the controls, on screen */
/* the list shown: the menu itself, or a choice of themes or fonts (moving
 * through them tries each at once; circle puts the old one back) */
enum { ML_MAIN, ML_THEMES, ML_FONTS };
static int g_menu_list, g_menu_undo;
/* the rows shown: a heading, then its items; heading rows have item NULL */
typedef struct MenuRow { const char *heading; const TermMenuItem *item; } MenuRow;
static MenuRow g_rows[80];
static int g_nrows;
static TermMenuItem g_choice[16];

static void menu_build(void)
{
    int sec = T.on_alt ? TM_FULL : TM_SHELL, i;
    g_nrows = 0;
    if (g_menu_list == ML_THEMES || g_menu_list == ML_FONTS) {
        int n = g_menu_list == ML_THEMES ? TERM_THEME_COUNT : TERM_FONT_COUNT;
        g_rows[g_nrows++] = (MenuRow){ g_menu_list == ML_THEMES ? "Theme" : "Font size", NULL };
        for (i = 0; i < n && i < 16; i++) {
            if (g_menu_list == ML_THEMES) snprintf(g_choice[i].label, sizeof(g_choice[i].label), "%s", TERM_THEMES[i].name);
            else snprintf(g_choice[i].label, sizeof(g_choice[i].label), "%s  (%d x %d characters)", TERM_FONTS[i].name,
                          480 / TERM_FONTS[i].w > COLS_MAX ? COLS_MAX : 480 / TERM_FONTS[i].w, 272 / TERM_FONTS[i].h > ROWS_MAX ? ROWS_MAX : 272 / TERM_FONTS[i].h);
            g_choice[i].kind = TM_ACTION;
            snprintf(g_choice[i].arg, sizeof(g_choice[i].arg), "choose");
            g_rows[g_nrows++] = (MenuRow){ NULL, &g_choice[i] };
        }
        g_menu_undo = g_menu_list == ML_THEMES ? g_theme : g_font;
        g_menu_sel = 1 + g_menu_undo;
        g_menu_top = 0;
        return;
    }
    g_rows[g_nrows++] = (MenuRow){ T.on_alt ? "Full-screen program" : "Shell", NULL };
    for (i = 0; i < g_menu.n[sec]; i++) g_rows[g_nrows++] = (MenuRow){ NULL, &g_menu.item[sec][i] };
    g_rows[g_nrows++] = (MenuRow){ "Terminal", NULL };
    for (i = 0; i < g_menu.n[TM_ALWAYS]; i++) g_rows[g_nrows++] = (MenuRow){ NULL, &g_menu.item[TM_ALWAYS][i] };
    g_menu_sel = 1;
    g_menu_top = 0;
}

static void menu_open(void)
{
    char path[512];
    settings_path(path, sizeof(path), "menu");
    term_menu_load(&g_menu, path);   /* read each time: edits apply at once */
    g_menu_list = ML_MAIN;
    menu_build();
    g_menu_open = 1;
    T.dirty = 1;
}

/* in a choice list: try the highlighted one */
static void menu_preview(void)
{
    int i = g_menu_sel - 1;
    if (g_menu_list == ML_THEMES && i != g_theme) { g_theme = i; T.dirty = 1; }
    if (g_menu_list == ML_FONTS && i != g_font) font_apply(i);
}

static void menu_move(int d)
{
    int i = g_menu_sel;
    do { i = (i + d + g_nrows) % g_nrows; } while (!g_rows[i].item && i != g_menu_sel);
    g_menu_sel = i;
    if (g_menu_list != ML_MAIN) menu_preview();
    T.dirty = 1;
}

/* circle: a choice list goes back to the old value and to the menu */
static void menu_back(void)
{
    if (g_menu_list == ML_THEMES) g_theme = g_menu_undo;
    else if (g_menu_list == ML_FONTS) { if (g_font != g_menu_undo) font_apply(g_menu_undo); }
    else { g_menu_open = 0; T.dirty = 1; return; }
    g_menu_list = ML_MAIN;
    menu_build();
    T.dirty = 1;
}

static void menu_run(const TermMenuItem *it)
{
    T.dirty = 1;
    if (g_menu_list != ML_MAIN) {   /* a theme or font chosen: kept */
        settings_save();
        g_menu_open = 0;
        return;
    }
    if (!strcmp(it->arg, "themes") && it->kind == TM_ACTION) { g_menu_list = ML_THEMES; menu_build(); return; }
    if (!strcmp(it->arg, "fonts") && it->kind == TM_ACTION) { g_menu_list = ML_FONTS; menu_build(); return; }
    g_menu_open = 0;
    if (it->kind == TM_KEYS) { pty_write(it->arg, (size_t)it->len); T.view = 0; return; }
    if (!strcmp(it->arg, "scroll-top")) T.view = T.on_alt ? 0 : T.sb_count;
    else if (!strcmp(it->arg, "scroll-bottom")) T.view = 0;
    else if (!strcmp(it->arg, "keyboard")) { kb_shown = !kb_shown; term_resize(term_rows()); }
    else if (!strcmp(it->arg, "keys")) g_help_open = 1;
    else if (!strcmp(it->arg, "scroll-mode")) g_scroll_mode = !T.on_alt;
    else fprintf(stderr, "[pspexp-term] menu: unknown action @%s\n", it->arg);
}

/* ---- the controls, on screen (menu: Controls...) ---------------------------------- */

static const char *const HELP[][2] = {
    { "#", "Typing" },
    { "D-pad", "move on the keyboard (hidden: arrow keys)" },
    { "Cross", "type the highlighted key" },
    { "Circle", "Backspace" },
    { "Square", "Space" },
    { "Triangle", "Enter" },
    { "Start", "Tab (completion)" },
    { "L held / R held", "Shift / Ctrl" },
    { "L + Triangle", "Esc" },
    { "R + Circle", "Ctrl-C (stop)" },
    { "Nub", "arrow keys (keyboard shown)" },
    { "#", "Screen" },
    { "Select", "keyboard on / off   held: the launcher" },
    { "R + up/down", "scroll a line   left/right: a page" },
    { "R + nub", "scroll like a wheel (keyboard hidden:" },
    { "", "  the nub alone)" },
    { "L + Select", "scroll mode: nothing held; circle leaves" },
    { "#", "Menu" },
    { "L tapped", "this menu: keys, commands, settings" },
    { "D-pad / Cross", "choose / run it     Circle: back" },
};

static void draw_help(void)
{
    const TermTheme *th = theme();
    int n = (int)(sizeof(HELP) / sizeof(HELP[0])), i, x0 = 40, y0 = 14, w = 400, h = n * 11 + 30;
    pspexp_fill_rect(ctx, (int16_t)(x0 - 1), (int16_t)(y0 - 1), (int16_t)(w + 2), (int16_t)(h + 2), rgb565(th->accent), 255);
    pspexp_fill_rect(ctx, (int16_t)x0, (int16_t)y0, (int16_t)w, (int16_t)h, rgb565(th->panel), 250);
    text(x0 + 8, y0 + 6, "Controls", rgb565(th->accent));
    for (i = 0; i < n; i++) {
        int y = y0 + 20 + i * 11;
        if (!strcmp(HELP[i][0], "#")) { text(x0 + 8, y, HELP[i][1], rgb565(th->heading)); continue; }
        text(x0 + 20, y, HELP[i][0], rgb565(th->accent));
        text(x0 + 20 + 17 * UI_W, y, HELP[i][1], rgb565(th->key_text));
    }
    text(x0 + w - 26 * UI_W, y0 + h - 12, "any button closes this", rgb565(th->heading));
}

#define MENU_W 34                      /* characters */
#define MENU_ROWS 28                   /* at most, on screen */
static void draw_menu(void)
{
    const TermTheme *th = theme();
    int shown = g_nrows < MENU_ROWS ? g_nrows : MENU_ROWS, i;
    int w = MENU_W * UI_W + 8, h = shown * 9 + 8, x0 = 480 - w - 6, y0 = (272 - h) / 2;
    if (g_menu_sel < g_menu_top + 1) g_menu_top = g_menu_sel > 0 ? g_menu_sel - 1 : 0;
    if (g_menu_sel >= g_menu_top + shown) g_menu_top = g_menu_sel - shown + 1;
    pspexp_fill_rect(ctx, (int16_t)(x0 - 1), (int16_t)(y0 - 1), (int16_t)(w + 2), (int16_t)(h + 2), rgb565(th->accent), 255);
    pspexp_fill_rect(ctx, (int16_t)x0, (int16_t)y0, (int16_t)w, (int16_t)h, rgb565(th->panel), 245);
    for (i = 0; i < shown; i++) {
        const MenuRow *r = &g_rows[g_menu_top + i];
        int y = y0 + 4 + i * 9;
        if (!r->item) {
            text(x0 + 4, y, r->heading, rgb565(th->heading));
            pspexp_fill_rect(ctx, (int16_t)(x0 + 6 + (int)strlen(r->heading) * UI_W), (int16_t)(y + 4),
                             (int16_t)(w - 10 - (int)strlen(r->heading) * UI_W), 1, rgb565(th->key), 255);
            continue;
        }
        if (g_menu_top + i == g_menu_sel) {
            pspexp_fill_rect(ctx, (int16_t)(x0 + 2), (int16_t)(y - 1), (int16_t)(w - 4), 9, rgb565(th->accent), 255);
            text(x0 + 10, y, r->item->label, rgb565(th->accent_text));
        } else text(x0 + 10, y, r->item->label, rgb565(th->key_text));
    }
    if (g_menu_top > 0) text(x0 + w - 10, y0 + 4, "^", rgb565(th->heading));
    if (g_menu_top + shown < g_nrows) text(x0 + w - 10, y0 + h - 12, "v", rgb565(th->heading));
}

/* while scrolled back: where in the history the screen is */
static void draw_scrollbar(int rows)
{
    int track = rows * g_ch, total = T.sb_count + rows, len, pos;
    if (T.view <= 0 || T.sb_count <= 0) return;
    len = track * rows / total; if (len < 6) len = 6;
    pos = (track - len) * (T.sb_count - T.view) / T.sb_count;
    pspexp_fill_rect(ctx, 477, 0, 3, (int16_t)track, rgb565(theme()->key), 200);
    pspexp_fill_rect(ctx, 477, (int16_t)pos, 3, (int16_t)len, rgb565(theme()->accent), 255);
}

static void render(void)
{
    int rows = term_rows();
    g_batch.n = 0; g_tbatch.n = 0;
    pspexp_begin_frame(ctx);
    pspexp_set_clip(ctx, 0, 0, 480, 272);
    pspexp_fill_rect(ctx, 0, 0, 480, 272, rgb565(theme()->bg), 255);
    if (have_font && have_tfont) {
        draw_terminal(rows);
        if (g_tbatch.n) pspexp_draw_sprite_batch(ctx, TFONT_ID, g_tbatch.r, (uint32_t)g_tbatch.n);
        if (kb_shown) draw_keyboard();
        draw_scroll_label();
        if (g_batch.n) pspexp_draw_sprite_batch(ctx, font.atlas, g_batch.r, (uint32_t)g_batch.n);
        g_batch.n = 0;
        draw_scrollbar(rows);
        if (g_menu_open) draw_menu();   /* over everything: its own text batch */
        if (g_help_open) draw_help();
        if (g_batch.n) pspexp_draw_sprite_batch(ctx, font.atlas, g_batch.r, (uint32_t)g_batch.n);
    }
    draw_pointer();
    pspexp_present(ctx);
    T.dirty = 0;
}

/* ---- sound: bell and the audio FIFO ------------------------------------------ */

static void beep(void)
{
    static int16_t tone[2 * 4410];   /* 100 ms, 880 Hz, fading */
    static int made;
    int i;
    if (!made) {
        for (i = 0; i < 4410; i++) {
            double env = 1.0 - (double)i / 4410.0;
            int16_t v = (int16_t)(sin(2.0 * 3.14159265 * 880.0 * i / 44100.0) * 6000.0 * env);
            tone[2 * i] = tone[2 * i + 1] = v;
        }
        made = 1;
    }
    pspexp_audio_stream(ctx, tone, 4410);
}

static int g_fifo = -1, g_fifo_keep = -1;
static uint64_t g_audio_t0; static uint64_t g_audio_sent;

static void audio_fifo_open(const char *path)
{
    struct stat st;
    if (!path) return;
    /* a regular file there (an ALSA "file" output that ran before us) would
     * grow forever: replace it with the FIFO */
    if (lstat(path, &st) == 0 && !S_ISFIFO(st.st_mode)) unlink(path);
    if (mkfifo(path, 0666) < 0 && errno != EEXIST) { fprintf(stderr, "pspexp-term: mkfifo %s: %s\n", path, strerror(errno)); return; }
    chmod(path, 0666);
    g_fifo = open(path, O_RDONLY | O_NONBLOCK);
    g_fifo_keep = open(path, O_WRONLY | O_NONBLOCK);   /* keeps the FIFO from reporting EOF between writers */
}

/* Raw 44.1 kHz S16 stereo from the FIFO, paced on our own clock so a writer
 * that dumps a whole file does not overrun the PSP's buffer (the pipe's own
 * back-pressure then holds the writer). */
static void audio_fifo_pump(void)
{
    static int16_t buf[2 * 2048];
    static int partial;
    uint64_t now, allowed;
    if (g_fifo < 0 || !ctx || pspexp_link_state(ctx) != PSPEXP_LINK_CONNECTED) return;
    now = pspexp_time_us();
    if (!g_audio_t0 || now - g_audio_t0 > 2000000u + g_audio_sent * 1000000u / 44100u) { g_audio_t0 = now; g_audio_sent = 0; }
    allowed = (now - g_audio_t0) * 44100u / 1000000u + 44100u / 5;   /* 200 ms ahead */
    while (g_audio_sent < allowed) {
        size_t want = (size_t)(allowed - g_audio_sent);
        ssize_t n;
        if (want > 2048) want = 2048;
        n = read(g_fifo, (char *)buf + partial, want * 4 - (size_t)partial);
        if (n <= 0) break;
        n += partial;
        partial = (int)(n % 4);
        if (n / 4) { pspexp_audio_stream(ctx, buf, (uint32_t)(n / 4)); g_audio_sent += (uint64_t)(n / 4); }
        if (partial) memmove(buf, (char *)buf + n - partial, (size_t)partial);
    }
}

/* ---- the shell ------------------------------------------------------------ */

static const char *g_shell, *g_cmd;

static void spawn_shell(void)
{
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)T.rows; ws.ws_col = T.cols;
    g_child = forkpty(&g_master, NULL, NULL, &ws);
    if (g_child < 0) { perror("forkpty"); exit(1); }
    if (g_child == 0) {
        setenv("TERM", "xterm", 1);
        setenv("PSPEXP_TERM", "1", 1);   /* lets login scripts tell the PSP console apart (the GameShell's starts X) */
        setenv("COLUMNS", "80", 1);
        unsetenv("LINES");
        if (g_cmd) execl("/bin/sh", "sh", "-c", g_cmd, (char *)NULL);
        else execl(g_shell, g_shell, "-l", (char *)NULL);
        _exit(127);
    }
    fcntl(g_master, F_SETFL, fcntl(g_master, F_GETFL) | O_NONBLOCK);
}

static void on_sigchld(int s) { (void)s; g_child_died = 1; }
static void on_usr1(int s) { (void)s; g_release_req = 1; }
static void on_usr2(int s) { (void)s; g_acquire_req = 1; }
static volatile sig_atomic_t g_quit;
static void on_term(int s) { (void)s; g_quit = 1; }

/* ---- the PSP link ------------------------------------------------------------ */

enum { MODE_USB, MODE_TCP, MODE_NULL, MODE_SDL };
static int g_mode;
static PspExpTcpOptions g_tcp;
#ifdef PSPEXP_HAVE_USB
static PspExpUsbOptions g_usb;
#endif
static PspExpNullExecutor *g_nul;
#ifdef PSPEXP_HAVE_SDL
static PspExpSdlExecutor *g_sdl;
#endif
static int g_released;

static void log_fn(void *u, const char *msg) { (void)u; fprintf(stderr, "[pspexp-term] %s\n", msg); }

static int g_want_usb, g_want_tcp;   /* both may be set: USB is tried first, then TCP */

static int g_link_tcp;   /* the current link is TCP (Wi-Fi), not USB: games are told so */

static PspExpTransport *open_transport(void)
{
    char err[200];
    PspExpTransport *t = NULL;
#ifdef PSPEXP_HAVE_USB
    if (g_want_usb) t = pspexp_usb_open(&g_usb, err, sizeof(err));
#endif
    g_link_tcp = 0;
    if (!t && g_want_tcp && (t = pspexp_tcp_open(&g_tcp, err, sizeof(err)))) g_link_tcp = 1;
    return t;
}

static int try_connect(void)
{
    PspExpTransport *t = open_transport();
    if (!t) return 0;
    pspexp_set_transport(ctx, t);
    if (pspexp_connect(ctx, 3000) < 0) { pspexp_set_transport(ctx, NULL); return 0; }
    fprintf(stderr, "[pspexp-term] PSP connected\n");
    T.dirty = 1;
    mirror_reset();
    {
        char link[160];
        if (g_link_tcp) snprintf(link, sizeof(link), "Wi-Fi, PSP at %s", g_tcp.host ? g_tcp.host : "?");
        else snprintf(link, sizeof(link), "USB");
        launcher_set_link(link);
    }
    launcher_invalidate();
    return 1;
}

static void release_psp(void)
{
    if (g_released) return;
    keypad_release_all();
    if (pspexp_link_state(ctx) == PSPEXP_LINK_CONNECTED) pspexp_send_raw(ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_DISCONNECT, NULL, 0);
    pspexp_set_transport(ctx, NULL);
    g_released = 1;
    fprintf(stderr, "[pspexp-term] PSP released\n");
}

/* ---- input: buttons with auto-repeat --------------------------------------------- */

enum { V_UP, V_DOWN, V_LEFT, V_RIGHT, N_UP, N_DOWN, N_LEFT, N_RIGHT, V_CROSS, V_CIRCLE, V_COUNT };
static uint64_t v_down_at[V_COUNT], v_next[V_COUNT];
static int v_state[V_COUNT];

/* 1 on press and on each auto-repeat */
static int fire(int v, int held, uint64_t now, int repeat)
{
    if (!held) { v_state[v] = 0; return 0; }
    if (!v_state[v]) { v_state[v] = 1; v_down_at[v] = now; v_next[v] = now + 350000u; return 1; }
    if (repeat && now >= v_next[v]) { v_next[v] = now + 60000u; return 1; }
    return 0;
}

static uint32_t g_prev_buttons;
static int g_prev_l_held;
static int g_prev_select_held, g_prev_select_released, g_select_tap;
static int g_view_mirror;           /* 1: the host framebuffer is shown, buttons drive the keypad */
static int mirror_available(void);
static void mirror_input(uint32_t b, uint32_t pressed, int ax, int ay);
static int g_view_launcher, g_launcher_on;   /* --launcher: the menu tree as the home view (launcher.c) */
static const char *g_launcher_assets;
static pid_t g_launch_pid;                   /* a program started from the launcher */
static int g_launch_aware;                   /* it hands the PSP over itself: no mirror while it runs */

/* hold select: launcher -> terminal -> mirror -> launcher (the views that exist) */
static void view_cycle(void)
{
    if (g_view_launcher) { g_view_launcher = 0; T.dirty = 1; }
    else if (!g_view_mirror) {
        if (mirror_available()) { g_view_mirror = 1; mirror_invalidate(); }
        else if (g_launcher_on) { g_view_launcher = 1; launcher_invalidate(); }
    } else {
        keypad_release_all();
        g_view_mirror = 0;
        if (g_launcher_on) { g_view_launcher = 1; launcher_invalidate(); } else T.dirty = 1;
    }
}

/* PSPEXP_TERM_KEYS="right,down,cross,..." (tests, screenshots): each name is
 * pressed for 150 ms, released for 150 ms, in order; "wait" is a 300 ms pause */
static const char *g_test_keys;
static uint32_t test_keys_buttons(uint64_t now)
{
    static const struct { const char *n; uint32_t b; } names[] = {
        { "up", PSPEXP_BUTTON_UP }, { "down", PSPEXP_BUTTON_DOWN }, { "left", PSPEXP_BUTTON_LEFT }, { "right", PSPEXP_BUTTON_RIGHT },
        { "cross", PSPEXP_BUTTON_CROSS }, { "circle", PSPEXP_BUTTON_CIRCLE }, { "square", PSPEXP_BUTTON_SQUARE }, { "triangle", PSPEXP_BUTTON_TRIANGLE },
        { "l", PSPEXP_BUTTON_LTRIGGER }, { "r", PSPEXP_BUTTON_RTRIGGER }, { "start", PSPEXP_BUTTON_START }, { "select", PSPEXP_BUTTON_SELECT }, { "wait", 0 } };
    static uint64_t t0;
    static int step_len;
    int step, i, k;
    const char *s = g_test_keys;
    if (!t0) t0 = now;
    step = (int)((now - t0) / 300000u);
    for (k = 0; k < step && *s; k++) { s = strchr(s, ','); if (!s) return 0; s++; }
    if (!*s) return 0;
    step_len = (int)strcspn(s, ",");
    if ((now - t0) % 300000u >= 150000u) return 0;   /* released half */
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
        if ((int)strlen(names[i].n) == step_len && !strncmp(names[i].n, s, (size_t)step_len)) return names[i].b;
    return 0;
}

static void handle_input(void)
{
    const PspExpInput *in = pspexp_input(ctx);
    uint64_t now = pspexp_time_us();
    uint32_t b = g_test_keys ? test_keys_buttons(now) : in->buttons, pressed = b & ~g_prev_buttons;
    int shift = (b & PSPEXP_BUTTON_LTRIGGER) != 0, ctrl = (b & PSPEXP_BUTTON_RTRIGGER) != 0;
    int ax = (int)in->analog_x - 128, ay = (int)in->analog_y - 128;
    int up = fire(V_UP, b & PSPEXP_BUTTON_UP, now, 1), down = fire(V_DOWN, b & PSPEXP_BUTTON_DOWN, now, 1);
    int left = fire(V_LEFT, b & PSPEXP_BUTTON_LEFT, now, 1), right = fire(V_RIGHT, b & PSPEXP_BUTTON_RIGHT, now, 1);
    g_prev_buttons = b;
    /* select: a long hold switches between the terminal and the mirrored
     * screen; a tap is resolved on release (keyboard toggle / Space) */
    {
        static uint64_t sel_down; static int sel_used;
        int sel_tap = 0;
        if (pressed & PSPEXP_BUTTON_SELECT) { sel_down = now; sel_used = 0; }
        if ((b & PSPEXP_BUTTON_SELECT) && !sel_used && now - sel_down >= 700000u && (mirror_available() || g_launcher_on)) {
            sel_used = 1;
            view_cycle();
        }
        if ((g_prev_select_released = (!(b & PSPEXP_BUTTON_SELECT) && g_prev_select_held))) sel_tap = !sel_used;
        g_prev_select_held = (b & PSPEXP_BUTTON_SELECT) != 0;
        g_select_tap = sel_tap;
        if (!mirror_available() && !g_launcher_on) g_select_tap = (pressed & PSPEXP_BUTTON_SELECT) != 0;   /* no other view: taps act at once */
    }
    if (g_view_mirror) { mirror_input(b, pressed, ax, ay); return; }
    if (g_view_launcher) { launcher_input(b, pressed, up, down, left, right, now); return; }
    if (shift != held_shift || ctrl != held_ctrl) { held_shift = shift; held_ctrl = ctrl; if (kb_shown) T.dirty = 1; }

    /* L tapped alone (no other button, no nub, under 0.4 s): the context menu;
     * held, it stays Shift */
    {
        static uint64_t l_down; static int l_used;
        int nub = ax < -60 || ax > 60 || ay < -60 || ay > 60;
        if (pressed & PSPEXP_BUTTON_LTRIGGER) { l_down = now; l_used = 0; }
        if (shift && ((b & ~PSPEXP_BUTTON_LTRIGGER) || nub)) l_used = 1;
        if (!shift && (g_prev_l_held) && !l_used && now - l_down < 400000u) {
            if (g_menu_open) { g_menu_open = 0; T.dirty = 1; } else menu_open();
        }
        g_prev_l_held = shift;
    }
    if (g_help_open) {
        if (pressed || up || down || left || right) { g_help_open = 0; T.dirty = 1; }
        return;
    }
    if (g_menu_open) {
        if (up) menu_move(-1);
        if (down) menu_move(1);
        if (left) { int k; for (k = 0; k < 8; k++) menu_move(-1); }
        if (right) { int k; for (k = 0; k < 8; k++) menu_move(1); }
        if (fire(V_CROSS, b & PSPEXP_BUTTON_CROSS, now, 0) && g_rows[g_menu_sel].item) menu_run(g_rows[g_menu_sel].item);
        else if (pressed & PSPEXP_BUTTON_CIRCLE) menu_back();
        if (g_select_tap) { while (g_menu_open) menu_back(); }
        return;
    }

    /* the wheel: R + nub, the nub alone when the keyboard is hidden, or in
     * scroll mode; faster the further it is pushed */
    if ((ctrl || !kb_shown || g_scroll_mode) && !T.on_alt && (ay < -30 || ay > 30)) {
        static uint64_t last; static float acc;
        float dt = last && now - last < 200000u ? (now - last) / 1e6f : 0.016f;
        float v = (float)((ay < 0 ? -ay : ay) - 30) / 97.0f;   /* 0..1 */
        acc += (ay < 0 ? 1.0f : -1.0f) * v * v * 120.0f * dt;   /* up to 120 lines a second */
        last = now;
        if (acc >= 1.0f || acc <= -1.0f) {
            T.view += (int)acc;
            acc -= (float)(int)acc;
            if (T.view > T.sb_count) T.view = T.sb_count;
            if (T.view < 0) T.view = 0;
            T.dirty = 1;
        }
        ax = ay = 0;   /* not arrows too */
    }
    if ((ctrl || !kb_shown || g_scroll_mode) && !T.on_alt) ax = ay = 0;   /* the nub scrolls here: never arrows */

    /* scroll mode (L + Select, or the menu): the D-pad scrolls with nothing
     * held; circle, or any typing, leaves */
    if (shift && g_select_tap) { g_scroll_mode = !T.on_alt; g_select_tap = 0; T.dirty = 1; }
    if (g_scroll_mode) {
        int page = term_rows() - 1;
        if (up) T.view++;
        if (down) T.view--;
        if (left) T.view += page;
        if (right) T.view -= page;
        if (T.view > T.sb_count) T.view = T.sb_count;
        if (T.view < 0) T.view = 0;
        if (up || down || left || right) T.dirty = 1;
        if (pressed & ~(PSPEXP_BUTTON_UP | PSPEXP_BUTTON_DOWN | PSPEXP_BUTTON_LEFT | PSPEXP_BUTTON_RIGHT | PSPEXP_BUTTON_LTRIGGER | PSPEXP_BUTTON_RTRIGGER)) {
            g_scroll_mode = 0; T.dirty = 1;
            if (pressed & PSPEXP_BUTTON_CIRCLE) { T.view = 0; fire(V_CIRCLE, 1, now, 1); return; }   /* circle only leaves */
        } else return;
    }

    if (g_select_tap) { kb_shown = !kb_shown; term_resize(term_rows()); }

    if (ctrl && (up || down || left || right)) {   /* R + direction: scrollback */
        int page = term_rows() - 1;
        if (up) T.view++;
        if (down) T.view--;
        if (left) T.view += page;
        if (right) T.view -= page;
        if (T.view > T.sb_count) T.view = T.sb_count;
        if (T.view < 0 || T.on_alt) T.view = 0;
        T.dirty = 1;
    } else if (kb_shown) {
        int px = key_x(kb_row, kb_col) + KROWS_[kb_row][kb_col].w * 8;
        if (up) { kb_row = (kb_row + KROWS - 1) % KROWS; kb_col = key_at_x(kb_row, px); T.dirty = 1; }
        if (down) { kb_row = (kb_row + 1) % KROWS; kb_col = key_at_x(kb_row, px); T.dirty = 1; }
        if (left) { kb_col = (kb_col + KCOUNT[kb_row] - 1) % KCOUNT[kb_row]; T.dirty = 1; }
        if (right) { kb_col = (kb_col + 1) % KCOUNT[kb_row]; T.dirty = 1; }
    } else {
        if (up) send_arrow('A');
        if (down) send_arrow('B');
        if (right) send_arrow('C');
        if (left) send_arrow('D');
    }
    /* the nub: arrow keys (with the keyboard shown) */
    if (fire(N_UP, ay < -60, now, 1)) send_arrow('A');
    if (fire(N_DOWN, ay > 60, now, 1)) send_arrow('B');
    if (fire(N_LEFT, ax < -60, now, 1)) send_arrow('D');
    if (fire(N_RIGHT, ax > 60, now, 1)) send_arrow('C');

    if (fire(V_CROSS, b & PSPEXP_BUTTON_CROSS, now, 1) && kb_shown) { send_key(&KROWS_[kb_row][kb_col], shift, ctrl); T.view = 0; T.dirty = 1; }
    if (ctrl && (pressed & PSPEXP_BUTTON_CIRCLE)) { pty_puts("\003"); T.view = 0; fire(V_CIRCLE, 1, now, 1); }   /* R + circle: Ctrl-C */
    else if (!ctrl && fire(V_CIRCLE, b & PSPEXP_BUTTON_CIRCLE, now, 1)) { pty_puts("\177"); T.view = 0; }
    if (pressed & PSPEXP_BUTTON_SQUARE) { pty_puts(" "); T.view = 0; }
    if (pressed & PSPEXP_BUTTON_TRIANGLE) { pty_puts(shift ? "\033" : "\r"); T.view = 0; }   /* L + triangle: Esc */
    if (pressed & PSPEXP_BUTTON_START) pty_puts("\t");
}

/* ---- mirror view: the host framebuffer and a virtual keypad (Linux) ------------------ */

#ifdef __linux__
#include <linux/fb.h>
#include <linux/uinput.h>
#include <sys/mman.h>

static const char *g_fb_path;
static int g_fb = -1, g_ui = -1;
static uint8_t *g_fb_mem;
static size_t g_fb_len;
static struct fb_fix_screeninfo g_fbf;
static uint8_t *g_fb_prev;          /* last frame sent, for change detection */
static size_t g_fb_prev_len;
static int g_mirror_force;
static uint16_t g_mirror_tex[8];
static int g_mirror_npages;
static uint64_t g_mirror_last;
/* tiled mode (screens up to 480x272, sent at native size): 64x32 tiles kept
 * on the PSP as separate textures; only the tiles that changed are re-sent */
#define MT_W 64
#define MT_H 32
#define MT_MAX ((480 / MT_W + 1) * (272 / MT_H + 1))
static uint16_t g_tile_id[MT_MAX];   /* 0: not uploaded */
static int g_tile_gen[MT_MAX];
static int g_tile_cols, g_tile_rows, g_tile_w, g_tile_h;
static unsigned g_tiles_sent, g_tiles_frames;

static int mirror_available(void) { return g_fb_path != NULL; }
/* redraw everything (view switch): the PSP still holds the tiles, their ids stay known */
static void mirror_invalidate(void) { g_mirror_force = 1; }
/* after a (re)connect the PSP holds none of them: forget the ids, or new
 * uploads would reuse names the old session left behind -> DUPLICATE_ID */
static void mirror_reset(void)
{
    int i;
    g_mirror_force = 1;
    for (i = 0; i < MT_MAX; i++) g_tile_id[i] = 0;
}

static int mirror_open(void)
{
    if (g_fb >= 0) return 0;
    if (!g_fb_path) return -1;
    g_fb = open(g_fb_path, O_RDONLY);
    if (g_fb < 0) return -1;
    if (ioctl(g_fb, FBIOGET_FSCREENINFO, &g_fbf) < 0) { close(g_fb); g_fb = -1; return -1; }
    g_fb_len = g_fbf.smem_len;
    g_fb_mem = (uint8_t *)mmap(NULL, g_fb_len, PROT_READ, MAP_SHARED, g_fb, 0);
    if (g_fb_mem == MAP_FAILED) { g_fb_mem = NULL; close(g_fb); g_fb = -1; return -1; }
    return 0;
}

/* 16 or 32 bpp framebuffer pixel -> 5650 (R in the low bits, as the PSP wants) */
static inline uint16_t fb_pixel(const uint8_t *row, int x, const struct fb_var_screeninfo *v)
{
    uint32_t p, r, g, b;
    if (v->bits_per_pixel == 16) p = ((const uint16_t *)row)[x];
    else p = ((const uint32_t *)row)[x];
    r = (p >> v->red.offset) & ((1u << v->red.length) - 1);
    g = (p >> v->green.offset) & ((1u << v->green.length) - 1);
    b = (p >> v->blue.offset) & ((1u << v->blue.length) - 1);
    r = v->red.length >= 5 ? r >> (v->red.length - 5) : r << (5 - v->red.length);
    g = v->green.length >= 6 ? g >> (v->green.length - 6) : g << (6 - v->green.length);
    b = v->blue.length >= 5 ? b >> (v->blue.length - 5) : b << (5 - v->blue.length);
    return (uint16_t)(r | (g << 5) | (b << 11));
}

/* Tiled mirror: compare each 64x32 tile with the last frame sent, upload the
 * changed ones (alternating texture ids, the old one released), and draw all
 * tiles from what the PSP holds. A launcher cursor move costs a few tiles,
 * not the whole 150 KB screen: the difference that matters over Wi-Fi. */
static int mirror_frame_tiled(const struct fb_var_screeninfo *v, const uint8_t *base, size_t fl)
{
    int w = (int)v->xres, h = (int)v->yres, tx, ty, i, changed = 0, bpp = (int)v->bits_per_pixel / 8;
    float s = 480.0f / w < 272.0f / h ? 480.0f / w : 272.0f / h;
    int qw = (int)(w * s), qh = (int)(h * s), ox = (480 - qw) / 2, oy = (272 - qh) / 2;
    int cols = (w + MT_W - 1) / MT_W, rows = (h + MT_H - 1) / MT_H;
    uint8_t dirty[MT_MAX];
    size_t line = g_fbf.line_length;

    if (cols * rows > MT_MAX) return 0;
    if (cols != g_tile_cols || rows != g_tile_rows || w != g_tile_w || h != g_tile_h || g_fb_prev_len != fl) {
        for (i = 0; i < MT_MAX; i++) { if (g_tile_id[i]) pspexp_release(ctx, g_tile_id[i]); g_tile_id[i] = 0; }
        free(g_fb_prev); g_fb_prev = (uint8_t *)malloc(fl); g_fb_prev_len = g_fb_prev ? fl : 0;
        g_tile_cols = cols; g_tile_rows = rows; g_tile_w = w; g_tile_h = h;
        g_mirror_force = 1;
    }
    if (!g_fb_prev) return 0;
    /* which tiles changed */
    for (ty = 0; ty < rows; ty++)
        for (tx = 0; tx < cols; tx++) {
            int k = ty * cols + tx, y, y1 = (ty + 1) * MT_H < h ? (ty + 1) * MT_H : h;
            size_t xoff = (size_t)tx * MT_W * bpp, xlen = (size_t)((tx + 1) * MT_W < w ? MT_W : w - tx * MT_W) * bpp;
            dirty[k] = g_mirror_force || !g_tile_id[k];
            for (y = ty * MT_H; !dirty[k] && y < y1; y++)
                if (memcmp(g_fb_prev + (size_t)y * line + xoff, base + (size_t)y * line + xoff, xlen)) dirty[k] = 1;
            changed |= dirty[k];
        }
    if (!changed) return 0;
    g_mirror_force = 0;

    pspexp_begin_frame(ctx);
    pspexp_set_clip(ctx, 0, 0, 480, 272);
    pspexp_fill_rect(ctx, 0, 0, 480, 272, 0, 255);
    for (ty = 0; ty < rows; ty++)
        for (tx = 0; tx < cols; tx++) {
            int k = ty * cols + tx, x0 = tx * MT_W, y0 = ty * MT_H;
            int pw = w - x0 < MT_W ? w - x0 : MT_W, ph = h - y0 < MT_H ? h - y0 : MT_H;
            PspExpQuadExRec q;
            if (dirty[k]) {
                int tw = 16, th = 8, y, rowblocks;
                uint16_t id = (uint16_t)(1000 + k * 2 + (g_tile_gen[k] & 1)), *dst;
                PspExpTextureDesc d;
                PspExpFrame f;
                while (tw < pw) tw <<= 1;
                while (th < ph) th <<= 1;
                memset(&d, 0, sizeof(d));
                d.width = (uint16_t)tw; d.height = (uint16_t)th; d.format = PSPEXP_TEX_5650;
                d.layout = PSPEXP_LAYOUT_SWIZZLED; d.priority = PSPEXP_PRIORITY_NORMAL;
                f.u0 = 0; f.v0 = 0; f.w = (uint16_t)(tw < 64 ? tw : 64); f.h = (uint16_t)th;
                dst = (uint16_t *)pspexp_upload_texture_begin(ctx, id, &d, (uint32_t)(tw * th * 2), &f, 1);
                if (!dst) continue;
                rowblocks = tw >> 3;
                for (y = 0; y < th; y++) {
                    uint16_t *band = dst + (size_t)(y >> 3) * (size_t)rowblocks * 64u + (size_t)((y & 7) << 3);
                    int x = 0;
                    if (y < ph) {
                        const uint8_t *row = base + (size_t)(y0 + y) * line;
                        for (; x < pw; x++) band[((x >> 3) << 6) + (x & 7)] = fb_pixel(row, x0 + x, v);
                        memcpy(g_fb_prev + (size_t)(y0 + y) * line + (size_t)x0 * bpp, row + (size_t)x0 * bpp, (size_t)pw * bpp);
                    }
                    for (; x < tw; x++) band[((x >> 3) << 6) + (x & 7)] = 0;
                }
                if (pspexp_upload_texture_commit(ctx, 0) < 0) continue;
                if (g_tile_id[k]) pspexp_release(ctx, g_tile_id[k]);
                g_tile_id[k] = id;
                g_tile_gen[k]++;
                g_tiles_sent++;
            }
            if (!g_tile_id[k]) continue;
            memset(&q, 0, sizeof(q));
            q.x = (int16_t)(ox + x0 * qw / w); q.y = (int16_t)(oy + y0 * qh / h);
            q.w = (int16_t)(ox + (x0 + pw) * qw / w - q.x); q.h = (int16_t)(oy + (y0 + ph) * qh / h - q.y);
            q.u0 = 0; q.v0 = 0; q.u1 = (uint16_t)pw; q.v1 = (uint16_t)ph; q.color = 0xFFFF;
            pspexp_draw_quad_ex_batch(ctx, g_tile_id[k], &q, 1, PSPEXP_PALETTE_DEFAULT, PSPEXP_VTX_SPRITE_2D_5650);
        }
    draw_pointer();
    pspexp_present(ctx);
    g_mirror_last++;
    g_tiles_frames++;
    return 1;
}

/* One frame: if the screen changed, upload it (downscaled when larger than the
 * PSP, else native and scaled by the GE) as swizzled 5650 pages and draw it
 * letterboxed. Returns 1 when a frame was sent. */
static int mirror_frame(void)
{
    struct fb_var_screeninfo v;
    int w, h, dw, dh, x0, y0, i, ox, oy, qw, qh;
    const uint8_t *base;
    size_t fl;
    float s;
    if (mirror_open() < 0) return 0;
    if (ioctl(g_fb, FBIOGET_VSCREENINFO, &v) < 0) return 0;
    if (v.bits_per_pixel != 16 && v.bits_per_pixel != 32) return 0;
    w = (int)v.xres; h = (int)v.yres;
    base = g_fb_mem + (size_t)v.yoffset * g_fbf.line_length + (size_t)v.xoffset * (v.bits_per_pixel / 8);
    fl = (size_t)h * g_fbf.line_length;
    if (base + fl > g_fb_mem + g_fb_len) return 0;
    if (w <= 480 && h <= 272) return mirror_frame_tiled(&v, base, fl);
    if (!g_mirror_force && g_fb_prev && g_fb_prev_len == fl && memcmp(g_fb_prev, base, fl) == 0) return 0;
    if (g_fb_prev_len != fl) { free(g_fb_prev); g_fb_prev = (uint8_t *)malloc(fl); g_fb_prev_len = g_fb_prev ? fl : 0; }
    if (g_fb_prev) memcpy(g_fb_prev, base, fl);
    g_mirror_force = 0;

    s = 480.0f / w < 272.0f / h ? 480.0f / w : 272.0f / h;
    if (s < 1.0f) { dw = (int)(w * s); dh = (int)(h * s); } else { dw = w; dh = h; }
    qw = (int)(w * s); qh = (int)(h * s);
    ox = (480 - qw) / 2; oy = (272 - qh) / 2;

    for (i = 0; i < g_mirror_npages; i++) pspexp_release(ctx, g_mirror_tex[i]);
    g_mirror_npages = 0;
    pspexp_begin_frame(ctx);
    pspexp_set_clip(ctx, 0, 0, 480, 272);
    pspexp_fill_rect(ctx, 0, 0, 480, 272, 0, 255);
    for (y0 = 0; y0 < dh; y0 += 256) {
        for (x0 = 0; x0 < dw; x0 += 256) {
            int pw = dw - x0 < 256 ? dw - x0 : 256, ph = dh - y0 < 256 ? dh - y0 : 256;
            int tw = 16, th = 8, y, rowblocks;
            PspExpTextureDesc d;
            PspExpFrame f;
            PspExpQuadExRec q;
            uint16_t *dst, id;
            while (tw < pw) tw <<= 1;
            while (th < ph) th <<= 1;
            memset(&d, 0, sizeof(d));
            d.width = (uint16_t)tw; d.height = (uint16_t)th; d.format = PSPEXP_TEX_5650;
            d.layout = PSPEXP_LAYOUT_SWIZZLED; d.priority = PSPEXP_PRIORITY_NORMAL;
            f.u0 = 0; f.v0 = 0; f.w = (uint16_t)(tw < 64 ? tw : 64); f.h = (uint16_t)th;
            id = (uint16_t)(100 + (g_mirror_last & 1) * 8 + g_mirror_npages);   /* alternate ids: old pages may still be drawn */
            dst = (uint16_t *)pspexp_upload_texture_begin(ctx, id, &d, (uint32_t)(tw * th * 2), &f, 1);
            if (!dst) continue;
            rowblocks = tw >> 3;
            for (y = 0; y < th; y++) {
                uint16_t *band = dst + (size_t)(y >> 3) * (size_t)rowblocks * 64u + (size_t)((y & 7) << 3);
                int x = 0;
                if (y < ph) {
                    int sy = dh == h ? y0 + y : (int)((int64_t)(y0 + y) * h / dh);
                    const uint8_t *row = base + (size_t)sy * g_fbf.line_length;
                    for (; x < pw; x++) {
                        int sx = dw == w ? x0 + x : (int)((int64_t)(x0 + x) * w / dw);
                        band[((x >> 3) << 6) + (x & 7)] = fb_pixel(row, sx, &v);
                    }
                }
                for (; x < tw; x++) band[((x >> 3) << 6) + (x & 7)] = 0;
            }
            if (pspexp_upload_texture_commit(ctx, 0) < 0) continue;
            g_mirror_tex[g_mirror_npages++] = id;
            memset(&q, 0, sizeof(q));
            q.x = (int16_t)(ox + x0 * qw / dw); q.y = (int16_t)(oy + y0 * qh / dh);
            q.w = (int16_t)(pw * qw / dw); q.h = (int16_t)(ph * qh / dh);
            q.u0 = 0; q.v0 = 0; q.u1 = (uint16_t)pw; q.v1 = (uint16_t)ph; q.color = 0xFFFF;
            pspexp_draw_quad_ex_batch(ctx, id, &q, 1, PSPEXP_PALETTE_DEFAULT, PSPEXP_VTX_SPRITE_2D_5650);
        }
    }
    draw_pointer();
    pspexp_present(ctx);
    g_mirror_last++;
    return 1;
}

/* the virtual keypad */
static const struct { uint32_t button; int key; } KEYPAD[] = {
    { PSPEXP_BUTTON_UP, KEY_UP }, { PSPEXP_BUTTON_DOWN, KEY_DOWN }, { PSPEXP_BUTTON_LEFT, KEY_LEFT }, { PSPEXP_BUTTON_RIGHT, KEY_RIGHT },
    { PSPEXP_BUTTON_CROSS, KEY_J }, { PSPEXP_BUTTON_CIRCLE, KEY_K }, { PSPEXP_BUTTON_SQUARE, KEY_U }, { PSPEXP_BUTTON_TRIANGLE, KEY_I },
    { PSPEXP_BUTTON_START, KEY_ENTER }, { PSPEXP_BUTTON_SELECT, KEY_SPACE },
    { PSPEXP_BUTTON_LTRIGGER, KEY_H }, { PSPEXP_BUTTON_RTRIGGER, KEY_L },
};
#define NKEYPAD (int)(sizeof(KEYPAD) / sizeof(KEYPAD[0]))
static int g_kp_down[NKEYPAD], g_kp_menu, g_kp_nub[4];

/* XTEST fallback (kernels without uinput, like the GameShell's 5.3.6): a
 * minimal X11 client, speaking the protocol directly so the static binary
 * needs no libraries. Keys go to the X server on :0 as XTEST fake input,
 * X keycode = Linux keycode + 8 (the evdev keymap). */
#include <sys/socket.h>
#include <sys/un.h>
static int g_x = -1, g_xtest_op;
static uint64_t g_x_retry;

static int read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        got += (size_t)r;
    }
    return 0;
}

/* MIT-MAGIC-COOKIE-1 for display 0 from $XAUTHORITY or ~/.Xauthority */
static int xauth_cookie(uint8_t *cookie, int max)
{
    char path[512];
    const char *xa = getenv("XAUTHORITY"), *home = getenv("HOME");
    FILE *f;
    int n = -1;
    if (xa) snprintf(path, sizeof(path), "%s", xa); else snprintf(path, sizeof(path), "%s/.Xauthority", home ? home : "/root");
    f = fopen(path, "rb");
    if (!f) return -1;
    for (;;) {
        uint8_t hdr[2], len[2], fields[4][256];
        int fl[4], i;
        if (fread(hdr, 1, 2, f) != 2) break;
        for (i = 0; i < 4; i++) {
            if (fread(len, 1, 2, f) != 2) { fclose(f); return n; }
            fl[i] = (len[0] << 8) | len[1];
            if (fl[i] > 255 || fread(fields[i], 1, (size_t)fl[i], f) != (size_t)fl[i]) { fclose(f); return n; }
        }
        /* fields: address, display number, name, data */
        if (fl[2] == 18 && !memcmp(fields[2], "MIT-MAGIC-COOKIE-1", 18) && fl[3] <= max &&
            (fl[1] == 0 || (fl[1] == 1 && fields[1][0] == '0'))) {
            memcpy(cookie, fields[3], (size_t)fl[3]); n = fl[3];
            break;
        }
    }
    fclose(f);
    return n;
}

static int xtest_open(void)
{
    struct sockaddr_un sa;
    uint8_t cookie[64], req[64], rep[32];
    int clen, fd, len;
    uint64_t now = pspexp_time_us();
    if (g_x >= 0) return 0;
    if (now - g_x_retry < 3000000u) return -1;
    g_x_retry = now;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, "/tmp/.X11-unix/X0");
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return -1; }
    clen = xauth_cookie(cookie, (int)sizeof(cookie));
    memset(req, 0, sizeof(req));
    req[0] = 'l'; req[2] = 11; req[4] = 0;               /* little-endian, protocol 11.0 */
    if (clen > 0) {
        req[6] = 18; req[8] = (uint8_t)clen;
        memcpy(req + 12, "MIT-MAGIC-COOKIE-1", 18);     /* 18 -> padded to 20 */
        memcpy(req + 32, cookie, (size_t)clen);
        len = 32 + ((clen + 3) & ~3);
    } else len = 12;
    if (write(fd, req, (size_t)len) != len || read_full(fd, rep, 8) < 0 || rep[0] != 1) {
        fprintf(stderr, "[pspexp-term] X connection refused (auth?)\n");
        close(fd); return -1;
    }
    {   /* skip the rest of the setup reply */
        size_t extra = (size_t)(rep[6] | (rep[7] << 8)) * 4;
        uint8_t *tmp = (uint8_t *)malloc(extra ? extra : 1);
        if (!tmp || read_full(fd, tmp, extra) < 0) { free(tmp); close(fd); return -1; }
        free(tmp);
    }
    /* QueryExtension "XTEST" */
    memset(req, 0, sizeof(req));
    req[0] = 98; req[2] = 4; req[4] = 5;                  /* length 4 units: 8 + "XTEST" padded to 8 */
    memcpy(req + 8, "XTEST", 5);
    if (write(fd, req, 16) != 16 || read_full(fd, rep, 32) < 0 || rep[0] != 1 || !rep[8]) {
        fprintf(stderr, "[pspexp-term] X server has no XTEST\n");
        close(fd); return -1;
    }
    g_xtest_op = rep[9];
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    g_x = fd;
    fprintf(stderr, "[pspexp-term] keypad through X (XTEST)\n");
    return 0;
}

static void xtest_key(int linux_code, int down)
{
    uint8_t req[36], junk[256];
    if (xtest_open() < 0) return;
    while (read(g_x, junk, sizeof(junk)) > 0) {}          /* discard events / errors */
    memset(req, 0, sizeof(req));
    req[0] = (uint8_t)g_xtest_op; req[1] = 2;             /* FakeInput */
    req[2] = 9;                                           /* 36 bytes */
    req[4] = down ? 2 : 3;                                /* KeyPress / KeyRelease */
    req[5] = (uint8_t)(linux_code + 8);
    if (write(g_x, req, sizeof(req)) != (ssize_t)sizeof(req)) { close(g_x); g_x = -1; }
}

static void ui_emit(int type, int code, int value)
{
    struct input_event e;
    if (g_ui < 0) { if (type == EV_KEY) xtest_key(code, value); return; }
    memset(&e, 0, sizeof(e));
    e.type = (uint16_t)type; e.code = (uint16_t)code; e.value = value;
    if (write(g_ui, &e, sizeof(e)) < 0) { /* keypad gone */ }
}

static int keypad_open(void)
{
    struct uinput_setup us;
    int i;
    static int uinput_failed;
    if (g_ui >= 0) return 0;
    if (uinput_failed) return xtest_open() == 0 ? 0 : -1;
    g_ui = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (g_ui < 0) {
        fprintf(stderr, "[pspexp-term] /dev/uinput: %s; trying the X server\n", strerror(errno));
        uinput_failed = 1;
        return xtest_open() == 0 ? 0 : -1;
    }
    ioctl(g_ui, UI_SET_EVBIT, EV_KEY);
    for (i = 0; i < NKEYPAD; i++) ioctl(g_ui, UI_SET_KEYBIT, KEYPAD[i].key);
    ioctl(g_ui, UI_SET_KEYBIT, KEY_ESC);
    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_VIRTUAL; us.id.vendor = 0x054c; us.id.product = 0x01ca;
    strcpy(us.name, "PSPExp keypad");
    if (ioctl(g_ui, UI_DEV_SETUP, &us) < 0 || ioctl(g_ui, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "[pspexp-term] uinput setup failed: %s\n", strerror(errno));
        close(g_ui); g_ui = -1; return -1;
    }
    fprintf(stderr, "[pspexp-term] virtual keypad created\n");
    return 0;
}

static void keypad_release_all(void)
{
    int i;
    for (i = 0; i < NKEYPAD; i++) if (g_kp_down[i]) { ui_emit(EV_KEY, KEYPAD[i].key, 0); g_kp_down[i] = 0; }
    for (i = 0; i < 4; i++) if (g_kp_nub[i]) { ui_emit(EV_KEY, i == 0 ? KEY_UP : i == 1 ? KEY_DOWN : i == 2 ? KEY_LEFT : KEY_RIGHT, 0); g_kp_nub[i] = 0; }
    if (g_kp_menu) { ui_emit(EV_KEY, KEY_ESC, 0); g_kp_menu = 0; }
    ui_emit(EV_SYN, SYN_REPORT, 0);
}

static void tap_key(int key)
{
    ui_emit(EV_KEY, key, 1); ui_emit(EV_SYN, SYN_REPORT, 0);
    usleep(30000);
    ui_emit(EV_KEY, key, 0); ui_emit(EV_SYN, SYN_REPORT, 0);
}

static void mirror_input(uint32_t b, uint32_t pressed, int ax, int ay)
{
    static uint64_t start_down; static int start_held, start_used;
    uint64_t now = pspexp_time_us();
    int i, nub[4];
    (void)pressed;
    if (keypad_open() < 0) return;
    /* start: a long hold is Menu (Esc), held while start stays down; a tap is Enter */
    if ((b & PSPEXP_BUTTON_START) && !start_held) { start_held = 1; start_down = now; start_used = 0; }
    if ((b & PSPEXP_BUTTON_START) && !start_used && now - start_down >= 700000u) { ui_emit(EV_KEY, KEY_ESC, 1); g_kp_menu = 1; start_used = 1; }
    if (!(b & PSPEXP_BUTTON_START) && start_held) {
        start_held = 0;
        if (g_kp_menu) { ui_emit(EV_KEY, KEY_ESC, 0); g_kp_menu = 0; }
        else if (!start_used) tap_key(KEY_ENTER);
    }
    if (g_select_tap) tap_key(KEY_SPACE);
    for (i = 0; i < NKEYPAD; i++) {
        int down = (b & KEYPAD[i].button) != 0;
        if (KEYPAD[i].button == PSPEXP_BUTTON_START || KEYPAD[i].button == PSPEXP_BUTTON_SELECT) continue;   /* above */
        if (down != g_kp_down[i]) { ui_emit(EV_KEY, KEYPAD[i].key, down); g_kp_down[i] = down; }
    }
    nub[0] = ay < -60; nub[1] = ay > 60; nub[2] = ax < -60; nub[3] = ax > 60;
    for (i = 0; i < 4; i++)
        if (nub[i] != g_kp_nub[i]) { ui_emit(EV_KEY, i == 0 ? KEY_UP : i == 1 ? KEY_DOWN : i == 2 ? KEY_LEFT : KEY_RIGHT, nub[i]); g_kp_nub[i] = nub[i]; }
    ui_emit(EV_SYN, SYN_REPORT, 0);
}
#else
static int mirror_available(void) { return 0; }
static void mirror_invalidate(void) {}
static void mirror_reset(void) {}
static void mirror_input(uint32_t b, uint32_t pressed, int ax, int ay) { (void)b; (void)pressed; (void)ax; (void)ay; }
static int mirror_frame(void) { return 0; }
static void keypad_release_all(void) {}
#endif

/* ---- host keyboard and mouse (Linux evdev) ------------------------------------------ */

#ifdef __linux__
#include <dirent.h>
#include <linux/input.h>

typedef struct InDev { int fd, kbd, mouse, grabbed, num; } InDev;
static InDev g_in[16];
static int g_nin, g_host_input = 1, g_grab = 1;
static uint64_t g_in_scan;
static int k_shift, k_ctrl, k_alt, k_caps;
static int m_x = 240, m_y = 136, m_buttons;   /* pointer in PSP pixels */
static uint64_t m_shown_until;

#define BIT(arr, n) ((arr[(n) / (8 * sizeof(long))] >> ((n) % (8 * sizeof(long)))) & 1)

static void host_input_scan(void)
{
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) && g_nin < 16) {
        char path[300];
        unsigned long ev[1] = { 0 }, keys[KEY_MAX / (8 * sizeof(long)) + 1], rel[REL_MAX / (8 * sizeof(long)) + 1];
        int num, fd, i, kbd, mouse;
        if (sscanf(e->d_name, "event%d", &num) != 1) continue;
        for (i = 0; i < g_nin; i++) if (g_in[i].num == num) break;
        if (i < g_nin) continue;
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        memset(keys, 0, sizeof(keys)); memset(rel, 0, sizeof(rel));
        ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
        kbd = BIT(ev, EV_KEY) && BIT(keys, KEY_A) && BIT(keys, KEY_Z) && BIT(keys, KEY_ENTER);
        mouse = BIT(ev, EV_REL) && BIT(rel, REL_X) && BIT(keys, BTN_LEFT);
        if (!kbd && !mouse) { close(fd); continue; }
        g_in[g_nin].fd = fd; g_in[g_nin].kbd = kbd; g_in[g_nin].mouse = mouse; g_in[g_nin].grabbed = 0; g_in[g_nin].num = num;
        g_nin++;
        {
            char name[80] = "";
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            fprintf(stderr, "[pspexp-term] host %s: %s (%s)\n", kbd ? "keyboard" : "mouse", name, path);
        }
    }
    closedir(d);
}

static void host_input_grab(int want)
{
    int i;
    for (i = 0; i < g_nin; i++) {
        if (g_in[i].grabbed == want) continue;
        if (ioctl(g_in[i].fd, EVIOCGRAB, want ? 1 : 0) == 0) g_in[i].grabbed = want;
        if (!want) k_shift = k_ctrl = k_alt = 0;
    }
}

/* US layout: [keycode] = { normal, shifted } */
static const char *const KMAP[KEY_MAX + 1][2] = {
    [KEY_1] = {"1","!"}, [KEY_2] = {"2","@"}, [KEY_3] = {"3","#"}, [KEY_4] = {"4","$"}, [KEY_5] = {"5","%"},
    [KEY_6] = {"6","^"}, [KEY_7] = {"7","&"}, [KEY_8] = {"8","*"}, [KEY_9] = {"9","("}, [KEY_0] = {"0",")"},
    [KEY_MINUS] = {"-","_"}, [KEY_EQUAL] = {"=","+"}, [KEY_LEFTBRACE] = {"[","{"}, [KEY_RIGHTBRACE] = {"]","}"},
    [KEY_SEMICOLON] = {";",":"}, [KEY_APOSTROPHE] = {"'","\""}, [KEY_GRAVE] = {"`","~"}, [KEY_BACKSLASH] = {"\\","|"},
    [KEY_COMMA] = {",","<"}, [KEY_DOT] = {".",">"}, [KEY_SLASH] = {"/","?"}, [KEY_SPACE] = {" "," "}, [KEY_102ND] = {"<",">"},
    [KEY_Q] = {"q","Q"}, [KEY_W] = {"w","W"}, [KEY_E] = {"e","E"}, [KEY_R] = {"r","R"}, [KEY_T] = {"t","T"},
    [KEY_Y] = {"y","Y"}, [KEY_U] = {"u","U"}, [KEY_I] = {"i","I"}, [KEY_O] = {"o","O"}, [KEY_P] = {"p","P"},
    [KEY_A] = {"a","A"}, [KEY_S] = {"s","S"}, [KEY_D] = {"d","D"}, [KEY_F] = {"f","F"}, [KEY_G] = {"g","G"},
    [KEY_H] = {"h","H"}, [KEY_J] = {"j","J"}, [KEY_K] = {"k","K"}, [KEY_L] = {"l","L"},
    [KEY_Z] = {"z","Z"}, [KEY_X] = {"x","X"}, [KEY_C] = {"c","C"}, [KEY_V] = {"v","V"}, [KEY_B] = {"b","B"},
    [KEY_N] = {"n","N"}, [KEY_M] = {"m","M"},
    [KEY_KP0] = {"0","0"}, [KEY_KP1] = {"1","1"}, [KEY_KP2] = {"2","2"}, [KEY_KP3] = {"3","3"}, [KEY_KP4] = {"4","4"},
    [KEY_KP5] = {"5","5"}, [KEY_KP6] = {"6","6"}, [KEY_KP7] = {"7","7"}, [KEY_KP8] = {"8","8"}, [KEY_KP9] = {"9","9"},
    [KEY_KPDOT] = {".","."}, [KEY_KPPLUS] = {"+","+"}, [KEY_KPMINUS] = {"-","-"}, [KEY_KPASTERISK] = {"*","*"},
    [KEY_KPSLASH] = {"/","/"},
    [KEY_ENTER] = {"\r","\r"}, [KEY_KPENTER] = {"\r","\r"}, [KEY_TAB] = {"\t","\033[Z"}, [KEY_BACKSPACE] = {"\177","\177"},
    [KEY_ESC] = {"\033","\033"},
    [KEY_HOME] = {"\033[H","\033[H"}, [KEY_END] = {"\033[F","\033[F"}, [KEY_INSERT] = {"\033[2~","\033[2~"},
    [KEY_DELETE] = {"\033[3~","\033[3~"}, [KEY_PAGEUP] = {"\033[5~","\033[5~"}, [KEY_PAGEDOWN] = {"\033[6~","\033[6~"},
    [KEY_F1] = {"\033OP","\033OP"}, [KEY_F2] = {"\033OQ","\033OQ"}, [KEY_F3] = {"\033OR","\033OR"}, [KEY_F4] = {"\033OS","\033OS"},
    [KEY_F5] = {"\033[15~","\033[15~"}, [KEY_F6] = {"\033[17~","\033[17~"}, [KEY_F7] = {"\033[18~","\033[18~"},
    [KEY_F8] = {"\033[19~","\033[19~"}, [KEY_F9] = {"\033[20~","\033[20~"}, [KEY_F10] = {"\033[21~","\033[21~"},
    [KEY_F11] = {"\033[23~","\033[23~"}, [KEY_F12] = {"\033[24~","\033[24~"},
};

static void host_key(int code, int value)
{
    const char *s;
    char c;
    switch (code) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: k_shift = value != 0; return;
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: k_ctrl = value != 0; return;
    case KEY_LEFTALT: case KEY_RIGHTALT: k_alt = value != 0; return;
    case KEY_CAPSLOCK: if (value == 1) k_caps = !k_caps; return;
    default: break;
    }
    if (value == 0) return;   /* 1 press, 2 auto-repeat */
    if (kb_shown) { kb_shown = 0; term_resize(term_rows()); }   /* a real keyboard: no need for the on-screen one */
    if (k_shift && (code == KEY_PAGEUP || code == KEY_PAGEDOWN)) {   /* Shift+PgUp/PgDn: scrollback */
        int page = T.rows - 1;
        T.view += code == KEY_PAGEUP ? page : -page;
        if (T.view > T.sb_count) T.view = T.sb_count;
        if (T.view < 0 || T.on_alt) T.view = 0;
        T.dirty = 1;
        return;
    }
    T.view = 0;
    if (code == KEY_UP || code == KEY_DOWN || code == KEY_LEFT || code == KEY_RIGHT) {
        if (k_alt) pty_puts("\033");
        send_arrow(code == KEY_UP ? 'A' : code == KEY_DOWN ? 'B' : code == KEY_RIGHT ? 'C' : 'D');
        return;
    }
    if (code > KEY_MAX || !KMAP[code][0]) return;
    {
        int shifted = k_shift;
        if (k_caps && code >= KEY_Q && code <= KEY_M && KMAP[code][0][0] >= 'a' && KMAP[code][0][0] <= 'z' && !KMAP[code][0][1]) shifted = !shifted;
        s = KMAP[code][shifted ? 1 : 0];
    }
    if (k_ctrl && s[0] && !s[1]) {
        c = s[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
        else if (c == '[' || c == '{') c = 27;
        else if (c == '\\' || c == '|') c = 28;
        else if (c == ']' || c == '}') c = 29;
        else if (c == '6' || c == '^') c = 30;
        else if (c == '-' || c == '_') c = 31;
        else if (c == ' ' || c == '2' || c == '@') c = 0;
        else if (c == '/' || c == '?') c = 31;
        if (k_alt) pty_puts("\033");
        pty_write(&c, 1);
        return;
    }
    if (k_alt && s[0] != '\033') pty_puts("\033");
    pty_puts(s);
}

static void mouse_report(int button, int press, int motion)
{
    char buf[40];
    int cx = m_x / g_cw + 1, cy = m_y / g_ch + 1;
    if (!T.mouse_mode || T.view) return;
    if (cy > T.rows) return;
    if (motion) button += 32;
    if (T.mouse_sgr) snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c", button, cx, cy, press ? 'M' : 'm');
    else {
        int b = press ? button : 3;
        if (cx > 223) cx = 223;
        if (cy > 223) cy = 223;
        snprintf(buf, sizeof(buf), "\033[M%c%c%c", 32 + b, 32 + cx, 32 + cy);
    }
    pty_puts(buf);
}

static void host_mouse(const struct input_event *e)
{
    if (e->type == EV_REL) {
        if (e->code == REL_X) { m_x += e->value; if (m_x < 0) m_x = 0; if (m_x > 479) m_x = 479; }
        else if (e->code == REL_Y) { m_y += e->value; if (m_y < 0) m_y = 0; if (m_y > 271) m_y = 271; }
        else if (e->code == REL_WHEEL) {
            if (T.mouse_mode && !T.view) { mouse_report(e->value > 0 ? 64 : 65, 1, 0); return; }
            T.view += e->value * 3;
            if (T.view > T.sb_count) T.view = T.sb_count;
            if (T.view < 0 || T.on_alt) T.view = 0;
        }
        if ((e->code == REL_X || e->code == REL_Y) && (T.mouse_mode == 1003 || (T.mouse_mode == 1002 && m_buttons)))
            mouse_report(m_buttons & 1 ? 0 : m_buttons & 2 ? 2 : m_buttons & 4 ? 1 : 3, 1, 1);
        m_shown_until = pspexp_time_us() + 3000000u;
        T.dirty = 1;
    } else if (e->type == EV_KEY && (e->code == BTN_LEFT || e->code == BTN_RIGHT || e->code == BTN_MIDDLE)) {
        int b = e->code == BTN_LEFT ? 0 : e->code == BTN_MIDDLE ? 1 : 2;
        int bit = e->code == BTN_LEFT ? 1 : e->code == BTN_MIDDLE ? 4 : 2;
        if (e->value) m_buttons |= bit; else m_buttons &= ~bit;
        mouse_report(b, e->value != 0, 0);
        m_shown_until = pspexp_time_us() + 3000000u;
        T.dirty = 1;
    }
}

static void host_input_poll(int connected)
{
    int i;
    uint64_t now = pspexp_time_us();
    if (!g_host_input) return;
    if (now - g_in_scan > 3000000u) { g_in_scan = now; host_input_scan(); }
    if (g_grab) host_input_grab(connected && !g_released && !g_view_mirror);   /* mirror view: keys go to the host's own UI */
    for (i = 0; i < g_nin; i++) {
        struct input_event ev[32];
        ssize_t n;
        while ((n = read(g_in[i].fd, ev, sizeof(ev))) > 0) {
            int k, cnt = (int)(n / (ssize_t)sizeof(ev[0]));
            if (!connected || g_released || g_view_mirror) continue;   /* not ours to use: drain */
            for (k = 0; k < cnt; k++) {
                if (g_in[i].kbd && ev[k].type == EV_KEY && ev[k].code < BTN_MISC) host_key(ev[k].code, ev[k].value);
                else if (g_in[i].mouse) host_mouse(&ev[k]);
            }
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) {   /* unplugged */
            close(g_in[i].fd);
            g_in[i] = g_in[--g_nin];
            i--;
        }
    }
}

static void draw_pointer(void)
{
    if (pspexp_time_us() > m_shown_until) return;
    pspexp_fill_rect(ctx, (int16_t)m_x, (int16_t)m_y, 2, 8, rgb565(0xffffff), 230);
    pspexp_fill_rect(ctx, (int16_t)m_x, (int16_t)m_y, 6, 2, rgb565(0xffffff), 230);
}
#else
static int g_host_input, g_grab;
static void host_input_poll(int connected) { (void)connected; }
static void draw_pointer(void) {}
#endif

/* ---- programs started from the launcher ------------------------------------------- */

static void release_psp(void);
static void launch_child(const char *cmd, const char *cwd, int aware)
{
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        /* the programs' output, kept small: it lives in RAM (/tmp), and a
         * log grown to megabytes made anything reading it (a watcher on the
         * Omega2+: seconds of CPU a pass) slow the game down */
        struct stat lst;
        int trunc = stat("/tmp/pspexp-launcher.log", &lst) == 0 && lst.st_size > 256 * 1024;
        int fd = open("/tmp/pspexp-launcher.log", O_WRONLY | O_CREAT | (trunc ? O_TRUNC : O_APPEND), 0644), i;
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        /* nothing of the console's reaches the program: its PSP link (a
         * copy would keep the connection open after the console lets go),
         * the terminal, the audio FIFO, input devices */
        for (i = 3; i < 1024; i++) close(i);
        if (chdir(cwd) != 0) { /* run anyway */ }
        if (!getenv("DISPLAY")) setenv("DISPLAY", ":0", 1);
        /* the program reaches the PSP the way the console just did (the SDL
         * layer's default is USB): Wi-Fi when that was the link */
        if (g_link_tcp) {
            char v[300];
            snprintf(v, sizeof(v), "tcp:%s:%u", g_tcp.host ? g_tcp.host : "127.0.0.1", g_tcp.port ? g_tcp.port : 3838);
            setenv("PSPEXP", v, 1);
        } else if (g_want_usb) setenv("PSPEXP", "usb", 1);
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    g_launch_pid = pid;
    g_launch_aware = aware;
    /* a PSPExp program needs the PSP to itself: let go of it now, not only
     * when a wrapper (pspexp-exec, pspexp-run) asks with SIGUSR1: a player or
     * game started without one would otherwise wait for a PSP we hold */
    if (aware && pid > 0) release_psp();
    fprintf(stderr, "[pspexp-term] launcher: started pid %d (%s): %s\n", (int)pid, aware ? "PSPExp-aware" : "mirrored", cmd);
    if (!aware && mirror_available()) { g_view_launcher = 0; g_view_mirror = 1; mirror_invalidate(); }
}

static void launch_done(int status)
{
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    fprintf(stderr, "[pspexp-term] launcher: pid %d ended (%d)\n", (int)g_launch_pid, code);
    g_launch_pid = 0;
    if (g_launch_aware) g_acquire_req = 1;   /* ours again (pspexp-exec's SIGUSR2 may have said so already) */
    if (g_view_mirror) { keypad_release_all(); g_view_mirror = 0; }
    g_view_launcher = 1;
    launcher_child_exited(code);
}

/* ---- main ------------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr, "usage: pspexp-term [--usb | --tcp host[:port] | --null | --sdl] [--shell PATH] [--cmd \"command\"]\n"
                    "                   [--audio-fifo PATH | --no-audio-fifo] [--pidfile PATH] [--shot N]\n"
                    "                   [--no-host-input] [--no-grab] [--mirror [/dev/fbN]]\n"
                    "                   [--launcher] [--assets DIR]\n");
}

int main(int argc, char **argv)
{
    const char *fifo_path = "/tmp/pspexp-audio", *pidfile = "/tmp/pspexp-term.pid";
    static char tcp_host[256];
    int shot_every = 0, i;
    uint64_t last_blink = 0, last_try = 0;
    unsigned frames = 0;
    uint16_t shot_index = 0;

#ifdef PSPEXP_HAVE_USB
    g_mode = MODE_USB;
    pspexp_usb_options_default(&g_usb);
#else
    g_mode = MODE_NULL;
#endif
    g_shell = getenv("SHELL") ? getenv("SHELL") : "/bin/sh";
    if (access(g_shell, X_OK) != 0) g_shell = "/bin/sh";
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--usb")) { g_mode = MODE_USB; g_want_usb = 1; }
        else if (!strcmp(argv[i], "--null")) g_mode = MODE_NULL;
        else if (!strcmp(argv[i], "--sdl")) g_mode = MODE_SDL;
        else if (!strcmp(argv[i], "--tcp")) {
            char *colon;
            if (g_mode != MODE_USB) g_mode = MODE_TCP;
            g_want_tcp = 1;
            pspexp_tcp_options_default(&g_tcp);
            g_tcp.connect_timeout_ms = 700;   /* retried every second: do not stall the terminal when the PSP is away */
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                strncpy(tcp_host, argv[++i], sizeof(tcp_host) - 1);
                colon = strrchr(tcp_host, ':');
                if (colon) { *colon = 0; g_tcp.port = (uint16_t)atoi(colon + 1); }
                if (tcp_host[0]) g_tcp.host = tcp_host;
            }
        }
        else if (!strcmp(argv[i], "--shell") && i + 1 < argc) g_shell = argv[++i];
        else if (!strcmp(argv[i], "--cmd") && i + 1 < argc) g_cmd = argv[++i];
        else if (!strcmp(argv[i], "--audio-fifo") && i + 1 < argc) fifo_path = argv[++i];
        else if (!strcmp(argv[i], "--no-audio-fifo")) fifo_path = NULL;
        else if (!strcmp(argv[i], "--pidfile") && i + 1 < argc) pidfile = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-host-input")) g_host_input = 0;
#ifdef __linux__
        else if (!strcmp(argv[i], "--test-key") && i + 1 < argc) {   /* send one key through the keypad path and exit */
            int code = atoi(argv[++i]);
            if (keypad_open() < 0) { fprintf(stderr, "no keypad (uinput or X)\n"); return 1; }
            usleep(300000);
            ui_emit(EV_KEY, code, 1); ui_emit(EV_SYN, SYN_REPORT, 0);
            usleep(80000);
            ui_emit(EV_KEY, code, 0); ui_emit(EV_SYN, SYN_REPORT, 0);
            usleep(300000);
            return 0;
        }
        else if (!strcmp(argv[i], "--mirror")) {
            g_fb_path = (i + 1 < argc && argv[i + 1][0] == '/') ? argv[++i] : "/dev/fb0";
            g_view_mirror = 1;
        }
#endif
        else if (!strcmp(argv[i], "--no-grab")) g_grab = 0;
        else if (!strcmp(argv[i], "--launcher")) g_launcher_on = 1;
        else if (!strcmp(argv[i], "--assets") && i + 1 < argc) g_launcher_assets = argv[++i];
        else { usage(); return 1; }
    }

    if (!g_want_usb && !g_want_tcp && g_mode == MODE_USB) g_want_usb = 1;
    g_test_keys = getenv("PSPEXP_TERM_KEYS");
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, on_sigchld);
    signal(SIGUSR1, on_usr1);
    signal(SIGUSR2, on_usr2);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    /* the pid file tells pspexp-exec whom to ask for the PSP. A second
     * console (screenshots, tests) leaves a running one's file alone, and
     * each removes the file at exit only when it still names itself. */
    if (pidfile) {
        FILE *f = fopen(pidfile, "r");
        int other = 0;
        if (f) { if (fscanf(f, "%d", &other) != 1) other = 0; fclose(f); }
        if (other > 0 && other != (int)getpid() && kill(other, 0) == 0) {
            fprintf(stderr, "[pspexp-term] %s names a running console (%d): not taking it over\n", pidfile, other);
            pidfile = NULL;
        } else if ((f = fopen(pidfile, "w"))) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
    }

    settings_load();
    g_cw = TERM_FONTS[g_font].w; g_ch = TERM_FONTS[g_font].h;
    T.cols = 480 / g_cw > COLS_MAX ? COLS_MAX : 480 / g_cw;
    term_reset(term_rows());
    spawn_shell();
    audio_fifo_open(fifo_path);

    if (g_mode == MODE_NULL) {
        g_nul = pspexp_null_create(NULL);
        pspexp_null_set_paced(g_nul, 1);
        ctx = pspexp_create(pspexp_null_transport(g_nul), NULL);
    } else if (g_mode == MODE_SDL) {
#ifdef PSPEXP_HAVE_SDL
        PspExpSdlOptions o;
        char err[200];
        pspexp_sdl_options_default(&o);
        o.title = "PSPExp terminal";
        g_sdl = pspexp_sdl_create(&o, err, sizeof(err));
        if (!g_sdl) { fprintf(stderr, "%s\n", err); return 1; }
        ctx = pspexp_create(pspexp_sdl_transport(g_sdl), NULL);
#else
        fprintf(stderr, "built without SDL\n"); return 1;
#endif
    } else {
        ctx = pspexp_create(NULL, NULL);
    }
    pspexp_set_log(ctx, log_fn, NULL);
    if (g_mode == MODE_NULL || g_mode == MODE_SDL) {
        if (pspexp_connect(ctx, 5000) < 0) { fprintf(stderr, "no HELLO\n"); return 1; }
        launcher_set_link(g_mode == MODE_SDL ? "SDL window (a test executor)" : "none (null executor)");
    } else {
        fprintf(stderr, "[pspexp-term] waiting for the PSP (%s%s%s)\n", g_want_usb ? "usb" : "", g_want_usb && g_want_tcp ? " + " : "", g_want_tcp ? "tcp" : "");
    }
    /* uploaded once, replayed by the library after every reconnect */
    have_font = pspexp_font_builtin_upload(ctx, FONT_ID, FONT_PAL_ID, &font) == 0;
    tfont_upload();   /* after the UI font: it uses the same palette */
    if (g_launcher_on) {
        if (!g_launcher_assets) g_launcher_assets = getenv("PSPEXP_LAUNCHER_ASSETS");
        if (!g_launcher_assets) g_launcher_assets = access("/usr/local/share/pspexp/launcher/lucide.ttf", R_OK) == 0 ? "/usr/local/share/pspexp/launcher"
#ifdef PSPEXP_LAUNCHER_ASSET_DIR
                                                    : PSPEXP_LAUNCHER_ASSET_DIR;   /* the source tree, for development */
#else
                                                    : "/usr/local/share/pspexp/launcher";
#endif
        if (launcher_init(ctx, g_launcher_assets) < 0) g_launcher_on = 0;
        else { g_view_launcher = 1; g_view_mirror = 0; }   /* the home view; the mirror shows the programs it starts */
    }

    while (!g_quit) {
        struct pollfd pfd[1];
        uint8_t buf[4096];
        uint64_t now;
        int connected;

        /* shell output */
        pfd[0].fd = g_master; pfd[0].events = POLLIN;
        if (poll(pfd, 1, 8) > 0 && (pfd[0].revents & (POLLIN | POLLHUP))) {
            for (;;) {
                ssize_t n = read(g_master, buf, sizeof(buf));
                if (n > 0) { term_feed(buf, (size_t)n); if (T.view) T.view = 0; continue; }
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) g_child_died = 1;
                break;
            }
        }
        if (g_child_died) {
            int status;
            if (waitpid(g_child, &status, WNOHANG) == g_child || g_master < 0) {
                close(g_master); g_master = -1;
                if (g_cmd) break;   /* --cmd: done when the command is */
                term_feed((const uint8_t *)"\r\n[shell exited; starting a new one]\r\n", 38);
                spawn_shell();
            }
            g_child_died = 0;
        }

        if (g_launch_pid > 0) {   /* a program started from the launcher */
            int st;
            if (waitpid(g_launch_pid, &st, WNOHANG) == g_launch_pid) launch_done(st);
        }
        /* hand-over */
        if (g_release_req) { g_release_req = 0; release_psp(); }
        if (g_acquire_req) { g_acquire_req = 0; g_released = 0; last_try = 0; }

        now = pspexp_time_us();
        connected = pspexp_link_state(ctx) == PSPEXP_LINK_CONNECTED;
        host_input_poll(connected);
        if (!connected && !g_released && (g_mode == MODE_USB || g_mode == MODE_TCP) && now - last_try > 1000000u) {
            last_try = now;
            if (try_connect()) { connected = 1; g_prev_buttons = 0xFFFFFFFFu; }   /* ignore buttons held while connecting */
        }
        if (!connected) continue;

        pspexp_pump(ctx, 0);
        {   /* liveness: the PSP sends input samples at 60 Hz; a link that stays
             * silent for 3 s is dead (a Wi-Fi peer that vanished without closing
             * leaves a half-open TCP connection that never errors) */
            static uint32_t last_inputs; static uint64_t last_heard;
            const PspExpStats *st = pspexp_stats(ctx);
            if (st->inputs_received != last_inputs || !last_heard) { last_inputs = st->inputs_received; last_heard = now; }
            else if (now - last_heard > 3000000u) {
                fprintf(stderr, "[pspexp-term] no word from the PSP for 3 s: dropping the link\n");
                keypad_release_all();
                pspexp_set_transport(ctx, NULL);
                last_heard = 0; last_try = 0;
                continue;
            }
        }
        handle_input();
        if (g_launcher_on && g_launch_pid <= 0) {
            char cmd[1024], cwd[512];
            int aware;
            if (launcher_take_launch(cmd, sizeof(cmd), cwd, sizeof(cwd), &aware)) launch_child(cmd, cwd, aware);
            if (launcher_take_console()) { g_view_launcher = 0; T.dirty = 1; }
        }
        audio_fifo_pump();
        if (g_bell) { g_bell = 0; beep(); }
        if (now - last_blink > 500000u) { last_blink = now; blink_on = !blink_on; if (T.cursor_visible && !T.view) T.dirty = 1; }
        if (g_view_launcher) {
            if (launcher_needs_frame(now) && pspexp_should_render(ctx)) {
                launcher_render(ctx, now, 1);
                frames++;
                if (shot_every && frames % (unsigned)shot_every == 0) pspexp_request_screenshot(ctx, shot_index++);
            }
        } else if (g_view_mirror) {
            /* ~30 fps poll of the framebuffer; frames only when it changed */
            static uint64_t last_mirror;
            if (now - last_mirror >= 16000u && pspexp_should_render(ctx)) {
                last_mirror = now;
                if (mirror_frame()) frames++;
            }
        } else if (T.dirty && pspexp_should_render(ctx)) {
            render();
            frames++;
            if (shot_every && frames % (unsigned)shot_every == 0) pspexp_request_screenshot(ctx, shot_index++);
        }
#ifdef PSPEXP_HAVE_SDL
        if (g_sdl && pspexp_sdl_quit_requested(g_sdl)) break;
#endif
        {   /* a status line every 5 s: what reaches the PSP and what comes back */
            static uint64_t last_stat;
            static unsigned last_frames, last_presented, last_inputs;
            if (now - last_stat >= 5000000u) {
                const PspExpStats *st = pspexp_stats(ctx);
                const PspExpInput *in = pspexp_input(ctx);
                fprintf(stderr, "[pspexp-term] %s view: frames sent %u, presented %u, inputs %u (buttons %08x), in flight %d, errors %u/%u, B/frame %u\n",
                        g_view_launcher ? "launcher" : g_view_mirror ? "mirror" : "terminal", frames - last_frames, st->frames_presented - last_presented,
                        st->inputs_received - last_inputs, (unsigned)in->buttons, pspexp_frames_in_flight(ctx),
                        st->protocol_errors, st->resource_errors, st->last_frame_bytes);
                last_stat = now; last_frames = frames; last_presented = st->frames_presented; last_inputs = st->inputs_received;
            }
        }
    }

    if (g_child > 0) kill(-g_child, SIGHUP);
    if (pspexp_link_state(ctx) == PSPEXP_LINK_CONNECTED) {
        pspexp_send_raw(ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_DISCONNECT, NULL, 0);
    }
    pspexp_destroy(ctx);
    if (g_nul) pspexp_null_destroy(g_nul);
#ifdef PSPEXP_HAVE_SDL
    if (g_sdl) pspexp_sdl_destroy(g_sdl);
#endif
    if (pidfile) {
        FILE *f = fopen(pidfile, "r");
        int who = 0;
        if (f) { if (fscanf(f, "%d", &who) != 1) who = 0; fclose(f); }
        if (who == (int)getpid()) unlink(pidfile);
    }
    return 0;
}
