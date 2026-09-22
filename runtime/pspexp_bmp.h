/* Minimal 24-bit BMP encoder for screenshots (plain C99, no allocation). */
#ifndef PSPEXP_BMP_H
#define PSPEXP_BMP_H
#include <stdint.h>
#include <stddef.h>
/* Fills a 54-byte BMP header for a width x height 24-bit bottom-up image. */
void pspexp_bmp_header(uint8_t out[54], uint32_t width, uint32_t height);
/* Row stride in bytes of a 24-bit BMP row (padded to 4). */
static inline uint32_t pspexp_bmp_stride(uint32_t width) { return (width * 3u + 3u) & ~3u; }
/* Converts one row of RGB565 pixels to BGR bytes. */
void pspexp_bmp_row_5650(uint8_t *out, const uint16_t *pixels, uint32_t width);
#endif
