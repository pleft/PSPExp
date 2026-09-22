/*
 * Null executor (section 11): consumes and validates the command stream
 * without rendering. Exposed to libPSPExp as an in-process transport.
 */
#ifndef PSPEXP_NULL_H
#define PSPEXP_NULL_H

#include "../libpspexp/pspexp_transport.h"
#include "../../runtime/pspexp_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PspExpNullExecutor PspExpNullExecutor;

typedef struct PspExpNullStats {
    uint32_t textures_created, textures_destroyed;
    uint32_t frames, clips, sprite_draws, sprites, rects;
    uint32_t vertices_expanded;
    uint32_t palettes_created, indexed_draws, alpha_draws, flipped_draws, lines;
} PspExpNullStats;

/* `cfg` may be NULL for defaults. */
PspExpNullExecutor *pspexp_null_create(const PspExpRuntimeConfig *cfg);
void pspexp_null_destroy(PspExpNullExecutor *n);
PspExpTransport *pspexp_null_transport(PspExpNullExecutor *n);
PspExpRuntime   *pspexp_null_runtime(PspExpNullExecutor *n);
const PspExpNullStats *pspexp_null_stats(const PspExpNullExecutor *n);
/* Simulate a cable pull and re-plug: the runtime drops everything and
 * sends a fresh HELLO. */
void pspexp_null_simulate_reconnect(PspExpNullExecutor *n);
/* Inject a controller sample as if sampled on the PSP. */
void pspexp_null_push_input(PspExpNullExecutor *n, uint32_t buttons, uint8_t analog_x, uint8_t analog_y);
/* Audio: the Null executor keeps the mixer the runtime started and renders
 * it only when asked, so tests can inspect the output. Returns frames
 * rendered (0 when audio is not active). */
uint32_t pspexp_null_render_audio(PspExpNullExecutor *n, int16_t *stereo, uint32_t frames);
/* When non-zero, present() sleeps to emulate a 60 Hz display (default 0). */
void pspexp_null_set_paced(PspExpNullExecutor *n, int paced);

#ifdef __cplusplus
}
#endif
#endif /* PSPEXP_NULL_H */
