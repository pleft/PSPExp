/*
 * The receiver's PSP clock (tools/pspexp-plugin-host/clock64.h): the PSP stamps its capture
 * packets with 32 bits of microseconds. A session of PSPExp lasts hours, so the times must go on
 * past 35.8 minutes (where a 32-bit difference turned negative: recordings and clips dropped,
 * 2026-10-29) and past the clock's wrap at 71.6 minutes; an event stamped at its press, a little
 * before the packets around it, keeps its place.
 */
#include <stdio.h>
#include "../tools/pspexp-plugin-host/clock64.h"

static int g_fail;
static void check(int ok, const char *what) { printf("  %s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) g_fail = 1; }

int main(void)
{
    Clock64 c = { 0, 0, 0 };
    const uint32_t start = 0xF0000000u;   /* a session starting 4.5 minutes before the wrap */
    int64_t base = clock64(&c, start), t = 0;
    uint64_t us;
    int monotonic = 1;
    /* three hours of packets, one every 33 ms, an event 350 ms back each minute */
    for (us = 33333; us <= 3ull * 3600 * 1000000; us += 33333) {
        int64_t now = clock64(&c, (uint32_t)(start + us)) - base;
        if (now != (int64_t)us) { monotonic = 0; break; }
        if (us % 60000000u < 33333) {
            int64_t ev = clock64(&c, (uint32_t)(start + us - 350000)) - base;
            if (ev != (int64_t)us - 350000) { monotonic = 0; break; }
        }
        t = now;
    }
    check(monotonic, "every packet's time exact for three hours, across two wraps of the PSP's clock");
    check(t > 2147483647LL, "past 35.8 minutes (2^31 us) the time stays positive");
    {   /* the case that broke: a recording started 2129 s in, stopped 21 s later */
        Clock64 d = { 0, 0, 0 };
        int64_t b = clock64(&d, 1000), a, z;
        a = clock64(&d, 1000 + 2129000000u) - b;
        z = clock64(&d, 1000 + 2150000000u) - b;
        check(a == 2129000000LL && z == 2150000000LL && z > a, "a recording from 2129 s to 2150 s: 21 s long, not negative");
    }
    {   /* a packet a little older than the latest does not move the clock back */
        Clock64 e = { 0, 0, 0 };
        int64_t b = clock64(&e, 5000000), x = clock64(&e, 6000000) - b, y = clock64(&e, 5600000) - b, w = clock64(&e, 6100000) - b;
        check(x == 1000000 && y == 600000 && w == 1100000, "an earlier-stamped event is placed before, the clock goes on after it");
    }
    printf(g_fail ? "clock64: FAILED\n" : "clock64: all passed\n");
    return g_fail;
}
