/* The launcher's drawing layer (see launcher_gfx.h). */
#include "launcher_gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb/stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ---- texture ids on the PSP --------------------------------------------------------- */
#define ID_FONT       20
#define ID_FONT_PAL   21
#define ID_NINE       40   /* + style */
#define ID_BUTTON     50   /* + 0..3 */
#define ID_BACKDROP   60   /* + 0..5 */
#define ID_ICON       3000 /* + slot */
#define ICON_SLOTS    96
#define ID_COVER      4000 /* + pool index */
#define COVER_SLOTS   10
#define COVER_IDS     32

#define SCREEN_W 480
#define SCREEN_H 272

static PspExpContext *G;

static uint16_t c4444(uint32_t rgb, int a)
{
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return pspexp_color_4444((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, (uint8_t)a);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static int pow2(int v) { int p = 8; while (p < v) p <<= 1; return p; }

static int read_file(const char *path, uint8_t **out, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    *out = NULL;
    if (!f) return -1;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return -1; }
    *out = (uint8_t *)malloc((size_t)n);
    if (!*out || fread(*out, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(*out); *out = NULL; return -1; }
    fclose(f);
    *len = (size_t)n;
    return 0;
}

/* RGBA8 (w x h) -> a pow2 8888 texture; returns 0 */
static int upload_rgba(uint16_t id, const uint8_t *rgba, int w, int h)
{
    int tw = pow2(w), th = pow2(h), y;
    PspExpTextureDesc d;
    PspExpFrame f;
    uint8_t *dst;
    memset(&d, 0, sizeof(d));
    d.width = (uint16_t)tw; d.height = (uint16_t)th; d.format = PSPEXP_TEX_8888;
    d.layout = PSPEXP_LAYOUT_LINEAR; d.priority = PSPEXP_PRIORITY_NORMAL;
    f.u0 = 0; f.v0 = 0; f.w = (uint16_t)(tw < 64 ? tw : 64); f.h = (uint16_t)th;
    dst = pspexp_upload_texture_begin(G, id, &d, (uint32_t)(tw * th * 4), &f, 1);
    if (!dst) return -1;
    memset(dst, 0, (size_t)tw * (size_t)th * 4);
    for (y = 0; y < h; y++) memcpy(dst + (size_t)y * (size_t)tw * 4, rgba + (size_t)y * (size_t)w * 4, (size_t)w * 4);
    return pspexp_upload_texture_commit(G, 1) < 0 ? -1 : 0;
}

static void quad(uint16_t tex, int x, int y, int w, int h, int u0, int v0, int u1, int v1, uint16_t color)
{
    PspExpQuadExRec q;
    memset(&q, 0, sizeof(q));
    q.x = (int16_t)x; q.y = (int16_t)y; q.w = (int16_t)w; q.h = (int16_t)h;
    q.u0 = (uint16_t)u0; q.v0 = (uint16_t)v0; q.u1 = (uint16_t)u1; q.v1 = (uint16_t)v1;
    q.color = color;
    pspexp_draw_quad_ex_batch(G, tex, &q, 1, PSPEXP_PALETTE_DEFAULT, PSPEXP_VTX_SPRITE_2D_4444);
}

uint32_t gfx_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

/* ---- signed distances ------------------------------------------------------------------ */

/* rounded box centred at (cx, cy), half size (hx, hy), radius r */
static float sd_rbox(float px, float py, float cx, float cy, float hx, float hy, float r)
{
    float qx = fabsf(px - cx) - hx + r, qy = fabsf(py - cy) - hy + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

static float sd_seg(float px, float py, float ax, float ay, float bx, float by)
{
    float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    float h = clampf((pax * bax + pay * bay) / (bax * bax + bay * bay), 0, 1);
    float dx = pax - bax * h, dy = pay - bay * h;
    return sqrtf(dx * dx + dy * dy);
}

static float cover_of(float d) { return clampf(0.5f - d, 0, 1); }

/* ---- fonts ------------------------------------------------------------------------------ */

#define G_FIRST 32
#define G_COUNT 95
#define ATLAS_W 512
#define ATLAS_H 256
typedef struct Glyph { int16_t xoff, yoff, adv; uint16_t frame; } Glyph;
typedef struct Font { Glyph g[G_COUNT]; int ascent, height; stbtt_fontinfo *info; float scale; } Font;
static Font F[FONT_COUNT];
static stbtt_fontinfo TTF_REG, TTF_SEMI, TTF_ICONS;
static uint8_t *TTF_DATA[3];
static int have_text, have_icons;
static uint8_t atlas[ATLAS_W * ATLAS_H];
static PspExpFrame frames[FONT_COUNT * G_COUNT];
static int nframes, pen_x, pen_y, row_h;

static int bake(Font *f, stbtt_fontinfo *info, float px)
{
    int c, a, d, lg;
    f->info = info;
    f->scale = stbtt_ScaleForPixelHeight(info, px);
    stbtt_GetFontVMetrics(info, &a, &d, &lg);
    f->ascent = (int)ceilf(a * f->scale);
    f->height = (int)ceilf((a - d) * f->scale);
    for (c = 0; c < G_COUNT; c++) {
        int x0, y0, x1, y1, adv, lsb, w, h;
        Glyph *g = &f->g[c];
        stbtt_GetCodepointHMetrics(info, G_FIRST + c, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(info, G_FIRST + c, f->scale, f->scale, &x0, &y0, &x1, &y1);
        w = x1 - x0; h = y1 - y0;
        g->adv = (int16_t)lroundf(adv * f->scale);
        g->xoff = (int16_t)x0; g->yoff = (int16_t)y0;
        g->frame = 0xFFFF;
        if (w <= 0 || h <= 0 || w > 64) continue;
        if (pen_x + w + 1 > ATLAS_W) { pen_x = 0; pen_y += row_h + 1; row_h = 0; }
        if (pen_y + h > ATLAS_H) return -1;
        stbtt_MakeCodepointBitmap(info, atlas + pen_y * ATLAS_W + pen_x, w, h, ATLAS_W, f->scale, f->scale, G_FIRST + c);
        frames[nframes].u0 = (uint16_t)pen_x; frames[nframes].v0 = (uint16_t)pen_y;
        frames[nframes].w = (uint16_t)w; frames[nframes].h = (uint16_t)h;
        g->frame = (uint16_t)nframes++;
        pen_x += w + 1;
        if (h > row_h) row_h = h;
    }
    return 0;
}

static int load_ttf(const char *dir, const char *name, stbtt_fontinfo *info, uint8_t **keep)
{
    char p[1024];
    size_t len;
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    if (read_file(p, keep, &len) < 0) { fprintf(stderr, "[launcher] missing %s\n", p); return -1; }
    return stbtt_InitFont(info, *keep, stbtt_GetFontOffsetForIndex(*keep, 0)) ? 0 : -1;
}

static int fonts_init(const char *dir)
{
    uint8_t pal[256 * 4];
    PspExpTextureDesc d;
    int i;
    if (load_ttf(dir, "Inter-Regular.ttf", &TTF_REG, &TTF_DATA[0]) < 0) return -1;
    if (load_ttf(dir, "Inter-SemiBold.ttf", &TTF_SEMI, &TTF_DATA[1]) < 0) TTF_SEMI = TTF_REG;
    if (bake(&F[FONT_H1], &TTF_SEMI, 24.0f) < 0 || bake(&F[FONT_TITLE], &TTF_SEMI, 16.0f) < 0 ||
        bake(&F[FONT_BODY], &TTF_REG, 13.0f) < 0 || bake(&F[FONT_SMALL], &TTF_REG, 11.5f) < 0 ||
        bake(&F[FONT_BADGE], &TTF_SEMI, 10.0f) < 0) { fprintf(stderr, "[launcher] glyph atlas full\n"); return -1; }
    for (i = 0; i < 256; i++) { pal[i * 4] = pal[i * 4 + 1] = pal[i * 4 + 2] = 255; pal[i * 4 + 3] = (uint8_t)i; }
    if (pspexp_upload_palette(G, ID_FONT_PAL, PSPEXP_TEX_8888, 256, pal, sizeof(pal)) < 0) return -1;
    memset(&d, 0, sizeof(d));
    d.width = ATLAS_W; d.height = ATLAS_H; d.format = PSPEXP_TEX_T8;
    d.layout = PSPEXP_LAYOUT_LINEAR; d.priority = PSPEXP_PRIORITY_HIGH; d.palette = ID_FONT_PAL;
    if (pspexp_upload_texture(G, ID_FONT, &d, atlas, sizeof(atlas), frames, (uint16_t)nframes) < 0) return -1;
    have_text = 1;
    return 0;
}

int gfx_line_height(int font) { return F[font].height; }

int gfx_text_width(int font, const char *s)
{
    int w = 0;
    for (; *s; s++) {
        int c = (unsigned char)*s;
        if (c < G_FIRST || c >= G_FIRST + G_COUNT) c = '?';
        w += F[font].g[c - G_FIRST].adv;
    }
    return w;
}

int gfx_text(int font, int x, int y, const char *s, uint32_t rgb, int alpha, int max_w)
{
    PspExpSpriteRec r[160];
    const Font *f = &F[font];
    int n = (int)strlen(s), i, pen = x, cnt = 0, ell = 0;
    uint16_t col = c4444(rgb, alpha);
    if (!have_text || alpha <= 0) return 0;
    if (max_w > 0 && gfx_text_width(font, s) > max_w) {
        int dots = f->g['.' - G_FIRST].adv * 3;
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%s", s);
        while (n > 0) { tmp[n] = 0; if (gfx_text_width(font, tmp) + dots <= max_w) break; n--; }
        while (n > 0 && s[n - 1] == ' ') n--;
        ell = 1;
    }
    for (i = 0; i < n + (ell ? 3 : 0) && cnt < 160; i++) {
        int c = i < n ? (unsigned char)s[i] : '.';
        const Glyph *g;
        if (c < G_FIRST || c >= G_FIRST + G_COUNT) c = '?';
        g = &f->g[c - G_FIRST];
        if (g->frame != 0xFFFF) {
            r[cnt].x = (int16_t)(pen + g->xoff); r[cnt].y = (int16_t)(y + f->ascent + g->yoff);
            r[cnt].frame = g->frame; r[cnt].color = col;
            cnt++;
        }
        pen += g->adv;
    }
    if (cnt) pspexp_draw_sprite_batch_ex(G, ID_FONT, r, (uint32_t)cnt, 0, PSPEXP_PALETTE_DEFAULT, PSPEXP_VTX_SPRITE_2D_4444);
    return pen - x;
}

static int CLX, CLY, CLW = SCREEN_W, CLH = SCREEN_H;

void gfx_set_clip(int x, int y, int w, int h)
{
    CLX = x; CLY = y; CLW = w; CLH = h;
    pspexp_set_clip(G, (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h);
}

int gfx_text_marquee(int font, int x, int y, const char *s, uint32_t rgb, int alpha, int max_w, uint64_t t_us, int *moving)
{
    const float speed = 32.0f, pause = 1.4f;   /* px/s, s */
    int w = gfx_text_width(font, s), over, x0, y0, x1, y1;
    float t, run, cycle, off;
    if (max_w <= 0 || w <= max_w) return gfx_text(font, x, y, s, rgb, alpha, 0);
    over = w - max_w;
    run = over / speed;
    cycle = pause + run + pause;
    t = fmodf((float)(t_us / 1000) / 1000.0f, cycle);
    off = t < pause ? 0.0f : t < pause + run ? (t - pause) * speed : (float)over;
    /* clip to the text's box, within the current clip */
    x0 = x > CLX ? x : CLX; y0 = (y - 2) > CLY ? (y - 2) : CLY;
    x1 = (x + max_w) < (CLX + CLW) ? (x + max_w) : (CLX + CLW);
    y1 = (y + F[font].height + 4) < (CLY + CLH) ? (y + F[font].height + 4) : (CLY + CLH);
    if (x1 > x0 && y1 > y0) {
        pspexp_set_clip(G, (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0));
        gfx_text(font, x - (int)lroundf(off), y, s, rgb, alpha, 0);
        pspexp_set_clip(G, (int16_t)CLX, (int16_t)CLY, (int16_t)CLW, (int16_t)CLH);
    }
    if (moving) *moving = 1;
    return max_w;
}

/* ---- flat rectangles and 9-slices ---------------------------------------------------------- */

void gfx_rect(int x, int y, int w, int h, uint32_t rgb, int alpha)
{
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    pspexp_fill_rect(G, (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h,
                     pspexp_color_5650((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb), (uint8_t)(alpha > 255 ? 255 : alpha));
}

typedef struct Nine { int corner, size; } Nine;   /* corner: the 1:1 part; size = 2 * corner + 2 */
static Nine NINE[NINE_COUNT];

static void nine_make(int style, float radius, float blur)
{
    int c = (int)ceilf(radius + blur), s = 2 * c + 2, x, y;
    uint8_t *px = (uint8_t *)calloc((size_t)s * (size_t)s, 4);
    float half = s / 2.0f;
    for (y = 0; y < s; y++)
        for (x = 0; x < s; x++) {
            float d = sd_rbox(x + 0.5f, y + 0.5f, half, half, half - blur, half - blur, radius), a;
            if (blur > 0) a = 0.5f * erfcf(d / (blur * 0.55f)) ;
            else a = cover_of(d);
            px[(y * s + x) * 4] = px[(y * s + x) * 4 + 1] = px[(y * s + x) * 4 + 2] = 255;
            px[(y * s + x) * 4 + 3] = (uint8_t)lroundf(clampf(a, 0, 1) * 255.0f);
        }
    NINE[style].corner = c; NINE[style].size = s;
    upload_rgba((uint16_t)(ID_NINE + style), px, s, s);
    free(px);
}

void gfx_nine(int style, int x, int y, int w, int h, uint32_t rgb, int alpha)
{
    PspExpQuadExRec q[9];
    int c = NINE[style].corner, s = NINE[style].size, n = 0, i, j;
    int xs[3], ws[3], us[4], ys[3], hs[3], vs[4];
    uint16_t col = c4444(rgb, alpha);
    if (alpha <= 0 || w <= 0 || h <= 0) return;
    if (w < 2 * c) { c = w / 2; }
    if (h < 2 * c) { c = h / 2 < c ? h / 2 : c; }
    xs[0] = x; ws[0] = c; xs[1] = x + c; ws[1] = w - 2 * c; xs[2] = x + w - c; ws[2] = c;
    ys[0] = y; hs[0] = c; ys[1] = y + c; hs[1] = h - 2 * c; ys[2] = y + h - c; hs[2] = c;
    /* source: corners from the texture's corners (possibly cropped when the box is small) */
    us[0] = 0; us[1] = c; us[2] = s - c; us[3] = s;
    vs[0] = 0; vs[1] = c; vs[2] = s - c; vs[3] = s;
    for (j = 0; j < 3; j++)
        for (i = 0; i < 3; i++) {
            int u0 = i == 1 ? NINE[style].corner : us[i], u1 = i == 1 ? NINE[style].corner + 2 : us[i + 1];
            int v0 = j == 1 ? NINE[style].corner : vs[j], v1 = j == 1 ? NINE[style].corner + 2 : vs[j + 1];
            if (ws[i] <= 0 || hs[j] <= 0) continue;
            if (i == 0) { u0 = 0; u1 = c; }
            if (i == 2) { u0 = s - c; u1 = s; }
            if (j == 0) { v0 = 0; v1 = c; }
            if (j == 2) { v0 = s - c; v1 = s; }
            memset(&q[n], 0, sizeof(q[n]));
            q[n].x = (int16_t)xs[i]; q[n].y = (int16_t)ys[j]; q[n].w = (int16_t)ws[i]; q[n].h = (int16_t)hs[j];
            q[n].u0 = (uint16_t)u0; q[n].v0 = (uint16_t)v0; q[n].u1 = (uint16_t)u1; q[n].v1 = (uint16_t)v1;
            q[n].color = col;
            n++;
        }
    if (n) pspexp_draw_quad_ex_batch(G, (uint16_t)(ID_NINE + style), q, (uint32_t)n, PSPEXP_PALETTE_DEFAULT, PSPEXP_VTX_SPRITE_2D_4444);
}

void gfx_shadow(int style, int x, int y, int w, int h, uint32_t rgb, int alpha)
{
    int b = style == NINE_SHADOW10 ? 8 : 12;
    gfx_nine(style, x - b, y - b, w + 2 * b, h + 2 * b, rgb, alpha);
}

/* ---- icons ------------------------------------------------------------------------------------ */

typedef struct IconName { const char *name; int cp; } IconName;
static const IconName ICONS[] = {
    { "gamepad-2", 0xe0df }, { "gamepad", 0xe0de }, { "joystick", 0xe355 }, { "history", 0xe1f5 }, { "star", 0xe176 },
    { "clock", 0xe087 }, { "rocket", 0xe286 }, { "settings", 0xe154 }, { "power", 0xe140 }, { "rotate-ccw", 0xe148 },
    { "refresh-cw", 0xe145 }, { "moon", 0xe11e }, { "music", 0xe122 }, { "wifi", 0xe1ae }, { "wifi-off", 0xe1af },
    { "wifi-low", 0xe5f8 }, { "wifi-high", 0xe5f7 }, { "wifi-zero", 0xe5f9 }, { "battery", 0xe053 }, { "battery-full", 0xe055 },
    { "battery-medium", 0xe057 }, { "battery-low", 0xe056 }, { "battery-charging", 0xe054 }, { "battery-warning", 0xe3ac },
    { "monitor", 0xe11d }, { "cpu", 0xe0a9 }, { "folder", 0xe0d7 }, { "terminal", 0xe181 }, { "disc", 0xe0af }, { "disc-3", 0xe494 },
    { "cassette-tape", 0xe4ca }, { "save", 0xe14d }, { "tv", 0xe195 }, { "smartphone", 0xe163 }, { "gamepad-directional", 0xe69b },
    { "puzzle", 0xe29c }, { "swords", 0xe2b4 }, { "ghost", 0xe20e }, { "trophy", 0xe373 }, { "sparkles", 0xe412 },
    { "layout-grid", 0xe0ff }, { "app-window", 0xe426 }, { "package", 0xe129 }, { "play", 0xe13c }, { "circle-play", 0xe080 },
    { "info", 0xe0f9 }, { "search", 0xe151 }, { "zap", 0xe1b4 }, { "usb", 0xe356 }, { "heart", 0xe0f2 },
    { "sliders-horizontal", 0xe29a }, { "volume-2", 0xe1ab }, { "hard-drive", 0xe0ed }, { "square-terminal", 0xe20a },
    { "layers", 0xe529 }, { "house", 0xe0f5 }, { "circle-power", 0xe550 }, { "list", 0xe106 }, { "wand-sparkles", 0xe357 },
    { "code", 0xe093 }, { "paintbrush", 0xe2e7 }, { "cloud", 0xe088 }, { "globe", 0xe0e8 }, { "link", 0xe102 }, { "unplug", 0xe45d },
    { "gem", 0xe242 }, { "tablet-smartphone", 0xe50a }, { "monitor-play", 0xe485 }, { "keyboard", 0xe284 },
    { "delete", 0xe0ae }, { "space", 0xe3dd }, { "corner-down-left", 0xe0a1 }, { "check", 0xe06c }, { "x", 0xe1b2 },
    { "toggle-left", 0xe18b }, { "toggle-right", 0xe18c }, { "image", 0xe0f6 }, { "image-off", 0xe1c0 }, { "router", 0xe3bf },
    { "microchip", 0xe61a }, { "circuit-board", 0xe403 }, { "laptop", 0xe1cd }, { "computer", 0xe4e4 },
    { "plug", 0xe37f }, { "ethernet-port", 0xe620 },
    { "radio-tower", 0xe404 }, { "folder-open", 0xe247 }, { "file-text", 0xe0cc }, { "tag", 0xe17f }, { "search-x", 0xe4ad },
    { "arrow-left", 0xe048 }, { "text-search", 0xe5ad }, { "lock", 0xe10b }, { "eye", 0xe0ba }, { "eye-off", 0xe0bb },
};

static int icon_cp(const char *name)
{
    size_t i;
    if (!name || !*name) return 0;
    for (i = 0; i < sizeof(ICONS) / sizeof(ICONS[0]); i++) if (!strcmp(ICONS[i].name, name)) return ICONS[i].cp;
    return 0;
}

typedef struct IconSlot { int cp, px, w, h; } IconSlot;
static IconSlot islot[ICON_SLOTS];
static int nislots;

/* renders the glyph centred in px x px (optionally into a caller buffer) */
static uint8_t *icon_bitmap(int cp, int px)
{
    float scale = stbtt_ScaleForMappingEmToPixels(&TTF_ICONS, (float)px);
    int x0, y0, x1, y1, w, h, ox, oy, ascent, descent, gap;
    uint8_t *cov, *out;
    int y, x;
    stbtt_GetFontVMetrics(&TTF_ICONS, &ascent, &descent, &gap);
    stbtt_GetCodepointBitmapBox(&TTF_ICONS, cp, scale, scale, &x0, &y0, &x1, &y1);
    w = x1 - x0; h = y1 - y0;
    if (w <= 0 || h <= 0) return NULL;
    cov = (uint8_t *)calloc((size_t)(w * h), 1);
    stbtt_MakeCodepointBitmap(&TTF_ICONS, cov, w, h, w, scale, scale, cp);
    out = (uint8_t *)calloc((size_t)(px * px), 1);
    /* Lucide glyphs sit on a 24-unit em box: place by the em box, not the ink box */
    { int adv, lsb; stbtt_GetCodepointHMetrics(&TTF_ICONS, cp, &adv, &lsb); ox = (int)lroundf((px - adv * scale) / 2.0f) + x0; }
    oy = (int)lroundf(ascent * scale) + y0 - (int)lroundf(((ascent - descent) * scale - px) / 2.0f);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int tx = ox + x, ty = oy + y;
            if (tx >= 0 && ty >= 0 && tx < px && ty < px) out[ty * px + tx] = cov[y * w + x];
        }
    free(cov);
    return out;
}

int gfx_icon(const char *name, int px, int x, int y, uint32_t rgb, int alpha)
{
    int cp = icon_cp(name), i, slot = -1;
    if (!have_icons || !cp || alpha <= 0) return 0;
    for (i = 0; i < nislots; i++) if (islot[i].cp == cp && islot[i].px == px) { slot = i; break; }
    if (slot < 0) {
        uint8_t *cov = icon_bitmap(cp, px), *rgba;
        if (!cov || nislots >= ICON_SLOTS) { free(cov); return 0; }
        rgba = (uint8_t *)malloc((size_t)(px * px * 4));
        for (i = 0; i < px * px; i++) { rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255; rgba[i * 4 + 3] = cov[i]; }
        free(cov);
        slot = nislots;
        if (upload_rgba((uint16_t)(ID_ICON + slot), rgba, px, px) < 0) { free(rgba); return 0; }
        free(rgba);
        islot[slot].cp = cp; islot[slot].px = px;
        nislots++;
    }
    quad((uint16_t)(ID_ICON + slot), x, y, px, px, 0, 0, px, px, c4444(rgb, alpha));
    return 1;
}

/* ---- PSP button badges -------------------------------------------------------------------------- */

static void buttons_make(void)
{
    static const struct { char k; uint32_t col; } B[4] = { { 'x', 0x8ab4ff }, { 'o', 0xff7a7a }, { 't', 0x5ee3a1 }, { 's', 0xf58bd0 } };
    int b, x, y, s = 14;
    for (b = 0; b < 4; b++) {
        uint8_t px[14 * 14 * 4];
        for (y = 0; y < s; y++)
            for (x = 0; x < s; x++) {
                float fx = x + 0.5f, fy = y + 0.5f, c = 7.0f, d, sym, bg, a;
                bg = cover_of(sqrtf((fx - c) * (fx - c) + (fy - c) * (fy - c)) - 6.8f);
                switch (B[b].k) {
                case 'x': d = fminf(sd_seg(fx, fy, 4.6f, 4.6f, 9.4f, 9.4f), sd_seg(fx, fy, 9.4f, 4.6f, 4.6f, 9.4f)); break;
                case 'o': d = fabsf(sqrtf((fx - c) * (fx - c) + (fy - c) * (fy - c)) - 3.0f); break;
                case 't': d = fminf(fminf(sd_seg(fx, fy, 7.0f, 3.8f, 10.0f, 9.2f), sd_seg(fx, fy, 10.0f, 9.2f, 4.0f, 9.2f)), sd_seg(fx, fy, 4.0f, 9.2f, 7.0f, 3.8f)); break;
                default:  d = fabsf(sd_rbox(fx, fy, c, c, 2.9f, 2.9f, 0.4f)); break;
                }
                sym = cover_of(d - 0.7f);
                /* badge: dark disc, coloured symbol */
                a = bg;
                {
                    float r = (0x22 / 255.0f) * (1 - sym) + ((B[b].col >> 16 & 255) / 255.0f) * sym;
                    float g = (0x28 / 255.0f) * (1 - sym) + ((B[b].col >> 8 & 255) / 255.0f) * sym;
                    float bl = (0x36 / 255.0f) * (1 - sym) + ((B[b].col & 255) / 255.0f) * sym;
                    uint8_t *p = px + (y * s + x) * 4;
                    p[0] = (uint8_t)lroundf(r * 255); p[1] = (uint8_t)lroundf(g * 255); p[2] = (uint8_t)lroundf(bl * 255);
                    p[3] = (uint8_t)lroundf(a * 255);
                }
            }
        upload_rgba((uint16_t)(ID_BUTTON + b), px, s, s);
    }
}

void gfx_button(char which, int x, int y)
{
    int b = which == 'x' ? 0 : which == 'o' ? 1 : which == 't' ? 2 : 3;
    quad((uint16_t)(ID_BUTTON + b), x, y, 14, 14, 0, 0, 14, 14, 0xFFFF);
}

/* ---- backdrop ------------------------------------------------------------------------------------ */

static const uint8_t BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static uint16_t dither5650(float r, float g, float b, int x, int y)
{
    float t = (BAYER[y & 3][x & 3] + 0.5f) / 16.0f - 0.5f;
    int R = (int)floorf(clampf(r, 0, 1) * 31.0f + 0.5f + t), Gc = (int)floorf(clampf(g, 0, 1) * 63.0f + 0.5f + t), B = (int)floorf(clampf(b, 0, 1) * 31.0f + 0.5f + t);
    R = R < 0 ? 0 : R > 31 ? 31 : R; Gc = Gc < 0 ? 0 : Gc > 63 ? 63 : Gc; B = B < 0 ? 0 : B > 31 ? 31 : B;
    return (uint16_t)((B << 11) | (Gc << 5) | R);
}

static void glow(float *r, float *g, float *b, float x, float y, float cx, float cy, float rad, uint32_t col, float k)
{
    float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) / rad, w;
    if (d >= 1) return;
    w = (1 - d) * (1 - d) * k;
    *r += ((col >> 16) & 255) / 255.0f * w; *g += ((col >> 8) & 255) / 255.0f * w; *b += (col & 255) / 255.0f * w;
}

static const int BD_X[2] = { 0, 256 }, BD_Y[3] = { 0, 128, 256 }, BD_H[3] = { 128, 128, 16 };

static void backdrop_make(void)
{
    int tx, ty;
    for (ty = 0; ty < 3; ty++)
        for (tx = 0; tx < 2; tx++) {
            int w = tx ? SCREEN_W - 256 : 256, h = BD_H[ty], x, y;
            PspExpTextureDesc d;
            PspExpFrame f;
            uint16_t *dst;
            memset(&d, 0, sizeof(d));
            d.width = 256; d.height = (uint16_t)pow2(h); d.format = PSPEXP_TEX_5650;
            d.layout = PSPEXP_LAYOUT_LINEAR; d.priority = PSPEXP_PRIORITY_NORMAL;
            f.u0 = 0; f.v0 = 0; f.w = 64; f.h = d.height;
            dst = (uint16_t *)pspexp_upload_texture_begin(G, (uint16_t)(ID_BACKDROP + ty * 2 + tx), &d, (uint32_t)(256 * d.height * 2), &f, 1);
            if (!dst) continue;
            memset(dst, 0, (size_t)256 * d.height * 2);
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) {
                    float X = (float)(BD_X[tx] + x), Y = (float)(BD_Y[ty] + y), t = Y / SCREEN_H;
                    float r = (0x14 * (1 - t) + 0x07 * t) / 255.0f, g = (0x17 * (1 - t) + 0x08 * t) / 255.0f, b = (0x26 * (1 - t) + 0x0e * t) / 255.0f;
                    glow(&r, &g, &b, X, Y, 90, -60, 330, 0x5b6cff, 0.30f);
                    glow(&r, &g, &b, X, Y, 500, 300, 300, 0x14b8c8, 0.13f);
                    glow(&r, &g, &b, X, Y, 300, 40, 220, 0x9b5cff, 0.08f);
                    dst[y * 256 + x] = dither5650(r, g, b, (int)X, (int)Y);
                }
            pspexp_upload_texture_commit(G, 1);
        }
}

void gfx_backdrop(void)
{
    int tx, ty;
    for (ty = 0; ty < 3; ty++)
        for (tx = 0; tx < 2; tx++) {
            int w = tx ? SCREEN_W - 256 : 256, h = BD_H[ty];
            PspExpQuadExRec q;
            memset(&q, 0, sizeof(q));
            q.x = (int16_t)BD_X[tx]; q.y = (int16_t)BD_Y[ty]; q.w = (int16_t)w; q.h = (int16_t)h;
            q.u1 = (uint16_t)w; q.v1 = (uint16_t)h; q.color = 0xFFFF;
            pspexp_draw_quad_ex_batch(G, (uint16_t)(ID_BACKDROP + ty * 2 + tx), &q, 1, PSPEXP_PALETTE_DEFAULT, PSPEXP_VTX_SPRITE_2D_5650);
        }
}

/* ---- covers ----------------------------------------------------------------------------------------- */

typedef struct CoverSlot { char key[160]; uint16_t id; int from_art; uint64_t used; char art[256]; } CoverSlot;
static CoverSlot cslot[COVER_SLOTS];
static int next_cover_id;
static uint64_t cover_clock;

static const uint32_t GRADS[][2] = {
    { 0x5b5ff0, 0x9d4edd }, { 0x0f8fd6, 0x1fc8db }, { 0xe8436a, 0xf58a42 }, { 0x0fa37a, 0x7cc41b },
    { 0xe38a0a, 0xe0453f }, { 0x7c4ef0, 0xe0469a }, { 0x11a598, 0x3a78f0 }, { 0x4c5a74, 0x273248 },
    { 0x2563eb, 0x7c3aed }, { 0xdb2777, 0x9333ea },
};

static void blend_px(uint8_t *p, float r, float g, float b, float a)
{
    p[0] = (uint8_t)lroundf(p[0] * (1 - a) + r * 255 * a);
    p[1] = (uint8_t)lroundf(p[1] * (1 - a) + g * 255 * a);
    p[2] = (uint8_t)lroundf(p[2] * (1 - a) + b * 255 * a);
}

/* draws text into an RGBA buffer (w x h) with full 8-bit coverage */
static int text_into(uint8_t *buf, int w, int h, stbtt_fontinfo *info, float px, int x, int y, const char *s, float alpha)
{
    float scale = stbtt_ScaleForPixelHeight(info, px);
    int asc, desc, lg, pen = x;
    stbtt_GetFontVMetrics(info, &asc, &desc, &lg);
    for (; *s; s++) {
        int adv, lsb, x0, y0, x1, y1, gw, gh, i, j;
        uint8_t *cov;
        stbtt_GetCodepointHMetrics(info, (unsigned char)*s, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(info, (unsigned char)*s, scale, scale, &x0, &y0, &x1, &y1);
        gw = x1 - x0; gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            cov = (uint8_t *)malloc((size_t)(gw * gh));
            stbtt_MakeCodepointBitmap(info, cov, gw, gh, gw, scale, scale, (unsigned char)*s);
            for (j = 0; j < gh; j++)
                for (i = 0; i < gw; i++) {
                    int tx = pen + x0 + i, ty = y + (int)lroundf(asc * scale) + y0 + j;
                    if (tx >= 0 && ty >= 0 && tx < w && ty < h && cov[j * gw + i]) blend_px(buf + (ty * w + tx) * 4, 1, 1, 1, cov[j * gw + i] / 255.0f * alpha);
                }
            free(cov);
        }
        pen += (int)lroundf(adv * scale);
        if (s[1]) pen += (int)lroundf(stbtt_GetCodepointKernAdvance(info, (unsigned char)s[0], (unsigned char)s[1]) * scale);
    }
    return pen - x;
}

static int text_w(stbtt_fontinfo *info, float px, const char *s, int n)
{
    float scale = stbtt_ScaleForPixelHeight(info, px);
    int w = 0, i;
    for (i = 0; s[i] && (n < 0 || i < n); i++) { int adv, lsb; stbtt_GetCodepointHMetrics(info, (unsigned char)s[i], &adv, &lsb); w += (int)lroundf(adv * scale); }
    return w;
}

/* word-wraps `s` into at most `max` lines of width `w` */
static int wrap(stbtt_fontinfo *info, float px, const char *s, int w, char lines[][64], int max)
{
    int n = 0;
    while (*s && n < max) {
        int len = (int)strlen(s), cut = len, last_space = -1, i;
        while (*s == ' ') s++;
        len = (int)strlen(s);
        for (i = 1; i <= len; i++) {
            if (s[i] == ' ' || s[i] == 0) { if (text_w(info, px, s, i) <= w) last_space = i; else break; }
        }
        cut = last_space > 0 ? last_space : len;
        if (last_space < 0) {   /* one long word: cut by width */
            for (cut = len; cut > 1 && text_w(info, px, s, cut) > w; cut--) {}
        }
        if (n == max - 1 && s[cut]) {   /* last line: ellipsis */
            while (cut > 1 && text_w(info, px, s, cut) + text_w(info, px, "...", -1) > w) cut--;
            snprintf(lines[n++], 64, "%.*s...", cut, s);
            return n;
        }
        snprintf(lines[n++], 64, "%.*s", cut > 63 ? 63 : cut, s);
        s += cut;
    }
    return n;
}

static void rounded_mask(uint8_t *px, int s, float radius)
{
    int x, y;
    for (y = 0; y < s; y++)
        for (x = 0; x < s; x++) {
            float d = sd_rbox(x + 0.5f, y + 0.5f, s / 2.0f, s / 2.0f, s / 2.0f, s / 2.0f, radius);
            uint8_t *p = px + (y * s + x) * 4;
            float ring = cover_of(fabsf(d + 0.5f) - 0.5f);   /* a faint 1 px light edge */
            blend_px(p, 1, 1, 1, ring * 0.10f);
            p[3] = (uint8_t)lroundf(p[3] * cover_of(d));
        }
}

static int load_art(const char *path, uint8_t *out, int s)
{
    uint8_t *file, *img;
    size_t len;
    int w, h, n, x, y, dw, dh, ox, oy;
    float avg[3] = { 0, 0, 0 };
    if (!path || !*path || read_file(path, &file, &len) < 0) return -1;
    img = stbi_load_from_memory(file, (int)len, &w, &h, &n, 4);
    free(file);
    if (!img) return -1;
    /* fit inside s x s, box-filtered */
    if (w >= h) { dw = s; dh = (int)lroundf((float)s * h / w); } else { dh = s; dw = (int)lroundf((float)s * w / h); }
    ox = (s - dw) / 2; oy = (s - dh) / 2;
    for (y = 0; y < h; y += 4) for (x = 0; x < w; x += 4) { const uint8_t *p = img + ((size_t)y * w + x) * 4; avg[0] += p[0]; avg[1] += p[1]; avg[2] += p[2]; }
    { float cnt = (float)(((h + 3) / 4) * ((w + 3) / 4)); avg[0] /= cnt; avg[1] /= cnt; avg[2] /= cnt; }
    for (y = 0; y < s; y++)
        for (x = 0; x < s; x++) {
            uint8_t *p = out + (y * s + x) * 4;
            p[0] = (uint8_t)(avg[0] * 0.35f + 10); p[1] = (uint8_t)(avg[1] * 0.35f + 12); p[2] = (uint8_t)(avg[2] * 0.35f + 18); p[3] = 255;
        }
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++) {
            int sx0 = x * w / dw, sx1 = (x + 1) * w / dw, sy0 = y * h / dh, sy1 = (y + 1) * h / dh, i, j, cnt = 0;
            float acc[4] = { 0, 0, 0, 0 };
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sy1 <= sy0) sy1 = sy0 + 1;
            for (j = sy0; j < sy1; j++) for (i = sx0; i < sx1; i++) { const uint8_t *p = img + ((size_t)j * w + i) * 4; acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3]; cnt++; }
            {
                uint8_t *p = out + ((oy + y) * s + ox + x) * 4;
                float a = acc[3] / cnt / 255.0f;
                blend_px(p, acc[0] / cnt / 255.0f, acc[1] / cnt / 255.0f, acc[2] / cnt / 255.0f, a);
            }
        }
    stbi_image_free(img);
    return 0;
}

static void cover_generate(const CoverSpec *c, uint8_t *px, int s)
{
    uint32_t h = gfx_hash(c->title);
    const uint32_t *gr = GRADS[h % (sizeof(GRADS) / sizeof(GRADS[0]))];
    char lines[3][64];
    int x, y, n, i;
    float tp = 17.0f;
    for (y = 0; y < s; y++)
        for (x = 0; x < s; x++) {
            float t = clampf((x * 0.55f + y * 0.85f) / (s * 1.4f), 0, 1);
            float r = (((gr[0] >> 16) & 255) * (1 - t) + ((gr[1] >> 16) & 255) * t) / 255.0f;
            float g = (((gr[0] >> 8) & 255) * (1 - t) + ((gr[1] >> 8) & 255) * t) / 255.0f;
            float b = ((gr[0] & 255) * (1 - t) + (gr[1] & 255) * t) / 255.0f;
            float shade = clampf((y - s * 0.45f) / (s * 0.55f), 0, 1) * 0.38f;   /* darker at the bottom, for the title */
            uint8_t *p = px + (y * s + x) * 4;
            p[0] = (uint8_t)lroundf(r * (1 - shade) * 255); p[1] = (uint8_t)lroundf(g * (1 - shade) * 255); p[2] = (uint8_t)lroundf(b * (1 - shade) * 255); p[3] = 255;
        }
    /* watermark icon, large and faint, off the right edge */
    if (have_icons && icon_cp(c->icon)) {
        int ip = 96;
        uint8_t *cov = icon_bitmap(icon_cp(c->icon), ip);
        if (cov) {
            int ox = s - ip + 22, oy = -6;
            for (y = 0; y < ip; y++) for (x = 0; x < ip; x++) {
                int tx = ox + x, ty = oy + y;
                if (tx >= 0 && ty >= 0 && tx < s && ty < s && cov[y * ip + x]) blend_px(px + (ty * s + tx) * 4, 1, 1, 1, cov[y * ip + x] / 255.0f * 0.18f);
            }
            free(cov);
        }
    }
    /* badge, top left */
    if (c->badge && c->badge[0]) {
        float bp = 9.5f;
        int bw = text_w(&TTF_SEMI, bp, c->badge, -1) + 12, bh = 15;
        for (y = 0; y < bh; y++) for (x = 0; x < bw; x++) {
            float d = sd_rbox(x + 0.5f, y + 0.5f, bw / 2.0f, bh / 2.0f, bw / 2.0f, bh / 2.0f, 7.5f);
            blend_px(px + ((10 + y) * s + 10 + x) * 4, 0, 0, 0, cover_of(d) * 0.28f);
        }
        text_into(px, s, s, &TTF_SEMI, bp, 16, 11, c->badge, 0.95f);
    }
    /* title, bottom left, up to three lines */
    n = wrap(&TTF_SEMI, tp, c->title, s - 20, lines, 3);
    if (n == 3 || (n == 2 && text_w(&TTF_SEMI, tp, lines[1], -1) > s - 20)) { tp = 15.0f; n = wrap(&TTF_SEMI, tp, c->title, s - 20, lines, 3); }
    for (i = 0; i < n; i++) text_into(px, s, s, &TTF_SEMI, tp, 10, s - 10 - (n - i) * (int)(tp + 3) + 2, lines[i], 1.0f);
}

static int cover_slot(const CoverSpec *c)
{
    int i, best = 0;
    uint8_t *px;
    for (i = 0; i < COVER_SLOTS; i++) if (cslot[i].key[0] && !strcmp(cslot[i].key, c->key)) {
        /* art that appeared since (a finished download) replaces the generated cover */
        if (!cslot[i].from_art && c->art && c->art[0] && strcmp(cslot[i].art, c->art)) { FILE *f = fopen(c->art, "rb"); if (f) { fclose(f); break; } }
        cslot[i].used = ++cover_clock;
        return i;
    }
    if (i < COVER_SLOTS) best = i;   /* re-render in place */
    else for (i = 1; i < COVER_SLOTS; i++) if (!cslot[i].key[0] || cslot[i].used < cslot[best].used) best = i;
    px = (uint8_t *)calloc(COVER_SIZE * COVER_SIZE, 4);
    cslot[best].from_art = load_art(c->art, px, COVER_SIZE) == 0;
    if (!cslot[best].from_art) cover_generate(c, px, COVER_SIZE);
    rounded_mask(px, COVER_SIZE, 12.0f);
    if (cslot[best].key[0]) pspexp_release(G, cslot[best].id);
    cslot[best].id = (uint16_t)(ID_COVER + (next_cover_id++ % COVER_IDS));   /* fresh id: the old one may still be on screen */
    upload_rgba(cslot[best].id, px, COVER_SIZE, COVER_SIZE);
    free(px);
    snprintf(cslot[best].key, sizeof(cslot[best].key), "%s", c->key);
    snprintf(cslot[best].art, sizeof(cslot[best].art), "%s", c->art ? c->art : "");
    cslot[best].used = ++cover_clock;
    return best;
}

int gfx_cover(const CoverSpec *c, int x, int y, int alpha)
{
    int s;
    if (alpha <= 0) return 0;
    s = cover_slot(c);
    quad(cslot[s].id, x, y, COVER_SIZE, COVER_SIZE, 0, 0, COVER_SIZE, COVER_SIZE, c4444(0xffffff, alpha));
    return cslot[s].from_art;
}

void gfx_cover_forget(const char *key)
{
    int i;
    for (i = 0; i < COVER_SLOTS; i++) if (!strcmp(cslot[i].key, key)) { pspexp_release(G, cslot[i].id); cslot[i].key[0] = 0; }
}

/* ---- init -------------------------------------------------------------------------------------------- */

void gfx_frame(PspExpContext *ctx) { G = ctx; CLX = 0; CLY = 0; CLW = SCREEN_W; CLH = SCREEN_H; }

int gfx_init(PspExpContext *ctx, const char *assets)
{
    G = ctx;
    if (fonts_init(assets) < 0) return -1;
    have_icons = load_ttf(assets, "lucide.ttf", &TTF_ICONS, &TTF_DATA[2]) == 0;
    nine_make(NINE_R6, 6, 0);
    nine_make(NINE_R10, 10, 0);
    nine_make(NINE_R14, 14, 0);
    nine_make(NINE_SHADOW10, 10, 8);
    nine_make(NINE_SHADOW14, 14, 12);
    buttons_make();
    backdrop_make();
    return 0;
}
