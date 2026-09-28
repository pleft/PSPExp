/*
 * PSPExp wire protocol - shared by the host (any C99 compiler) and the PSP
 * runtime (PSPSDK GCC). Plain C99, no platform headers, no allocation.
 *
 * See docs/PSPExp_Technical_Architecture.md, sections 3.2, 5.4, 6, 9 and 10.
 *
 * Every message is a 16-byte header followed by `length` payload bytes.
 * `length` is always a multiple of 16, so when a message starts on a 16-byte
 * boundary every payload does too (GE-consumed payloads can then be read in
 * place, section 9). All fields are little-endian; every target processor is
 * little-endian (section 3.2), so structures are laid out on the wire exactly
 * as they are in memory and no field is ever byte-swapped. Every structure
 * below is naturally aligned with no implicit padding; the static asserts
 * enforce the sizes.
 */
#ifndef PSPEXP_PROTOCOL_H
#define PSPEXP_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSPEXP_STATIC_ASSERT(cond, name) typedef char pspexp_static_assert_##name[(cond) ? 1 : -1]

/* ---- Framing ----------------------------------------------------------- */

#define PSPEXP_MAGIC             0x5850u   /* "PX" as a little-endian uint16 */
#define PSPEXP_PROTOCOL_VERSION  1
#define PSPEXP_HEADER_SIZE       16
#define PSPEXP_ALIGN             16
#define PSPEXP_ALIGN_UP(n)       (((uint32_t)(n) + (PSPEXP_ALIGN - 1u)) & ~(uint32_t)(PSPEXP_ALIGN - 1u))
/* Largest payload any executor must accept: a 512x512 RGBA8888 texture plus
 * its frame table, or a 4 MiB audio stream block later. */
#define PSPEXP_MAX_PAYLOAD       (4u * 1024u * 1024u)

typedef struct PspExpMessageHeader {
    uint16_t magic;      /* PSPEXP_MAGIC */
    uint8_t  version;    /* PSPEXP_PROTOCOL_VERSION */
    uint8_t  channel;    /* PspExpChannel */
    uint16_t opcode;     /* pspexp_opcodes.h */
    uint16_t flags;      /* PSPEXP_FLAG_* */
    uint32_t sequence;   /* per direction, increments by one per message */
    uint32_t length;     /* payload bytes following the header, multiple of 16 */
} PspExpMessageHeader;
PSPEXP_STATIC_ASSERT(sizeof(PspExpMessageHeader) == PSPEXP_HEADER_SIZE, header_size);

enum PspExpChannel {
    PSPEXP_CH_SYSTEM   = 0,  /* handshake, reconnect, PRESENT_DONE, errors */
    PSPEXP_CH_GRAPHICS = 1,  /* frames */
    PSPEXP_CH_RESOURCE = 2,  /* uploads, releases, residency */
    PSPEXP_CH_AUDIO    = 3,  /* cached SFX control, PCM stream (v0.3) */
    PSPEXP_CH_INPUT    = 4,  /* controller samples, PSP -> host */
    PSPEXP_CH_DEBUG    = 5,  /* metrics, traces, transport benchmark */
    PSPEXP_CH_FILE     = 6,  /* the PSP's memory stick, for the host (only with PSPEXP_FEATURE_FILES) */
    PSPEXP_CH_COUNT    = 7
};

#define PSPEXP_FLAG_NONE 0u

/* ---- Fixed hardware facts (section 3.2) ------------------------------- */

#define PSPEXP_DISPLAY_WIDTH    480
#define PSPEXP_DISPLAY_HEIGHT   272
#define PSPEXP_DISPLAY_STRIDE   512
#define PSPEXP_DISPLAY_REFRESH  60
#define PSPEXP_TEXTURE_MAX      512      /* hardware corrupts anything larger */
#define PSPEXP_FRAME_MAX_WIDTH  64       /* texture-cache friendly strip width */
#define PSPEXP_VRAM_BYTES       (2u * 1024u * 1024u)
#define PSPEXP_FRAMEBUFFER_BYTES ((uint32_t)PSPEXP_DISPLAY_STRIDE * PSPEXP_DISPLAY_HEIGHT * 2u)
/* Two 16-bit framebuffers, no depth buffer (2D does not need one). */
#define PSPEXP_VRAM_TEXTURE_BUDGET (PSPEXP_VRAM_BYTES - 2u * PSPEXP_FRAMEBUFFER_BYTES)

/* Texture pixel formats. Values equal the PSP GE GU_PSM_* constants so the
 * PSP runtime passes them straight to sceGuTexImage. */
enum PspExpTextureFormat {
    PSPEXP_TEX_5650 = 0,
    PSPEXP_TEX_5551 = 1,
    PSPEXP_TEX_4444 = 2,
    PSPEXP_TEX_8888 = 3,
    PSPEXP_TEX_T4   = 4,   /* CLUT4  (v0.3) */
    PSPEXP_TEX_T8   = 5,   /* CLUT8  (v0.3) */
    PSPEXP_TEX_T16  = 6,
    PSPEXP_TEX_T32  = 7,
    PSPEXP_TEX_DXT1 = 8,
    PSPEXP_TEX_DXT3 = 9,
    PSPEXP_TEX_DXT5 = 10,
    PSPEXP_TEX_COUNT = 11
};
#define PSPEXP_TEX_BIT(fmt) ((uint16_t)(1u << (fmt)))
/* Formats every executor implements: the four direct formats plus CLUT4/CLUT8. */
#define PSPEXP_TEX_FORMATS_V01 (PSPEXP_TEX_BIT(PSPEXP_TEX_5650) | PSPEXP_TEX_BIT(PSPEXP_TEX_5551) | \
                                PSPEXP_TEX_BIT(PSPEXP_TEX_4444) | PSPEXP_TEX_BIT(PSPEXP_TEX_8888) | \
                                PSPEXP_TEX_BIT(PSPEXP_TEX_T4)   | PSPEXP_TEX_BIT(PSPEXP_TEX_T8))
#define PSPEXP_TEX_IS_INDEXED(fmt) ((fmt) == PSPEXP_TEX_T4 || (fmt) == PSPEXP_TEX_T8)
/* Palette (CLUT) entry formats are the direct texture formats 5650..8888. */
#define PSPEXP_PALETTE_ENTRIES_T4  16
#define PSPEXP_PALETTE_ENTRIES_T8  256

enum PspExpTextureLayout {
    PSPEXP_LAYOUT_LINEAR   = 0,
    PSPEXP_LAYOUT_SWIZZLED = 1   /* PSP 16-byte x 8-row block order */
};

enum PspExpPriority {
    PSPEXP_PRIORITY_LOW    = 0,   /* placed in RAM, never VRAM: for textures re-uploaded every frame */
    PSPEXP_PRIORITY_NORMAL = 1,
    PSPEXP_PRIORITY_HIGH   = 2
};

enum PspExpTier {
    PSPEXP_TIER_VRAM = 0,
    PSPEXP_TIER_RAM  = 1
};

/* Vertex formats a batch may request; advertised as a bit mask in HELLO. */
enum PspExpVertexFormat {
    PSPEXP_VTX_SPRITE_2D_5650 = 0,  /* PspExpVertex2D, colour = RGB 5650 tint (no alpha) */
    PSPEXP_VTX_SPRITE_2D_4444 = 1,  /* same layout, colour = RGBA 4444 tint with per-sprite alpha */
    PSPEXP_VTX_COUNT = 2
};
#define PSPEXP_VTX_BIT(fmt) ((uint16_t)(1u << (fmt)))

/* Controller buttons: the PSP_CTRL_* bit values of pspctrl.h. */
#define PSPEXP_BUTTON_SELECT    0x000001u
#define PSPEXP_BUTTON_START     0x000008u
#define PSPEXP_BUTTON_UP        0x000010u
#define PSPEXP_BUTTON_RIGHT     0x000020u
#define PSPEXP_BUTTON_DOWN      0x000040u
#define PSPEXP_BUTTON_LEFT      0x000080u
#define PSPEXP_BUTTON_LTRIGGER  0x000100u
#define PSPEXP_BUTTON_RTRIGGER  0x000200u
#define PSPEXP_BUTTON_TRIANGLE  0x001000u
#define PSPEXP_BUTTON_CIRCLE    0x002000u
#define PSPEXP_BUTTON_CROSS     0x004000u
#define PSPEXP_BUTTON_SQUARE    0x008000u
#define PSPEXP_BUTTON_HOME      0x010000u
#define PSPEXP_BUTTON_HOLD      0x020000u
#define PSPEXP_BUTTON_NOTE      0x800000u
#define PSPEXP_ANALOG_CENTER    128

/* Resource error reasons (RESOURCE_ERROR.reason). */
enum PspExpResourceErrorReason {
    PSPEXP_RERR_NONE            = 0,
    PSPEXP_RERR_OUT_OF_MEMORY   = 1,  /* no tier can hold it; host must release something */
    PSPEXP_RERR_BAD_FORMAT      = 2,  /* format not in the advertised set */
    PSPEXP_RERR_BAD_DIMENSIONS  = 3,  /* not power of two, or above PSPEXP_TEXTURE_MAX */
    PSPEXP_RERR_BAD_LENGTH      = 4,  /* data_length does not match width x height x format */
    PSPEXP_RERR_BAD_LAYOUT      = 5,  /* swizzled layout with incompatible dimensions */
    PSPEXP_RERR_BAD_FRAME_TABLE = 6,  /* a frame lies outside the texture */
    PSPEXP_RERR_DUPLICATE_ID    = 7,  /* handle already resident */
    PSPEXP_RERR_UNKNOWN_ID      = 8,  /* release/hint/draw of a handle that is not resident */
    PSPEXP_RERR_TABLE_FULL      = 9,  /* executor's handle table is full */
    PSPEXP_RERR_BAD_PALETTE     = 10, /* indexed texture without a suitable resident palette, or bad palette upload */
    PSPEXP_RERR_WRONG_KIND      = 11, /* handle exists but is not a texture / palette */
    PSPEXP_RERR_COUNT           = 12
};

/* ---- SYSTEM channel payloads ------------------------------------------ */

/* PSP -> host on every (re)connection. Any HELLO invalidates all PSP-side
 * state: the runtime has released every resource before sending it. */
typedef struct PspExpHello {
    uint16_t protocol;          /* PSPEXP_PROTOCOL_VERSION */
    uint16_t runtime_version;
    uint16_t display_width;
    uint16_t display_height;
    uint8_t  refresh_hz;
    uint8_t  audio_channels;
    uint8_t  input_rate_max;
    uint8_t  executor;          /* PspExpExecutorKind, informational */
    uint16_t texture_formats;   /* PSPEXP_TEX_BIT mask */
    uint16_t vertex_formats;    /* PSPEXP_VTX_BIT mask */
    uint16_t texture_max;       /* 512 */
    uint16_t max_batch_records; /* records per DRAW_*_BATCH the runtime accepts */
    uint32_t vram_budget;       /* bytes available for textures in VRAM */
    uint32_t ram_budget;        /* bytes available for resources in main RAM */
    uint32_t features;          /* PSPEXP_FEATURE_* */
    uint16_t audio_rate_min;
    uint16_t audio_rate_max;
    uint32_t usb_chunk;         /* transport hint: preferred host->PSP transfer size, 0 = n/a */
    uint8_t  reserved[8];
} PspExpHello;
PSPEXP_STATIC_ASSERT(sizeof(PspExpHello) == 48, hello_size);

enum PspExpExecutorKind {
    PSPEXP_EXECUTOR_NULL = 0,
    PSPEXP_EXECUTOR_SDL  = 1,
    PSPEXP_EXECUTOR_PSP  = 2
};

#define PSPEXP_FEATURE_BENCH      0x00000001u  /* answers DEBUG-channel benchmark messages */
#define PSPEXP_FEATURE_TIMESTAMPS 0x00000002u  /* PRESENT_DONE/INPUT timestamps are meaningful */
#define PSPEXP_FEATURE_FILES      0x00000004u  /* answers the FILE channel: the memory stick for the host.
                                                  A host must not send on that channel otherwise: runtimes
                                                  before it drop a link on a channel they do not know */
#define PSPEXP_FEATURE_CLOCK      0x00000008u  /* answers CLOCK_SET: the host may set the PSP's clock */

/* Host -> PSP (CLOCK_SET): the board's clock, for a PSP whose own one is
 * wrong (a flat clock battery puts it back years). The PSP keeps UTC; what
 * it shows is UTC plus its own time-zone setting, which is not changed. */
typedef struct PspExpClockSet {
    uint32_t utc_lo, utc_hi;    /* seconds since 1970, UTC */
    int32_t  tz_minutes;        /* the board's offset from UTC, east positive (for the report) */
    uint32_t min_diff;          /* set only when the PSP is further off (seconds); 0: always; PSPEXP_CLOCK_REPORT_ONLY: never */
} PspExpClockSet;
#define PSPEXP_CLOCK_REPORT_ONLY 0xffffffffu

/* PSP -> host (CLOCK_STATUS). */
typedef struct PspExpClockStatus {
    uint32_t before_lo, before_hi;   /* the PSP's UTC clock before, seconds since 1970 */
    uint32_t after_lo, after_hi;     /* and after */
    int32_t  tz_minutes;             /* the PSP's time-zone setting, east positive */
    int32_t  dst;                    /* its daylight-saving setting: 1 adds an hour */
    int32_t  result;                 /* 1 set, 0 left as it was, < 0 could not */
    uint32_t reserved;
} PspExpClockStatus;

/* Host -> PSP, reply to HELLO. */
typedef struct PspExpHelloAck {
    uint16_t protocol;
    uint16_t input_rate;        /* Hz, <= HELLO.input_rate_max */
    uint32_t audio_rate;        /* 0 = no audio stream */
    uint8_t  audio_channels;
    uint8_t  reserved[7];
} PspExpHelloAck;
PSPEXP_STATIC_ASSERT(sizeof(PspExpHelloAck) == 16, hello_ack_size);

/* PSP -> host after each vblank at which a new frame was displayed. */
typedef struct PspExpPresentDone {
    uint32_t frame_seq;         /* BEGIN_FRAME.frame_seq of the displayed frame */
    uint32_t timestamp_us;      /* PSP clock at the vblank */
    uint16_t queue_depth;       /* frames still queued after this one */
    uint16_t dropped;           /* frames discarded before execution, cumulative */
    uint32_t execute_us;        /* parse + expand + display-list build, CPU */
    uint32_t ge_us;             /* GE render time (list finish to sync), 0 if unknown */
    uint32_t frame_bytes;       /* bytes of the frame as received */
    uint32_t sprites_drawn;     /* sprites that reached the GE this frame (older runtimes: 0) */
    uint32_t sprites_dropped;   /* sprites dropped this frame: no vertex space (older runtimes: 0) */
} PspExpPresentDone;
PSPEXP_STATIC_ASSERT(sizeof(PspExpPresentDone) == 32, present_done_size);

typedef struct PspExpMemoryStatus {
    uint32_t vram_free;
    uint32_t ram_free;
    uint16_t vram_count;
    uint16_t ram_count;
    uint8_t  reserved[4];
} PspExpMemoryStatus;
PSPEXP_STATIC_ASSERT(sizeof(PspExpMemoryStatus) == 16, memory_status_size);

/* Either direction: a protocol-level problem. */
typedef struct PspExpError {
    uint16_t code;              /* PspExpErrorCode */
    uint16_t channel;           /* offending message, if any */
    uint16_t opcode;
    uint16_t reserved0;
    uint32_t sequence;
    uint32_t detail;
} PspExpError;
PSPEXP_STATIC_ASSERT(sizeof(PspExpError) == 16, error_size);

enum PspExpErrorCode {
    PSPEXP_ERR_NONE            = 0,
    PSPEXP_ERR_BAD_OPCODE      = 1,
    PSPEXP_ERR_BAD_LENGTH      = 2,  /* payload too short for its opcode */
    PSPEXP_ERR_NOT_IN_FRAME    = 3,  /* draw outside BEGIN_FRAME..PRESENT */
    PSPEXP_ERR_BATCH_TOO_LARGE = 4,  /* count > HELLO.max_batch_records */
    PSPEXP_ERR_BAD_VERTEX_FMT  = 5,
    PSPEXP_ERR_NOT_CONNECTED   = 6   /* traffic before HELLO_ACK */
};

/* ---- GRAPHICS channel payloads ---------------------------------------- */

typedef struct PspExpBeginFrame {
    uint32_t frame_seq;
    uint32_t input_seq;         /* newest INPUT.sequence the host consumed, for latency */
    uint32_t input_timestamp_us;/* its PSP timestamp */
    uint8_t  reserved[4];
} PspExpBeginFrame;
PSPEXP_STATIC_ASSERT(sizeof(PspExpBeginFrame) == 16, begin_frame_size);

typedef struct PspExpSetClip {
    int16_t x, y, w, h;
    uint8_t reserved[8];
} PspExpSetClip;
PSPEXP_STATIC_ASSERT(sizeof(PspExpSetClip) == 16, set_clip_size);

typedef struct PspExpDrawRect {
    int16_t  x, y, w, h;
    uint16_t color;             /* RGB 5650 */
    uint8_t  alpha;             /* 255 = opaque */
    uint8_t  reserved[5];
} PspExpDrawRect;
PSPEXP_STATIC_ASSERT(sizeof(PspExpDrawRect) == 16, draw_rect_size);

typedef struct PspExpDrawLine {
    int16_t  x0, y0, x1, y1;    /* inclusive end points */
    uint16_t color;             /* RGB 5650 */
    uint8_t  alpha;
    uint8_t  reserved[5];
} PspExpDrawLine;
PSPEXP_STATIC_ASSERT(sizeof(PspExpDrawLine) == 16, draw_line_size);

#define PSPEXP_BATCH_FLIP_X      0x0001u   /* mirror every sprite horizontally */
#define PSPEXP_BATCH_SCALED      0x0002u   /* DRAW_QUAD_BATCH records are PspExpQuadExRec */
#define PSPEXP_PALETTE_DEFAULT   0xFFFFu   /* use the atlas's own palette */

/* Followed by `count` PspExpSpriteRec (pspexp_batch.h), padded to 16 bytes. */
typedef struct PspExpSpriteBatch {
    uint16_t atlas;             /* texture handle */
    uint16_t count;
    uint16_t vertex_format;     /* PspExpVertexFormat */
    uint16_t flags;             /* PSPEXP_BATCH_* */
    uint16_t palette;           /* palette override for an indexed atlas, or PSPEXP_PALETTE_DEFAULT */
    uint8_t  reserved[6];
} PspExpSpriteBatch;
PSPEXP_STATIC_ASSERT(sizeof(PspExpSpriteBatch) == 16, sprite_batch_size);

/* Followed by `count` PspExpTileRec. Tile i is drawn at
 * (origin_x + (i % columns) * cell_w, origin_y + (i / columns) * cell_h). */
typedef struct PspExpTileBatch {
    uint16_t atlas;
    uint16_t count;
    int16_t  origin_x, origin_y;
    uint16_t cell_w, cell_h;
    uint16_t columns;
    uint16_t vertex_format;
    uint16_t palette;           /* override or PSPEXP_PALETTE_DEFAULT */
    uint8_t  reserved[14];
} PspExpTileBatch;
PSPEXP_STATIC_ASSERT(sizeof(PspExpTileBatch) == 32, tile_batch_size);

/* PRESENT has no payload. */

/* ---- RESOURCE channel payloads ---------------------------------------- */

/* Followed by data_length pixel bytes padded to 16, then frame_count
 * PspExpFrame entries padded to 16. Use pspexp_texture_payload_length(). */
typedef struct PspExpTextureUpload {
    uint16_t id;
    uint16_t width;
    uint16_t height;
    uint8_t  format;            /* PspExpTextureFormat */
    uint8_t  layout;            /* PspExpTextureLayout */
    uint8_t  priority;          /* PspExpPriority */
    uint8_t  reserved0;
    uint16_t frame_count;
    uint32_t data_length;
    uint16_t palette;           /* palette handle for T4/T8, must already be resident */
    uint8_t  reserved[14];
} PspExpTextureUpload;
PSPEXP_STATIC_ASSERT(sizeof(PspExpTextureUpload) == 32, texture_upload_size);

/* RESOURCE_UPLOAD_PALETTE: followed by `entries` colours in `format`
 * (2 or 4 bytes each), padded to 16 bytes. */
typedef struct PspExpPaletteUpload {
    uint16_t id;
    uint8_t  format;            /* PSPEXP_TEX_5650 .. PSPEXP_TEX_8888 */
    uint8_t  reserved0;
    uint16_t entries;           /* 16 or 256 */
    uint8_t  reserved[10];
} PspExpPaletteUpload;
PSPEXP_STATIC_ASSERT(sizeof(PspExpPaletteUpload) == 16, palette_upload_size);

typedef struct PspExpResourceId {      /* RESOURCE_RELEASE */
    uint16_t id;
    uint8_t  reserved[14];
} PspExpResourceId;
PSPEXP_STATIC_ASSERT(sizeof(PspExpResourceId) == 16, resource_id_size);

typedef struct PspExpResourceHint {    /* RESOURCE_HINT */
    uint16_t id;
    uint8_t  priority;
    uint8_t  reserved[13];
} PspExpResourceHint;
PSPEXP_STATIC_ASSERT(sizeof(PspExpResourceHint) == 16, resource_hint_size);

typedef struct PspExpResourceReady {   /* RESOURCE_READY, RESOURCE_TIER */
    uint16_t id;
    uint8_t  tier;              /* PspExpTier */
    uint8_t  reserved[13];
} PspExpResourceReady;
PSPEXP_STATIC_ASSERT(sizeof(PspExpResourceReady) == 16, resource_ready_size);

typedef struct PspExpResourceError {   /* RESOURCE_ERROR, RESOURCE_MISS */
    uint16_t id;
    uint16_t reason;            /* PspExpResourceErrorReason */
    uint32_t frame_seq;         /* for RESOURCE_MISS: frame that referenced it */
    uint8_t  reserved[8];
} PspExpResourceError;
PSPEXP_STATIC_ASSERT(sizeof(PspExpResourceError) == 16, resource_error_size);

/* ---- AUDIO channel payloads (section 8) -------------------------------- */

/* Cached sound effect: followed by frames x channels s16 samples, already
 * at the negotiated output rate (the host resamples), padded to 16 bytes. */
typedef struct PspExpSoundUpload {
    uint16_t id;
    uint8_t  channels;          /* 1 or 2 */
    uint8_t  reserved0;
    uint32_t frames;
    uint32_t sample_rate;       /* informational; must equal HELLO_ACK.audio_rate */
    uint8_t  reserved[4];
} PspExpSoundUpload;
PSPEXP_STATIC_ASSERT(sizeof(PspExpSoundUpload) == 16, sound_upload_size);

typedef struct PspExpPlaySound {
    uint16_t id;
    uint8_t  volume;            /* 0..255 */
    int8_t   pan;               /* -128 left .. 127 right */
    uint8_t  loop;
    uint8_t  reserved[11];
} PspExpPlaySound;
PSPEXP_STATIC_ASSERT(sizeof(PspExpPlaySound) == 16, play_sound_size);

typedef struct PspExpStopSound {
    uint16_t id;                /* stops every voice playing this sound; 0xFFFF = all */
    uint8_t  reserved[14];
} PspExpStopSound;
PSPEXP_STATIC_ASSERT(sizeof(PspExpStopSound) == 16, stop_sound_size);

/* Streamed audio: followed by `frames` s16 stereo frames at the negotiated rate. */
typedef struct PspExpAudioStream {
    uint32_t frames;
    uint8_t  reserved[12];
} PspExpAudioStream;
PSPEXP_STATIC_ASSERT(sizeof(PspExpAudioStream) == 16, audio_stream_size);

typedef struct PspExpAudioStatus {
    uint32_t buffered_frames;   /* stream frames waiting to play */
    uint32_t underruns;         /* cumulative */
    uint32_t received_frames;   /* cumulative */
    uint32_t dropped_frames;    /* cumulative, buffer overflow */
} PspExpAudioStatus;
PSPEXP_STATIC_ASSERT(sizeof(PspExpAudioStatus) == 16, audio_status_size);

#define PSPEXP_AUDIO_MAX_VOICES 8

/* ---- INPUT channel payload -------------------------------------------- */

typedef struct PspExpInput {
    uint32_t sequence;          /* monotonically increasing per sample */
    uint32_t timestamp_us;      /* PSP clock, for latency measurement */
    uint32_t buttons;           /* PSPEXP_BUTTON_* */
    uint8_t  analog_x;          /* 0..255, 128 = centre */
    uint8_t  analog_y;
    uint8_t  reserved[2];
} PspExpInput;
PSPEXP_STATIC_ASSERT(sizeof(PspExpInput) == 16, input_size);

/* ---- DEBUG channel: transport benchmark (section 17, v0.1 USB measurement) */

/* Host -> PSP: reset counters. The runtime timestamps the first BENCH_DATA
 * it sees afterwards. */
typedef struct PspExpBenchStart {
    uint32_t expected_bytes;    /* informational */
    uint32_t block_size;
    uint8_t  reserved[8];
} PspExpBenchStart;
PSPEXP_STATIC_ASSERT(sizeof(PspExpBenchStart) == 16, bench_start_size);

/* BENCH_DATA carries an arbitrary payload; only its length matters.
 * BENCH_END has no payload and is answered with BENCH_REPORT. */
typedef struct PspExpBenchReport {
    uint32_t bytes;             /* payload bytes counted since BENCH_START */
    uint32_t messages;
    uint32_t elapsed_us;        /* first BENCH_DATA arrival to BENCH_END, PSP clock */
    uint32_t timestamp_us;      /* PSP clock when the report was written */
} PspExpBenchReport;
PSPEXP_STATIC_ASSERT(sizeof(PspExpBenchReport) == 16, bench_report_size);

/* Host -> PSP: send `count` BENCH_DATA messages of `block_size` payload bytes
 * back to the host, followed by BENCH_REPORT. */
typedef struct PspExpBenchUplink {
    uint32_t block_size;
    uint32_t count;
    uint8_t  reserved[8];
} PspExpBenchUplink;
PSPEXP_STATIC_ASSERT(sizeof(PspExpBenchUplink) == 16, bench_uplink_size);

/* BENCH_ECHO: the runtime returns the message unchanged (same payload). */

/* DEBUG_SCREENSHOT: the executor saves the frame currently displayed as
 * pspexp-shot-<index>.bmp (PSP: next to the EBOOT; SDL: working directory). */
typedef struct PspExpScreenshot {
    uint16_t index;
    uint8_t  reserved[14];
} PspExpScreenshot;
PSPEXP_STATIC_ASSERT(sizeof(PspExpScreenshot) == 16, screenshot_size);

/* ---- FILE channel: the PSP's memory stick for the host ------------------ */

/* Paths are the PSP's: "ms0:/PSP/GAME" (memory stick), "ef0:/..." (PSP Go
 * internal storage); the runtime refuses anything else. One request at a
 * time is enough: the host waits for each reply (matched by req_id). */
#define PSPEXP_FS_PATH       256
#define PSPEXP_FS_NAME       112
#define PSPEXP_FS_CHUNK      (64u * 1024u)   /* the most READ returns / WRITE carries */
#define PSPEXP_FS_LIST_MAX   64              /* entries per LIST reply; more: ask again from index */

/* host -> PSP, for every FS_* request; WRITE's data follows it */
typedef struct PspExpFsRequest {
    uint32_t req_id;
    uint32_t flags;             /* WRITE: PSPEXP_FS_CREATE | PSPEXP_FS_TRUNCATE */
    uint32_t offset_lo, offset_hi;   /* READ / WRITE: file offset; LIST: first entry index */
    uint32_t length;            /* READ: bytes wanted (<= PSPEXP_FS_CHUNK); WRITE: bytes following */
    uint32_t reserved[3];
    char     path[PSPEXP_FS_PATH];
    char     path2[PSPEXP_FS_PATH];  /* RENAME: the new path */
} PspExpFsRequest;
PSPEXP_STATIC_ASSERT(sizeof(PspExpFsRequest) == 544, fs_request_size);

#define PSPEXP_FS_CREATE   0x1u
#define PSPEXP_FS_TRUNCATE 0x2u

/* PSP -> host, FS_REPLY: the result, then (LIST) entries or (READ) data */
typedef struct PspExpFsReply {
    uint32_t req_id;
    int32_t  status;            /* >= 0 ok (READ / WRITE: bytes), < 0 a PSPEXP_FSERR_* */
    uint32_t count;             /* LIST: entries following */
    uint32_t more;              /* LIST: 1 when entries remain after these */
    uint32_t value_lo, value_hi;     /* INFO: free bytes */
    uint32_t value2_lo, value2_hi;   /* INFO: total bytes */
} PspExpFsReply;
PSPEXP_STATIC_ASSERT(sizeof(PspExpFsReply) == 32, fs_reply_size);

typedef struct PspExpFsEntry {
    uint32_t size_lo, size_hi;
    uint32_t mtime;             /* seconds since 1970, 0 unknown */
    uint32_t flags;             /* PSPEXP_FS_DIR */
    char     name[PSPEXP_FS_NAME];
} PspExpFsEntry;
PSPEXP_STATIC_ASSERT(sizeof(PspExpFsEntry) == 128, fs_entry_size);

#define PSPEXP_FS_DIR 0x1u

enum PspExpFsError {
    PSPEXP_FSERR_IO        = -1,   /* the device failed */
    PSPEXP_FSERR_NOT_FOUND = -2,
    PSPEXP_FSERR_EXISTS    = -3,
    PSPEXP_FSERR_NOT_EMPTY = -4,   /* REMOVE of a folder with something in it */
    PSPEXP_FSERR_PATH      = -5,   /* not on a device the runtime serves, or too long */
    PSPEXP_FSERR_FULL      = -6,
    PSPEXP_FSERR_UNSUPPORTED = -7
};

/* ---- Texture helpers (pspexp_texture.c) ------------------------------- */

/* Bits per pixel for the uncompressed formats; 0 for DXT. */
uint32_t pspexp_texture_bits_per_pixel(uint8_t format);
/* Byte length of width x height pixels in `format`, including DXT. 0 if invalid. */
uint32_t pspexp_texture_data_length(uint16_t width, uint16_t height, uint8_t format);
/* Total RESOURCE_UPLOAD_TEXTURE payload length for the given data and frame table. */
uint32_t pspexp_texture_payload_length(uint32_t data_length, uint16_t frame_count);
/* Validates an upload against the constraints of section 3.2 and the
 * executor's supported format mask. Returns PSPEXP_RERR_NONE or a reason. */
int pspexp_texture_validate(const PspExpTextureUpload *t, uint16_t supported_formats, uint32_t payload_length);
/* Pointer to the frame table inside an upload payload (payload points just
 * after the PspExpTextureUpload header). */
const void *pspexp_texture_frames(const PspExpTextureUpload *t, const void *payload);

/* Byte length of a sound's PCM; 0 if the parameters are invalid. */
uint32_t pspexp_sound_data_length(uint8_t channels, uint32_t frames);
int pspexp_sound_validate(const PspExpSoundUpload *s, uint32_t payload_length);

/* Byte length of a palette's entries; 0 if the format or count is invalid. */
uint32_t pspexp_palette_data_length(uint8_t format, uint16_t entries);
/* Validates a palette upload. Returns PSPEXP_RERR_NONE or a reason. */
int pspexp_palette_validate(const PspExpPaletteUpload *p, uint32_t payload_length);

/* PSP texture swizzle (16-byte by 8-row blocks). width_bytes must be a
 * multiple of 16 and height a multiple of 8. in/out must not overlap. */
void pspexp_swizzle(void *out, const void *in, uint32_t width_bytes, uint32_t height);
void pspexp_unswizzle(void *out, const void *in, uint32_t width_bytes, uint32_t height);

/* Pack/unpack RGB565 ("5650") colours. */
static inline uint16_t pspexp_color_5650(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
}
static inline void pspexp_color_unpack_5650(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint32_t rr = c & 0x1Fu, gg = (c >> 5) & 0x3Fu, bb = (c >> 11) & 0x1Fu;
    *r = (uint8_t)((rr << 3) | (rr >> 2));
    *g = (uint8_t)((gg << 2) | (gg >> 4));
    *b = (uint8_t)((bb << 3) | (bb >> 2));
}
#define PSPEXP_COLOR_NONE 0xFFFFu   /* white tint: no modulation (both 5650 and 4444) */
/* RGBA 4444 for the SPRITE_2D_4444 vertex format: R low nibble .. A high nibble. */
static inline uint16_t pspexp_color_4444(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return (uint16_t)(((a >> 4) << 12) | ((b >> 4) << 8) | ((g >> 4) << 4) | (r >> 4));
}

const char *pspexp_channel_name(uint8_t channel);
const char *pspexp_opcode_name(uint16_t opcode);
const char *pspexp_resource_error_name(uint16_t reason);

#ifdef __cplusplus
}
#endif
#endif /* PSPEXP_PROTOCOL_H */
