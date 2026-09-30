/*
 * libPSPExp: the host application API (sections 5, 6, 10).
 *
 * The application owns all state. It uploads resources by host-assigned
 * handle, builds a frame with draw calls, presents it, and paces itself on
 * PRESENT_DONE. It never needs to know whether the command stream goes to a
 * local executor or over USB to a PSP.
 */
#ifndef PSPEXP_H
#define PSPEXP_H

#include "../../protocol/pspexp_protocol.h"
#include "../../protocol/pspexp_opcodes.h"
#include "../../protocol/pspexp_batch.h"
#include "../../protocol/serialization/pspexp_reader.h"
#include "pspexp_transport.h"
#include "pspexp_time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PspExpContext PspExpContext;

typedef struct PspExpConfig {
    uint16_t input_rate;        /* requested INPUT rate, Hz (default 60) */
    uint32_t audio_rate;        /* 0 = none */
    uint8_t  audio_channels;
    uint16_t max_batch_records; /* batch split size; clamped to the runtime's */
    int      max_in_flight;     /* frames sent but not presented before should_render() says no (default 1) */
    int      retain_uploads;    /* keep copies of uploads to replay after reconnect (default 1) */
    int      depth_buffer;      /* ask for a depth buffer (3D programs; default 0) */
} PspExpConfig;

typedef enum PspExpLinkState {
    PSPEXP_LINK_DISCONNECTED = 0,   /* no HELLO yet, or transport lost */
    PSPEXP_LINK_CONNECTED    = 1    /* HELLO seen and HELLO_ACK sent */
} PspExpLinkState;

typedef enum PspExpResourceState {
    PSPEXP_RES_NONE = 0,
    PSPEXP_RES_PENDING,             /* uploaded, no RESOURCE_READY yet */
    PSPEXP_RES_RESIDENT,
    PSPEXP_RES_FAILED
} PspExpResourceState;

typedef struct PspExpStats {
    uint32_t frames_sent, frames_presented, frames_dropped_remote;
    uint32_t reconnects;
    uint64_t bytes_sent, bytes_received;
    uint32_t last_frame_bytes, last_frame_batches, last_frame_sprites;
    uint32_t last_present_latency_us;   /* host clock: present() to PRESENT_DONE */
    uint32_t last_input_to_present_us;  /* PSP clock: INPUT.timestamp -> PRESENT_DONE.timestamp */
    uint32_t last_execute_us;           /* PSP-side CPU time for the last frame */
    uint32_t last_ge_us;                /* PSP-side GE render time for the last frame */
    uint32_t last_psp_sprites;          /* PSP-side: sprites that reached the GE (0 from older runtimes) */
    uint32_t last_psp_dropped;          /* PSP-side: sprites dropped for lack of vertex space */
    uint32_t inputs_received;
    uint32_t resource_errors, protocol_errors;
    uint32_t audio_frames_sent;
} PspExpStats;

typedef struct PspExpTextureDesc {
    uint16_t width, height;
    uint8_t  format;    /* PspExpTextureFormat */
    uint8_t  layout;    /* PspExpTextureLayout */
    uint8_t  priority;  /* PspExpPriority */
    uint16_t palette;   /* palette handle for T4/T8 (upload it first) */
} PspExpTextureDesc;

/* Called for every message received; return 1 to stop default handling. */
typedef int (*PspExpMessageHook)(void *user, PspExpContext *ctx, const PspExpMessage *m);
typedef void (*PspExpLogFn)(void *user, const char *msg);

void pspexp_config_default(PspExpConfig *cfg);
PspExpContext *pspexp_create(PspExpTransport *transport, const PspExpConfig *cfg);
void pspexp_destroy(PspExpContext *ctx);
void pspexp_set_log(PspExpContext *ctx, PspExpLogFn fn, void *user);
void pspexp_set_message_hook(PspExpContext *ctx, PspExpMessageHook fn, void *user);

/* Wait for HELLO and answer it. 0 on success, -1 on timeout/transport loss. */
int pspexp_connect(PspExpContext *ctx, int timeout_ms);
PspExpLinkState pspexp_link_state(const PspExpContext *ctx);
/* connected for maintenance only: the PSP's runtime speaks another protocol
 * version; the file channel works (to install a matching runtime), nothing is
 * drawn (pspexp_should_render says no) */
int pspexp_link_maintenance(const PspExpContext *ctx);
const PspExpHello *pspexp_caps(const PspExpContext *ctx);

/* Receive and process pending messages, waiting up to timeout_ms for the
 * first. Returns messages processed (>= 0) or -1 if the transport is gone. */
int pspexp_pump(PspExpContext *ctx, int timeout_ms);

/* Resources. Handles are chosen by the application. */
int pspexp_upload_texture(PspExpContext *ctx, uint16_t id, const PspExpTextureDesc *desc,
                          const void *pixels, uint32_t pixel_bytes,
                          const PspExpFrame *frames, uint16_t frame_count);
/* Two-step upload for callers that produce the pixels themselves: begin
 * returns where to write pixel_bytes (inside the outgoing message, so no
 * copy), commit sends it. retain = 0: no replay copy kept for a reconnect
 * (for textures re-uploaded every frame anyway). NULL / < 0 on error. */
/* Part of a resident texture replaced (PSPEXP_FEATURE_PATCH): `length` bytes at `offset` in its data
 * as uploaded, written into the returned space, then _commit. NULL: the PSP cannot (upload it whole). */
int pspexp_patch_supported(const PspExpContext *ctx);
uint8_t *pspexp_patch_texture_begin(PspExpContext *ctx, uint16_t id, uint32_t offset, uint32_t length);
int pspexp_patch_texture_commit(PspExpContext *ctx);
uint8_t *pspexp_upload_texture_begin(PspExpContext *ctx, uint16_t id, const PspExpTextureDesc *desc,
                                     uint32_t pixel_bytes, const PspExpFrame *frames, uint16_t frame_count);
int pspexp_upload_texture_commit(PspExpContext *ctx, int retain);
/* Palette for indexed textures: `entries` colours (16 or 256) in `format`
 * (5650, 5551, 4444 or 8888), `bytes` = entries x bytes per colour. */
int pspexp_upload_palette(PspExpContext *ctx, uint16_t id, uint8_t format, uint16_t entries,
                          const void *colors, uint32_t bytes);
/* Cached sound effect (section 8): s16 PCM, `channels` 1 or 2, already at
 * the negotiated output rate (see pspexp_caps / PspExpConfig.audio_rate). */
int pspexp_upload_sound(PspExpContext *ctx, uint16_t id, const int16_t *pcm, uint32_t frames, uint8_t channels);
int pspexp_play_sound(PspExpContext *ctx, uint16_t id, uint8_t volume, int8_t pan, uint8_t loop);
int pspexp_stop_sound(PspExpContext *ctx, uint16_t id);   /* 0xFFFF = every voice */
/* Streamed audio: s16 stereo frames at the negotiated rate. The runtime
 * starts playing once ~100 ms is buffered and reports AUDIO_STATUS. */
int pspexp_audio_stream(PspExpContext *ctx, const int16_t *stereo, uint32_t frames);
int pspexp_audio_stream_reset(PspExpContext *ctx);
const PspExpAudioStatus *pspexp_audio_status(const PspExpContext *ctx);
int pspexp_release(PspExpContext *ctx, uint16_t id);
/* Gather sends (uploads, palettes, audio, releases) into one buffer from
 * pspexp_batch_begin until pspexp_batch_flush, which sends them as one
 * transfer and stops gathering: links with a high cost per transfer (the
 * GameShell's USB host: 15 page uploads a frame took 8-11 ms) pay it once.
 * Order is kept; a message larger than the buffer goes out on its own. */
void pspexp_batch_begin(PspExpContext *ctx);
int  pspexp_batch_flush(PspExpContext *ctx);
int pspexp_hint(PspExpContext *ctx, uint16_t id, uint8_t priority);
PspExpResourceState pspexp_resource_state(const PspExpContext *ctx, uint16_t id, uint8_t *tier);
int pspexp_memory_query(PspExpContext *ctx);
const PspExpMemoryStatus *pspexp_memory_status(const PspExpContext *ctx);

/* Frames. Draw calls between begin and present are buffered and sent as one block. */
void pspexp_begin_frame(PspExpContext *ctx);
void pspexp_set_clip(PspExpContext *ctx, int16_t x, int16_t y, int16_t w, int16_t h);
void pspexp_draw_sprite_batch(PspExpContext *ctx, uint16_t atlas, const PspExpSpriteRec *recs, uint32_t count);
/* Full form: `flags` = PSPEXP_BATCH_*, `palette` overrides an indexed atlas's
 * palette (PSPEXP_PALETTE_DEFAULT for none), `vertex_format` selects 5650
 * tints or 4444 tints with per-sprite alpha. */
void pspexp_draw_sprite_batch_ex(PspExpContext *ctx, uint16_t atlas, const PspExpSpriteRec *recs, uint32_t count,
                                 uint16_t flags, uint16_t palette, uint16_t vertex_format);
void pspexp_draw_sprite(PspExpContext *ctx, uint16_t atlas, int16_t x, int16_t y, uint16_t frame, uint16_t color);
/* Arbitrary source rectangles (16 bytes per quad; frames are 8). */
void pspexp_draw_quad_batch(PspExpContext *ctx, uint16_t atlas, const PspExpQuadRec *recs, uint32_t count,
                            uint16_t palette, uint16_t vertex_format);
/* Scaled / flipped quads (24 bytes per quad). */
void pspexp_draw_quad_ex_batch(PspExpContext *ctx, uint16_t atlas, const PspExpQuadExRec *recs, uint32_t count,
                               uint16_t palette, uint16_t vertex_format);
void pspexp_draw_tile_batch(PspExpContext *ctx, uint16_t atlas, int16_t origin_x, int16_t origin_y,
                            uint16_t cell_w, uint16_t cell_h, uint16_t columns,
                            const PspExpTileRec *recs, uint32_t count);
void pspexp_draw_tile_batch_ex(PspExpContext *ctx, uint16_t atlas, int16_t origin_x, int16_t origin_y,
                               uint16_t cell_w, uint16_t cell_h, uint16_t columns,
                               const PspExpTileRec *recs, uint32_t count, uint16_t palette);
void pspexp_draw_rect(PspExpContext *ctx, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void pspexp_fill_rect(PspExpContext *ctx, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color, uint8_t alpha);
void pspexp_draw_line(PspExpContext *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color, uint8_t alpha);
/* 3D (OpenGL programs), when the runtime offers it (PSPEXP_FEATURE_3D); see
 * the 3D messages in pspexp_protocol.h. A depth buffer needs
 * PspExpConfig.depth_buffer or pspexp_set_depth_buffer (connected already:
 * it takes effect if no texture is in the PSP's VRAM yet). The draw calls
 * go in a frame. */
int  pspexp_3d_supported(const PspExpContext *ctx);
void pspexp_set_depth_buffer(PspExpContext *ctx, int on);
void pspexp_3d_state(PspExpContext *ctx, const PspExp3DState *s);
void pspexp_3d_matrices(PspExpContext *ctx, const float projection[16], const float modelview[16], const float texture[16]);
void pspexp_3d_lights(PspExpContext *ctx, const PspExp3DLights *l);
void pspexp_3d_clear(PspExpContext *ctx, uint32_t flags, uint32_t color, uint16_t depth);
/* Space for `count` vertices of `vertex_type` (pspexp_3d_vertex_size each)
 * in the frame, valid until the next call on ctx; NULL on error. */
void *pspexp_3d_draw(PspExpContext *ctx, uint8_t primitive, uint32_t vertex_type, uint32_t count);
/* Meshes (resources, like textures): `kind` PSPEXP_MESH_VERTICES (count
 * vertices of vertex_type) or PSPEXP_MESH_INDICES16 (count indices). Two
 * steps: begin returns where to write `count` of them, commit sends it. */
void *pspexp_upload_mesh_begin(PspExpContext *ctx, uint16_t id, uint8_t kind, uint32_t vertex_type, uint32_t count);
int   pspexp_upload_mesh_commit(PspExpContext *ctx);
/* Render targets (resources): a texture in VRAM the GE draws into, and
 * (in a frame) where the draws that follow go: a target, or
 * PSPEXP_TARGET_SCREEN. */
int  pspexp_create_target(PspExpContext *ctx, uint16_t id, uint16_t width, uint16_t height, uint8_t format);
void pspexp_3d_target(PspExpContext *ctx, uint16_t id);
/* Draw from meshes (in a frame): `indices` a mesh handle, PSPEXP_MESH_NONE,
 * or PSPEXP_MESH_INLINE, which returns where to write the `count` indices
 * (else a non-NULL pointer that is not to be written; NULL on error). */
uint16_t *pspexp_3d_draw_mesh(PspExpContext *ctx, uint16_t vertices, uint16_t indices, uint8_t primitive,
                              uint32_t first, uint32_t count);

/* Sends the frame. Returns its frame_seq, or -1 if the transport is gone. */
long pspexp_present(PspExpContext *ctx);

/* Pacing (section 10.2). */
int  pspexp_should_render(const PspExpContext *ctx);
int  pspexp_frames_in_flight(const PspExpContext *ctx);
/* Pump until should_render() is true, or timeout. 0 ok, 1 timeout, -1 transport gone. */
int  pspexp_wait_present(PspExpContext *ctx, int timeout_ms);

/* Newest controller sample (section 7). */
const PspExpInput *pspexp_input(const PspExpContext *ctx);
const PspExpStats *pspexp_stats(const PspExpContext *ctx);

/* Replace the transport after the link was lost (cable pulled): closes the
 * old one and keeps resources for replay. Pass NULL to close it right away
 * while waiting for a device, then the new one. Follow with pspexp_connect(). */
void pspexp_set_transport(PspExpContext *ctx, PspExpTransport *transport);

/* Ask the executor to save the displayed frame as pspexp-shot-<index>.bmp. */
int pspexp_request_screenshot(PspExpContext *ctx, uint16_t index);

/* The PSP's clock (CLOCK_SET), when the runtime offers it (PSPEXP_FEATURE_CLOCK):
 * utc = this computer's clock (seconds since 1970, UTC); tz_minutes its
 * standard offset and dst its summer time. The PSP's clock moves only when it
 * is further off than min_diff seconds (0: always; PSPEXP_CLOCK_REPORT_ONLY:
 * never); with flags PSPEXP_CLOCK_ZONE its time zone and summer time follow
 * when they differ. Waits up to 3 s; 1 set, 0 left, < 0 no answer or could
 * not; out: before, after, the PSP's zone, zone_result. Not in a frame. */
int pspexp_clock_supported(const PspExpContext *ctx);
int pspexp_clock_sync(PspExpContext *ctx, uint64_t utc, int tz_minutes, int dst, uint32_t flags, uint32_t min_diff, PspExpClockStatus *out);

/* The PSP's memory stick (the FILE channel), when the runtime offers it
 * (PSPEXP_FEATURE_FILES). Paths are the PSP's: "ms0:/PSP/GAME". Each call
 * waits for its reply (pumping the link meanwhile, up to 15 s) and returns
 * >= 0 or a PSPEXP_FSERR_* (PSPEXP_FSERR_LINK: no reply). Not in a frame,
 * and from one thread: the one that drives ctx. */
#define PSPEXP_FSERR_LINK (-100)
int pspexp_fs_supported(const PspExpContext *ctx);
/* entries from index first (up to max, PSPEXP_FS_LIST_MAX a reply); *more: some left */
int pspexp_fs_list(PspExpContext *ctx, const char *path, uint32_t first, PspExpFsEntry *out, int max, int *more);
int pspexp_fs_stat(PspExpContext *ctx, const char *path, PspExpFsEntry *out);
/* up to len (PSPEXP_FS_CHUNK at most) bytes at offset; the count read */
int pspexp_fs_read(PspExpContext *ctx, const char *path, uint64_t offset, void *buf, uint32_t len);
/* len (PSPEXP_FS_CHUNK at most) bytes at offset; flags PSPEXP_FS_CREATE | PSPEXP_FS_TRUNCATE */
int pspexp_fs_write(PspExpContext *ctx, const char *path, uint64_t offset, const void *data, uint32_t len, uint32_t flags);
int pspexp_fs_mkdir(PspExpContext *ctx, const char *path);
int pspexp_fs_remove(PspExpContext *ctx, const char *path);   /* a file or an empty folder */
int pspexp_fs_rename(PspExpContext *ctx, const char *from, const char *to);
int pspexp_fs_info(PspExpContext *ctx, const char *path, uint64_t *free_bytes, uint64_t *total_bytes);

/* Raw access for tools and tests. */
int pspexp_send_raw(PspExpContext *ctx, uint8_t channel, uint16_t opcode, const void *payload, size_t len);
PspExpTransport *pspexp_transport(PspExpContext *ctx);

#ifdef __cplusplus
}
#endif
#endif /* PSPEXP_H */
