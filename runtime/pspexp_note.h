/*
 * The note button (♪, the PSP-1000's sound button) inside PSPExp, while the PSPExp plugin host
 * is there (the PSP captures: pspexp_capture_psp.c): ♪+R saves the last 30 seconds as a clip, ♪+L
 * starts a recording and, pressed again, ends it, as the plugin does in a native game. While ♪ is
 * down the board sees none of the game's buttons (nor those held with it, until let go); ♪ pressed
 * alone reaches the board when it is let go, held for a moment (the console's music toggle). The
 * system toggles its mute as ♪ is pressed: for a pair it is put back, at once and for half a
 * second after; for ♪ alone it is applied when ♪ is let go, as it always was. Without a receiver
 * every button passes as read. With a receiver keeping recordings only (no last 30 seconds:
 * PSPEXP_CAPTURE_RECORDINGS), ♪+R is a pair that does nothing. A pair is ♪ held, then R or L: ♪
 * and R or L appearing in one reading is not one (the PSP-1000 read both, untouched, in the
 * middle of a recording, which stopped it: 2026-10-25), and does nothing at all (no tap either).
 *
 * Plain C99, no PSP calls: the runtime reads the buttons and the mute, and does what comes out.
 */
#ifndef PSPEXP_NOTE_H
#define PSPEXP_NOTE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the events (the recording's "PXCE" types) */
#define PSPEXP_NOTE_CLIP       1
#define PSPEXP_NOTE_REC_START  2
#define PSPEXP_NOTE_REC_STOP   3

/* the dot that confirms a press, in the screen's bottom-right corner */
#define PSPEXP_NOTE_DOT_NONE   0
#define PSPEXP_NOTE_DOT_GREEN  1   /* a clip: for 1.2 s */
#define PSPEXP_NOTE_DOT_RED    2   /* recording */

typedef struct PspExpNote {
    uint32_t was;            /* the buttons at the last step */
    uint32_t hidden;         /* buttons the board does not see */
    int pair;                /* R or L used while ♪ is down */
    uint32_t tap_until;      /* ♪ alone let go: the board sees it pressed until then (0: no) */
    int mute_before;         /* the system's mute before ♪ (-1: not known) */
    int mute_wanted;         /* its toggle while ♪ is down, applied for ♪ alone (-1: none) */
    uint32_t keep_until;     /* after a pair: the mute kept as it was until then (0: no) */
    int recording;
    uint32_t green_until;    /* the clip's dot until then (0: none) */
    uint32_t off_since;      /* the receiver gone since then (0: there); a recording forgotten after 8 s */
} PspExpNote;

typedef struct PspExpNoteOut {
    uint32_t buttons;        /* for the board */
    uint32_t events;         /* bit 1 << PSPEXP_NOTE_*: to send, in that order */
    int set_mute;            /* -1, or the mute to set */
    int dot;                 /* PSPEXP_NOTE_DOT_* */
    uint32_t refused;        /* the buttons read when ♪ came with R or L at once (0: not this step), for the log */
} PspExpNoteOut;

void pspexp_note_init(PspExpNote *n);
/* one reading: the buttons (♪ and the volume buttons included), the system's mute (-1: not
 * known), the receiver's PSPEXP_CAPTURE_* (0: none there), the time in microseconds (wrapping) */
void pspexp_note_step(PspExpNote *n, uint32_t buttons, int mute, int capture, uint32_t now_us, PspExpNoteOut *out);

#ifdef __cplusplus
}
#endif

#endif /* PSPEXP_NOTE_H */
