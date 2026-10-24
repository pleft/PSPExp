/*
 * PSP game discs on the board: an ISO, a CSO (compressed in blocks), or a zip holding one of them
 * (a collection kept zipped). Sectors of 2048 bytes read from any of them, a file found on the
 * disc (ISO 9660), and what the launcher shows: the game's title and disc ID from PARAM.SFO and
 * its own icon, ICON0.PNG (144x80), the PSP games shelf's cover when no box art is there.
 *
 * A zip is read front to back only (deflate): enough for the metadata, which sits 40–50 MB in
 * (after the firmware update) and at most a few hundred MB; a sector behind the last one read
 * starts it again from the beginning.
 */
#ifndef PSPEXP_DISC_H
#define PSPEXP_DISC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSPEXP_DISC_SECTOR 2048

typedef struct PspExpDisc PspExpDisc;

typedef struct PspExpDiscInfo {
    char title[128];      /* PARAM.SFO's TITLE (UTF-8) */
    char disc_id[16];     /* DISC_ID: ULES00151 */
    char kind[4];         /* "iso", "cso", "zip" */
    uint64_t size;        /* the disc's bytes (as an ISO) */
} PspExpDiscInfo;

/* NULL with a reason in err when it is none of them */
PspExpDisc *pspexp_disc_open(const char *path, char *err, size_t errn);
void pspexp_disc_close(PspExpDisc *d);
uint64_t pspexp_disc_size(const PspExpDisc *d);
const char *pspexp_disc_kind(const PspExpDisc *d);
/* count sectors from lba into out; 0, or -1 (past the end, damaged) */
int pspexp_disc_read(PspExpDisc *d, uint32_t lba, uint32_t count, void *out);
/* a file on the disc ("PSP_GAME/PARAM.SFO", any case): its first sector and size; 0 or -1 */
int pspexp_disc_find(PspExpDisc *d, const char *path, uint32_t *lba, uint32_t *size);
/* a whole file into a new buffer (free it), at most max bytes; NULL when missing or larger */
uint8_t *pspexp_disc_load(PspExpDisc *d, const char *path, uint32_t max, uint32_t *size);

/* a key of a PARAM.SFO (a string or a number, as text); 0, or -1 when missing */
int pspexp_sfo_get(const uint8_t *sfo, size_t n, const char *key, char *out, size_t outn);

/* the title, disc ID and size; the icon written to icon_png when not NULL (left out when the
 * disc has none); 0 or -1 with a reason in err */
int pspexp_disc_info(const char *path, PspExpDiscInfo *info, const char *icon_png, char *err, size_t errn);

#ifdef __cplusplus
}
#endif

#endif /* PSPEXP_DISC_H */
