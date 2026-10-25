/*
 * Settings > Recording > PSPExp plugin's edit of PRO's game.txt (tools/pspexp-term/psp_plugin.c):
 * the plugin's line set on or off, added when missing, every other plugin's line and the file's
 * line ends kept as they were.
 */
#include <stdio.h>
#include <string.h>
#include "../tools/pspexp-term/psp_plugin.h"

static int g_fail;
static void check(int ok, const char *what) { printf("  %s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) g_fail = 1; }

#define P "ms0:/PSP/GAME/PSPExp/plugin/pspexp_plugin.prx"

static void edit(const char *in, int on, const char *want, const char *what)
{
    char out[512];
    int n = psp_plugin_game_txt(in, P, on, out, sizeof(out));
    int ok = want ? n == (int)strlen(want) && !strcmp(out, want) : n == 0;
    if (!ok) printf("    got %d: [%s]\n    want: [%s]\n", n, n > 0 ? out : "", want ? want : "(nothing written)");
    check(ok, what);
}

int main(void)
{
    char small[16];
    printf("game.txt read:\n");
    check(psp_plugin_listed(P " 1\n"), "the plugin's line ending in 1: on");
    check(!psp_plugin_listed(P " 0\n"), "ending in 0: off");
    check(psp_plugin_listed("ms0:/seplugins/cxmb.prx 1\r\n" P " 1\r\n"), "with CRLF and another plugin before it");
    check(psp_plugin_listed(P "\t1   \n"), "a tab and trailing blanks");
    check(!psp_plugin_listed("ms0:/seplugins/other.prx 1\n"), "another plugin on: not this one");
    check(!psp_plugin_listed(""), "an empty file: off");
    check(psp_plugin_listed(P " 1"), "the last line without its end");

    printf("game.txt written:\n");
    edit("", 1, P " 1\n", "none or empty, on: the line added");
    edit("", 0, NULL, "none, off: nothing written");
    edit(P " 0\n", 1, P " 1\n", "off to on");
    edit(P " 1\n", 0, P " 0\n", "on to off");
    edit("ms0:/seplugins/a.prx 1\r\nms0:/seplugins/b.prx 0\r\n", 1,
         "ms0:/seplugins/a.prx 1\r\nms0:/seplugins/b.prx 0\r\n" P " 1\r\n", "added after the others, with their CRLF");
    edit("ms0:/seplugins/a.prx 1", 1, "ms0:/seplugins/a.prx 1\n" P " 1\n", "a last line without its end: one added first");
    edit("ms0:/seplugins/a.prx 1\r\n" P " 0\r\nms0:/seplugins/b.prx 1\r\n", 1,
         "ms0:/seplugins/a.prx 1\r\n" P " 1\r\nms0:/seplugins/b.prx 1\r\n", "in the middle, its place and the others kept");
    edit("ms0:/PSP/GAME/Old/pspexp_plugin.prx 1\n" P " 1\n", 0,
         "ms0:/PSP/GAME/Old/pspexp_plugin.prx 0\n" P " 0\n", "off: every line naming it (an old path too)");
    edit(P "\t1  \n", 0, P " 0\n", "its blanks tidied, the line end kept");
    edit(P " 1", 0, P " 0", "the last line without its end: still none");
    check(psp_plugin_game_txt("", P, 1, small, sizeof(small)) == -1, "too small an output: -1");

    printf(g_fail ? "FAILED\n" : "all passed\n");
    return g_fail;
}
