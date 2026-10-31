/* The note button inside PSPExp (runtime/pspexp_note.c): the pairs, the buttons the board sees,
 * the mute put back, the dot. */
#include "test_util.h"
#include "../runtime/pspexp_note.h"
#include "../protocol/pspexp_protocol.h"

#define NOTE PSPEXP_BUTTON_NOTE
#define R PSPEXP_BUTTON_RTRIGGER
#define L PSPEXP_BUTTON_LTRIGGER
#define X PSPEXP_BUTTON_CROSS
#define VOL PSPEXP_BUTTON_VOLUP
#define MS 1000u

static PspExpNote n;
static PspExpNoteOut o;
static uint32_t t;

/* one reading, 16 ms after the last */
static void step(uint32_t b, int mute, int capture)
{
    t += 16 * MS;
    pspexp_note_step(&n, b, mute, capture, t, &o);
}

/* readings of b until an event comes (a single ♪ + L acts once the double-tap time has passed):
 * the events; 0 if none came within 0.6 s */
static uint32_t until_event(uint32_t b, int capture)
{
    int k;
    for (k = 0; k < 38; k++) { step(b, 0, capture); if (o.events) return o.events; }
    return 0;
}

int main(void)
{
    int i, seen;

    /* no receiver: every button as read, nothing else */
    pspexp_note_init(&n); t = 0xFFFF0000u;   /* (the clock wraps on the way) */
    step(0, 0, 0);
    step(NOTE | R, 1, 0);
    CHECK_EQ(o.buttons, NOTE | R); CHECK_EQ(o.events, 0); CHECK_EQ(o.set_mute, -1); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_NONE);

    /* ♪ alone: the board sees nothing while it is down, then a press of ~120 ms; the system's
     * mute, toggled on the press, is held back and applied on the release */
    pspexp_note_init(&n);
    step(0, 0, 1);
    step(NOTE, 0, 1);
    CHECK_EQ(o.buttons, 0); CHECK_EQ(o.set_mute, -1);
    step(NOTE, 1, 1);                       /* the system muted */
    CHECK_EQ(o.buttons, 0); CHECK_EQ(o.set_mute, 0);
    step(NOTE, 0, 1);
    CHECK_EQ(o.set_mute, -1);
    step(0, 0, 1);                          /* let go: muted now, as it always was */
    CHECK_EQ(o.set_mute, 1); CHECK_EQ(o.buttons, NOTE); CHECK_EQ(o.events, 0);
    for (i = 0, seen = 0; i < 20; i++) { step(0, 1, 1); if (o.buttons & NOTE) seen++; CHECK_EQ(o.set_mute, -1); }
    CHECK(seen >= 5 && seen <= 8);          /* (120 ms of 16 ms readings, the first one counted above) */
    CHECK_EQ(o.buttons, 0);

    /* ♪+R: a clip, R hidden, the green dot for 1.2 s, no ♪ for the board, the mute put back on the
     * press and for half a second after the release */
    pspexp_note_init(&n);
    step(0, 0, 1);
    step(NOTE, 1, 1);
    CHECK_EQ(o.set_mute, 0);
    step(NOTE | R, 0, 1);
    CHECK_EQ(o.events, 1u << PSPEXP_NOTE_CLIP); CHECK_EQ(o.buttons, 0); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_GREEN);
    step(NOTE | R, 0, 1);
    CHECK_EQ(o.events, 0);                  /* once per press */
    step(R, 0, 1);                          /* ♪ let go first: R still hidden until let go */
    CHECK_EQ(o.buttons, 0); CHECK_EQ(o.set_mute, -1);
    step(R, 1, 1);                          /* the system toggles again on the release: put back */
    CHECK_EQ(o.set_mute, 0);
    step(0, 0, 1);
    CHECK_EQ(o.buttons, 0);                 /* no tap after a pair */
    step(R, 0, 1);
    CHECK_EQ(o.buttons, R);                 /* pressed again: the game's */
    for (i = 0; i < 80; i++) step(0, 0, 1);
    step(0, 1, 1);                          /* past the half second: a mute is the person's own */
    CHECK_EQ(o.set_mute, -1);
    CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_NONE);  /* (past 1.2 s) */

    /* ♪+L twice: a recording starts, the red dot, then it stops; the volume buttons pass */
    pspexp_note_init(&n);
    step(0, 0, 1);
    step(NOTE | VOL, 0, 1);
    step(NOTE | L | VOL, 0, 1);
    CHECK_EQ(o.events, 0); CHECK_EQ(o.buttons, VOL);   /* (waiting for a second L) */
    {
        uint32_t pressed = t;
        CHECK_EQ(until_event(NOTE | VOL, 1), 1u << PSPEXP_NOTE_REC_START);
        CHECK_EQ(o.rec_at, pressed);                      /* stamped with the press, not the decision */
        CHECK(t - pressed >= PSPEXP_NOTE_DOUBLE_US && t - pressed < PSPEXP_NOTE_DOUBLE_US + 20 * MS);
    }
    CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_RED); CHECK_EQ(o.buttons, VOL);
    step(0, 0, 1);
    step(X, 0, 1);
    CHECK_EQ(o.buttons, X); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_RED);
    step(NOTE, 0, 1);
    step(NOTE | L, 0, 1);
    CHECK_EQ(until_event(0, 1), 1u << PSPEXP_NOTE_REC_STOP); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_NONE);   /* (♪ let go meanwhile: still decided) */

    /* ♪ + L twice: a screenshot, the white dot for 0.6 s, no recording; while recording, it goes on */
    pspexp_note_init(&n);
    step(0, 0, 1);
    step(NOTE, 0, 1);
    step(NOTE | L, 0, 1);
    step(NOTE, 0, 1);
    step(NOTE | L, 0, 1);
    CHECK_EQ(o.events, 1u << PSPEXP_NOTE_SHOT); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_NONE); CHECK_EQ(o.buttons, 0);   /* (the dot waits: not in the picture) */
    for (i = 0; i < 13; i++) step(NOTE, 0, 1);
    CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_WHITE);
    CHECK_EQ(until_event(NOTE, 1), 0);                   /* no recording afterwards */
    CHECK_EQ(n.recording, 0);
    for (i = 0; i < 10; i++) step(0, 0, 1);
    CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_NONE); CHECK_EQ(o.buttons, 0);   /* (past 0.6 s; no tap: it was a pair) */
    step(NOTE, 0, 1); step(NOTE | L, 0, 1);
    CHECK_EQ(until_event(NOTE, 1), 1u << PSPEXP_NOTE_REC_START);
    step(NOTE, 0, 1); step(NOTE | L, 0, 1); step(NOTE, 0, 1); step(NOTE | L, 0, 1);
    CHECK_EQ(o.events, 1u << PSPEXP_NOTE_SHOT); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_RED);
    for (i = 0; i < 13; i++) step(0, 0, 1);
    CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_WHITE);
    for (i = 0; i < 45; i++) step(0, 0, 1);
    CHECK_EQ(n.recording, 1); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_RED);
    /* two L presses farther apart than the double-tap time: two recording toggles, no screenshot */
    pspexp_note_init(&n);
    step(0, 0, 1);
    step(NOTE, 0, 1); step(NOTE | L, 0, 1);
    CHECK_EQ(until_event(NOTE, 1), 1u << PSPEXP_NOTE_REC_START);
    step(NOTE | L, 0, 1);
    CHECK_EQ(until_event(NOTE, 1), 1u << PSPEXP_NOTE_REC_STOP);
    /* a ♪ + L waiting when the receiver goes: dropped */
    pspexp_note_init(&n);
    step(0, 0, 1); step(NOTE, 0, 1); step(NOTE | L, 0, 1);
    step(0, 0, 0);
    CHECK_EQ(until_event(0, 1), 0); CHECK_EQ(n.recording, 0);

    /* R and L in one reading, ♪ already held: both */
    pspexp_note_init(&n);
    step(NOTE, 0, 1);
    step(NOTE | R | L, 0, 1);
    CHECK_EQ(o.events, 1u << PSPEXP_NOTE_CLIP);          /* the clip at once */
    CHECK_EQ(until_event(NOTE, 1), 1u << PSPEXP_NOTE_REC_START);   /* the recording once no second L came */

    /* the receiver gone: buttons as read; the recording kept over a hand-over (under 8 s), forgotten after */
    step(0, 0, 0);
    for (i = 0; i < 300; i++) step(0, 0, 0);   /* 4.8 s */
    step(0, 0, 1);
    CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_RED);
    for (i = 0; i < 600; i++) step(0, 0, 0);   /* 9.6 s */
    CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_NONE);
    step(NOTE, 0, 1);
    step(NOTE | L, 0, 1);
    CHECK_EQ(until_event(NOTE, 1), 1u << PSPEXP_NOTE_REC_START);

    /* recordings only: ♪+R a pair doing nothing (R hidden, no event, no dot, no ♪ for the board);
     * ♪+L records as before */
    pspexp_note_init(&n);
    step(0, 0, PSPEXP_CAPTURE_RECORDINGS);
    step(NOTE, 0, PSPEXP_CAPTURE_RECORDINGS);
    step(NOTE | R, 0, PSPEXP_CAPTURE_RECORDINGS);
    CHECK_EQ(o.events, 0); CHECK_EQ(o.buttons, 0); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_NONE);
    step(0, 0, PSPEXP_CAPTURE_RECORDINGS);
    CHECK_EQ(o.buttons, 0);
    step(NOTE, 0, PSPEXP_CAPTURE_RECORDINGS);
    step(NOTE | L, 0, PSPEXP_CAPTURE_RECORDINGS);
    CHECK_EQ(until_event(NOTE, PSPEXP_CAPTURE_RECORDINGS), 1u << PSPEXP_NOTE_REC_START); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_RED);
    /* and screenshots: recordings only or not */
    step(NOTE, 0, PSPEXP_CAPTURE_RECORDINGS); step(NOTE | L, 0, PSPEXP_CAPTURE_RECORDINGS);
    step(NOTE, 0, PSPEXP_CAPTURE_RECORDINGS); step(NOTE | L, 0, PSPEXP_CAPTURE_RECORDINGS);
    CHECK_EQ(o.events, 1u << PSPEXP_NOTE_SHOT);

    /* ♪ and L in one reading (the PSP-1000 read both, untouched, mid-recording): no event, L
     * hidden, no tap for the board when it goes, the mute kept; said once for the log; a real ♪+L
     * after it still counts */
    pspexp_note_init(&n);
    step(0, 0, 1);
    step(NOTE | L, 1, 1);
    CHECK_EQ(o.events, 0); CHECK_EQ(o.refused, NOTE | L); CHECK_EQ(o.buttons, 0); CHECK_EQ(o.set_mute, 0);
    step(NOTE | L, 0, 1);
    CHECK_EQ(o.events, 0); CHECK_EQ(o.refused, 0);
    step(0, 0, 1);
    CHECK_EQ(o.buttons, 0); CHECK_EQ(o.set_mute, -1); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_NONE);
    for (i = 0; i < 10; i++) { step(0, 0, 1); CHECK_EQ(o.buttons & NOTE, 0); }
    step(NOTE, 0, 1);
    step(NOTE | L, 0, 1);
    CHECK_EQ(until_event(NOTE, 1), 1u << PSPEXP_NOTE_REC_START); CHECK_EQ(o.refused, 0);
    /* R the same */
    pspexp_note_init(&n);
    step(0, 0, 1);
    step(NOTE | R, 0, 1);
    CHECK_EQ(o.events, 0); CHECK_EQ(o.refused, NOTE | R);

    /* no mute setting known (-1): nothing set */
    pspexp_note_init(&n);
    step(0, -1, 1); step(NOTE, -1, 1); step(0, -1, 1);
    CHECK_EQ(o.set_mute, -1); CHECK_EQ(o.buttons, NOTE);

    /* the board's requests (CAPTURE_REQUEST): as the buttons would, at the next step */
    pspexp_note_init(&n);
    step(0, 0, PSPEXP_CAPTURE_RECORDINGS);
    pspexp_note_request(&n, PSPEXP_NOTE_SHOT, 42);
    step(0, 0, PSPEXP_CAPTURE_RECORDINGS);
    CHECK_EQ(o.events, 1u << PSPEXP_NOTE_SHOT); CHECK_EQ(o.shot_tag, 42); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_NONE);   /* (the board's own: no dot) */
    for (i = 0; i < 20; i++) { step(0, 0, PSPEXP_CAPTURE_RECORDINGS); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_NONE); }
    step(0, 0, PSPEXP_CAPTURE_RECORDINGS);
    CHECK_EQ(o.events, 0); CHECK_EQ(o.shot_tag, 0);   /* (once) */
    pspexp_note_request(&n, PSPEXP_NOTE_REC_START, 0);
    step(0, 0, PSPEXP_CAPTURE_RECORDINGS);
    CHECK_EQ(o.events, 1u << PSPEXP_NOTE_REC_START); CHECK(n.recording); CHECK(o.rec_at != 0);
    pspexp_note_request(&n, PSPEXP_NOTE_REC_START, 0);   /* already recording: nothing */
    step(0, 0, PSPEXP_CAPTURE_RECORDINGS);
    CHECK_EQ(o.events, 0); CHECK(n.recording);
    for (i = 0; i < 40; i++) step(0, 0, PSPEXP_CAPTURE_RECORDINGS);
    CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_RED);
    pspexp_note_request(&n, PSPEXP_NOTE_CLIP, 0);   /* recordings only: no clip */
    step(0, 0, PSPEXP_CAPTURE_RECORDINGS);
    CHECK_EQ(o.events, 0);
    pspexp_note_request(&n, PSPEXP_NOTE_REC_STOP, 0);
    step(0, 0, PSPEXP_CAPTURE_RECORDINGS);
    CHECK_EQ(o.events, 1u << PSPEXP_NOTE_REC_STOP); CHECK(!n.recording);
    pspexp_note_request(&n, PSPEXP_NOTE_REC_STOP, 0);   /* not recording: nothing */
    step(0, 0, PSPEXP_CAPTURE_RECORDINGS);
    CHECK_EQ(o.events, 0);
    pspexp_note_request(&n, PSPEXP_NOTE_CLIP, 0);   /* the last seconds kept: a clip, the green dot */
    step(0, 0, PSPEXP_CAPTURE_ALL);
    CHECK_EQ(o.events, 1u << PSPEXP_NOTE_CLIP); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_GREEN);
    /* a ♪ + L recording stopped by the board, then the board's start while ♪ + L stops: one change only */
    pspexp_note_init(&n);
    step(0, 0, 1);
    pspexp_note_request(&n, PSPEXP_NOTE_REC_START, 0); step(0, 0, 1);
    CHECK(n.recording);
    step(NOTE, 0, 1); step(NOTE | L, 0, 1);
    pspexp_note_request(&n, PSPEXP_NOTE_REC_START, 0);
    seen = (int)until_event(NOTE, 1);
    CHECK_EQ(seen, 1u << PSPEXP_NOTE_REC_STOP); CHECK(!n.recording);
    /* no receiver: requests dropped */
    pspexp_note_init(&n);
    pspexp_note_request(&n, PSPEXP_NOTE_SHOT, 7); step(0, 0, 0);
    CHECK_EQ(o.events, 0);
    step(0, 0, 1); CHECK_EQ(o.events, 0);

    return test_summary("note");
}
