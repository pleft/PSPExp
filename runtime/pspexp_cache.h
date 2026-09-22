/*
 * Two-tier resource cache (section 6.3). Shared by every executor so that
 * SDL, Null and PSP account for memory identically. Placement policy for
 * v0.1 is deliberately naive: VRAM if it fits, else main RAM, else fail.
 */
#ifndef PSPEXP_CACHE_H
#define PSPEXP_CACHE_H

#include "../protocol/pspexp_batch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSPEXP_CACHE_MAX 1024

enum PspExpResourceKind {
    PSPEXP_KIND_TEXTURE = 0,
    PSPEXP_KIND_PALETTE = 1
};

typedef struct PspExpCacheEntry {
    uint16_t id;
    uint8_t  used;
    uint8_t  tier;          /* PspExpTier */
    uint8_t  format;        /* texture format, or palette entry format */
    uint8_t  layout;
    uint8_t  priority;
    uint8_t  kind;          /* PspExpResourceKind */
    uint16_t width, height; /* palette: width = entry count */
    uint32_t bytes;         /* bytes charged to the tier */
    uint16_t frame_count;
    uint16_t palette;       /* texture: palette handle for T4/T8 */
    PspExpFrame *frames;    /* owned copy of the frame table */
    void    *user;          /* executor-specific (SDL_Texture*, VRAM pointer, ...) */
} PspExpCacheEntry;

typedef struct PspExpCache {
    PspExpCacheEntry entries[PSPEXP_CACHE_MAX];
    uint32_t vram_budget, ram_budget;
    uint32_t vram_used, ram_used;
    uint16_t vram_count, ram_count;
} PspExpCache;

void pspexp_cache_init(PspExpCache *c, uint32_t vram_budget, uint32_t ram_budget);
PspExpCacheEntry *pspexp_cache_find(PspExpCache *c, uint16_t id);
/* Allocates a slot and a tier for the upload and copies its frame table.
 * Returns NULL and sets *reason (PSPEXP_RERR_*) on failure. */
PspExpCacheEntry *pspexp_cache_insert(PspExpCache *c, const PspExpTextureUpload *t,
                                      const PspExpFrame *frames, int *reason);
/* Palettes always live in main RAM. */
PspExpCacheEntry *pspexp_cache_insert_palette(PspExpCache *c, const PspExpPaletteUpload *p,
                                              uint32_t bytes, int *reason);
void pspexp_cache_remove(PspExpCache *c, PspExpCacheEntry *e);
/* Move an entry's accounting to another tier (the executor found VRAM
 * fragmented). -1 if the target tier's budget cannot take it. */
int  pspexp_cache_retier(PspExpCache *c, PspExpCacheEntry *e, uint8_t tier);
void pspexp_cache_status(const PspExpCache *c, PspExpMemoryStatus *out);

#ifdef __cplusplus
}
#endif
#endif /* PSPEXP_CACHE_H */
