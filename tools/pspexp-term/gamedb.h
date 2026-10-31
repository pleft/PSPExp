/*
 * A ROM's facts for Game details: its description, developer, publisher, genre and release date
 * from OpenVGDB (github.com/OpenVGDB/OpenVGDB, the database OpenEmu and sselph's scraper use),
 * which each board downloads once (9 MB; 42 MB unpacked, ~/.cache/pspexp-launcher/openvgdb.sqlite)
 * and then reads offline: no account, no key, no limit. A ROM is found by its CRC (an NES, Lynx or
 * copier SNES header left out; a zipped one's CRC read from the zip), a PSP game by its disc's
 * serial, else by its name (punctuation, case and "the" aside). Looked up once, in a child, the
 * answer kept in ~/.cache/pspexp-launcher/facts/<system>/<rom>.txt (".none": not found).
 */
#ifndef PSPEXP_TERM_GAMEDB_H
#define PSPEXP_TERM_GAMEDB_H

typedef struct GameFacts {
    char desc[1200], developer[96], publisher[96], genre[96], year[8];
} GameFacts;

/* the facts of the ROM at path (system id: "nes", "md", "psp"...; its title for the name match).
 * 1 known (out filled); 0 not yet: the database or this ROM on its way (a child: gamedb_poll), or
 * downloads off (allow 0) with no database yet; -1 none to be had */
int  gamedb_facts(const char *path, const char *sys, const char *title, int allow, GameFacts *out);
/* each turn: 1 when a download or a lookup just finished (draw again) */
int  gamedb_poll(void);
/* what is under way, for the screen: 1 the database downloading, 2 a ROM looked up, 0 nothing */
int  gamedb_busy(void);

#endif
