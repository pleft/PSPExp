/*
 * The launcher's library (tools/pspexp-term/launcher_lib.c): entries from
 * installed packages and hand-made files, emulated systems and their ROMs,
 * the commands it builds, favourites and recent, settings, and noticing a
 * package installed or removed. Works in a temporary directory standing in
 * for /opt/pspexp and ~/.config/pspexp-launcher.
 */
#include "test_util.h"
#include "../tools/pspexp-term/launcher_lib.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char base[256];

static void put(const char *rel, const char *text)
{
    char p[512], d[512], *s;
    FILE *f;
    snprintf(p, sizeof(p), "%s/%s", base, rel);
    snprintf(d, sizeof(d), "%s", p);
    for (s = d + strlen(base) + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(d, 0755); *s = '/'; }
    f = fopen(p, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static LEntry *by_title(Library *L, const char *t)
{
    int i;
    for (i = 0; i < L->n; i++) if (!strcmp(L->e[i].title, t)) return &L->e[i];
    return NULL;
}

static int sys_index(Library *L, const char *id)
{
    int i;
    for (i = 0; i < L->nsys; i++) if (!strcmp(L->sys[i].id, id)) return i;
    return -1;
}

int main(void)
{
    Library L;
    char p[600], cmd[1200], home[300];
    LEntry *e;
    int s, n;

    snprintf(base, sizeof(base), "/tmp/pspexp-launcher-test-%d", (int)getpid());
    mkdir(base, 0755);
    snprintf(p, sizeof(p), "%s/root", base); setenv("PSPEXP_ROOT", p, 1);
    snprintf(home, sizeof(home), "%s/home", base);
    setenv("HOME", home, 1);
    snprintf(p, sizeof(p), "%s/home/.config/pspexp-launcher", base); setenv("PSPEXP_LAUNCHER_HOME", p, 1);

    /* a package with two entries (Freedoom), one with a cover (Tyrian), a hidden entry */
    put("root/apps/freedoom/phase1.entry",
        "title=Freedoom: Phase 1\ntype=game\nexec=pspexp-run chocolate-doom -iwad ./freedoom1.wad\n"
        "env=PSPEXP_KEYS=cross=lctrl,circle=space\nenv=PSPEXP_NUB=keys\npspexp=1\ndescription=Episodic Doom\ngraphics=sdl\n"
        "control=cross: Fire\ncontrol=no colon here\ncontrol=l r: Strafe\n");
    put("root/apps/freedoom/phase2.entry", "title=Freedoom: Phase 2\nexec=pspexp-run chocolate-doom -iwad ./freedoom2.wad\npspexp=1\n");
    put("root/apps/freedoom/phase2.png", "PNG");
    put("root/apps/opentyrian/opentyrian.entry", "title=OpenTyrian\nexec=./run.sh\npspexp=1\n");
    put("root/apps/opentyrian/cover.png", "PNG");
    put("root/apps/opentyrian/debug.entry", "title=Tyrian debug\nexec=./run.sh --debug\nhidden=1\n");
    /* a hand-made entry */
    put("home/.config/pspexp-launcher/library/notes.entry", "title=Notes\ntype=app\nexec=nano\nicon=file-text\n");
    /* an emulated system with ROMs in ~/roms, and one without a ROM folder yet */
    put("root/systems/gba.system", "name=Game Boy Advance\nshort=GBA\nroms=~/roms/gba\next=gba,gb\nexec=pspexp-retro /opt/pspexp/cores/mgba.so %ROM%\nthumbs=Nintendo - Game Boy Advance\ngraphics=sdl\npspexp=1\n");
    put("root/systems/snes.system", "name=Super Nintendo\nshort=SNES\nroms=~/roms/snes\next=sfc\nexec=./pspexp-retro snes9x %ROM%\n"
        "pkg=snes\nenv=PSPEXP_RETRO_ASPECT=pixel\ncontrol=cross circle: B, A\n");
    put("home/roms/snes/Super Game (Europe).sfc", "x");
    put("home/roms/gba/Legend of Zelda, The - The Minish Cap (USA).gba", "x");
    put("home/roms/gba/Golden Sun (USA, Europe).gba", "xx");
    put("home/roms/gba/readme.txt", "not a rom");

    CHECK_EQ(lib_load(&L), 0);

    /* entries: packages and hand-made ones, sorted by title, hidden ones left out */
    CHECK_EQ(L.n, 4);
    CHECK(by_title(&L, "Tyrian debug") == NULL);
    e = by_title(&L, "Freedoom: Phase 1");
    CHECK(e && !strcmp(e->id, "p:freedoom/phase1") && !strcmp(e->pkg, "freedoom") && e->pspexp == 1);
    if (e) {
        snprintf(p, sizeof(p), "%s/root/apps/freedoom", base);
        CHECK(!strcmp(e->dir, p));   /* runs in its package */
        CHECK(!strcmp(e->desc, "Episodic Doom"));
        CHECK(!strcmp(e->graphics, "sdl"));
        CHECK(e->art[0] == 0);       /* no phase1.png, no cover.png */
        lib_command(&L, e, cmd, sizeof(cmd));
        CHECK(!strcmp(cmd, "PSPEXP_KEYS='cross=lctrl,circle=space' PSPEXP_NUB='keys' pspexp-run chocolate-doom -iwad ./freedoom1.wad"));
        /* its controls: the control= lines with a colon, read from its file */
        CHECK_EQ(lib_controls(&L, e, cmd, sizeof(cmd)), 2);
        CHECK(!strcmp(cmd, "cross: Fire\nl r: Strafe\n"));
        CHECK_EQ(lib_controls(&L, e, cmd, 8), 0);   /* too long for the room: left out */
    }
    e = by_title(&L, "Freedoom: Phase 2");
    CHECK(e && strstr(e->art, "/freedoom/phase2.png") != NULL);   /* <stem>.png */
    e = by_title(&L, "OpenTyrian");
    CHECK(e && strstr(e->art, "/opentyrian/cover.png") != NULL);
    e = by_title(&L, "Notes");
    CHECK(e && !strcmp(e->id, "e:notes") && e->type == LT_APP && e->pkg[0] == 0);
    if (e) CHECK_EQ(lib_controls(&L, e, cmd, sizeof(cmd)), 0);   /* none */
    CHECK(!strcmp(L.e[0].title, "Freedoom: Phase 1") && !strcmp(L.e[3].title, "OpenTyrian"));

    /* systems: ~ expanded, ROM folders created, ROMs named and sorted */
    CHECK_EQ(L.nsys, 2);
    snprintf(p, sizeof(p), "%s/home/roms/snes", base);
    { struct stat st; CHECK(stat(p, &st) == 0 && S_ISDIR(st.st_mode)); }
    s = sys_index(&L, "gba");
    CHECK(s >= 0);
    if (s >= 0) {
        CHECK_EQ(L.sys[s].nroms, 2);
        CHECK(L.sys[s].pspexp == 1);
        CHECK(!strcmp(L.sys[s].graphics, "sdl"));
        if (L.sys[s].nroms == 2) {
            CHECK(!strcmp(L.sys[s].roms_list[0].title, "Golden Sun"));
            CHECK(!strcmp(L.sys[s].roms_list[0].tags, "USA, Europe"));
            CHECK(!strcmp(L.sys[s].roms_list[1].title, "The Legend of Zelda - The Minish Cap"));
            CHECK(L.sys[s].roms_list[0].pspexp == 1);
            lib_command(&L, &L.sys[s].roms_list[0], cmd, sizeof(cmd));
            { const char *want = "exec pspexp-retro /opt/pspexp/cores/mgba.so '"; CHECK(!strncmp(cmd, want, strlen(want))); }
            CHECK(strstr(cmd, "Golden Sun (USA, Europe).gba'") != NULL);
            CHECK_EQ(lib_controls(&L, &L.sys[s].roms_list[0], cmd, sizeof(cmd)), 0);   /* a system without */
        }
    }

    /* a system from a package runs in that package, with its environment */
    s = sys_index(&L, "snes");
    CHECK(s >= 0);
    if (s >= 0) {
        lib_scan_roms(&L, s);
        CHECK_EQ(L.sys[s].nroms, 1);
        if (L.sys[s].nroms == 1) {
            snprintf(p, sizeof(p), "%s/root/apps/snes", base);
            CHECK(!strcmp(L.sys[s].roms_list[0].dir, p));
            lib_command(&L, &L.sys[s].roms_list[0], cmd, sizeof(cmd));
            { const char *want = "PSPEXP_RETRO_ASPECT='pixel' exec ./pspexp-retro snes9x '"; CHECK(!strncmp(cmd, want, strlen(want))); }
            CHECK_EQ(lib_controls(&L, &L.sys[s].roms_list[0], cmd, sizeof(cmd)), 1);   /* a ROM: its system's controls */
            CHECK(!strcmp(cmd, "cross circle: B, A\n"));
        }
    }

    /* favourites and recent persist as ids */
    CHECK_EQ(lib_fav_toggle(&L, "p:opentyrian/opentyrian"), 1);
    lib_recent_push(&L, "p:freedoom/phase1");
    lib_recent_push(&L, "e:notes");
    lib_recent_push(&L, "p:freedoom/phase1");
    lib_free(&L);
    CHECK_EQ(lib_load(&L), 0);
    CHECK(lib_is_fav(&L, "p:opentyrian/opentyrian"));
    CHECK(by_title(&L, "OpenTyrian") && by_title(&L, "OpenTyrian")->fav);
    CHECK_EQ(L.nrecent, 2);
    CHECK(!strcmp(L.recent[0], "p:freedoom/phase1"));

    /* settings */
    CHECK_EQ(lib_setting_int(&L, "boxart", 1), 1);
    lib_setting_set(&L, "boxart", 0);
    lib_setting_set(&L, "other", 7);
    lib_setting_set(&L, "boxart", 0);
    CHECK_EQ(lib_setting_int(&L, "boxart", 1), 0);
    CHECK_EQ(lib_setting_int(&L, "other", 0), 7);

    /* nothing changed: no reload wanted; a package installed or removed, a ROM added: noticed */
    CHECK_EQ(lib_changed(&L), 0);
    sleep(1);   /* directory times have one-second resolution */
    put("root/apps/cavestory/cavestory.entry", "title=Cave Story\nexec=./nx\npspexp=1\n");
    CHECK_EQ(lib_changed(&L), 1);
    n = lib_reload(&L);
    CHECK_EQ(L.n, 5);
    CHECK_EQ(n, 5 + 3);   /* games and apps, GBA and SNES ROMs */
    CHECK_EQ(lib_changed(&L), 0);
    sleep(1);
    put("home/roms/gba/Metroid Fusion (USA).gba", "x");
    CHECK_EQ(lib_changed(&L), 1);
    lib_reload(&L);
    s = sys_index(&L, "gba");
    CHECK(s >= 0 && L.sys[s].nroms == 3);
    sleep(1);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s/root/apps/freedoom'", base);
    CHECK_EQ(system(cmd), 0);
    CHECK_EQ(lib_changed(&L), 1);
    lib_reload(&L);
    CHECK_EQ(L.n, 3);
    CHECK(lib_find(&L, "p:freedoom/phase1") == NULL);

    /* box art names from a thumbnails listing, matched whatever the ROM's naming */
    {
        static const char html[] =
            "<a href=\"../\">Parent</a><a href=\"Sonic%20The%20Hedgehog%20(Japan%2C%20Korea).png\">x</a>"
            "<a href=\"Sonic%20The%20Hedgehog%20(USA%2C%20Europe).png\">x</a>"
            "<a href=\"Sonic%20The%20Hedgehog%20(USA%2C%20Europe)%20(Beta).png\">x</a>"
            "<a href=\"Sonic%20The%20Hedgehog%202%20(World).png\">x</a>"
            "<a href=\"Legend%20of%20Zelda%2C%20The%20-%20Link's%20Awakening%20DX%20(USA%2C%20Europe)%20(Rev%202).png\">x</a>"
            "<a href=\"Street%20Fighter%20II'%20-%20Special%20Champion%20Edition%20(Europe).png\">x</a>"
            "<a href=\"Street%20Fighter%20II'%20-%20Special%20Champion%20Edition%20(USA).png\">x</a>"
            "<a href=\"Tom%20%26amp%3B%20Jerry.png\">x</a><a href=\"notes.txt\">x</a>"
            "<a href=\"Rayman%202%20(USA).png\">x</a><a href=\"Castlevania%20-%20Legends%20(USA).png\">x</a>";
        char **nm;
        int nn = lib_thumbs_parse(html, sizeof(html) - 1, &nm), k;
        CHECK_EQ(nn, 10);
        if (nn == 10) {
            CHECK(!strcmp(nm[1], "Sonic The Hedgehog (USA, Europe)"));
            /* GoodTools "(W) (REV01) [!]": the world-wide release, not the beta or the Japanese one */
            k = lib_thumbs_match(nm, nn, "Sonic The Hedgehog", "W, REV01, !");
            CHECK(k == 1);
            k = lib_thumbs_match(nm, nn, "Sonic The Hedgehog 2", "W, REV01, !");
            CHECK(k == 3);
            /* the article moved, "Link's", a revision: still the DX cover */
            k = lib_thumbs_match(nm, nn, "The Legend of Zelda - Link's Awakening DX", "U, V1.2, C, !");
            CHECK(k == 4);
            /* the ROM's region picks between otherwise equal covers */
            k = lib_thumbs_match(nm, nn, "Street Fighter II' - Special Champion Edition", "E, !");
            CHECK(k >= 0 && strstr(nm[k], "(Europe)") != NULL);
            k = lib_thumbs_match(nm, nn, "Street Fighter II' - Special Champion Edition", "U");
            CHECK(k >= 0 && strstr(nm[k], "(USA)") != NULL);
            CHECK_EQ(lib_thumbs_match(nm, nn, "Super Castlevania IV", ""), -1);
            /* a subtitle the collection does not have: the plain title */
            k = lib_thumbs_match(nm, nn, "Rayman 2 - The Great Escape", "E, M5, C, !");
            CHECK(k >= 0 && !strcmp(nm[k], "Rayman 2 (USA)"));
            /* but never another game of a series */
            CHECK_EQ(lib_thumbs_match(nm, nn, "Castlevania - Bloodlines", "U"), -1);
        }
        lib_thumbs_free(nm, nn);
    }

    lib_free(&L);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    if (system(cmd) != 0) fprintf(stderr, "cleanup failed\n");
    return test_summary("launcher_lib");
}
