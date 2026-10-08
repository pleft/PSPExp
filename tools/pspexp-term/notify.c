/*
 * Notifications (notify.h): the list, its file, the banner queue, the hint,
 * and the notification centre (Apps' NOTIFY_APP, opened from the launcher's
 * bell).
 *
 *   up / down      a notification      cross    go where it is about
 *   square         remove it           START    clear all (pressed twice)
 *   circle         close
 */
#include "notify.h"
#include "appview.h"
#include "launcher_gfx.h"
#include "launcher_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>
#include <time.h>

#define NOTE_MAX 50
#define QUEUE_MAX 8
#define SUMMARY_ID 0xffffffffu   /* the banner "N while you played": not in the list */

typedef struct Note {
    unsigned id;
    int level, count, unread, pct;   /* pct: a job's progress 0-100, -1 for none */
    int fresh;                       /* unread when the centre was opened: marked there */
    time_t at;
    char icon[32], title[96], detail[160], action[48], key[48];
} Note;

static Note g_n[NOTE_MAX];           /* newest first */
static int g_nn;
static unsigned g_next_id = 1;
static int g_save_due;
static uint64_t g_save_at;

static unsigned g_queue[QUEUE_MAX];  /* banners waiting: ids */
static int g_qn;
static Note g_cur;                   /* the banner shown: a copy (its entry may change or go) */
static int g_cur_on;
static uint64_t g_cur_at, g_cur_until, g_last_draw;
static float g_cur_a;
static int g_quiet, g_held;
static char g_hint[160], g_hint_icon[32];
static uint64_t g_hint_at, g_hint_until;
static float g_hint_a;
static int g_moving;                 /* a marquee scrolled in the last frame */
static char g_action[48];

/* ---- the list and its file ---------------------------------------------------------------- */

static void copy_clean(char *dst, size_t n, const char *src)
{
    size_t i;
    if (!src) src = "";
    for (i = 0; i + 1 < n && src[i]; i++) dst[i] = src[i] == '\t' || src[i] == '\n' || src[i] == '\r' ? ' ' : src[i];
    dst[i] = 0;
}

static void file_path(char *p, size_t n)
{
    const char *home = getenv("HOME");
    snprintf(p, n, "%s/.config/pspexp/notifications", home && *home ? home : "/tmp");
}

static void save(void)
{
    char p[512], t[520];
    FILE *f;
    int i;
    file_path(p, sizeof(p));
    snprintf(t, sizeof(t), "%s.new", p);
    {   /* ~/.config, then ~/.config/pspexp: a fresh home has neither */
        char d[512];
        snprintf(d, sizeof(d), "%s", p); *strrchr(d, '/') = 0;
        if (mkdir(d, 0755) != 0) { char *sl = strrchr(d, '/'); if (sl) { *sl = 0; mkdir(d, 0755); *sl = '/'; } mkdir(d, 0755); }
    }
    if (!(f = fopen(t, "w"))) return;
    for (i = g_nn - 1; i >= 0; i--) {   /* oldest first: loading adds each at the top */
        const Note *e = &g_n[i];
        fprintf(f, "%u\t%d\t%lld\t%d\t%d\t%d\t%s\t%s\t%s\t%s\t%s\n", e->id, e->level, (long long)e->at, e->count, e->unread,
                e->pct, e->icon, e->title, e->detail, e->action, e->key);
    }
    if (fclose(f) == 0) rename(t, p);
}

static void field(char **s, char *out, size_t n)
{
    char *t = *s ? strchr(*s, '\t') : NULL;
    if (t) *t = 0;
    snprintf(out, n, "%s", *s ? *s : "");
    *s = t ? t + 1 : NULL;
}

void notify_init(void)
{
    char p[512], line[700];
    FILE *f;
    file_path(p, sizeof(p));
    g_nn = 0;
    if (!(f = fopen(p, "r"))) return;
    while (fgets(line, sizeof(line), f)) {
        Note e;
        char num[32], *s = line;
        long long at = 0;
        line[strcspn(line, "\n")] = 0;
        memset(&e, 0, sizeof(e));
        field(&s, num, sizeof(num)); e.id = (unsigned)strtoul(num, NULL, 10);
        field(&s, num, sizeof(num)); e.level = atoi(num);
        field(&s, num, sizeof(num)); at = atoll(num); e.at = (time_t)at;
        field(&s, num, sizeof(num)); e.count = atoi(num);
        field(&s, num, sizeof(num)); e.unread = atoi(num);
        field(&s, num, sizeof(num)); e.pct = atoi(num);
        field(&s, e.icon, sizeof(e.icon)); field(&s, e.title, sizeof(e.title)); field(&s, e.detail, sizeof(e.detail));
        field(&s, e.action, sizeof(e.action)); field(&s, e.key, sizeof(e.key));
        if (!e.id || !e.title[0] || e.level <= NOTE_HINT || e.level > NOTE_WARN) continue;
        if (e.pct >= 0 && e.pct < 100) e.pct = -1;   /* a job the console's restart left unfinished */
        if (g_nn == NOTE_MAX) g_nn--;
        memmove(&g_n[1], &g_n[0], sizeof(Note) * (size_t)g_nn);
        g_n[0] = e; g_nn++;
        if (e.id >= g_next_id) g_next_id = e.id + 1;
    }
    fclose(f);
}

static int find_key(const char *key)
{
    int i;
    if (!key || !*key) return -1;
    for (i = 0; i < g_nn; i++) if (!strcmp(g_n[i].key, key)) return i;
    return -1;
}

static int find_id(unsigned id)
{
    int i;
    for (i = 0; i < g_nn; i++) if (g_n[i].id == id) return i;
    return -1;
}

static void remove_at(int i)
{
    memmove(&g_n[i], &g_n[i + 1], sizeof(Note) * (size_t)(g_nn - i - 1));
    g_nn--;
    g_save_due = 1;
}

/* how long a banner stays: longer for what matters, and for a long text (it scrolls) */
static uint64_t banner_time(const Note *e)
{
    uint64_t t = e->level == NOTE_WARN ? 6000000u : e->level == NOTE_ATTN || e->level == NOTE_FAIL ? 5000000u : e->level == NOTE_DONE ? 3500000u : 3000000u;
    if (strlen(e->title) + strlen(e->detail) > 70) t += 2000000u;
    return t;
}

static void banner_push(unsigned id)
{
    int i;
    if (g_quiet) { g_held++; return; }
    if (g_cur_on && g_cur.id == id) {   /* the one shown changed (a job's progress, a repeat): shown as it is now */
        int k = find_id(id);
        if (k >= 0) { int pct = g_cur.pct; g_cur = g_n[k]; if (g_n[k].pct < 0) g_cur.pct = pct < 0 ? -1 : g_n[k].pct; }
        g_cur_until = pspexp_time_us() + banner_time(&g_cur);
        return;
    }
    for (i = 0; i < g_qn; i++) if (g_queue[i] == id) return;
    if (g_qn == QUEUE_MAX) { memmove(&g_queue[0], &g_queue[1], sizeof(unsigned) * (QUEUE_MAX - 1)); g_qn--; }   /* the oldest waiting goes */
    g_queue[g_qn++] = id;
}

unsigned notify(int level, const char *icon, const char *title, const char *detail, const char *action, const char *key)
{
    Note e;
    int i;
    if (level == NOTE_HINT) { notify_hint(icon, title); return 0; }
    if (level < NOTE_INFO || level > NOTE_WARN || !title || !*title) return 0;
    i = find_key(key);
    if (i >= 0) { e = g_n[i]; remove_at(i); e.count++; }
    else { memset(&e, 0, sizeof(e)); e.id = g_next_id++; e.count = 1; }
    e.level = level; e.unread = 1; e.pct = -1; e.at = time(NULL);
    copy_clean(e.icon, sizeof(e.icon), icon && *icon ? icon : level == NOTE_WARN ? "alert-triangle" : level == NOTE_FAIL ? "circle-x" : level == NOTE_DONE ? "check-circle" : "info");
    copy_clean(e.title, sizeof(e.title), title);
    copy_clean(e.detail, sizeof(e.detail), detail);
    copy_clean(e.action, sizeof(e.action), action);
    copy_clean(e.key, sizeof(e.key), key);
    if (g_nn == NOTE_MAX) g_nn--;
    memmove(&g_n[1], &g_n[0], sizeof(Note) * (size_t)g_nn);
    g_n[0] = e; g_nn++;
    g_save_due = 1;
    fprintf(stderr, "[pspexp-term] notification: %s%s%s\n", e.title, e.detail[0] ? ": " : "", e.detail);
    banner_push(e.id);
    return e.id;
}

void notify_job(const char *key, const char *icon, const char *title, int pct)
{
    int i = find_key(key);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (i < 0 || g_n[i].pct < 0) { notify(NOTE_INFO, icon, title, NULL, NULL, key); i = find_key(key); if (i < 0) return; }
    else if (title && strcmp(g_n[i].title, title)) copy_clean(g_n[i].title, sizeof(g_n[i].title), title);
    g_n[i].pct = pct;
    g_save_due = 1;
    if (g_cur_on && g_cur.id == g_n[i].id) { g_cur.pct = pct; copy_clean(g_cur.title, sizeof(g_cur.title), g_n[i].title); g_cur_until = pspexp_time_us() + 4000000u; }
}

void notify_hint(const char *icon, const char *text)
{
    uint64_t now = pspexp_time_us();
    copy_clean(g_hint, sizeof(g_hint), text);
    copy_clean(g_hint_icon, sizeof(g_hint_icon), icon);
    g_hint_at = now;
    g_hint_until = now + (strlen(g_hint) > 60 ? 4600000u : 2600000u);
}

int notify_level(const char *name)
{
    static const char *names[] = { "hint", "info", "done", "fail", "attention", "warning" };
    int i;
    for (i = 0; i <= NOTE_WARN; i++) if (name && !strcmp(name, names[i])) return i;
    return -1;
}

int notify_unread(void)
{
    int i, n = 0;
    for (i = 0; i < g_nn; i++) n += g_n[i].unread != 0;
    return n;
}

int notify_take_action(char *out, size_t n)
{
    if (!g_action[0]) return 0;
    snprintf(out, n, "%s", g_action);
    g_action[0] = 0;
    return 1;
}

void notify_set_quiet(int quiet)
{
    if (quiet == g_quiet) return;
    g_quiet = quiet;
    if (quiet) { g_held = 0; g_qn = 0; g_cur_on = 0; g_cur_a = 0; return; }
    if (g_held == 1 && g_nn) banner_push(g_n[0].id);
    else if (g_held > 1) {   /* "3 while you played": shown, not kept */
        memset(&g_cur, 0, sizeof(g_cur));
        g_cur.id = SUMMARY_ID; g_cur.level = NOTE_INFO; g_cur.pct = -1;
        snprintf(g_cur.icon, sizeof(g_cur.icon), "bell");
        snprintf(g_cur.title, sizeof(g_cur.title), "%d notifications while you played", g_held);
        snprintf(g_cur.detail, sizeof(g_cur.detail), "The bell on the home screen lists them");
        g_cur_on = 1; g_cur_a = 0;
        g_cur_at = pspexp_time_us(); g_cur_until = g_cur_at + 4000000u;
    }
    g_held = 0;
}

void notify_idle(uint64_t now)
{
    if (g_save_due && now - g_save_at > 1000000u) { g_save_due = 0; g_save_at = now; save(); }
}

int notify_tick(uint64_t now)
{
    notify_idle(now);
    if (g_quiet) return 0;
    if (!g_cur_on && g_qn) {   /* the next banner */
        int k;
        unsigned id = g_queue[0];
        memmove(&g_queue[0], &g_queue[1], sizeof(unsigned) * (size_t)(g_qn - 1));
        g_qn--;
        if ((k = find_id(id)) >= 0) {
            g_cur = g_n[k]; g_cur_on = 1; g_cur_a = 0;
            g_cur_at = now; g_cur_until = now + banner_time(&g_cur);
        }
    }
    if (g_cur_on && now > g_cur_until && g_cur.pct >= 0 && g_cur.pct < 100) g_cur_until = now + 500000u;   /* a job: until it ends (or 4 s without news) */
    if (g_cur_on && now > g_cur_until && g_cur_a <= 0.01f) { g_cur_on = 0; g_cur_until = 0; }
    if (g_hint[0] && now > g_hint_until && g_hint_a <= 0.01f) g_hint[0] = 0;
    return g_cur_on || g_qn || g_hint[0];
}

static float ease(float v, float to, float speed, float dt)
{
    v += (to - v) * (1.0f - expf(-speed * dt));
    if (v > to - 0.01f && v < to + 0.01f) v = to;
    return v;
}

static uint32_t level_colour(int level)
{
    return level == NOTE_WARN || level == NOTE_FAIL ? 0xf87171 : level == NOTE_ATTN ? 0xffc766 : level == NOTE_DONE ? 0x4ade80 : UI_ACCENT;
}

static int marquee(int font, int x, int y, const char *s, uint32_t rgb, int a, int w, uint64_t t)
{
    int moving = 0, r = gfx_text_marquee(font, x, y, s, rgb, a, w, t, &moving);
    g_moving |= moving;
    return r;
}

void notify_draw(uint64_t now)
{
    float dt = g_last_draw ? (float)(now - g_last_draw) / 1e6f : 0.016f;
    if (dt > 0.1f) dt = 0.1f;
    g_last_draw = now;
    g_moving = 0;
    gfx_set_clip(0, 0, UI_SCREEN_W, UI_SCREEN_H);
    if (g_cur_on) {   /* the banner, at the top: slides down in, up out */
        int two = g_cur.detail[0] != 0, h = two ? 44 : 30, w = 400, x = (UI_SCREEN_W - w) / 2, y, a;
        uint32_t col = level_colour(g_cur.level);
        g_cur_a = ease(g_cur_a, now < g_cur_until ? 1.0f : 0.0f, 12.0f, dt);
        a = (int)(g_cur_a * 255);
        y = 4 - (int)((1.0f - g_cur_a) * (float)(h + 8));
        gfx_shadow(NINE_SHADOW10, x, y + 3, w, h, 0x000000, a * 180 / 255);
        gfx_nine(NINE_R10, x, y, w, h, 0x1a2744, a * 250 / 255);
        gfx_rect(x + 6, y + 7, 3, h - 14, col, a);
        gfx_icon(g_cur.icon, 18, x + 16, y + (h - 18) / 2, col, a);
        {
            int tx = x + 44, tw = w - 44 - 12;
            char cnt[16] = "";
            if (g_cur.count > 1) { snprintf(cnt, sizeof(cnt), "x%d", g_cur.count); tw -= gfx_text_width(FONT_BADGE, cnt) + 8; }
            marquee(FONT_BODY, tx, y + (two ? 5 : 7), g_cur.title, UI_TEXT, a, tw, now - g_cur_at);
            if (two) marquee(FONT_SMALL, tx, y + 24, g_cur.detail, UI_SUB, a, tw, now - g_cur_at);
            if (cnt[0]) gfx_text(FONT_BADGE, x + w - 12 - gfx_text_width(FONT_BADGE, cnt), y + 8, cnt, UI_MUTED, a, 0);
        }
        if (g_cur.pct >= 0) {   /* a job's bar along the bottom edge */
            int bw = (w - 32) * g_cur.pct / 100;
            gfx_rect(x + 16, y + h - 5, w - 32, 2, 0xffffff, a * 40 / 255);
            if (bw > 0) gfx_rect(x + 16, y + h - 5, bw, 2, col, a);
        }
    }
    if (g_hint[0]) {   /* the hint, above the hint row (the old toast's place) */
        int ic = g_hint_icon[0] ? 20 : 0, w = gfx_text_width(FONT_BODY, g_hint) + 28 + ic, x, y = UI_HINT_Y - 36, a;
        g_hint_a = ease(g_hint_a, now < g_hint_until ? 1.0f : 0.0f, 14.0f, dt);
        a = (int)(g_hint_a * 255);
        if (w > UI_SCREEN_W - 40) w = UI_SCREEN_W - 40;
        x = (UI_SCREEN_W - w) / 2;
        y += (int)((1.0f - g_hint_a) * 8);
        gfx_shadow(NINE_SHADOW10, x, y + 3, w, 26, 0x000000, a * 170 / 255);
        gfx_nine(NINE_R10, x, y, w, 26, 0x1e2c4a, a * 245 / 255);
        if (ic) gfx_icon(g_hint_icon, 14, x + 12, y + 6, UI_ACCENT, a);
        marquee(FONT_BODY, x + 14 + ic, y + (26 - gfx_line_height(FONT_BODY)) / 2, g_hint, UI_TEXT, a, w - 28 - ic, now - g_hint_at);
    }
}

const char *notify_showing(void)
{
    uint64_t now = pspexp_time_us();
    if (g_cur_on && now < g_cur_until) return g_cur.title;
    if (g_hint[0] && now < g_hint_until) return g_hint;
    return "";
}

void notify_state(char *out, size_t n)
{
    size_t l = strlen(out);
    if (l < n) snprintf(out + l, n - l, "notify.count=%d\nnotify.unread=%d\nnotify.latest=%s\nnotify.banner=%s\nnotify.hint=%s\nnotify.waiting=%d\nnotify.quiet=%d\n",
                        g_nn, notify_unread(), g_nn ? g_n[0].title : "", g_cur_on ? g_cur.title : "", g_hint, g_qn, g_quiet);
}

/* ---- the notification centre --------------------------------------------------------------- */

static int c_sel, c_top, c_exit, c_dirty = 1, c_clear;
static uint64_t c_clear_until;
#define ROWS 5
#define ROW_H 40
#define LIST_Y 34

static void c_open(void)
{
    int i;
    for (i = 0; i < g_nn; i++) { g_n[i].fresh = g_n[i].unread; g_n[i].unread = 0; }
    g_save_due = 1;
    g_qn = 0;   /* the banners waiting or shown are all in the list below: gone */
    if (g_cur_on) g_cur_until = 0;
    c_sel = 0; c_top = 0; c_exit = 0; c_clear = 0; c_dirty = 1;
}

static void c_input(uint32_t b, uint32_t pressed, int up, int down, int left, int right, uint64_t now)
{
    (void)b; (void)left; (void)right;
    if (up && c_sel > 0) { c_sel--; c_dirty = 1; }
    if (down && c_sel < g_nn - 1) { c_sel++; c_dirty = 1; }
    if (c_sel < c_top) c_top = c_sel;
    if (c_sel >= c_top + ROWS) c_top = c_sel - ROWS + 1;
    if (pressed & PSPEXP_BUTTON_CIRCLE) { c_exit = 1; return; }
    if ((pressed & PSPEXP_BUTTON_CROSS) && c_sel < g_nn && g_n[c_sel].action[0]) {
        snprintf(g_action, sizeof(g_action), "%s", g_n[c_sel].action);
        c_exit = 1;
        return;
    }
    if ((pressed & PSPEXP_BUTTON_SQUARE) && c_sel < g_nn) {
        remove_at(c_sel);
        if (c_sel >= g_nn && c_sel > 0) c_sel--;
        if (c_top > 0 && c_top + ROWS > g_nn) c_top = g_nn > ROWS ? g_nn - ROWS : 0;
        c_dirty = 1;
    }
    if ((pressed & PSPEXP_BUTTON_START) && g_nn) {
        if (c_clear && now < c_clear_until) { g_nn = 0; g_save_due = 1; c_sel = c_top = 0; c_clear = 0; }
        else { c_clear = 1; c_clear_until = now + 3000000u; notify_hint("bell-off", "Press START again to clear them all"); }
        c_dirty = 1;
    }
}

static int c_needs_frame(uint64_t now)
{
    static int minute = -1;
    int m = (int)(time(NULL) / 60);
    (void)now;
    if (m != minute) { minute = m; c_dirty = 1; }   /* the times ("5 min") move on */
    return c_dirty;
}

static void ago(time_t at, char *out, size_t n)
{
    long d = (long)(time(NULL) - at);
    if (d < 60) snprintf(out, n, "now");
    else if (d < 3600) snprintf(out, n, "%ld min", d / 60);
    else if (d < 86400) snprintf(out, n, "%ld h", d / 3600);
    else { struct tm t; localtime_r(&at, &t); strftime(out, n, "%d %b", &t); }
}

static void c_render(PspExpContext *ctx, uint64_t now)
{
    int i, x;
    char s[64];
    gfx_frame(ctx);
    pspexp_begin_frame(ctx);
    pspexp_set_clip(ctx, 0, 0, UI_SCREEN_W, UI_SCREEN_H);
    gfx_backdrop();
    gfx_icon("bell", 16, 12, 8, UI_ACCENT, 255);
    gfx_text(FONT_TITLE, 34, 5, "Notifications", UI_TEXT, 255, 0);
    if (g_nn) { snprintf(s, sizeof(s), "%d", g_nn); gfx_text(FONT_SMALL, UI_SCREEN_W - 12 - gfx_text_width(FONT_SMALL, s), 9, s, UI_MUTED, 255, 0); }
    if (!g_nn) {
        gfx_icon("bell-off", 32, (UI_SCREEN_W - 32) / 2, 96, UI_MUTED, 255);
        snprintf(s, sizeof(s), "No notifications");
        gfx_text(FONT_BODY, (UI_SCREEN_W - gfx_text_width(FONT_BODY, s)) / 2, 138, s, UI_SUB, 255, 0);
    }
    for (i = c_top; i < g_nn && i < c_top + ROWS; i++) {
        const Note *e = &g_n[i];
        int y = LIST_Y + (i - c_top) * ROW_H, on = i == c_sel, tw;
        uint32_t col = level_colour(e->level);
        if (on) { gfx_shadow(NINE_SHADOW10, 10, y, UI_SCREEN_W - 20, ROW_H - 4, 0x40a0ff, 150); gfx_nine(NINE_R10, 10, y, UI_SCREEN_W - 20, ROW_H - 4, 0x1e3a6e, 235); }
        gfx_icon(e->icon, 18, 22, y + 9, col, 255);
        ago(e->at, s, sizeof(s));
        tw = gfx_text_width(FONT_SMALL, s);
        gfx_text(FONT_SMALL, UI_SCREEN_W - 22 - tw, y + 4, s, UI_MUTED, 255, 0);
        if (e->fresh) gfx_rect(UI_SCREEN_W - 22 - tw - 10, y + 9, 5, 5, UI_ACCENT, 255);
        {
            int w = UI_SCREEN_W - 22 - tw - 16 - 52;
            if (e->count > 1) { char c[16]; snprintf(c, sizeof(c), "x%d", e->count); gfx_text(FONT_BADGE, UI_SCREEN_W - 22 - gfx_text_width(FONT_BADGE, c), y + 21, c, UI_MUTED, 255, 0); }
            if (on) marquee(FONT_BODY, 50, y + 2, e->title, UI_TEXT, 255, w, now);
            else gfx_text(FONT_BODY, 50, y + 2, e->title, UI_SUB, 255, w);
            if (e->pct >= 0 && e->pct < 100) snprintf(s, sizeof(s), "%d%%", e->pct);
            gfx_text(FONT_SMALL, 50, y + 20, e->pct >= 0 && e->pct < 100 ? s : e->detail, UI_MUTED, 255, UI_SCREEN_W - 50 - 46);
        }
    }
    if (g_nn > ROWS) {   /* where in the list */
        int track = ROWS * ROW_H - 8, len = track * ROWS / g_nn, pos = (track - len) * c_top / (g_nn - ROWS);
        gfx_rect(UI_SCREEN_W - 6, LIST_Y + pos, 2, len, 0xffffff, 60);
    }
    x = 12;
    if (c_sel < g_nn && g_n[c_sel].action[0]) x = ui_hint_btn(x, 'x', "Go there");
    if (g_nn) { x = ui_hint_btn(x, 's', "Remove"); x = ui_hint_cap(x, "START", "Clear all", UI_SUB); }
    ui_hint_btn(x, 'o', "Close");
    notify_draw(now);
    pspexp_present(ctx);
    c_dirty = g_moving;
}

static int c_take_exit(void) { int r = c_exit; c_exit = 0; return r; }

static void c_state(char *out, size_t n)
{
    size_t l = strlen(out);
    if (l < n) snprintf(out + l, n - l, "notifications.selected=%s\nnotifications.action=%s\n",
                        c_sel < g_nn ? g_n[c_sel].title : "", c_sel < g_nn ? g_n[c_sel].action : "");
}

const AppView NOTIFY_APP = { "notifications", c_open, c_input, c_needs_frame, c_render, c_take_exit, c_state };
