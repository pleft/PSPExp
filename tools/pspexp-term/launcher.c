/*
 * pspexp-term launcher view (see launcher.h). Layout at 480 x 272:
 *
 *   status strip   y 0..20     clock, battery, Wi-Fi, PSP link
 *   category rail  y 26..100   one icon per category, the selected one lifted
 *   cover + list   y 108..250  the entry's icon (or a generated cover) on the
 *                              left, the category's entries on the right
 *   hint bar       y 254..272  what the buttons do here
 */
#include "launcher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb/stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb/stb_image.h"
#pragma GCC diagnostic pop

/* ---- resources on the PSP ---------------------------------------------------- */

#define L_FONT_ATLAS   20     /* CLUT4 glyph atlas, both sizes */
#define L_FONT_PAL     21     /* 16 alpha levels of white */
#define L_ICON_FIRST   2000   /* one 4444 texture per icon */
#define L_ICON_MAX     160

#define SCREEN_W 480
#define SCREEN_H 272

/* colours (RGB) */
#define C_BG      0x0d1117
#define C_STRIP   0x161b22
#define C_CARD    0x1c2430
#define C_CARD_HI 0x243043
#define C_TEXT    0xe6edf3
#define C_MUTED   0x8b949e
#define C_DIM     0x484f58
static uint32_t C_ACCENT = 0x33a6ff;

static uint16_t rgb(uint32_t c) { return pspexp_color_5650((uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c); }

/* ---- fonts: TTF -> CLUT4 atlas ---------------------------------------------- */

#define G_FIRST 32
#define G_COUNT 95
typedef struct Glyph { int16_t xoff, yoff; int16_t adv; uint16_t frame; uint8_t w, h; } Glyph;
typedef struct UiFont { Glyph g[G_COUNT]; int ascent, height; float scale; stbtt_fontinfo *info; } UiFont;
static UiFont F_UI, F_TITLE, F_SMALL;
static stbtt_fontinfo g_ttf;
static uint8_t *g_ttf_data;
static int g_have_font;
#define ATLAS_W 256
#define ATLAS_H 256
static uint8_t g_atlas8[ATLAS_W * ATLAS_H];
static PspExpFrame g_frames[3 * G_COUNT];
static int g_nframes, g_pen_x, g_pen_y, g_row_h;

static int font_bake(UiFont *f, float px)
{
    int c;
    memset(f, 0, sizeof(*f));
    f->info = &g_ttf;
    f->scale = stbtt_ScaleForPixelHeight(&g_ttf, px);
    {
        int a, d, lg;
        stbtt_GetFontVMetrics(&g_ttf, &a, &d, &lg);
        f->ascent = (int)(a * f->scale + 0.5f);
        f->height = (int)((a - d) * f->scale + 0.5f);
    }
    for (c = 0; c < G_COUNT; c++) {
        int x0, y0, x1, y1, adv, lsb, w, h;
        Glyph *g = &f->g[c];
        stbtt_GetCodepointHMetrics(&g_ttf, G_FIRST + c, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&g_ttf, G_FIRST + c, f->scale, f->scale, &x0, &y0, &x1, &y1);
        w = x1 - x0; h = y1 - y0;
        g->adv = (int16_t)floorf(adv * f->scale + 0.5f);
        g->xoff = (int16_t)x0; g->yoff = (int16_t)y0;
        g->w = (uint8_t)w; g->h = (uint8_t)h;
        g->frame = 0xFFFF;
        if (w <= 0 || h <= 0 || w > 64) continue;
        if (g_pen_x + w + 1 > ATLAS_W) { g_pen_x = 0; g_pen_y += g_row_h + 1; g_row_h = 0; }
        if (g_pen_y + h + 1 > ATLAS_H || g_nframes >= (int)(sizeof(g_frames) / sizeof(g_frames[0]))) return -1;
        stbtt_MakeCodepointBitmap(&g_ttf, g_atlas8 + g_pen_y * ATLAS_W + g_pen_x, w, h, ATLAS_W, f->scale, f->scale, G_FIRST + c);
        g_frames[g_nframes].u0 = (uint16_t)g_pen_x; g_frames[g_nframes].v0 = (uint16_t)g_pen_y;
        g_frames[g_nframes].w = (uint16_t)w; g_frames[g_nframes].h = (uint16_t)h;
        g->frame = (uint16_t)g_nframes++;
        g_pen_x += w + 1;
        if (h > g_row_h) g_row_h = h;
    }
    return 0;
}

static int fonts_upload(PspExpContext *ctx)
{
    static uint8_t pixels[ATLAS_W * ATLAS_H / 2];
    uint16_t pal[16];
    PspExpTextureDesc d;
    int i, r;
    for (i = 0; i < 16; i++) pal[i] = pspexp_color_4444(255, 255, 255, (uint8_t)(i * 17));
    memset(pixels, 0, sizeof(pixels));
    for (i = 0; i < ATLAS_W * ATLAS_H; i++) pixels[i >> 1] |= (uint8_t)((g_atlas8[i] >> 4) << ((i & 1) * 4));
    r = pspexp_upload_palette(ctx, L_FONT_PAL, PSPEXP_TEX_4444, 16, pal, sizeof(pal));
    if (r < 0) return r;
    memset(&d, 0, sizeof(d));
    d.width = ATLAS_W; d.height = ATLAS_H; d.format = PSPEXP_TEX_T4;
    d.layout = PSPEXP_LAYOUT_LINEAR; d.priority = PSPEXP_PRIORITY_HIGH; d.palette = L_FONT_PAL;
    return pspexp_upload_texture(ctx, L_FONT_ATLAS, &d, pixels, sizeof(pixels), g_frames, (uint16_t)g_nframes);
}

static int read_file(const char *path, uint8_t **out, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return -1; }
    *out = (uint8_t *)malloc((size_t)n);
    if (!*out || fread(*out, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(*out); *out = NULL; return -1; }
    fclose(f);
    *len = (size_t)n;
    return 0;
}

static int fonts_init(PspExpContext *ctx, const char *skin)
{
    static const char *names[] = { "VarelaRound-Regular.ttf", "NotoSansMono-Regular.ttf", "VeraMono.ttf", NULL };
    char path[1024];
    size_t len;
    int i;
    for (i = 0; names[i]; i++) {
        snprintf(path, sizeof(path), "%s/truetype/%s", skin ? skin : ".", names[i]);
        if (read_file(path, &g_ttf_data, &len) == 0) break;
    }
    if (!g_ttf_data) { fprintf(stderr, "[launcher] no TTF in %s/truetype: text off\n", skin ? skin : "."); return -1; }
    if (!stbtt_InitFont(&g_ttf, g_ttf_data, stbtt_GetFontOffsetForIndex(g_ttf_data, 0))) return -1;
    if (font_bake(&F_TITLE, 20.0f) < 0 || font_bake(&F_UI, 14.0f) < 0 || font_bake(&F_SMALL, 11.0f) < 0) return -1;
    if (fonts_upload(ctx) < 0) return -1;
    g_have_font = 1;
    return 0;
}

/* text: sprite batches per call (a screen has a few dozen strings) */
static PspExpContext *g_ctx;
static PspExpSpriteRec g_srec[512];
static int g_sn;

static void text_flush(void)
{
    if (g_sn) pspexp_draw_sprite_batch(g_ctx, L_FONT_ATLAS, g_srec, (uint32_t)g_sn);
    g_sn = 0;
}

static int text_width(const UiFont *f, const char *s, int n)
{
    int w = 0, i;
    for (i = 0; (n < 0 || i < n) && s[i]; i++) {
        int c = (unsigned char)s[i];
        if (c < G_FIRST || c >= G_FIRST + G_COUNT) c = '?';
        w += f->g[c - G_FIRST].adv;
    }
    return w;
}

/* draws at (x, y) = top-left of the line box; clipped to max_w with "..." */
static int text_draw(const UiFont *f, int x, int y, const char *s, uint32_t color, int max_w)
{
    int pen = x, i, n = (int)strlen(s), ell = 0;
    uint16_t col = rgb(color);
    if (!g_have_font) return 0;
    if (max_w > 0 && text_width(f, s, -1) > max_w) {
        int dots = text_width(f, "...", -1);
        while (n > 0 && text_width(f, s, n) + dots > max_w) n--;
        ell = 1;
    }
    for (i = 0; i < n + (ell ? 3 : 0); i++) {
        int c = i < n ? (unsigned char)s[i] : '.';
        const Glyph *g;
        if (c < G_FIRST || c >= G_FIRST + G_COUNT) c = '?';
        g = &f->g[c - G_FIRST];
        if (g->frame != 0xFFFF && g_sn < (int)(sizeof(g_srec) / sizeof(g_srec[0]))) {
            g_srec[g_sn].x = (int16_t)(pen + g->xoff); g_srec[g_sn].y = (int16_t)(y + f->ascent + g->yoff);
            g_srec[g_sn].frame = g->frame; g_srec[g_sn].color = col;
            g_sn++;
        }
        pen += g->adv;
    }
    return pen - x;
}

/* ---- icons: PNG -> 4444 texture ----------------------------------------------- */

typedef struct Icon { char path[512]; uint16_t tex; uint16_t w, h, tw, th; int state; } Icon;   /* state 0 none, 1 uploaded, -1 failed */
static Icon g_icons[L_ICON_MAX];
static int g_nicons;

/* averages the image down by 2 until it fits 128 */
static void halve(uint8_t *px, int *w, int *h)
{
    int nw = *w / 2, nh = *h / 2, x, y, c;
    for (y = 0; y < nh; y++)
        for (x = 0; x < nw; x++)
            for (c = 0; c < 4; c++) {
                const uint8_t *a = px + ((size_t)(2 * y) * (size_t)*w + (size_t)(2 * x)) * 4 + c;
                px[((size_t)y * (size_t)nw + (size_t)x) * 4 + c] = (uint8_t)((a[0] + a[4] + a[(size_t)*w * 4] + a[(size_t)*w * 4 + 4] + 2) / 4);
            }
    *w = nw; *h = nh;
}

static int icon_upload(PspExpContext *ctx, Icon *ic)
{
    uint8_t *file, *px;
    size_t len;
    int w, h, n, tw = 8, th = 8, x, y;
    uint16_t *dst;
    PspExpTextureDesc d;
    PspExpFrame f;
    if (read_file(ic->path, &file, &len) < 0) return -1;
    px = stbi_load_from_memory(file, (int)len, &w, &h, &n, 4);
    free(file);
    if (!px) return -1;
    while (w > 128 || h > 128) halve(px, &w, &h);
    while (tw < w) tw <<= 1;
    while (th < h) th <<= 1;
    memset(&d, 0, sizeof(d));
    d.width = (uint16_t)tw; d.height = (uint16_t)th; d.format = PSPEXP_TEX_4444;
    d.layout = PSPEXP_LAYOUT_LINEAR; d.priority = PSPEXP_PRIORITY_NORMAL;
    f.u0 = 0; f.v0 = 0; f.w = (uint16_t)(tw < 64 ? tw : 64); f.h = (uint16_t)th;
    dst = (uint16_t *)pspexp_upload_texture_begin(ctx, ic->tex, &d, (uint32_t)(tw * th * 2), &f, 1);
    if (!dst) { stbi_image_free(px); return -1; }
    for (y = 0; y < th; y++)
        for (x = 0; x < tw; x++) {
            const uint8_t *p = px + ((size_t)y * (size_t)w + (size_t)x) * 4;
            dst[y * tw + x] = (x < w && y < h) ? pspexp_color_4444(p[0], p[1], p[2], p[3]) : 0;
        }
    stbi_image_free(px);
    if (pspexp_upload_texture_commit(ctx, 1) < 0) return -1;
    ic->w = (uint16_t)w; ic->h = (uint16_t)h; ic->tw = (uint16_t)tw; ic->th = (uint16_t)th;
    return 0;
}

/* returns an icon index (+1) for the file, 0 if none */
static int icon_get(const char *path)
{
    int i;
    struct stat st;
    if (!path || !*path || stat(path, &st) != 0) return 0;
    for (i = 0; i < g_nicons; i++) if (!strcmp(g_icons[i].path, path)) return i + 1;
    if (g_nicons >= L_ICON_MAX) return 0;
    snprintf(g_icons[g_nicons].path, sizeof(g_icons[g_nicons].path), "%s", path);
    g_icons[g_nicons].tex = (uint16_t)(L_ICON_FIRST + g_nicons);
    g_icons[g_nicons].state = 0;
    return ++g_nicons;
}

/* draws icon (index+1) fitted into a box, centred; alpha 0..255 */
static int icon_draw(int idx, int x, int y, int box, int alpha)
{
    Icon *ic;
    PspExpQuadExRec q;
    int w, h;
    if (idx <= 0) return 0;
    ic = &g_icons[idx - 1];
    if (ic->state == 0) ic->state = icon_upload(g_ctx, ic) == 0 ? 1 : -1;
    if (ic->state < 0) return 0;
    if (ic->w >= ic->h) { w = box; h = box * ic->h / ic->w; } else { h = box; w = box * ic->w / ic->h; }
    memset(&q, 0, sizeof(q));
    q.x = (int16_t)(x + (box - w) / 2); q.y = (int16_t)(y + (box - h) / 2); q.w = (int16_t)w; q.h = (int16_t)h;
    q.u0 = 0; q.v0 = 0; q.u1 = ic->w; q.v1 = ic->h;
    q.color = pspexp_color_4444(255, 255, 255, (uint8_t)alpha);
    pspexp_draw_quad_ex_batch(g_ctx, ic->tex, &q, 1, PSPEXP_PALETTE_DEFAULT, PSPEXP_VTX_SPRITE_2D_4444);
    return 1;
}

/* ---- the content model ------------------------------------------------------- */

enum { K_SCRIPT, K_FOLDER, K_PLUGIN, K_EMU, K_ROM };

typedef struct Entry {
    char name[96];          /* shown: "NN_" prefix and ".sh" stripped */
    char path[512];         /* the script, folder or ROM */
    char detail[128];
    int kind, icon, fav;
    /* K_EMU */
    char rom_dir[256], ext[64], launcher[256], rom_so[256];
    /* K_FOLDER / K_EMU: children, loaded on first open */
    struct Entry *kids; int nkids, loaded;
} Entry;

typedef struct Category { char name[96]; char dir[512]; int icon; Entry *e; int n; } Category;

#define MAX_CATS 24
static Category g_cats[MAX_CATS];
static int g_ncats;
static char g_skin[512];
static char g_config_dir[512];

/* "20_Retro Games" -> "Retro Games"; "freeDM.sh" -> "freeDM" */
static void display_name(const char *file, char *out, size_t n)
{
    const char *s = file, *dot;
    size_t len;
    if (s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9' && s[2] == '_') s += 3;
    else if (s[0] >= '0' && s[0] <= '9' && s[1] == '_') s += 2;
    snprintf(out, n, "%s", s);
    len = strlen(out);
    dot = len > 3 ? out + len - 3 : NULL;
    if (dot && !strcmp(dot, ".sh")) out[len - 3] = 0;
}

static int cmp_names(const void *a, const void *b) { return strcasecmp(*(const char *const *)a, *(const char *const *)b); }

/* sorted directory listing (names only); returns count, fills `names` (malloc'd) */
static int list_dir(const char *dir, char ***names)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    int n = 0, cap = 0;
    *names = NULL;
    if (!d) return 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (n == cap) { cap = cap ? cap * 2 : 32; *names = (char **)realloc(*names, (size_t)cap * sizeof(char *)); }
        (*names)[n++] = strdup(de->d_name);
    }
    closedir(d);
    if (n) qsort(*names, (size_t)n, sizeof(char *), cmp_names);
    return n;
}

static void free_names(char **names, int n) { int i; for (i = 0; i < n; i++) free(names[i]); free(names); }

static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '\t')) s[--n] = 0;
}

/* action.config: KEY=value lines */
static void read_action_config(Entry *e, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0; trim(line); trim(eq);
        if (!strcmp(line, "ROM")) snprintf(e->rom_dir, sizeof(e->rom_dir), "%s", eq);
        else if (!strcmp(line, "EXT")) snprintf(e->ext, sizeof(e->ext), "%s", eq);
        else if (!strcmp(line, "LAUNCHER")) snprintf(e->launcher, sizeof(e->launcher), "%s", eq);
        else if (!strcmp(line, "ROM_SO")) snprintf(e->rom_so, sizeof(e->rom_so), "%s", eq);
        else if (!strcmp(line, "TITLE")) snprintf(e->detail, sizeof(e->detail), "%s", eq);
    }
    fclose(f);
}

/* the icon for an entry: next to it, else in the skin under the same folder path */
static int find_icon(const char *skin_rel, const char *name, const char *beside_dir)
{
    char p[1024];
    int ic;
    if (beside_dir) {
        snprintf(p, sizeof(p), "%s/%s.png", beside_dir, name);
        if ((ic = icon_get(p))) return ic;
    }
    snprintf(p, sizeof(p), "%s/Menu/GameShell/%s%s%s.png", g_skin, skin_rel, *skin_rel ? "/" : "", name);
    if ((ic = icon_get(p))) return ic;
    snprintf(p, sizeof(p), "%s/Menu/GameShell/%s.png", g_skin, name);
    return icon_get(p);
}

static int has_ext(const char *file, const char *exts)
{
    const char *dot = strrchr(file, '.');
    char list[64], *tok, *save;
    if (!dot) return 0;
    snprintf(list, sizeof(list), "%s", exts);
    for (tok = strtok_r(list, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save))
        if (!strcasecmp(dot + 1, tok)) return 1;
    return 0;
}

static Entry *add_entry(Entry **arr, int *n)
{
    Entry *e;
    *arr = (Entry *)realloc(*arr, (size_t)(*n + 1) * sizeof(Entry));
    e = &(*arr)[(*n)++];
    memset(e, 0, sizeof(*e));
    return e;
}

/* fills a folder's entries: scripts, sub-folders, plugin pages, emulators */
static void load_folder(const char *dir, const char *skin_rel, Entry **out, int *n)
{
    char **names;
    int cnt = list_dir(dir, &names), i;
    for (i = 0; i < cnt; i++) {
        char p[1024], pj[1024], ac[1024], rel[256];
        Entry *e;
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        if (is_dir(p)) {
            snprintf(pj, sizeof(pj), "%s/plugin.json", p);
            snprintf(ac, sizeof(ac), "%s/action.config", p);
            e = add_entry(out, n);
            display_name(names[i], e->name, sizeof(e->name));
            snprintf(e->path, sizeof(e->path), "%s", p);
            if (exists(ac)) { e->kind = K_EMU; read_action_config(e, ac); }
            else if (exists(pj)) { e->kind = K_PLUGIN; snprintf(e->detail, sizeof(e->detail), "built-in page"); }
            else {
                /* a folder holding exactly its own script (apps/Menu style: 34_OpenTyrian/OpenTyrian.sh) is that script */
                char **kids; int nk = list_dir(p, &kids), k, script = -1, scripts = 0;
                for (k = 0; k < nk; k++) if (strlen(kids[k]) > 3 && !strcmp(kids[k] + strlen(kids[k]) - 3, ".sh")) { script = k; scripts++; }
                if (scripts == 1) {
                    e->kind = K_SCRIPT;
                    snprintf(e->path, sizeof(e->path), "%s/%s", p, kids[script]);
                    snprintf(e->detail, sizeof(e->detail), "%s", kids[script]);
                    e->icon = find_icon(skin_rel, e->name, p);
                } else {
                    e->kind = K_FOLDER;
                    snprintf(e->detail, sizeof(e->detail), "folder");
                }
                free_names(kids, nk);
            }
            if (!e->icon) e->icon = find_icon(skin_rel, e->name, NULL);
            if (e->kind == K_EMU && !e->icon) {
                snprintf(rel, sizeof(rel), "%s%s%s", skin_rel, *skin_rel ? "/" : "", names[i]);
                e->icon = find_icon(rel, e->name, NULL);
            }
        } else if (strlen(names[i]) > 3 && !strcmp(names[i] + strlen(names[i]) - 3, ".sh")) {
            e = add_entry(out, n);
            e->kind = K_SCRIPT;
            display_name(names[i], e->name, sizeof(e->name));
            snprintf(e->path, sizeof(e->path), "%s", p);
            snprintf(e->detail, sizeof(e->detail), "%s", names[i]);
            e->icon = find_icon(skin_rel, e->name, NULL);
        }
    }
    free_names(names, cnt);
}

static void load_roms(Entry *emu)
{
    char **names;
    int cnt = list_dir(emu->rom_dir, &names), i;
    for (i = 0; i < cnt; i++) {
        char p[1024];
        Entry *e;
        snprintf(p, sizeof(p), "%s/%s", emu->rom_dir, names[i]);
        if (is_dir(p) || (emu->ext[0] && !has_ext(names[i], emu->ext))) continue;
        e = add_entry(&emu->kids, &emu->nkids);
        e->kind = K_ROM;
        snprintf(e->name, sizeof(e->name), "%s", names[i]);
        { char *dot = strrchr(e->name, '.'); if (dot && dot != e->name) *dot = 0; }
        snprintf(e->path, sizeof(e->path), "%s", p);
        snprintf(e->detail, sizeof(e->detail), "%s", emu->detail[0] ? emu->detail : emu->name);
    }
    free_names(names, cnt);
    emu->loaded = 1;
}

/* the rail: every root's sub-folders are categories; its loose scripts form one */
static void load_roots(const char *roots)
{
    char list[2048], *tok, *save;
    snprintf(list, sizeof(list), "%s", roots);
    for (tok = strtok_r(list, ":", &save); tok && g_ncats < MAX_CATS - 1; tok = strtok_r(NULL, ":", &save)) {
        char **names;
        int cnt = list_dir(tok, &names), i;
        Category *loose = NULL;
        for (i = 0; i < cnt; i++) {
            char p[1024];
            snprintf(p, sizeof(p), "%s/%s", tok, names[i]);
            if (is_dir(p) && g_ncats < MAX_CATS - 1) {
                char pj[1024], ac[1024];
                Category *c;
                snprintf(pj, sizeof(pj), "%s/plugin.json", p);
                snprintf(ac, sizeof(ac), "%s/action.config", p);
                if (exists(pj) || exists(ac)) {   /* a page or an emulator at the top: goes into the loose group */
                    if (!loose) { loose = &g_cats[g_ncats++]; snprintf(loose->name, sizeof(loose->name), "Apps"); }
                    continue;
                }
                c = &g_cats[g_ncats++];
                display_name(names[i], c->name, sizeof(c->name));
                snprintf(c->dir, sizeof(c->dir), "%s", p);
                load_folder(p, names[i], &c->e, &c->n);
                c->icon = find_icon("", c->name, NULL);
                if (c->n == 0) { g_ncats--; memset(c, 0, sizeof(*c)); }
            } else if (!is_dir(p) && strlen(names[i]) > 3 && !strcmp(names[i] + strlen(names[i]) - 3, ".sh")) {
                if (!loose) { loose = &g_cats[g_ncats++]; snprintf(loose->name, sizeof(loose->name), "Apps"); }
            }
        }
        if (loose) {
            /* loose scripts, plugin pages and emulators at the root, in order */
            Entry *all = NULL; int n = 0, k;
            load_folder(tok, "", &all, &n);
            for (k = 0; k < n; k++) if (all[k].kind != K_FOLDER) { Entry *e = add_entry(&loose->e, &loose->n); *e = all[k]; }
            free(all);
            snprintf(loose->dir, sizeof(loose->dir), "%s", tok);
            loose->icon = find_icon("", "GS", NULL);
            if (!loose->icon) { char p[1024]; snprintf(p, sizeof(p), "%s/sysgo/gameshell/icons/GS.png", g_skin); loose->icon = icon_get(p); }
        }
        free_names(names, cnt);
    }
}

/* ---- recent and favourites -------------------------------------------------------- */

#define MAX_RECENT 8
static char g_recent[MAX_RECENT][512];
static int g_nrecent;
static char g_favs[64][512];
static int g_nfavs;

static void list_load(const char *file, char (*arr)[512], int *n, int max)
{
    char p[1024], line[512];
    FILE *f;
    snprintf(p, sizeof(p), "%s/%s", g_config_dir, file);
    *n = 0;
    if (!(f = fopen(p, "r"))) return;
    while (*n < max && fgets(line, sizeof(line), f)) { trim(line); if (line[0]) snprintf(arr[(*n)++], 512, "%s", line); }
    fclose(f);
}

static void list_save(const char *file, char (*arr)[512], int n)
{
    char p[1024];
    FILE *f;
    int i;
    {   /* mkdir -p */
        char *q = g_config_dir + 1;
        for (; *q; q++) if (*q == '/') { *q = 0; mkdir(g_config_dir, 0755); *q = '/'; }
        mkdir(g_config_dir, 0755);
    }
    snprintf(p, sizeof(p), "%s/%s", g_config_dir, file);
    if (!(f = fopen(p, "w"))) return;
    for (i = 0; i < n; i++) fprintf(f, "%s\n", arr[i]);
    fclose(f);
}

static int list_find(char (*arr)[512], int n, const char *path) { int i; for (i = 0; i < n; i++) if (!strcmp(arr[i], path)) return i; return -1; }

static void recent_push(const char *path)
{
    int i = list_find(g_recent, g_nrecent, path);
    if (i < 0) { if (g_nrecent < MAX_RECENT) g_nrecent++; i = g_nrecent - 1; }
    for (; i > 0; i--) memcpy(g_recent[i], g_recent[i - 1], 512);
    snprintf(g_recent[0], 512, "%s", path);
    list_save("recent", g_recent, g_nrecent);
}

static Entry *find_entry_rec(Entry *arr, int n, const char *path)
{
    int i;
    for (i = 0; i < n; i++) {
        Entry *k;
        if (!strcmp(arr[i].path, path)) return &arr[i];
        if (arr[i].kids && (k = find_entry_rec(arr[i].kids, arr[i].nkids, path))) return k;
    }
    return NULL;
}

static Entry *find_entry(const char *path)
{
    int c;
    for (c = 0; c < g_ncats; c++) { Entry *e = find_entry_rec(g_cats[c].e, g_cats[c].n, path); if (e) return e; }
    return NULL;
}

/* ---- navigation state ----------------------------------------------------------- */

typedef struct Level { Entry *e; int n, sel; float scroll; char title[96]; Entry *owner; } Level;
static Level g_lv[6];
static int g_depth;                 /* g_lv[0] = the category's entries */
static int g_cat, g_cat_sel_of[MAX_CATS];
static Entry g_recent_e[MAX_RECENT];
static int g_recent_cat = -1;
static int g_dirty = 1;
static float g_rail_x, g_rail_target;      /* rail scroll (pixels) */
static float g_sel_y, g_sel_target;        /* list highlight */
static float g_lift[MAX_CATS];             /* 0..1 per rail icon */
static uint64_t g_last_anim;
static char g_toast[128];
static uint64_t g_toast_until;
static char g_launch_cmd[1024], g_launch_cwd[512];
static int g_launch_pending, g_launch_aware, g_running;
static Entry *g_running_e;

static void toast(const char *s, uint64_t now) { snprintf(g_toast, sizeof(g_toast), "%s", s); g_toast_until = now + 2500000u; g_dirty = 1; }

static void build_recent_cat(void)
{
    Category *c;
    int i;
    if (g_recent_cat < 0) {
        if (g_nrecent == 0) return;
        /* insert at the front */
        memmove(&g_cats[1], &g_cats[0], (size_t)g_ncats * sizeof(Category));
        g_ncats++;
        g_recent_cat = 0;
        c = &g_cats[0];
        memset(c, 0, sizeof(*c));
        snprintf(c->name, sizeof(c->name), "Recent");
        { char p[1024]; snprintf(p, sizeof(p), "%s/sysgo/gameshell/icons/app.png", g_skin); c->icon = icon_get(p); }
    }
    c = &g_cats[g_recent_cat];
    c->n = 0; c->e = g_recent_e;
    for (i = 0; i < g_nrecent; i++) {
        Entry *e = find_entry(g_recent[i]);
        if (e) { g_recent_e[c->n] = *e; g_recent_e[c->n].kids = NULL; g_recent_e[c->n].nkids = 0; c->n++; }
    }
}

static void open_category(int cat)
{
    Category *c = &g_cats[cat];
    g_cat = cat;
    g_depth = 0;
    g_lv[0].e = c->e; g_lv[0].n = c->n; g_lv[0].sel = g_cat_sel_of[cat] < c->n ? g_cat_sel_of[cat] : 0;
    g_lv[0].scroll = 0; g_lv[0].owner = NULL;
    snprintf(g_lv[0].title, sizeof(g_lv[0].title), "%s", c->name);
    g_dirty = 1;
}

static Entry *current_entry(void)
{
    Level *l = &g_lv[g_depth];
    return l->n ? &l->e[l->sel] : NULL;
}

static void shell_quote(char *out, size_t n, const char *s)
{
    size_t o = 0;
    if (o < n) out[o++] = '\'';
    for (; *s && o + 5 < n; s++) {
        if (*s == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
        else out[o++] = *s;
    }
    if (o < n) out[o++] = '\'';
    out[o < n ? o : n - 1] = 0;
}

static int script_is_pspexp_aware(const char *path)
{
    uint8_t *d; size_t len; int r;
    if (read_file(path, &d, &len) < 0) return 0;
    d = (uint8_t *)realloc(d, len + 1); d[len] = 0;
    r = strstr((char *)d, "pspexp-run") != NULL || strstr((char *)d, "pspexp-exec") != NULL || strstr((char *)d, "pspexp-") != NULL;
    free(d);
    return r;
}

static void launch(Entry *e, Entry *owner, uint64_t now)
{
    char q[1024];
    const char *slash;
    if (e->kind == K_SCRIPT) {
        shell_quote(q, sizeof(q), e->path);
        snprintf(g_launch_cmd, sizeof(g_launch_cmd), "exec /bin/sh %s", q);
        g_launch_aware = script_is_pspexp_aware(e->path);
    } else if (e->kind == K_ROM && owner) {
        shell_quote(q, sizeof(q), e->path);
        snprintf(g_launch_cmd, sizeof(g_launch_cmd), "exec %s %s %s", owner->launcher, owner->rom_so, q);
        g_launch_aware = strstr(owner->launcher, "pspexp-") != NULL;
    } else if (e->kind == K_PLUGIN && !strcasecmp(e->name, "PowerOFF")) {
        snprintf(g_launch_cmd, sizeof(g_launch_cmd), "exec sudo -n poweroff");
        g_launch_aware = 1;
    } else { toast("Not available in this launcher yet", now); return; }
    slash = strrchr(e->path, '/');
    snprintf(g_launch_cwd, sizeof(g_launch_cwd), "%.*s", slash ? (int)(slash - e->path) : 1, slash ? e->path : ".");
    g_launch_pending = 1;
    g_running = 1;
    g_running_e = e;
    recent_push(e->path);
    g_dirty = 1;
}

/* ---- input ------------------------------------------------------------------------ */

void launcher_input(uint32_t b, uint32_t pressed, int up, int down, int left, int right, uint64_t now)
{
    Level *l = &g_lv[g_depth];
    (void)b;
    if (g_running) return;   /* a game has the screen */
    if (g_ncats == 0) return;
    if (left || right) {
        if (g_depth == 0) {
            int c = g_cat + (right ? 1 : -1);
            if (c < 0) c = g_ncats - 1;
            if (c >= g_ncats) c = 0;
            g_cat_sel_of[g_cat] = g_lv[0].sel;
            open_category(c);
        } else {   /* inside a folder: page */
            l->sel += right ? 5 : -5;
            if (l->sel < 0) l->sel = 0;
            if (l->sel >= l->n) l->sel = l->n - 1;
            g_dirty = 1;
        }
    }
    if (up && l->n) { l->sel = (l->sel + l->n - 1) % l->n; g_dirty = 1; }
    if (down && l->n) { l->sel = (l->sel + 1) % l->n; g_dirty = 1; }
    if (b & PSPEXP_BUTTON_LTRIGGER && (pressed & PSPEXP_BUTTON_LTRIGGER) && l->n) { l->sel = l->sel >= 5 ? l->sel - 5 : 0; g_dirty = 1; }
    if (b & PSPEXP_BUTTON_RTRIGGER && (pressed & PSPEXP_BUTTON_RTRIGGER) && l->n) { l->sel = l->sel + 5 < l->n ? l->sel + 5 : l->n - 1; g_dirty = 1; }
    if (pressed & PSPEXP_BUTTON_CROSS) {
        Entry *e = current_entry();
        if (!e) return;
        if (e->kind == K_FOLDER || e->kind == K_EMU) {
            if (g_depth + 1 >= (int)(sizeof(g_lv) / sizeof(g_lv[0]))) return;
            if (!e->loaded) {
                if (e->kind == K_EMU) load_roms(e);
                else { load_folder(e->path, "", &e->kids, &e->nkids); e->loaded = 1; }
            }
            if (e->nkids == 0) { toast(e->kind == K_EMU ? "No ROMs in that folder" : "Empty folder", now); return; }
            g_depth++;
            g_lv[g_depth].e = e->kids; g_lv[g_depth].n = e->nkids; g_lv[g_depth].sel = 0; g_lv[g_depth].scroll = 0;
            g_lv[g_depth].owner = e;
            snprintf(g_lv[g_depth].title, sizeof(g_lv[g_depth].title), "%s", e->name);
            g_dirty = 1;
        } else launch(e, l->owner, now);
    }
    if (pressed & PSPEXP_BUTTON_CIRCLE) {
        if (g_depth > 0) { g_depth--; g_dirty = 1; }
    }
    if (pressed & PSPEXP_BUTTON_SQUARE) {
        Entry *e = current_entry();
        if (e && (e->kind == K_SCRIPT || e->kind == K_ROM)) {
            int i = list_find(g_favs, g_nfavs, e->path);
            Entry *orig;
            if (i >= 0) { memmove(g_favs[i], g_favs[i + 1], (size_t)(g_nfavs - i - 1) * 512); g_nfavs--; e->fav = 0; }
            else if (g_nfavs < 64) { snprintf(g_favs[g_nfavs++], 512, "%s", e->path); e->fav = 1; }
            if ((orig = find_entry(e->path)) && orig != e) orig->fav = e->fav;
            list_save("favourites", g_favs, g_nfavs);
            toast(e->fav ? "Added to favourites" : "Removed from favourites", now);
        }
    }
    if (pressed & PSPEXP_BUTTON_TRIANGLE) {
        Entry *e = current_entry();
        char s[200];
        if (e) { snprintf(s, sizeof(s), "%s", e->path); toast(s, now); }
    }
}

int launcher_take_launch(char *cmd, size_t cmd_len, char *cwd, size_t cwd_len, int *pspexp_aware)
{
    if (!g_launch_pending) return 0;
    g_launch_pending = 0;
    snprintf(cmd, cmd_len, "%s", g_launch_cmd);
    snprintf(cwd, cwd_len, "%s", g_launch_cwd);
    *pspexp_aware = g_launch_aware;
    return 1;
}

void launcher_child_exited(int status)
{
    char s[160];
    g_running = 0;
    if (status != 0 && g_running_e) { snprintf(s, sizeof(s), "%s ended with status %d", g_running_e->name, status); toast(s, pspexp_time_us()); }
    build_recent_cat();
    if (g_recent_cat >= 0 && g_cat == g_recent_cat) open_category(g_cat);
    g_dirty = 1;
}

void launcher_invalidate(void) { g_dirty = 1; }

/* ---- status sources ---------------------------------------------------------------- */

static int read_int_file(const char *p, int *v)
{
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    if (fscanf(f, "%d", v) != 1) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

static int battery(int *charging)
{
    static const char *dirs[] = { "/sys/class/power_supply/axp20x-battery", "/sys/class/power_supply/battery", "/sys/class/power_supply/BAT0", NULL };
    char p[256], st[32];
    int i, v;
    *charging = 0;
    for (i = 0; dirs[i]; i++) {
        snprintf(p, sizeof(p), "%s/capacity", dirs[i]);
        if (read_int_file(p, &v) == 0) {
            FILE *f;
            snprintf(p, sizeof(p), "%s/status", dirs[i]);
            if ((f = fopen(p, "r"))) { if (fgets(st, sizeof(st), f) && !strncmp(st, "Charging", 8)) *charging = 1; fclose(f); }
            return v;
        }
    }
    return -1;
}

static int wifi_level(void)   /* 0..4, -1 unknown */
{
    FILE *f = fopen("/proc/net/wireless", "r");
    char line[256];
    int q = -1;
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        char name[32]; int status; float link, level;
        if (sscanf(line, " %31[^:]: %d %f %f", name, &status, &link, &level) == 4) { q = (int)link; break; }
    }
    fclose(f);
    if (q < 0) return -1;
    return q >= 60 ? 4 : q >= 45 ? 3 : q >= 30 ? 2 : q > 0 ? 1 : 0;
}

/* ---- drawing ------------------------------------------------------------------------ */

static void rect(int x, int y, int w, int h, uint32_t c, int a) { pspexp_fill_rect(g_ctx, (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, rgb(c), (uint8_t)a); }

/* a card with 2 px "rounded" corners: the body and two inset bands */
static void card(int x, int y, int w, int h, uint32_t c, int a)
{
    rect(x + 2, y, w - 4, h, c, a);
    rect(x, y + 2, 2, h - 4, c, a);
    rect(x + w - 2, y + 2, 2, h - 4, c, a);
    rect(x + 1, y + 1, 1, 1, c, a / 2); rect(x + w - 2, y + 1, 1, 1, c, a / 2);
    rect(x + 1, y + h - 2, 1, 1, c, a / 2); rect(x + w - 2, y + h - 2, 1, 1, c, a / 2);
}

static void line(int x0, int y0, int x1, int y1, uint32_t c, int a) { pspexp_draw_line(g_ctx, (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, rgb(c), (uint8_t)a); }

/* PSP button glyphs, 9 px */
static void glyph_button(int x, int y, char which, uint32_t c)
{
    switch (which) {
    case 'x': line(x, y, x + 8, y + 8, c, 255); line(x + 8, y, x, y + 8, c, 255); break;
    case 'o': line(x + 2, y, x + 6, y, c, 255); line(x + 2, y + 8, x + 6, y + 8, c, 255); line(x, y + 2, x, y + 6, c, 255); line(x + 8, y + 2, x + 8, y + 6, c, 255);
              line(x, y + 2, x + 2, y, c, 255); line(x + 6, y, x + 8, y + 2, c, 255); line(x, y + 6, x + 2, y + 8, c, 255); line(x + 6, y + 8, x + 8, y + 6, c, 255); break;
    case 't': line(x + 4, y, x, y + 8, c, 255); line(x + 4, y, x + 8, y + 8, c, 255); line(x, y + 8, x + 8, y + 8, c, 255); break;
    case 's': pspexp_draw_rect(g_ctx, (int16_t)x, (int16_t)y, 9, 9, rgb(c)); break;
    default: break;
    }
}

static uint32_t name_colour(const char *s)
{
    static const uint32_t pal[] = { 0x2f6fb5, 0x8e44ad, 0x16a085, 0xc0392b, 0xd35400, 0x27ae60, 0x2980b9, 0x7f8c8d, 0xb5892f };
    unsigned h = 5381;
    for (; *s; s++) h = h * 33u + (unsigned char)*s;
    return pal[h % (sizeof(pal) / sizeof(pal[0]))];
}

/* cover for an entry without an icon: its initials on a colour from its name */
static void generated_cover(const Entry *e, int x, int y, int box, int alpha)
{
    char ini[4] = { 0, 0, 0, 0 };
    const char *s = e->name;
    int n = 0, w;
    while (*s && n < 2) {
        if ((*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || (n == 0 && *s >= 'a' && *s <= 'z')) ini[n++] = (char)(*s >= 'a' ? *s - 32 : *s);
        s++;
    }
    if (!n) ini[n++] = '?';
    card(x, y, box, box, name_colour(e->name), alpha);
    if (box >= 48) {
        w = text_width(&F_TITLE, ini, -1);
        text_draw(&F_TITLE, x + (box - w) / 2, y + (box - F_TITLE.height) / 2, ini, C_TEXT, 0);
    } else {
        w = text_width(&F_UI, ini, -1);
        text_draw(&F_UI, x + (box - w) / 2, y + (box - F_UI.height) / 2, ini, C_TEXT, 0);
    }
}

static const char *kind_word(const Entry *e)
{
    switch (e->kind) {
    case K_FOLDER: return "folder";
    case K_PLUGIN: return "page";
    case K_EMU: return "emulator";
    case K_ROM: return "rom";
    default: return "game";
    }
}

static float approach(float cur, float target, float k)
{
    float d = target - cur;
    if (fabsf(d) < 0.4f) return target;
    return cur + d * k;
}

#define RAIL_Y 26
#define RAIL_BOX 56
#define RAIL_GAP 18
#define LIST_Y 114
#define ROW_H 25
#define LIST_X 164
#define LIST_W 300
#define ROWS_VISIBLE 5

int launcher_needs_frame(uint64_t now)
{
    int i;
    if (g_dirty) return 1;
    if (g_toast[0] && now >= g_toast_until) { g_toast[0] = 0; return 1; }
    if (fabsf(g_rail_x - g_rail_target) >= 0.4f || fabsf(g_sel_y - g_sel_target) >= 0.4f) return 1;
    for (i = 0; i < g_ncats; i++) if (fabsf(g_lift[i] - (i == g_cat ? 1.0f : 0.0f)) >= 0.02f) return 1;
    return 0;
}

static void draw_status(uint64_t now, int connected)
{
    char s[64];
    time_t t = time(NULL);
    struct tm tmv;
    int x, lvl, chg, bat, i;
    (void)now;
    rect(0, 0, SCREEN_W, 20, C_STRIP, 255);
    localtime_r(&t, &tmv);
    strftime(s, sizeof(s), "%H:%M", &tmv);
    text_draw(&F_UI, 12, 2, s, C_TEXT, 0);
    strftime(s, sizeof(s), "%a %d %b", &tmv);
    text_draw(&F_SMALL, 56, 4, s, C_MUTED, 0);
    /* right side: PSP link, Wi-Fi, battery */
    x = SCREEN_W - 12;
    bat = battery(&chg);
    if (bat >= 0) {
        int w = 22, fill = (w - 4) * bat / 100;
        snprintf(s, sizeof(s), "%d%%", bat);
        x -= text_width(&F_SMALL, s, -1);
        text_draw(&F_SMALL, x, 4, s, C_MUTED, 0);
        x -= w + 6;
        pspexp_draw_rect(g_ctx, (int16_t)x, 5, (int16_t)w, 10, rgb(C_MUTED));
        rect(x + w, 8, 2, 4, C_MUTED, 255);
        rect(x + 2, 7, fill, 6, chg ? 0x3fb950 : bat < 20 ? 0xf85149 : C_TEXT, 255);
        x -= 14;
    }
    lvl = wifi_level();
    if (lvl >= 0) {
        for (i = 0; i < 4; i++) rect(x - 16 + i * 4, 14 - i * 3, 3, 3 + i * 3, i < lvl ? C_TEXT : C_DIM, 255);
        x -= 24;
    }
    rect(x - 8, 7, 6, 6, connected ? 0x3fb950 : 0xf85149, 255);
    x -= 8 + 6;
    text_draw(&F_SMALL, x - text_width(&F_SMALL, "PSP", -1), 4, "PSP", C_MUTED, 0);
}

static void draw_rail(void)
{
    int i, x, total = g_ncats * (RAIL_BOX + RAIL_GAP) - RAIL_GAP;
    /* the selected icon is kept near the centre when the rail overflows */
    float sel_center = (float)(g_cat * (RAIL_BOX + RAIL_GAP) + RAIL_BOX / 2);
    if (total <= SCREEN_W - 40) g_rail_target = (float)((SCREEN_W - total) / 2);
    else {
        g_rail_target = SCREEN_W / 2.0f - sel_center;
        if (g_rail_target > 20) g_rail_target = 20;
        if (g_rail_target < SCREEN_W - 20 - total) g_rail_target = (float)(SCREEN_W - 20 - total);
    }
    g_rail_x = approach(g_rail_x, g_rail_target, 0.22f);
    for (i = 0; i < g_ncats; i++) {
        float lift = g_lift[i] = approach(g_lift[i], i == g_cat ? 1.0f : 0.0f, 0.25f);
        int box = RAIL_BOX + (int)(lift * 14.0f);
        int cx = (int)(g_rail_x + i * (RAIL_BOX + RAIL_GAP)) + RAIL_BOX / 2;
        int y = RAIL_Y + 8 - (int)(lift * 8.0f);
        x = cx - box / 2;
        if (x + box < 0 || x > SCREEN_W) continue;
        if (lift > 0.05f) card(x - 4, y - 4, box + 8, box + 8, C_ACCENT, (int)(lift * 70.0f) + 20);
        card(x, y, box, box, C_CARD_HI, 255);
        if (!icon_draw(g_cats[i].icon, x + 4, y + 4, box - 8, 200 + (int)(lift * 55.0f))) {
            Entry fake; memset(&fake, 0, sizeof(fake)); snprintf(fake.name, sizeof(fake.name), "%s", g_cats[i].name);
            generated_cover(&fake, x + 4, y + 4, box - 8, 200 + (int)(lift * 55.0f));
        }
        if (i == g_cat) {
            int w = text_width(&F_UI, g_cats[i].name, -1);
            text_draw(&F_UI, cx - w / 2, RAIL_Y + RAIL_BOX + 14, g_cats[i].name, C_TEXT, 0);
        }
    }
}

static void draw_list(void)
{
    Level *l = &g_lv[g_depth];
    int i, first, y, cover_x = 16, cover_y = LIST_Y + 4, cover = 128;
    Entry *cur = current_entry();
    char s[160];
    /* cover panel */
    if (cur) {
        if (!icon_draw(cur->icon, cover_x, cover_y, cover, 255)) generated_cover(cur, cover_x, cover_y, cover, 255);
        if (cur->fav) { card(cover_x + cover - 24, cover_y + 6, 18, 18, C_ACCENT, 240); rect(cover_x + cover - 19, cover_y + 11, 8, 8, C_TEXT, 255); }
    } else {
        card(cover_x, cover_y, cover, cover, C_CARD, 255);
        text_draw(&F_SMALL, cover_x + 30, cover_y + 58, "nothing here", C_MUTED, 0);
    }
    /* breadcrumb when inside a folder */
    if (g_depth > 0) {
        snprintf(s, sizeof(s), "%s  /  %s", g_lv[0].title, l->title);
        text_draw(&F_SMALL, LIST_X, LIST_Y - 12, s, C_MUTED, LIST_W);
    }
    /* rows: the selection kept in view */
    if (l->sel < (int)l->scroll) l->scroll = (float)l->sel;
    if (l->sel >= (int)l->scroll + ROWS_VISIBLE) l->scroll = (float)(l->sel - ROWS_VISIBLE + 1);
    first = (int)l->scroll;
    g_sel_target = (float)(LIST_Y + (l->sel - first) * ROW_H);
    g_sel_y = approach(g_sel_y, g_sel_target, 0.35f);
    pspexp_set_clip(g_ctx, LIST_X - 4, LIST_Y - 2, LIST_W + 8, ROWS_VISIBLE * ROW_H + 4);
    if (l->n) card(LIST_X - 4, (int)g_sel_y, LIST_W + 8, ROW_H, C_CARD_HI, 255);
    if (l->n) rect(LIST_X - 4, (int)g_sel_y + 3, 3, ROW_H - 6, C_ACCENT, 255);
    for (i = first; i < l->n && i < first + ROWS_VISIBLE; i++) {
        Entry *e = &l->e[i];
        int sel = i == l->sel;
        y = LIST_Y + (i - first) * ROW_H;
        text_draw(&F_UI, LIST_X + 8, y + 3, e->name, sel ? C_TEXT : 0xb8c0cc, LIST_W - 90);
        if (e->fav) rect(LIST_X + 8 + text_width(&F_UI, e->name, -1) + 8, y + 10, 5, 5, C_ACCENT, 255);
        {
            const char *k = kind_word(e);
            int w = text_width(&F_SMALL, k, -1);
            text_draw(&F_SMALL, LIST_X + LIST_W - w - 8, y + 7, k, sel ? C_MUTED : C_DIM, 0);
        }
        if (e->kind == K_FOLDER || e->kind == K_EMU) { line(LIST_X + LIST_W - 60, y + 9, LIST_X + LIST_W - 56, y + 13, C_MUTED, 255); line(LIST_X + LIST_W - 56, y + 13, LIST_X + LIST_W - 60, y + 17, C_MUTED, 255); }
    }
    pspexp_set_clip(g_ctx, 0, 0, SCREEN_W, SCREEN_H);
    /* scrollbar */
    if (l->n > ROWS_VISIBLE) {
        int h = ROWS_VISIBLE * ROW_H, th = h * ROWS_VISIBLE / l->n, ty = h * first / l->n;
        if (th < 8) th = 8;
        rect(LIST_X + LIST_W + 8, LIST_Y, 2, h, C_DIM, 255);
        rect(LIST_X + LIST_W + 8, LIST_Y + ty, 2, th, C_MUTED, 255);
    }
    /* detail line under the list */
    if (cur) {
        snprintf(s, sizeof(s), "%s", cur->detail[0] ? cur->detail : cur->path);
        text_draw(&F_SMALL, LIST_X, LIST_Y + ROWS_VISIBLE * ROW_H + 4, s, C_MUTED, LIST_W);
        if (l->n) { snprintf(s, sizeof(s), "%d / %d", l->sel + 1, l->n); text_draw(&F_SMALL, LIST_X + LIST_W - text_width(&F_SMALL, s, -1), LIST_Y + ROWS_VISIBLE * ROW_H + 4, s, C_DIM, 0); }
    }
}

static void draw_hints(void)
{
    Entry *cur = current_entry();
    int x = 16, y = SCREEN_H - 16;
    const char *a = cur && (cur->kind == K_FOLDER || cur->kind == K_EMU) ? "open" : "launch";
    rect(0, SCREEN_H - 20, SCREEN_W, 20, C_STRIP, 255);
    glyph_button(x, y + 1, 'x', C_ACCENT); x += 13; x += text_draw(&F_SMALL, x, y - 2, a, C_MUTED, 0) + 16;
    if (g_depth > 0) { glyph_button(x, y + 1, 'o', C_ACCENT); x += 13; x += text_draw(&F_SMALL, x, y - 2, "back", C_MUTED, 0) + 16; }
    glyph_button(x, y + 1, 's', C_ACCENT); x += 13; x += text_draw(&F_SMALL, x, y - 2, cur && cur->fav ? "unfavourite" : "favourite", C_MUTED, 0) + 16;
    glyph_button(x, y + 1, 't', C_ACCENT); x += 13; x += text_draw(&F_SMALL, x, y - 2, "where", C_MUTED, 0) + 16;
    text_draw(&F_SMALL, SCREEN_W - 16 - text_width(&F_SMALL, "hold select: console", -1), y - 2, "hold select: console", C_DIM, 0);
}

void launcher_render(PspExpContext *ctx, uint64_t now, int psp_connected)
{
    g_ctx = ctx;
    g_sn = 0;
    pspexp_begin_frame(ctx);
    pspexp_set_clip(ctx, 0, 0, SCREEN_W, SCREEN_H);
    rect(0, 0, SCREEN_W, SCREEN_H, C_BG, 255);
    if (!g_have_font) {
        pspexp_present(ctx);
        g_dirty = 0;
        return;
    }
    if (g_running && g_running_e) {
        char s[160];
        int w;
        snprintf(s, sizeof(s), "%s", g_running_e->name);
        w = text_width(&F_TITLE, s, -1);
        if (!icon_draw(g_running_e->icon, SCREEN_W / 2 - 40, 70, 80, 255)) generated_cover(g_running_e, SCREEN_W / 2 - 40, 70, 80, 255);
        text_draw(&F_TITLE, (SCREEN_W - w) / 2, 164, s, C_TEXT, 0);
        w = text_width(&F_UI, "is running", -1);
        text_draw(&F_UI, (SCREEN_W - w) / 2, 192, "is running", C_MUTED, 0);
        text_flush();
        pspexp_present(ctx);
        g_dirty = 0;
        return;
    }
    draw_status(now, psp_connected);
    draw_rail();
    draw_list();
    draw_hints();
    if (g_toast[0] && now < g_toast_until) {
        int w = text_width(&F_UI, g_toast, -1) + 24;
        if (w > SCREEN_W - 32) w = SCREEN_W - 32;
        card((SCREEN_W - w) / 2, SCREEN_H - 50, w, 26, C_CARD_HI, 240);
        text_draw(&F_UI, (SCREEN_W - w) / 2 + 12, SCREEN_H - 46, g_toast, C_TEXT, w - 24);
    }
    text_flush();
    pspexp_present(ctx);
    g_dirty = 0;
    g_last_anim = now;
}

/* ---- init ---------------------------------------------------------------------------- */

int launcher_init(PspExpContext *ctx, const char *roots, const char *skin)
{
    const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
    char cfg[1024];
    FILE *f;
    int i, c;
    g_ctx = ctx;
    snprintf(g_skin, sizeof(g_skin), "%s", skin ? skin : "");
    snprintf(g_config_dir, sizeof(g_config_dir), "%s/.config/pspexp-launcher", home);
    /* accent from the skin's config.cfg ([Colors] High = #rrggbb) */
    snprintf(cfg, sizeof(cfg), "%s/config.cfg", g_skin);
    if ((f = fopen(cfg, "r"))) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            char *h = strstr(line, "High");
            if (h && (h = strchr(h, '#'))) { C_ACCENT = (uint32_t)strtoul(h + 1, NULL, 16); break; }
        }
        fclose(f);
    }
    fonts_init(ctx, skin);
    load_roots(roots);
    list_load("recent", g_recent, &g_nrecent, MAX_RECENT);
    list_load("favourites", g_favs, &g_nfavs, 64);
    for (c = 0; c < g_ncats; c++) for (i = 0; i < g_cats[c].n; i++) g_cats[c].e[i].fav = list_find(g_favs, g_nfavs, g_cats[c].e[i].path) >= 0;
    build_recent_cat();
    if (g_ncats == 0) { fprintf(stderr, "[launcher] nothing to show in %s\n", roots); return -1; }
    /* start on the first real category, not Recent */
    open_category(g_recent_cat == 0 && g_ncats > 1 ? 1 : 0);
    g_rail_x = g_rail_target = 0;
    g_sel_y = g_sel_target = LIST_Y;
    fprintf(stderr, "[launcher] %d categories, %d icons, font %s\n", g_ncats, g_nicons, g_have_font ? "ok" : "missing");
    return 0;
}
