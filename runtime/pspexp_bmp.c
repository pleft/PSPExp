#include "pspexp_bmp.h"
#include <string.h>

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

void pspexp_bmp_header(uint8_t out[54], uint32_t width, uint32_t height)
{
    uint32_t image = pspexp_bmp_stride(width) * height;
    memset(out, 0, 54);
    out[0] = 'B'; out[1] = 'M';
    put32(out + 2, 54 + image);
    put32(out + 10, 54);
    put32(out + 14, 40);
    put32(out + 18, width);
    put32(out + 22, height);
    put16(out + 26, 1);
    put16(out + 28, 24);
    put32(out + 34, image);
}

void pspexp_bmp_row_5650(uint8_t *out, const uint16_t *pixels, uint32_t width)
{
    uint32_t x;
    for (x = 0; x < width; x++) {
        uint16_t c = pixels[x];
        uint32_t r = c & 0x1F, g = (c >> 5) & 0x3F, b = (c >> 11) & 0x1F;
        out[x * 3 + 0] = (uint8_t)((b << 3) | (b >> 2));
        out[x * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
        out[x * 3 + 2] = (uint8_t)((r << 3) | (r >> 2));
    }
}
