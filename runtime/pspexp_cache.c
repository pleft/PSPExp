#include "pspexp_cache.h"
#include <stdlib.h>
#include <string.h>

void pspexp_cache_init(PspExpCache *c, uint32_t vram_budget, uint32_t ram_budget)
{
    memset(c, 0, sizeof(*c));
    c->vram_budget = vram_budget;
    c->ram_budget = ram_budget;
}

PspExpCacheEntry *pspexp_cache_find(PspExpCache *c, uint16_t id)
{
    int i;
    for (i = 0; i < PSPEXP_CACHE_MAX; i++)
        if (c->entries[i].used && c->entries[i].id == id) return &c->entries[i];
    return NULL;
}

PspExpCacheEntry *pspexp_cache_insert(PspExpCache *c, const PspExpTextureUpload *t,
                                      const PspExpFrame *frames, int *reason)
{
    int i;
    PspExpCacheEntry *e = NULL;
    uint8_t tier;

    if (pspexp_cache_find(c, t->id)) { *reason = PSPEXP_RERR_DUPLICATE_ID; return NULL; }
    for (i = 0; i < PSPEXP_CACHE_MAX; i++)
        if (!c->entries[i].used) { e = &c->entries[i]; break; }
    if (!e) { *reason = PSPEXP_RERR_TABLE_FULL; return NULL; }

    /* Low priority means "changes often" (streamed frames, per-frame texture
     * updates): keep it out of VRAM so the first-fit allocator there does not
     * fragment; the GE reads main RAM fine for a texture used once. */
    if (t->priority != PSPEXP_PRIORITY_LOW && c->vram_used + t->data_length <= c->vram_budget) tier = PSPEXP_TIER_VRAM;
    else if (c->ram_used + t->data_length <= c->ram_budget) tier = PSPEXP_TIER_RAM;
    else { *reason = PSPEXP_RERR_OUT_OF_MEMORY; return NULL; }

    memset(e, 0, sizeof(*e));
    if (t->frame_count) {
        e->frames = (PspExpFrame *)malloc((size_t)t->frame_count * sizeof(PspExpFrame));
        if (!e->frames) { *reason = PSPEXP_RERR_OUT_OF_MEMORY; return NULL; }
        memcpy(e->frames, frames, (size_t)t->frame_count * sizeof(PspExpFrame));
    }
    e->used = 1;
    e->id = t->id;
    e->tier = tier;
    e->format = t->format;
    e->layout = t->layout;
    e->priority = t->priority;
    e->width = t->width;
    e->height = t->height;
    e->bytes = t->data_length;
    e->frame_count = t->frame_count;
    e->palette = t->palette;
    e->kind = PSPEXP_KIND_TEXTURE;
    if (tier == PSPEXP_TIER_VRAM) { c->vram_used += e->bytes; c->vram_count++; }
    else { c->ram_used += e->bytes; c->ram_count++; }
    *reason = PSPEXP_RERR_NONE;
    return e;
}

PspExpCacheEntry *pspexp_cache_insert_palette(PspExpCache *c, const PspExpPaletteUpload *p,
                                              uint32_t bytes, int *reason)
{
    int i;
    PspExpCacheEntry *e = NULL;
    if (pspexp_cache_find(c, p->id)) { *reason = PSPEXP_RERR_DUPLICATE_ID; return NULL; }
    for (i = 0; i < PSPEXP_CACHE_MAX; i++)
        if (!c->entries[i].used) { e = &c->entries[i]; break; }
    if (!e) { *reason = PSPEXP_RERR_TABLE_FULL; return NULL; }
    if (c->ram_used + bytes > c->ram_budget) { *reason = PSPEXP_RERR_OUT_OF_MEMORY; return NULL; }
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->id = p->id;
    e->kind = PSPEXP_KIND_PALETTE;
    e->tier = PSPEXP_TIER_RAM;
    e->format = p->format;
    e->width = p->entries;
    e->bytes = bytes;
    c->ram_used += bytes; c->ram_count++;
    *reason = PSPEXP_RERR_NONE;
    return e;
}

PspExpCacheEntry *pspexp_cache_insert_sound(PspExpCache *c, const PspExpSoundUpload *snd,
                                            uint32_t bytes, int *reason)
{
    int i;
    PspExpCacheEntry *e = NULL;
    if (pspexp_cache_find(c, snd->id)) { *reason = PSPEXP_RERR_DUPLICATE_ID; return NULL; }
    for (i = 0; i < PSPEXP_CACHE_MAX; i++)
        if (!c->entries[i].used) { e = &c->entries[i]; break; }
    if (!e) { *reason = PSPEXP_RERR_TABLE_FULL; return NULL; }
    if (c->ram_used + bytes > c->ram_budget) { *reason = PSPEXP_RERR_OUT_OF_MEMORY; return NULL; }
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->id = snd->id;
    e->kind = PSPEXP_KIND_SOUND;
    e->tier = PSPEXP_TIER_RAM;
    e->width = snd->channels;
    e->bytes = bytes;
    c->ram_used += bytes; c->ram_count++;
    *reason = PSPEXP_RERR_NONE;
    return e;
}

PspExpCacheEntry *pspexp_cache_insert_mesh(PspExpCache *c, const PspExpMeshUpload *m, int *reason)
{
    int i;
    PspExpCacheEntry *e = NULL;
    if (pspexp_cache_find(c, m->id)) { *reason = PSPEXP_RERR_DUPLICATE_ID; return NULL; }
    for (i = 0; i < PSPEXP_CACHE_MAX; i++)
        if (!c->entries[i].used) { e = &c->entries[i]; break; }
    if (!e) { *reason = PSPEXP_RERR_TABLE_FULL; return NULL; }
    if (c->ram_used + m->data_length > c->ram_budget) { *reason = PSPEXP_RERR_OUT_OF_MEMORY; return NULL; }
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->id = m->id;
    e->kind = PSPEXP_KIND_MESH;
    e->tier = PSPEXP_TIER_RAM;
    e->format = m->kind;
    e->count = m->count;
    e->vertex_type = m->vertex_type;
    e->bytes = m->data_length;
    c->ram_used += m->data_length; c->ram_count++;
    *reason = PSPEXP_RERR_NONE;
    return e;
}

void pspexp_cache_remove(PspExpCache *c, PspExpCacheEntry *e)
{
    if (!e->used) return;
    if (e->tier == PSPEXP_TIER_VRAM) { c->vram_used -= e->bytes; c->vram_count--; }
    else { c->ram_used -= e->bytes; c->ram_count--; }
    free(e->frames);
    memset(e, 0, sizeof(*e));
}

int pspexp_cache_retier(PspExpCache *c, PspExpCacheEntry *e, uint8_t tier)
{
    if (!e->used || e->tier == tier) return 0;
    if (tier == PSPEXP_TIER_VRAM) {
        if (c->vram_used + e->bytes > c->vram_budget) return -1;
        c->ram_used -= e->bytes; c->ram_count--;
        c->vram_used += e->bytes; c->vram_count++;
    } else {
        if (c->ram_used + e->bytes > c->ram_budget) return -1;
        c->vram_used -= e->bytes; c->vram_count--;
        c->ram_used += e->bytes; c->ram_count++;
    }
    e->tier = tier;
    return 0;
}

void pspexp_cache_status(const PspExpCache *c, PspExpMemoryStatus *out)
{
    memset(out, 0, sizeof(*out));
    out->vram_free = c->vram_budget - c->vram_used;
    out->ram_free = c->ram_budget - c->ram_used;
    out->vram_count = c->vram_count;
    out->ram_count = c->ram_count;
}
