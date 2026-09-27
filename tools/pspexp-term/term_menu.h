/*
 * The terminal's context menu (a tap of L): the items for the shell or for a
 * full-screen program (vi, less, top: the alternate screen), then the ones
 * always there. Built-in defaults, or ~/.config/pspexp-term/menu:
 *
 *   # comment
 *   [shell]                     at the prompt
 *   Ctrl-C = ^C                 keys: ^X control, \e Esc, \r Enter, \t Tab, \\
 *   sudo = "sudo "              quotes keep spaces at the ends
 *   [full]                      in vi, less, man, top...
 *   Save and quit = \e:wq\r
 *   [always]
 *   Scroll to top = @scroll-top  @name: one of the terminal's own actions
 *
 * The file replaces a section's defaults only for the sections it names.
 */
#ifndef PSPEXP_TERM_MENU_H
#define PSPEXP_TERM_MENU_H

#include <stddef.h>

enum { TM_SHELL, TM_FULL, TM_ALWAYS, TM_SECTIONS };
enum { TM_KEYS, TM_ACTION };

typedef struct TermMenuItem {
    char label[40];
    char arg[96];       /* TM_KEYS: the bytes to type (decoded); TM_ACTION: the action's name */
    int kind, len;      /* len: bytes in arg for TM_KEYS (may hold a NUL: ^@) */
} TermMenuItem;

typedef struct TermMenu {
    TermMenuItem item[TM_SECTIONS][32];
    int n[TM_SECTIONS];
} TermMenu;

/* the built-in menu */
void term_menu_defaults(TermMenu *m);
/* a menu file's text over m: each section it names replaces that section;
 * returns the number of items read, -1 on nothing usable */
int  term_menu_parse(TermMenu *m, const char *text);
/* the defaults, then path if it can be read */
void term_menu_load(TermMenu *m, const char *path);
/* "^C", "\e:wq\r"... -> bytes; returns the length */
int  term_menu_decode(const char *s, char *out, size_t cap);

#endif
