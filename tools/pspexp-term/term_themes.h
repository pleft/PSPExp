/*
 * The terminal's colour themes: the 16 ANSI colours, the default text and
 * background, the cursor, and the on-screen keyboard's and menu's colours to
 * match. Colours are 0xRRGGBB.
 */
#ifndef PSPEXP_TERM_THEMES_H
#define PSPEXP_TERM_THEMES_H

#include <stdint.h>

typedef struct TermTheme {
    const char *name;
    uint32_t fg, bg, cursor;
    uint32_t ansi[16];
    /* the keyboard and the menu */
    uint32_t panel, key, key_text, accent, accent_text, active, heading;
} TermTheme;

extern const TermTheme TERM_THEMES[];
extern const int TERM_THEME_COUNT;

/* the theme called name (any case), -1 when none */
int term_theme_find(const char *name);

#endif
