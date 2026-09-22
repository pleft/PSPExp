#include "pspexp_font.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

/* Built-in 5x7 glyphs in 6x8 cells, ASCII 32..126. Each glyph is 7 rows of
 * 5 characters; '#' is a set pixel. Lowercase letters share the uppercase
 * glyphs. Unknown characters draw as a box. */
#define ROWS 7
#define COLS 5
#define CELL_W 6
#define CELL_H 8
#define FIRST 32
#define COUNT 95
#define ATLAS_COLS 16
#define ATLAS_W 128
#define ATLAS_H 64   /* 6 rows of 8 px = 48, padded to a power of two */

typedef struct Glyph { char c; const char *rows[ROWS]; } Glyph;

static const Glyph GLYPHS[] = {
    {' ', {"     ","     ","     ","     ","     ","     ","     "}},
    {'!', {"  #  ","  #  ","  #  ","  #  ","  #  ","     ","  #  "}},
    {'"', {" # # "," # # ","     ","     ","     ","     ","     "}},
    {'#', {" # # ","#####"," # # "," # # ","#####"," # # ","     "}},
    {'$', {"  #  "," ####","# #  "," ### ","  # #","#### ","  #  "}},
    {'%', {"##   ","##  #","   # ","  #  "," #   ","#  ##","   ##"}},
    {'&', {" ##  ","#  # "," ##  "," ## #","#  # ","#  # "," ## #"}},
    {'\'',{"  #  ","  #  ","     ","     ","     ","     ","     "}},
    {'(', {"   # ","  #  "," #   "," #   "," #   ","  #  ","   # "}},
    {')', {" #   ","  #  ","   # ","   # ","   # ","  #  "," #   "}},
    {'*', {"     ","# # #"," ### ","#####"," ### ","# # #","     "}},
    {'+', {"     ","  #  ","  #  ","#####","  #  ","  #  ","     "}},
    {',', {"     ","     ","     ","     ","  ## ","  #  "," #   "}},
    {'-', {"     ","     ","     ","#####","     ","     ","     "}},
    {'.', {"     ","     ","     ","     ","     "," ##  "," ##  "}},
    {'/', {"    #","   # ","   # ","  #  "," #   "," #   ","#    "}},
    {'0', {" ### ","#   #","#  ##","# # #","##  #","#   #"," ### "}},
    {'1', {"  #  "," ##  ","  #  ","  #  ","  #  ","  #  "," ### "}},
    {'2', {" ### ","#   #","    #","   # ","  #  "," #   ","#####"}},
    {'3', {"#####","   # ","  #  ","   # ","    #","#   #"," ### "}},
    {'4', {"   # ","  ## "," # # ","#  # ","#####","   # ","   # "}},
    {'5', {"#####","#    ","#### ","    #","    #","#   #"," ### "}},
    {'6', {"  ## "," #   ","#    ","#### ","#   #","#   #"," ### "}},
    {'7', {"#####","    #","   # ","  #  "," #   "," #   "," #   "}},
    {'8', {" ### ","#   #","#   #"," ### ","#   #","#   #"," ### "}},
    {'9', {" ### ","#   #","#   #"," ####","    #","   # "," ##  "}},
    {':', {"     "," ##  "," ##  ","     "," ##  "," ##  ","     "}},
    {';', {"     "," ##  "," ##  ","     "," ##  ","  #  "," #   "}},
    {'<', {"   # ","  #  "," #   ","#    "," #   ","  #  ","   # "}},
    {'=', {"     ","     ","#####","     ","#####","     ","     "}},
    {'>', {" #   ","  #  ","   # ","    #","   # ","  #  "," #   "}},
    {'?', {" ### ","#   #","    #","   # ","  #  ","     ","  #  "}},
    {'@', {" ### ","#   #","# ###","# # #","# ###","#    "," ### "}},
    {'A', {" ### ","#   #","#   #","#####","#   #","#   #","#   #"}},
    {'B', {"#### ","#   #","#   #","#### ","#   #","#   #","#### "}},
    {'C', {" ### ","#   #","#    ","#    ","#    ","#   #"," ### "}},
    {'D', {"###  ","#  # ","#   #","#   #","#   #","#  # ","###  "}},
    {'E', {"#####","#    ","#    ","#### ","#    ","#    ","#####"}},
    {'F', {"#####","#    ","#    ","#### ","#    ","#    ","#    "}},
    {'G', {" ### ","#   #","#    ","# ###","#   #","#   #"," ####"}},
    {'H', {"#   #","#   #","#   #","#####","#   #","#   #","#   #"}},
    {'I', {" ### ","  #  ","  #  ","  #  ","  #  ","  #  "," ### "}},
    {'J', {"  ###","   # ","   # ","   # ","   # ","#  # "," ##  "}},
    {'K', {"#   #","#  # ","# #  ","##   ","# #  ","#  # ","#   #"}},
    {'L', {"#    ","#    ","#    ","#    ","#    ","#    ","#####"}},
    {'M', {"#   #","## ##","# # #","# # #","#   #","#   #","#   #"}},
    {'N', {"#   #","#   #","##  #","# # #","#  ##","#   #","#   #"}},
    {'O', {" ### ","#   #","#   #","#   #","#   #","#   #"," ### "}},
    {'P', {"#### ","#   #","#   #","#### ","#    ","#    ","#    "}},
    {'Q', {" ### ","#   #","#   #","#   #","# # #","#  # "," ## #"}},
    {'R', {"#### ","#   #","#   #","#### ","# #  ","#  # ","#   #"}},
    {'S', {" ####","#    ","#    "," ### ","    #","    #","#### "}},
    {'T', {"#####","  #  ","  #  ","  #  ","  #  ","  #  ","  #  "}},
    {'U', {"#   #","#   #","#   #","#   #","#   #","#   #"," ### "}},
    {'V', {"#   #","#   #","#   #","#   #","#   #"," # # ","  #  "}},
    {'W', {"#   #","#   #","#   #","# # #","# # #","## ##","#   #"}},
    {'X', {"#   #","#   #"," # # ","  #  "," # # ","#   #","#   #"}},
    {'Y', {"#   #","#   #"," # # ","  #  ","  #  ","  #  ","  #  "}},
    {'Z', {"#####","    #","   # ","  #  "," #   ","#    ","#####"}},
    {'[', {" ### "," #   "," #   "," #   "," #   "," #   "," ### "}},
    {'\\',{"#    "," #   "," #   ","  #  ","   # ","   # ","    #"}},
    {']', {" ### ","   # ","   # ","   # ","   # ","   # "," ### "}},
    {'^', {"  #  "," # # ","#   #","     ","     ","     ","     "}},
    {'_', {"     ","     ","     ","     ","     ","     ","#####"}},
    {'`', {" #   ","  #  ","     ","     ","     ","     ","     "}},
    {'{', {"   ##","  #  ","  #  "," #   ","  #  ","  #  ","   ##"}},
    {'|', {"  #  ","  #  ","  #  ","  #  ","  #  ","  #  ","  #  "}},
    {'}', {"##   ","  #  ","  #  ","   # ","  #  ","  #  ","##   "}},
    {'~', {"     ","     "," #  #","# # #","#  # ","     ","     "}},
};

static const Glyph *find_glyph(int c)
{
    size_t i;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    for (i = 0; i < sizeof(GLYPHS) / sizeof(GLYPHS[0]); i++)
        if (GLYPHS[i].c == c) return &GLYPHS[i];
    return NULL;
}

int pspexp_font_builtin_upload(PspExpContext *ctx, uint16_t atlas_id, uint16_t palette_id, PspExpFont *font)
{
    static uint8_t pixels[ATLAS_W * ATLAS_H / 2];   /* CLUT4: two texels per byte, low nibble first */
    static PspExpFrame frames[COUNT];
    static uint16_t palette[16];
    PspExpTextureDesc desc;
    int c, r;

    memset(pixels, 0, sizeof(pixels));
    memset(palette, 0, sizeof(palette));
    palette[1] = 0xFFFF;   /* 5551 white, opaque */
    for (c = 0; c < COUNT; c++) {
        const Glyph *g = find_glyph(FIRST + c);
        const int cx = (c % ATLAS_COLS) * CELL_W, cy = (c / ATLAS_COLS) * CELL_H;
        for (r = 0; r < ROWS; r++) {
            int x;
            for (x = 0; x < COLS; x++) {
                int on = g ? g->rows[r][x] == '#' : (r == 0 || r == ROWS - 1 || x == 0 || x == COLS - 1);
                if (on) {
                    int px = cx + x, py = cy + r;
                    int idx = py * ATLAS_W + px;
                    pixels[idx / 2] |= (uint8_t)(1u << ((idx & 1) * 4));
                }
            }
        }
        frames[c].u0 = (uint16_t)cx; frames[c].v0 = (uint16_t)cy; frames[c].w = CELL_W; frames[c].h = CELL_H;
    }
    r = pspexp_upload_palette(ctx, palette_id, PSPEXP_TEX_5551, 16, palette, sizeof(palette));
    if (r < 0) return r;
    memset(&desc, 0, sizeof(desc));
    desc.width = ATLAS_W; desc.height = ATLAS_H; desc.format = PSPEXP_TEX_T4;
    desc.layout = PSPEXP_LAYOUT_LINEAR; desc.priority = PSPEXP_PRIORITY_HIGH; desc.palette = palette_id;
    r = pspexp_upload_texture(ctx, atlas_id, &desc, pixels, sizeof(pixels), frames, COUNT);
    if (r < 0) return r;
    memset(font, 0, sizeof(*font));
    font->atlas = atlas_id;
    font->first_frame = 0;
    font->first_char = FIRST;
    font->glyph_count = COUNT;
    font->advance = CELL_W;
    font->line_height = CELL_H;
    return 0;
}

int pspexp_text_width(const PspExpFont *font, const char *text)
{
    int w = 0, best = 0;
    for (; *text; text++) {
        if (*text == '\n') { if (w > best) best = w; w = 0; continue; }
        w += font->advance;
    }
    return w > best ? w : best;
}

int pspexp_draw_text(PspExpContext *ctx, const PspExpFont *font, int16_t x, int16_t y, const char *text, uint16_t color)
{
    PspExpSpriteRec recs[256];
    int n = 0, total = 0;
    int16_t px = x, py = y;
    for (; *text; text++) {
        int c = (unsigned char)*text;
        if (c == '\n') { px = x; py = (int16_t)(py + font->line_height); continue; }
        if (c != ' ' && c >= font->first_char && c < font->first_char + font->glyph_count) {
            recs[n].x = px; recs[n].y = py;
            recs[n].frame = (uint16_t)(font->first_frame + (c - font->first_char));
            recs[n].color = color;
            if (++n == 256) { pspexp_draw_sprite_batch(ctx, font->atlas, recs, (uint32_t)n); total += n; n = 0; }
        }
        px = (int16_t)(px + font->advance);
    }
    if (n) { pspexp_draw_sprite_batch(ctx, font->atlas, recs, (uint32_t)n); total += n; }
    return total;
}

int pspexp_draw_textf(PspExpContext *ctx, const PspExpFont *font, int16_t x, int16_t y, uint16_t color, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return pspexp_draw_text(ctx, font, x, y, buf, color);
}
