/*
 * A built-in app of the console, shown instead of the launcher (Apps > ...):
 * the system monitor (monitor.c) and the package manager (store.c). The file
 * manager came first and is wired in on its own (files.c); these share this.
 * They draw with the launcher's graphics (launcher_gfx), which must be up.
 */
#ifndef PSPEXP_TERM_APPVIEW_H
#define PSPEXP_TERM_APPVIEW_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "../../host/libpspexp/pspexp.h"

typedef struct AppView {
    const char *id;                  /* "monitor", "store": what the launcher and the control port call it */
    void (*open)(void);
    void (*input)(uint32_t b, uint32_t pressed, int up, int down, int left, int right, uint64_t now);
    int  (*needs_frame)(uint64_t now);
    void (*render)(PspExpContext *ctx, uint64_t now);
    int  (*take_exit)(void);         /* 1 once when the user closed it */
    void (*state)(char *out, size_t n);   /* "<id>.key=value" lines for the control port */
} AppView;

extern const AppView MONITOR_APP, STORE_APP;
const AppView *app_find(const char *id);

/* the monitor may end the program the launcher started (on boards where
 * everything runs as root, that is all it may end) */
void monitor_set_program(pid_t pid);

#endif
