/*
 * The terminal's fonts (Spleen, generated into term_fonts.c by
 * scripts/dev/bdf2c.py): cells of w x h pixels, one byte per row (bit 7 =
 * leftmost), glyphs for ASCII 32..126 then Latin-1 160..255. Spleen 2.2.0,
 * BSD-2-Clause (assets/spleen-LICENSE.txt); regenerate with
 *   scripts/dev/bdf2c.py tools/pspexp-term/term_fonts.c small=6x8=spleen-5x8.bdf \
 *       medium=6x12=spleen-6x12.bdf large=8x16=spleen-8x16.bdf
 */
#ifndef PSPEXP_TERM_FONTS_H
#define PSPEXP_TERM_FONTS_H

#include <stdint.h>

typedef struct TermFont { const char *name; uint8_t w, h; const uint8_t *bits; } TermFont;

#define TERM_FONT_GLYPHS 191
/* the glyph index for a character (32..126, 160..255), -1 when none */
static inline int term_font_index(int c) { return c >= 32 && c <= 126 ? c - 32 : c >= 160 && c <= 255 ? c - 160 + 95 : -1; }

extern const TermFont TERM_FONTS[];
extern const int TERM_FONT_COUNT;

#endif
