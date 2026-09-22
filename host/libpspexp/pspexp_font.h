/*
 * Host-baked text (section 5.5): a font is a glyph atlas uploaded as an
 * ordinary texture, and DRAW_TEXT is a host-side convenience that expands
 * to a DRAW_SPRITE_BATCH. The PSP runtime has no text opcode.
 *
 * A small built-in 6x8 bitmap font (ASCII 32..126, lowercase rendered as
 * uppercase) covers HUDs and diagnostics; games bake their own fonts the
 * same way: one frame per glyph.
 */
#ifndef PSPEXP_FONT_H
#define PSPEXP_FONT_H

#include "pspexp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PspExpFont {
    uint16_t atlas;        /* texture handle holding the glyphs */
    uint16_t first_frame;  /* frame index of character `first_char` */
    uint8_t  first_char;   /* usually 32 */
    uint8_t  glyph_count;  /* consecutive characters covered */
    uint8_t  advance;      /* pen advance per character, pixels */
    uint8_t  line_height;
} PspExpFont;

/* Uploads the built-in font as a CLUT4 atlas (`atlas_id`) with a 16-entry
 * palette (`palette_id`); entry 1 is white so the draw colour tints it.
 * Fills *font. Returns 0 or a negative PSPEXP_RERR_*. */
int pspexp_font_builtin_upload(PspExpContext *ctx, uint16_t atlas_id, uint16_t palette_id, PspExpFont *font);

/* Width in pixels of the longest line of `text`. */
int pspexp_text_width(const PspExpFont *font, const char *text);

/* Draws `text` at (x, y) in `color` (5650 tint, PSPEXP_COLOR_NONE = white)
 * as one sprite batch per call; '\n' starts a new line. Must be called
 * between pspexp_begin_frame and pspexp_present. Returns glyphs drawn. */
int pspexp_draw_text(PspExpContext *ctx, const PspExpFont *font, int16_t x, int16_t y, const char *text, uint16_t color);

/* Same, with printf formatting (buffer of 255 characters). */
int pspexp_draw_textf(PspExpContext *ctx, const PspExpFont *font, int16_t x, int16_t y, uint16_t color, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif /* PSPEXP_FONT_H */
