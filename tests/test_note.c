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
    CHECK_EQ(o.events, 1u << PSPEXP_NOTE_REC_START); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_RED); CHECK_EQ(o.buttons, VOL);
    step(0, 0, 1);
    step(X, 0, 1);
    CHECK_EQ(o.buttons, X); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_RED);
    step(NOTE, 0, 1);
    step(NOTE | L, 0, 1);
    CHECK_EQ(o.events, 1u << PSPEXP_NOTE_REC_STOP); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_NONE);

    /* R and L in one reading, ♪ already held: both */
    pspexp_note_init(&n);
    step(NOTE, 0, 1);
    step(NOTE | R | L, 0, 1);
    CHECK_EQ(o.events, (1u << PSPEXP_NOTE_CLIP) | (1u << PSPEXP_NOTE_REC_START));

    /* the receiver gone: buttons as read; the recording kept over a hand-over (under 8 s), forgotten after */
    step(0, 0, 0);
    for (i = 0; i < 300; i++) step(0, 0, 0);   /* 4.8 s */
    step(0, 0, 1);
    CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_RED);
    for (i = 0; i < 600; i++) step(0, 0, 0);   /* 9.6 s */
    CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_NONE);
    step(NOTE, 0, 1);
    step(NOTE | L, 0, 1);
    CHECK_EQ(o.events, 1u << PSPEXP_NOTE_REC_START);

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
    CHECK_EQ(o.events, 1u << PSPEXP_NOTE_REC_START); CHECK_EQ(o.dot, PSPEXP_NOTE_DOT_RED);

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
    CHECK_EQ(o.events, 1u << PSPEXP_NOTE_REC_START); CHECK_EQ(o.refused, 0);
    /* R the same */
    pspexp_note_init(&n);
    step(0, 0, 1);
    step(NOTE | R, 0, 1);
    CHECK_EQ(o.events, 0); CHECK_EQ(o.refused, NOTE | R);

    /* no mute setting known (-1): nothing set */
    pspexp_note_init(&n);
    step(0, -1, 1); step(NOTE, -1, 1); step(0, -1, 1);
    CHECK_EQ(o.set_mute, -1); CHECK_EQ(o.buttons, NOTE);

    return test_summary("note");
}
