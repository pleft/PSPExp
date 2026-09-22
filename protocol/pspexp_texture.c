#include "pspexp_protocol.h"
#include "pspexp_batch.h"

uint32_t pspexp_texture_bits_per_pixel(uint8_t format)
{
    switch (format) {
    case PSPEXP_TEX_5650: case PSPEXP_TEX_5551: case PSPEXP_TEX_4444: case PSPEXP_TEX_T16: return 16;
    case PSPEXP_TEX_8888: case PSPEXP_TEX_T32: return 32;
    case PSPEXP_TEX_T8: return 8;
    case PSPEXP_TEX_T4: return 4;
    default: return 0;
    }
}

uint32_t pspexp_texture_data_length(uint16_t width, uint16_t height, uint8_t format)
{
    uint32_t w = width, h = height;
    switch (format) {
    case PSPEXP_TEX_DXT1: return ((w + 3) / 4) * ((h + 3) / 4) * 8;
    case PSPEXP_TEX_DXT3:
    case PSPEXP_TEX_DXT5: return ((w + 3) / 4) * ((h + 3) / 4) * 16;
    default: {
        uint32_t bpp = pspexp_texture_bits_per_pixel(format);
        if (bpp == 0) return 0;
        return (w * h * bpp) / 8;
    }
    }
}

uint32_t pspexp_texture_payload_length(uint32_t data_length, uint16_t frame_count)
{
    return PSPEXP_ALIGN_UP(data_length) + PSPEXP_ALIGN_UP((uint32_t)frame_count * sizeof(PspExpFrame));
}

static int is_pow2(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

int pspexp_texture_validate(const PspExpTextureUpload *t, uint16_t supported_formats, uint32_t payload_length)
{
    uint32_t expected;
    uint16_t i;
    const PspExpFrame *frames;

    if (t->format >= PSPEXP_TEX_COUNT || !(supported_formats & PSPEXP_TEX_BIT(t->format)))
        return PSPEXP_RERR_BAD_FORMAT;
    if (!is_pow2(t->width) || !is_pow2(t->height) ||
        t->width > PSPEXP_TEXTURE_MAX || t->height > PSPEXP_TEXTURE_MAX)
        return PSPEXP_RERR_BAD_DIMENSIONS;
    expected = pspexp_texture_data_length(t->width, t->height, t->format);
    if (expected == 0 || t->data_length != expected)
        return PSPEXP_RERR_BAD_LENGTH;
    if (t->layout == PSPEXP_LAYOUT_SWIZZLED) {
        uint32_t row_bytes = (t->width * pspexp_texture_bits_per_pixel(t->format)) / 8;
        if (t->format >= PSPEXP_TEX_DXT1 || (row_bytes % 16) != 0 || (t->height % 8) != 0)
            return PSPEXP_RERR_BAD_LAYOUT;
    } else if (t->layout != PSPEXP_LAYOUT_LINEAR) {
        return PSPEXP_RERR_BAD_LAYOUT;
    }
    if (payload_length != pspexp_texture_payload_length(t->data_length, t->frame_count))
        return PSPEXP_RERR_BAD_LENGTH;
    frames = (const PspExpFrame *)pspexp_texture_frames(t, (const uint8_t *)t + sizeof(*t));
    for (i = 0; i < t->frame_count; i++) {
        const PspExpFrame *f = &frames[i];
        if (f->w == 0 || f->h == 0 ||
            (uint32_t)f->u0 + f->w > t->width || (uint32_t)f->v0 + f->h > t->height)
            return PSPEXP_RERR_BAD_FRAME_TABLE;
    }
    return PSPEXP_RERR_NONE;
}

const void *pspexp_texture_frames(const PspExpTextureUpload *t, const void *payload)
{
    return (const uint8_t *)payload + PSPEXP_ALIGN_UP(t->data_length);
}

void pspexp_swizzle(void *out, const void *in, uint32_t width_bytes, uint32_t height)
{
    const uint8_t *src = (const uint8_t *)in;
    uint8_t *dst = (uint8_t *)out;
    const uint32_t rowblocks = width_bytes / 16;
    uint32_t x, y;
    for (y = 0; y < height; y++) {
        const uint32_t blocky = y / 8, yin = y % 8;
        for (x = 0; x < width_bytes; x++) {
            const uint32_t blockx = x / 16, xin = x % 16;
            const uint32_t block = blockx + blocky * rowblocks;
            dst[block * 128 + yin * 16 + xin] = src[y * width_bytes + x];
        }
    }
}

void pspexp_unswizzle(void *out, const void *in, uint32_t width_bytes, uint32_t height)
{
    const uint8_t *src = (const uint8_t *)in;
    uint8_t *dst = (uint8_t *)out;
    const uint32_t rowblocks = width_bytes / 16;
    uint32_t x, y;
    for (y = 0; y < height; y++) {
        const uint32_t blocky = y / 8, yin = y % 8;
        for (x = 0; x < width_bytes; x++) {
            const uint32_t blockx = x / 16, xin = x % 16;
            const uint32_t block = blockx + blocky * rowblocks;
            dst[y * width_bytes + x] = src[block * 128 + yin * 16 + xin];
        }
    }
}
