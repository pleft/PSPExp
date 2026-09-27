/*
 * The PSP's memory stick as a drive of the file manager: files_ops' remote
 * functions (FRemote) over the link's FILE channel (pspexp_fs_*).
 *
 * The link belongs to the thread that drives it (pspexp-term's main loop),
 * while copies run on a job thread: a job's request waits here until the
 * main loop calls files_psp_service, which runs it. Requests from the main
 * thread itself run at once.
 */
#ifndef PSPEXP_TERM_FILES_PSP_H
#define PSPEXP_TERM_FILES_PSP_H

#include <stdint.h>
#include "files_ops.h"
#include "../../host/libpspexp/pspexp.h"

/* the calling thread is the one that drives ctx; installs the remote functions */
void files_psp_init(PspExpContext *ctx);
/* the main loop, each turn (outside a frame): runs waiting requests, up to budget_us */
void files_psp_service(PspExpContext *ctx, uint32_t budget_us);
/* the PSP answers file requests now (a connected runtime that offers them) */
int  files_psp_available(void);
/* free and total bytes of the drive that holds path; 0 or -1 */
int  files_psp_space(const char *path, uint64_t *free_bytes, uint64_t *total_bytes);

#endif
