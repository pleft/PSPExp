#include "pspexp_ge.h"
#include <pspgu.h>
#include <pspge.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <malloc.h>
#include <string.h>

#define FB_BYTES        ((uint32_t)PSPEXP_DISPLAY_STRIDE * PSPEXP_DISPLAY_HEIGHT * 2u)
#define VRAM_TEX_BASE   (2u * FB_BYTES)
#define VRAM_TEX_SIZE   (PSPEXP_VRAM_BYTES - VRAM_TEX_BASE)
#define UNCACHED(p)     ((void *)((uintptr_t)(p) | 0x40000000u))

typedef struct GeTexture {
    void    *ptr;        /* address handed to sceGuTexImage */
    uint32_t vram_off;   /* valid when tier == VRAM */
    uint8_t  tier;
} GeTexture;

/* ---- VRAM allocator: first fit over the used-block list ------------------- */

static int vram_alloc(PspExpGeExecutor *ge, uint32_t size, uint32_t *out)
{
    uint32_t cursor = 0;
    int i, progress = 1;
    size = (size + 15u) & ~15u;
    /* Walk used blocks in address order (small n, repeated scans are fine). */
    while (progress) {
        uint32_t best = 0xFFFFFFFFu;
        progress = 0;
        for (i = 0; i < PSPEXP_GE_VRAM_BLOCKS; i++) {
            if (!ge->vram[i].used) continue;
            if (ge->vram[i].offset + ge->vram[i].size <= cursor) continue;
            if (ge->vram[i].offset < best) best = ge->vram[i].offset;
        }
        if (best == 0xFFFFFFFFu) break;                 /* no block beyond cursor */
        if (best >= cursor + size) break;               /* the gap before it fits */
        for (i = 0; i < PSPEXP_GE_VRAM_BLOCKS; i++)     /* skip past that block */
            if (ge->vram[i].used && ge->vram[i].offset == best) { cursor = best + ge->vram[i].size; break; }
        cursor = (cursor + 15u) & ~15u;
        progress = 1;
    }
    if (cursor + size > VRAM_TEX_SIZE) return -1;
    for (i = 0; i < PSPEXP_GE_VRAM_BLOCKS; i++)
        if (!ge->vram[i].used) {
            ge->vram[i].used = 1;
            ge->vram[i].offset = cursor;
            ge->vram[i].size = size;
            *out = cursor;
            return 0;
        }
    return -1;
}

static void vram_free(PspExpGeExecutor *ge, uint32_t offset)
{
    int i;
    for (i = 0; i < PSPEXP_GE_VRAM_BLOCKS; i++)
        if (ge->vram[i].used && ge->vram[i].offset == offset) { ge->vram[i].used = 0; return; }
}

/* ---- ops ------------------------------------------------------------------ */

static int op_texture_create(void *u, PspExpCacheEntry *e, const void *pixels)
{
    PspExpGeExecutor *ge = (PspExpGeExecutor *)u;
    GeTexture *t = (GeTexture *)malloc(sizeof(*t));
    if (!t) return PSPEXP_RERR_OUT_OF_MEMORY;
    memset(t, 0, sizeof(*t));
    if (e->tier == PSPEXP_TIER_VRAM) {
        uint32_t off;
        if (vram_alloc(ge, e->bytes, &off) == 0) {
            t->ptr = (unsigned char *)sceGeEdramGetAddr() + VRAM_TEX_BASE + off;
            t->vram_off = off;
            t->tier = PSPEXP_TIER_VRAM;
            memcpy(UNCACHED(t->ptr), pixels, e->bytes);
        } else {
            /* Fragmented: fall back to main RAM if the budget allows. */
            ge->stats.vram_alloc_failures++;
            if (!ge->rt || pspexp_cache_retier(&ge->rt->cache, e, PSPEXP_TIER_RAM) < 0) { free(t); return PSPEXP_RERR_OUT_OF_MEMORY; }
        }
    }
    if (e->tier == PSPEXP_TIER_RAM) {
        void *p = memalign(16, e->bytes);
        if (!p) { free(t); return PSPEXP_RERR_OUT_OF_MEMORY; }
        memcpy(p, pixels, e->bytes);
        sceKernelDcacheWritebackRange(p, e->bytes);
        t->ptr = p;
        t->tier = PSPEXP_TIER_RAM;
    }
    e->user = t;
    return PSPEXP_RERR_NONE;
}

static void op_texture_destroy(void *u, PspExpCacheEntry *e)
{
    PspExpGeExecutor *ge = (PspExpGeExecutor *)u;
    GeTexture *t = (GeTexture *)e->user;
    if (!t) return;
    if (t->tier == PSPEXP_TIER_VRAM) vram_free(ge, t->vram_off);
    else free(t->ptr);
    free(t);
    e->user = NULL;
    if (ge->cur_atlas == e) ge->cur_atlas = NULL;
}

static int op_palette_create(void *u, PspExpCacheEntry *e, const void *entries)
{
    void *p = memalign(16, e->bytes);
    (void)u;
    if (!p) return PSPEXP_RERR_OUT_OF_MEMORY;
    memcpy(p, entries, e->bytes);
    sceKernelDcacheWritebackRange(p, e->bytes);
    e->user = p;
    return PSPEXP_RERR_NONE;
}

static void op_palette_destroy(void *u, PspExpCacheEntry *e)
{
    PspExpGeExecutor *ge = (PspExpGeExecutor *)u;
    free(e->user);
    e->user = NULL;
    if (ge->cur_palette == e) ge->cur_palette = NULL;
}

static void op_frame_begin(void *u, uint32_t seq)
{
    PspExpGeExecutor *ge = (PspExpGeExecutor *)u;
    (void)seq;
    if (ge->in_frame) { sceGuFinish(); sceGuSync(0, 0); }
    sceGuStart(GU_DIRECT, ge->list);
    ge->ring_cur ^= 1;
    ge->ring_used = 0;
    ge->cur_atlas = NULL;
    ge->cur_palette = NULL;
    ge->tex_enabled = 0;
    ge->in_frame = 1;
    sceGuScissor(0, 0, PSPEXP_DISPLAY_WIDTH, PSPEXP_DISPLAY_HEIGHT);
    sceGuDisable(GU_TEXTURE_2D);
}

static void op_set_clip(void *u, int16_t x, int16_t y, int16_t w, int16_t h)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    (void)u;
    if (x1 > PSPEXP_DISPLAY_WIDTH) x1 = PSPEXP_DISPLAY_WIDTH;
    if (y1 > PSPEXP_DISPLAY_HEIGHT) y1 = PSPEXP_DISPLAY_HEIGHT;
    if (x1 <= x0 || y1 <= y0) { x0 = y0 = 0; x1 = y1 = 1; }   /* empty: clip everything */
    sceGuScissor(x0, y0, x1 - x0, y1 - y0);
}

static PspExpVertex2D *op_vertex_alloc(void *u, uint32_t vertex_count)
{
    PspExpGeExecutor *ge = (PspExpGeExecutor *)u;
    uint32_t bytes = (vertex_count * (uint32_t)sizeof(PspExpVertex2D) + 15u) & ~15u;
    unsigned char *p;
    if (ge->ring_used + bytes > PSPEXP_GE_RING_HALF) { ge->stats.dropped_batches++; return NULL; }
    p = ge->ring[ge->ring_cur] + ge->ring_used;
    ge->ring_used += bytes;
    return (PspExpVertex2D *)p;
}

static void op_draw_sprites(void *u, PspExpCacheEntry *atlas, PspExpCacheEntry *palette, uint16_t fmt, const PspExpVertex2D *v, uint32_t count)
{
    PspExpGeExecutor *ge = (PspExpGeExecutor *)u;
    GeTexture *t = (GeTexture *)atlas->user;
    if (!t) return;
    sceKernelDcacheWritebackRange((void *)v, count * 2u * sizeof(PspExpVertex2D));
    if (palette && (palette != ge->cur_palette || atlas != ge->cur_atlas)) {
        /* CLUT bound per draw: index mask 0xF for T4, 0xFF for T8; blocks of 8 entries. */
        sceGuClutMode(palette->format, 0, atlas->format == PSPEXP_TEX_T4 ? 0x0F : 0xFF, 0);
        sceGuClutLoad((int)(palette->width / 8), palette->user);
        ge->cur_palette = palette;
    }
    if (atlas != ge->cur_atlas) {
        sceGuTexMode(atlas->format, 0, 0, atlas->layout == PSPEXP_LAYOUT_SWIZZLED);
        sceGuTexImage(0, atlas->width, atlas->height, atlas->width, t->ptr);
        sceGuTexFlush();
        ge->cur_atlas = atlas;
        ge->stats.texture_changes++;
    }
    if (!ge->tex_enabled) {
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        ge->tex_enabled = 1;
    }
    sceGuDrawArray(GU_SPRITES, fmt == PSPEXP_VTX_SPRITE_2D_4444 ? PSPEXP_GU_VERTEX2D_4444_TYPE : PSPEXP_GU_VERTEX2D_TYPE,
                   (int)(2u * count), 0, v);
    ge->stats.batches++;
    ge->stats.last_vertex_bytes = ge->ring_used;
}

/* Untextured primitives use an 8888 vertex colour so the alpha survives. */
typedef struct FlatVertex { uint32_t color; int16_t x, y, z, pad; } FlatVertex;
#define FLAT_VTYPE (GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D)

static uint32_t flat_color(uint16_t c5650, uint8_t alpha)
{
    uint32_t r = c5650 & 0x1F, g = (c5650 >> 5) & 0x3F, b = (c5650 >> 11) & 0x1F;
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16) | ((uint32_t)alpha << 24);   /* ABGR as the GE wants */
}

static void op_draw_rect(void *u, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color, uint8_t alpha)
{
    PspExpGeExecutor *ge = (PspExpGeExecutor *)u;
    FlatVertex *rv = (FlatVertex *)sceGuGetMemory(2 * sizeof(FlatVertex));
    if (!rv) return;
    if (ge->tex_enabled) { sceGuDisable(GU_TEXTURE_2D); ge->tex_enabled = 0; }
    rv[0].color = flat_color(color, alpha); rv[0].x = x; rv[0].y = y; rv[0].z = 0; rv[0].pad = 0;
    rv[1].color = rv[0].color; rv[1].x = (int16_t)(x + w); rv[1].y = (int16_t)(y + h); rv[1].z = 0; rv[1].pad = 0;
    sceGuDrawArray(GU_SPRITES, FLAT_VTYPE, 2, 0, rv);
    ge->stats.rects++;
}

static void op_draw_line(void *u, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color, uint8_t alpha)
{
    PspExpGeExecutor *ge = (PspExpGeExecutor *)u;
    FlatVertex *lv = (FlatVertex *)sceGuGetMemory(2 * sizeof(FlatVertex));
    if (!lv) return;
    if (ge->tex_enabled) { sceGuDisable(GU_TEXTURE_2D); ge->tex_enabled = 0; }
    lv[0].color = flat_color(color, alpha); lv[0].x = x0; lv[0].y = y0; lv[0].z = 0; lv[0].pad = 0;
    lv[1].color = lv[0].color; lv[1].x = x1; lv[1].y = y1; lv[1].z = 0; lv[1].pad = 0;
    sceGuDrawArray(GU_LINES, FLAT_VTYPE, 2, 0, lv);
    ge->stats.rects++;
}

static uint32_t op_present(void *u)
{
    PspExpGeExecutor *ge = (PspExpGeExecutor *)u;
    uint32_t t0;
    sceKernelDcacheWritebackAll();
    sceGuFinish();
    t0 = sceKernelGetSystemTimeLow();
    sceGuSync(0, 0);
    ge->stats.last_ge_us = sceKernelGetSystemTimeLow() - t0;
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
    ge->in_frame = 0;
    ge->stats.frames++;
    return sceKernelGetSystemTimeLow();
}

static uint32_t op_frame_ge_us(void *u) { return ((PspExpGeExecutor *)u)->stats.last_ge_us; }

/* ---- setup ---------------------------------------------------------------- */

int pspexp_ge_init(PspExpGeExecutor *ge)
{
    memset(&ge->stats, 0, sizeof(ge->stats));
    memset(ge->vram, 0, sizeof(ge->vram));
    ge->ring_cur = 0; ge->ring_used = 0; ge->in_frame = 0; ge->cur_atlas = NULL; ge->tex_enabled = 0;
    sceGuInit();
    sceGuStart(GU_DIRECT, ge->list);
    sceGuDrawBuffer(GU_PSM_5650, (void *)0, PSPEXP_DISPLAY_STRIDE);
    sceGuDispBuffer(PSPEXP_DISPLAY_WIDTH, PSPEXP_DISPLAY_HEIGHT, (void *)FB_BYTES, PSPEXP_DISPLAY_STRIDE);
    sceGuOffset(2048 - PSPEXP_DISPLAY_WIDTH / 2, 2048 - PSPEXP_DISPLAY_HEIGHT / 2);
    sceGuViewport(2048, 2048, PSPEXP_DISPLAY_WIDTH, PSPEXP_DISPLAY_HEIGHT);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuScissor(0, 0, PSPEXP_DISPLAY_WIDTH, PSPEXP_DISPLAY_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuShadeModel(GU_FLAT);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuClearColor(0);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    return 0;
}

void pspexp_ge_ops(PspExpGeExecutor *ge, PspExpRuntimeOps *ops)
{
    ops->user = ge;
    ops->texture_create = op_texture_create;
    ops->texture_destroy = op_texture_destroy;
    ops->palette_create = op_palette_create;
    ops->palette_destroy = op_palette_destroy;
    ops->frame_begin = op_frame_begin;
    ops->set_clip = op_set_clip;
    ops->vertex_alloc = op_vertex_alloc;
    ops->draw_sprites = op_draw_sprites;
    ops->draw_rect = op_draw_rect;
    ops->draw_line = op_draw_line;
    ops->present = op_present;
    ops->frame_ge_us = op_frame_ge_us;
}

void pspexp_ge_bind(PspExpGeExecutor *ge, PspExpRuntime *rt) { ge->rt = rt; }

void pspexp_ge_term(PspExpGeExecutor *ge)
{
    if (ge->in_frame) { sceGuFinish(); sceGuSync(0, 0); ge->in_frame = 0; }
    sceGuDisplay(GU_FALSE);
    sceGuTerm();
}
