/*
 * Notifications: what the console has to tell the player, wherever they are.
 * One service for every part of it (the launcher, the apps, Files, the
 * terminal, the board's watchers, programs through the control port and
 * pspexp-notify). A notification shows as a banner at the top of whatever is
 * on screen, one at a time, the others waiting their turn; it is kept, the
 * last 50 across restarts (~/.config/pspexp/notifications), in the
 * notification centre (NOTIFY_APP, the bell in the launcher's top bar), with
 * an action to go where it is about. While a game has the PSP, banners are
 * held and summed up when the launcher is back.
 *
 * Hints are not notifications: the one-line answer to what was just pressed
 * ("Press X again to power off", "Added to favourites"), at the bottom of the
 * screen for a moment and not kept.
 */
#ifndef PSPEXP_TERM_NOTIFY_H
#define PSPEXP_TERM_NOTIFY_H

#include <stddef.h>
#include <stdint.h>

enum {
    NOTE_HINT,   /* not kept: the answer to a press */
    NOTE_INFO,   /* happened, nothing to do */
    NOTE_DONE,   /* a job that finished */
    NOTE_FAIL,   /* a job or a program that failed */
    NOTE_ATTN,   /* something to act on (an update, a restart) */
    NOTE_WARN,   /* a problem coming (battery, storage) */
};

/* the history from its file; once, at start */
void notify_init(void);
/* post one. icon: a launcher icon name; detail, action, key may be NULL. key: a later
 * notification with the same key replaces this one (counted, moved to the top, unread
 * again) instead of adding another. action: where cross in the centre goes:
 * "app:<id>" (an app: store, music, monitor), "files", "category:<name>" (a launcher
 * section: Settings). Returns its id (0 for a hint) */
unsigned notify(int level, const char *icon, const char *title, const char *detail, const char *action, const char *key);
/* a job's progress (0-100) under its key: the banner's bar; the job ends with a
 * notify(NOTE_DONE or NOTE_FAIL, ..., key) */
void notify_job(const char *key, const char *icon, const char *title, int pct);
void notify_hint(const char *icon, const char *text);
/* "info", "done", "fail", "attention", "warning" (or "hint"): NOTE_*, -1 when unknown */
int  notify_level(const char *name);

/* each main-loop turn: 1 while a banner or a hint is on screen or moving (the view
 * shown redraws: they draw only on change) */
int  notify_tick(uint64_t now);
/* with no PSP connected: the list saved, the banners left waiting for it */
void notify_idle(uint64_t now);
/* the banner and the hint over the view just drawn, before its present (launcher_gfx) */
void notify_draw(uint64_t now);
/* a program has the PSP: banners held; when it ends, what came meanwhile is summed up */
void notify_set_quiet(int quiet);

int  notify_unread(void);
/* the action chosen in the centre, once (main.c goes there) */
int  notify_take_action(char *out, size_t n);
/* "notify.*" lines for the control port; and what the banner or hint says now */
void notify_state(char *out, size_t n);
const char *notify_showing(void);

#endif
