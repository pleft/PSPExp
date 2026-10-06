/*
 * The system monitor (Apps > System Monitor): the board's CPU, memory,
 * temperature, storage, network and the PSP's link on one page, its
 * programs on another, where one can be ended. Refreshed once a second.
 *
 *   L / R          the page (Overview, Programs)
 *   up / down      a program            cross   end it (asks)
 *   square         sort by CPU / memory circle, select   close
 */
#include "appview.h"
#include "sysinfo.h"
#include "files_ops.h"
#include "files_psp.h"
#include "launcher_gfx.h"
#include "launcher_ui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

enum { PAGE_OVERVIEW, PAGE_PROGRAMS };
static SysInfo g_s;
static uint64_t g_at;                 /* last refresh */
static int g_page, g_sel, g_top, g_sort_mem, g_dirty = 1, g_exit;
static int g_confirm;                 /* the "end it?" question is up */
static int g_sel_pid;                 /* the highlighted program, kept across refreshes */
static char g_msg[160];               /* a result line, for a few seconds */
static uint64_t g_msg_until;
static pid_t g_program;               /* what the launcher started */
static char g_psp_space[48];
static uint64_t g_psp_space_at;
static const PspExpStats *g_stats_ref;
static uint32_t g_frames_prev, g_frames_rate;
static uint64_t g_bytes_prev, g_bytes_rate;

void monitor_set_program(pid_t pid) { g_program = pid; }

static int cmp_mem(const void *a, const void *b)
{
    const SysProc *x = a, *y = b;
    return x->rss < y->rss ? 1 : x->rss > y->rss ? -1 : 0;
}

static void refresh(uint64_t now)
{
    int i;
    sysinfo_update(&g_s);
    if (g_sort_mem) qsort(g_s.proc, (size_t)g_s.nproc, sizeof(SysProc), cmp_mem);
    for (i = 0; i < g_s.nproc; i++) if (g_s.proc[i].pid == g_sel_pid) { g_sel = i; break; }
    if (g_sel >= g_s.nproc) g_sel = g_s.nproc ? g_s.nproc - 1 : 0;
    if (g_s.nproc) g_sel_pid = g_s.proc[g_sel].pid;
    if (files_psp_available() && (!g_psp_space_at || now - g_psp_space_at > 5000000u)) {
        uint64_t fr, tot;
        char a[32], b[32];
        g_psp_space_at = now;
        if (files_psp_space("ms0:/", &fr, &tot) == 0 && tot) { fops_human(fr, a, sizeof(a)); fops_human(tot, b, sizeof(b)); snprintf(g_psp_space, sizeof(g_psp_space), "%s free of %s", a, b); }
    } else if (!files_psp_available()) g_psp_space[0] = 0;
    g_at = now;
    g_dirty = 1;
}

/* may this program be ended from here: the user's own (not root's), or on a
 * board where everything is root, only what the launcher started */
static int endable(const SysProc *p)
{
    if (p->pid <= 1 || p->pid == getpid() || p->pid == getppid()) return 0;
    if (g_program > 0 && (p->pid == g_program || getpgid(p->pid) == g_program)) return 1;
    return getuid() != 0 && p->uid == (int)getuid();
}

static void mon_open(void)
{
    g_page = PAGE_OVERVIEW; g_confirm = 0; g_exit = 0; g_msg[0] = 0;
    refresh(0);   /* the first figures; rates and shares need the second, a second later */
    g_at = 0;
    g_dirty = 1;
}

static int mon_take_exit(void) { int e = g_exit; g_exit = 0; return e; }

static void mon_input(uint32_t b, uint32_t pressed, int up, int down, int left, int right, uint64_t now)
{
    (void)b; (void)left; (void)right;
    if (!pressed && !up && !down) return;
    g_dirty = 1;
    if (g_confirm) {
        if (pressed & PSPEXP_BUTTON_CROSS) {
            const SysProc *p = &g_s.proc[g_sel];
            if (sysinfo_end(p->pid, 0) == 0) snprintf(g_msg, sizeof(g_msg), "Asked %s (%d) to end", p->name, p->pid);
            else snprintf(g_msg, sizeof(g_msg), "Could not end %s: %s", p->name, strerror(errno));
            g_msg_until = now + 4000000u;
            g_confirm = 0;
            g_at = 0;   /* refresh now */
        } else if (pressed & PSPEXP_BUTTON_CIRCLE) g_confirm = 0;
        return;
    }
    if (pressed & PSPEXP_BUTTON_LTRIGGER) g_page = PAGE_OVERVIEW;
    if (pressed & PSPEXP_BUTTON_RTRIGGER) g_page = PAGE_PROGRAMS;
    if (pressed & PSPEXP_BUTTON_CIRCLE) { g_exit = 1; return; }
    if (g_page != PAGE_PROGRAMS) return;
    if (up && g_sel > 0) g_sel--;
    if (down && g_sel < g_s.nproc - 1) g_sel++;
    if (g_s.nproc) g_sel_pid = g_s.proc[g_sel].pid;
    if (pressed & PSPEXP_BUTTON_SQUARE) { g_sort_mem = !g_sort_mem; g_at = 0; }
    if ((pressed & PSPEXP_BUTTON_CROSS) && g_s.nproc) {
        if (endable(&g_s.proc[g_sel])) g_confirm = 1;
        else { snprintf(g_msg, sizeof(g_msg), "%s is the system's: not ended from here", g_s.proc[g_sel].name); g_msg_until = now + 4000000u; }
    }
}

static int mon_needs_frame(uint64_t now)
{
    if (!g_at || now - g_at >= 1000000u) refresh(now);
    if (g_msg[0] && now > g_msg_until) { g_msg[0] = 0; g_dirty = 1; }
    return g_dirty;
}

/* ---- drawing ---------------------------------------------------------------------- */

static void bytes(uint64_t b, char *o, size_t n) { fops_human(b, o, n); }

static void rate(double bps, char *o, size_t n)
{
    if (bps >= 1048576) snprintf(o, n, "%.1f MB/s", bps / 1048576);
    else if (bps >= 1024) snprintf(o, n, "%.0f KB/s", bps / 1024);
    else snprintf(o, n, "%.0f B/s", bps);
}

static uint32_t level_col(double pct) { return pct > 85 ? 0xff7a7a : pct > 60 ? 0xffc766 : UI_ACCENT; }

static void bar(int x, int y, int w, int h, double pct)
{
    int f = (int)(w * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100);
    gfx_nine(NINE_R6, x, y, w, h, 0x0d1628, 255);
    if (f > 2) gfx_nine(NINE_R6, x, y, f, h, level_col(pct), 255);
}

static void card(int x, int y, int w, int h, const char *icon, const char *title)
{
    gfx_nine(NINE_R10, x, y, w, h, UI_TILE, 235);
    gfx_icon(icon, 12, x + 9, y + 7, UI_ACCENT, 255);
    gfx_text(FONT_SMALL, x + 26, y + 4, title, UI_SUB, 255, w - 34);
}

static void draw_overview(PspExpContext *ctx)
{
    char a[64], b[64], s[160];
    int i, x, y, cw;
    const int L = 8, R = 246, W = 226;
    /* CPU */
    card(L, 32, W, 78, "cpu", "CPU");
    snprintf(s, sizeof(s), "%.0f%%", g_s.cpu);
    gfx_text(FONT_H1, L + 10, 48, s, level_col(g_s.cpu), 255, 0);
    cw = g_s.ncpu > 0 ? (W - 86 - (g_s.ncpu - 1) * 3) / g_s.ncpu : 0;
    for (i = 0; i < g_s.ncpu && i < SYS_CORES; i++) {   /* one column per core */
        int h = 30, f = (int)(h * g_s.core[i] / 100), cx = L + 76 + i * (cw + 3);
        gfx_nine(NINE_R6, cx, 50, cw, h, 0x0d1628, 255);
        if (f > 1) gfx_rect(cx + 1, 50 + h - f, cw - 2, f, level_col(g_s.core[i]), 255);
    }
    s[0] = 0;
    snprintf(s, sizeof(s), "%d core%s", g_s.ncpu, g_s.ncpu == 1 ? "" : "s");
    if (g_s.mhz) { snprintf(a, sizeof(a), "  %d MHz", g_s.mhz); strncat(s, a, sizeof(s) - strlen(s) - 1); }
    if (g_s.temp_mc >= 0) { snprintf(a, sizeof(a), "  %.0f C", g_s.temp_mc / 1000.0); strncat(s, a, sizeof(s) - strlen(s) - 1); }   /* no degree sign in the font */
    snprintf(a, sizeof(a), "  load %.2f %.2f %.2f", g_s.load[0], g_s.load[1], g_s.load[2]);
    strncat(s, a, sizeof(s) - strlen(s) - 1);
    gfx_text(FONT_BADGE, L + 10, 92, s, UI_MUTED, 255, W - 20);
    /* memory */
    card(L, 116, W, 50, "gauge", "Memory");
    {
        uint64_t used = g_s.mem_total > g_s.mem_avail ? g_s.mem_total - g_s.mem_avail : 0;
        double pct = g_s.mem_total ? 100.0 * used / g_s.mem_total : 0;
        bytes(used, a, sizeof(a)); bytes(g_s.mem_total, b, sizeof(b));
        snprintf(s, sizeof(s), "%s of %s used", a, b);
        gfx_text(FONT_SMALL, L + W - 10 - gfx_text_width(FONT_SMALL, s), 120, s, UI_TEXT, 255, 0);
        bar(L + 10, 138, W - 20, 8, pct);
        if (g_s.swap_total) { bytes(g_s.swap_total - g_s.swap_free, a, sizeof(a)); bytes(g_s.swap_total, b, sizeof(b)); snprintf(s, sizeof(s), "swap %s of %s", a, b); }
        else snprintf(s, sizeof(s), "no swap");
        gfx_text(FONT_BADGE, L + 10, 150, s, UI_MUTED, 255, W - 20);
    }
    /* the board */
    card(L, 172, W, 72, "server", "This board");
    gfx_text(FONT_SMALL, L + 10, 190, g_s.model, UI_TEXT, 255, W - 20);
    {
        long up = (long)g_s.uptime;
        if (up >= 86400) snprintf(a, sizeof(a), "up %ld d %ld h", up / 86400, up / 3600 % 24);
        else snprintf(a, sizeof(a), "up %ld h %ld min", up / 3600, up / 60 % 60);
        snprintf(s, sizeof(s), "%s  %s", g_s.host, a);
        gfx_text(FONT_BADGE, L + 10, 207, s, UI_SUB, 255, W - 20);
        gfx_text(FONT_BADGE, L + 10, 221, g_s.kernel, UI_MUTED, 255, W - 20);
    }
    /* storage: the board's drives and the PSP's stick */
    {
        FDrive d[FOPS_DRIVES];
        int n = fops_board_drives(d, FOPS_DRIVES, NULL), rows = n + (g_psp_space[0] ? 1 : 0), h;
        if (rows > 4) rows = 4;
        h = 24 + rows * 22;
        card(R, 32, W, h, "hard-drive", "Storage");
        y = 50;
        for (i = 0; i < n && i < 4; i++, y += 22) {
            struct statvfs sv;
            double pct = 0;
            s[0] = 0;
            if (statvfs(d[i].root, &sv) == 0 && sv.f_blocks) {
                uint64_t tot = (uint64_t)sv.f_blocks * sv.f_frsize, fr = (uint64_t)sv.f_bavail * sv.f_frsize;
                pct = 100.0 * (tot - fr) / tot;
                bytes(fr, a, sizeof(a)); bytes(tot, b, sizeof(b));
                snprintf(s, sizeof(s), "%s free of %s", a, b);
            }
            gfx_text(FONT_BADGE, R + 10, y, d[i].name, UI_TEXT, 255, 80);
            gfx_text(FONT_BADGE, R + W - 10 - gfx_text_width(FONT_BADGE, s), y, s, UI_MUTED, 255, 0);
            bar(R + 10, y + 12, W - 20, 5, pct);
        }
        if (g_psp_space[0] && i < 4) {
            gfx_text(FONT_BADGE, R + 10, y, "PSP", UI_TEXT, 255, 80);
            gfx_text(FONT_BADGE, R + W - 10 - gfx_text_width(FONT_BADGE, g_psp_space), y, g_psp_space, UI_MUTED, 255, 0);
            y += 22;
        }
        y += 8;
    }
    /* network */
    {
        int h = 24 + (g_s.nnet ? g_s.nnet : 1) * 16;
        if (y + h > 196) h = 196 - y;
        card(R, y, W, h, "network", "Network");
        x = y + 20;
        if (!g_s.nnet) gfx_text(FONT_BADGE, R + 10, x, "no address", UI_MUTED, 255, 0);
        for (i = 0; i < g_s.nnet && x + 12 <= y + h; i++, x += 16) {
            rate(g_s.net[i].rx_bps, a, sizeof(a)); rate(g_s.net[i].tx_bps, b, sizeof(b));
            snprintf(s, sizeof(s), "%s  %s", g_s.net[i].name, g_s.net[i].addr);
            if (g_s.net[i].wifi >= 0) { char q[16]; snprintf(q, sizeof(q), "  %d%%", g_s.net[i].wifi); strncat(s, q, sizeof(s) - strlen(s) - 1); }
            gfx_text(FONT_BADGE, R + 10, x, s, UI_TEXT, 255, 130);
            {   /* down and up, each with its arrow */
                int wb = gfx_text_width(FONT_BADGE, b), wa = gfx_text_width(FONT_BADGE, a), ex = R + W - 10;
                gfx_text(FONT_BADGE, ex - wb, x, b, UI_MUTED, 255, 0);
                gfx_icon("arrow-up", 9, ex - wb - 11, x + 2, UI_MUTED, 255);
                gfx_text(FONT_BADGE, ex - wb - 18 - wa, x, a, UI_MUTED, 255, 0);
                gfx_icon("arrow-down", 9, ex - wb - 29 - wa, x + 2, UI_MUTED, 255);
            }
        }
        y += h + 6;
    }
    /* the PSP's link */
    {
        const PspExpStats *st = pspexp_stats(ctx);
        card(R, y, W, 244 - y, "gamepad-2", "PSP link");
        if (g_stats_ref == st) { g_frames_rate = st->frames_presented - g_frames_prev; g_bytes_rate = st->bytes_sent - g_bytes_prev; }
        g_stats_ref = st; g_frames_prev = st->frames_presented; g_bytes_prev = st->bytes_sent;
        rate((double)g_bytes_rate, a, sizeof(a));
        snprintf(s, sizeof(s), "%u frames/s  %s  latency %.1f ms", g_frames_rate, a, st->last_present_latency_us / 1000.0);
        if (244 - y >= 36) gfx_text(FONT_BADGE, R + 10, y + 21, s, UI_SUB, 255, W - 20);
    }
}

static void draw_programs(void)
{
    const int X = 8, W = 464, ROWH = 15, ROWS = 12, Y0 = 50;
    int i;
    char a[32], s[64];
    gfx_nine(NINE_R10, X, 32, W, 214, UI_TILE, 235);
    gfx_text(FONT_BADGE, X + 12, 37, "PROGRAM", UI_MUTED, 255, 0);
    gfx_text(FONT_BADGE, X + 250, 37, "PID", UI_MUTED, 255, 0);
    gfx_text(FONT_BADGE, X + 310, 37, "CPU", g_sort_mem ? UI_MUTED : UI_ACCENT, 255, 0);
    gfx_text(FONT_BADGE, X + 380, 37, "MEMORY", g_sort_mem ? UI_ACCENT : UI_MUTED, 255, 0);
    gfx_icon("chevron-down", 9, X + (g_sort_mem ? 380 + gfx_text_width(FONT_BADGE, "MEMORY") : 310 + gfx_text_width(FONT_BADGE, "CPU")) + 3, 39, UI_ACCENT, 255);   /* the sort */
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + ROWS) g_top = g_sel - ROWS + 1;
    for (i = 0; i < ROWS && g_top + i < g_s.nproc; i++) {
        const SysProc *p = &g_s.proc[g_top + i];
        int y = Y0 + i * ROWH, sel = g_top + i == g_sel, can = endable(p);
        uint32_t tc = sel ? 0xffffff : can ? UI_TEXT : UI_SUB;
        if (sel) gfx_nine(NINE_R6, X + 4, y - 1, W - 8, ROWH, UI_ACCENT, 70);
        if (!can) gfx_icon("lock", 10, X + 10, y + 2, UI_MUTED, 200);
        gfx_text(FONT_SMALL, X + 26, y - 1, p->name, tc, 255, 215);
        snprintf(s, sizeof(s), "%d", p->pid);
        gfx_text(FONT_BADGE, X + 250, y + 1, s, UI_SUB, 255, 0);
        snprintf(s, sizeof(s), "%.1f%%", p->cpu);
        gfx_text(FONT_BADGE, X + 310, y + 1, s, p->cpu >= 50 ? 0xffc766 : UI_SUB, 255, 0);
        bytes(p->rss, a, sizeof(a));
        gfx_text(FONT_BADGE, X + 380, y + 1, a, UI_SUB, 255, 0);
    }
    if (g_confirm && g_sel < g_s.nproc) {
        const SysProc *p = &g_s.proc[g_sel];
        int w = 300, h = 76, bx = (UI_SCREEN_W - w) / 2, by = 90;
        gfx_shadow(NINE_SHADOW14, bx, by, w, h, 0x000000, 160);
        gfx_nine(NINE_R14, bx, by, w, h, 0x1b2a46, 250);
        snprintf(s, sizeof(s), "End %s (%d)?", p->name, p->pid);
        gfx_text(FONT_BODY, bx + 16, by + 12, s, UI_TEXT, 255, w - 32);
        gfx_text(FONT_SMALL, bx + 16, by + 32, "Unsaved work in it is lost.", UI_SUB, 255, w - 32);
        gfx_button('x', bx + 16, by + 54); gfx_text(FONT_SMALL, bx + 34, by + 53, "End it", UI_TEXT, 255, 0);
        gfx_button('o', bx + 120, by + 54); gfx_text(FONT_SMALL, bx + 138, by + 53, "Keep", UI_TEXT, 255, 0);
    }
}

static void mon_render(PspExpContext *ctx, uint64_t now)
{
    int x;
    char s[96];
    time_t t = time(NULL);
    struct tm lt;
    (void)now;
    gfx_frame(ctx);
    pspexp_begin_frame(ctx);
    pspexp_set_clip(ctx, 0, 0, UI_SCREEN_W, UI_SCREEN_H);
    gfx_backdrop();
    gfx_icon("activity", 16, 12, 8, UI_ACCENT, 255);
    gfx_text(FONT_TITLE, 34, 5, "System Monitor", UI_TEXT, 255, 0);
    {   /* the pages as tabs */
        const char *tab[2] = { "Overview", "Programs" };
        int i, tx = 190;
        for (i = 0; i < 2; i++) {
            int w = gfx_text_width(FONT_SMALL, tab[i]) + 18, on = i == g_page;
            if (on) gfx_nine(NINE_R6, tx, 7, w, 18, UI_ACCENT, 200);
            gfx_text(FONT_SMALL, tx + 9, 8, tab[i], on ? 0xffffff : UI_SUB, 255, 0);
            tx += w + 4;
        }
    }
    localtime_r(&t, &lt);
    strftime(s, sizeof(s), "%H:%M", &lt);
    gfx_text(FONT_BODY, UI_SCREEN_W - 12 - gfx_text_width(FONT_BODY, s), 7, s, UI_SUB, 255, 0);
    if (g_page == PAGE_OVERVIEW) draw_overview(ctx);
    else draw_programs();
    x = 12;
    if (g_msg[0]) gfx_text(FONT_SMALL, x, UI_HINT_Y - 1, g_msg, 0xffc766, 255, UI_SCREEN_W - 24);
    else {
        x = ui_hint_cap(x, "L/R", g_page == PAGE_OVERVIEW ? "Programs" : "Overview", UI_SUB);
        if (g_page == PAGE_PROGRAMS) {
            x = ui_hint_btn(x, 'x', "End");
            x = ui_hint_btn(x, 's', g_sort_mem ? "By CPU" : "By memory");
        }
        x = ui_hint_btn(x, 'o', "Close");
    }
    pspexp_present(ctx);
    g_dirty = 0;
}

static void mon_state(char *out, size_t n)
{
    size_t l = strlen(out);
    if (l < n) l += (size_t)snprintf(out + l, n - l, "monitor.page=%s\nmonitor.cpu=%.0f\nmonitor.cores=%d\nmonitor.mem_total=%llu\nmonitor.mem_avail=%llu\nmonitor.temp_mc=%d\nmonitor.programs=%d\nmonitor.selected=%s\nmonitor.confirm=%d\nmonitor.message=%s\n",
                                     g_page ? "programs" : "overview", g_s.cpu, g_s.ncpu, (unsigned long long)g_s.mem_total, (unsigned long long)g_s.mem_avail,
                                     g_s.temp_mc, g_s.nproc, g_s.nproc ? g_s.proc[g_sel].name : "", g_confirm, g_msg);
}

const AppView MONITOR_APP = { "monitor", mon_open, mon_input, mon_needs_frame, mon_render, mon_take_exit, mon_state };
