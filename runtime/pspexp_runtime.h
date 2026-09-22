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
    void (*frame_begin)(void *user, uint32_t frame_seq);
    void (*set_clip)(void *user, int16_t x, int16_t y, int16_t w, int16_t h);
    /* Space for `vertex_count` vertices the runtime will expand a batch into.
     * On the PSP this is the GE vertex ring. NULL drops the batch. */
    PspExpVertex2D *(*vertex_alloc)(void *user, uint32_t vertex_count);
    void (*draw_sprites)(void *user, PspExpCacheEntry *atlas, const PspExpVertex2D *v, uint32_t sprite_count);
    void (*draw_rect)(void *user, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    /* Finish and display the frame; return the clock at the vblank it was shown. */
    uint32_t (*present)(void *user);
    /* Optional: GE render time of the frame just presented, microseconds. */
    uint32_t (*frame_ge_us)(void *user);
    /* Optional diagnostics. */
    void (*log)(void *user, const char *msg);
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
    uint32_t frames, batches, sprites, tiles, rects;
    uint32_t uploads, releases, resource_errors, protocol_errors, misses;
    uint32_t last_frame_bytes, last_execute_us;
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
    uint8_t *scratch;            /* expansion scratch when vertex_alloc is NULL */
    size_t   scratch_cap;
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
