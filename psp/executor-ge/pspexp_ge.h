/*
 * PSP GE executor: the runtime ops that turn expanded batches into GE
 * display lists (section 11, "PSP executor" implementation notes).
 */
#ifndef PSPEXP_GE_H
#define PSPEXP_GE_H

#include "../../runtime/pspexp_runtime.h"

#define PSPEXP_GE_LIST_WORDS   (64 * 1024)      /* 256 KiB display list */
#define PSPEXP_GE_RING_HALF    (1024 * 1024)    /* vertex ring, per frame: 24 B per sprite, ~43,000 sprites */
#define PSPEXP_GE_VRAM_BLOCKS  PSPEXP_CACHE_MAX

typedef struct PspExpGeVramBlock { uint32_t offset, size; uint8_t used; } PspExpGeVramBlock;

typedef struct PspExpGeStats {
    uint32_t frames, batches, dropped_batches, texture_changes, rects;
    uint32_t last_ge_us;          /* sceGuSync wait for the last frame */
    uint32_t last_vertex_bytes;
    uint32_t vram_alloc_failures; /* fragmentation: demoted to RAM */
} PspExpGeStats;

typedef struct PspExpGeExecutor {
    unsigned int list[PSPEXP_GE_LIST_WORDS] __attribute__((aligned(16)));
    unsigned char ring[2][PSPEXP_GE_RING_HALF] __attribute__((aligned(16)));
    int ring_cur;
    uint32_t ring_used;
    int in_frame;
    int tex_enabled;
    PspExpCacheEntry *cur_atlas;
    PspExpCacheEntry *cur_palette;
    PspExpGeVramBlock vram[PSPEXP_GE_VRAM_BLOCKS];
    PspExpRuntime *rt;
    PspExpGeStats stats;
    uint32_t displayed_offset;   /* VRAM offset of the framebuffer on screen */
} PspExpGeExecutor;

/* Copies the displayed framebuffer (480x272 RGB565) into `out` (row stride 480). */
void pspexp_ge_read_display(PspExpGeExecutor *ge, uint16_t *out);

/* Initialises the GU (framebuffers at VRAM 0 and 0x44000, no depth buffer). */
int  pspexp_ge_init(PspExpGeExecutor *ge);
/* Fills the executor ops; the caller sets ops->now_us and ops->log itself. */
void pspexp_ge_ops(PspExpGeExecutor *ge, PspExpRuntimeOps *ops);
/* Lets the executor re-tier textures when VRAM is fragmented. */
void pspexp_ge_bind(PspExpGeExecutor *ge, PspExpRuntime *rt);
void pspexp_ge_term(PspExpGeExecutor *ge);

#endif
