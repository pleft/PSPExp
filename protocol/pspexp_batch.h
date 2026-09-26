/*
 * PSPExp batch records and the shared expansion routine (section 5.4).
 *
 * Compact on the wire, trivial on the PSP: each record is expanded into two
 * GU_SPRITES vertices with one table lookup, four additions and a dozen
 * 16-bit stores, no branches. The same routine runs on the PSP (into the
 * GE vertex ring) and in the SDL/Null executors, so they agree by construction.
 */
#ifndef PSPEXP_BATCH_H
#define PSPEXP_BATCH_H

#include "pspexp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Uploaded once per atlas as part of the texture resource. */
typedef struct PspExpFrame {
    uint16_t u0, v0;        /* texel origin in the atlas */
    uint16_t w,  h;         /* frame size; w <= PSPEXP_FRAME_MAX_WIDTH by host construction */
} PspExpFrame;
PSPEXP_STATIC_ASSERT(sizeof(PspExpFrame) == 8, frame_size);

/* One sprite in a DRAW_SPRITE_BATCH payload. */
typedef struct PspExpSpriteRec {
    int16_t  x, y;          /* screen position */
    uint16_t frame;         /* index into the atlas frame table */
    uint16_t color;         /* 5650 tint, PSPEXP_COLOR_NONE for none */
} PspExpSpriteRec;
PSPEXP_STATIC_ASSERT(sizeof(PspExpSpriteRec) == 8, sprite_rec_size);

/* One tile in a DRAW_TILE_BATCH payload; position is implied by the grid. */
typedef struct PspExpTileRec {
    uint16_t frame;
    uint16_t color;
} PspExpTileRec;
PSPEXP_STATIC_ASSERT(sizeof(PspExpTileRec) == 4, tile_rec_size);

/* One quad in a DRAW_QUAD_BATCH payload: an arbitrary source rectangle,
 * drawn unscaled at (x, y). u0 > u1 mirrors it horizontally. */
typedef struct PspExpQuadRec {
    int16_t  x, y;
    uint16_t u0, v0, u1, v1;   /* texel corners, exclusive far edge */
    uint16_t color;
    uint16_t reserved;
} PspExpQuadRec;
PSPEXP_STATIC_ASSERT(sizeof(PspExpQuadRec) == 16, quad_rec_size);

/* Scaled quad (DRAW_QUAD_BATCH with PSPEXP_BATCH_SCALED): the source rect
 * is stretched to w x h at (x, y). u0 > u1 mirrors horizontally, v0 > v1
 * vertically. 24 bytes; used by SDL_RenderCopy-style APIs. */
typedef struct PspExpQuadExRec {
    int16_t  x, y, w, h;
    uint16_t u0, v0, u1, v1;
    uint16_t color;
    uint8_t  reserved[6];
} PspExpQuadExRec;
PSPEXP_STATIC_ASSERT(sizeof(PspExpQuadExRec) == 24, quadex_rec_size);

/* The GE vertex the records expand into:
 * GU_TEXTURE_16BIT | GU_COLOR_5650 | GU_VERTEX_16BIT | GU_TRANSFORM_2D.
 * Member order (texture, colour, position) and 16-bit alignment are what
 * the GE requires (section 3.2). */
typedef struct PspExpVertex2D {
    uint16_t u, v;
    uint16_t color;
    int16_t  x, y, z;
} PspExpVertex2D;
PSPEXP_STATIC_ASSERT(sizeof(PspExpVertex2D) == 12, vertex2d_size);

/* Numeric value of the GU vertex-type flags above, so the PSP runtime does
 * not depend on pspgu.h to describe this format:
 *   GU_TEXTURE_16BIT = 2 << 0, GU_COLOR_5650 = 4 << 2,
 *   GU_VERTEX_16BIT = 2 << 7, GU_TRANSFORM_2D = 1 << 23. */
#define PSPEXP_GU_VERTEX2D_TYPE ((2u << 0) | (4u << 2) | (2u << 7) | (1u << 23))
/* The 4444 variant: GU_COLOR_4444 = 6 << 2 (5 << 2 is GU_COLOR_5551, which
 * read every 4444 tint as 5551: wrong colours on the PSP, while the SDL
 * executor, which decodes 4444 itself, looked right). */
#define PSPEXP_GU_VERTEX2D_4444_TYPE ((2u << 0) | (6u << 2) | (2u << 7) | (1u << 23))

/* Conservative record count per draw so no single draw approaches the GE
 * primitive-count field limits, and so the expansion scratch stays bounded. */
#define PSPEXP_BATCH_MAX_RECORDS 4096u

/* Expand `count` sprite records into 2*count vertices. `frames` must have
 * at least (max frame index + 1) entries; see pspexp_sprites_max_frame(). */
void pspexp_expand_sprites(const PspExpFrame *frames, const PspExpSpriteRec *recs,
                           uint32_t count, PspExpVertex2D *out);
/* Same, mirrored horizontally: u runs from u0+w down to u0 (PSPEXP_BATCH_FLIP_X). */
void pspexp_expand_sprites_flipped(const PspExpFrame *frames, const PspExpSpriteRec *recs,
                                   uint32_t count, PspExpVertex2D *out);

/* Expand `count` quads into 2*count vertices. */
void pspexp_expand_quads(const PspExpQuadRec *recs, uint32_t count, PspExpVertex2D *out);

void pspexp_expand_quads_ex(const PspExpQuadExRec *recs, uint32_t count, PspExpVertex2D *out);

/* Expand a tile batch into 2*count vertices. */
void pspexp_expand_tiles(const PspExpFrame *frames, const PspExpTileBatch *batch,
                         const PspExpTileRec *recs, PspExpVertex2D *out);

/* Largest frame index referenced by a batch (0 when count is 0). Executors
 * check this against the atlas frame count before expanding, keeping the
 * expansion loop itself branch-free. */
uint32_t pspexp_sprites_max_frame(const PspExpSpriteRec *recs, uint32_t count);
uint32_t pspexp_tiles_max_frame(const PspExpTileRec *recs, uint32_t count);

#ifdef __cplusplus
}
#endif
#endif /* PSPEXP_BATCH_H */
