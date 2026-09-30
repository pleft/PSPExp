#include "pspexp_ge.h"
#include <pspgu.h>
#include <pspge.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <malloc.h>
#include <string.h>

#define FB_BYTES        ((uint32_t)PSPEXP_DISPLAY_STRIDE * PSPEXP_DISPLAY_HEIGHT * 2u)
#define VRAM_TEX_SIZE   (PSPEXP_VRAM_BYTES - ge->tex_base)
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
            t->ptr = (unsigned char *)sceGeEdramGetAddr() + ge->tex_base + off;
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

/* ---- 3D state --------------------------------------------------------------- */

/* Back to the state the 2D draws expect (pspexp_ge_init's). */
static void leave_3d(PspExpGeExecutor *ge)
{
    if (!ge->mode3d) return;
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_DITHER);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuShadeModel(GU_FLAT);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuPixelMask(0);
    sceGuScissor(ge->clip_x, ge->clip_y, ge->clip_w, ge->clip_h);
    ge->tex_enabled = 0;
    ge->mode3d = 0;
    ge->state_dirty = 1;
}

static void bind_texture(PspExpGeExecutor *ge, PspExpCacheEntry *atlas, PspExpCacheEntry *palette)
{
    GeTexture *t = (GeTexture *)atlas->user;
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
}

static void set_enabled(int cap, int on) { if (on) sceGuEnable(cap); else sceGuDisable(cap); }

/* The stored 3D state onto the GE (all of it: cheap next to a frame). */
static void apply_3d(PspExpGeExecutor *ge)
{
    const PspExp3DState *s = &ge->st;
    int depth = ge->depth_on;
    ge->mode3d = 1;
    ge->state_dirty = 0;
    set_enabled(GU_DEPTH_TEST, depth && (s->enable & PSPEXP_3D_DEPTH_TEST));
    sceGuDepthMask(depth && (s->enable & PSPEXP_3D_DEPTH_WRITE) && (s->enable & PSPEXP_3D_DEPTH_TEST) ? GU_FALSE : GU_TRUE);
    sceGuDepthFunc(s->depth_func);
    sceGuDepthRange(s->depth_near, s->depth_far);
    set_enabled(GU_CULL_FACE, s->enable & PSPEXP_3D_CULL);
    sceGuFrontFace(s->front_face);
    set_enabled(GU_BLEND, s->enable & PSPEXP_3D_BLEND);
    sceGuBlendFunc(s->blend_op, s->blend_src, s->blend_dst, s->blend_fix_src, s->blend_fix_dst);
    set_enabled(GU_ALPHA_TEST, s->enable & PSPEXP_3D_ALPHA_TEST);
    sceGuAlphaFunc(s->alpha_func, s->alpha_ref, 0xFF);
    set_enabled(GU_FOG, s->enable & PSPEXP_3D_FOG);
    if (s->enable & PSPEXP_3D_FOG) sceGuFog(s->fog_near, s->fog_far, s->fog_color);
    set_enabled(GU_LIGHTING, s->enable & PSPEXP_3D_LIGHTING);
    set_enabled(GU_DITHER, s->enable & PSPEXP_3D_DITHER);
    sceGuShadeModel(s->enable & PSPEXP_3D_SMOOTH ? GU_SMOOTH : GU_FLAT);
    sceGuTexFunc(s->tex_func, s->tex_alpha);
    sceGuTexFilter(s->tex_min, s->tex_mag);
    sceGuTexWrap(s->tex_wrap_s, s->tex_wrap_t);
    sceGuTexEnvColor(s->tex_env_color);
    sceGuTexScale(s->tex_scale_u, s->tex_scale_v);
    sceGuTexOffset(s->tex_offset_u, s->tex_offset_v);
    if (s->tex_map == GU_ENVIRONMENT_MAP) sceGuTexMapMode(GU_ENVIRONMENT_MAP, s->tex_env_lights & 3, (s->tex_env_lights >> 4) & 3);
    else sceGuTexMapMode(s->tex_map == GU_TEXTURE_MATRIX ? GU_TEXTURE_MATRIX : GU_TEXTURE_COORDS, 0, 0);
    if (s->tex_map == GU_TEXTURE_MATRIX) sceGuTexProjMapMode(s->tex_source & 3);
    sceGuViewport(2048 - PSPEXP_DISPLAY_WIDTH / 2 + s->viewport_x, 2048 - PSPEXP_DISPLAY_HEIGHT / 2 + s->viewport_y,
                  s->viewport_w, s->viewport_h);
    {
        int x0 = s->scissor_x < 0 ? 0 : s->scissor_x, y0 = s->scissor_y < 0 ? 0 : s->scissor_y;
        int x1 = s->scissor_x + s->scissor_w, y1 = s->scissor_y + s->scissor_h;
        if (x1 > PSPEXP_DISPLAY_WIDTH) x1 = PSPEXP_DISPLAY_WIDTH;
        if (y1 > PSPEXP_DISPLAY_HEIGHT) y1 = PSPEXP_DISPLAY_HEIGHT;
        if (x1 <= x0 || y1 <= y0) { x0 = y0 = 0; x1 = y1 = 1; }
        sceGuScissor(x0, y0, x1 - x0, y1 - y0);
    }
    sceGuPixelMask(s->pixel_mask);
    sceGuEnable(GU_CLIP_PLANES);
    ge->tex_enabled = -1;   /* texturing is set per draw */
}

static void op_depth_buffer(void *u, int on)
{
    PspExpGeExecutor *ge = (PspExpGeExecutor *)u;
    uint32_t base = on ? 3u * FB_BYTES : 2u * FB_BYTES;
    int i;
    if (base == ge->tex_base) { ge->depth_on = on; return; }
    for (i = 0; i < PSPEXP_GE_VRAM_BLOCKS; i++)
        if (ge->vram[i].used) return;   /* textures already in VRAM: cannot move them (HELLO drops them all first) */
    if (ge->in_frame) return;         /* the depth buffer's place is drawn over mid-frame */
    ge->tex_base = base;
    ge->depth_on = on;
}

static void op_state3d(void *u, const PspExp3DState *s)
{
    PspExpGeExecutor *ge = (PspExpGeExecutor *)u;
    ge->st = *s;
    ge->have_state = 1;
    ge->state_dirty = 1;
}

static void op_matrices3d(void *u, const PspExp3DMatrices *m)
{
    static const ScePspFMatrix4 identity = { {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1} };
    (void)u;
    sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4 *)m->projection);
    sceGuSetMatrix(GU_VIEW, &identity);
    sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *)m->modelview);
    sceGuSetMatrix(GU_TEXTURE, (const ScePspFMatrix4 *)m->texture);
}

static void op_lights3d(void *u, const PspExp3DLights *l)
{
    int i;
    (void)u;
    sceGuAmbient(l->scene_ambient);
    sceGuModelColor(l->emissive & 0xFFFFFF, l->ambient & 0xFFFFFF, l->diffuse & 0xFFFFFF, l->specular & 0xFFFFFF);
    sceGuAmbientColor((l->ambient & 0xFFFFFF) | (l->diffuse & 0xFF000000u));   /* the lit alpha: GL takes the diffuse one */
    sceGuColorMaterial((int)l->color_material);
    sceGuSpecular(l->shininess);
    sceGuLightMode(GU_SINGLE_COLOR);
    for (i = 0; i < PSPEXP_3D_LIGHTS; i++) {
        const PspExp3DLight *li = &l->light[i];
        sceGuLight(i, li->type, li->components, (const ScePspFVector3 *)li->position);
        if (!li->enabled) { sceGuDisable(GU_LIGHT0 + i); continue; }
        sceGuLightColor(i, GU_AMBIENT, li->ambient);
        sceGuLightColor(i, GU_DIFFUSE, li->diffuse);
        sceGuLightColor(i, GU_SPECULAR, li->specular);
        sceGuLightAtt(i, li->attenuation[0], li->attenuation[1], li->attenuation[2]);
        if (li->type == GU_SPOTLIGHT)
            sceGuLightSpot(i, (const ScePspFVector3 *)li->direction, li->spot_exponent, li->spot_cutoff);
        sceGuEnable(GU_LIGHT0 + i);
    }
}

static void op_clear3d(void *u, const PspExp3DClear *c)
{
    PspExpGeExecutor *ge = (PspExpGeExecutor *)u;
    int flags = (int)(c->flags & PSPEXP_3D_CLEAR_COLOR);
    if (ge->depth_on) flags |= (int)(c->flags & PSPEXP_3D_CLEAR_DEPTH);
    if (!flags) return;
    if (!ge->mode3d || ge->state_dirty) {
        if (ge->have_state) apply_3d(ge);
        else leave_3d(ge);
    }
    sceGuClearColor(c->color);
    sceGuClearDepth(c->depth);
    sceGuClear(flags);
}

static void op_draw3d(void *u, PspExpCacheEntry *tex, PspExpCacheEntry *palette, uint8_t prim,
                      uint32_t vtype, uint32_t count, const void *vertices)
{
    PspExpGeExecutor *ge = (PspExpGeExecutor *)u;
    uint32_t bytes = count * pspexp_3d_vertex_size(vtype), rounded = (bytes + 15u) & ~15u;
    unsigned char *v;
    int textured = tex && tex->user;   /* coordinates in the vertices, or from the texture matrix */
    if (ge->ring_used + rounded > PSPEXP_GE_RING_HALF) { ge->stats.dropped3d++; return; }
    v = ge->ring[ge->ring_cur] + ge->ring_used;
    ge->ring_used += rounded;
    memcpy(v, vertices, bytes);
    sceKernelDcacheWritebackRange(v, rounded);
    if (!ge->mode3d || ge->state_dirty) apply_3d(ge);
    if (textured) {
        bind_texture(ge, tex, palette);
        if (ge->tex_enabled != 1) { sceGuEnable(GU_TEXTURE_2D); ge->tex_enabled = 1; }
    } else if (ge->tex_enabled != 0) { sceGuDisable(GU_TEXTURE_2D); ge->tex_enabled = 0; }
    sceGuDrawArray(prim, (int)vtype | GU_TRANSFORM_3D, (int)count, 0, v);
    ge->stats.draws3d++;
    ge->stats.last_vertex_bytes = ge->ring_used;
}

static void op_draw3d_mesh(void *u, PspExpCacheEntry *tex, PspExpCacheEntry *palette, uint8_t prim, uint32_t vtype,
                           uint32_t count, const void *vertices, const uint16_t *indices, int indices_inline)
{
    PspExpGeExecutor *ge = (PspExpGeExecutor *)u;
    int textured = tex && tex->user;
    if (indices_inline) {   /* into the ring: the message is gone before the GE reads it */
        uint32_t bytes = count * 2u, rounded = (bytes + 15u) & ~15u;
        unsigned char *p;
        if (ge->ring_used + rounded > PSPEXP_GE_RING_HALF) { ge->stats.dropped3d++; return; }
        p = ge->ring[ge->ring_cur] + ge->ring_used;
        ge->ring_used += rounded;
        memcpy(p, indices, bytes);
        sceKernelDcacheWritebackRange(p, rounded);
        indices = (const uint16_t *)p;
    }
    if (!ge->mode3d || ge->state_dirty) apply_3d(ge);
    if (textured) {
        bind_texture(ge, tex, palette);
        if (ge->tex_enabled != 1) { sceGuEnable(GU_TEXTURE_2D); ge->tex_enabled = 1; }
    } else if (ge->tex_enabled != 0) { sceGuDisable(GU_TEXTURE_2D); ge->tex_enabled = 0; }
    /* meshes are written back with the whole data cache at present */
    sceGuDrawArray(prim, (int)vtype | GU_TRANSFORM_3D | (indices ? GU_INDEX_16BIT : 0), (int)count, indices, vertices);
    ge->stats.draws3d++;
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
    ge->clip_x = ge->clip_y = 0; ge->clip_w = PSPEXP_DISPLAY_WIDTH; ge->clip_h = PSPEXP_DISPLAY_HEIGHT;
    ge->mode3d = 1;   /* whatever the last frame left: set the 2D state afresh */
    leave_3d(ge);
    if (ge->depth_on) sceGuDepthBuffer((void *)(2u * FB_BYTES), PSPEXP_DISPLAY_STRIDE);
}

static void op_set_clip(void *u, int16_t x, int16_t y, int16_t w, int16_t h)
{
    PspExpGeExecutor *ge = (PspExpGeExecutor *)u;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > PSPEXP_DISPLAY_WIDTH) x1 = PSPEXP_DISPLAY_WIDTH;
    if (y1 > PSPEXP_DISPLAY_HEIGHT) y1 = PSPEXP_DISPLAY_HEIGHT;
    if (x1 <= x0 || y1 <= y0) { x0 = y0 = 0; x1 = y1 = 1; }   /* empty: clip everything */
    ge->clip_x = (int16_t)x0; ge->clip_y = (int16_t)y0; ge->clip_w = (int16_t)(x1 - x0); ge->clip_h = (int16_t)(y1 - y0);
    if (!ge->mode3d) sceGuScissor(x0, y0, x1 - x0, y1 - y0);   /* else leave_3d sets it */
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
    leave_3d(ge);
    bind_texture(ge, atlas, palette);
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
    leave_3d(ge);
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
    leave_3d(ge);
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
    /* the buffer just drawn is now on screen: draw buffers alternate 0 / FB_BYTES */
    ge->displayed_offset = ge->displayed_offset == 0 ? FB_BYTES : 0;
    /* Set the display format ourselves: the debug status page leaves the
     * LCD in 32-bit mode, and on hardware the GU swap did not restore 16-bit
     * (the frame then shows quartered and tiled 2x2). */
    sceDisplaySetFrameBuf((unsigned char *)sceGeEdramGetAddr() + ge->displayed_offset, PSPEXP_DISPLAY_STRIDE,
                          PSP_DISPLAY_PIXEL_FORMAT_565, PSP_DISPLAY_SETBUF_NEXTFRAME);
    ge->in_frame = 0;
    ge->stats.frames++;
    return sceKernelGetSystemTimeLow();
}

void pspexp_ge_read_display(PspExpGeExecutor *ge, uint16_t *out)
{
    const uint16_t *fb = (const uint16_t *)UNCACHED((unsigned char *)sceGeEdramGetAddr() + ge->displayed_offset);
    int y;
    if (!ge->in_frame && ((uintptr_t)out & 15u) == 0) {
        /* The GE copies it (a block transfer): the same bytes on the PSP, and
         * PPSSPP, which keeps drawn frames in its own GPU, copies the real
         * frame back for it (a CPU read of VRAM gets a stale one there). */
        sceGuStart(GU_DIRECT, ge->list);
        sceGuCopyImage(GU_PSM_5650, 0, 0, PSPEXP_DISPLAY_WIDTH, PSPEXP_DISPLAY_HEIGHT, PSPEXP_DISPLAY_STRIDE,
                       (unsigned char *)sceGeEdramGetAddr() + ge->displayed_offset, 0, 0, PSPEXP_DISPLAY_WIDTH, out);
        sceGuTexSync();
        sceGuFinish();
        sceGuSync(0, 0);
        sceKernelDcacheInvalidateRange(out, PSPEXP_DISPLAY_WIDTH * PSPEXP_DISPLAY_HEIGHT * 2);
        return;
    }
    for (y = 0; y < PSPEXP_DISPLAY_HEIGHT; y++)
        memcpy(out + (size_t)y * PSPEXP_DISPLAY_WIDTH, fb + (size_t)y * PSPEXP_DISPLAY_STRIDE, PSPEXP_DISPLAY_WIDTH * 2);
}

static uint32_t op_frame_ge_us(void *u) { return ((PspExpGeExecutor *)u)->stats.last_ge_us; }

/* ---- setup ---------------------------------------------------------------- */

int pspexp_ge_init(PspExpGeExecutor *ge)
{
    memset(&ge->stats, 0, sizeof(ge->stats));
    memset(ge->vram, 0, sizeof(ge->vram));
    ge->ring_cur = 0; ge->ring_used = 0; ge->in_frame = 0; ge->cur_atlas = NULL; ge->tex_enabled = 0;
    ge->tex_base = 2u * FB_BYTES; ge->depth_on = 0; ge->mode3d = 0; ge->have_state = 0; ge->state_dirty = 1;
    ge->clip_x = ge->clip_y = 0; ge->clip_w = PSPEXP_DISPLAY_WIDTH; ge->clip_h = PSPEXP_DISPLAY_HEIGHT;
    ge->displayed_offset = FB_BYTES;   /* sceGuDispBuffer below */
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
    sceDisplaySetMode(0, PSPEXP_DISPLAY_WIDTH, PSPEXP_DISPLAY_HEIGHT);
    sceDisplaySetFrameBuf((unsigned char *)sceGeEdramGetAddr() + FB_BYTES, PSPEXP_DISPLAY_STRIDE,
                          PSP_DISPLAY_PIXEL_FORMAT_565, PSP_DISPLAY_SETBUF_IMMEDIATE);
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
    ops->depth_buffer = op_depth_buffer;
    ops->state3d = op_state3d;
    ops->matrices3d = op_matrices3d;
    ops->lights3d = op_lights3d;
    ops->draw3d = op_draw3d;
    ops->clear3d = op_clear3d;
    ops->draw3d_mesh = op_draw3d_mesh;
}

void pspexp_ge_bind(PspExpGeExecutor *ge, PspExpRuntime *rt) { ge->rt = rt; }

void pspexp_ge_term(PspExpGeExecutor *ge)
{
    if (ge->in_frame) { sceGuFinish(); sceGuSync(0, 0); ge->in_frame = 0; }
    sceGuDisplay(GU_FALSE);
    sceGuTerm();
}
