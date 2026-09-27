/* The FILE channel on the PSP: the memory stick (ms0:) and a PSP Go's
 * internal storage (ef0:) through sceIo. */
#ifndef PSPEXP_FS_PSP_H
#define PSPEXP_FS_PSP_H

#include "../../runtime/pspexp_runtime.h"

void pspexp_fs_psp_ops(PspExpRuntimeOps *ops);

#endif
