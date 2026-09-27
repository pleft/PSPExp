/* The launcher's library (see launcher_lib.h). */
#include "launcher_lib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- small helpers ------------------------------------------------------------ */

static void trim(char *s)
{
    size_t n = strlen(s), i = 0;
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, n - i + 1);
}

static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int ends_with(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcasecmp(s + a - b, suf); }

static void mkdir_p(const char *path)
{
    char p[512], *q;
    snprintf(p, sizeof(p), "%s", path);
    for (q = p + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(p, 0755); *q = '/'; }
    mkdir(p, 0755);
}

static int cmp_str(const void *a, const void *b) { return strcasecmp(*(const char *const *)a, *(const char *const *)b); }

static int list_dir(const char *dir, char ***names)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    int n = 0, cap = 0;
    *names = NULL;
    if (!d) return 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (n == cap) { cap = cap ? cap * 2 : 32; *names = (char **)realloc(*names, (size_t)cap * sizeof(char *)); }
        (*names)[n++] = strdup(de->d_name);
    }
    closedir(d);
    if (n) qsort(*names, (size_t)n, sizeof(char *), cmp_str);
    return n;
}

static void free_names(char **names, int n) { int i; for (i = 0; i < n; i++) free(names[i]); free(names); }

static void shell_quote(char *out, size_t n, const char *s)
{
    size_t o = 0;
    if (n < 3) { if (n) out[0] = 0; return; }
    out[o++] = '\'';
    for (; *s && o + 6 < n; s++) {
        if (*s == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
        else out[o++] = *s;
    }
    out[o++] = '\'';
    out[o] = 0;
}

/* "~/roms/snes" -> "$HOME/roms/snes" */
static void expand_home(char *p, size_t n)
{
    const char *home = getenv("HOME");
    char tmp[512];
    if (p[0] != '~' || (p[1] && p[1] != '/') || !home) return;
    snprintf(tmp, sizeof(tmp), "%s%s", home, p + 1);
    snprintf(p, n, "%s", tmp);
}

typedef void (*kv_fn)(void *u, const char *k, const char *v);
static int read_kv(const char *path, kv_fn fn, void *u)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        char *eq;
        trim(line);
        if (!line[0] || line[0] == '#' || !(eq = strchr(line, '='))) continue;
        *eq++ = 0;
        trim(line); trim(eq);
        fn(u, line, eq);
    }
    fclose(f);
    return 0;
}

/* ---- entries and systems ------------------------------------------------------------ */

static void entry_kv(void *u, const char *k, const char *v)
{
    LEntry *e = (LEntry *)u;
    if (!strcmp(k, "title")) snprintf(e->title, sizeof(e->title), "%s", v);
    else if (!strcmp(k, "type")) e->type = !strcmp(v, "app") ? LT_APP : !strcmp(v, "tool") ? LT_TOOL : LT_GAME;
    else if (!strcmp(k, "exec")) snprintf(e->exec, sizeof(e->exec), "%s", v);
    else if (!strcmp(k, "dir")) snprintf(e->dir, sizeof(e->dir), "%s", v);
    else if (!strcmp(k, "icon")) snprintf(e->icon, sizeof(e->icon), "%s", v);
    else if (!strcmp(k, "art")) snprintf(e->art, sizeof(e->art), "%s", v);
    else if (!strcmp(k, "tags")) snprintf(e->tags, sizeof(e->tags), "%s", v);
    else if (!strcmp(k, "description")) snprintf(e->desc, sizeof(e->desc), "%s", v);
    else if (!strcmp(k, "env") && strchr(v, '=') && strlen(e->env) + strlen(v) + 2 < sizeof(e->env)) {
        if (e->env[0]) strcat(e->env, " ");
        strcat(e->env, v);
    }
    else if (!strcmp(k, "pspexp")) e->pspexp = atoi(v);
    else if (!strcmp(k, "hidden") && atoi(v)) e->type = -1;
}

static void system_kv(void *u, const char *k, const char *v)
{
    LSystem *s = (LSystem *)u;
    if (!strcmp(k, "name")) snprintf(s->name, sizeof(s->name), "%s", v);
    else if (!strcmp(k, "short")) snprintf(s->shortname, sizeof(s->shortname), "%s", v);
    else if (!strcmp(k, "roms")) snprintf(s->roms, sizeof(s->roms), "%s", v);
    else if (!strcmp(k, "ext")) snprintf(s->ext, sizeof(s->ext), "%s", v);
    else if (!strcmp(k, "exec")) snprintf(s->exec, sizeof(s->exec), "%s", v);
    else if (!strcmp(k, "icon")) snprintf(s->icon, sizeof(s->icon), "%s", v);
    else if (!strcmp(k, "thumbs")) snprintf(s->thumbs, sizeof(s->thumbs), "%s", v);
    else if (!strcmp(k, "pkg") && !strchr(v, '/')) snprintf(s->pkg, sizeof(s->pkg), "%s", v);
    else if (!strcmp(k, "env") && strchr(v, '=') && strlen(s->env) + strlen(v) + 2 < sizeof(s->env)) {
        if (s->env[0]) strcat(s->env, " ");
        strcat(s->env, v);
    }
    else if (!strcmp(k, "pspexp")) s->pspexp = atoi(v);
}

static int cmp_entry(const void *a, const void *b) { return strcasecmp(((const LEntry *)a)->title, ((const LEntry *)b)->title); }
static int cmp_system(const void *a, const void *b) { return strcasecmp(((const LSystem *)a)->name, ((const LSystem *)b)->name); }

/* the .entry files of one directory; pkg = the package name or NULL */
static void load_entry_dir(Library *L, const char *dir, const char *pkg)
{
    char **names;
    int cnt = list_dir(dir, &names), i;
    for (i = 0; i < cnt; i++) {
        char p[900];
        LEntry e;
        int stem = (int)strlen(names[i]) - 6;
        if (!ends_with(names[i], ".entry")) continue;
        memset(&e, 0, sizeof(e));
        e.sys = -1;
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        if (read_kv(p, entry_kv, &e) < 0 || e.type < 0 || !e.exec[0]) continue;
        if (pkg) {
            snprintf(e.id, sizeof(e.id), "p:%s/%.*s", pkg, stem, names[i]);
            snprintf(e.pkg, sizeof(e.pkg), "%s", pkg);
            if (!e.dir[0]) snprintf(e.dir, sizeof(e.dir), "%s", dir);
            if (e.art[0] && e.art[0] != '/' && e.art[0] != '~') { char a[320]; snprintf(a, sizeof(a), "%s/%s", dir, e.art); snprintf(e.art, sizeof(e.art), "%s", a); }
            if (!e.art[0]) {   /* <stem>.png, else cover.png */
                struct stat st;
                snprintf(p, sizeof(p), "%s/%.*s.png", dir, stem, names[i]);
                if (stat(p, &st) != 0) snprintf(p, sizeof(p), "%s/cover.png", dir);
                if (stat(p, &st) == 0) snprintf(e.art, sizeof(e.art), "%s", p);
            }
        } else snprintf(e.id, sizeof(e.id), "e:%.*s", stem, names[i]);
        expand_home(e.dir, sizeof(e.dir));
        expand_home(e.art, sizeof(e.art));
        if (!e.title[0]) snprintf(e.title, sizeof(e.title), "%.*s", stem, names[i]);
        L->e = (LEntry *)realloc(L->e, (size_t)(L->n + 1) * sizeof(LEntry));
        L->e[L->n++] = e;
    }
    free_names(names, cnt);
}

static void load_entries(Library *L)
{
    char dir[600], **pkgs;
    int cnt, i;
    snprintf(dir, sizeof(dir), "%s/apps", L->root);
    cnt = list_dir(dir, &pkgs);
    for (i = 0; i < cnt; i++) {
        char p[900];
        snprintf(p, sizeof(p), "%s/%s", dir, pkgs[i]);
        if (is_dir(p)) load_entry_dir(L, p, pkgs[i]);
    }
    free_names(pkgs, cnt);
    snprintf(dir, sizeof(dir), "%s/library", L->home);
    load_entry_dir(L, dir, NULL);
    if (L->n) qsort(L->e, (size_t)L->n, sizeof(LEntry), cmp_entry);
}

static void load_system_dir(Library *L, const char *dir)
{
    char **names;
    int cnt = list_dir(dir, &names), i, k;
    for (i = 0; i < cnt && L->nsys < (int)(sizeof(L->sys) / sizeof(L->sys[0])); i++) {
        char p[900], id[48];
        LSystem *s = &L->sys[L->nsys];
        if (!ends_with(names[i], ".system")) continue;
        snprintf(id, sizeof(id), "%.*s", (int)(strlen(names[i]) - 7), names[i]);
        for (k = 0; k < L->nsys; k++) if (!strcmp(L->sys[k].id, id)) break;
        if (k < L->nsys) continue;   /* the first definition of a system wins (the user's) */
        memset(s, 0, sizeof(*s));
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        if (read_kv(p, system_kv, s) < 0 || !s->exec[0] || !s->roms[0]) continue;
        snprintf(s->id, sizeof(s->id), "%s", id);
        if (!s->name[0]) snprintf(s->name, sizeof(s->name), "%s", id);
        if (s->pkg[0]) snprintf(s->dir, sizeof(s->dir), "%s/apps/%s", L->root, s->pkg);
        expand_home(s->roms, sizeof(s->roms));
        if (!is_dir(s->roms)) mkdir_p(s->roms);   /* shows the user where ROMs go */
        L->nsys++;
    }
    free_names(names, cnt);
}

static void load_systems(Library *L)
{
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/systems", L->home);
    load_system_dir(L, dir);
    snprintf(dir, sizeof(dir), "%s/systems", L->root);
    load_system_dir(L, dir);
    if (L->nsys) qsort(L->sys, (size_t)L->nsys, sizeof(LSystem), cmp_system);
}

/* ---- ROMs ------------------------------------------------------------------------- */

static int has_ext(const char *file, const char *exts)
{
    const char *dot = strrchr(file, '.');
    char list[128], *tok, *save;
    if (!dot || !exts[0]) return exts[0] == 0;
    snprintf(list, sizeof(list), "%s", exts);
    for (tok = strtok_r(list, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save))
        if (!strcasecmp(dot + 1, tok)) return 1;
    return 0;
}

static void add_rom(LSystem *s, int sys, const char *dir, const char *file)
{
    char p[800], *paren;
    const char *dot;
    struct stat st;
    LEntry *e;
    snprintf(p, sizeof(p), "%s/%s", dir, file);
    if (stat(p, &st) != 0) return;
    s->roms_list = (LEntry *)realloc(s->roms_list, (size_t)(s->nroms + 1) * sizeof(LEntry));
    e = &s->roms_list[s->nroms++];
    memset(e, 0, sizeof(*e));
    e->type = LT_ROM; e->sys = sys; e->size = (long long)st.st_size; e->pspexp = s->pspexp;
    snprintf(e->id, sizeof(e->id), "r:%s:%s", s->id, p);
    snprintf(e->exec, sizeof(e->exec), "%s", p);   /* the ROM path; lib_command builds the command */
    snprintf(e->dir, sizeof(e->dir), "%s", s->dir[0] ? s->dir : dir);
    snprintf(e->env, sizeof(e->env), "%s", s->env);
    dot = strrchr(file, '.');
    snprintf(e->title, sizeof(e->title), "%.*s", dot ? (int)(dot - file) : (int)strlen(file), file);
    /* "Super Mario World (USA) (Rev 1)" -> title "Super Mario World", tags "USA, Rev 1" */
    if ((paren = strstr(e->title, " (")) && paren != e->title) {
        char *t = paren + 1, *o = e->tags;
        size_t room = sizeof(e->tags) - 1;
        for (; *t && room > 2; t++) {
            if (*t == '(' || *t == '[') continue;
            if (*t == ')' || *t == ']') { if (t[1] && room > 2) { *o++ = ','; *o++ = ' '; room -= 2; while (t[1] == ' ') t++; } continue; }
            *o++ = *t; room--;
        }
        *o = 0;
        { size_t l = strlen(e->tags); while (l && (e->tags[l - 1] == ' ' || e->tags[l - 1] == ',')) e->tags[--l] = 0; }
        *paren = 0;
    }
    /* No-Intro sorts articles last: "Legend of Zelda, The - The Minish Cap" -> "The Legend of Zelda - The Minish Cap" */
    {
        static const char *arts[] = { ", The", ", A", ", An", NULL };
        int k;
        for (k = 0; arts[k]; k++) {
            char *hit = strstr(e->title, arts[k]);
            size_t al = strlen(arts[k]);
            if (hit && (hit[al] == 0 || !strncmp(hit + al, " - ", 3))) {
                char head[96], rest[96];
                snprintf(head, sizeof(head), "%.*s", (int)(hit - e->title), e->title);
                snprintf(rest, sizeof(rest), "%s", hit + al);
                snprintf(e->title, sizeof(e->title), "%s %s%s", arts[k] + 2, head, rest);
                break;
            }
        }
    }
}

void lib_scan_roms(Library *L, int sys)
{
    LSystem *s = &L->sys[sys];
    char **names;
    int cnt, i, k;
    free(s->roms_list); s->roms_list = NULL; s->nroms = 0;
    cnt = list_dir(s->roms, &names);
    for (i = 0; i < cnt; i++) {
        char p[800];
        snprintf(p, sizeof(p), "%s/%s", s->roms, names[i]);
        if (is_dir(p)) {   /* one level down: multi-file games (cue/bin) */
            char **sub; int ns = list_dir(p, &sub);
            for (k = 0; k < ns; k++) if (has_ext(sub[k], s->ext)) add_rom(s, sys, p, sub[k]);
            free_names(sub, ns);
        } else if (has_ext(names[i], s->ext)) add_rom(s, sys, s->roms, names[i]);
    }
    free_names(names, cnt);
    /* a .cue or .m3u stands for its tracks: hide .bin / .img of the same name */
    for (i = 0; i < s->nroms; i++) {
        const char *f = s->roms_list[i].exec, *dot = strrchr(f, '.');
        if (!dot || !(!strcasecmp(dot, ".bin") || !strcasecmp(dot, ".img"))) continue;
        for (k = 0; k < s->nroms; k++) {
            const char *g = s->roms_list[k].exec, *gd = strrchr(g, '.');
            if (k != i && gd && (gd - g) == (dot - f) && !strncmp(f, g, (size_t)(dot - f)) && (!strcasecmp(gd, ".cue") || !strcasecmp(gd, ".m3u"))) {
                s->roms_list[i] = s->roms_list[--s->nroms]; i--; break;
            }
        }
    }
    if (s->nroms) qsort(s->roms_list, (size_t)s->nroms, sizeof(LEntry), cmp_entry);
    for (i = 0; i < s->nroms; i++) s->roms_list[i].fav = lib_is_fav(L, s->roms_list[i].id);
    s->scanned = 1;
}

/* ---- recent and favourites -------------------------------------------------------- */

static int valid_id(const char *s) { return (s[0] == 'e' || s[0] == 'r' || s[0] == 'p') && s[1] == ':'; }

static void list_load(Library *L, const char *file, char (*arr)[200], int *n, int max)
{
    char p[700], line[240];
    FILE *f;
    snprintf(p, sizeof(p), "%s/%s", L->home, file);
    *n = 0;
    if (!(f = fopen(p, "r"))) return;
    while (*n < max && fgets(line, sizeof(line), f)) { trim(line); if (valid_id(line)) snprintf(arr[(*n)++], 200, "%s", line); }
    fclose(f);
}

static void list_save(Library *L, const char *file, char (*arr)[200], int n)
{
    char p[700];
    FILE *f;
    int i;
    mkdir_p(L->home);
    snprintf(p, sizeof(p), "%s/%s", L->home, file);
    if (!(f = fopen(p, "w"))) return;
    for (i = 0; i < n; i++) fprintf(f, "%s\n", arr[i]);
    fclose(f);
}

int lib_is_fav(const Library *L, const char *id) { int i; for (i = 0; i < L->nfavs; i++) if (!strcmp(L->favs[i], id)) return 1; return 0; }

void lib_recent_push(Library *L, const char *id)
{
    int i, at = -1, max = (int)(sizeof(L->recent) / sizeof(L->recent[0]));
    for (i = 0; i < L->nrecent; i++) if (!strcmp(L->recent[i], id)) at = i;
    if (at < 0) { if (L->nrecent < max) L->nrecent++; at = L->nrecent - 1; }
    for (i = at; i > 0; i--) memcpy(L->recent[i], L->recent[i - 1], 200);
    snprintf(L->recent[0], 200, "%s", id);
    list_save(L, "recent", L->recent, L->nrecent);
}

int lib_fav_toggle(Library *L, const char *id)
{
    int i, on;
    LEntry *e;
    for (i = 0; i < L->nfavs; i++) if (!strcmp(L->favs[i], id)) break;
    if (i < L->nfavs) { memmove(L->favs[i], L->favs[i + 1], (size_t)(L->nfavs - i - 1) * 200); L->nfavs--; on = 0; }
    else if (L->nfavs < (int)(sizeof(L->favs) / sizeof(L->favs[0]))) { snprintf(L->favs[L->nfavs++], 200, "%s", id); on = 1; }
    else return 0;
    list_save(L, "favourites", L->favs, L->nfavs);
    if ((e = lib_find(L, id))) e->fav = on;
    return on;
}

/* ---- lookup and commands ------------------------------------------------------------ */

LEntry *lib_find(Library *L, const char *id)
{
    int i, s;
    if (id[0] == 'e' || id[0] == 'p') { for (i = 0; i < L->n; i++) if (!strcmp(L->e[i].id, id)) return &L->e[i]; return NULL; }
    if (!strncmp(id, "r:", 2)) {
        const char *sid = id + 2, *colon = strchr(sid, ':');
        if (!colon) return NULL;
        for (s = 0; s < L->nsys; s++) {
            if (strlen(L->sys[s].id) != (size_t)(colon - sid) || strncmp(L->sys[s].id, sid, (size_t)(colon - sid))) continue;
            if (!L->sys[s].scanned) lib_scan_roms(L, s);
            for (i = 0; i < L->sys[s].nroms; i++) if (!strcmp(L->sys[s].roms_list[i].id, id)) return &L->sys[s].roms_list[i];
        }
    }
    return NULL;
}

void lib_command(const Library *L, const LEntry *e, char *out, size_t n)
{
    size_t o = 0;
    out[0] = 0;
    if (e->env[0]) {   /* NAME='value' ... command */
        char env[400], *tok, *save;
        snprintf(env, sizeof(env), "%s", e->env);
        for (tok = strtok_r(env, " ", &save); tok && o + 8 < n; tok = strtok_r(NULL, " ", &save)) {
            char *eq = strchr(tok, '='), q[300];
            if (!eq) continue;
            *eq = 0;
            shell_quote(q, sizeof(q), eq + 1);
            o += (size_t)snprintf(out + o, n - o, "%s=%s ", tok, q);
        }
    }
    if (o >= n) return;
    if (e->type == LT_ROM && e->sys >= 0) {
        const char *x = L->sys[e->sys].exec, *hit = strstr(x, "%ROM%");
        char q[700];
        shell_quote(q, sizeof(q), e->exec);
        if (hit) snprintf(out + o, n - o, "exec %.*s%s%s", (int)(hit - x), x, q, hit + 5);
        else snprintf(out + o, n - o, "exec %s %s", x, q);
    } else snprintf(out + o, n - o, "%s", e->exec);
}

/* ---- box art names --------------------------------------------------------------------- */

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

int lib_thumbs_parse(const char *html, size_t len, char ***names)
{
    const char *p = html, *end = html + len;
    int n = 0, cap = 0;
    char **v = NULL;
    *names = NULL;
    while (p < end) {
        const char *h = NULL, *q, *png;
        const char *s;
        for (s = p; s + 6 < end; s++) if (!memcmp(s, "href=\"", 6)) { h = s + 6; break; }
        if (!h) break;
        q = memchr(h, '"', (size_t)(end - h));
        if (!q) break;
        p = q + 1;
        png = q - 4;
        if (q - h < 5 || memcmp(png, ".png", 4)) continue;
        if (n == cap) { char **nv = (char **)realloc(v, (size_t)(cap ? cap * 2 : 1024) * sizeof(char *)); if (!nv) break; v = nv; cap = cap ? cap * 2 : 1024; }
        {
            char *o = (char *)malloc((size_t)(png - h) + 1), *w = o;
            const char *r;
            if (!o) break;
            for (r = h; r < png; r++) {
                if (*r == '%' && r + 2 < png && hexval(r[1]) >= 0 && hexval(r[2]) >= 0) { *w++ = (char)(hexval(r[1]) * 16 + hexval(r[2])); r += 2; }
                else if (*r == '&' && !strncmp(r, "&amp;", 5)) { *w++ = '&'; r += 4; }
                else *w++ = *r;
            }
            *w = 0;
            v[n++] = o;
        }
    }
    *names = v;
    return n;
}

void lib_thumbs_free(char **names, int n)
{
    int i;
    for (i = 0; i < n; i++) free(names[i]);
    free(names);
}

/* the title part of a name, as a comparison key: lower case letters and
 * digits only, without "the" (No-Intro moves it: "Legend of Zelda, The") */
static void title_key(const char *name, char *out, size_t n)
{
    char word[64];
    size_t o = 0, w = 0;
    const char *s;
    for (s = name; ; s++) {
        int c = (unsigned char)*s;
        int end = !c || c == '(' || c == '[';
        if (isalnum(c)) { if (w + 1 < sizeof(word)) word[w++] = (char)tolower(c); continue; }
        if (c == '\'') continue;   /* "Link's" = "Links" */
        /* a word ends */
        word[w] = 0;
        if (w && strcmp(word, "the") && o + w < n) { memcpy(out + o, word, w); o += w; }
        w = 0;
        if (end) break;
    }
    out[o] = 0;
}

/* the regions a ROM's tags ask for: GoodTools letters or No-Intro words */
static void wanted_regions(const char *tags, int *usa, int *eur, int *jpn)
{
    char t[128], *tok, *save;
    *usa = *eur = *jpn = 0;
    snprintf(t, sizeof(t), "%s", tags ? tags : "");
    for (tok = strtok_r(t, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
        if (!strcmp(tok, "U") || !strcmp(tok, "UE") || !strcmp(tok, "JU") || !strcasecmp(tok, "USA") || !strcmp(tok, "W") || !strcasecmp(tok, "World")) *usa = 1;
        if (!strcmp(tok, "E") || !strcmp(tok, "UE") || !strcmp(tok, "JE") || !strcasecmp(tok, "Europe") || !strcmp(tok, "W") || !strcasecmp(tok, "World")) *eur = 1;
        if (!strcmp(tok, "J") || !strcmp(tok, "JU") || !strcmp(tok, "JE") || !strcasecmp(tok, "Japan")) *jpn = 1;
    }
}

/* the title without its subtitle ("Rayman 2 - The Great Escape" -> "Rayman 2") */
static void main_title(const char *title, char *out, size_t n)
{
    const char *dash = strstr(title, " - ");
    snprintf(out, n, "%.*s", dash ? (int)(dash - title) : (int)strlen(title), title);
}

static int match_pass(char *const *names, int n, const char *want, int usa, int eur, int jpn, int loose)
{
    char key[256], mt[256];
    int i, best = -1, best_score = -1000000;
    for (i = 0; i < n; i++) {
        const char *nm = names[i], *paren;
        int score = 0, extra = 0;
        if (loose) {   /* only covers without a subtitle of their own */
            main_title(nm, mt, sizeof(mt));
            paren = strchr(nm, '(');
            if (strlen(mt) < (paren ? (size_t)(paren - nm) - 1 : strlen(nm))) continue;
        }
        title_key(nm, key, sizeof(key));
        if (strcmp(key, want)) continue;
        paren = strchr(nm, '(');
        if (paren) {
            if ((usa && strstr(paren, "USA")) || (eur && strstr(paren, "Europe")) || (jpn && strstr(paren, "Japan"))) score += 100;
            if (strstr(paren, "World")) score += (usa || eur) ? 90 : 20;
            if (!usa && !eur && !jpn && (strstr(paren, "USA") || strstr(paren, "Europe"))) score += 50;   /* no region asked: the common ones */
            if (strstr(paren, "Beta") || strstr(paren, "Proto") || strstr(paren, "Demo") || strstr(paren, "Sample") || strstr(paren, "Pirate") || strstr(paren, "Unl")) score -= 200;
            { const char *c; for (c = paren; *c; c++) if (*c == '(') extra++; }
        }
        score -= extra * 5;
        score -= (int)strlen(nm) / 10;
        if (score > best_score) { best_score = score; best = i; }
    }
    return best;
}

int lib_thumbs_match(char *const *names, int n, const char *title, const char *tags)
{
    char want[256], mt[256];
    int usa, eur, jpn, k;
    title_key(title, want, sizeof(want));
    if (!want[0]) return -1;
    wanted_regions(tags, &usa, &eur, &jpn);
    k = match_pass(names, n, want, usa, eur, jpn, 0);
    if (k >= 0) return k;
    /* the ROM's title without its subtitle, against covers that have none:
     * "Rayman 2 - The Great Escape" finds "Rayman 2 (USA)", while two games
     * of a series ("Zelda - A", "Zelda - B") are never taken for each other */
    main_title(title, mt, sizeof(mt));
    if (!strcmp(mt, title)) return -1;
    title_key(mt, want, sizeof(want));
    return want[0] ? match_pass(names, n, want, usa, eur, jpn, 1) : -1;
}

/* ---- settings ---------------------------------------------------------------------- */

typedef struct SetQ { const char *key; int val, found; } SetQ;
static void set_kv(void *u, const char *k, const char *v) { SetQ *q = (SetQ *)u; if (!strcmp(k, q->key)) { q->val = atoi(v); q->found = 1; } }

int lib_setting_int(const Library *L, const char *key, int def)
{
    char p[600];
    SetQ q;
    q.key = key; q.val = def; q.found = 0;
    snprintf(p, sizeof(p), "%s/settings", L->home);
    read_kv(p, set_kv, &q);
    return q.found ? q.val : def;
}

void lib_setting_set(const Library *L, const char *key, int value)
{
    char p[600], tmp[620], line[512];
    FILE *in, *out;
    int done = 0;
    mkdir_p(L->home);
    snprintf(p, sizeof(p), "%s/settings", L->home);
    snprintf(tmp, sizeof(tmp), "%s.new", p);
    if (!(out = fopen(tmp, "w"))) return;
    if ((in = fopen(p, "r"))) {
        while (fgets(line, sizeof(line), in)) {
            char k[128];
            if (sscanf(line, " %127[^= ] =", k) == 1 && !strcmp(k, key)) { if (!done) fprintf(out, "%s=%d\n", key, value); done = 1; }
            else fputs(line, out);
        }
        fclose(in);
    }
    if (!done) fprintf(out, "%s=%d\n", key, value);
    fclose(out);
    rename(tmp, p);
}

/* ---- change detection -------------------------------------------------------------- */

static unsigned long long mix(unsigned long long h, unsigned long long v) { return (h ^ v) * 1099511628211ull; }

static unsigned long long dir_sig(const char *p)
{
    struct stat st;
    if (stat(p, &st) != 0) return 1;
    return (unsigned long long)st.st_mtime * 1000003ull + (unsigned long long)st.st_ino;
}

/* the modification times of every directory whose contents the library shows */
static unsigned long long signature(Library *L)
{
    char p[900], **names;
    unsigned long long h = 1469598103934665603ull;
    int cnt, i;
    snprintf(p, sizeof(p), "%s/apps", L->root); h = mix(h, dir_sig(p));
    cnt = list_dir(p, &names);
    for (i = 0; i < cnt; i++) { char q[1000]; snprintf(q, sizeof(q), "%s/%s", p, names[i]); h = mix(h, dir_sig(q)); }
    free_names(names, cnt);
    snprintf(p, sizeof(p), "%s/systems", L->root); h = mix(h, dir_sig(p));
    snprintf(p, sizeof(p), "%s/library", L->home); h = mix(h, dir_sig(p));
    snprintf(p, sizeof(p), "%s/systems", L->home); h = mix(h, dir_sig(p));
    for (i = 0; i < L->nsys; i++) h = mix(h, dir_sig(L->sys[i].roms));
    return h;
}

int lib_changed(Library *L) { return signature(L) != L->sig; }

/* ---- load ------------------------------------------------------------------------ */

int lib_reload(Library *L)
{
    int i, total;
    for (i = 0; i < L->nsys; i++) free(L->sys[i].roms_list);
    free(L->e); L->e = NULL; L->n = 0; L->nsys = 0;
    list_load(L, "recent", L->recent, &L->nrecent, (int)(sizeof(L->recent) / sizeof(L->recent[0])));
    list_load(L, "favourites", L->favs, &L->nfavs, (int)(sizeof(L->favs) / sizeof(L->favs[0])));
    load_entries(L);
    load_systems(L);
    for (i = 0; i < L->n; i++) L->e[i].fav = lib_is_fav(L, L->e[i].id);
    total = L->n;
    for (i = 0; i < L->nsys; i++) { lib_scan_roms(L, i); total += L->sys[i].nroms; }
    L->sig = signature(L);
    return total;
}

int lib_load(Library *L)
{
    const char *h = getenv("PSPEXP_LAUNCHER_HOME"), *home = getenv("HOME"), *root = getenv("PSPEXP_ROOT");
    memset(L, 0, sizeof(*L));
    if (h && h[0]) snprintf(L->home, sizeof(L->home), "%s", h);
    else snprintf(L->home, sizeof(L->home), "%s/.config/pspexp-launcher", home ? home : "/tmp");
    snprintf(L->root, sizeof(L->root), "%s", root && root[0] ? root : "/opt/pspexp");
    lib_reload(L);
    return 0;
}

void lib_free(Library *L)
{
    int i;
    for (i = 0; i < L->nsys; i++) free(L->sys[i].roms_list);
    free(L->e);
    memset(L, 0, sizeof(*L));
}
