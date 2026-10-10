/* End-to-end through libPSPExp and the Null executor: the same path the
 * sprite demo takes, minus rendering. */
#include "test_util.h"
#include "../host/libpspexp/pspexp.h"
#include "../host/libpspexp/pspexp_font.h"
#include "../host/executor-null/pspexp_null.h"
#include "../protocol/pspexp_mp3.h"
#include <string.h>

static uint16_t g_pixels[64 * 64];
static PspExpFrame g_frames[4] = { {0,0,32,32}, {32,0,32,32}, {0,32,32,32}, {32,32,32,32} };

typedef struct Seen { int echo; int report; uint32_t data; PspExpBenchReport rep; } Seen;
static int hook(void *u, PspExpContext *ctx, const PspExpMessage *m)
{
    Seen *s = (Seen *)u; (void)ctx;
    if (m->header->opcode == PSPEXP_OP_BENCH_ECHO) { s->echo++; return 1; }
    if (m->header->opcode == PSPEXP_OP_BENCH_REPORT) { s->report++; memcpy(&s->rep, m->payload, sizeof(s->rep)); return 1; }
    if (m->header->opcode == PSPEXP_OP_BENCH_DATA) { s->data += m->length; return 1; }
    return 0;
}

static void quiet_log(void *u, const char *msg) { (void)u; (void)msg; }

int main(void)
{
    PspExpNullExecutor *nul = pspexp_null_create(NULL);
    PspExpContext *ctx = pspexp_create(pspexp_null_transport(nul), NULL);
    PspExpTextureDesc desc;
    PspExpSpriteRec recs[100];
    const PspExpStats *st;
    const PspExpNullStats *ns;
    Seen seen;
    uint8_t tier = 99;
    int i;

    memset(&seen, 0, sizeof(seen));
    pspexp_set_log(ctx, quiet_log, NULL);
    pspexp_set_message_hook(ctx, hook, &seen);
    CHECK(ctx != NULL);
    CHECK_EQ(pspexp_link_state(ctx), PSPEXP_LINK_DISCONNECTED);
    CHECK_EQ(pspexp_connect(ctx, 1000), 0);
    CHECK_EQ(pspexp_link_state(ctx), PSPEXP_LINK_CONNECTED);
    CHECK_EQ(pspexp_caps(ctx)->executor, PSPEXP_EXECUTOR_NULL);
    CHECK_EQ(pspexp_caps(ctx)->display_width, 480);
    CHECK_EQ(pspexp_caps(ctx)->vram_budget, PSPEXP_VRAM_TEXTURE_BUDGET);
    CHECK(pspexp_null_runtime(nul)->connected);
    CHECK_EQ(pspexp_null_runtime(nul)->ack.input_rate, 60);

    /* The battery (POWER_QUERY): asked, answered with a later pump. */
    {
        PspExpPowerStatus p;
        CHECK(pspexp_power_supported(ctx));
        CHECK_EQ(pspexp_power_status(ctx, &p, NULL), -1);   /* none yet */
        CHECK_EQ(pspexp_power_query(ctx), 0);
        pspexp_pump(ctx, 0);
        CHECK_EQ(pspexp_power_status(ctx, &p, NULL), 0);
        CHECK_EQ(p.percent, 76);
        CHECK_EQ(p.minutes_left, 125);
        CHECK_EQ(p.current_ma, -250);
        CHECK_EQ(p.flags, PSPEXP_POWER_BATTERY);
    }

    /* Texture upload and residency. */
    memset(&desc, 0, sizeof(desc));
    desc.width = 64; desc.height = 64; desc.format = PSPEXP_TEX_5650; desc.layout = PSPEXP_LAYOUT_LINEAR; desc.priority = PSPEXP_PRIORITY_HIGH;
    CHECK_EQ(pspexp_upload_texture(ctx, 7, &desc, g_pixels, sizeof(g_pixels), g_frames, 4), 0);
    CHECK_EQ(pspexp_resource_state(ctx, 7, NULL), PSPEXP_RES_PENDING);
    CHECK(pspexp_pump(ctx, 0) >= 1);
    CHECK_EQ(pspexp_resource_state(ctx, 7, &tier), PSPEXP_RES_RESIDENT);
    CHECK_EQ(tier, PSPEXP_TIER_VRAM);
    CHECK_EQ(pspexp_null_stats(nul)->textures_created, 1);

    /* Host-side rejection: frame wider than 64 px, oversize texture. */
    {
        PspExpFrame wide = { 0, 0, 65, 8 };
        PspExpTextureDesc big = desc;
        CHECK_EQ(pspexp_upload_texture(ctx, 8, &desc, g_pixels, sizeof(g_pixels), &wide, 1), -PSPEXP_RERR_BAD_FRAME_TABLE);
        big.width = 1024;
        CHECK_EQ(pspexp_upload_texture(ctx, 8, &big, g_pixels, sizeof(g_pixels), g_frames, 4), -PSPEXP_RERR_BAD_DIMENSIONS);
        CHECK_EQ(pspexp_resource_state(ctx, 8, NULL), PSPEXP_RES_NONE);
    }

    /* Out of memory: fill VRAM then RAM with 512 KiB textures. */
    {
        static uint16_t big_px[512 * 512];
        PspExpTextureDesc big = desc;
        PspExpFrame f = { 0, 0, 64, 512 };
        int id;
        big.width = 512; big.height = 512;
        for (id = 100; id < 100 + 19; id++)   /* VRAM holds 2, RAM holds 16: the 19th fails */
            CHECK_EQ(pspexp_upload_texture(ctx, (uint16_t)id, &big, big_px, sizeof(big_px), &f, 1), 0);
        pspexp_pump(ctx, 0);
        CHECK_EQ(pspexp_resource_state(ctx, 100, &tier), PSPEXP_RES_RESIDENT);
        CHECK_EQ(tier, PSPEXP_TIER_VRAM);
        CHECK_EQ(pspexp_resource_state(ctx, 102, &tier), PSPEXP_RES_RESIDENT);
        CHECK_EQ(tier, PSPEXP_TIER_RAM);
        CHECK_EQ(pspexp_resource_state(ctx, 117, &tier), PSPEXP_RES_RESIDENT);
        CHECK_EQ(pspexp_resource_state(ctx, 118, &tier), PSPEXP_RES_FAILED);
        CHECK_EQ(pspexp_stats(ctx)->resource_errors, 1);
        for (id = 100; id < 100 + 19; id++) pspexp_release(ctx, (uint16_t)id);
        pspexp_pump(ctx, 0);
        CHECK_EQ(pspexp_memory_query(ctx), 0);
        pspexp_pump(ctx, 0);
        CHECK_EQ(pspexp_memory_status(ctx)->vram_free, PSPEXP_VRAM_TEXTURE_BUDGET - 64 * 64 * 2);
        CHECK_EQ(pspexp_memory_status(ctx)->ram_count, 0);
    }

    /* Input arrives independently of frames. */
    pspexp_null_push_input(nul, PSPEXP_BUTTON_CROSS, 200, 100);
    pspexp_pump(ctx, 0);
    CHECK_EQ(pspexp_input(ctx)->buttons, PSPEXP_BUTTON_CROSS);
    CHECK_EQ(pspexp_input(ctx)->analog_x, 200);

    /* A frame: 100 sprites in one batch, a rect, PRESENT_DONE pacing. */
    for (i = 0; i < 100; i++) { recs[i].x = (int16_t)i; recs[i].y = (int16_t)(2 * i); recs[i].frame = (uint16_t)(i & 3); recs[i].color = PSPEXP_COLOR_NONE; }
    CHECK(pspexp_should_render(ctx));
    pspexp_begin_frame(ctx);
    pspexp_set_clip(ctx, 0, 0, 480, 272);
    pspexp_draw_rect(ctx, 0, 224, 480, 48, 0x1234);
    pspexp_draw_sprite_batch(ctx, 7, recs, 100);
    pspexp_draw_sprite(ctx, 7, 10, 10, 2, 0xF800);
    CHECK_EQ(pspexp_present(ctx), 1);
    CHECK_EQ(pspexp_frames_in_flight(ctx), 1);
    CHECK(!pspexp_should_render(ctx));
    CHECK_EQ(pspexp_wait_present(ctx, 100), 0);
    CHECK_EQ(pspexp_frames_in_flight(ctx), 0);
    st = pspexp_stats(ctx);
    ns = pspexp_null_stats(nul);
    CHECK_EQ(st->frames_sent, 1);
    CHECK_EQ(st->frames_presented, 1);
    CHECK_EQ(st->last_frame_batches, 2);
    CHECK_EQ(st->last_frame_sprites, 101);
    /* BEGIN 32 + CLIP 32 + RECT 32 + batch (16 + 16 + 800) + sprite (16 + 16 + 16) + PRESENT 16 */
    CHECK_EQ(st->last_frame_bytes, 32 + 32 + 32 + 832 + 48 + 16);
    CHECK(st->last_input_to_present_us < 1000000u);
    CHECK_EQ(ns->frames, 1);
    CHECK_EQ(ns->clips, 1);
    CHECK_EQ(ns->rects, 1);
    CHECK_EQ(ns->sprite_draws, 2);
    CHECK_EQ(ns->sprites, 101);
    CHECK_EQ(ns->vertices_expanded, 202);

    /* Batch splitting at the runtime's limit. */
    {
        static PspExpSpriteRec many[PSPEXP_BATCH_MAX_RECORDS * 2 + 5];
        memset(many, 0, sizeof(many));
        pspexp_begin_frame(ctx);
        pspexp_draw_sprite_batch(ctx, 7, many, PSPEXP_BATCH_MAX_RECORDS * 2 + 5);
        pspexp_present(ctx);
        pspexp_wait_present(ctx, 100);
        CHECK_EQ(pspexp_stats(ctx)->last_frame_batches, 3);
        CHECK_EQ(pspexp_null_stats(nul)->sprites, 101 + PSPEXP_BATCH_MAX_RECORDS * 2 + 5);
        CHECK_EQ(pspexp_null_runtime(nul)->stats.protocol_errors, 0);
    }

    /* Tile batch split on row boundaries. */
    {
        static PspExpTileRec tiles[30 * 17];
        memset(tiles, 0, sizeof(tiles));
        pspexp_begin_frame(ctx);
        pspexp_draw_tile_batch(ctx, 7, 0, 0, 16, 16, 30, tiles, 30 * 17);
        pspexp_present(ctx);
        pspexp_wait_present(ctx, 100);
        CHECK_EQ(pspexp_null_runtime(nul)->stats.tiles, 30 * 17);
        CHECK_EQ(pspexp_null_runtime(nul)->stats.protocol_errors, 0);
    }

    /* Drawing outside a frame and with an unknown atlas are reported. */
    {
        uint32_t misses_before = pspexp_null_runtime(nul)->stats.misses;
        PspExpSpriteBatch b; memset(&b, 0, sizeof(b)); b.atlas = 7; b.count = 0;
        CHECK_EQ(pspexp_send_raw(ctx, PSPEXP_CH_GRAPHICS, PSPEXP_OP_DRAW_SPRITE_BATCH, &b, sizeof(b)), 0);
        pspexp_pump(ctx, 0);
        CHECK_EQ(pspexp_null_runtime(nul)->stats.protocol_errors, 1);
        pspexp_begin_frame(ctx);
        pspexp_draw_sprite(ctx, 99, 0, 0, 0, 0);
        pspexp_present(ctx);
        pspexp_wait_present(ctx, 100);
        CHECK_EQ(pspexp_null_runtime(nul)->stats.misses, misses_before + 1);
        /* Frame index out of range is a resource error, not a crash. */
        pspexp_begin_frame(ctx);
        pspexp_draw_sprite(ctx, 7, 0, 0, 4, 0);
        pspexp_present(ctx);
        pspexp_wait_present(ctx, 100);
        CHECK_EQ(pspexp_null_runtime(nul)->stats.resource_errors, 2);  /* one OOM earlier, one now */
    }

    /* Release, then draw after release -> RESOURCE_MISS. */
    CHECK_EQ(pspexp_release(ctx, 7), 0);
    pspexp_begin_frame(ctx);
    pspexp_draw_sprite(ctx, 7, 0, 0, 0, 0);
    pspexp_present(ctx);
    pspexp_wait_present(ctx, 100);
    CHECK_EQ(pspexp_null_stats(nul)->textures_destroyed, 18 + 1);
    CHECK_EQ(pspexp_resource_state(ctx, 7, NULL), PSPEXP_RES_NONE);

    /* Cable pull: the runtime forgets everything; the host replays its working set. */
    CHECK_EQ(pspexp_upload_texture(ctx, 7, &desc, g_pixels, sizeof(g_pixels), g_frames, 4), 0);
    pspexp_pump(ctx, 0);
    CHECK_EQ(pspexp_resource_state(ctx, 7, NULL), PSPEXP_RES_RESIDENT);
    pspexp_begin_frame(ctx); pspexp_present(ctx);       /* one frame left in flight */
    pspexp_null_simulate_reconnect(nul);
    CHECK_EQ(pspexp_null_stats(nul)->textures_destroyed, 18 + 2);
    pspexp_pump(ctx, 0);
    CHECK_EQ(pspexp_stats(ctx)->reconnects, 1);
    CHECK_EQ(pspexp_link_state(ctx), PSPEXP_LINK_CONNECTED);
    CHECK_EQ(pspexp_frames_in_flight(ctx), 0);
    CHECK_EQ(pspexp_resource_state(ctx, 7, NULL), PSPEXP_RES_RESIDENT);
    CHECK_EQ(pspexp_null_stats(nul)->textures_created, 1 + 18 + 1 + 1);
    pspexp_begin_frame(ctx);
    pspexp_draw_sprite(ctx, 7, 0, 0, 0, 0);
    pspexp_present(ctx);
    CHECK_EQ(pspexp_wait_present(ctx, 100), 0);
    CHECK_EQ(pspexp_null_runtime(nul)->stats.misses, 2);   /* no new miss after replay */

    /* Indexed textures: palette first, then a CLUT8 atlas; palette swap and release. */
    {
        static uint16_t pal[256];
        static uint8_t idx[64 * 64];
        PspExpTextureDesc cd;
        uint32_t misses = pspexp_null_runtime(nul)->stats.misses;
        uint32_t rerr = pspexp_null_runtime(nul)->stats.resource_errors;
        for (i = 0; i < 256; i++) pal[i] = (uint16_t)(0x8000 | i);
        memset(&cd, 0, sizeof(cd));
        cd.width = 64; cd.height = 64; cd.format = PSPEXP_TEX_T8; cd.layout = PSPEXP_LAYOUT_SWIZZLED; cd.palette = 40;
        cd.priority = PSPEXP_PRIORITY_NORMAL;
        /* Texture before its palette is rejected on the host. */
        CHECK_EQ(pspexp_upload_texture(ctx, 41, &cd, idx, sizeof(idx), g_frames, 4), -PSPEXP_RERR_BAD_PALETTE);
        CHECK_EQ(pspexp_upload_palette(ctx, 40, PSPEXP_TEX_5551, 256, pal, 100), -PSPEXP_RERR_BAD_PALETTE);
        CHECK_EQ(pspexp_upload_palette(ctx, 40, PSPEXP_TEX_5551, 256, pal, sizeof(pal)), 0);
        CHECK_EQ(pspexp_upload_texture(ctx, 41, &cd, idx, sizeof(idx), g_frames, 4), 0);
        pspexp_pump(ctx, 0);
        CHECK_EQ(pspexp_resource_state(ctx, 40, &tier), PSPEXP_RES_RESIDENT);
        CHECK_EQ(tier, PSPEXP_TIER_RAM);
        CHECK_EQ(pspexp_resource_state(ctx, 41, &tier), PSPEXP_RES_RESIDENT);
        CHECK_EQ(tier, PSPEXP_TIER_VRAM);
        CHECK_EQ(pspexp_null_stats(nul)->palettes_created, 1);
        /* Low priority (re-uploaded every frame) is placed in RAM even when VRAM has room. */
        cd.priority = PSPEXP_PRIORITY_LOW;
        CHECK_EQ(pspexp_upload_texture(ctx, 42, &cd, idx, sizeof(idx), g_frames, 4), 0);
        pspexp_pump(ctx, 0);
        CHECK_EQ(pspexp_resource_state(ctx, 42, &tier), PSPEXP_RES_RESIDENT);
        CHECK_EQ(tier, PSPEXP_TIER_RAM);
        pspexp_release(ctx, 42);
        pspexp_begin_frame(ctx);
        pspexp_draw_sprite(ctx, 41, 0, 0, 1, PSPEXP_COLOR_NONE);
        pspexp_present(ctx);
        pspexp_wait_present(ctx, 100);
        CHECK_EQ(pspexp_null_stats(nul)->indexed_draws, 1);
        /* A 16-entry palette cannot serve a T8 texture. */
        CHECK_EQ(pspexp_upload_palette(ctx, 42, PSPEXP_TEX_5650, 16, pal, 32), 0);
        cd.palette = 42;
        CHECK_EQ(pspexp_upload_texture(ctx, 43, &cd, idx, sizeof(idx), g_frames, 4), 0);
        pspexp_pump(ctx, 0);
        CHECK_EQ(pspexp_resource_state(ctx, 43, NULL), PSPEXP_RES_FAILED);
        CHECK_EQ(pspexp_null_runtime(nul)->stats.resource_errors, rerr + 1);
        /* Drawing with a palette handle as the atlas is a kind error; releasing the
         * palette makes later draws miss it. */
        pspexp_begin_frame(ctx);
        pspexp_draw_sprite(ctx, 40, 0, 0, 0, 0);
        pspexp_present(ctx);
        pspexp_wait_present(ctx, 100);
        CHECK_EQ(pspexp_null_runtime(nul)->stats.resource_errors, rerr + 2);
        CHECK_EQ(pspexp_release(ctx, 40), 0);
        pspexp_begin_frame(ctx);
        pspexp_draw_sprite(ctx, 41, 0, 0, 0, 0);
        pspexp_present(ctx);
        pspexp_wait_present(ctx, 100);
        CHECK_EQ(pspexp_null_runtime(nul)->stats.misses, misses + 1);
        /* Reconnect replays the palette before the texture. */
        CHECK_EQ(pspexp_upload_palette(ctx, 40, PSPEXP_TEX_5551, 256, pal, sizeof(pal)), 0);
        pspexp_null_simulate_reconnect(nul);
        pspexp_pump(ctx, 0);
        CHECK_EQ(pspexp_resource_state(ctx, 40, NULL), PSPEXP_RES_RESIDENT);
        CHECK_EQ(pspexp_resource_state(ctx, 41, NULL), PSPEXP_RES_RESIDENT);
        pspexp_begin_frame(ctx);
        pspexp_draw_sprite(ctx, 41, 0, 0, 3, 0);
        pspexp_present(ctx);
        pspexp_wait_present(ctx, 100);
        CHECK_EQ(pspexp_null_stats(nul)->indexed_draws, 2);
        /* Per-batch palette override, flip, alpha format, translucent fills, lines. */
        {
            static uint16_t pal2[256];
            PspExpSpriteRec r = { 5, 5, 0, pspexp_color_4444(255, 255, 255, 128) };
            uint32_t rerr2 = pspexp_null_runtime(nul)->stats.resource_errors;
            uint32_t miss2 = pspexp_null_runtime(nul)->stats.misses;
            CHECK_EQ(pspexp_upload_palette(ctx, 44, PSPEXP_TEX_5551, 256, pal2, sizeof(pal2)), 0);
            pspexp_pump(ctx, 0);
            pspexp_begin_frame(ctx);
            pspexp_draw_sprite_batch_ex(ctx, 41, &r, 1, PSPEXP_BATCH_FLIP_X, 44, PSPEXP_VTX_SPRITE_2D_4444);
            pspexp_draw_sprite_batch_ex(ctx, 41, &r, 1, 0, 42, PSPEXP_VTX_SPRITE_2D_5650);      /* 16-entry palette: error */
            pspexp_draw_sprite_batch_ex(ctx, 41, &r, 1, 0, 99, PSPEXP_VTX_SPRITE_2D_5650);      /* unknown palette: miss */
            pspexp_draw_sprite_batch_ex(ctx, 41, &r, 1, 0, PSPEXP_PALETTE_DEFAULT, 7);          /* bad vertex format */
            pspexp_fill_rect(ctx, 0, 0, 10, 10, 0, 128);
            pspexp_draw_line(ctx, 0, 0, 100, 50, 0xFFFF, 255);
            {
                PspExpQuadRec q[2] = { { 1, 1, 0, 0, 64, 64, 0xFFFF, 0 }, { 1, 1, 0, 0, 65, 64, 0xFFFF, 0 } };
                PspExpQuadExRec qe = { 0, 0, 480, 272, 64, 64, 0, 0, 0xFFFF, {0} };   /* stretched, flipped both ways */
                pspexp_draw_quad_batch(ctx, 41, q, 1, PSPEXP_PALETTE_DEFAULT, PSPEXP_VTX_SPRITE_2D_5650);   /* ok */
                pspexp_draw_quad_batch(ctx, 41, q + 1, 1, PSPEXP_PALETTE_DEFAULT, PSPEXP_VTX_SPRITE_2D_5650); /* u1 past the edge */
                pspexp_draw_quad_ex_batch(ctx, 41, &qe, 1, PSPEXP_PALETTE_DEFAULT, PSPEXP_VTX_SPRITE_2D_5650); /* ok */
            }
            pspexp_present(ctx);
            pspexp_wait_present(ctx, 100);
            CHECK_EQ(pspexp_null_stats(nul)->flipped_draws, 2);   /* FLIP_X batch and the flipped scaled quad */
            CHECK_EQ(pspexp_null_stats(nul)->alpha_draws, 1);
            CHECK_EQ(pspexp_null_stats(nul)->lines, 1);
            CHECK_EQ(pspexp_null_runtime(nul)->stats.resource_errors, rerr2 + 2);   /* 16-entry palette, quad off the edge */
            CHECK_EQ(pspexp_null_runtime(nul)->stats.misses, miss2 + 1);
            CHECK_EQ(pspexp_null_runtime(nul)->stats.protocol_errors, 2);   /* one earlier, one now */
            pspexp_release(ctx, 44);
        }
        pspexp_release(ctx, 41); pspexp_release(ctx, 40); pspexp_release(ctx, 42);
        pspexp_pump(ctx, 0);
    }

    /* Host-baked text: the built-in font is a CLUT4 atlas; DRAW_TEXT is a sprite batch. */
    {
        PspExpFont font;
        uint32_t rerr_font_check = pspexp_null_runtime(nul)->stats.resource_errors;
        uint32_t sprites_before = pspexp_null_runtime(nul)->stats.sprites;
        CHECK_EQ(pspexp_font_builtin_upload(ctx, 60, 61, &font), 0);
        pspexp_pump(ctx, 0);
        CHECK_EQ(pspexp_resource_state(ctx, 60, NULL), PSPEXP_RES_RESIDENT);
        CHECK_EQ(pspexp_text_width(&font, "GOLD: 1275"), 10 * 6);
        CHECK_EQ(pspexp_text_width(&font, "AB\nABCD"), 4 * 6);
        pspexp_begin_frame(ctx);
        CHECK_EQ(pspexp_draw_text(ctx, &font, 10, 10, "Gold: 1275\nok", 0xFFFF), 11);   /* spaces skipped */
        pspexp_present(ctx);
        pspexp_wait_present(ctx, 100);
        CHECK_EQ(pspexp_null_runtime(nul)->stats.sprites, sprites_before + 11);
        CHECK_EQ(pspexp_null_runtime(nul)->stats.resource_errors, rerr_font_check);
        pspexp_release(ctx, 60); pspexp_release(ctx, 61);
        pspexp_pump(ctx, 0);
    }

    /* Audio: a cached sound plays through the shared mixer; the stream starts
     * after its jitter buffer fills; status reports arrive. */
    {
        static int16_t tone[441];       /* 10 ms mono */
        static int16_t music[4410 * 2]; /* 100 ms stereo */
        static int16_t out[512 * 2];
        int silent = 1, nonzero = 0;
        uint32_t k;
        for (i = 0; i < 441; i++) tone[i] = (int16_t)((i & 8) ? 8000 : -8000);
        for (i = 0; i < 4410 * 2; i++) music[i] = (int16_t)((i & 4) ? 3000 : -3000);
        CHECK(pspexp_null_runtime(nul)->audio_active);
        CHECK_EQ(pspexp_upload_sound(ctx, 70, tone, 441, 1), 0);
        pspexp_pump(ctx, 0);
        CHECK_EQ(pspexp_resource_state(ctx, 70, NULL), PSPEXP_RES_RESIDENT);
        CHECK_EQ(pspexp_play_sound(ctx, 70, 255, 0, 0), 0);
        pspexp_pump(ctx, 0);
        CHECK_EQ(pspexp_null_runtime(nul)->stats.plays, 1);
        CHECK_EQ(pspexp_null_render_audio(nul, out, 512), 512);
        for (k = 0; k < 512 * 2; k++) if (out[k] != 0) nonzero++;
        CHECK(nonzero > 400);   /* 441 frames of tone on both channels, then silence */
        CHECK_EQ(out[511 * 2], 0);
        /* Stream: nothing plays until ~100 ms is buffered (4410 frames at 44.1 kHz). */
        CHECK_EQ(pspexp_audio_stream(ctx, music, 2000), 0);
        pspexp_pump(ctx, 0);
        pspexp_null_render_audio(nul, out, 512);
        for (k = 0; k < 512 * 2; k++) if (out[k] != 0) silent = 0;
        CHECK(silent);
        CHECK_EQ(pspexp_audio_stream(ctx, music + 2000 * 2, 2410), 0);
        pspexp_pump(ctx, 0);
        pspexp_null_render_audio(nul, out, 512);
        nonzero = 0;
        for (k = 0; k < 512 * 2; k++) if (out[k] != 0) nonzero++;
        CHECK(nonzero > 900);
        /* Playing an unknown sound is a miss; releasing the sound stops its voice. */
        CHECK_EQ(pspexp_play_sound(ctx, 71, 255, 0, 0), 0);
        pspexp_pump(ctx, 0);
        CHECK_EQ(pspexp_play_sound(ctx, 70, 200, -128, 1), 0);
        pspexp_release(ctx, 70);
        pspexp_pump(ctx, 0);
        pspexp_null_render_audio(nul, out, 512);
        CHECK_EQ(pspexp_null_runtime(nul)->cache.ram_count, 0 + 0);   /* sound freed (palette 40/42 released above) */
        CHECK_EQ(pspexp_audio_stream_reset(ctx), 0);
        pspexp_pump(ctx, 0);
        CHECK_EQ(pspexp_audio_status(ctx)->buffered_frames, 0);
        CHECK(pspexp_audio_status(ctx)->received_frames >= 4410);
        /* Compressed audio (AUDIO_CODED_STREAM): whole MP3 frames, decoded on
         * the PSP at their own rate and resampled into the stream. */
        {
            static uint8_t mp3[4 * 417 + 2 * 208];
            const PspExpAudioStatus *as = pspexp_audio_status(ctx);
            uint32_t before = as->received_frames;
            PspExpMp3Frame f;
            memset(mp3, 0, sizeof(mp3));
            for (i = 0; i < 4; i++) { uint8_t *h = mp3 + i * 417; h[0] = 0xFF; h[1] = 0xFB; h[2] = 0x90; h[3] = 0x64; }   /* MPEG-1, 128 kbps, 44.1 kHz, joint stereo */
            for (i = 0; i < 2; i++) { uint8_t *h = mp3 + 4 * 417 + i * 208; h[0] = 0xFF; h[1] = 0xF3; h[2] = 0x80; h[3] = 0xC0; }   /* MPEG-2, 64 kbps, 22.05 kHz, mono */
            CHECK_EQ(pspexp_mp3_header(mp3, sizeof(mp3), &f), 0);
            CHECK_EQ(f.bytes, 417); CHECK_EQ(f.samples, 1152); CHECK_EQ(f.sample_rate, 44100); CHECK_EQ(f.channels, 2); CHECK_EQ(f.bitrate_kbps, 128);
            CHECK_EQ(pspexp_mp3_header(mp3 + 4 * 417, 208, &f), 0);
            CHECK_EQ(f.bytes, 208); CHECK_EQ(f.samples, 576); CHECK_EQ(f.sample_rate, 22050); CHECK_EQ(f.channels, 1); CHECK_EQ(f.version, 20);
            CHECK_EQ(pspexp_mp3_header(mp3 + 1, 16, &f), -1);
            CHECK(pspexp_audio_mp3_supported(ctx));
            CHECK_EQ(pspexp_audio_coded(ctx, PSPEXP_CODEC_MP3, mp3, 4 * 417), 0);
            pspexp_audio_stream_reset(ctx);   /* (status sent back with it) */
            pspexp_pump(ctx, 0);
            CHECK_EQ(as->received_frames - before, 4 * 1152);
            before = as->received_frames;
            CHECK_EQ(pspexp_audio_coded(ctx, PSPEXP_CODEC_MP3, mp3 + 4 * 417, 2 * 208), 0);   /* 576 at 22.05 kHz: 1152 at 44.1 */
            pspexp_audio_stream_reset(ctx);
            pspexp_pump(ctx, 0);
            CHECK_EQ(as->received_frames - before, 2 * 1152);
            CHECK_EQ(pspexp_audio_coded(ctx, 9, mp3, 417), 0);   /* an unknown codec: refused by the PSP, nothing played */
            pspexp_audio_stream_reset(ctx);
            pspexp_pump(ctx, 0);
            CHECK_EQ(as->received_frames - before, 2 * 1152);
        }
        /* A length past the payload is refused, also one that wraps 32 bits
         * with the header added (0xFFFFFFF0 + 16 = 0 on the PSP), for coded
         * audio and for plain stream frames (0x40000001 x 4 = 4). */
        {
            PspExpRuntime *rt = pspexp_null_runtime(nul);
            const PspExpAudioStatus *as = pspexp_audio_status(ctx);
            uint32_t errors = rt->stats.protocol_errors, before = as->received_frames;
            struct { PspExpAudioCoded h; uint8_t d[16]; } coded;
            struct { PspExpAudioStream h; int16_t d[8]; } plain;
            memset(&coded, 0, sizeof(coded));
            coded.h.codec = PSPEXP_CODEC_MP3;
            coded.h.bytes = 0xFFFFFFF0u;
            coded.d[0] = 0xFF; coded.d[1] = 0xFB; coded.d[2] = 0x90; coded.d[3] = 0x64;
            CHECK_EQ(pspexp_send_raw(ctx, PSPEXP_CH_AUDIO, PSPEXP_OP_AUDIO_CODED_STREAM, &coded, sizeof(coded)), 0);
            coded.h.bytes = sizeof(coded.d) + 1;
            CHECK_EQ(pspexp_send_raw(ctx, PSPEXP_CH_AUDIO, PSPEXP_OP_AUDIO_CODED_STREAM, &coded, sizeof(coded)), 0);
            memset(&plain, 0, sizeof(plain));
            plain.h.frames = 0x40000001u;
            CHECK_EQ(pspexp_send_raw(ctx, PSPEXP_CH_AUDIO, PSPEXP_OP_AUDIO_STREAM, &plain, sizeof(plain)), 0);
            pspexp_audio_stream_reset(ctx);
            pspexp_pump(ctx, 0);
            CHECK_EQ(rt->stats.protocol_errors, errors + 3);
            CHECK_EQ(as->received_frames, before);
        }
        /* Resampling the largest steps (full scale, from one sign to the
         * other) at a rate that is not the stream's: 32 kHz into 44.1 kHz,
         * where the position's fraction runs past one half: (b - a) x f no
         * longer fits 32 bits (a sanitized build catches the overflow); the
         * samples in between land between a peak and a trough. */
        {
            static uint8_t mp3[576];
            PspExpMixer *mx = &pspexp_null_runtime(nul)->mixer;
            uint32_t j, n, mid = 0;
            memset(mp3, 0, sizeof(mp3));
            mp3[0] = 0xFF; mp3[1] = 0xFB; mp3[2] = 0x98; mp3[3] = 0x64; mp3[4] = 0xA5;   /* MPEG-1, 128 kbps, 32 kHz */
            pspexp_audio_stream_reset(ctx);
            pspexp_pump(ctx, 0);
            CHECK_EQ(pspexp_audio_coded(ctx, PSPEXP_CODEC_MP3, mp3, sizeof(mp3)), 0);
            pspexp_pump(ctx, 0);
            n = mx->buffered;
            CHECK(n >= 1580 && n <= 1590);   /* 1152 x 44100 / 32000 = 1587.6 */
            for (j = 1; j < n; j++) {        /* (the first starts from the silence before) */
                int16_t v = mx->ring[((mx->tail + j) % mx->ring_frames) * 2];
                if (v > -32000 && v < 32000) mid++;
            }
            CHECK(mid > n / 2);              /* most fall between a peak and a trough */
            pspexp_audio_stream_reset(ctx);
            pspexp_pump(ctx, 0);
        }
        /* Rate adaptation: a slow source keeps the buffer under half the
         * target, so rendering consumes fewer frames than it outputs. */
        {
            PspExpMixer *mx = &pspexp_null_runtime(nul)->mixer;
            uint32_t before;
            pspexp_mixer_stream_write(mx, music, 4410);   /* target reached: playing */
            pspexp_mixer_render(mx, out, 512);            /* 3898 left: above half */
            before = mx->buffered;
            pspexp_mixer_render(mx, out, 512);            /* still above half (3386) */
            CHECK_EQ(before - mx->buffered, 512);
            pspexp_mixer_render(mx, out, 512);
            pspexp_mixer_render(mx, out, 512);
            pspexp_mixer_render(mx, out, 512);            /* now 1850 < 2205: stretching */
            before = mx->buffered;
            pspexp_mixer_render(mx, out, 512);
            CHECK(before - mx->buffered < 512);
            CHECK(mx->stretched > 0);
            pspexp_mixer_stream_reset(mx);
        }
    }

    /* Benchmark messages on the DEBUG channel. */
    {
        PspExpBenchStart bs; PspExpBenchUplink up; uint8_t blob[1000];
        memset(&bs, 0, sizeof(bs)); memset(&up, 0, sizeof(up)); memset(blob, 1, sizeof(blob));
        CHECK_EQ(pspexp_send_raw(ctx, PSPEXP_CH_DEBUG, PSPEXP_OP_BENCH_ECHO, blob, 100), 0);
        pspexp_pump(ctx, 0);
        CHECK_EQ(seen.echo, 1);
        CHECK_EQ(pspexp_send_raw(ctx, PSPEXP_CH_DEBUG, PSPEXP_OP_BENCH_START, &bs, sizeof(bs)), 0);
        for (i = 0; i < 10; i++) pspexp_send_raw(ctx, PSPEXP_CH_DEBUG, PSPEXP_OP_BENCH_DATA, blob, sizeof(blob));
        CHECK_EQ(pspexp_send_raw(ctx, PSPEXP_CH_DEBUG, PSPEXP_OP_BENCH_END, NULL, 0), 0);
        pspexp_pump(ctx, 0);
        CHECK_EQ(seen.report, 1);
        CHECK_EQ(seen.rep.messages, 10);
        CHECK_EQ(seen.rep.bytes, 10 * 1008);
        up.block_size = 4000; up.count = 5;
        CHECK_EQ(pspexp_send_raw(ctx, PSPEXP_CH_DEBUG, PSPEXP_OP_BENCH_UPLINK, &up, sizeof(up)), 0);
        for (i = 0; i < 20 && seen.report < 2; i++) pspexp_pump(ctx, 0);
        CHECK_EQ(seen.report, 2);
        CHECK_EQ(seen.data, 5 * 4000);
        CHECK_EQ(seen.rep.messages, 5);
    }

    pspexp_destroy(ctx);
    pspexp_null_destroy(nul);
    return test_summary("test_runtime");
}
