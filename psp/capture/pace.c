/*
 * The frames copied for a clip or a recording, paced by what they cost the game (included by
 * main.c; tests/test_pace.c runs it against games modelled on the PSP's measurements).
 *
 * Copying a frame costs a game frames in some scenes and none in others: Gran Turismo lost none
 * in most of a race at 24 copies a second, but in one heavier stretch fell from 60 fps to 20 with
 * 20 copies, 36 with 12, 51 with 4.5 (about 2 frames a copy); Burnout Legends, its copies 23-25 ms
 * through the kernel's bands, ran its races at 20-22 with 15 copies and 28-30 with 5 (2026-10-25).
 * And a game's own rate changes with its scenes (menus at 60, races at 30, loads), which from the
 * rate alone looks like a cost: guessed from a peak, a race was copied at 5 a second; guessed from
 * whether slowing helped, a load in the window misled it, and probes kept costing Burnout frames.
 * So the game's rate without copies is measured, not guessed: when the game's rate moves 10% from
 * it (either way: a new scene), the copies stop for one second and its frames are counted. Faster
 * without them: they cost it, and they go down a step (30 a second, 15, 10, 5; a step a second
 * while it stays down, just measured). Not faster: that is the game's own rate now. Slowed, they
 * are tried a step faster when a hold ends (10 s, doubled up to 60 s each time that costs the
 * game over 5% and is undone). A clip has a held frame for each measured second, and fewer frames
 * while the game is busy. No formatting here (the kernel's libc has none): what changed is told
 * in `said` and `a`, `b`, for the caller's log (pace_note).
 */

#define PACE_NONE 0xFFFFFFFFu   /* no frame copied (the second measured) */

enum { PACE_SAID_NOTHING, PACE_SAID_COST, PACE_SAID_OWN, PACE_SAID_FURTHER, PACE_SAID_TRYING, PACE_SAID_KEPT, PACE_SAID_UNDONE };

typedef struct Pace {
    unsigned level;       /* 0..3: 30, 15, 10, 5 copies a second */
    unsigned free_rate;   /* the game's frames a second without copies (measured) */
    unsigned with_rate;   /* its rate copying, before a measured second */
    int measuring;        /* this second without copies */
    int probing, probe_left;
    unsigned probe_sum, hold, wait, cool, since_measure;
    int said;             /* what changed this second, with a and b (pace_note) */
    unsigned a, b;
} Pace;

static const unsigned g_pace_gap_us[4] = { 30000, 66666, 100000, 200000 };
static const unsigned g_pace_per_s[4] = { 30, 15, 10, 5 };

static void pace_init(Pace *p)
{
    p->level = 0; p->free_rate = 0; p->with_rate = 0; p->measuring = 0;
    p->probing = 0; p->probe_left = 0; p->probe_sum = 0;
    p->hold = 10; p->wait = 0; p->cool = 0; p->since_measure = 1000;
    p->said = PACE_SAID_NOTHING; p->a = p->b = 0;
}

/* the log's line for what changed (its numbers: copies a second, a, b) */
static const char *pace_note(int said)
{
    switch (said) {
    case PACE_SAID_COST: return "frames copied %u a second: the game at %u fps copying, %u without";
    case PACE_SAID_OWN: return "frames copied %u a second: the game's own pace %u fps (%u copying)";
    case PACE_SAID_FURTHER: return "frames copied %u a second: the game at %u fps of its %u";
    case PACE_SAID_TRYING: return "frames copied %u a second: trying (the game at %u fps of its %u)";
    case PACE_SAID_KEPT: return "frames copied %u a second (the game at %u fps of its %u)";
    case PACE_SAID_UNDONE: return "frames copied %u a second: faster cost the game (%u fps of its %u)";
    }
    return 0;
}

static unsigned pace_say(Pace *p, int said, unsigned a, unsigned b)
{
    p->said = said; p->a = a; p->b = b;
    return g_pace_gap_us[p->level];
}

/* once a second: rate, the game's frames in it; copying, frames were handed on in it (Clips on, a
 * recording). The gap between two copies from now, in microseconds (PACE_NONE: none this second). */
static unsigned pace_second(Pace *p, unsigned rate, int copying)
{
    p->said = PACE_SAID_NOTHING;
    if (p->cool) p->cool--;
    if (p->since_measure < 1000) p->since_measure++;
    if (p->measuring) {   /* the second without copies: its rate is the game's own */
        p->measuring = 0;
        p->free_rate = rate;
        p->since_measure = 0;
        if (p->with_rate * 10 < rate * 9) {
            if (p->level == 3) {   /* (already at the fewest: measured again only later, each time later) */
                p->hold = p->hold * 2 > 60 ? 60 : p->hold * 2;
                p->cool = p->hold;
                return pace_say(p, PACE_SAID_COST, p->with_rate, rate);
            }
            p->level++;
            p->wait = p->hold;
            return pace_say(p, PACE_SAID_COST, p->with_rate, rate);
        }
        return pace_say(p, PACE_SAID_OWN, rate, p->with_rate);
    }
    if (!copying) return g_pace_gap_us[p->level];
    if (p->probing) {   /* a step faster tried: kept, or undone and tried later */
        p->probe_sum += rate;
        if (--p->probe_left > 0) return g_pace_gap_us[p->level];
        p->probing = 0;
        if (p->probe_sum * 100 < p->free_rate * 2 * 95) {
            p->level++;
            p->hold = p->hold * 2 > 60 ? 60 : p->hold * 2;
            p->wait = p->hold;
            return pace_say(p, PACE_SAID_UNDONE, p->probe_sum / 2, p->free_rate);
        }
        p->wait = 3;
        if (!p->level) { p->hold = 10; p->cool = 0; }   /* (back at the full rate: the next scene measured at once) */
        return pace_say(p, PACE_SAID_KEPT, p->probe_sum / 2, p->free_rate);
    }
    if (rate * 10 > p->free_rate * 11 && p->level) {   /* faster than its own rate while slowed: a new scene, the copies
                                                         * not holding it back: that is its rate, and a step faster tried */
        p->free_rate = rate;
        p->level--;
        p->probing = 1; p->probe_left = 2; p->probe_sum = 0;
        return pace_say(p, PACE_SAID_TRYING, rate, p->free_rate);
    }
    if (rate * 10 < p->free_rate * 9 || rate * 10 > p->free_rate * 11) {   /* 10% from its own rate */
        if (rate < p->free_rate && p->level && p->level < 3 && p->since_measure <= 10) {   /* the cost just measured: a step further */
            p->level++;
            p->wait = p->hold;
            return pace_say(p, PACE_SAID_FURTHER, rate, p->free_rate);
        }
        if (!p->cool) {   /* a second without copies, to see */
            p->measuring = 1;
            p->with_rate = rate;
            p->cool = p->level == 3 ? p->hold : 3;
            return PACE_NONE;
        }
        return g_pace_gap_us[p->level];
    }
    if (p->level) {   /* at its pace, slowed: a step faster when the hold ends */
        if (p->wait) p->wait--;
        if (!p->wait) {
            p->level--;
            p->probing = 1; p->probe_left = 2; p->probe_sum = 0;
            return pace_say(p, PACE_SAID_TRYING, rate, p->free_rate);
        }
    }
    return g_pace_gap_us[p->level];
}
