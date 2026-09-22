/*
 * PSPExp opcodes. Opcodes are unique across channels: the high byte is the
 * channel number, which makes a dump readable and a mismatch detectable.
 */
#ifndef PSPEXP_OPCODES_H
#define PSPEXP_OPCODES_H

enum PspExpOpcode {
    /* SYSTEM (0x00xx) */
    PSPEXP_OP_HELLO          = 0x0001,  /* PSP -> host, PspExpHello */
    PSPEXP_OP_HELLO_ACK      = 0x0002,  /* host -> PSP, PspExpHelloAck */
    PSPEXP_OP_PRESENT_DONE   = 0x0003,  /* PSP -> host, PspExpPresentDone */
    PSPEXP_OP_MEMORY_QUERY   = 0x0004,  /* host -> PSP, no payload */
    PSPEXP_OP_MEMORY_STATUS  = 0x0005,  /* PSP -> host, PspExpMemoryStatus */
    PSPEXP_OP_ERROR          = 0x0006,  /* either, PspExpError */
    PSPEXP_OP_DISCONNECT     = 0x0007,  /* either, no payload: orderly close */

    /* GRAPHICS (0x01xx) */
    PSPEXP_OP_BEGIN_FRAME       = 0x0101,  /* PspExpBeginFrame */
    PSPEXP_OP_SET_CLIP          = 0x0102,  /* PspExpSetClip */
    PSPEXP_OP_DRAW_SPRITE_BATCH = 0x0103,  /* PspExpSpriteBatch + records */
    PSPEXP_OP_DRAW_TILE_BATCH   = 0x0104,  /* PspExpTileBatch + records */
    PSPEXP_OP_DRAW_QUAD_BATCH   = 0x0105,  /* reserved (arbitrary source rects) */
    PSPEXP_OP_DRAW_RECT         = 0x0106,  /* PspExpDrawRect */
    PSPEXP_OP_PRESENT           = 0x0107,  /* no payload */

    /* RESOURCE (0x02xx) */
    PSPEXP_OP_RESOURCE_UPLOAD_TEXTURE = 0x0201,  /* host -> PSP, PspExpTextureUpload + data + frames */
    PSPEXP_OP_RESOURCE_RELEASE        = 0x0202,  /* host -> PSP, PspExpResourceId */
    PSPEXP_OP_RESOURCE_HINT           = 0x0203,  /* host -> PSP, PspExpResourceHint */
    PSPEXP_OP_RESOURCE_READY          = 0x0204,  /* PSP -> host, PspExpResourceReady */
    PSPEXP_OP_RESOURCE_ERROR          = 0x0205,  /* PSP -> host, PspExpResourceError */
    PSPEXP_OP_RESOURCE_TIER           = 0x0206,  /* PSP -> host, PspExpResourceReady */
    PSPEXP_OP_RESOURCE_MISS           = 0x0207,  /* PSP -> host, PspExpResourceError */

    /* AUDIO (0x03xx): v0.3 */

    /* INPUT (0x04xx) */
    PSPEXP_OP_INPUT_SAMPLE   = 0x0401,  /* PSP -> host, PspExpInput */

    /* DEBUG (0x05xx) */
    PSPEXP_OP_BENCH_START    = 0x0501,  /* host -> PSP, PspExpBenchStart */
    PSPEXP_OP_BENCH_DATA     = 0x0502,  /* either, arbitrary payload */
    PSPEXP_OP_BENCH_END      = 0x0503,  /* host -> PSP, no payload */
    PSPEXP_OP_BENCH_REPORT   = 0x0504,  /* PSP -> host, PspExpBenchReport */
    PSPEXP_OP_BENCH_ECHO     = 0x0505,  /* either, payload returned unchanged */
    PSPEXP_OP_BENCH_UPLINK   = 0x0506   /* host -> PSP, PspExpBenchUplink */
};

#define PSPEXP_OPCODE_CHANNEL(op) ((uint8_t)((op) >> 8))

#endif /* PSPEXP_OPCODES_H */
