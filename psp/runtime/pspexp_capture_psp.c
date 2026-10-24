/*
 * PSPExp Capture inside PSPExp: what the PSP shows and plays, sent back to the board while a
 * receiver is there (CAPTURE_SET), in PSPExp Capture's own packets ("PXCF" frames, "PXCA" sound,
 * "PXCE" note-button events; tools/pspexp-capture), as CAPTURE_DATA messages on the link.
 *
 * The frame: at its end the GE copies it into one of two buffers in RAM (480x272 RGB 5650,
 * before the volume bar and the recording dot: pspexp_ge.c's op_present), up to 30 a second
 * and only when a buffer is free. The sound: each block the audio thread is about to play,
 * copied into a ring with the PSP's time. A thread of its own, below the main loop's priority,
 * sends the ring's packets, then a frame in 32 KB messages: the link's writes block, and the
 * main loop's own messages (PRESENT_DONE, input) go in between, the lock keeping each message
 * whole. A frame's bytes never have others inside them: the receiver reads whole packets.
 * A receiver keeping recordings only (PSPEXP_CAPTURE_RECORDINGS: the last 30 seconds off in the
 * board's settings) gets the frames and the sound only while a ♪+L recording runs: nothing is
 * copied or sent otherwise, the events aside.
 */
#include <pspkernel.h>
#include <pspthreadman.h>
#include <string.h>
#include <malloc.h>
#include "../../protocol/pspexp_protocol.h"
#include "../../protocol/pspexp_opcodes.h"
#include "pspexp_capture_psp.h"

#define FRAME_BYTES (PSPEXP_DISPLAY_WIDTH * PSPEXP_DISPLAY_HEIGHT * 2)
#define PIECE (32 * 1024)
#define RING (64 * 1024)         /* the sound's and the events' packets */
#define FRAME_GAP_US 33000       /* 30 frames a second at most: the clips' rate */

static PspExpGeExecutor *g_ge;
static int (*g_write)(const void *, int);
static void (*g_lock)(void), (*g_unlock)(void);
static volatile int g_on;                  /* PSPEXP_CAPTURE_* */
static volatile int g_rec;                 /* a ♪+L recording under way */
static unsigned char *g_frame[2];          /* (64-byte aligned, taken the first time it is on) */
static volatile int g_state[2];            /* 0 free, 1 the GE copying into it, 2 to send, 3 sending */
static volatile uint32_t g_ftime[2];
static volatile int g_copying = -1;
static uint32_t g_last_frame, g_fseq, g_out_seq;
static unsigned char g_ring[RING] __attribute__((aligned(64)));
static volatile uint32_t g_head, g_tail;   /* bytes put and sent: whole packets only */
static SceUID g_wake = -1;

static void put16(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

/* a packet into the ring, whole or not at all (interrupts off: the audio thread and the main
 * loop both put) */
static void ring_put(const unsigned char *h, const void *body, uint32_t n)
{
    int intr = sceKernelCpuSuspendIntr();
    uint32_t i, head = g_head;
    if (RING - (head - g_tail) >= 32 + n) {
        for (i = 0; i < 32; i++) g_ring[(head + i) % RING] = h[i];
        for (i = 0; i < n; i++) g_ring[(head + 32 + i) % RING] = ((const unsigned char *)body)[i];
        g_head = head + 32 + n;
    }
    sceKernelCpuResumeIntr(intr);
}

/* one CAPTURE_DATA message: a's bytes, then b's (the lock held: the main loop's messages wait) */
static int send_msg(const void *a, uint32_t an, const void *b, uint32_t bn)
{
    static unsigned char h[32] __attribute__((aligned(64)));
    static const unsigned char zero[16];
    PspExpMessageHeader *mh = (PspExpMessageHeader *)h;
    uint32_t n = an + bn, pad = (16 - n % 16) % 16;
    int r;
    memset(h, 0, sizeof(h));
    mh->magic = PSPEXP_MAGIC; mh->version = PSPEXP_PROTOCOL_VERSION; mh->channel = PSPEXP_CH_DEBUG;
    mh->opcode = PSPEXP_OP_CAPTURE_DATA; mh->sequence = g_out_seq++; mh->length = 16 + n + pad;
    put32(h + 16, n);
    g_lock();
    r = g_write(h, 32) < 0 || (an && g_write(a, (int)an) < 0) || (bn && g_write(b, (int)bn) < 0) || (pad && g_write(zero, (int)pad) < 0) ? -1 : 0;
    g_unlock();
    return r;
}

/* the ring's packets so far (up to where it was when looked at: whole packets) */
static int send_ring(void)
{
    uint32_t head = g_head, tail = g_tail;
    while (tail != head && g_on) {
        uint32_t at = tail % RING, n = head - tail;
        if (n > RING - at) n = RING - at;
        if (n > PIECE) n = PIECE;
        if (send_msg(g_ring + at, n, NULL, 0) < 0) return -1;
        tail += n;
        g_tail = tail;
    }
    return 0;
}

/* a frame: its header with the first piece, then the rest */
static int send_frame(int k)
{
    unsigned char h[32];
    uint32_t off;
    memset(h, 0, sizeof(h));
    memcpy(h, "PXCF", 4);
    put16(h + 4, PSPEXP_DISPLAY_WIDTH); put16(h + 6, PSPEXP_DISPLAY_HEIGHT); put16(h + 8, 0);   /* (RGB 5650, the receiver's format 0) */
    put32(h + 12, g_fseq); put32(h + 16, g_fseq); put32(h + 20, g_ftime[k]); put32(h + 24, FRAME_BYTES);
    g_fseq++;
    if (send_msg(h, 32, g_frame[k], PIECE) < 0) return -1;
    for (off = PIECE; off < FRAME_BYTES && g_on; off += PIECE)
        if (send_msg(g_frame[k] + off, FRAME_BYTES - off < PIECE ? FRAME_BYTES - off : PIECE, NULL, 0) < 0) return -1;
    return 0;
}

static int sender(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        SceUInt t = 50000;
        int k;
        sceKernelWaitSema(g_wake, 1, &t);
        if (!g_on) continue;
        if (send_ring() < 0) continue;   /* (the main loop finds the link gone) */
        for (k = 0; k < 2; k++)
            if (g_state[k] == 2) {
                g_state[k] = 3;
                send_frame(k);
                g_state[k] = 0;
                send_ring();
            }
    }
    return 0;
}

/* ---- the hooks ---- */

static int streaming(void) { return g_on == PSPEXP_CAPTURE_ALL || (g_on == PSPEXP_CAPTURE_RECORDINGS && g_rec); }

/* the GE, at a frame's end: the buffer to copy this one into, or NULL (off, too soon, none free) */
static void *cap_target(void *u)
{
    uint32_t now = sceKernelGetSystemTimeLow();
    int k;
    (void)u;
    if (!streaming() || now - g_last_frame < FRAME_GAP_US) return NULL;
    for (k = 0; k < 2; k++)
        if (g_state[k] == 0) { g_state[k] = 1; g_copying = k; return g_frame[k]; }
    return NULL;
}
static void cap_done(void *u, uint32_t t)
{
    int k = g_copying;
    (void)u;
    if (k < 0) return;
    g_ftime[k] = t;
    g_state[k] = 2;
    g_copying = -1;
    g_last_frame = t;
    sceKernelSignalSema(g_wake, 1);
}

static void cap_set(void *u, int on)
{
    (void)u;
    if (on && !g_frame[0]) {   /* (taken once, kept: the sender may still be reading one) */
        g_frame[0] = memalign(64, FRAME_BYTES);
        g_frame[1] = memalign(64, FRAME_BYTES);
        if (!g_frame[0] || !g_frame[1]) { free(g_frame[0]); free(g_frame[1]); g_frame[0] = g_frame[1] = NULL; return; }
    }
    if (on && !g_on) { g_tail = g_head; g_state[0] = g_state[1] = 0; g_last_frame = sceKernelGetSystemTimeLow() - FRAME_GAP_US; }
    g_on = on;
}

void pspexp_capture_psp_audio(const short *pcm, int frames)
{
    unsigned char h[32];
    if (!streaming()) return;
    memset(h, 0, sizeof(h));
    memcpy(h, "PXCA", 4);
    h[4] = 0; h[5] = 0;   /* (one channel: what the PSP plays, mixed) */
    put16(h + 6, 2); put32(h + 8, 44100); put16(h + 12, 0x8000); put16(h + 14, 0x8000);
    put32(h + 16, (uint32_t)frames); put32(h + 20, sceKernelGetSystemTimeLow()); put32(h + 24, (uint32_t)frames * 4);
    ring_put(h, pcm, (uint32_t)frames * 4);
}

void pspexp_capture_psp_event(int type)
{
    unsigned char h[32];
    if (!g_on) return;
    memset(h, 0, sizeof(h));
    memcpy(h, "PXCE", 4);
    put32(h + 4, (uint32_t)type); put32(h + 20, sceKernelGetSystemTimeLow());
    ring_put(h, NULL, 0);
    sceKernelSignalSema(g_wake, 1);
}

/* the frame on screen, now: a recording starting while nothing new is drawn (the launcher may draw
 * nothing for seconds; recordings only, no earlier frame went), copied by the GE as a screenshot
 * is, before the dot goes on it */
void pspexp_capture_psp_grab(void)
{
    int k;
    if (!g_on || !g_frame[0] || g_ge->in_frame || g_ge->keyboard) return;   /* (a frame on its way comes anyway) */
    for (k = 0; k < 2 && g_state[k] != 0; k++) ;
    if (k == 2) return;
    g_state[k] = 1;
    pspexp_ge_read_display(g_ge, (uint16_t *)g_frame[k]);
    g_ftime[k] = g_last_frame = sceKernelGetSystemTimeLow();
    g_state[k] = 2;
    sceKernelSignalSema(g_wake, 1);
}

int pspexp_capture_psp_on(void) { return g_on; }
void pspexp_capture_psp_recording(int rec) { g_rec = rec; }

void pspexp_capture_psp_init(PspExpGeExecutor *ge, int (*write)(const void *, int), void (*lock)(void), void (*unlock)(void))
{
    SceUID th;
    g_ge = ge; g_write = write; g_lock = lock; g_unlock = unlock;
    ge->cap_target = cap_target;
    ge->cap_done = cap_done;
    ge->cap_user = NULL;
    g_wake = sceKernelCreateSema("PSPExpCaptureWake", 0, 0, 1, NULL);
    th = sceKernelCreateThread("PSPExpCapture", sender, 0x30, 0x2000, 0, NULL);   /* (below the main loop and the sound) */
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}

void pspexp_capture_psp_ops(PspExpRuntimeOps *ops)
{
    ops->capture_set = cap_set;
}
