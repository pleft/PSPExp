#include "pspexp_batch.h"

void pspexp_expand_sprites(const PspExpFrame *frames, const PspExpSpriteRec *recs,
                           uint32_t count, PspExpVertex2D *out)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        const PspExpSpriteRec r = recs[i];
        const PspExpFrame f = frames[r.frame];
        PspExpVertex2D *v = out + 2 * i;
        v[0].u = f.u0;
        v[0].v = f.v0;
        v[0].color = r.color;
        v[0].x = r.x;
        v[0].y = r.y;
        v[0].z = 0;
        v[1].u = (uint16_t)(f.u0 + f.w);
        v[1].v = (uint16_t)(f.v0 + f.h);
        v[1].color = r.color;
        v[1].x = (int16_t)(r.x + f.w);
        v[1].y = (int16_t)(r.y + f.h);
        v[1].z = 0;
    }
}

void pspexp_expand_sprites_flipped(const PspExpFrame *frames, const PspExpSpriteRec *recs,
                                   uint32_t count, PspExpVertex2D *out)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        const PspExpSpriteRec r = recs[i];
        const PspExpFrame f = frames[r.frame];
        PspExpVertex2D *v = out + 2 * i;
        v[0].u = (uint16_t)(f.u0 + f.w);
        v[0].v = f.v0;
        v[0].color = r.color;
        v[0].x = r.x;
        v[0].y = r.y;
        v[0].z = 0;
        v[1].u = f.u0;
        v[1].v = (uint16_t)(f.v0 + f.h);
        v[1].color = r.color;
        v[1].x = (int16_t)(r.x + f.w);
        v[1].y = (int16_t)(r.y + f.h);
        v[1].z = 0;
    }
}

void pspexp_expand_quads(const PspExpQuadRec *recs, uint32_t count, PspExpVertex2D *out)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        const PspExpQuadRec r = recs[i];
        PspExpVertex2D *v = out + 2 * i;
        const uint16_t w = (uint16_t)(r.u1 > r.u0 ? r.u1 - r.u0 : r.u0 - r.u1);
        const uint16_t h = (uint16_t)(r.v1 - r.v0);
        v[0].u = r.u0;
        v[0].v = r.v0;
        v[0].color = r.color;
        v[0].x = r.x;
        v[0].y = r.y;
        v[0].z = 0;
        v[1].u = r.u1;
        v[1].v = r.v1;
        v[1].color = r.color;
        v[1].x = (int16_t)(r.x + w);
        v[1].y = (int16_t)(r.y + h);
        v[1].z = 0;
    }
}

void pspexp_expand_quads_ex(const PspExpQuadExRec *recs, uint32_t count, PspExpVertex2D *out)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        const PspExpQuadExRec r = recs[i];
        PspExpVertex2D *v = out + 2 * i;
        v[0].u = r.u0; v[0].v = r.v0; v[0].color = r.color; v[0].x = r.x; v[0].y = r.y; v[0].z = 0;
        v[1].u = r.u1; v[1].v = r.v1; v[1].color = r.color;
        v[1].x = (int16_t)(r.x + r.w); v[1].y = (int16_t)(r.y + r.h); v[1].z = 0;
    }
}

void pspexp_expand_tiles(const PspExpFrame *frames, const PspExpTileBatch *batch,
                         const PspExpTileRec *recs, PspExpVertex2D *out)
{
    uint32_t i;
    int16_t x = batch->origin_x;
    int16_t y = batch->origin_y;
    uint16_t col = 0;
    const uint16_t columns = batch->columns ? batch->columns : 1;
    for (i = 0; i < batch->count; i++) {
        const PspExpTileRec r = recs[i];
        const PspExpFrame f = frames[r.frame];
        PspExpVertex2D *v = out + 2 * i;
        v[0].u = f.u0;
        v[0].v = f.v0;
        v[0].color = r.color;
        v[0].x = x;
        v[0].y = y;
        v[0].z = 0;
        v[1].u = (uint16_t)(f.u0 + f.w);
        v[1].v = (uint16_t)(f.v0 + f.h);
        v[1].color = r.color;
        v[1].x = (int16_t)(x + f.w);
        v[1].y = (int16_t)(y + f.h);
        v[1].z = 0;
        x = (int16_t)(x + batch->cell_w);
        if (++col == columns) {
            col = 0;
            x = batch->origin_x;
            y = (int16_t)(y + batch->cell_h);
        }
    }
}

uint32_t pspexp_sprites_max_frame(const PspExpSpriteRec *recs, uint32_t count)
{
    uint32_t i, m = 0;
    for (i = 0; i < count; i++)
        if (recs[i].frame > m) m = recs[i].frame;
    return m;
}

uint32_t pspexp_tiles_max_frame(const PspExpTileRec *recs, uint32_t count)
{
    uint32_t i, m = 0;
    for (i = 0; i < count; i++)
        if (recs[i].frame > m) m = recs[i].frame;
    return m;
}
