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
    uint32_t draws3d, dropped3d;  /* 3D draws, and those dropped for lack of vertex space */
} PspExpGeStats;

typedef struct PspExpGeExecutor {
    unsigned int list[PSPEXP_GE_LIST_WORDS] __attribute__((aligned(16)));
    unsigned char ring[2][PSPEXP_GE_RING_HALF] __attribute__((aligned(16)));
    int ring_cur;
    uint32_t ring_used;
    int in_frame;
    int osd_percent; uint32_t osd_until;   /* the volume bar (OSD_VOLUME) */
    int keyboard;                          /* the PSP's keyboard owns the screen: frames drawn, not shown */
    int tex_enabled;
    PspExpCacheEntry *cur_atlas;
    PspExpCacheEntry *cur_palette;
    PspExpGeVramBlock vram[PSPEXP_GE_VRAM_BLOCKS];
    PspExpRuntime *rt;
    PspExpGeStats stats;
    uint32_t displayed_offset;   /* VRAM offset of the framebuffer on screen */
    uint32_t tex_base;           /* VRAM offset where textures start: after the framebuffers (and depth buffer) */
    int16_t  clip_x, clip_y, clip_w, clip_h;   /* the 2D scissor, restored after 3D draws */
    /* 3D */
    int      depth_on;           /* the host asked for a depth buffer */
    int      mode3d;             /* the GE holds the 3D state (else the 2D one) */
    int      state_dirty;
    int      have_state;
    PspExp3DState st;
    PspExpCacheEntry *target;    /* the render target drawn into, NULL: the screen */
    /* PSPExp Capture inside PSPExp (psp/runtime/pspexp_capture_psp.c): at each frame's end the GE
     * copies the frame into cap_target's buffer (480x272 RGB 5650, before the volume bar and
     * the dot), cap_done once it has; rec_dot the dot's colour (RGB 5650, 0: none) */
    void *(*cap_target)(void *user);
    void (*cap_done)(void *user, uint32_t t);
    void *cap_user;
    uint16_t rec_dot;
} PspExpGeExecutor;

/* The hand-over spinner (PspExpDisconnect), between programs: a PSPEXP_SPINNER_SIZE square of
 * bg centred at (cx, cy) with eight dots round it, dot `phase` brightest (fg), drawn straight
 * into the frame on screen. Not inside a frame. */
void pspexp_ge_spinner(PspExpGeExecutor *ge, int cx, int cy, uint32_t bg, uint32_t fg, unsigned phase);

/* Copies the displayed framebuffer (480x272 RGB565) into `out` (row stride 480). */
void pspexp_ge_read_display(PspExpGeExecutor *ge, uint16_t *out);
/* One frame of the PSP's own keyboard (the system's on-screen keyboard, already
 * started): the screen cleared, the keyboard drawn over it, shown at the next
 * vertical blank (update: let it draw and take the buttons; only while it is
 * visible). Only between the host's frames (not while one is open). */
void pspexp_ge_keyboard_frame(PspExpGeExecutor *ge, int update);

/* Initialises the GU (framebuffers at VRAM 0 and 0x44000; a depth buffer at
 * 0x88000 when the host asks for one). */
int  pspexp_ge_init(PspExpGeExecutor *ge);
/* Fills the executor ops; the caller sets ops->now_us and ops->log itself. */
void pspexp_ge_ops(PspExpGeExecutor *ge, PspExpRuntimeOps *ops);
/* Lets the executor re-tier textures when VRAM is fragmented. */
void pspexp_ge_bind(PspExpGeExecutor *ge, PspExpRuntime *rt);
void pspexp_ge_term(PspExpGeExecutor *ge);

#endif
