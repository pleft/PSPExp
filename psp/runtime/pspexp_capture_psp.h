#ifndef PSPEXP_CAPTURE_PSP_H
#define PSPEXP_CAPTURE_PSP_H

#include "../../runtime/pspexp_runtime.h"
#include "../executor-ge/pspexp_ge.h"

/* PSPExp Capture inside PSPExp (pspexp_capture_psp.c). write sends bytes on the link; lock and
 * unlock keep one message whole against the main loop's writes. */
void pspexp_capture_psp_init(PspExpGeExecutor *ge, int (*write)(const void *, int), void (*lock)(void), void (*unlock)(void));
void pspexp_capture_psp_ops(PspExpRuntimeOps *ops);
/* the sound the PSP plays, as it is about to (the audio thread): s16 stereo */
void pspexp_capture_psp_audio(const short *pcm, int frames);
/* a note-button event into the stream (1 a clip, 2 a recording starts, 3 it stops) */
void pspexp_capture_psp_event(int type);
/* the receiver's PSPEXP_CAPTURE_* (0: none) */
int pspexp_capture_psp_on(void);
/* a ♪+L recording under way (after its start event; before its stop event goes, 0) */
void pspexp_capture_psp_recording(int rec);
/* the frame on screen sent as it is (a recording starting: recordings only) */
void pspexp_capture_psp_grab(void);

#endif
