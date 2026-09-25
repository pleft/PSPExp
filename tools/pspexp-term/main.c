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
 *   nub           arrow keys (history, cursor, vi)
 *   R + up/down   scroll back through the history (R + left/right: a page)
 *
 * Hand-over: another PSPExp program (a game) can take the PSP while the
 * terminal keeps running: SIGUSR1 releases the PSP, SIGUSR2 takes it back
 * (scripts/pspexp-exec does both around a command).
 */
#define _GNU_SOURCE
#include "../../host/libpspexp/pspexp.h"
#include "../../host/libpspexp/pspexp_font.h"
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

#define COLS        80
#define ROWS_FULL   34          /* 272 / 8 */
#define ROWS_KB     25          /* 200 px of terminal above the keyboard */
#define CELL_W      6
#define CELL_H      8
#define KB_Y        (ROWS_KB * CELL_H)
#define KB_H        (272 - KB_Y)
#define SCROLLBACK  1000

#define FONT_ID     1
#define FONT_PAL_ID 2

/* ---- terminal state ------------------------------------------------------ */

enum { A_BOLD = 1, A_UNDER = 2, A_REVERSE = 4 };

typedef struct Cell { uint8_t ch, attr; uint16_t fg, bg; } Cell;   /* colours: 5650 */
typedef struct Line { Cell c[COLS]; } Line;

static const uint32_t PALETTE[16] = {
    0x000000, 0xcd3131, 0x0dbc79, 0xe5e510, 0x3b78ff, 0xbc3fbc, 0x11a8cd, 0xd0d0d0,
    0x767676, 0xf14c4c, 0x23d18b, 0xf5f543, 0x5c8cff, 0xd670d6, 0x29b8db, 0xffffff,
};
#define DEF_FG 7
#define DEF_BG 0

typedef struct Pen { int fg, bg; uint16_t fg_rgb, bg_rgb; uint8_t attr; } Pen;   /* fg/bg: palette index, -2 = direct rgb */

typedef struct Term {
    int rows;                       /* 34 or 25 */
    Line screen[ROWS_FULL], alt[ROWS_FULL];
    int on_alt;
    int cx, cy, wrap_pending;
    int saved_cx, saved_cy; Pen saved_pen;
    int top, bottom;                /* scroll region, inclusive */
    Pen pen;
    int cursor_visible, autowrap, app_cursor;
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

static uint16_t color_of(int idx, uint16_t direct)
{
    if (idx == -2) return direct;
    if (idx < 16) return rgb565(PALETTE[idx]);
    if (idx < 232) {                /* 6x6x6 cube */
        int i = idx - 16, r = i / 36, g = (i / 6) % 6, b = i % 6;
        static const uint8_t lv[6] = { 0, 95, 135, 175, 215, 255 };
        return pspexp_color_5650(lv[r], lv[g], lv[b]);
    }
    { uint8_t v = (uint8_t)(8 + (idx - 232) * 10); return pspexp_color_5650(v, v, v); }
}

static Cell blank_cell(void)
{
    Cell c;
    c.ch = ' '; c.attr = 0;
    c.fg = color_of(DEF_FG, 0);
    c.bg = T.pen.bg == DEF_BG ? 0 : color_of(T.pen.bg, T.pen.bg_rgb);   /* erase uses the current background */
    return c;
}

static void clear_line(Line *l, int from, int to)
{
    int x;
    Cell b = blank_cell();
    for (x = from; x < to && x < COLS; x++) l->c[x] = b;
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
        clear_line(line(bottom), 0, COLS);
    }
    T.dirty = 1;
}

static void scroll_down(int top, int bottom, int n)
{
    int y;
    while (n-- > 0) {
        for (y = bottom; y > top; y--) *line(y) = *line(y - 1);
        clear_line(line(top), 0, COLS);
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
    for (y = 0; y < ROWS_FULL; y++) { clear_line(&T.screen[y], 0, COLS); clear_line(&T.alt[y], 0, COLS); }
    T.dirty = 1;
}

/* Keyboard shown / hidden: 25 / 34 rows. Content stays bottom-aligned (the
 * prompt stays put): shrinking pushes top lines into the scrollback, growing
 * pulls them back. The shell and full-screen programs get SIGWINCH. */
static void term_resize(int rows)
{
    int d, y;
    if (rows == T.rows) return;
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
        for (y = T.rows + d; y < rows; y++) clear_line(&T.screen[y], 0, COLS);
    }
    for (y = T.rows; y < rows; y++) clear_line(&T.alt[y], 0, COLS);
    T.rows = rows;
    T.top = 0; T.bottom = rows - 1;
    if (T.cy >= rows) T.cy = rows - 1;
    T.view = 0;
    T.dirty = 1;
    if (g_master >= 0) {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_row = (unsigned short)rows; ws.ws_col = COLS;
        ws.ws_xpixel = COLS * CELL_W; ws.ws_ypixel = (unsigned short)(rows * CELL_H);
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
    c->fg = color_of((T.pen.attr & A_BOLD) && T.pen.fg >= 0 && T.pen.fg < 8 ? T.pen.fg + 8 : T.pen.fg, T.pen.fg_rgb);
    c->bg = T.pen.bg == DEF_BG ? 0 : color_of(T.pen.bg, T.pen.bg_rgb);
    if (T.cx == COLS - 1) { if (T.autowrap) T.wrap_pending = 1; }
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
    if (T.cx >= COLS) T.cx = COLS - 1;
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
        else if (p == 47 || p == 1047 || p == 1049) {
            if (on && !T.on_alt) {
                int y;
                if (p == 1049) { T.saved_cx = T.cx; T.saved_cy = T.cy; T.saved_pen = T.pen; }
                T.on_alt = 1;
                for (y = 0; y < ROWS_FULL; y++) clear_line(&T.alt[y], 0, COLS);
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
        if (n == 0) { clear_line(line(T.cy), T.cx, COLS); for (y = T.cy + 1; y < T.rows; y++) clear_line(line(y), 0, COLS); }
        else if (n == 1) { clear_line(line(T.cy), 0, T.cx + 1); for (y = 0; y < T.cy; y++) clear_line(line(y), 0, COLS); }
        else { for (y = 0; y < T.rows; y++) clear_line(line(y), 0, COLS); }
        T.dirty = 1; break;
    case 'K':
        n = P0(0);
        if (n == 0) clear_line(line(T.cy), T.cx, COLS);
        else if (n == 1) clear_line(line(T.cy), 0, T.cx + 1);
        else clear_line(line(T.cy), 0, COLS);
        T.dirty = 1; break;
    case 'L': if (T.cy >= T.top && T.cy <= T.bottom) scroll_down(T.cy, T.bottom, P(0, 1)); break;
    case 'M': if (T.cy >= T.top && T.cy <= T.bottom) scroll_up(T.cy, T.bottom, P(0, 1)); break;
    case 'S': scroll_up(T.top, T.bottom, P(0, 1)); break;
    case 'T': scroll_down(T.top, T.bottom, P(0, 1)); break;
    case 'P': {
        Line *l = line(T.cy); int k = P(0, 1), x;
        if (k > COLS - T.cx) k = COLS - T.cx;
        for (x = T.cx; x < COLS - k; x++) l->c[x] = l->c[x + k];
        clear_line(l, COLS - k, COLS); T.dirty = 1; break;
    }
    case '@': {
        Line *l = line(T.cy); int k = P(0, 1), x;
        if (k > COLS - T.cx) k = COLS - T.cx;
        for (x = COLS - 1; x >= T.cx + k; x--) l->c[x] = l->c[x - k];
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
    case 't': if (P0(0) == 18) { snprintf(buf, sizeof(buf), "\033[8;%d;%dt", T.rows, COLS); pty_puts(buf); } break;
    default: break;
    }
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
                if ((b & 0xC0) == 0x80) { if (--T.utf8_left == 0) put_char('?'); continue; }
                T.utf8_left = 0;
            }
            if (b >= 0xC0 && b < 0xF8) { T.utf8_left = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : 1; continue; }
            if (b >= 0x80) { put_char('?'); continue; }
            if (b >= 32 && b < 127) { put_char(b); continue; }
            switch (b) {
            case 7: g_bell = 1; break;
            case 8: if (T.cx > 0) T.cx--; T.wrap_pending = 0; T.dirty = 1; break;
            case 9: T.cx = (T.cx / 8 + 1) * 8; if (T.cx >= COLS) T.cx = COLS - 1; T.wrap_pending = 0; T.dirty = 1; break;
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
            case '(': case ')': case '*': case '+': T.state = S_CHARSET; break;
            case '7': T.saved_cx = T.cx; T.saved_cy = T.cy; T.saved_pen = T.pen; break;
            case '8': T.cx = T.saved_cx; T.cy = T.saved_cy; T.pen = T.saved_pen; clamp_cursor(); break;
            case 'D': newline(); break;
            case 'E': T.cx = 0; newline(); break;
            case 'M': if (T.cy == T.top) scroll_down(T.top, T.bottom, 1); else if (T.cy > 0) T.cy--; T.dirty = 1; break;
            case 'c': term_reset(T.rows); break;
            default: break;   /* ESC = / ESC > (keypad modes) and others */
            }
            break;
        case S_CHARSET: T.state = S_GROUND; break;
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

static PspExpContext *ctx;
static PspExpFont font;
static int have_font, blink_on = 1;

typedef struct Batch { PspExpSpriteRec r[COLS * ROWS_FULL + 512]; int n; } Batch;
static Batch g_batch;

static void glyph(int x, int y, uint8_t ch, uint16_t color)
{
    if (ch <= ' ' || ch < font.first_char || ch >= font.first_char + font.glyph_count) return;
    if (g_batch.n >= (int)(sizeof(g_batch.r) / sizeof(g_batch.r[0]))) return;
    g_batch.r[g_batch.n].x = (int16_t)x; g_batch.r[g_batch.n].y = (int16_t)y;
    g_batch.r[g_batch.n].frame = (uint16_t)(font.first_frame + (ch - font.first_char));
    g_batch.r[g_batch.n].color = color;
    g_batch.n++;
}

static void text(int x, int y, const char *s, uint16_t color) { for (; *s; s++, x += CELL_W) glyph(x, y, (uint8_t)*s, color); }

static void draw_terminal(int rows)
{
    int y, x;
    for (y = 0; y < rows; y++) {
        const Line *l;
        int src = y - T.view;   /* with scrollback: rows above the screen come from the history */
        if (src < 0) l = sb_line(-src); else l = line(src);
        /* backgrounds: runs of equal non-black colour */
        for (x = 0; x < COLS;) {
            const Cell *c = &l->c[x];
            uint16_t bg = (c->attr & A_REVERSE) ? c->fg : c->bg;
            int x1 = x + 1;
            while (x1 < COLS) {
                const Cell *d = &l->c[x1];
                uint16_t bg2 = (d->attr & A_REVERSE) ? d->fg : d->bg;
                if (bg2 != bg) break;
                x1++;
            }
            if (bg) pspexp_fill_rect(ctx, (int16_t)(x * CELL_W), (int16_t)(y * CELL_H), (int16_t)((x1 - x) * CELL_W), CELL_H, bg, 255);
            x = x1;
        }
        for (x = 0; x < COLS; x++) {
            const Cell *c = &l->c[x];
            uint16_t fg = (c->attr & A_REVERSE) ? (c->bg ? c->bg : 0) : c->fg;
            if ((c->attr & A_REVERSE) && !c->bg) fg = 0;
            glyph(x * CELL_W, y * CELL_H, c->ch, fg ? fg : 0x0001);
            if (c->attr & A_UNDER) pspexp_fill_rect(ctx, (int16_t)(x * CELL_W), (int16_t)(y * CELL_H + 7), CELL_W, 1, fg, 255);
        }
    }
    if (T.view == 0 && T.cursor_visible && blink_on && T.cy < rows)
        pspexp_fill_rect(ctx, (int16_t)(T.cx * CELL_W), (int16_t)(T.cy * CELL_H + 6), CELL_W, 2, rgb565(0xffffff), 200);
    if (T.view > 0) {
        char s[40];
        snprintf(s, sizeof(s), " SCROLLBACK -%d ", T.view);
        pspexp_fill_rect(ctx, (int16_t)(480 - (int)strlen(s) * CELL_W), 0, (int16_t)(strlen(s) * CELL_W), CELL_H, rgb565(0xe5e510), 255);
        text(480 - (int)strlen(s) * CELL_W, 0, s, 0x0001);
    }
}

static void draw_keyboard(void)
{
    int r, i;
    pspexp_fill_rect(ctx, 0, KB_Y, 480, KB_H, rgb565(0x202830), 255);
    for (r = 0; r < KROWS; r++) {
        int x = 0, y = KB_Y + r * KEY_H;
        for (i = 0; i < KCOUNT[r]; i++) {
            const Key *k = &KROWS_[r][i];
            int w = k->w * 16, sel = r == kb_row && i == kb_col;
            int shifted = sticky_shift || held_shift;
            int active = (k->kind == K_SHIFT && shifted) || (k->kind == K_CTRL && (sticky_ctrl || held_ctrl));
            uint16_t bg = sel ? rgb565(0xe0a020) : active ? rgb565(0x3b78ff) : rgb565(0x3a4450);
            uint16_t fg = sel ? rgb565(0x101010) : rgb565(0xe8e8e8);
            /* with Shift active the keys show what they will type */
            const char *label = (shifted && k->sout && k->kind == 0 && k->out && !k->out[1]) ? k->sout : k->label;
            int len = (int)strlen(label);
            pspexp_fill_rect(ctx, (int16_t)(x + 1), (int16_t)(y + 1), (int16_t)(w - 2), KEY_H - 2, bg, 255);
            text(x + (w - len * CELL_W) / 2, y + (KEY_H - CELL_H) / 2 + 1, label, fg);
            x += w;
        }
    }
}

static void render(void)
{
    int rows = kb_shown ? ROWS_KB : ROWS_FULL;
    g_batch.n = 0;
    pspexp_begin_frame(ctx);
    pspexp_set_clip(ctx, 0, 0, 480, 272);
    pspexp_fill_rect(ctx, 0, 0, 480, 272, 0, 255);
    if (have_font) {
        draw_terminal(rows);
        if (kb_shown) draw_keyboard();
        if (g_batch.n) pspexp_draw_sprite_batch(ctx, font.atlas, g_batch.r, (uint32_t)g_batch.n);
    }
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
    if (!path) return;
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
    ws.ws_row = (unsigned short)T.rows; ws.ws_col = COLS;
    g_child = forkpty(&g_master, NULL, NULL, &ws);
    if (g_child < 0) { perror("forkpty"); exit(1); }
    if (g_child == 0) {
        setenv("TERM", "xterm", 1);
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

static PspExpTransport *open_transport(void)
{
    char err[200];
    switch (g_mode) {
    case MODE_TCP: return pspexp_tcp_open(&g_tcp, err, sizeof(err));
#ifdef PSPEXP_HAVE_USB
    case MODE_USB: return pspexp_usb_open(&g_usb, err, sizeof(err));
#endif
    default: return NULL;
    }
}

static int try_connect(void)
{
    PspExpTransport *t = open_transport();
    if (!t) return 0;
    pspexp_set_transport(ctx, t);
    if (pspexp_connect(ctx, 3000) < 0) { pspexp_set_transport(ctx, NULL); return 0; }
    fprintf(stderr, "[pspexp-term] PSP connected\n");
    T.dirty = 1;
    return 1;
}

static void release_psp(void)
{
    if (g_released) return;
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

static void handle_input(void)
{
    const PspExpInput *in = pspexp_input(ctx);
    uint32_t b = in->buttons, pressed = b & ~g_prev_buttons;
    uint64_t now = pspexp_time_us();
    int shift = (b & PSPEXP_BUTTON_LTRIGGER) != 0, ctrl = (b & PSPEXP_BUTTON_RTRIGGER) != 0;
    int ax = (int)in->analog_x - 128, ay = (int)in->analog_y - 128;
    int up = fire(V_UP, b & PSPEXP_BUTTON_UP, now, 1), down = fire(V_DOWN, b & PSPEXP_BUTTON_DOWN, now, 1);
    int left = fire(V_LEFT, b & PSPEXP_BUTTON_LEFT, now, 1), right = fire(V_RIGHT, b & PSPEXP_BUTTON_RIGHT, now, 1);
    g_prev_buttons = b;
    if (shift != held_shift || ctrl != held_ctrl) { held_shift = shift; held_ctrl = ctrl; if (kb_shown) T.dirty = 1; }

    if (pressed & PSPEXP_BUTTON_SELECT) { kb_shown = !kb_shown; term_resize(kb_shown ? ROWS_KB : ROWS_FULL); }

    if (ctrl && (up || down || left || right)) {   /* R + direction: scrollback */
        int page = (kb_shown ? ROWS_KB : ROWS_FULL) - 1;
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
    /* the nub: arrow keys, always */
    if (fire(N_UP, ay < -60, now, 1)) send_arrow('A');
    if (fire(N_DOWN, ay > 60, now, 1)) send_arrow('B');
    if (fire(N_LEFT, ax < -60, now, 1)) send_arrow('D');
    if (fire(N_RIGHT, ax > 60, now, 1)) send_arrow('C');

    if (fire(V_CROSS, b & PSPEXP_BUTTON_CROSS, now, 1) && kb_shown) { send_key(&KROWS_[kb_row][kb_col], shift, ctrl); T.view = 0; T.dirty = 1; }
    if (fire(V_CIRCLE, b & PSPEXP_BUTTON_CIRCLE, now, 1)) { pty_puts("\177"); T.view = 0; }
    if (pressed & PSPEXP_BUTTON_SQUARE) { pty_puts(" "); T.view = 0; }
    if (pressed & PSPEXP_BUTTON_TRIANGLE) { pty_puts("\r"); T.view = 0; }
    if (pressed & PSPEXP_BUTTON_START) pty_puts("\t");
}

/* ---- main ------------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr, "usage: pspexp-term [--usb | --tcp host[:port] | --null | --sdl] [--shell PATH] [--cmd \"command\"]\n"
                    "                   [--audio-fifo PATH | --no-audio-fifo] [--pidfile PATH] [--shot N]\n");
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
        if (!strcmp(argv[i], "--usb")) g_mode = MODE_USB;
        else if (!strcmp(argv[i], "--null")) g_mode = MODE_NULL;
        else if (!strcmp(argv[i], "--sdl")) g_mode = MODE_SDL;
        else if (!strcmp(argv[i], "--tcp")) {
            char *colon;
            g_mode = MODE_TCP;
            pspexp_tcp_options_default(&g_tcp);
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
        else { usage(); return 1; }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, on_sigchld);
    signal(SIGUSR1, on_usr1);
    signal(SIGUSR2, on_usr2);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    if (pidfile) { FILE *f = fopen(pidfile, "w"); if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); } }

    term_reset(kb_shown ? ROWS_KB : ROWS_FULL);
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
    } else {
        fprintf(stderr, "[pspexp-term] waiting for the PSP (%s)\n", g_mode == MODE_TCP ? "tcp" : "usb");
    }
    /* uploaded once, replayed by the library after every reconnect */
    have_font = pspexp_font_builtin_upload(ctx, FONT_ID, FONT_PAL_ID, &font) == 0;

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

        /* hand-over */
        if (g_release_req) { g_release_req = 0; release_psp(); }
        if (g_acquire_req) { g_acquire_req = 0; g_released = 0; last_try = 0; }

        now = pspexp_time_us();
        connected = pspexp_link_state(ctx) == PSPEXP_LINK_CONNECTED;
        if (!connected && !g_released && (g_mode == MODE_USB || g_mode == MODE_TCP) && now - last_try > 1000000u) {
            last_try = now;
            if (try_connect()) { connected = 1; g_prev_buttons = 0xFFFFFFFFu; }   /* ignore buttons held while connecting */
        }
        if (!connected) continue;

        pspexp_pump(ctx, 0);
        handle_input();
        audio_fifo_pump();
        if (g_bell) { g_bell = 0; beep(); }
        if (now - last_blink > 500000u) { last_blink = now; blink_on = !blink_on; if (T.cursor_visible && !T.view) T.dirty = 1; }
        if (T.dirty && pspexp_should_render(ctx)) {
            render();
            frames++;
            if (shot_every && frames % (unsigned)shot_every == 0) pspexp_request_screenshot(ctx, shot_index++);
        }
#ifdef PSPEXP_HAVE_SDL
        if (g_sdl && pspexp_sdl_quit_requested(g_sdl)) break;
#endif
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
    if (pidfile) unlink(pidfile);
    return 0;
}
