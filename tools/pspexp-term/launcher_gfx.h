/*
 * The launcher's drawing layer. Everything that is not a flat rectangle is
 * rendered on the host at its final pixel size (the GE samples with nearest
 * filtering) and cached on the PSP as a texture:
 *
 *   text      Inter (Regular, SemiBold) at five sizes in one CLUT8 atlas
 *             whose palette is white with 256 alpha levels; tinted per string
 *   icons     Lucide glyphs, one 8888 texture per icon and size, tinted
 *   cards     anti-aliased rounded rectangles and soft shadows as 9-slices
 *             (corners drawn 1:1, uniform edges and centre stretched)
 *   backdrop  the 480x272 background, gradient and glows, dithered, 6 tiles
 *   covers    128x128 8888 per entry: box art when there is some, else a
 *             generated one (gradient, watermark icon, badge, title); an LRU
 *             keeps the last few on the PSP
 */
#ifndef PSPEXP_LAUNCHER_GFX_H
#define PSPEXP_LAUNCHER_GFX_H

#include <stdint.h>
#include "../../host/libpspexp/pspexp.h"

enum { FONT_H1, FONT_TITLE, FONT_BODY, FONT_SMALL, FONT_BADGE, FONT_COUNT };
enum { NINE_R6, NINE_R10, NINE_R14, NINE_SHADOW10, NINE_SHADOW14, NINE_COUNT };

#define COVER_SIZE 128

typedef struct CoverSpec {
    const char *key;        /* cache key: the entry id */
    const char *title;
    const char *badge;      /* "GBA", "GAME", ... */
    const char *icon;       /* watermark icon name */
    const char *art;        /* PNG to show instead, or NULL */
    const uint32_t *grad;   /* two colours for the generated cover, or NULL: by the title */
    int hero;               /* the icon large and bright at the top, not a faint watermark */
} CoverSpec;

int  gfx_init(PspExpContext *ctx, const char *assets_dir);
void gfx_frame(PspExpContext *ctx);   /* call at the start of every frame */

int  gfx_text(int font, int x, int y, const char *s, uint32_t rgb, int alpha, int max_w);
/* like gfx_text, but a line wider than max_w scrolls instead of ending in
 * "...": still for a moment, then to its end, still again, back. t_us is the
 * time since the text appeared (restart it when it changes). Sets *moving
 * when it scrolls, so the caller keeps drawing frames. */
int  gfx_text_marquee(int font, int x, int y, const char *s, uint32_t rgb, int alpha, int max_w, uint64_t t_us, int *moving);
/* the clip rectangle, remembered so text scrolling inside it can narrow it
 * and put it back */
void gfx_set_clip(int x, int y, int w, int h);
int  gfx_text_width(int font, const char *s);
/* s word-wrapped at spaces into lines of at most w pixels, line_h apart,
 * at most max_lines (the last one cut, with "..."); the lines drawn */
int  gfx_text_wrapped(int font, int x, int y, const char *s, uint32_t rgb, int alpha, int w, int line_h, int max_lines);
int  gfx_line_height(int font);

void gfx_rect(int x, int y, int w, int h, uint32_t rgb, int alpha);
void gfx_nine(int style, int x, int y, int w, int h, uint32_t rgb, int alpha);
/* a soft shadow / glow around the box (x, y, w, h) */
void gfx_shadow(int style, int x, int y, int w, int h, uint32_t rgb, int alpha);
int  gfx_icon(const char *name, int px, int x, int y, uint32_t rgb, int alpha);
void gfx_backdrop(void);
/* PSP button badge, 14x14: 'x' cross, 'o' circle, 't' triangle, 's' square */
void gfx_button(char which, int x, int y);

/* draws the cover at (x, y); 1 when it came from art */
int  gfx_cover(const CoverSpec *c, int x, int y, int alpha);
void gfx_cover_forget(const char *key);

/* the cover in a disc (gfx_disc's comment in launcher_gfx.c), centred at
 * (cx, cy): DISC_SMALL across, or DISC_BIG with the selection's glow; each
 * drawn from a DISC_TEX square texture. 1 when the cover came from art */
#define DISC_SMALL 70
#define DISC_BIG   86
#define DISC_TEX   128
int  gfx_disc(const CoverSpec *c, int cx, int cy, int big, int alpha);
void gfx_disc_forget(const char *key);

/* one picture at a time, for full-screen viewing: decoded, fitted inside
 * max_w x max_h (box-filtered, dithered to 5650) and kept on the PSP; its
 * size before fitting in *w, *h. 0 or -1 */
int  gfx_image_load(const char *path, int max_w, int max_h, int *w, int *h);
void gfx_image_draw(int x, int y);
void gfx_image_size(int *w, int *h);   /* as shown */
void gfx_image_free(void);

uint32_t gfx_hash(const char *s);

#endif
