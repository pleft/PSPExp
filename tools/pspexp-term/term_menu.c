/* The terminal's context menu: see term_menu.h */
#include "term_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char DEFAULTS[] =
    "[shell]\n"
    "Ctrl-C  stop = ^C\n"
    "Ctrl-Z  suspend = ^Z\n"
    "Ctrl-R  search history = ^R\n"
    "Ctrl-L  clear = ^L\n"
    "Ctrl-D  end input = ^D\n"
    "sudo = \"sudo \"\n"
    "cd .. = cd ..\\r\n"
    "ls -la = ls -la\\r\n"
    "| less = \" | less\"\n"
    "&& = \" && \"\n"
    "~/ = ~/\n"
    "Word left = \\eb\n"
    "Word right = \\ef\n"
    "[full]\n"
    "Esc = \\e\n"
    "vi: save = \\e:w\\r\n"
    "vi: quit = \\e:q\\r\n"
    "vi: save and quit = \\e:wq\\r\n"
    "vi: quit, no save = \\e:q!\\r\n"
    "Search / = /\n"
    "Next match = n\n"
    "Quit (less, man, top) = q\n"
    "Page up = \\e[5~\n"
    "Page down = \\e[6~\n"
    "[always]\n"
    "Scroll mode = @scroll-mode\n"
    "Scroll to top = @scroll-top\n"
    "Scroll to bottom = @scroll-bottom\n"
    "Keyboard on/off = @keyboard\n"
    "Theme... = @themes\n"
    "Font size... = @fonts\n"
    "Controls... = @keys\n";

int term_menu_decode(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    while (*s && o < cap) {
        char c = *s++;
        if (c == '^' && *s) {
            char k = *s++;
            if (k == '?') c = 127;
            else if (k >= 'a' && k <= 'z') c = (char)(k - 'a' + 1);
            else c = (char)(k & 0x1f);   /* ^@ ^[ ^\ ^] ^^ ^_ and capitals */
        } else if (c == '\\' && *s) {
            char k = *s++;
            c = k == 'e' ? 27 : k == 'r' ? '\r' : k == 'n' ? '\n' : k == 't' ? '\t' : k;
        }
        out[o++] = c;
    }
    return (int)o;
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

int term_menu_parse(TermMenu *m, const char *text)
{
    char line[256];
    int sec = -1, seen[TM_SECTIONS] = { 0 }, count = 0;
    const char *p = text;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        char *s, *eq, *k, *v;
        if (l >= sizeof(line)) l = sizeof(line) - 1;
        memcpy(line, p, l); line[l] = 0;
        p = nl ? nl + 1 : NULL;
        s = trim(line);
        if (!*s || *s == '#') continue;
        if (*s == '[') {
            sec = !strncmp(s, "[shell]", 7) ? TM_SHELL : !strncmp(s, "[full]", 6) ? TM_FULL : !strncmp(s, "[always]", 8) ? TM_ALWAYS : -1;
            if (sec >= 0 && !seen[sec]) { seen[sec] = 1; m->n[sec] = 0; }   /* the file's section replaces the default */
            continue;
        }
        if (sec < 0 || !(eq = strchr(s, '='))) continue;
        *eq = 0;
        k = trim(s); v = trim(eq + 1);
        if (!*k || !*v || m->n[sec] >= (int)(sizeof(m->item[0]) / sizeof(m->item[0][0]))) continue;
        {
            TermMenuItem *it = &m->item[sec][m->n[sec]];
            size_t vl = strlen(v);
            snprintf(it->label, sizeof(it->label), "%s", k);
            if (*v == '@') { it->kind = TM_ACTION; snprintf(it->arg, sizeof(it->arg), "%s", v + 1); it->len = 0; }
            else {
                if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') { v[vl - 1] = 0; v++; }
                it->kind = TM_KEYS;
                it->len = term_menu_decode(v, it->arg, sizeof(it->arg));
            }
            m->n[sec]++;
            count++;
        }
    }
    return count ? count : -1;
}

void term_menu_defaults(TermMenu *m)
{
    memset(m, 0, sizeof(*m));
    term_menu_parse(m, DEFAULTS);
}

void term_menu_load(TermMenu *m, const char *path)
{
    FILE *f;
    char *buf;
    long n;
    term_menu_defaults(m);
    if (!path || !(f = fopen(path, "rb"))) return;
    buf = malloc(16384);
    if (buf) {
        n = (long)fread(buf, 1, 16383, f);
        buf[n > 0 ? n : 0] = 0;
        term_menu_parse(m, buf);
        free(buf);
    }
    fclose(f);
}
