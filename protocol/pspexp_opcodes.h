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
    PSPEXP_OP_DISCONNECT     = 0x0007,  /* either: orderly close; from the host, a PspExpDisconnect may say the PSP is handed over */
    PSPEXP_OP_CLOCK_SET      = 0x0008,  /* host -> PSP, PspExpClockSet (only with PSPEXP_FEATURE_CLOCK) */
    PSPEXP_OP_CLOCK_STATUS   = 0x0009,  /* PSP -> host, PspExpClockStatus: the answer */
    PSPEXP_OP_POWER_QUERY    = 0x000A,  /* host -> PSP, no payload (only with PSPEXP_FEATURE_POWER) */
    PSPEXP_OP_POWER_STATUS   = 0x000B,  /* PSP -> host, PspExpPowerStatus: the answer */
    PSPEXP_OP_OSD_VOLUME     = 0x000C,  /* host -> PSP, PspExpOsdVolume (only with PSPEXP_FEATURE_OSD) */
    PSPEXP_OP_TEXT_REQUEST   = 0x000D,  /* host -> PSP, PspExpTextRequest + title + text: the PSP's keyboard (PSPEXP_FEATURE_TEXT) */
    PSPEXP_OP_TEXT_RESULT    = 0x000E,  /* PSP -> host, PspExpTextResult + text: what was typed, the answer */
    PSPEXP_OP_RESTART        = 0x000F,  /* host -> PSP, no payload: PSPExp starts itself again, its loader booting the active slot (PSPEXP_FEATURE_RESTART) */

    /* GRAPHICS (0x01xx) */
    PSPEXP_OP_BEGIN_FRAME       = 0x0101,  /* PspExpBeginFrame */
    PSPEXP_OP_SET_CLIP          = 0x0102,  /* PspExpSetClip */
    PSPEXP_OP_DRAW_SPRITE_BATCH = 0x0103,  /* PspExpSpriteBatch + records */
    PSPEXP_OP_DRAW_TILE_BATCH   = 0x0104,  /* PspExpTileBatch + records */
    PSPEXP_OP_DRAW_QUAD_BATCH   = 0x0105,  /* PspExpSpriteBatch + PspExpQuadRec records */
    PSPEXP_OP_DRAW_RECT         = 0x0106,  /* PspExpDrawRect */
    PSPEXP_OP_PRESENT           = 0x0107,  /* no payload */
    PSPEXP_OP_DRAW_LINE         = 0x0108,  /* PspExpDrawLine */
    /* 3D, only with PSPEXP_FEATURE_3D */
    PSPEXP_OP_3D_STATE          = 0x0110,  /* PspExp3DState */
    PSPEXP_OP_3D_MATRICES       = 0x0111,  /* PspExp3DMatrices */
    PSPEXP_OP_3D_LIGHTS         = 0x0112,  /* PspExp3DLights */
    PSPEXP_OP_3D_DRAW           = 0x0113,  /* PspExp3DDraw + vertices */
    PSPEXP_OP_3D_CLEAR          = 0x0114,  /* PspExp3DClear */
    PSPEXP_OP_3D_DRAW_MESH      = 0x0115,  /* PspExp3DDrawMesh [+ 16-bit indices] */
    PSPEXP_OP_3D_TARGET         = 0x0116,  /* PspExp3DTarget: draw into a render target, or the screen */
    PSPEXP_OP_3D_MORPH          = 0x0117,  /* PspExp3DMorph: morph weights (PSPEXP_FEATURE_3D_MORPH) */
    PSPEXP_OP_3D_COPY           = 0x0118,  /* PspExp3DCopy: drawn pixels into a render target (PSPEXP_FEATURE_3D_COPY) */

    /* RESOURCE (0x02xx) */
    PSPEXP_OP_RESOURCE_UPLOAD_TEXTURE = 0x0201,  /* host -> PSP, PspExpTextureUpload + data + frames */
    PSPEXP_OP_RESOURCE_RELEASE        = 0x0202,  /* host -> PSP, PspExpResourceId */
    PSPEXP_OP_RESOURCE_HINT           = 0x0203,  /* host -> PSP, PspExpResourceHint */
    PSPEXP_OP_RESOURCE_READY          = 0x0204,  /* PSP -> host, PspExpResourceReady */
    PSPEXP_OP_RESOURCE_ERROR          = 0x0205,  /* PSP -> host, PspExpResourceError */
    PSPEXP_OP_RESOURCE_TIER           = 0x0206,  /* PSP -> host, PspExpResourceReady */
    PSPEXP_OP_RESOURCE_MISS           = 0x0207,  /* PSP -> host, PspExpResourceError */
    PSPEXP_OP_RESOURCE_UPLOAD_PALETTE = 0x0208,  /* host -> PSP, PspExpPaletteUpload + entries */
    PSPEXP_OP_RESOURCE_UPLOAD_MESH    = 0x0209,  /* host -> PSP, PspExpMeshUpload + data (PSPEXP_FEATURE_3D) */
    PSPEXP_OP_RESOURCE_CREATE_TARGET  = 0x020A,  /* host -> PSP, PspExpTargetCreate (PSPEXP_FEATURE_3D) */
    PSPEXP_OP_RESOURCE_PATCH          = 0x020B,  /* host -> PSP, PspExpResourcePatch + data (PSPEXP_FEATURE_PATCH) */

    /* AUDIO (0x03xx) */
    PSPEXP_OP_RESOURCE_UPLOAD_SOUND = 0x0301,  /* host -> PSP, PspExpSoundUpload + s16 PCM */
    PSPEXP_OP_PLAY_SOUND            = 0x0302,  /* host -> PSP, PspExpPlaySound */
    PSPEXP_OP_STOP_SOUND            = 0x0303,  /* host -> PSP, PspExpStopSound */
    PSPEXP_OP_AUDIO_STREAM          = 0x0304,  /* host -> PSP, PspExpAudioStream + s16 stereo PCM */
    PSPEXP_OP_AUDIO_STATUS          = 0x0305,  /* PSP -> host, PspExpAudioStatus */
    PSPEXP_OP_AUDIO_STREAM_RESET    = 0x0306,  /* host -> PSP, no payload: drop buffered stream */
    PSPEXP_OP_AUDIO_CODED_STREAM    = 0x0307,  /* host -> PSP, PspExpAudioCoded + compressed frames (PSPEXP_FEATURE_AUDIO_MP3) */

    /* INPUT (0x04xx) */
    PSPEXP_OP_INPUT_SAMPLE   = 0x0401,  /* PSP -> host, PspExpInput */

    /* DEBUG (0x05xx) */
    PSPEXP_OP_BENCH_START    = 0x0501,  /* host -> PSP, PspExpBenchStart */
    PSPEXP_OP_BENCH_DATA     = 0x0502,  /* either, arbitrary payload */
    PSPEXP_OP_BENCH_END      = 0x0503,  /* host -> PSP, no payload */
    PSPEXP_OP_BENCH_REPORT   = 0x0504,  /* PSP -> host, PspExpBenchReport */
    PSPEXP_OP_BENCH_ECHO     = 0x0505,  /* either, payload returned unchanged */
    PSPEXP_OP_BENCH_UPLINK   = 0x0506,  /* host -> PSP, PspExpBenchUplink */
    PSPEXP_OP_DEBUG_SCREENSHOT = 0x0507, /* host -> PSP, PspExpScreenshot: dump the displayed frame to a file */
    PSPEXP_OP_CAPTURE_SET    = 0x0508,  /* host -> PSP, PspExpCaptureSet: PSPExp Capture on or off (PSPEXP_FEATURE_CAPTURE) */
    PSPEXP_OP_CAPTURE_DATA   = 0x0509,  /* PSP -> host, PspExpCaptureData + bytes: the next bytes of the capture stream */

    /* FILE (0x06xx), only with PSPEXP_FEATURE_FILES: host -> PSP PspExpFsRequest,
     * PSP -> host FS_REPLY */
    PSPEXP_OP_FS_LIST    = 0x0601,  /* path: its entries from index offset */
    PSPEXP_OP_FS_READ    = 0x0602,  /* path, offset, length: the bytes */
    PSPEXP_OP_FS_WRITE   = 0x0603,  /* path, offset, flags + data */
    PSPEXP_OP_FS_MKDIR   = 0x0604,
    PSPEXP_OP_FS_REMOVE  = 0x0605,  /* a file, or an empty folder */
    PSPEXP_OP_FS_RENAME  = 0x0606,  /* path -> path2 */
    PSPEXP_OP_FS_INFO    = 0x0607,  /* path's device: free and total bytes */
    PSPEXP_OP_FS_STAT    = 0x0608,  /* path: one entry (count 1), or NOT_FOUND */
    PSPEXP_OP_FS_REPLY   = 0x0610   /* PSP -> host, PspExpFsReply + entries / data */
};

#define PSPEXP_OPCODE_CHANNEL(op) ((uint8_t)((op) >> 8))

#endif /* PSPEXP_OPCODES_H */
