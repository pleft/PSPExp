/*
 * A ROM's facts for Game details: its description, developer, publisher, genres, year and players
 * from the LaunchBox Games Database (gamesdb.launchbox-app.com, its daily Metadata.zip), which each
 * board downloads once, streamed (109 MB; never kept whole): the games of the systems PSPExp plays
 * kept in ~/.cache/pspexp-launcher/launchbox.sqlite, read offline after (no account, no key, no
 * limit). A ROM is found by its title, a game's name and its other names as words (punctuation,
 * case, "the" and bracketed tags aside: "Legend of Zelda, The - A Link to the Past (E) [!]" is "The
 * Legend of Zelda: A Link to the Past"), else by its file name in LaunchBox's list of No-Intro's and
 * Redump's. Looked up once, in a child, the answer kept in facts/<system>/<rom>.txt (".none": not
 * found in that copy).
 */
#ifndef PSPEXP_TERM_GAMEDB_H
#define PSPEXP_TERM_GAMEDB_H

/* desc: the whole of it, its paragraphs apart ("\n" between them) */
typedef struct GameFacts {
    char desc[12288], developer[96], publisher[96], genre[96], year[8], players[16];
} GameFacts;

/* the facts of the ROM at path (system id: "nes", "md", "psp"...; its title for the name match).
 * 1 known (out filled); 0 not yet: the database or this ROM on its way (a child: gamedb_poll); -1
 * none to be had (not found, or downloads off (allow 0) and no database yet) */
int  gamedb_facts(const char *path, const char *sys, const char *title, int allow, GameFacts *out);
/* each turn: 1 when the download or a lookup just finished (draw again) */
int  gamedb_poll(void);
/* what is under way, for the screen: 1 the database downloading, 2 a ROM looked up, 0 nothing */
int  gamedb_busy(void);

#endif
