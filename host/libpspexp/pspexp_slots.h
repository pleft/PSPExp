/*
 * The PSP's runtime in two slots (docs/Package_Manager.md §6.1), seen from the
 * host: which slot runs, and an update of the other one over the FILE channel.
 *
 * On the stick, next to the EBOOT: slot-a/, slot-b/ (each a whole set with a
 * VERSION listing every file's size and SHA-256), `active` (slot=X, tries=N)
 * and, after an update, `active.new` (the boot loader takes it at the next
 * start: writing it completely is the one switch point).
 */
#ifndef PSPEXP_SLOTS_H
#define PSPEXP_SLOTS_H

#include <stddef.h>
#include "pspexp.h"

typedef struct PspExpSlots {
    char dir[256];        /* "ms0:/PSP/GAME/PSPExp" */
    char current;         /* 'a' or 'b': what `active` names (0 unknown) */
    char pending;         /* what a complete `active.new` names (0 none) */
    char version[2][64];  /* each slot's VERSION's version= ("" none) */
    char fallback[160];   /* the loader's last fallback, "" none */
} PspExpSlots;

/* the PSP's slotted install; 0, or -1 when there is none (an install from
 * before slots: its loader is replaced once, by hand) */
int pspexp_slots_read(PspExpContext *ctx, PspExpSlots *out);

typedef void (*PspExpSlotLog)(void *user, const char *msg);

/* the runtime set in srcdir (its VERSION and the files it lists) into the
 * inactive slot, in order: invalidate that slot's VERSION, copy, read every
 * file back and check it, write VERSION and check it, write active.new. The
 * running runtime is untouched; the next start of PSPExp boots the new slot.
 * crash: a test hook, the step after which to stop dead (as a cable pull):
 * invalidated, copy-partial, copied, version-partial, version,
 * switch-partial; NULL in use. 0, -1 with err, -2 when crashed on purpose */
int pspexp_slots_update(PspExpContext *ctx, const char *srcdir, const char *crash,
                        PspExpSlotLog log, void *user, char *err, size_t errlen);

#endif
