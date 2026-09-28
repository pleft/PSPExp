/*
 * pspexp-term's test-control port: a Unix socket that takes one command line
 * at a time and answers when the command is done (tests/psp drives a real or
 * stand-in PSP through it). The commands themselves are carried out by the
 * console's main loop (main.c); this file only moves lines.
 *
 * Reply: text lines, the first "ok ..." or "error ...", then a line "END".
 * `pspexp-term --ctl "COMMAND"` is the client: it prints the reply and exits
 * 0 on ok, 1 on error, 2 when no console answers.
 */
#ifndef PSPEXP_TERM_CONTROL_H
#define PSPEXP_TERM_CONTROL_H

#include <stddef.h>

/* the socket's path: $PSPEXP_TERM_CONTROL, or /tmp/pspexp-term-<uid>.ctl */
const char *ctl_path(void);
/* listen (owner only); 0 or -1 */
int  ctl_open(const char *path);
void ctl_close(void);
/* a complete command line, once, when none is in progress; NULL otherwise */
const char *ctl_take(void);
/* the command in progress is done: send text (lines; "ok"/"error" first) and END */
void ctl_reply(const char *text);
/* the client: send cmd, print the reply */
int  ctl_client(const char *path, const char *cmd, int timeout_s);

#endif
