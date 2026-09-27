/*
 * The FILE channel's functions on a desktop (the SDL stand-in PSP, the null
 * executor in tests): "ms0:/..." is a folder of this computer, "ef0:/..." its
 * "ef0" subfolder. One root per process.
 */
#ifndef PSPEXP_FS_POSIX_H
#define PSPEXP_FS_POSIX_H

#include "pspexp_runtime.h"

/* root: the folder that plays the memory stick; NULL or "": getenv("PSPEXP_MS0");
 * nothing when neither is set (the runtime then does not offer FILES) */
void pspexp_fs_posix_install(PspExpRuntimeOps *ops, const char *root);

#endif
