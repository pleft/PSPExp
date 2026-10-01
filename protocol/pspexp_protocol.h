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
#define PSPEXP_ALIGN4(n)         (((uint32_t)(n) + 3u) & ~3u)
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
/* Two 16-bit framebuffers, no depth buffer (2D does not need one; a 3D
 * program asks for one with PSPEXP_ACK_DEPTH, and textures lose its room). */
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
#define PSPEXP_FEATURE_3D         0x00000010u  /* draws the 3D messages (GRAPHICS 0x0110..0x0114): OpenGL
                                                  programs. A host must not send them otherwise */
#define PSPEXP_FEATURE_3D_INDEXED 0x00000080u  /* 3D_DRAW with indices after its vertices */
#define PSPEXP_FEATURE_3D16       0x00000040u  /* 3D vertices with 16-bit coordinates and positions
                                                  (PSPEXP_3D_VT_TEXTURE16, _POSITION16) */
#define PSPEXP_FEATURE_3D_MORPH   0x00000100u  /* 3D_MORPH and vertices with 8-bit normals and morph
                                                  targets (PSPEXP_3D_VT_NORMAL8, _MORPH) */
#define PSPEXP_FEATURE_PATCH      0x00000020u  /* answers RESOURCE_PATCH: part of a resident texture replaced.
                                                  A host must not send it otherwise (older runtimes drop it) */

/* Host -> PSP (CLOCK_SET): the board's clock, for a PSP whose own one is
 * wrong (a flat clock battery puts it back years). The PSP keeps UTC; what
 * it shows is UTC plus its own time-zone setting, which is not changed. */
typedef struct PspExpClockSet {
    uint32_t utc_lo, utc_hi;    /* seconds since 1970, UTC */
    int32_t  tz_minutes;        /* the board's standard offset from UTC (summer time not included), east positive */
    uint32_t min_diff;          /* set only when the PSP is further off (seconds); 0: always; PSPEXP_CLOCK_REPORT_ONLY: never */
    int32_t  dst;               /* the board is on summer time (1) */
    uint32_t flags;             /* PSPEXP_CLOCK_ZONE: also the PSP's time zone and summer time, when they differ */
    uint32_t reserved[2];
} PspExpClockSet;               /* the first 16 bytes are the whole message of the first runtimes with CLOCK */
#define PSPEXP_CLOCK_REPORT_ONLY 0xffffffffu
#define PSPEXP_CLOCK_ZONE        0x1u

/* PSP -> host (CLOCK_STATUS). */
typedef struct PspExpClockStatus {
    uint32_t before_lo, before_hi;   /* the PSP's UTC clock before, seconds since 1970 */
    uint32_t after_lo, after_hi;     /* and after */
    int32_t  tz_minutes;             /* the PSP's time-zone setting, east positive */
    int32_t  dst;                    /* its daylight-saving setting: 1 adds an hour */
    int32_t  result;                 /* 1 set, 0 left as it was, < 0 could not */
    int32_t  zone_result;            /* the time zone: 1 set, 0 left, < 0 could not (0 from runtimes without it) */
} PspExpClockStatus;

/* Host -> PSP, reply to HELLO. */
typedef struct PspExpHelloAck {
    uint16_t protocol;
    uint16_t input_rate;        /* Hz, <= HELLO.input_rate_max */
    uint32_t audio_rate;        /* 0 = no audio stream */
    uint8_t  audio_channels;
    uint8_t  flags;             /* PSPEXP_ACK_*; 0 in hosts before maintenance mode */
    uint8_t  reserved[6];
} PspExpHelloAck;
/* maintenance: the host speaks another protocol version; only SYSTEM and FILE
 * are used (docs/Package_Manager.md §6.1: to install a matching runtime) */
#define PSPEXP_ACK_MAINTENANCE 0x01
#define PSPEXP_ACK_DEPTH       0x02   /* keep a depth buffer (3D): VRAM for textures shrinks by one framebuffer */
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

/* ---- 3D (PSPEXP_FEATURE_3D) ---------------------------------------------
 *
 * OpenGL 1.x fixed-function programs, translated on the host into the GE's
 * own terms: the fields below hold GE values (the GU_* constants of
 * pspgu.h, noted beside each), so the PSP passes them on unchanged. The PSP
 * transforms and lights the vertices; the host keeps GL's state and sends
 * it when it changed. Every 3D message may come in any order within a frame;
 * 2D draws may be mixed in (the executor restores its 2D state for them).
 */

enum PspExp3DEnable {
    PSPEXP_3D_DEPTH_TEST  = 0x0001,
    PSPEXP_3D_DEPTH_WRITE = 0x0002,
    PSPEXP_3D_CULL        = 0x0004,
    PSPEXP_3D_BLEND       = 0x0008,
    PSPEXP_3D_ALPHA_TEST  = 0x0010,
    PSPEXP_3D_TEXTURE     = 0x0020,
    PSPEXP_3D_FOG         = 0x0040,
    PSPEXP_3D_LIGHTING    = 0x0080,
    PSPEXP_3D_SMOOTH      = 0x0100,   /* Gouraud shading, else flat */
    PSPEXP_3D_DITHER      = 0x0200
};

/* 3D_STATE: the render state for the draws that follow. */
typedef struct PspExp3DState {
    uint32_t enable;            /* PspExp3DEnable bits */
    uint8_t  depth_func;        /* GU_NEVER..GU_GEQUAL (the GE's depth runs 65535 near .. 0 far) */
    uint8_t  alpha_func;        /* GU_NEVER..GU_GEQUAL */
    uint8_t  alpha_ref;
    uint8_t  front_face;        /* GU_CW / GU_CCW: the winding that is kept when culling */
    uint8_t  blend_op;          /* GU_ADD .. GU_ABS */
    uint8_t  blend_src;         /* GU_SRC_COLOR .. GU_FIX */
    uint8_t  blend_dst;
    uint8_t  tex_func;          /* GU_TFX_MODULATE .. GU_TFX_ADD */
    uint8_t  tex_alpha;         /* GU_TCC_RGB / GU_TCC_RGBA */
    uint8_t  tex_min, tex_mag;  /* GU_NEAREST / GU_LINEAR */
    uint8_t  tex_wrap_s, tex_wrap_t;   /* GU_REPEAT / GU_CLAMP */
    uint8_t  reserved0;
    uint16_t texture;           /* texture handle (with PSPEXP_3D_TEXTURE) */
    uint32_t blend_fix_src;     /* 0xBBGGRR for GU_FIX */
    uint32_t blend_fix_dst;
    uint32_t tex_env_color;     /* 0xBBGGRR, GU_TFX_BLEND */
    uint32_t fog_color;         /* 0xBBGGRR */
    float    fog_near, fog_far; /* eye-space distances */
    float    tex_scale_u, tex_scale_v, tex_offset_u, tex_offset_v;
    int16_t  viewport_x, viewport_y;   /* centre on the PSP's screen, pixels (y down) */
    int16_t  viewport_w, viewport_h;   /* size; the GL y flip is the GE's own */
    uint16_t depth_near, depth_far;    /* the GE's depth range, 0..65535 */
    int16_t  scissor_x, scissor_y, scissor_w, scissor_h;
    uint32_t pixel_mask;        /* 0xAABBGGRR bits NOT written (glColorMask) */
    uint8_t  tex_map;           /* GU_TEXTURE_COORDS; GU_TEXTURE_MATRIX: the coordinates are the texture
                                   matrix (3D_MATRICES) times tex_source; GU_ENVIRONMENT_MAP: (1 + n.L) / 2 of
                                   the eye-space normal and two lights' directions (tex_env_lights) */
    uint8_t  tex_source;        /* GU_POSITION, GU_UV, GU_NORMALIZED_NORMAL, GU_NORMAL */
    uint8_t  tex_env_lights;    /* the two lights, as sceGuTexMapMode takes them: first | second << 4. The
                                   GE makes v from the first and u from the second (PSP-1000 and PPSSPP alike) */
    uint8_t  reserved[9];
} PspExp3DState;
PSPEXP_STATIC_ASSERT(sizeof(PspExp3DState) == 96, state3d_size);

/* 3D_MATRICES: OpenGL's matrices, column-major as glLoadMatrixf takes them.
 * The GE's world matrix is the modelview (its last row taken as 0 0 0 1),
 * its view matrix the identity, so lighting happens in eye space. The
 * texture matrix counts with 3D_STATE.tex_map GU_TEXTURE_MATRIX: its rows
 * give u, v and q (divided by). */
typedef struct PspExp3DMatrices {
    float projection[16];
    float modelview[16];
    float texture[16];
} PspExp3DMatrices;
PSPEXP_STATIC_ASSERT(sizeof(PspExp3DMatrices) == 192, matrices3d_size);

#define PSPEXP_3D_LIGHTS 4      /* the GE's light count */

typedef struct PspExp3DLight {
    uint8_t  enabled;
    uint8_t  type;              /* GU_DIRECTIONAL, GU_POINTLIGHT, GU_SPOTLIGHT */
    uint8_t  components;        /* GU_AMBIENT_AND_DIFFUSE, GU_DIFFUSE_AND_SPECULAR */
    uint8_t  reserved0;
    uint32_t ambient, diffuse, specular;   /* 0xBBGGRR */
    float    position[3];       /* eye space; the direction towards it when directional */
    float    direction[3];      /* spot direction, eye space */
    float    attenuation[3];    /* constant, linear, quadratic */
    float    spot_exponent, spot_cutoff;   /* cutoff: the cosine */
    uint8_t  reserved[4];
} PspExp3DLight;
PSPEXP_STATIC_ASSERT(sizeof(PspExp3DLight) == 64, light3d_size);

/* 3D_LIGHTS: the lights and the material. A light that is not enabled
 * still sets its position (an environment map's direction). */
typedef struct PspExp3DLights {
    uint32_t scene_ambient;     /* 0xAABBGGRR, glLightModel */
    uint32_t emissive, ambient, diffuse, specular;   /* the material, 0xAABBGGRR */
    float    shininess;
    uint32_t color_material;    /* GU_AMBIENT | GU_DIFFUSE | GU_SPECULAR: taken from the vertex colour */
    uint8_t  reserved[4];
    PspExp3DLight light[PSPEXP_3D_LIGHTS];
} PspExp3DLights;
PSPEXP_STATIC_ASSERT(sizeof(PspExp3DLights) == 288, lights3d_size);

/* 3D_DRAW: followed by `count` vertices of the GE vertex type `vertex_type`
 * (GU_TRANSFORM_3D), each laid out texture, colour, normal, position:
 *   texture  none or GU_TEXTURE_32BITF (u, v normalised to the texture)
 *   colour   none or GU_COLOR_8888
 *   normal   none or GU_NORMAL_32BITF
 *   position GU_VERTEX_32BITF
 * Nothing else is accepted (no weights or morphs). With PSPEXP_FEATURE_3D_INDEXED, index_count > 0:
 * the vertices (padded to 4 bytes) are followed by that many 16-bit indices, each below `count`, and
 * the primitive is drawn over the indices (a mesh of shared vertices sent once each). */
typedef struct PspExp3DDraw {
    uint8_t  primitive;         /* GU_POINTS .. GU_TRIANGLE_FAN (not GU_SPRITES) */
    uint8_t  reserved0[3];
    uint32_t vertex_type;
    uint32_t count;
    uint32_t index_count;       /* 0: the vertices in order */
} PspExp3DDraw;
PSPEXP_STATIC_ASSERT(sizeof(PspExp3DDraw) == 16, draw3d_size);

#define PSPEXP_3D_VT_TEXTURE   (3u << 0)    /* GU_TEXTURE_32BITF */
#define PSPEXP_3D_VT_COLOR     (7u << 2)    /* GU_COLOR_8888 */
#define PSPEXP_3D_VT_NORMAL    (3u << 5)    /* GU_NORMAL_32BITF */
#define PSPEXP_3D_VT_POSITION  (3u << 7)    /* GU_VERTEX_32BITF */
/* With PSPEXP_FEATURE_3D16: 16-bit texture coordinates and positions, as the GE reads them when it
 * transforms: coordinates unsigned, over 32768 (0 .. 2), positions signed, over 32768 (-1 .. 1); the
 * host scales the positions and the matrices undo it. A third less than floats. */
#define PSPEXP_3D_VT_TEXTURE16  (2u << 0)   /* GU_TEXTURE_16BIT */
#define PSPEXP_3D_VT_POSITION16 (2u << 7)   /* GU_VERTEX_16BIT */
/* With PSPEXP_FEATURE_3D_MORPH: normals as signed bytes (over 127), and vertices made of 2 to 8 morph
 * targets one after another, each in the layout above, mixed by the weights of the last 3D_MORPH
 * (GU_VERTICES(n): an animation's two poses, blended on the GE; Quake's models). */
#define PSPEXP_3D_VT_NORMAL8    (1u << 5)   /* GU_NORMAL_8BIT */
#define PSPEXP_3D_VT_MORPH(n)   ((((uint32_t)(n)) - 1u) << 18)   /* GU_VERTICES(n) */
#define PSPEXP_3D_VT_MORPH_MASK (7u << 18)
#define PSPEXP_3D_PRIM_MAX     5u           /* GU_TRIANGLE_FAN */
/* A vertex's size for a vertex type (the GE's layout: each part aligned to its own size, the whole to the
 * largest); 0 when the type is not accepted. */
static inline uint32_t pspexp_3d_vertex_size(uint32_t vt)
{
    uint32_t t = vt & 3u, c = (vt >> 2) & 7u, nm = (vt >> 5) & 3u, p = (vt >> 7) & 3u, n = 0, al = 1;
    uint32_t morphs = ((vt & PSPEXP_3D_VT_MORPH_MASK) >> 18) + 1u;
    if (vt & ~(PSPEXP_3D_VT_TEXTURE | PSPEXP_3D_VT_COLOR | PSPEXP_3D_VT_NORMAL | PSPEXP_3D_VT_POSITION | PSPEXP_3D_VT_MORPH_MASK)) return 0;
    if ((p != 3 && p != 2) || (t != 0 && t != 3 && t != 2) || (c != 0 && c != 7) || (nm != 0 && nm != 3 && nm != 1)) return 0;
    if (t) { al = t == 3 ? 4 : 2; n = t == 3 ? 8 : 4; }
    if (c) { n = (n + 3u) & ~3u; n += 4; al = 4; }
    if (nm == 1) n += 3;
    else if (nm) { n = (n + 3u) & ~3u; n += 12; al = 4; }
    if (p == 3) { n = (n + 3u) & ~3u; n += 12; al = 4; }
    else { n = (n + 1u) & ~1u; n += 6; if (al < 2) al = 2; }
    return ((n + al - 1u) & ~(al - 1u)) * morphs;
}
#define PSPEXP_3D_MAX_VERTICES 65535u       /* the GE's count field */

/* 3D_CLEAR: clears the colour and/or depth buffer within the state's scissor
 * (glClear). */
#define PSPEXP_3D_CLEAR_COLOR 0x1u
#define PSPEXP_3D_CLEAR_DEPTH 0x4u          /* the GE's own bits */
typedef struct PspExp3DClear {
    uint32_t flags;
    uint32_t color;             /* 0xAABBGGRR */
    uint16_t depth;             /* the GE's value: 0 is the far end */
    uint8_t  reserved[6];
} PspExp3DClear;
PSPEXP_STATIC_ASSERT(sizeof(PspExp3DClear) == 16, clear3d_size);

/* 3D_MORPH (PSPEXP_FEATURE_3D_MORPH): the weights of the morph targets of the draws that follow
 * (sceGuMorphWeight), each target's position, normal, colour and coordinates times its weight. */
typedef struct PspExp3DMorph {
    float weight[8];
} PspExp3DMorph;
PSPEXP_STATIC_ASSERT(sizeof(PspExp3DMorph) == 32, morph3d_size);

/* Meshes: vertices (a 3D vertex type, as in 3D_DRAW) or 16-bit indices,
 * uploaded once (RESOURCE_UPLOAD_MESH, kept in main RAM) and drawn by
 * 3D_DRAW_MESH as often as wanted: static geometry (OpenGL buffer objects)
 * crosses the link once. */
enum PspExpMeshKind {
    PSPEXP_MESH_VERTICES  = 0,
    PSPEXP_MESH_INDICES16 = 1
};
typedef struct PspExpMeshUpload {
    uint16_t id;
    uint8_t  kind;              /* PspExpMeshKind */
    uint8_t  reserved0;
    uint32_t vertex_type;       /* vertices: the 3D vertex type */
    uint32_t count;             /* vertices, or indices */
    uint32_t data_length;       /* count x the vertex size, or count x 2 */
} PspExpMeshUpload;
PSPEXP_STATIC_ASSERT(sizeof(PspExpMeshUpload) == 16, mesh_upload_size);

#define PSPEXP_MESH_NONE   0xFFFFu  /* 3D_DRAW_MESH.indices: vertices first.. first+count-1 in order */
#define PSPEXP_MESH_INLINE 0xFFFEu  /* 3D_DRAW_MESH.indices: `count` 16-bit indices follow the header */

/* 3D_DRAW_MESH: `count` vertices of a vertex mesh, in order from `first`,
 * or through an index mesh (its indices first.. first+count-1), or through
 * indices that follow. The render state, matrices and lights are 3D_DRAW's. */
typedef struct PspExp3DDrawMesh {
    uint16_t vertices;          /* vertex mesh handle */
    uint16_t indices;           /* index mesh handle, PSPEXP_MESH_NONE or PSPEXP_MESH_INLINE */
    uint8_t  primitive;         /* GU_POINTS .. GU_TRIANGLE_FAN */
    uint8_t  reserved[3];
    uint32_t first;
    uint32_t count;
} PspExp3DDrawMesh;
PSPEXP_STATIC_ASSERT(sizeof(PspExp3DDrawMesh) == 16, draw_mesh_size);

/* Render targets: textures in VRAM the GE draws into (OpenGL framebuffer
 * objects). RESOURCE_CREATE_TARGET makes one (cleared to zero; a texture
 * like any other for draws that sample it); 3D_TARGET, in a frame, sends
 * the draws and clears that follow into it, or back to the screen. Row 0
 * is the texture's first row (v = 0). */
typedef struct PspExpTargetCreate {
    uint16_t id;
    uint16_t width, height;     /* powers of two, at most 512 */
    uint8_t  format;            /* PSPEXP_TEX_5650, 5551, 4444 or 8888 */
    uint8_t  reserved[9];
} PspExpTargetCreate;
PSPEXP_STATIC_ASSERT(sizeof(PspExpTargetCreate) == 16, target_create_size);

#define PSPEXP_TARGET_SCREEN 0xFFFFu

/* RESOURCE_PATCH (PSPEXP_FEATURE_PATCH): bytes of a resident texture (not a
 * render target) replaced, at `offset` in its data as uploaded (its layout:
 * a swizzled texture's 8-row strips are whole runs of bytes). The draws of
 * the frame see the new bytes; `length` bytes of data follow. An OpenGL
 * program's glTexSubImage2D (Quake's lightmaps, a few rows a frame) instead
 * of the whole texture again. */
typedef struct PspExpResourcePatch {
    uint16_t id;
    uint16_t reserved;
    uint32_t offset;
    uint32_t length;
} PspExpResourcePatch;
PSPEXP_STATIC_ASSERT(sizeof(PspExpResourcePatch) == 12, resource_patch_size);
typedef struct PspExp3DTarget {
    uint16_t id;                /* a render target, or PSPEXP_TARGET_SCREEN */
    uint8_t  reserved[14];
} PspExp3DTarget;
PSPEXP_STATIC_ASSERT(sizeof(PspExp3DTarget) == 16, target3d_size);

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
