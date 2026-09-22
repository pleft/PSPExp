/*
 * sprite-demo: the v0.1 proof of concept (section 16).
 *
 * One procedurally generated atlas is uploaded once. The host owns a player
 * object and moves it from the PSP's controls; a field of background sprites
 * provides load. The same binary runs through the Null executor, the SDL
 * executor, or a real PSP over USB, and survives a cable pull.
 *
 * Controls (PSP / SDL keyboard):
 *   d-pad or analog / arrows or IJKL   move
 *   cross / Z        next player frame
 *   circle / X       next tint
 *   square / A       toggle the background field
 *   triangle / S     cycle the field size
 *   start / Return   reset
 */
#include "../../host/libpspexp/pspexp.h"
#include "../../host/executor-null/pspexp_null.h"
#ifdef PSPEXP_HAVE_USB
#include "../../host/transport-usb/pspexp_usb.h"
#endif
#ifdef PSPEXP_HAVE_SDL
#include "../../host/executor-sdl/pspexp_sdl.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATLAS_ID      1
#define ATLAS_W       128
#define ATLAS_H       64
#define FRAME_PLAYER0 0     /* four 32x32 player frames */
#define FRAME_STAR    4     /* 8x8 */
#define FRAME_BLOCK   5     /* 16x16 */
#define MAX_FIELD     8000

typedef struct Star { int16_t x, y; int8_t dx, dy; uint16_t color; } Star;

static uint16_t g_atlas[ATLAS_W * ATLAS_H];
static PspExpFrame g_frames[6];
static Star g_field[MAX_FIELD];
static PspExpSpriteRec g_recs[MAX_FIELD];

static uint16_t rgba5551(int r, int g, int b, int a)
{
    return (uint16_t)(((a ? 1 : 0) << 15) | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
}

static void put(int x, int y, uint16_t c) { g_atlas[y * ATLAS_W + x] = c; }

static void build_atlas(void)
{
    int x, y, i;
    memset(g_atlas, 0, sizeof(g_atlas));
    for (i = 0; i < 4; i++) {
        int ox = i * 32;
        for (y = 0; y < 32; y++)
            for (x = 0; x < 32; x++) {
                int cx = x - 16, cy = y - 16, on = 0;
                uint16_t c = 0;
                switch (i) {
                case 0: on = cx * cx + cy * cy <= 14 * 14; c = rgba5551(240, 80, 60, 1); break;           /* disc */
                case 1: on = abs(cx) <= 13 && abs(cy) <= 13; c = rgba5551(70, 200, 90, 1); break;        /* square */
                case 2: on = abs(cx) + abs(cy) <= 15; c = rgba5551(80, 120, 250, 1); break;              /* diamond */
                default: on = cx * cx + cy * cy <= 15 * 15 && cx * cx + cy * cy >= 9 * 9; c = rgba5551(250, 220, 60, 1); break; /* ring */
                }
                if (on && ((x + y) & 7) == 0) c = rgba5551(255, 255, 255, 1);   /* a few highlight texels */
                if (on) put(ox + x, y, c);
            }
        g_frames[i].u0 = (uint16_t)ox; g_frames[i].v0 = 0; g_frames[i].w = 32; g_frames[i].h = 32;
    }
    /* 8x8 star */
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
            if (abs(x - 3) + abs(y - 3) <= 3 || abs(x - 4) + abs(y - 4) <= 3) put(x, 32 + y, rgba5551(255, 255, 255, 1));
    g_frames[FRAME_STAR].u0 = 0; g_frames[FRAME_STAR].v0 = 32; g_frames[FRAME_STAR].w = 8; g_frames[FRAME_STAR].h = 8;
    /* 16x16 block with a border */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++)
            put(16 + x, 32 + y, (x == 0 || y == 0 || x == 15 || y == 15) ? rgba5551(255, 255, 255, 1) : rgba5551(90, 90, 110, 1));
    g_frames[FRAME_BLOCK].u0 = 16; g_frames[FRAME_BLOCK].v0 = 32; g_frames[FRAME_BLOCK].w = 16; g_frames[FRAME_BLOCK].h = 16;
}

static uint32_t g_rng = 12345;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

static void build_field(void)
{
    int i;
    for (i = 0; i < MAX_FIELD; i++) {
        g_field[i].x = (int16_t)(rnd() % 480);
        g_field[i].y = (int16_t)(rnd() % 248);
        g_field[i].dx = (int8_t)((int)(rnd() % 3) - 1);
        g_field[i].dy = (int8_t)((int)(rnd() % 3) - 1);
        g_field[i].color = pspexp_color_5650((uint8_t)(120 + rnd() % 136), (uint8_t)(120 + rnd() % 136), (uint8_t)(160 + rnd() % 96));
    }
}

static void log_fn(void *u, const char *msg) { (void)u; fprintf(stderr, "[pspexp] %s\n", msg); }

static void usage(void)
{
    printf("usage: pspexp-sprite-demo [--null|--sdl|--usb] [--field N] [--frames N] [--scale N] [--verbose]\n"
           "  --null       Null executor (no rendering), useful with --frames\n"
           "  --sdl        SDL executor window (default when built with SDL and no PSP is wanted)\n"
           "  --usb        real PSP over USB (default when built with libusb)\n"
           "  --field N    background sprites (default 500, max %d)\n"
           "  --frames N   exit after N frames (default: run until quit)\n", MAX_FIELD);
}

int main(int argc, char **argv)
{
    enum { MODE_USB, MODE_SDL, MODE_NULL } mode;
    int field = 500, max_frames = 0, scale = 2, verbose = 0, i;
    PspExpTransport *t = NULL;
    PspExpNullExecutor *nul = NULL;
#ifdef PSPEXP_HAVE_SDL
    PspExpSdlExecutor *sdl = NULL;
#endif
#ifdef PSPEXP_HAVE_USB
    PspExpUsbOptions usb_opt;
#endif
    PspExpContext *ctx;
    PspExpTextureDesc desc;
    char err[256] = "";
    int px = 224, py = 120, pframe = 0, ptint = 0, show_field = 1;
    uint32_t prev_buttons = 0;
    uint32_t frames = 0;
    uint64_t last_report = 0, report_frames = 0;
    static const uint16_t tints[4] = { PSPEXP_COLOR_NONE, 0xF800, 0x07E0, 0x001F };
    static const int field_sizes[4] = { 100, 500, 2000, 5000 };
    int field_idx = 1;

#if defined(PSPEXP_HAVE_USB)
    mode = MODE_USB;
#elif defined(PSPEXP_HAVE_SDL)
    mode = MODE_SDL;
#else
    mode = MODE_NULL;
#endif
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--null")) mode = MODE_NULL;
        else if (!strcmp(argv[i], "--sdl")) mode = MODE_SDL;
        else if (!strcmp(argv[i], "--usb")) mode = MODE_USB;
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--field") && i + 1 < argc) field = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else { usage(); return 1; }
    }
    if (field < 0) field = 0;
    if (field > MAX_FIELD) field = MAX_FIELD;
    for (i = 0; i < 4; i++) if (field_sizes[i] == field) field_idx = i;
    (void)scale; (void)verbose; (void)err;

    build_atlas();
    build_field();

    switch (mode) {
    case MODE_NULL:
        nul = pspexp_null_create(NULL);
        pspexp_null_set_paced(nul, 1);
        t = pspexp_null_transport(nul);
        break;
    case MODE_SDL:
#ifdef PSPEXP_HAVE_SDL
    {
        PspExpSdlOptions o;
        pspexp_sdl_options_default(&o);
        o.scale = scale;
        o.title = "PSPExp sprite demo";
        sdl = pspexp_sdl_create(&o, err, sizeof(err));
        if (!sdl) { fprintf(stderr, "%s\n", err); return 1; }
        t = pspexp_sdl_transport(sdl);
        break;
    }
#else
        fprintf(stderr, "built without SDL\n"); return 1;
#endif
    case MODE_USB:
#ifdef PSPEXP_HAVE_USB
        pspexp_usb_options_default(&usb_opt);
        usb_opt.verbose = verbose;
        printf("waiting for PSP over USB...\n");
        t = pspexp_usb_wait(&usb_opt, -1, err, sizeof(err));
        if (!t) { fprintf(stderr, "%s\n", err); return 1; }
        break;
#else
        fprintf(stderr, "built without libusb\n"); return 1;
#endif
    }

    ctx = pspexp_create(t, NULL);
    pspexp_set_log(ctx, log_fn, NULL);
    if (pspexp_connect(ctx, 5000) < 0) { fprintf(stderr, "no HELLO within 5 s\n"); return 1; }

    memset(&desc, 0, sizeof(desc));
    desc.width = ATLAS_W; desc.height = ATLAS_H; desc.format = PSPEXP_TEX_5551;
    desc.layout = PSPEXP_LAYOUT_LINEAR; desc.priority = PSPEXP_PRIORITY_HIGH;
    if (pspexp_upload_texture(ctx, ATLAS_ID, &desc, g_atlas, sizeof(g_atlas), g_frames, 6) < 0) {
        fprintf(stderr, "atlas upload rejected\n");
        return 1;
    }

    last_report = pspexp_time_us();
    for (;;) {
        const PspExpInput *in;
        uint32_t pressed;
        int n;

        /* ---- input: newest sample, edge detection for buttons ---- */
        in = pspexp_input(ctx);
        pressed = in->buttons & ~prev_buttons;
        prev_buttons = in->buttons;
        if (in->buttons & PSPEXP_BUTTON_LEFT) px -= 3;
        if (in->buttons & PSPEXP_BUTTON_RIGHT) px += 3;
        if (in->buttons & PSPEXP_BUTTON_UP) py -= 3;
        if (in->buttons & PSPEXP_BUTTON_DOWN) py += 3;
        if (abs((int)in->analog_x - 128) > 24) px += ((int)in->analog_x - 128) / 32;
        if (abs((int)in->analog_y - 128) > 24) py += ((int)in->analog_y - 128) / 32;
        if (pressed & PSPEXP_BUTTON_CROSS) pframe = (pframe + 1) & 3;
        if (pressed & PSPEXP_BUTTON_CIRCLE) ptint = (ptint + 1) & 3;
        if (pressed & PSPEXP_BUTTON_SQUARE) show_field = !show_field;
        if (pressed & PSPEXP_BUTTON_TRIANGLE) { field_idx = (field_idx + 1) & 3; field = field_sizes[field_idx]; }
        if (pressed & PSPEXP_BUTTON_START) { px = 224; py = 120; pframe = 0; ptint = 0; }
        if (px < 0) px = 0; if (px > 448) px = 448;
        if (py < 0) py = 0; if (py > 216) py = 216;

        /* ---- simulation ---- */
        for (i = 0; i < field; i++) {
            Star *s = &g_field[i];
            s->x = (int16_t)(s->x + s->dx); s->y = (int16_t)(s->y + s->dy);
            if (s->x < 0) s->x = 479; if (s->x > 479) s->x = 0;
            if (s->y < 0) s->y = 247; if (s->y > 247) s->y = 0;
            g_recs[i].x = s->x; g_recs[i].y = s->y; g_recs[i].frame = FRAME_STAR; g_recs[i].color = s->color;
        }

        /* ---- frame ---- */
        pspexp_begin_frame(ctx);
        pspexp_set_clip(ctx, 0, 0, PSPEXP_DISPLAY_WIDTH, PSPEXP_DISPLAY_HEIGHT);
        pspexp_draw_rect(ctx, 0, 0, PSPEXP_DISPLAY_WIDTH, 248, pspexp_color_5650(16, 24, 48));
        if (show_field && field > 0) pspexp_draw_sprite_batch(ctx, ATLAS_ID, g_recs, (uint32_t)field);
        pspexp_draw_sprite(ctx, ATLAS_ID, (int16_t)px, (int16_t)py, (uint16_t)(FRAME_PLAYER0 + pframe), tints[ptint]);
        /* HUD: bar, then blocks showing the player frame and tint index */
        pspexp_draw_rect(ctx, 0, 248, PSPEXP_DISPLAY_WIDTH, 24, pspexp_color_5650(40, 40, 56));
        pspexp_draw_rect(ctx, 8, 252, (int16_t)(px * 200 / 448 + 4), 16, pspexp_color_5650(90, 200, 120));
        for (i = 0; i <= pframe; i++) pspexp_draw_sprite(ctx, ATLAS_ID, (int16_t)(240 + i * 20), 252, FRAME_BLOCK, tints[ptint]);
        for (i = 0; i <= ptint; i++) pspexp_draw_sprite(ctx, ATLAS_ID, (int16_t)(340 + i * 20), 252, FRAME_BLOCK, PSPEXP_COLOR_NONE);
        if (pspexp_present(ctx) < 0 && pspexp_link_state(ctx) == PSPEXP_LINK_CONNECTED) {
            fprintf(stderr, "present failed\n");
            break;
        }
        frames++; report_frames++;

        /* ---- pacing and link maintenance ---- */
        n = pspexp_wait_present(ctx, 1000);
        if (n < 0 || pspexp_link_state(ctx) != PSPEXP_LINK_CONNECTED) {
#ifdef PSPEXP_HAVE_SDL
            if (sdl && pspexp_sdl_quit_requested(sdl)) break;
#endif
            if (mode != MODE_USB) break;
#ifdef PSPEXP_HAVE_USB
            printf("link lost; waiting for the PSP...\n");
            t = pspexp_usb_wait(&usb_opt, -1, err, sizeof(err));
            if (!t) { fprintf(stderr, "%s\n", err); break; }
            pspexp_set_transport(ctx, t);
            if (pspexp_connect(ctx, 5000) < 0) { fprintf(stderr, "reconnect: no HELLO\n"); continue; }
            printf("reconnected; atlas replayed\n");
            prev_buttons = 0;
#endif
        } else if (n > 0) {
            fprintf(stderr, "PRESENT_DONE timeout\n");
        }

        if (max_frames && frames >= (uint32_t)max_frames) break;
        if (pspexp_time_us() - last_report >= 1000000u) {
            const PspExpStats *st = pspexp_stats(ctx);
            uint64_t now = pspexp_time_us();
            printf("%5.1f fps  %5u B/frame  %2u batches  %5u sprites  present->done %5u us  input->done %5u us  psp cpu %4u us  ge %4u us  inputs %u  reconnects %u\n",
                   (double)report_frames * 1e6 / (double)(now - last_report), st->last_frame_bytes, st->last_frame_batches,
                   st->last_frame_sprites, st->last_present_latency_us, st->last_input_to_present_us, st->last_execute_us,
                   st->last_ge_us, st->inputs_received, st->reconnects);
            last_report = now; report_frames = 0;
        }
    }

    pspexp_send_raw(ctx, PSPEXP_CH_SYSTEM, PSPEXP_OP_DISCONNECT, NULL, 0);
    pspexp_destroy(ctx);
    if (nul) pspexp_null_destroy(nul);
#ifdef PSPEXP_HAVE_SDL
    if (sdl) pspexp_sdl_destroy(sdl);
#endif
    return 0;
}
