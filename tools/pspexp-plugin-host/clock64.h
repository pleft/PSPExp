/*
 * The PSP's clock, for the receiver (main.c): 32 bits of microseconds, wrapping every 71.6
 * minutes. A difference cast to 32 bits is right only within 35.8 minutes (2^31 us): until
 * 2026-10-29 a clip's time was that cast from the session's start, and 35.8 minutes in every
 * later recording and clip came out negative and was dropped (a session runs as long as PSPExp
 * does). Here each time is taken from the latest one seen, which is never that far back (an
 * event is stamped at its press, a little earlier than what came before it): 64 bits that go on.
 * Tested in tests/test_clock64.c.
 */
#ifndef PSPEXP_PLUGIN_HOST_CLOCK64_H
#define PSPEXP_PLUGIN_HOST_CLOCK64_H

#include <stdint.h>

typedef struct { uint32_t last; int64_t ext; int have; } Clock64;

static inline int64_t clock64(Clock64 *c, uint32_t when)
{
    int32_t d;
    if (!c->have) { c->have = 1; c->last = when; c->ext = when; return c->ext; }
    d = (int32_t)(when - c->last);
    if (d > 0) { c->last = when; c->ext += d; return c->ext; }
    return c->ext + d;
}

#endif
