/*
 * PSPExp runtime core: the command processor every executor shares.
 *
 * It owns the inbound stream, the resource cache and the outbound message
 * queue, validates every message against the constraints of section 3.2,
 * expands batches with the shared routine of section 5.4 and hands the
 * results to executor-specific operations (GE display list, SDL renderer,
 * or nothing at all). It is plain C99 so the PSP build compiles it as-is.
 *
 * Threading: single-threaded. Feed bytes, then drain output.
 */
#ifndef PSPEXP_RUNTIME_H
#define PSPEXP_RUNTIME_H

#include "../protocol/pspexp_protocol.h"
#include "../protocol/pspexp_opcodes.h"
#include "../protocol/pspexp_batch.h"
#include "../protocol/serialization/pspexp_reader.h"
#include "../protocol/serialization/pspexp_writer.h"
#include "pspexp_cache.h"
#include "pspexp_mixer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PspExpRuntimeOps {
    void *user;
    /* Monotonic microsecond clock (the "PSP clock" of the protocol). */
    uint32_t (*now_us)(void *user);
    /* Create the executor's view of a validated texture. `pixels` points at
     * the upload payload in the layout `e->layout` and is only valid during
     * the call. Return 0 or a PSPEXP_RERR_* reason. */
    int  (*texture_create)(void *user, PspExpCacheEntry *e, const void *pixels);
    void (*texture_destroy)(void *user, PspExpCacheEntry *e);
    /* Optional: palette entries in e->format, e->width entries. */
    int  (*palette_create)(void *user, PspExpCacheEntry *e, const void *entries);
    void (*palette_destroy)(void *user, PspExpCacheEntry *e);
    void (*frame_begin)(void *user, uint32_t frame_seq);
    void (*set_clip)(void *user, int16_t x, int16_t y, int16_t w, int16_t h);
    /* Space for `vertex_count` vertices the runtime will expand a batch into.
     * On the PSP this is the GE vertex ring. NULL drops the batch. */
    PspExpVertex2D *(*vertex_alloc)(void *user, uint32_t vertex_count);
    /* `palette` is the resident palette for an indexed atlas, NULL otherwise.
     * `vertex_format` is a PspExpVertexFormat; the vertices are already expanded
     * (mirrored when the batch asked for it). */
    void (*draw_sprites)(void *user, PspExpCacheEntry *atlas, PspExpCacheEntry *palette,
                         uint16_t vertex_format, const PspExpVertex2D *v, uint32_t sprite_count);
    void (*draw_rect)(void *user, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color, uint8_t alpha);
    void (*draw_line)(void *user, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color, uint8_t alpha);
    /* Optional 3D (PSPEXP_FEATURE_3D, advertised when draw3d is set). The
     * runtime checks every message first. depth_buffer: at each connection,
     * whether the host wants a depth buffer (PSPEXP_ACK_DEPTH). draw3d gets
     * the state's texture (NULL: none, or not resident) and its palette;
     * `vertices` is only valid during the call. */
    void (*depth_buffer)(void *user, int on);
    void (*state3d)(void *user, const PspExp3DState *s);
    void (*matrices3d)(void *user, const PspExp3DMatrices *m);
    void (*lights3d)(void *user, const PspExp3DLights *l);
    void (*draw3d)(void *user, PspExpCacheEntry *tex, PspExpCacheEntry *palette, uint8_t primitive,
                   uint32_t vertex_type, uint32_t count, const void *vertices);
    void (*clear3d)(void *user, const PspExp3DClear *c);
    /* A mesh draw: `vertices` is a vertex mesh's data (kept until released);
     * `indices` NULL, an index mesh's data, or (indices_inline) data only
     * valid during the call. The runtime has checked every index. */
    /* Optional render targets: the texture entry to draw into, NULL for the screen. */
    void (*target3d)(void *user, PspExpCacheEntry *target);
    void (*draw3d_mesh)(void *user, PspExpCacheEntry *tex, PspExpCacheEntry *palette, uint8_t primitive,
                        uint32_t vertex_type, uint32_t count, const void *vertices, const uint16_t *indices, int indices_inline);
    /* Finish and display the frame; return the clock at the vblank it was shown. */
    uint32_t (*present)(void *user);
    /* Optional: GE render time of the frame just presented, microseconds. */
    uint32_t (*frame_ge_us)(void *user);
    /* Optional audio output. audio_start is called once the host asked for
     * audio (HELLO_ACK.audio_rate): the executor starts rendering from the
     * mixer on its own audio thread, calling audio_lock/unlock around every
     * pspexp_mixer_render. The runtime takes the same lock when it changes
     * the mixer. audio_stop is called on disconnect. */
    int  (*audio_start)(void *user, PspExpMixer *mixer, uint32_t rate);
    void (*audio_stop)(void *user);
    void (*audio_lock)(void *user);
    void (*audio_unlock)(void *user);
    /* Optional diagnostics. */
    void (*log)(void *user, const char *msg);
    void (*screenshot)(void *user, uint16_t index);
    /* Optional: the clock (CLOCK_SET): UTC seconds since 1970; the time-zone
     * setting in minutes east and daylight saving (0 / 1). Both or neither;
     * with both, HELLO advertises PSPEXP_FEATURE_CLOCK. */
    int (*clock_get)(void *user, uint64_t *utc, int *tz_minutes, int *dst);
    int (*clock_set)(void *user, uint64_t utc);
    /* optional: the time-zone setting (standard offset, minutes east) and summer time */
    int (*zone_set)(void *user, int tz_minutes, int dst);
    /* Optional: the memory stick for the host (the FILE channel). With
     * fs_list set, HELLO advertises PSPEXP_FEATURE_FILES. Paths arrive
     * checked: "ms0:/..." or "ef0:/...", no ".." parts. Results: >= 0, or a
     * PSPEXP_FSERR_*. */
    int  (*fs_list)(void *user, const char *path, uint32_t first, PspExpFsEntry *out, int max, int *more);
    int  (*fs_stat)(void *user, const char *path, PspExpFsEntry *out);
    int  (*fs_read)(void *user, const char *path, uint64_t offset, void *buf, uint32_t len);
    int  (*fs_write)(void *user, const char *path, uint64_t offset, const void *buf, uint32_t len, uint32_t flags);
    int  (*fs_mkdir)(void *user, const char *path);
    int  (*fs_remove)(void *user, const char *path);
    int  (*fs_rename)(void *user, const char *from, const char *to);
    int  (*fs_info)(void *user, const char *path, uint64_t *free_bytes, uint64_t *total_bytes);
} PspExpRuntimeOps;

typedef struct PspExpRuntimeConfig {
    uint8_t  executor;           /* PspExpExecutorKind */
    uint16_t texture_formats;    /* PSPEXP_TEX_BIT mask this executor renders */
    uint32_t vram_budget;
    uint32_t ram_budget;
    uint16_t max_batch_records;  /* <= PSPEXP_BATCH_MAX_RECORDS */
    uint8_t  input_rate_max;
    uint32_t features;
    uint32_t usb_chunk;
    size_t   in_capacity;        /* 0 = growing heap stream */
    size_t   out_capacity;       /* 0 = growing heap writer */
} PspExpRuntimeConfig;

typedef struct PspExpRuntimeStats {
    uint32_t frames, batches, sprites, tiles, rects, lines;
    uint32_t uploads, releases, resource_errors, protocol_errors, misses, palettes;
    uint32_t sounds, plays, stream_frames;
    uint32_t last_frame_bytes, last_execute_us;
    uint32_t frame_sprites, frame_dropped;   /* this frame: drawn / dropped for lack of vertex space */
    uint32_t draws3d, vertices3d;            /* cumulative */
    uint32_t sprites_dropped;                /* cumulative */
    uint64_t bytes_in, bytes_out;
    uint32_t messages_in, messages_out;
} PspExpRuntimeStats;

typedef struct PspExpRuntime {
    PspExpRuntimeOps    ops;
    PspExpRuntimeConfig cfg;
    PspExpCache         cache;
    PspExpStream        in;
    PspExpWriter        out;
    PspExpRuntimeStats  stats;
    int      connected;          /* HELLO_ACK received */
    int      maintenance;        /* ...marked maintenance: only SYSTEM and FILE are served */
    int      in_frame;
    uint32_t frame_seq;
    uint32_t frame_start_us;
    uint32_t frame_bytes;
    uint32_t input_seq;
    PspExpHelloAck ack;
    /* benchmark state */
    int      bench_active;
    uint32_t bench_bytes, bench_msgs, bench_start_us;
    uint32_t uplink_block, uplink_remaining;
    uint8_t *fs_buf;                    /* FILE replies: allocated on the first request */
    uint8_t *scratch;            /* expansion scratch when vertex_alloc is NULL */
    size_t   scratch_cap;
    /* 3D: the texture of the last 3D_STATE, resolved once */
    uint16_t tex3d_id;
    int      tex3d_on;
    PspExpCacheEntry *tex3d, *pal3d;
    /* audio */
    PspExpMixer mixer;
    int      audio_active;
    uint32_t audio_status_us;
} PspExpRuntime;

int  pspexp_runtime_init(PspExpRuntime *rt, const PspExpRuntimeOps *ops, const PspExpRuntimeConfig *cfg);
void pspexp_runtime_free(PspExpRuntime *rt);
/* Fills a default configuration for the given executor kind. */
void pspexp_runtime_default_config(PspExpRuntimeConfig *cfg, uint8_t executor);

/* Link (re)established: drop all state and queue HELLO. */
void pspexp_runtime_connect(PspExpRuntime *rt);
/* Link lost: drop all state, queue nothing. */
void pspexp_runtime_disconnect(PspExpRuntime *rt);

/* Host -> PSP bytes. Every complete message is executed before returning.
 * Returns 0, or -1 if the stream is corrupt (drop the link). */
int  pspexp_runtime_feed(PspExpRuntime *rt, const void *data, size_t len);
/* Zero-copy variant: write into the returned space, then commit + process. */
size_t pspexp_runtime_in_writable(PspExpRuntime *rt, void **where);
int    pspexp_runtime_in_commit(PspExpRuntime *rt, size_t len);

/* Queue a controller sample (called at the sampling rate, never blocked by frames). */
void pspexp_runtime_push_input(PspExpRuntime *rt, uint32_t buttons, uint8_t analog_x, uint8_t analog_y);

/* Pending PSP -> host bytes. Send them, then call _out_consume(). Produces
 * pending benchmark uplink blocks one at a time when the queue is empty. */
const void *pspexp_runtime_out_peek(PspExpRuntime *rt, size_t *len);
void        pspexp_runtime_out_consume(PspExpRuntime *rt);

#ifdef __cplusplus
}
#endif
#endif /* PSPEXP_RUNTIME_H */
