/*
 * The note button inside PSPExp (pspexp_note.h), as the PSPExp plugin handles it in a native
 * game (psp/plugin/main.c, note_check).
 */
#include "pspexp_note.h"
#include "../protocol/pspexp_protocol.h"
#include <string.h>

#define GAME_BUTTONS (PSPEXP_BUTTON_SELECT | PSPEXP_BUTTON_START | PSPEXP_BUTTON_UP | PSPEXP_BUTTON_RIGHT | \
                      PSPEXP_BUTTON_DOWN | PSPEXP_BUTTON_LEFT | PSPEXP_BUTTON_LTRIGGER | PSPEXP_BUTTON_RTRIGGER | \
                      PSPEXP_BUTTON_TRIANGLE | PSPEXP_BUTTON_CIRCLE | PSPEXP_BUTTON_CROSS | PSPEXP_BUTTON_SQUARE)
#define TAP_US 120000u       /* ♪ alone as the board sees it: it keeps only the latest reading, so a press it cannot miss */
#define GREEN_US 1200000u
#define KEEP_US 500000u      /* (the mute may toggle again on the release) */
#define FORGET_US 8000000u   /* the receiver waits as long for the next program holding the PSP */

static uint32_t stamp(uint32_t t) { return t ? t : 1; }   /* (0: none) */
static int until(uint32_t now, uint32_t end) { return end && (int32_t)(now - end) < 0; }

void pspexp_note_init(PspExpNote *n)
{
    memset(n, 0, sizeof(*n));
    n->mute_before = n->mute_wanted = -1;
}

void pspexp_note_step(PspExpNote *n, uint32_t b, int mute, int capture, uint32_t now, PspExpNoteOut *out)
{
    out->events = 0;
    out->set_mute = -1;
    if (!capture) {   /* every button as read; a recording forgotten once the receiver has surely ended it */
        if (!n->off_since) n->off_since = stamp(now);
        else if (now - n->off_since >= FORGET_US) n->recording = 0;
        n->hidden = 0; n->pair = 0; n->tap_until = 0; n->keep_until = 0; n->mute_wanted = -1;
        n->mute_before = mute;
        out->buttons = b;
    } else {
        n->off_since = 0;
        if (b & PSPEXP_BUTTON_NOTE) {
            uint32_t pressed = b & ~n->was;
            if (!(n->was & PSPEXP_BUTTON_NOTE)) { n->pair = 0; n->mute_wanted = -1; n->tap_until = 0; }
            n->hidden = GAME_BUTTONS | PSPEXP_BUTTON_NOTE;
            if (mute >= 0 && n->mute_before >= 0 && mute != n->mute_before) {   /* the system's toggle: kept for ♪ alone */
                n->mute_wanted = mute;
                out->set_mute = n->mute_before;
            }
            if (pressed & PSPEXP_BUTTON_RTRIGGER) {
                n->pair = 1;
                if (capture == PSPEXP_CAPTURE_ALL) {   /* (recordings only: no last seconds to save) */
                    out->events |= 1u << PSPEXP_NOTE_CLIP;
                    n->green_until = stamp(now + GREEN_US);
                }
            }
            if (pressed & PSPEXP_BUTTON_LTRIGGER) {
                n->pair = 1;
                n->recording = !n->recording;
                out->events |= 1u << (n->recording ? PSPEXP_NOTE_REC_START : PSPEXP_NOTE_REC_STOP);
            }
        } else {
            n->hidden &= b;   /* (those held with it hidden until let go) */
            if (n->was & PSPEXP_BUTTON_NOTE) {   /* let go */
                if (n->pair) n->keep_until = stamp(now + KEEP_US);
                else {
                    if (n->mute_wanted >= 0) out->set_mute = n->mute_wanted;
                    n->tap_until = stamp(now + TAP_US);
                }
                n->mute_wanted = -1;
            }
            if (until(now, n->keep_until)) {
                if (mute >= 0 && n->mute_before >= 0 && mute != n->mute_before) out->set_mute = n->mute_before;
            } else {
                n->keep_until = 0;
                n->mute_before = out->set_mute >= 0 ? out->set_mute : mute;
            }
        }
        out->buttons = b & ~n->hidden;
        if (until(now, n->tap_until)) out->buttons |= PSPEXP_BUTTON_NOTE;
        else n->tap_until = 0;
    }
    n->was = b;
    if (!until(now, n->green_until)) n->green_until = 0;
    out->dot = n->recording ? PSPEXP_NOTE_DOT_RED : n->green_until ? PSPEXP_NOTE_DOT_GREEN : PSPEXP_NOTE_DOT_NONE;
}
