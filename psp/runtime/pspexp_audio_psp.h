/* PSP audio output for the runtime mixer: one hardware channel fed by a
 * high-priority thread, 1024 stereo frames per block at 44.1 kHz. */
#ifndef PSPEXP_AUDIO_PSP_H
#define PSPEXP_AUDIO_PSP_H
#include "../../runtime/pspexp_runtime.h"
void pspexp_audio_psp_ops(PspExpRuntimeOps *ops);   /* fills audio_start/stop/lock/unlock */
#endif
