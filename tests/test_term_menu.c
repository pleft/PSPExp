/*
 * The terminal's context menu (tools/pspexp-term/term_menu.c): key strings,
 * the built-in sections, and a menu file replacing only what it names.
 */
#include "test_util.h"
#include "../tools/pspexp-term/term_menu.h"
#include <string.h>

int main(void)
{
    TermMenu m;
    char b[32];
    int n;

    CHECK_EQ(term_menu_decode("^C", b, sizeof(b)), 1); CHECK(b[0] == 3);
    CHECK_EQ(term_menu_decode("^@", b, sizeof(b)), 1); CHECK(b[0] == 0);
    CHECK_EQ(term_menu_decode("^?", b, sizeof(b)), 1); CHECK(b[0] == 127);
    n = term_menu_decode("\\e:wq\\r", b, sizeof(b));
    CHECK_EQ(n, 5); CHECK(b[0] == 27 && !memcmp(b + 1, ":wq", 3) && b[4] == '\r');
    CHECK_EQ(term_menu_decode("a\\\\b", b, sizeof(b)), 3); CHECK(!memcmp(b, "a\\b", 3));

    term_menu_defaults(&m);
    CHECK(m.n[TM_SHELL] >= 8 && m.n[TM_FULL] >= 8 && m.n[TM_ALWAYS] >= 3);
    CHECK(!strcmp(m.item[TM_SHELL][0].label, "Ctrl-C  stop") && m.item[TM_SHELL][0].len == 1 && m.item[TM_SHELL][0].arg[0] == 3);
    CHECK(m.item[TM_ALWAYS][0].kind == TM_ACTION && !strcmp(m.item[TM_ALWAYS][0].arg, "scroll-top"));
    {   /* quotes keep the spaces of "sudo " */
        int i, found = 0;
        for (i = 0; i < m.n[TM_SHELL]; i++)
            if (!strcmp(m.item[TM_SHELL][i].label, "sudo")) found = m.item[TM_SHELL][i].len == 5 && !memcmp(m.item[TM_SHELL][i].arg, "sudo ", 5);
        CHECK(found);
    }

    /* a file naming only [shell]: that section replaced, the others kept */
    n = term_menu_parse(&m, "# mine\n[shell]\nupdate = sudo apt update\\r\n  htop = htop\\r  \n[nonsense]\nx = y\n");
    CHECK_EQ(n, 2);
    CHECK_EQ(m.n[TM_SHELL], 2);
    CHECK(!strcmp(m.item[TM_SHELL][1].label, "htop") && m.item[TM_SHELL][1].len == 5);
    CHECK(m.n[TM_FULL] >= 8);
    CHECK_EQ(term_menu_parse(&m, "nothing here\n"), -1);
    return test_summary("term_menu");
}
