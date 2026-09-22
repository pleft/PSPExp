#include "test_util.h"
#include "../protocol/pspexp_protocol.h"
#include "../protocol/pspexp_opcodes.h"
#include "../protocol/pspexp_batch.h"
#include "../protocol/serialization/pspexp_writer.h"
#include "../protocol/serialization/pspexp_reader.h"
#include <string.h>

static void test_writer_reader_roundtrip(void)
{
    PspExpWriter w;
    PspExpStream s;
    PspExpMessage m;
    PspExpSetClip clip = { 1, 2, 3, 4, {0} };
    uint8_t odd[37];
    size_t i;
    int r;

    memset(odd, 0xAB, sizeof(odd));
    CHECK_EQ(pspexp_writer_init_dynamic(&w, 64), 0);
    CHECK_EQ(pspexp_writer_message(&w, PSPEXP_CH_GRAPHICS, PSPEXP_OP_SET_CLIP, 0, &clip, sizeof(clip)), 0);
    CHECK_EQ(pspexp_writer_message(&w, PSPEXP_CH_DEBUG, PSPEXP_OP_BENCH_DATA, 7, odd, sizeof(odd)), 0);
    CHECK_EQ(pspexp_writer_message(&w, PSPEXP_CH_GRAPHICS, PSPEXP_OP_PRESENT, 0, NULL, 0), 0);
    CHECK_EQ(w.len % 16, 0);
    CHECK_EQ(w.len, 16 + 16 + 16 + 48 + 16);

    /* Feed one byte at a time to exercise partial framing. */
    CHECK_EQ(pspexp_stream_init_dynamic(&s, 32), 0);
    for (i = 0; i < w.len; i++) {
        CHECK_EQ(pspexp_stream_feed(&s, w.buf + i, 1), 0);
        r = pspexp_stream_next(&s, &m);
        if (i == 31) {
            CHECK_EQ(r, 1);
            CHECK_EQ(m.header->opcode, PSPEXP_OP_SET_CLIP);
            CHECK_EQ(m.header->sequence, 0);
            CHECK_EQ(m.length, 16);
            CHECK_EQ(((const PspExpSetClip *)m.payload)->h, 4);
        } else if (i == 31 + 64) {
            CHECK_EQ(r, 1);
            CHECK_EQ(m.header->opcode, PSPEXP_OP_BENCH_DATA);
            CHECK_EQ(m.header->flags, 7);
            CHECK_EQ(m.header->sequence, 1);
            CHECK_EQ(m.length, 48);
            CHECK_EQ(m.payload[36], 0xAB);
            CHECK_EQ(m.payload[37], 0);   /* padding is zeroed */
        } else if (i == w.len - 1) {
            CHECK_EQ(r, 1);
            CHECK_EQ(m.header->opcode, PSPEXP_OP_PRESENT);
            CHECK_EQ(m.length, 0);
        } else {
            CHECK_EQ(r, 0);
        }
    }
    CHECK_EQ(pspexp_stream_next(&s, &m), 0);
    CHECK_EQ(s.messages, 3);

    /* Corruption is detected. */
    pspexp_stream_reset(&s);
    CHECK_EQ(pspexp_stream_feed(&s, "XX", 2), 0);
    CHECK_EQ(pspexp_stream_next(&s, &m), -1);

    /* Fixed writer overflow is sticky and reported. */
    {
        uint8_t small[32] __attribute__((aligned(16)));
        PspExpWriter fw;
        pspexp_writer_init(&fw, small, sizeof(small));
        CHECK_EQ(pspexp_writer_message(&fw, PSPEXP_CH_SYSTEM, PSPEXP_OP_MEMORY_QUERY, 0, NULL, 0), 0);
        CHECK_EQ(pspexp_writer_message(&fw, PSPEXP_CH_GRAPHICS, PSPEXP_OP_SET_CLIP, 0, &clip, sizeof(clip)), -1);
        CHECK(fw.error);
    }
    pspexp_stream_free(&s);
    pspexp_writer_free(&w);
}

static void test_expansion(void)
{
    PspExpFrame frames[2] = { { 0, 0, 32, 16 }, { 32, 16, 8, 8 } };
    PspExpSpriteRec recs[2] = { { 10, 20, 0, PSPEXP_COLOR_NONE }, { -5, 7, 1, 0x1234 } };
    PspExpVertex2D v[4];
    PspExpTileBatch tb;
    PspExpTileRec tiles[5] = { {0,1}, {1,2}, {0,3}, {1,4}, {0,5} };
    PspExpVertex2D tv[10];

    pspexp_expand_sprites(frames, recs, 2, v);
    CHECK_EQ(v[0].u, 0); CHECK_EQ(v[0].v, 0); CHECK_EQ(v[0].x, 10); CHECK_EQ(v[0].y, 20); CHECK_EQ(v[0].color, 0xFFFF);
    CHECK_EQ(v[1].u, 32); CHECK_EQ(v[1].v, 16); CHECK_EQ(v[1].x, 42); CHECK_EQ(v[1].y, 36); CHECK_EQ(v[1].z, 0);
    CHECK_EQ(v[2].u, 32); CHECK_EQ(v[2].v, 16); CHECK_EQ(v[2].x, -5); CHECK_EQ(v[2].y, 7); CHECK_EQ(v[2].color, 0x1234);
    CHECK_EQ(v[3].u, 40); CHECK_EQ(v[3].v, 24); CHECK_EQ(v[3].x, 3); CHECK_EQ(v[3].y, 15);
    CHECK_EQ(pspexp_sprites_max_frame(recs, 2), 1);

    memset(&tb, 0, sizeof(tb));
    tb.count = 5; tb.origin_x = 100; tb.origin_y = 50; tb.cell_w = 32; tb.cell_h = 16; tb.columns = 2;
    pspexp_expand_tiles(frames, &tb, tiles, tv);
    CHECK_EQ(tv[0].x, 100); CHECK_EQ(tv[0].y, 50);
    CHECK_EQ(tv[2].x, 132); CHECK_EQ(tv[2].y, 50);
    CHECK_EQ(tv[4].x, 100); CHECK_EQ(tv[4].y, 66);
    CHECK_EQ(tv[6].x, 132); CHECK_EQ(tv[6].y, 66);
    CHECK_EQ(tv[8].x, 100); CHECK_EQ(tv[8].y, 82);
    CHECK_EQ(tv[9].x, 132); CHECK_EQ(tv[9].y, 98);
    CHECK_EQ(tv[3].color, 2);
    CHECK_EQ(pspexp_tiles_max_frame(tiles, 5), 1);
    CHECK_EQ(PSPEXP_GU_VERTEX2D_TYPE, 0x00800112u);
    CHECK_EQ(PSPEXP_GU_VERTEX2D_4444_TYPE, 0x00800116u);

    /* Quads: explicit source rects, mirrored when u0 > u1. */
    {
        PspExpQuadRec q[2] = { { 3, 4, 10, 20, 30, 28, 0x1234, 0 }, { 0, 0, 30, 0, 10, 8, 0, 0 } };
        PspExpVertex2D qv[4];
        pspexp_expand_quads(q, 2, qv);
        CHECK_EQ(qv[0].u, 10); CHECK_EQ(qv[0].v, 20); CHECK_EQ(qv[0].x, 3); CHECK_EQ(qv[0].y, 4);
        CHECK_EQ(qv[1].u, 30); CHECK_EQ(qv[1].v, 28); CHECK_EQ(qv[1].x, 23); CHECK_EQ(qv[1].y, 12);
        CHECK_EQ(qv[2].u, 30); CHECK_EQ(qv[3].u, 10); CHECK_EQ(qv[3].x, 20); CHECK_EQ(qv[3].y, 8);
    }

    /* Mirrored expansion swaps u only. */
    pspexp_expand_sprites_flipped(frames, recs, 2, v);
    CHECK_EQ(v[0].u, 32); CHECK_EQ(v[1].u, 0); CHECK_EQ(v[0].v, 0); CHECK_EQ(v[1].v, 16);
    CHECK_EQ(v[0].x, 10); CHECK_EQ(v[1].x, 42);
    CHECK_EQ(v[2].u, 40); CHECK_EQ(v[3].u, 32);
    CHECK_EQ(pspexp_color_4444(255, 0, 0, 255), 0xF00F);
    CHECK_EQ(pspexp_color_4444(0, 0, 0, 0x80), 0x8000);
    CHECK_EQ(sizeof(PspExpSpriteBatch), 16);
    CHECK_EQ(sizeof(PspExpTileBatch), 32);
}

static void test_texture_validation(void)
{
    uint8_t buf[sizeof(PspExpTextureUpload) + 64 * 64 * 2 + 32] __attribute__((aligned(16)));
    PspExpTextureUpload *t = (PspExpTextureUpload *)buf;
    PspExpFrame *frames = (PspExpFrame *)(buf + sizeof(PspExpTextureUpload) + 64 * 64 * 2);
    uint32_t payload;

    CHECK_EQ(sizeof(PspExpTextureUpload), 32);
    memset(buf, 0, sizeof(buf));
    t->id = 1; t->width = 64; t->height = 64; t->format = PSPEXP_TEX_5650; t->layout = PSPEXP_LAYOUT_LINEAR;
    t->frame_count = 2; t->data_length = 64 * 64 * 2;
    frames[0].u0 = 0; frames[0].v0 = 0; frames[0].w = 64; frames[0].h = 64;
    frames[1].u0 = 32; frames[1].v0 = 32; frames[1].w = 32; frames[1].h = 32;
    payload = pspexp_texture_payload_length(t->data_length, t->frame_count);
    CHECK_EQ(payload, 64 * 64 * 2 + 16);
    CHECK_EQ(pspexp_texture_validate(t, PSPEXP_TEX_FORMATS_V01, payload), PSPEXP_RERR_NONE);

    t->format = PSPEXP_TEX_DXT1;   /* not advertised by v0.1 executors */
    CHECK_EQ(pspexp_texture_validate(t, PSPEXP_TEX_FORMATS_V01, payload), PSPEXP_RERR_BAD_FORMAT);
    t->format = PSPEXP_TEX_5650;
    t->width = 48;
    CHECK_EQ(pspexp_texture_validate(t, PSPEXP_TEX_FORMATS_V01, payload), PSPEXP_RERR_BAD_DIMENSIONS);
    t->width = 1024; t->data_length = 1024 * 64 * 2;
    CHECK_EQ(pspexp_texture_validate(t, PSPEXP_TEX_FORMATS_V01, payload), PSPEXP_RERR_BAD_DIMENSIONS);
    t->width = 64; t->data_length = 64 * 64 * 2 - 2;
    CHECK_EQ(pspexp_texture_validate(t, PSPEXP_TEX_FORMATS_V01, payload), PSPEXP_RERR_BAD_LENGTH);
    t->data_length = 64 * 64 * 2;
    CHECK_EQ(pspexp_texture_validate(t, PSPEXP_TEX_FORMATS_V01, payload + 16), PSPEXP_RERR_BAD_LENGTH);
    frames[1].w = 33;
    CHECK_EQ(pspexp_texture_validate(t, PSPEXP_TEX_FORMATS_V01, payload), PSPEXP_RERR_BAD_FRAME_TABLE);
    frames[1].w = 32;
    t->layout = PSPEXP_LAYOUT_SWIZZLED;
    CHECK_EQ(pspexp_texture_validate(t, PSPEXP_TEX_FORMATS_V01, payload), PSPEXP_RERR_NONE);
    t->height = 4; t->data_length = 64 * 4 * 2;
    /* The frame table moves with the (padded) pixel data. */
    frames = (PspExpFrame *)pspexp_texture_frames(t, buf + sizeof(PspExpTextureUpload));
    frames[0].u0 = 0; frames[0].v0 = 0; frames[0].w = 64; frames[0].h = 4;
    frames[1].u0 = 32; frames[1].v0 = 0; frames[1].w = 32; frames[1].h = 4;
    payload = pspexp_texture_payload_length(t->data_length, t->frame_count);
    CHECK_EQ(pspexp_texture_validate(t, PSPEXP_TEX_FORMATS_V01, payload), PSPEXP_RERR_BAD_LAYOUT);
    t->layout = PSPEXP_LAYOUT_LINEAR;
    CHECK_EQ(pspexp_texture_validate(t, PSPEXP_TEX_FORMATS_V01, payload), PSPEXP_RERR_NONE);

    /* Indexed formats: swizzle needs 16-byte rows (T8: width >= 16, T4: width >= 32). */
    t->format = PSPEXP_TEX_T8; t->width = 64; t->height = 64; t->data_length = 64 * 64; t->layout = PSPEXP_LAYOUT_SWIZZLED;
    frames = (PspExpFrame *)pspexp_texture_frames(t, buf + sizeof(PspExpTextureUpload));
    frames[0].u0 = 0; frames[0].v0 = 0; frames[0].w = 64; frames[0].h = 64;
    frames[1].u0 = 32; frames[1].v0 = 32; frames[1].w = 32; frames[1].h = 32;
    payload = pspexp_texture_payload_length(t->data_length, t->frame_count);
    CHECK_EQ(pspexp_texture_validate(t, PSPEXP_TEX_FORMATS_V01, payload), PSPEXP_RERR_NONE);
    t->format = PSPEXP_TEX_T4; t->width = 16; t->data_length = 16 * 64 / 2;
    frames = (PspExpFrame *)pspexp_texture_frames(t, buf + sizeof(PspExpTextureUpload));
    frames[0].u0 = 0; frames[0].v0 = 0; frames[0].w = 16; frames[0].h = 64;
    frames[1].u0 = 8; frames[1].v0 = 32; frames[1].w = 8; frames[1].h = 32;
    payload = pspexp_texture_payload_length(t->data_length, t->frame_count);
    CHECK_EQ(pspexp_texture_validate(t, PSPEXP_TEX_FORMATS_V01, payload), PSPEXP_RERR_BAD_LAYOUT);
    t->layout = PSPEXP_LAYOUT_LINEAR;
    CHECK_EQ(pspexp_texture_validate(t, PSPEXP_TEX_FORMATS_V01, payload), PSPEXP_RERR_NONE);
    CHECK_EQ(pspexp_texture_validate(t, PSPEXP_TEX_FORMATS_V01 & ~PSPEXP_TEX_BIT(PSPEXP_TEX_T4), payload), PSPEXP_RERR_BAD_FORMAT);

    /* Palettes. */
    {
        PspExpPaletteUpload pu;
        memset(&pu, 0, sizeof(pu));
        pu.format = PSPEXP_TEX_5551; pu.entries = 256;
        CHECK_EQ(pspexp_palette_data_length(PSPEXP_TEX_5551, 256), 512);
        CHECK_EQ(pspexp_palette_data_length(PSPEXP_TEX_8888, 16), 64);
        CHECK_EQ(pspexp_palette_data_length(PSPEXP_TEX_T8, 256), 0);
        CHECK_EQ(pspexp_palette_data_length(PSPEXP_TEX_5551, 100), 0);
        CHECK_EQ(pspexp_palette_validate(&pu, 512), PSPEXP_RERR_NONE);
        CHECK_EQ(pspexp_palette_validate(&pu, 528), PSPEXP_RERR_BAD_LENGTH);
        pu.entries = 17;
        CHECK_EQ(pspexp_palette_validate(&pu, 48), PSPEXP_RERR_BAD_PALETTE);
    }

    {
        PspExpSoundUpload su;
        memset(&su, 0, sizeof(su));
        su.channels = 1; su.frames = 1000;
        CHECK_EQ(pspexp_sound_data_length(1, 1000), 2000);
        CHECK_EQ(pspexp_sound_data_length(2, 1000), 4000);
        CHECK_EQ(pspexp_sound_data_length(3, 1000), 0);
        CHECK_EQ(pspexp_sound_validate(&su, 2000), PSPEXP_RERR_NONE);
        CHECK_EQ(pspexp_sound_validate(&su, 2016), PSPEXP_RERR_BAD_LENGTH);
        CHECK_EQ(sizeof(PspExpAudioStatus), 16);
    }

    CHECK_EQ(pspexp_texture_data_length(512, 512, PSPEXP_TEX_8888), 512 * 512 * 4);
    CHECK_EQ(pspexp_texture_data_length(256, 128, PSPEXP_TEX_T8), 256 * 128);
    CHECK_EQ(pspexp_texture_data_length(256, 128, PSPEXP_TEX_T4), 256 * 128 / 2);
    CHECK_EQ(pspexp_texture_data_length(64, 64, PSPEXP_TEX_DXT1), 16 * 16 * 8);
    CHECK_EQ(pspexp_texture_data_length(64, 64, PSPEXP_TEX_DXT5), 16 * 16 * 16);
    CHECK_EQ(PSPEXP_VRAM_TEXTURE_BUDGET, 2097152 - 2 * 512 * 272 * 2);
}

static void test_swizzle(void)
{
    enum { W = 64, H = 16 };  /* 64 bytes wide: 4 blocks per row, 2 block rows */
    uint8_t in[W * H], sw[W * H], back[W * H];
    int i;
    for (i = 0; i < W * H; i++) in[i] = (uint8_t)(i * 31 + 7);
    pspexp_swizzle(sw, in, W, H);
    /* First block holds rows 0..7 of the first 16 bytes. */
    CHECK_EQ(sw[0], in[0]);
    CHECK_EQ(sw[16], in[W]);
    CHECK_EQ(sw[15], in[15]);
    CHECK_EQ(sw[128], in[16]);   /* second block: x 16..31 */
    pspexp_unswizzle(back, sw, W, H);
    CHECK_EQ(memcmp(in, back, sizeof(in)), 0);
}

static void test_colors(void)
{
    uint8_t r, g, b;
    uint16_t c = pspexp_color_5650(255, 0, 0);
    CHECK_EQ(c, 0x001F);
    c = pspexp_color_5650(0, 255, 0);
    CHECK_EQ(c, 0x07E0);
    c = pspexp_color_5650(0, 0, 255);
    CHECK_EQ(c, 0xF800);
    pspexp_color_unpack_5650(pspexp_color_5650(200, 100, 50), &r, &g, &b);
    CHECK(r >= 196 && r <= 207);   /* 5-bit channel, low bits replicated */
    CHECK(g >= 98 && g <= 103);
    CHECK(b >= 48 && b <= 55);
    CHECK_EQ(pspexp_color_5650(255, 255, 255), PSPEXP_COLOR_NONE);
}

int main(void)
{
    CHECK_EQ(sizeof(PspExpMessageHeader), 16);
    CHECK_EQ(PSPEXP_MAGIC, 0x5850);
    CHECK_EQ(PSPEXP_OPCODE_CHANNEL(PSPEXP_OP_DRAW_SPRITE_BATCH), PSPEXP_CH_GRAPHICS);
    CHECK_EQ(PSPEXP_OPCODE_CHANNEL(PSPEXP_OP_BENCH_ECHO), PSPEXP_CH_DEBUG);
    CHECK(strcmp(pspexp_opcode_name(PSPEXP_OP_PRESENT_DONE), "PRESENT_DONE") == 0);
    test_writer_reader_roundtrip();
    test_expansion();
    test_texture_validation();
    test_swizzle();
    test_colors();
    return test_summary("test_protocol");
}
