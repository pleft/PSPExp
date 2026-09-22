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
    uint32_t inputs_received;
    uint32_t resource_errors, protocol_errors;
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
const PspExpHello *pspexp_caps(const PspExpContext *ctx);

/* Receive and process pending messages, waiting up to timeout_ms for the
 * first. Returns messages processed (>= 0) or -1 if the transport is gone. */
int pspexp_pump(PspExpContext *ctx, int timeout_ms);

/* Resources. Handles are chosen by the application. */
int pspexp_upload_texture(PspExpContext *ctx, uint16_t id, const PspExpTextureDesc *desc,
                          const void *pixels, uint32_t pixel_bytes,
                          const PspExpFrame *frames, uint16_t frame_count);
/* Palette for indexed textures: `entries` colours (16 or 256) in `format`
 * (5650, 5551, 4444 or 8888), `bytes` = entries x bytes per colour. */
int pspexp_upload_palette(PspExpContext *ctx, uint16_t id, uint8_t format, uint16_t entries,
                          const void *colors, uint32_t bytes);
int pspexp_release(PspExpContext *ctx, uint16_t id);
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
void pspexp_draw_tile_batch(PspExpContext *ctx, uint16_t atlas, int16_t origin_x, int16_t origin_y,
                            uint16_t cell_w, uint16_t cell_h, uint16_t columns,
                            const PspExpTileRec *recs, uint32_t count);
void pspexp_draw_tile_batch_ex(PspExpContext *ctx, uint16_t atlas, int16_t origin_x, int16_t origin_y,
                               uint16_t cell_w, uint16_t cell_h, uint16_t columns,
                               const PspExpTileRec *recs, uint32_t count, uint16_t palette);
void pspexp_draw_rect(PspExpContext *ctx, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void pspexp_fill_rect(PspExpContext *ctx, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color, uint8_t alpha);
void pspexp_draw_line(PspExpContext *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color, uint8_t alpha);
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
 * old one and keeps resources for replay. Follow with pspexp_connect(). */
void pspexp_set_transport(PspExpContext *ctx, PspExpTransport *transport);

/* Raw access for tools and tests. */
int pspexp_send_raw(PspExpContext *ctx, uint8_t channel, uint16_t opcode, const void *payload, size_t len);
PspExpTransport *pspexp_transport(PspExpContext *ctx);

#ifdef __cplusplus
}
#endif
#endif /* PSPEXP_H */
