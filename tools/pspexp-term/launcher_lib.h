/*
 * The launcher's library: what can be started. It comes from two places and
 * nothing else (no folder trees of the old GameShell launcher):
 *
 *   /opt/pspexp/apps/<pkg>/NAME.entry       installed PSPExp packages
 *                                           (scripts/packages/pspexp-pkg-build, pspexp-pkg)
 *   /opt/pspexp/systems/<id>.system         emulated systems they install
 *   ~/.config/pspexp-launcher/library/NAME.entry  hand-made entries
 *   ~/.config/pspexp-launcher/systems/NAME.system hand-made systems
 *   ~/.config/pspexp-launcher/recent, favourites   entry ids, one per line
 *   ~/.config/pspexp-launcher/settings             the launcher's options
 *
 * (PSPEXP_ROOT replaces /opt/pspexp, PSPEXP_LAUNCHER_HOME the config
 * directory; for tests.) Both file kinds are KEY=value lines, '#' starts a
 * comment. Keys:
 *
 *   .entry   title, type (game | app | tool), exec (a shell command, run in
 *            dir), dir (default: the package directory), env (NAME=value,
 *            one per line), icon (a Lucide icon name), art (a PNG; relative
 *            to the package), tags, description, pspexp (1: drawn by the PSP
 *            itself, no mirror), hidden (1)
 *   .system  name, short (a 2-5 letter badge), roms (folder, "~/" allowed;
 *            created when missing), ext (comma list), exec (%ROM% becomes
 *            the quoted ROM path), icon, thumbs (the libretro-thumbnails
 *            system folder, for box art), pspexp (1), pkg (the package that
 *            installed it: exec then runs in that package's directory, so it
 *            can name the package's own player, "./pspexp-retro ..."; without
 *            it, in the ROM's folder), env (NAME=value, one per line)
 *
 * Ids: "p:<pkg>/<stem>" for package entries, "e:<stem>" for hand-made ones,
 * "r:<system>:<rom path>" for ROMs. lib_changed() notices installs, removals
 * and new ROMs by the directories' modification times.
 */
#ifndef PSPEXP_LAUNCHER_LIB_H
#define PSPEXP_LAUNCHER_LIB_H

#include <stddef.h>

enum { LT_GAME, LT_APP, LT_TOOL, LT_ROM, LT_ACTION };

typedef struct LEntry {
    char id[200];
    char title[96];
    char exec[512], dir[256];
    char icon[32], art[320], tags[96], desc[300];
    char graphics[16];      /* how it draws: "sdl", "opengl", "opengl-shaders" ("" not said) */
    char env[400];          /* NAME=value pairs, space-separated, set for the command */
    char pkg[64];           /* the package it came from, "" for hand-made entries */
    int type, pspexp, sys;  /* sys: system index for LT_ROM, else -1 */
    int fav;
    long long size;         /* ROM file size */
} LEntry;

typedef struct LSystem {
    char id[48], name[64], shortname[8];
    char roms[256], ext[96], exec[512], icon[32], thumbs[96];
    char graphics[16];                /* as an entry's: how its player draws */
    char pkg[64], dir[256];           /* pkg: its package; dir: where exec runs ("" = the ROM's folder) */
    char env[400];                    /* NAME=value pairs for exec */
    int pspexp;
    LEntry *roms_list; int nroms, scanned;
} LSystem;

typedef struct Library {
    char home[512];                    /* the config directory */
    char root[256];                    /* /opt/pspexp */
    LEntry *e; int n;                  /* games, apps, tools */
    LSystem sys[32]; int nsys;
    char recent[16][200]; int nrecent;
    char favs[128][200]; int nfavs;
    unsigned long long sig;            /* lib_changed's last signature */
} Library;

int  lib_load(Library *L);
/* reloads everything; returns the number of games, apps and ROMs now known */
int  lib_reload(Library *L);
/* 1 when a package, entry, system or ROM folder changed since the last load */
int  lib_changed(Library *L);
void lib_free(Library *L);

void lib_scan_roms(Library *L, int sys);
LEntry *lib_find(Library *L, const char *id);

void lib_recent_push(Library *L, const char *id);
int  lib_fav_toggle(Library *L, const char *id);   /* returns the new state */
int  lib_is_fav(const Library *L, const char *id);

/* the shell command that starts an entry */
void lib_command(const Library *L, const LEntry *e, char *out, size_t n);
/* the ROM entry for a file anywhere: the first system whose extensions
 * match (its index), -1 when none plays it */
int  lib_rom_entry(const Library *L, const char *path, LEntry *out);

/* Box art: the libretro-thumbnails names of one system, from its directory
 * listing (the HTML of .../Named_Boxarts/): names without ".png", decoded.
 * Returns the count (names freed with lib_thumbs_free). */
int  lib_thumbs_parse(const char *html, size_t len, char ***names);
void lib_thumbs_free(char **names, int n);
/* The best cover for a ROM, whatever its file's naming (No-Intro, GoodTools,
 * by hand): titles compared without tags, articles and punctuation; among
 * equal titles the region of the ROM's tags wins ((E) Europe, (U) USA,
 * (J) Japan, (W) World), then the fewest extra tags (no betas, protos,
 * revisions). -1 when no title matches. */
int  lib_thumbs_match(char *const *names, int n, const char *title, const char *tags);

/* settings: KEY=value in <home>/settings */
int  lib_setting_int(const Library *L, const char *key, int def);
void lib_setting_set(const Library *L, const char *key, int value);

#endif
