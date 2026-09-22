/* End-to-end through libPSPExp and the Null executor: the same path the
 * sprite demo takes, minus rendering. */
#include "test_util.h"
#include "../host/libpspexp/pspexp.h"
#include "../host/executor-null/pspexp_null.h"
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
        pspexp_release(ctx, 41); pspexp_release(ctx, 40); pspexp_release(ctx, 42);
        pspexp_pump(ctx, 0);
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
