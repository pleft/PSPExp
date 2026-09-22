/*
 * SDL executor (section 11): the reference renderer. Consumes the same
 * command stream as the PSP, enforces the same constraints (through the
 * shared runtime core) and shows what the application intended in a window.
 * Keyboard and game controller emulate the PSP controls.
 */
#ifndef PSPEXP_SDL_H
#define PSPEXP_SDL_H

#include "../libpspexp/pspexp_transport.h"
#include "../../runtime/pspexp_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PspExpSdlExecutor PspExpSdlExecutor;

typedef struct PspExpSdlOptions {
    int scale;        /* window scale factor (default 2) */
    int vsync;        /* default 1 */
    const char *title;
    /* 0 when the host application owns the SDL event loop (it must pump
     * events itself; the executor then only reads keyboard state and
     * never sees a window-close). Default 1. */
    int pump_events;
} PspExpSdlOptions;

void pspexp_sdl_options_default(PspExpSdlOptions *o);
PspExpSdlExecutor *pspexp_sdl_create(const PspExpSdlOptions *o, char *err, size_t errlen);
void pspexp_sdl_destroy(PspExpSdlExecutor *x);
PspExpTransport *pspexp_sdl_transport(PspExpSdlExecutor *x);
PspExpRuntime   *pspexp_sdl_runtime(PspExpSdlExecutor *x);
/* The window was closed (the transport then reports the link as gone). */
int pspexp_sdl_quit_requested(const PspExpSdlExecutor *x);

#ifdef __cplusplus
}
#endif
#endif
