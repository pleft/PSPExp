/*
 * glsl-pspexp: see glsl_pspexp.h. GLSL 1.10-1.30 and ES 1.00 as games write
 * them: the preprocessor's object-like macros and conditionals, functions
 * (inlined), constant loops (unrolled), uniform branches (taken). Structs,
 * interface blocks and function-like macros are not read: such a shader
 * gets the nearest plan (glsl_plan), its draws the texture and colour.
 */
#include "glsl_pspexp.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- memory: everything of a shader in one arena ---------------------------------------- */

typedef struct Arena { struct Chunk *chunks; } Arena;
struct Chunk { struct Chunk *next; size_t used, cap; unsigned char data[]; };

static void *amalloc(Arena *a, size_t n)
{
    struct Chunk *c = a->chunks;
    n = (n + 15) & ~(size_t)15;
    if (!c || c->used + n > c->cap) {
        size_t cap = n > 65536 ? n : 65536;
        c = (struct Chunk *)malloc(sizeof(*c) + cap);
        if (!c) return NULL;
        c->next = a->chunks; c->used = 0; c->cap = cap;
        a->chunks = c;
    }
    c->used += n;
    memset(c->data + c->used - n, 0, n);
    return c->data + c->used - n;
}

static void afree(Arena *a)
{
    while (a->chunks) { struct Chunk *n = a->chunks->next; free(a->chunks); a->chunks = n; }
}

static char *astrdup(Arena *a, const char *s, size_t n)
{
    char *d = (char *)amalloc(a, n + 1);
    if (d) { memcpy(d, s, n); d[n] = 0; }
    return d;
}

/* ---- types ---------------------------------------------------------------------------------- */

enum { T_VOID, T_FLOAT, T_INT, T_BOOL, T_SAMPLER };

typedef struct Type {
    unsigned char base;     /* T_* */
    unsigned char rows;     /* vector size, or matrix rows */
    unsigned char cols;     /* 1, or matrix columns */
    short array;            /* 0: not an array */
} Type;


static int parse_type_name(const char *s, Type *t)
{
    static const struct { const char *n; unsigned char b, r, c; } T[] = {
        { "void", T_VOID, 0, 0 }, { "float", T_FLOAT, 1, 1 }, { "int", T_INT, 1, 1 }, { "uint", T_INT, 1, 1 },
        { "bool", T_BOOL, 1, 1 },
        { "vec2", T_FLOAT, 2, 1 }, { "vec3", T_FLOAT, 3, 1 }, { "vec4", T_FLOAT, 4, 1 },
        { "ivec2", T_INT, 2, 1 }, { "ivec3", T_INT, 3, 1 }, { "ivec4", T_INT, 4, 1 },
        { "uvec2", T_INT, 2, 1 }, { "uvec3", T_INT, 3, 1 }, { "uvec4", T_INT, 4, 1 },
        { "bvec2", T_BOOL, 2, 1 }, { "bvec3", T_BOOL, 3, 1 }, { "bvec4", T_BOOL, 4, 1 },
        { "mat2", T_FLOAT, 2, 2 }, { "mat3", T_FLOAT, 3, 3 }, { "mat4", T_FLOAT, 4, 4 },
        { "mat2x2", T_FLOAT, 2, 2 }, { "mat3x3", T_FLOAT, 3, 3 }, { "mat4x4", T_FLOAT, 4, 4 },
        { "mat2x3", T_FLOAT, 3, 2 }, { "mat2x4", T_FLOAT, 4, 2 }, { "mat3x2", T_FLOAT, 2, 3 },
        { "mat3x4", T_FLOAT, 4, 3 }, { "mat4x2", T_FLOAT, 2, 4 }, { "mat4x3", T_FLOAT, 3, 4 },
        { "sampler2D", T_SAMPLER, 1, 1 }, { "samplerCube", T_SAMPLER, 1, 1 }, { "sampler2DArray", T_SAMPLER, 1, 1 },
        { "sampler2DShadow", T_SAMPLER, 1, 1 }, { "sampler1D", T_SAMPLER, 1, 1 }, { "sampler3D", T_SAMPLER, 1, 1 },
        { "sampler2DRect", T_SAMPLER, 1, 1 }, { "samplerExternalOES", T_SAMPLER, 1, 1 },
    };
    size_t i;
    for (i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (!strcmp(s, T[i].n)) { t->base = T[i].b; t->rows = T[i].r; t->cols = T[i].c; t->array = 0; return 1; }
    return 0;
}

/* ---- preprocessor --------------------------------------------------------------------------- */

typedef struct Macro { char name[48]; char *value; } Macro;

typedef struct Pre {
    Macro m[128]; int nm;
    char *out; size_t len, cap;
    char *err;
} Pre;

static void out_add(Pre *p, const char *s, size_t n)
{
    if (p->len + n + 1 > p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 4096;
        char *o;
        while (cap < p->len + n + 1) cap *= 2;
        if (!(o = (char *)realloc(p->out, cap))) return;
        p->out = o; p->cap = cap;
    }
    memcpy(p->out + p->len, s, n);
    p->len += n;
    p->out[p->len] = 0;
}

static Macro *find_macro(Pre *p, const char *name, size_t n)
{
    int i;
    for (i = 0; i < p->nm; i++) if (strlen(p->m[i].name) == n && !strncmp(p->m[i].name, name, n)) return &p->m[i];
    return NULL;
}

/* A line with macros replaced (depth-limited). */
static void expand(Pre *p, const char *s, size_t n, int depth)
{
    size_t i = 0;
    while (i < n) {
        if (isalpha((unsigned char)s[i]) || s[i] == '_') {
            size_t j = i;
            Macro *m;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            if (depth < 8 && (m = find_macro(p, s + i, j - i))) expand(p, m->value, strlen(m->value), depth + 1);
            else out_add(p, s + i, j - i);
            i = j;
        } else if (isdigit((unsigned char)s[i])) {   /* numbers are not macro names (1e5) */
            size_t j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' || ((s[j] == '+' || s[j] == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E')))) j++;
            out_add(p, s + i, j - i);
            i = j;
        } else { out_add(p, s + i, 1); i++; }
    }
}

/* #if expressions: integers, defined(), ! && || == != < > <= >= + - * / ( ) */
typedef struct PExpr { const char *s; Pre *p; } PExpr;
static long pe_or(PExpr *e);
static void pe_ws(PExpr *e) { while (*e->s == ' ' || *e->s == '\t') e->s++; }
static long pe_primary(PExpr *e)
{
    pe_ws(e);
    if (*e->s == '(') { long v; e->s++; v = pe_or(e); pe_ws(e); if (*e->s == ')') e->s++; return v; }
    if (*e->s == '!') { e->s++; return !pe_primary(e); }
    if (*e->s == '-') { e->s++; return -pe_primary(e); }
    if (isdigit((unsigned char)*e->s)) return strtol(e->s, (char **)&e->s, 0);
    if (isalpha((unsigned char)*e->s) || *e->s == '_') {
        const char *st = e->s;
        size_t n;
        Macro *m;
        while (isalnum((unsigned char)*e->s) || *e->s == '_') e->s++;
        n = (size_t)(e->s - st);
        if (n == 7 && !strncmp(st, "defined", 7)) {
            int par;
            pe_ws(e);
            par = *e->s == '(';
            if (par) e->s++;
            pe_ws(e);
            st = e->s;
            while (isalnum((unsigned char)*e->s) || *e->s == '_') e->s++;
            n = (size_t)(e->s - st);
            pe_ws(e);
            if (par && *e->s == ')') e->s++;
            return find_macro(e->p, st, n) != NULL;
        }
        if ((m = find_macro(e->p, st, n))) return strtol(m->value, NULL, 0);
        return 0;
    }
    return 0;
}
static long pe_mul(PExpr *e) { long v = pe_primary(e); for (;;) { pe_ws(e); if (*e->s == '*') { e->s++; v *= pe_primary(e); } else if (*e->s == '/') { long d; e->s++; d = pe_primary(e); v = d ? v / d : 0; } else return v; } }
static long pe_add(PExpr *e) { long v = pe_mul(e); for (;;) { pe_ws(e); if (*e->s == '+') { e->s++; v += pe_mul(e); } else if (*e->s == '-') { e->s++; v -= pe_mul(e); } else return v; } }
static long pe_rel(PExpr *e)
{
    long v = pe_add(e);
    for (;;) {
        pe_ws(e);
        if (!strncmp(e->s, "<=", 2)) { e->s += 2; v = v <= pe_add(e); }
        else if (!strncmp(e->s, ">=", 2)) { e->s += 2; v = v >= pe_add(e); }
        else if (*e->s == '<') { e->s++; v = v < pe_add(e); }
        else if (*e->s == '>') { e->s++; v = v > pe_add(e); }
        else return v;
    }
}
static long pe_eq(PExpr *e)
{
    long v = pe_rel(e);
    for (;;) {
        pe_ws(e);
        if (!strncmp(e->s, "==", 2)) { e->s += 2; v = v == pe_rel(e); }
        else if (!strncmp(e->s, "!=", 2)) { e->s += 2; v = v != pe_rel(e); }
        else return v;
    }
}
static long pe_and(PExpr *e) { long v = pe_eq(e); for (;;) { pe_ws(e); if (!strncmp(e->s, "&&", 2)) { long r; e->s += 2; r = pe_eq(e); v = v && r; } else return v; } }
static long pe_or(PExpr *e) { long v = pe_and(e); for (;;) { pe_ws(e); if (!strncmp(e->s, "||", 2)) { long r; e->s += 2; r = pe_and(e); v = v || r; } else return v; } }

static void add_macro(Pre *p, const char *name, size_t n, const char *value)
{
    Macro *m = find_macro(p, name, n);
    if (!m && p->nm < 128) m = &p->m[p->nm++];
    if (!m) return;
    snprintf(m->name, sizeof(m->name), "%.*s", (int)n, name);
    free(m->value);
    m->value = strdup(value);
}

/* Comments out, lines joined, directives done: plain GLSL. NULL on error. */
static char *preprocess(const char *src, char *err, size_t errn)
{
    Pre p;
    char *text, *line, *next;
    int skip[32], depth = 0, taken[32];
    size_t i, j, n = strlen(src);
    memset(&p, 0, sizeof(p));
    /* comments to spaces, continuation lines joined */
    text = (char *)malloc(n + 1);
    if (!text) return NULL;
    for (i = j = 0; i < n; ) {
        if (src[i] == '/' && src[i + 1] == '/') { while (i < n && src[i] != '\n') i++; }
        else if (src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            while (i < n && !(src[i] == '*' && src[i + 1] == '/')) { if (src[i] == '\n') text[j++] = '\n'; i++; }
            i += 2;
            text[j++] = ' ';
        } else if (src[i] == '\\' && src[i + 1] == '\n') i += 2;
        else text[j++] = src[i++];
    }
    text[j] = 0;
    add_macro(&p, "__VERSION__", 11, "120");
    skip[0] = 0;
    for (line = text; line; line = next) {
        char *s = line;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        if (*s == '#') {
            char *d = s + 1, *arg;
            size_t dl;
            while (*d == ' ' || *d == '\t') d++;
            arg = d;
            while (isalpha((unsigned char)*arg)) arg++;
            dl = (size_t)(arg - d);
            while (*arg == ' ' || *arg == '\t') arg++;
            if ((dl == 5 && !strncmp(d, "ifdef", 5)) || (dl == 6 && !strncmp(d, "ifndef", 6)) || (dl == 2 && !strncmp(d, "if", 2))) {
                long v;
                if (depth >= 30) { snprintf(err, errn, "#if nested too deep"); goto fail; }
                if (d[2] == 'd' || d[2] == 'n') {
                    size_t k = 0;
                    while (isalnum((unsigned char)arg[k]) || arg[k] == '_') k++;
                    v = find_macro(&p, arg, k) != NULL;
                    if (d[2] == 'n') v = !v;
                } else { PExpr e; e.s = arg; e.p = &p; v = pe_or(&e); }
                depth++;
                skip[depth] = skip[depth - 1] || !v;
                taken[depth] = v != 0;
            } else if (dl == 4 && !strncmp(d, "elif", 4)) {
                PExpr e; long v;
                e.s = arg; e.p = &p; v = pe_or(&e);
                if (depth == 0) { snprintf(err, errn, "#elif without #if"); goto fail; }
                skip[depth] = skip[depth - 1] || taken[depth] || !v;
                if (v) taken[depth] = 1;
            } else if (dl == 4 && !strncmp(d, "else", 4)) {
                if (depth == 0) { snprintf(err, errn, "#else without #if"); goto fail; }
                skip[depth] = skip[depth - 1] || taken[depth];
                taken[depth] = 1;
            } else if (dl == 5 && !strncmp(d, "endif", 5)) {
                if (depth > 0) depth--;
            } else if (!skip[depth] && dl == 6 && !strncmp(d, "define", 6)) {
                size_t k = 0;
                while (isalnum((unsigned char)arg[k]) || arg[k] == '_') k++;
                if (arg[k] == '(') { snprintf(err, errn, "function-like macros are not read"); goto fail; }
                {
                    Pre tmp;   /* the value with the macros of now expanded */
                    memset(&tmp, 0, sizeof(tmp));
                    memcpy(tmp.m, p.m, sizeof(p.m)); tmp.nm = p.nm;
                    expand(&tmp, arg + k, strlen(arg + k), 0);
                    add_macro(&p, arg, k, tmp.out ? tmp.out : "");
                    free(tmp.out);
                }
            } else if (!skip[depth] && dl == 5 && !strncmp(d, "undef", 5)) {
                size_t k = 0;
                Macro *m;
                while (isalnum((unsigned char)arg[k]) || arg[k] == '_') k++;
                if ((m = find_macro(&p, arg, k))) m->name[0] = 0;
            }
            /* #version, #extension, #pragma, #line: nothing to do */
            out_add(&p, "\n", 1);
            continue;
        }
        if (!skip[depth]) expand(&p, line, strlen(line), 0);
        out_add(&p, "\n", 1);
    }
    free(text);
    for (i = 0; i < (size_t)p.nm; i++) free(p.m[i].value);
    return p.out ? p.out : strdup("");
fail:
    free(text);
    for (i = 0; i < (size_t)p.nm; i++) free(p.m[i].value);
    free(p.out);
    return NULL;
}

/* ---- lexer ------------------------------------------------------------------------------------ */

enum {
    TK_EOF = 0, TK_IDENT = 256, TK_NUM, TK_INC, TK_DEC, TK_ADDA, TK_SUBA, TK_MULA, TK_DIVA,
    TK_EQ, TK_NE, TK_LE, TK_GE, TK_AND, TK_OR, TK_XOR, TK_SHL, TK_SHR
};

typedef struct Tok { int t; const char *s; size_t n; double num; int is_float; int line; } Tok;

typedef struct Parser {
    Arena *a;
    Tok *tk; int ntk, pos;
    char *err; size_t errn;
    int failed;
} Parser;

static int lex(const char *s, Tok **out, int *nout)
{
    int cap = 1024, n = 0, line = 1;
    Tok *t = (Tok *)malloc((size_t)cap * sizeof(Tok));
    if (!t) return -1;
    while (*s) {
        Tok k;
        memset(&k, 0, sizeof(k));
        if (*s == '\n') { line++; s++; continue; }
        if (isspace((unsigned char)*s)) { s++; continue; }
        k.s = s; k.line = line;
        if (isalpha((unsigned char)*s) || *s == '_') {
            while (isalnum((unsigned char)*s) || *s == '_') s++;
            k.t = TK_IDENT;
        } else if (isdigit((unsigned char)*s) || (*s == '.' && isdigit((unsigned char)s[1]))) {
            char *e;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { k.num = (double)strtoul(s, &e, 16); }
            else {
                k.num = strtod(s, &e);
                { const char *q; for (q = s; q < e; q++) if (*q == '.' || *q == 'e' || *q == 'E') k.is_float = 1; }
            }
            s = e;
            if (*s == 'f' || *s == 'F') { s++; k.is_float = 1; }
            else if (*s == 'u' || *s == 'U') s++;
            k.t = TK_NUM;
        } else {
            static const struct { const char *op; int t; } ops[] = {
                { "++", TK_INC }, { "--", TK_DEC }, { "+=", TK_ADDA }, { "-=", TK_SUBA }, { "*=", TK_MULA }, { "/=", TK_DIVA },
                { "==", TK_EQ }, { "!=", TK_NE }, { "<=", TK_LE }, { ">=", TK_GE }, { "&&", TK_AND }, { "||", TK_OR },
                { "^^", TK_XOR }, { "<<", TK_SHL }, { ">>", TK_SHR } };
            size_t i;
            k.t = (unsigned char)*s;
            for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
                if (s[0] == ops[i].op[0] && s[1] == ops[i].op[1]) { k.t = ops[i].t; break; }
            s += k.t >= 256 ? 2 : 1;
        }
        k.n = (size_t)(s - k.s);
        if (n + 1 >= cap) { Tok *nt; cap *= 2; if (!(nt = (Tok *)realloc(t, (size_t)cap * sizeof(Tok)))) { free(t); return -1; } t = nt; }
        t[n++] = k;
    }
    t[n].t = TK_EOF; t[n].s = s; t[n].n = 0; t[n].line = line;
    *out = t; *nout = n;
    return 0;
}

/* ---- syntax tree ------------------------------------------------------------------------------ */

enum {
    N_NUM = 1, N_BOOL, N_IDENT, N_BIN, N_UN, N_ASSIGN, N_CALL, N_FIELD, N_INDEX, N_COND, N_POST, N_PRE,
    S_DECL, S_EXPR, S_IF, S_FOR, S_WHILE, S_DO, S_RETURN, S_DISCARD, S_BLOCK, S_BREAK, S_CONTINUE, S_EMPTY
};

typedef struct Node {
    int kind, op;
    const char *name;
    double num; int is_float;
    Type type;                 /* declarations, constructors */
    struct Node *a, *b, *c, *d;
    struct Node **list; int nlist;
    int line;
} Node;

enum { Q_NONE = 0, Q_CONST = 1, Q_UNIFORM = 2, Q_IN = 4, Q_OUT = 8, Q_INOUT = 16 };

typedef struct Global { Type type; const char *name; int qual; Node *init; } Global;
typedef struct Param { Type type; const char *name; int qual; } Param;
typedef struct Func { Type ret; const char *name; Param p[8]; int np; Node *body; } Func;

struct GlslShader {
    int stage;
    Arena arena;
    Global g[160]; int ng;
    Func f[48]; int nf;
    char unread[96];           /* non-empty: parsed only in part (what stopped it) */
};

static Node *node(Parser *p, int kind)
{
    Node *n = (Node *)amalloc(p->a, sizeof(Node));
    if (n) { n->kind = kind; n->line = p->tk[p->pos].line; }
    return n;
}

static Tok *peek(Parser *p) { return &p->tk[p->pos]; }
static Tok *peek2(Parser *p) { return &p->tk[p->pos + 1 < p->ntk ? p->pos + 1 : p->ntk]; }
static int is(Parser *p, int t) { return peek(p)->t == t; }
static int is_word(Parser *p, const char *w) { Tok *k = peek(p); return k->t == TK_IDENT && k->n == strlen(w) && !strncmp(k->s, w, k->n); }
static const char *tok_str(Parser *p, Tok *k) { return astrdup(p->a, k->s, k->n); }

static void perr(Parser *p, const char *fmt, ...)
{
    va_list ap;
    if (p->failed) return;
    p->failed = 1;
    va_start(ap, fmt);
    {
        int off = snprintf(p->err, p->errn, "line %d: ", peek(p)->line);
        if (off > 0 && (size_t)off < p->errn) vsnprintf(p->err + off, p->errn - (size_t)off, fmt, ap);
    }
    va_end(ap);
}

static int accept(Parser *p, int t) { if (is(p, t)) { p->pos++; return 1; } return 0; }
static void expect(Parser *p, int t)
{
    if (!accept(p, t)) {
        Tok *k = peek(p);
        if (t < 256) perr(p, "expected '%c' before '%.*s'", t, (int)k->n, k->s);
        else perr(p, "unexpected '%.*s'", (int)k->n, k->s);
    }
}

static int peek_type(Parser *p, Type *t)
{
    Tok *k = peek(p);
    char buf[32];
    if (k->t != TK_IDENT || k->n >= sizeof(buf)) return 0;
    memcpy(buf, k->s, k->n); buf[k->n] = 0;
    return parse_type_name(buf, t);
}

static int is_qualifier(Parser *p)
{
    static const char *q[] = { "const", "uniform", "attribute", "varying", "in", "out", "inout", "centroid", "flat",
                               "smooth", "noperspective", "highp", "mediump", "lowp", "invariant", "precise", "layout" };
    size_t i;
    for (i = 0; i < sizeof(q) / sizeof(q[0]); i++) if (is_word(p, q[i])) return 1;
    return 0;
}

static int qualifiers(Parser *p)
{
    int q = 0;
    while (!p->failed && is_qualifier(p)) {
        if (is_word(p, "const")) q |= Q_CONST;
        else if (is_word(p, "uniform")) q |= Q_UNIFORM;
        else if (is_word(p, "attribute")) q |= Q_IN;
        else if (is_word(p, "varying")) q |= 0x100;   /* in or out by stage: decided by the caller */
        else if (is_word(p, "in")) q |= Q_IN;
        else if (is_word(p, "out")) q |= Q_OUT;
        else if (is_word(p, "inout")) q |= Q_INOUT;
        if (is_word(p, "layout")) {   /* layout(...): skipped */
            int depth = 0;
            p->pos++;
            do { if (is(p, '(')) depth++; else if (is(p, ')')) depth--; p->pos++; } while (depth > 0 && !is(p, TK_EOF));
            continue;
        }
        p->pos++;
    }
    return q;
}

static Node *expr(Parser *p);
static Node *assign_expr(Parser *p);

static Node *primary(Parser *p)
{
    Tok *k = peek(p);
    Node *n;
    Type t;
    if (k->t == TK_NUM) { n = node(p, N_NUM); n->num = k->num; n->is_float = k->is_float; p->pos++; return n; }
    if (accept(p, '(')) { n = expr(p); expect(p, ')'); return n; }
    if (k->t == TK_IDENT) {
        if (is_word(p, "true") || is_word(p, "false")) { n = node(p, N_BOOL); n->num = is_word(p, "true"); p->pos++; return n; }
        if (peek_type(p, &t) && peek2(p)->t == '(') {   /* a constructor */
            n = node(p, N_CALL); n->type = t; n->name = tok_str(p, k); p->pos++;
        } else if (peek2(p)->t == '(') {
            n = node(p, N_CALL); n->name = tok_str(p, k); p->pos++;
        } else if (peek_type(p, &t) && peek2(p)->t == '[') {   /* float[3](...) */
            n = node(p, N_CALL); n->type = t; n->name = tok_str(p, k); p->pos++;
            expect(p, '['); if (!is(p, ']')) (void)expr(p); expect(p, ']');
        } else { n = node(p, N_IDENT); n->name = tok_str(p, k); p->pos++; return n; }
        expect(p, '(');
        {
            Node *args[16]; int na = 0;
            if (is_word(p, "void") && peek2(p)->t == ')') p->pos++;
            if (!is(p, ')')) do { if (na < 16) args[na++] = assign_expr(p); else (void)assign_expr(p); } while (!p->failed && accept(p, ','));
            expect(p, ')');
            n->list = (Node **)amalloc(p->a, sizeof(Node *) * (size_t)(na ? na : 1));
            memcpy(n->list, args, sizeof(Node *) * (size_t)na);
            n->nlist = na;
        }
        return n;
    }
    perr(p, "unexpected '%.*s'", (int)k->n, k->s);
    return node(p, N_NUM);
}

static Node *postfix(Parser *p)
{
    Node *n = primary(p);
    while (!p->failed) {
        if (accept(p, '.')) {
            Node *f = node(p, N_FIELD);
            f->a = n; f->name = tok_str(p, peek(p));
            expect(p, TK_IDENT);
            n = f;
        } else if (accept(p, '[')) {
            Node *x = node(p, N_INDEX);
            x->a = n; x->b = expr(p);
            expect(p, ']');
            n = x;
        } else if (is(p, TK_INC) || is(p, TK_DEC)) {
            Node *x = node(p, N_POST);
            x->op = peek(p)->t; x->a = n; p->pos++;
            n = x;
        } else break;
    }
    return n;
}

static Node *unary(Parser *p)
{
    if (is(p, '-') || is(p, '+') || is(p, '!') || is(p, '~')) {
        Node *n = node(p, N_UN);
        n->op = peek(p)->t; p->pos++;
        n->a = unary(p);
        return n;
    }
    if (is(p, TK_INC) || is(p, TK_DEC)) {
        Node *n = node(p, N_PRE);
        n->op = peek(p)->t; p->pos++;
        n->a = unary(p);
        return n;
    }
    return postfix(p);
}

static int prec(int t)
{
    switch (t) {
    case '*': case '/': case '%': return 10;
    case '+': case '-': return 9;
    case TK_SHL: case TK_SHR: return 8;
    case '<': case '>': case TK_LE: case TK_GE: return 7;
    case TK_EQ: case TK_NE: return 6;
    case '&': return 5;
    case '^': return 4;
    case '|': return 3;
    case TK_AND: return 2;
    case TK_XOR: return 1;
    case TK_OR: return 0;
    default: return -1;
    }
}

static Node *binary(Parser *p, int min)
{
    Node *l = unary(p);
    while (!p->failed) {
        int t = peek(p)->t, pr = prec(t);
        Node *n;
        if (pr < min) break;
        p->pos++;
        n = node(p, N_BIN);
        n->op = t; n->a = l; n->b = binary(p, pr + 1);
        l = n;
    }
    return l;
}

static Node *cond_expr(Parser *p)
{
    Node *c = binary(p, 0);
    if (accept(p, '?')) {
        Node *n = node(p, N_COND);
        n->a = c; n->b = assign_expr(p);
        expect(p, ':');
        n->c = assign_expr(p);
        return n;
    }
    return c;
}

static Node *assign_expr(Parser *p)
{
    Node *l = cond_expr(p);
    int t = peek(p)->t;
    if (t == '=' || t == TK_ADDA || t == TK_SUBA || t == TK_MULA || t == TK_DIVA) {
        Node *n = node(p, N_ASSIGN);
        p->pos++;
        n->op = t; n->a = l; n->b = assign_expr(p);
        return n;
    }
    return l;
}

static Node *expr(Parser *p)
{
    Node *n = assign_expr(p);
    while (!p->failed && accept(p, ',')) {   /* the comma operator: the last value */
        Node *c = node(p, N_BIN);
        c->op = ','; c->a = n; c->b = assign_expr(p);
        n = c;
    }
    return n;
}

static int array_size(Parser *p)
{
    int n = 0;
    if (!accept(p, '[')) return 0;
    if (is(p, TK_NUM)) { n = (int)peek(p)->num; p->pos++; }
    else if (!is(p, ']')) { perr(p, "array sizes must be numbers"); }
    expect(p, ']');
    return n > 0 ? n : 1;
}

static Node *statement(Parser *p);

static Node *declaration(Parser *p, Type t)
{
    Node *blk = node(p, S_BLOCK), *items[16];
    int n = 0;
    blk->op = 1;   /* a list of declarations: no scope of its own */
    do {
        Node *d = node(p, S_DECL);
        d->type = t;
        d->name = tok_str(p, peek(p));
        expect(p, TK_IDENT);
        if (is(p, '[')) d->type.array = (short)array_size(p);
        if (accept(p, '=')) d->a = assign_expr(p);
        if (n < 16) items[n++] = d;
    } while (!p->failed && accept(p, ','));
    expect(p, ';');
    blk->list = (Node **)amalloc(p->a, sizeof(Node *) * (size_t)(n ? n : 1));
    memcpy(blk->list, items, sizeof(Node *) * (size_t)n);
    blk->nlist = n;
    return blk;
}

static Node *block(Parser *p)
{
    Node *b = node(p, S_BLOCK), **items = NULL;
    int n = 0, cap = 0;
    expect(p, '{');
    while (!p->failed && !is(p, '}') && !is(p, TK_EOF)) {
        Node *s = statement(p);
        if (n == cap) {
            Node **ni = (Node **)amalloc(p->a, sizeof(Node *) * (size_t)(cap ? cap * 2 : 16));
            if (items) memcpy(ni, items, sizeof(Node *) * (size_t)n);
            items = ni; cap = cap ? cap * 2 : 16;
        }
        items[n++] = s;
    }
    expect(p, '}');
    b->list = items; b->nlist = n;
    return b;
}

static Node *statement(Parser *p)
{
    Type t;
    Node *n;
    if (is(p, '{')) return block(p);
    if (accept(p, ';')) return node(p, S_EMPTY);
    if (is_word(p, "if")) {
        n = node(p, S_IF); p->pos++;
        expect(p, '('); n->a = expr(p); expect(p, ')');
        n->b = statement(p);
        if (is_word(p, "else")) { p->pos++; n->c = statement(p); }
        return n;
    }
    if (is_word(p, "for")) {
        n = node(p, S_FOR); p->pos++;
        expect(p, '(');
        n->a = statement(p);   /* the init: a declaration or an expression statement */
        if (!is(p, ';')) n->b = expr(p);
        expect(p, ';');
        if (!is(p, ')')) n->c = expr(p);
        expect(p, ')');
        n->d = statement(p);
        return n;
    }
    if (is_word(p, "while")) {
        n = node(p, S_WHILE); p->pos++;
        expect(p, '('); n->a = expr(p); expect(p, ')');
        n->d = statement(p);
        return n;
    }
    if (is_word(p, "do")) {
        n = node(p, S_DO); p->pos++;
        n->d = statement(p);
        if (!is_word(p, "while")) { perr(p, "expected while"); return n; }
        p->pos++;
        expect(p, '('); n->a = expr(p); expect(p, ')'); expect(p, ';');
        return n;
    }
    if (is_word(p, "return")) { n = node(p, S_RETURN); p->pos++; if (!is(p, ';')) n->a = expr(p); expect(p, ';'); return n; }
    if (is_word(p, "discard")) { p->pos++; expect(p, ';'); return node(p, S_DISCARD); }
    if (is_word(p, "break")) { p->pos++; expect(p, ';'); return node(p, S_BREAK); }
    if (is_word(p, "continue")) { p->pos++; expect(p, ';'); return node(p, S_CONTINUE); }
    if (is_qualifier(p)) {   /* const float x = ...; highp vec2 v; */
        int q = qualifiers(p);
        (void)q;
        if (!peek_type(p, &t)) { perr(p, "expected a type"); return node(p, S_EMPTY); }
        p->pos++;
        return declaration(p, t);
    }
    if (peek_type(p, &t) && (peek2(p)->t == TK_IDENT || peek2(p)->t == '[')) {
        p->pos++;
        if (is(p, '[')) t.array = (short)array_size(p);
        return declaration(p, t);
    }
    n = node(p, S_EXPR);
    n->a = expr(p);
    expect(p, ';');
    return n;
}

/* The whole shader: globals and functions. */
static void unit(Parser *p, GlslShader *sh)
{
    while (!p->failed && !is(p, TK_EOF)) {
        int q;
        Type t;
        if (accept(p, ';')) continue;
        if (is_word(p, "precision")) { while (!is(p, ';') && !is(p, TK_EOF)) p->pos++; accept(p, ';'); continue; }
        if (is_word(p, "struct")) { snprintf(sh->unread, sizeof(sh->unread), "structs"); perr(p, "structs are not read"); return; }
        q = qualifiers(p);
        /* varying: out in a vertex shader, in in a fragment one */
        if (q & 0x100) q = (q & ~0x100) | (sh->stage == GLSL_VERTEX ? Q_OUT : Q_IN);
        if (!peek_type(p, &t)) {
            if (peek(p)->t == TK_IDENT && peek2(p)->t == '{') { snprintf(sh->unread, sizeof(sh->unread), "interface blocks"); perr(p, "interface blocks are not read"); return; }
            if (q & Q_IN && peek(p)->t == ';') { p->pos++; continue; }   /* "invariant gl_Position;" and the like */
            perr(p, "expected a type, not '%.*s'", (int)peek(p)->n, peek(p)->s);
            return;
        }
        p->pos++;
        if (is(p, '[')) t.array = (short)array_size(p);
        if (peek(p)->t == TK_IDENT && peek2(p)->t == '(') {   /* a function */
            Func *f;
            if (sh->nf >= 48) { perr(p, "too many functions"); return; }
            f = &sh->f[sh->nf];
            memset(f, 0, sizeof(*f));
            f->ret = t;
            f->name = tok_str(p, peek(p));
            p->pos += 2;
            if (is_word(p, "void") && peek2(p)->t == ')') p->pos++;
            while (!p->failed && !is(p, ')')) {
                int pq = qualifiers(p);
                Type pt;
                if (!peek_type(p, &pt)) { perr(p, "expected a parameter type"); return; }
                p->pos++;
                if (f->np < 8) {
                    f->p[f->np].type = pt; f->p[f->np].qual = pq;
                    f->p[f->np].name = is(p, TK_IDENT) ? tok_str(p, peek(p)) : "";
                }
                if (is(p, TK_IDENT)) p->pos++;
                if (is(p, '[')) { int s = array_size(p); if (f->np < 8) f->p[f->np].type.array = (short)s; }
                f->np++;
                if (!accept(p, ',')) break;
            }
            expect(p, ')');
            if (accept(p, ';')) continue;   /* a prototype */
            f->body = block(p);
            sh->nf++;
            continue;
        }
        do {   /* global variables */
            Global *g;
            if (sh->ng >= 160) { perr(p, "too many globals"); return; }
            g = &sh->g[sh->ng++];
            g->type = t; g->qual = q;
            g->name = tok_str(p, peek(p));
            expect(p, TK_IDENT);
            if (is(p, '[')) g->type.array = (short)array_size(p);
            if (accept(p, '=')) g->init = assign_expr(p);
        } while (!p->failed && accept(p, ','));
        expect(p, ';');
    }
}

GlslShader *glsl_compile(int stage, const char *src, char *log, size_t logn)
{
    GlslShader *sh = (GlslShader *)calloc(1, sizeof(*sh));
    Parser p;
    char *text;
    if (!sh) return NULL;
    sh->stage = stage;
    if (log && logn) log[0] = 0;
    if (!(text = preprocess(src, log, logn))) { free(sh); return NULL; }
    memset(&p, 0, sizeof(p));
    p.a = &sh->arena; p.err = log; p.errn = logn;
    if (lex(text, &p.tk, &p.ntk) < 0) { free(text); free(sh); return NULL; }
    /* identifiers point into the text: kept with the shader */
    {
        char *keep = (char *)amalloc(&sh->arena, strlen(text) + 1);
        size_t off;
        int i;
        strcpy(keep, text);
        for (i = 0; i <= p.ntk; i++) { off = (size_t)(p.tk[i].s - text); p.tk[i].s = keep + off; }
        free(text);
    }
    unit(&p, sh);
    free(p.tk);
    if (p.failed) {
        /* kept: the plan says "not read" and draws the nearest thing */
        if (!sh->unread[0]) snprintf(sh->unread, sizeof(sh->unread), "%.95s", log ? log : "a parse error");
    }
    return sh;
}

void glsl_shader_free(GlslShader *s)
{
    if (!s) return;
    afree(&s->arena);
    free(s);
}

/* ---- symbols: scalar expressions ------------------------------------------------------------ */

enum { Y_K, Y_ATTR, Y_TEX, Y_TEXC, Y_FRAGCOORD, Y_UNKNOWN, Y_OP };
enum {
    O_ADD, O_SUB, O_MUL, O_DIV, O_NEG, O_LT, O_LE, O_GT, O_GE, O_EQ, O_NE, O_AND, O_OR, O_XOR, O_NOT, O_SEL,
    O_MIN, O_MAX, O_CLAMP, O_MIX, O_POW, O_SIN, O_COS, O_TAN, O_ASIN, O_ACOS, O_ATAN, O_ATAN2, O_EXP, O_LOG,
    O_EXP2, O_LOG2, O_SQRT, O_RSQRT, O_ABS, O_SIGN, O_FLOOR, O_CEIL, O_FRACT, O_MOD, O_STEP, O_SSTEP, O_TRUNC, O_ROUND
};

typedef struct Sym {
    unsigned char kind, op, comp;
    short idx;                  /* attribute location; texture unit */
    float k;
    struct Sym *a, *b, *c;
} Sym;

typedef struct Val { Type t; int n; Sym **c; } Val;

typedef struct Var { const char *name; Val v; } Var;

typedef struct Ev {
    Arena *a;
    GlslProgram *prog;
    GlslShader *sh;
    int stage;
    Var vars[512]; int nv;
    Sym *path;                  /* the condition this code runs under (NULL: always) */
    Sym *discard;               /* when the fragment is dropped */
    int flow;                   /* 1 break, 2 continue, 3 return */
    Val ret;
    int approx; char why[96];
    long steps;
    int depth;
    const struct VOut *vout; int nvout;   /* the vertex outputs, for the fragment's inputs */
} Ev;

typedef struct VOut { const char *name; Val v; } VOut;

static void approx(Ev *e, const char *fmt, ...)
{
    va_list ap;
    if (e->approx) return;
    e->approx = 1;
    va_start(ap, fmt);
    vsnprintf(e->why, sizeof(e->why), fmt, ap);
    va_end(ap);
}

static Sym *sym(Ev *e, int kind) { Sym *s = (Sym *)amalloc(e->a, sizeof(Sym)); if (s) s->kind = (unsigned char)kind; return s; }
static Sym *K(Ev *e, float v) { Sym *s = sym(e, Y_K); if (s) s->k = v; return s; }
static int isk(const Sym *s, float v) { return s && s->kind == Y_K && s->k == v; }

static float fold(int op, float x, float y, float z)
{
    switch (op) {
    case O_ADD: return x + y;   case O_SUB: return x - y;   case O_MUL: return x * y;
    case O_DIV: return y != 0 ? x / y : 0;                  case O_NEG: return -x;
    case O_LT: return x < y;    case O_LE: return x <= y;   case O_GT: return x > y;    case O_GE: return x >= y;
    case O_EQ: return x == y;   case O_NE: return x != y;
    case O_AND: return x != 0 && y != 0;  case O_OR: return x != 0 || y != 0;  case O_XOR: return (x != 0) != (y != 0);
    case O_NOT: return x == 0;  case O_SEL: return x != 0 ? y : z;
    case O_MIN: return x < y ? x : y;     case O_MAX: return x > y ? x : y;
    case O_CLAMP: return x < y ? y : x > z ? z : x;
    case O_MIX: return x + (y - x) * z;
    case O_POW: return powf(x, y);  case O_SIN: return sinf(x);  case O_COS: return cosf(x);  case O_TAN: return tanf(x);
    case O_ASIN: return asinf(x);   case O_ACOS: return acosf(x);  case O_ATAN: return atanf(x);  case O_ATAN2: return atan2f(x, y);
    case O_EXP: return expf(x);     case O_LOG: return logf(x);  case O_EXP2: return exp2f(x);  case O_LOG2: return log2f(x);
    case O_SQRT: return sqrtf(x);   case O_RSQRT: return x > 0 ? 1.0f / sqrtf(x) : 0;
    case O_ABS: return fabsf(x);    case O_SIGN: return (float)((x > 0) - (x < 0));
    case O_FLOOR: return floorf(x); case O_CEIL: return ceilf(x);  case O_FRACT: return x - floorf(x);
    case O_MOD: return y != 0 ? x - y * floorf(x / y) : 0;
    case O_STEP: return y < x ? 0.0f : 1.0f;
    case O_SSTEP: { float t = (z - x) / (y - x != 0 ? y - x : 1); t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t); }
    case O_TRUNC: return truncf(x); case O_ROUND: return roundf(x);
    default: return 0;
    }
}

static Sym *op3(Ev *e, int op, Sym *a, Sym *b, Sym *c)
{
    Sym *s;
    int nargs = op == O_CLAMP || op == O_MIX || op == O_SEL || op == O_SSTEP ? 3 :
                op == O_NEG || op == O_NOT || (op >= O_SIN && op <= O_ATAN) || (op >= O_EXP && op <= O_FRACT) ||
                op == O_TRUNC || op == O_ROUND ? 1 : 2;
    if (!a || (nargs > 1 && !b) || (nargs > 2 && !c)) return K(e, 0);
    if (a->kind == Y_K && (nargs < 2 || b->kind == Y_K) && (nargs < 3 || c->kind == Y_K))
        return K(e, fold(op, a->k, nargs > 1 ? b->k : 0, nargs > 2 ? c->k : 0));
    switch (op) {   /* the simplifications that keep matrix products linear */
    case O_ADD: if (isk(a, 0)) return b; if (isk(b, 0)) return a; break;
    case O_SUB: if (isk(b, 0)) return a; break;
    case O_MUL: if (isk(a, 0) || isk(b, 0)) return K(e, 0); if (isk(a, 1)) return b; if (isk(b, 1)) return a; break;
    case O_DIV: if (isk(b, 1)) return a; break;
    case O_SEL: if (a->kind == Y_K) return a->k != 0 ? b : c; if (b == c) return b; break;
    case O_AND: if (isk(a, 0) || isk(b, 0)) return K(e, 0); if (a->kind == Y_K) return b; if (b->kind == Y_K) return a; break;
    case O_OR: if (a->kind == Y_K) return a->k != 0 ? K(e, 1) : b; if (b->kind == Y_K) return b->k != 0 ? K(e, 1) : a; break;
    case O_MIX: if (isk(c, 0)) return a; if (isk(c, 1)) return b; break;
    default: break;
    }
    s = sym(e, Y_OP);
    s->op = (unsigned char)op; s->a = a; s->b = b; s->c = c;
    return s;
}
static Sym *op1(Ev *e, int op, Sym *a) { return op3(e, op, a, NULL, NULL); }
static Sym *op2(Ev *e, int op, Sym *a, Sym *b) { return op3(e, op, a, b, NULL); }

/* ---- values ------------------------------------------------------------------------------------ */

static Type scalar_type(int base) { Type t; t.base = (unsigned char)base; t.rows = 1; t.cols = 1; t.array = 0; return t; }
static Type vec_type(int base, int n) { Type t = scalar_type(base); t.rows = (unsigned char)n; return t; }

static Val val_new(Ev *e, Type t)
{
    Val v;
    int i, n = (t.base == T_SAMPLER ? 1 : t.rows * t.cols) * (t.array > 0 ? t.array : 1);
    v.t = t; v.n = n;
    v.c = (Sym **)amalloc(e->a, sizeof(Sym *) * (size_t)(n ? n : 1));
    for (i = 0; i < n; i++) v.c[i] = K(e, 0);
    return v;
}

static Val val_copy(Ev *e, Val s)
{
    Val v = s;
    v.c = (Sym **)amalloc(e->a, sizeof(Sym *) * (size_t)(s.n ? s.n : 1));
    memcpy(v.c, s.c, sizeof(Sym *) * (size_t)s.n);
    return v;
}

static Val scalar(Ev *e, Sym *s, int base) { Val v = val_new(e, scalar_type(base)); v.c[0] = s; return v; }

static Var *lookup(Ev *e, const char *name)
{
    int i;
    for (i = e->nv - 1; i >= 0; i--) if (!strcmp(e->vars[i].name, name)) return &e->vars[i];
    return NULL;
}

static Var *declare(Ev *e, const char *name, Val v)
{
    if (e->nv >= 512) { approx(e, "too many variables"); return &e->vars[511]; }
    e->vars[e->nv].name = name;
    e->vars[e->nv].v = v;
    return &e->vars[e->nv++];
}

/* ---- the program ------------------------------------------------------------------------------ */

struct GlslProgram {
    GlslShader *vs, *fs;
    GlslVar attr[GLSL_MAX_ATTRIBS]; int nattr;
    GlslVar uni[GLSL_MAX_UNIFORMS]; int nuni;
    int next_location;
    Arena plan_arena;           /* the last plan's expressions */
    Sym *pos[4], *uv[2], *col[4];
    int planned_ok;
};

static GlslBuiltinFn g_builtins;
void glsl_set_builtins(GlslBuiltinFn fn) { g_builtins = fn; }

static unsigned gl_type(Type t)
{
    if (t.base == T_SAMPLER) return 0x8B5E;   /* GL_SAMPLER_2D */
    if (t.cols > 1) return t.cols == 2 ? 0x8B5A : t.cols == 3 ? 0x8B5B : 0x8B5C;
    if (t.base == T_INT) return t.rows == 1 ? 0x1404 : 0x8B53 + (unsigned)(t.rows - 2);
    if (t.base == T_BOOL) return t.rows == 1 ? 0x8B56 : 0x8B57 + (unsigned)(t.rows - 2);
    return t.rows == 1 ? 0x1406 : 0x8B50 + (unsigned)(t.rows - 2);
}

static void add_uniform(GlslProgram *p, const Global *g)
{
    GlslVar *u;
    int i;
    for (i = 0; i < p->nuni; i++) if (!strcmp(p->uni[i].name, g->name)) return;
    if (p->nuni >= GLSL_MAX_UNIFORMS) return;
    u = &p->uni[p->nuni++];
    memset(u, 0, sizeof(*u));
    snprintf(u->name, sizeof(u->name), "%s", g->name);
    u->comps = g->type.base == T_SAMPLER ? 1 : g->type.rows * g->type.cols;
    u->count = g->type.array > 0 ? g->type.array : 1;
    u->is_int = g->type.base == T_INT || g->type.base == T_BOOL || g->type.base == T_SAMPLER;
    u->is_sampler = g->type.base == T_SAMPLER;
    u->is_matrix = g->type.cols > 1;
    u->gltype = gl_type(g->type);
    u->value = (float *)calloc((size_t)(u->comps * u->count), sizeof(float));
    u->location = p->next_location;
    p->next_location += u->count;
}

GlslProgram *glsl_link(GlslShader *vs, GlslShader *fs, const char *const *bound_names, const int *bound_locs, int nbound,
                       char *log, size_t logn)
{
    GlslProgram *p;
    int i, j, used = 0;
    if (log && logn) log[0] = 0;
    if (!vs || !fs) { if (log) snprintf(log, logn, "a vertex and a fragment shader are needed"); return NULL; }
    if (!(p = (GlslProgram *)calloc(1, sizeof(*p)))) return NULL;
    p->vs = vs; p->fs = fs;
    for (i = 0; i < vs->ng; i++) {
        const Global *g = &vs->g[i];
        if (g->qual & Q_UNIFORM) add_uniform(p, g);
        else if ((g->qual & Q_IN) && p->nattr < GLSL_MAX_ATTRIBS) {
            GlslVar *a = &p->attr[p->nattr++];
            memset(a, 0, sizeof(*a));
            snprintf(a->name, sizeof(a->name), "%s", g->name);
            a->comps = g->type.rows * g->type.cols; a->count = 1; a->gltype = gl_type(g->type);
            a->location = -1;
            for (j = 0; j < nbound; j++) if (!strcmp(bound_names[j], g->name)) a->location = bound_locs[j];
            if (a->location >= 0) used |= 1 << a->location;
        }
    }
    for (i = 0; i < fs->ng; i++) if (fs->g[i].qual & Q_UNIFORM) add_uniform(p, &fs->g[i]);
    for (i = 0; i < p->nattr; i++)   /* the rest: the free locations, in order */
        if (p->attr[i].location < 0)
            for (j = 0; j < GLSL_MAX_ATTRIBS; j++) if (!(used & (1 << j))) { p->attr[i].location = j; used |= 1 << j; break; }
    return p;
}

void glsl_program_free(GlslProgram *p)
{
    int i;
    if (!p) return;
    for (i = 0; i < p->nuni; i++) free(p->uni[i].value);
    afree(&p->plan_arena);
    free(p);
}

int glsl_attrib_count(const GlslProgram *p) { return p->nattr; }
const GlslVar *glsl_attrib(const GlslProgram *p, int i) { return &p->attr[i]; }
int glsl_uniform_count(const GlslProgram *p) { return p->nuni; }
GlslVar *glsl_uniform(GlslProgram *p, int i) { return &p->uni[i]; }

int glsl_uniform_location(const GlslProgram *p, const char *name)
{
    int i;
    char base[64];
    const char *br = strchr(name, '[');
    long idx = 0;
    size_t n = br ? (size_t)(br - name) : strlen(name);
    if (n >= sizeof(base)) return -1;
    memcpy(base, name, n); base[n] = 0;
    if (br) idx = strtol(br + 1, NULL, 10);
    for (i = 0; i < p->nuni; i++)
        if (!strcmp(p->uni[i].name, base)) return idx >= 0 && idx < p->uni[i].count ? p->uni[i].location + (int)idx : -1;
    return -1;
}

float *glsl_uniform_slot(GlslProgram *p, int location, int *comps, int *remaining, int *is_int)
{
    int i;
    for (i = 0; i < p->nuni; i++) {
        GlslVar *u = &p->uni[i];
        if (location >= u->location && location < u->location + u->count) {
            int k = location - u->location;
            *comps = u->comps; *remaining = u->count - k; *is_int = u->is_int;
            return u->value + k * u->comps;
        }
    }
    return NULL;
}

/* ---- evaluation: expressions ------------------------------------------------------------------ */

static Val eval(Ev *e, Node *n);
static void exec(Ev *e, Node *s);

static int comps_of(Type t) { return t.base == T_SAMPLER ? 1 : t.rows * t.cols; }

/* component i, a scalar broadcast */
static Sym *at(Val v, int i) { return v.n == 1 ? v.c[0] : i < v.n ? v.c[i] : v.c[v.n - 1]; }

static int arith_op(int tok)
{
    switch (tok) {
    case '+': case TK_ADDA: return O_ADD;
    case '-': case TK_SUBA: return O_SUB;
    case '*': case TK_MULA: return O_MUL;
    case '/': case TK_DIVA: return O_DIV;
    case '%': return O_MOD;
    default: return -1;
    }
}

static Val arith(Ev *e, int op, Val a, Val b)
{
    Val r;
    int i;
    int am = a.t.cols > 1, bm = b.t.cols > 1;
    if (op == O_MUL && (am || bm) && a.n > 1 && b.n > 1) {
        if (am && bm) {   /* matrix x matrix */
            int R = a.t.rows, Kk = a.t.cols, C = b.t.cols, rr, cc, k;
            Type t = a.t; t.cols = (unsigned char)C; t.array = 0;
            r = val_new(e, t);
            for (cc = 0; cc < C; cc++) for (rr = 0; rr < R; rr++) {
                Sym *s = K(e, 0);
                for (k = 0; k < Kk; k++) s = op2(e, O_ADD, s, op2(e, O_MUL, a.c[k * R + rr], b.c[cc * b.t.rows + k]));
                r.c[cc * R + rr] = s;
            }
            return r;
        }
        if (am) {         /* matrix x vector */
            int R = a.t.rows, C = a.t.cols, rr, cc;
            r = val_new(e, vec_type(T_FLOAT, R));
            for (rr = 0; rr < R; rr++) {
                Sym *s = K(e, 0);
                for (cc = 0; cc < C; cc++) s = op2(e, O_ADD, s, op2(e, O_MUL, a.c[cc * R + rr], at(b, cc)));
                r.c[rr] = s;
            }
            return r;
        }
        {                 /* vector x matrix */
            int R = b.t.rows, C = b.t.cols, rr, cc;
            r = val_new(e, vec_type(T_FLOAT, C));
            for (cc = 0; cc < C; cc++) {
                Sym *s = K(e, 0);
                for (rr = 0; rr < R; rr++) s = op2(e, O_ADD, s, op2(e, O_MUL, at(a, rr), b.c[cc * R + rr]));
                r.c[cc] = s;
            }
            return r;
        }
    }
    r = val_new(e, a.n >= b.n ? a.t : b.t);
    r.t.array = 0;
    for (i = 0; i < r.n; i++) {
        Sym *s = op2(e, op, at(a, i), at(b, i));
        if (op == O_DIV && a.t.base == T_INT && b.t.base == T_INT) s = op1(e, O_TRUNC, s);
        r.c[i] = s;
    }
    return r;
}

static int swizzle_index(char c)
{
    switch (c) {
    case 'x': case 'r': case 's': return 0;
    case 'y': case 'g': case 't': return 1;
    case 'z': case 'b': case 'p': return 2;
    case 'w': case 'a': case 'q': return 3;
    default: return -1;
    }
}

/* Where an assignment writes: pointers to the slots. */
static int lval(Ev *e, Node *n, Sym ***out, Type *t)
{
    if (n->kind == N_IDENT) {
        Var *v = lookup(e, n->name);
        int i;
        if (!v) { approx(e, "no variable %s", n->name); return 0; }
        *out = (Sym **)amalloc(e->a, sizeof(Sym *) * (size_t)(v->v.n ? v->v.n : 1));
        for (i = 0; i < v->v.n; i++) (*out)[i] = (Sym *)(void *)&v->v.c[i];
        *t = v->v.t;
        return v->v.n;
    }
    if (n->kind == N_FIELD) {
        Sym **base;
        Type bt;
        int nb = lval(e, n->a, &base, &bt), i, len = (int)strlen(n->name);
        if (!nb) return 0;
        *out = (Sym **)amalloc(e->a, sizeof(Sym *) * (size_t)len);
        for (i = 0; i < len; i++) {
            int k = swizzle_index(n->name[i]);
            if (k < 0 || k >= nb) { approx(e, "field .%s", n->name); return 0; }
            (*out)[i] = base[k];
        }
        *t = vec_type(bt.base, len);
        return len;
    }
    if (n->kind == N_INDEX) {
        Sym **base;
        Type bt;
        int nb = lval(e, n->a, &base, &bt), elem, idx;
        Val iv;
        if (!nb) return 0;
        iv = eval(e, n->b);
        if (iv.c[0]->kind != Y_K) { approx(e, "a write at a varying index"); return 0; }
        idx = (int)iv.c[0]->k;
        if (bt.array > 0) { elem = comps_of(bt); *t = bt; t->array = 0; }
        else if (bt.cols > 1) { elem = bt.rows; *t = vec_type(bt.base, bt.rows); }
        else { elem = 1; *t = scalar_type(bt.base); }
        if (idx < 0 || (idx + 1) * elem > nb) { approx(e, "an index out of range"); return 0; }
        *out = base + idx * elem;
        return elem;
    }
    approx(e, "an assignment to an expression");
    return 0;
}

static void store(Sym **slots, int n, Val v)
{
    int i;
    for (i = 0; i < n; i++) *(Sym **)(void *)slots[i] = at(v, i);
}

static Sym *tex_sym(Ev *e, Val sampler, Sym *u, Sym *v)
{
    Sym *t = sym(e, Y_TEX);
    if (sampler.c[0]->kind == Y_K) t->idx = (short)sampler.c[0]->k;
    else approx(e, "a sampler chosen at run time");
    t->a = u; t->b = v;
    return t;
}

static Val texture_call(Ev *e, Val s, Val coord, int proj)
{
    Val r = val_new(e, vec_type(T_FLOAT, 4));
    Sym *u = at(coord, 0), *v = coord.n > 1 ? coord.c[1] : K(e, 0), *t;
    int i;
    if (proj) { Sym *q = coord.c[coord.n - 1]; u = op2(e, O_DIV, u, q); v = op2(e, O_DIV, v, q); }
    t = tex_sym(e, s, u, v);
    for (i = 0; i < 4; i++) { Sym *c = sym(e, Y_TEXC); c->a = t; c->comp = (unsigned char)i; r.c[i] = c; }
    return r;
}

static Val call_user(Ev *e, Func *f, Val *args, Node **argn, int na);

static Sym *dot_syms(Ev *e, Val a, Val b)
{
    Sym *s = K(e, 0);
    int i, n = a.n > b.n ? a.n : b.n;
    for (i = 0; i < n; i++) s = op2(e, O_ADD, s, op2(e, O_MUL, at(a, i), at(b, i)));
    return s;
}

static Val each(Ev *e, int op, Val *a, int na)   /* a component-wise built-in */
{
    Val r;
    int i, n = 1, k;
    for (k = 0; k < na; k++) if (a[k].n > n) n = a[k].n;
    r = val_new(e, vec_type(a[0].t.base == T_INT && op != O_SQRT ? T_INT : T_FLOAT, n));
    for (i = 0; i < n; i++) r.c[i] = op3(e, op, at(a[0], i), na > 1 ? at(a[1], i) : NULL, na > 2 ? at(a[2], i) : NULL);
    return r;
}

static Val construct(Ev *e, Type t, Val *args, int na)
{
    Val r = val_new(e, t);
    Sym *flat[64];
    int nf = 0, i, k;
    r.t.array = 0;
    for (k = 0; k < na; k++) for (i = 0; i < args[k].n && nf < 64; i++) flat[nf++] = args[k].c[i];
    if (t.cols > 1 && na == 1 && args[0].t.cols > 1) {   /* matrix from matrix: the overlap, identity elsewhere */
        int R = t.rows, C = t.cols, sr = args[0].t.rows, sc = args[0].t.cols, rr, cc;
        for (cc = 0; cc < C; cc++) for (rr = 0; rr < R; rr++)
            r.c[cc * R + rr] = rr < sr && cc < sc ? args[0].c[cc * sr + rr] : K(e, rr == cc ? 1.0f : 0.0f);
        return r;
    }
    if (nf == 1 && r.n > 1) {
        if (t.cols > 1) { for (i = 0; i < r.n; i++) r.c[i] = i % (t.rows + 1) == 0 ? flat[0] : K(e, 0); }   /* diagonal */
        else for (i = 0; i < r.n; i++) r.c[i] = flat[0];
        return r;
    }
    for (i = 0; i < r.n; i++) r.c[i] = i < nf ? flat[i] : K(e, 0);
    if (t.base == T_INT) for (i = 0; i < r.n; i++) r.c[i] = op1(e, O_TRUNC, r.c[i]);
    if (t.base == T_BOOL) for (i = 0; i < r.n; i++) r.c[i] = op2(e, O_NE, r.c[i], K(e, 0));
    return r;
}

static Val call(Ev *e, Node *n)
{
    Val a[16];
    int na = n->nlist, i;
    const char *f = n->name;
    if (!strcmp(f, "ftransform")) {   /* old shaders: the fixed pipeline's position */
        Var *m = lookup(e, "gl_ModelViewProjectionMatrix"), *v = lookup(e, "gl_Vertex");
        if (m && v) return arith(e, O_MUL, m->v, v->v);
        approx(e, "ftransform");
        return val_new(e, vec_type(T_FLOAT, 4));
    }
    for (i = 0; i < na && i < 16; i++) a[i] = eval(e, n->list[i]);
    if (n->type.base != T_VOID || n->type.rows) return construct(e, n->type, a, na);
    for (i = 0; i < e->sh->nf; i++)
        if (!strcmp(e->sh->f[i].name, f) && e->sh->f[i].np == na) return call_user(e, &e->sh->f[i], a, n->list, na);
#define IS(s) (!strcmp(f, s))
    if (IS("texture2D") || IS("texture") || IS("texture2DLod") || IS("textureLod") || IS("textureCube") ||
        IS("texture2DRect") || IS("texture2DBias") || IS("textureGrad") || IS("texelFetch"))
        return na >= 2 ? texture_call(e, a[0], a[1], 0) : val_new(e, vec_type(T_FLOAT, 4));
    if (IS("texture2DProj") || IS("textureProj")) return na >= 2 ? texture_call(e, a[0], a[1], 1) : val_new(e, vec_type(T_FLOAT, 4));
    if (IS("shadow2D") || IS("shadow2DProj")) { Val r = val_new(e, vec_type(T_FLOAT, 4)); approx(e, "shadow maps"); for (i = 0; i < 4; i++) r.c[i] = K(e, 1); return r; }
    if (na == 1) {
        static const struct { const char *n; int op; } one[] = {
            { "sin", O_SIN }, { "cos", O_COS }, { "tan", O_TAN }, { "asin", O_ASIN }, { "acos", O_ACOS }, { "atan", O_ATAN },
            { "exp", O_EXP }, { "log", O_LOG }, { "exp2", O_EXP2 }, { "log2", O_LOG2 }, { "sqrt", O_SQRT },
            { "inversesqrt", O_RSQRT }, { "abs", O_ABS }, { "sign", O_SIGN }, { "floor", O_FLOOR }, { "ceil", O_CEIL },
            { "fract", O_FRACT }, { "trunc", O_TRUNC }, { "round", O_ROUND }, { "not", O_NOT } };
        size_t k;
        for (k = 0; k < sizeof(one) / sizeof(one[0]); k++) if (IS(one[k].n)) return each(e, one[k].op, a, 1);
        if (IS("radians")) { Val c = scalar(e, K(e, 3.14159265f / 180.0f), T_FLOAT); Val x[2]; x[0] = a[0]; x[1] = c; return each(e, O_MUL, x, 2); }
        if (IS("degrees")) { Val c = scalar(e, K(e, 180.0f / 3.14159265f), T_FLOAT); Val x[2]; x[0] = a[0]; x[1] = c; return each(e, O_MUL, x, 2); }
        if (IS("length")) return scalar(e, op1(e, O_SQRT, dot_syms(e, a[0], a[0])), T_FLOAT);
        if (IS("normalize")) {
            Val r = val_copy(e, a[0]);
            Sym *l = op1(e, O_RSQRT, dot_syms(e, a[0], a[0]));
            for (i = 0; i < r.n; i++) r.c[i] = op2(e, O_MUL, a[0].c[i], l);
            return r;
        }
        if (IS("any") || IS("all")) {
            Sym *s = K(e, IS("all") ? 1.0f : 0.0f);
            for (i = 0; i < a[0].n; i++) s = op2(e, IS("all") ? O_AND : O_OR, s, a[0].c[i]);
            return scalar(e, s, T_BOOL);
        }
        if (IS("transpose")) {
            Val r = val_copy(e, a[0]);
            int R = a[0].t.rows, C = a[0].t.cols, rr, cc;
            r.t.rows = (unsigned char)C; r.t.cols = (unsigned char)R;
            for (cc = 0; cc < C; cc++) for (rr = 0; rr < R; rr++) r.c[rr * C + cc] = a[0].c[cc * R + rr];
            return r;
        }
    }
    if (na == 2) {
        if (IS("min")) return each(e, O_MIN, a, 2);
        if (IS("max")) return each(e, O_MAX, a, 2);
        if (IS("pow")) return each(e, O_POW, a, 2);
        if (IS("mod")) return each(e, O_MOD, a, 2);
        if (IS("atan")) return each(e, O_ATAN2, a, 2);
        if (IS("step")) { Val x[2]; x[0] = a[1]; x[1] = a[0]; return each(e, O_STEP, x, 2); }   /* step(edge, x): x < edge ? 0 : 1 */
        if (IS("dot")) return scalar(e, dot_syms(e, a[0], a[1]), T_FLOAT);
        if (IS("distance")) { Val d = arith(e, O_SUB, a[0], a[1]); return scalar(e, op1(e, O_SQRT, dot_syms(e, d, d)), T_FLOAT); }
        if (IS("cross")) {
            Val r = val_new(e, vec_type(T_FLOAT, 3));
            r.c[0] = op2(e, O_SUB, op2(e, O_MUL, at(a[0], 1), at(a[1], 2)), op2(e, O_MUL, at(a[0], 2), at(a[1], 1)));
            r.c[1] = op2(e, O_SUB, op2(e, O_MUL, at(a[0], 2), at(a[1], 0)), op2(e, O_MUL, at(a[0], 0), at(a[1], 2)));
            r.c[2] = op2(e, O_SUB, op2(e, O_MUL, at(a[0], 0), at(a[1], 1)), op2(e, O_MUL, at(a[0], 1), at(a[1], 0)));
            return r;
        }
        if (IS("reflect")) {   /* I - 2 dot(N, I) N */
            Val r = val_copy(e, a[0]);
            Sym *d = op2(e, O_MUL, K(e, 2), dot_syms(e, a[1], a[0]));
            for (i = 0; i < r.n; i++) r.c[i] = op2(e, O_SUB, a[0].c[i], op2(e, O_MUL, d, at(a[1], i)));
            return r;
        }
        if (IS("lessThan")) return each(e, O_LT, a, 2);
        if (IS("lessThanEqual")) return each(e, O_LE, a, 2);
        if (IS("greaterThan")) return each(e, O_GT, a, 2);
        if (IS("greaterThanEqual")) return each(e, O_GE, a, 2);
        if (IS("equal")) return each(e, O_EQ, a, 2);
        if (IS("notEqual")) return each(e, O_NE, a, 2);
        if (IS("matrixCompMult")) return each(e, O_MUL, a, 2);
    }
    if (na == 3) {
        if (IS("clamp")) return each(e, O_CLAMP, a, 3);
        if (IS("mix")) {
            if (a[2].t.base == T_BOOL) { Val x[3]; x[0] = a[2]; x[1] = a[1]; x[2] = a[0]; return each(e, O_SEL, x, 3); }
            return each(e, O_MIX, a, 3);
        }
        if (IS("smoothstep")) return each(e, O_SSTEP, a, 3);
        if (IS("faceforward")) return a[0];
    }
#undef IS
    approx(e, "%s()", f);
    return val_new(e, vec_type(T_FLOAT, 4));
}

static Val eval(Ev *e, Node *n)
{
    Val r;
    int i;
    if (!n || ++e->steps > 400000) { if (n) approx(e, "too long to follow"); return scalar(e, K(e, 0), T_FLOAT); }
    switch (n->kind) {
    case N_NUM: return scalar(e, K(e, (float)n->num), n->is_float ? T_FLOAT : T_INT);
    case N_BOOL: return scalar(e, K(e, (float)n->num), T_BOOL);
    case N_IDENT: {
        Var *v = lookup(e, n->name);
        if (!v) { approx(e, "no variable %s", n->name); return scalar(e, K(e, 0), T_FLOAT); }
        return v->v;
    }
    case N_BIN: {
        Val a, b;
        int op;
        if (n->op == ',') { (void)eval(e, n->a); return eval(e, n->b); }
        a = eval(e, n->a);
        b = eval(e, n->b);
        if ((op = arith_op(n->op)) >= 0) return arith(e, op, a, b);
        switch (n->op) {
        case '<': return scalar(e, op2(e, O_LT, a.c[0], b.c[0]), T_BOOL);
        case '>': return scalar(e, op2(e, O_GT, a.c[0], b.c[0]), T_BOOL);
        case TK_LE: return scalar(e, op2(e, O_LE, a.c[0], b.c[0]), T_BOOL);
        case TK_GE: return scalar(e, op2(e, O_GE, a.c[0], b.c[0]), T_BOOL);
        case TK_EQ: case TK_NE: {
            Sym *s = K(e, 1);
            for (i = 0; i < (a.n > b.n ? a.n : b.n); i++) s = op2(e, O_AND, s, op2(e, O_EQ, at(a, i), at(b, i)));
            return scalar(e, n->op == TK_EQ ? s : op1(e, O_NOT, s), T_BOOL);
        }
        case TK_AND: return scalar(e, op2(e, O_AND, a.c[0], b.c[0]), T_BOOL);
        case TK_OR: return scalar(e, op2(e, O_OR, a.c[0], b.c[0]), T_BOOL);
        case TK_XOR: return scalar(e, op2(e, O_XOR, a.c[0], b.c[0]), T_BOOL);
        default: approx(e, "operator %d", n->op); return a;
        }
    }
    case N_UN: {
        Val a = eval(e, n->a);
        if (n->op == '+') return a;
        r = val_copy(e, a);
        r.t.array = 0;
        for (i = 0; i < r.n; i++) r.c[i] = op1(e, n->op == '-' ? O_NEG : O_NOT, a.c[i]);
        return r;
    }
    case N_ASSIGN: {
        Sym **slots;
        Type t;
        int ns = lval(e, n->a, &slots, &t), op = arith_op(n->op);
        Val v = eval(e, n->b);
        if (!ns) return v;
        if (op >= 0) {
            Val cur = val_new(e, t);
            for (i = 0; i < ns; i++) cur.c[i] = *(Sym **)(void *)slots[i];
            v = arith(e, op, cur, v);
        }
        if (e->path) {   /* under a condition not known here: keep the old value where it does not hold */
            Val m = val_new(e, t);
            for (i = 0; i < ns; i++) m.c[i] = op3(e, O_SEL, e->path, at(v, i), *(Sym **)(void *)slots[i]);
            v = m;
        }
        store(slots, ns, v);
        r = val_new(e, t);
        for (i = 0; i < ns; i++) r.c[i] = *(Sym **)(void *)slots[i];
        return r;
    }
    case N_PRE: case N_POST: {
        Sym **slots;
        Type t;
        int ns = lval(e, n->a, &slots, &t);
        Val old = val_new(e, t);
        for (i = 0; i < ns; i++) {
            old.c[i] = *(Sym **)(void *)slots[i];
            *(Sym **)(void *)slots[i] = op2(e, n->op == TK_INC ? O_ADD : O_SUB, old.c[i], K(e, 1));
        }
        if (n->kind == N_POST) return old;
        r = val_new(e, t);
        for (i = 0; i < ns; i++) r.c[i] = *(Sym **)(void *)slots[i];
        return r;
    }
    case N_CALL: return call(e, n);
    case N_FIELD: {
        Val a = eval(e, n->a);
        int len = (int)strlen(n->name);
        if (!strcmp(n->name, "length")) return scalar(e, K(e, (float)(a.t.array > 0 ? a.t.array : a.t.cols > 1 ? a.t.cols : a.n)), T_INT);
        r = val_new(e, vec_type(a.t.base, len));
        for (i = 0; i < len; i++) {
            int k = swizzle_index(n->name[i]);
            if (k < 0 || k >= a.n) { approx(e, "field .%s", n->name); return r; }
            r.c[i] = a.c[k];
        }
        return r;
    }
    case N_INDEX: {
        Val a = eval(e, n->a), iv = eval(e, n->b);
        int elem, idx, count;
        Type t;
        if (a.t.array > 0) { t = a.t; t.array = 0; elem = comps_of(t); count = a.t.array; }
        else if (a.t.cols > 1) { t = vec_type(a.t.base, a.t.rows); elem = a.t.rows; count = a.t.cols; }
        else { t = scalar_type(a.t.base); elem = 1; count = a.n; }
        r = val_new(e, t);
        if (iv.c[0]->kind != Y_K) {   /* a varying index: a choice among all of them */
            int k;
            for (i = 0; i < elem; i++) {
                Sym *s = a.c[i];
                for (k = 1; k < count; k++) s = op3(e, O_SEL, op2(e, O_EQ, iv.c[0], K(e, (float)k)), a.c[k * elem + i], s);
                r.c[i] = s;
            }
            return r;
        }
        idx = (int)iv.c[0]->k;
        if (idx < 0 || idx >= count) { approx(e, "an index out of range"); return r; }
        for (i = 0; i < elem; i++) r.c[i] = a.c[idx * elem + i];
        return r;
    }
    case N_COND: {
        Val c = eval(e, n->a), a, b;
        if (c.c[0]->kind == Y_K) return c.c[0]->k != 0 ? eval(e, n->b) : eval(e, n->c);
        a = eval(e, n->b); b = eval(e, n->c);
        r = val_copy(e, a.n >= b.n ? a : b);
        for (i = 0; i < r.n; i++) r.c[i] = op3(e, O_SEL, c.c[0], at(a, i), at(b, i));
        return r;
    }
    default:
        approx(e, "an expression of kind %d", n->kind);
        return scalar(e, K(e, 0), T_FLOAT);
    }
}

/* ---- evaluation: statements ------------------------------------------------------------------- */

static void exec_list(Ev *e, Node **l, int n)
{
    int i;
    for (i = 0; i < n && !e->flow; i++) exec(e, l[i]);
}

static void exec(Ev *e, Node *s)
{
    int mark, i;
    if (!s || e->flow) return;
    if (++e->steps > 400000) { approx(e, "too long to follow"); e->flow = 3; return; }
    switch (s->kind) {
    case S_EMPTY: return;
    case S_EXPR: (void)eval(e, s->a); return;
    case S_DECL: {
        Val v = val_new(e, s->type);
        if (s->a) {
            Val init = eval(e, s->a);
            for (i = 0; i < v.n; i++) v.c[i] = at(init, i);
        }
        declare(e, s->name, v);
        return;
    }
    case S_BLOCK:
        if (s->op == 1) { exec_list(e, s->list, s->nlist); return; }   /* declarations: in the enclosing scope */
        mark = e->nv;
        exec_list(e, s->list, s->nlist);
        e->nv = mark;
        return;
    case S_IF: {
        Val c = eval(e, s->a);
        Sym *cond = c.c[0], *saved = e->path;
        if (cond->kind == Y_K) { exec(e, cond->k != 0 ? s->b : s->c); return; }
        /* not known here: both, each writing only where its condition holds */
        e->path = saved ? op2(e, O_AND, saved, cond) : cond;
        mark = e->nv;
        exec(e, s->b);
        e->nv = mark;
        if (e->flow) { if (e->flow != 3 || e->stage == GLSL_VERTEX) approx(e, "a jump under a condition"); e->flow = 0; }
        if (s->c) {
            Sym *nc = op1(e, O_NOT, cond);
            e->path = saved ? op2(e, O_AND, saved, nc) : nc;
            exec(e, s->c);
            e->nv = mark;
            if (e->flow) { approx(e, "a jump under a condition"); e->flow = 0; }
        }
        e->path = saved;
        return;
    }
    case S_FOR: case S_WHILE: case S_DO: {
        int iter = 0;
        mark = e->nv;
        if (s->kind == S_FOR) exec(e, s->a);
        for (;;) {
            if (s->kind != S_DO || iter > 0) {
                if (s->b || s->kind != S_FOR) {
                    Val c = eval(e, s->kind == S_FOR ? s->b : s->a);
                    if (c.c[0]->kind != Y_K) { approx(e, "a loop whose end is not known here"); break; }
                    if (c.c[0]->k == 0) break;
                }
            }
            if (++iter > 256) { approx(e, "a loop of more than 256 turns"); break; }
            exec(e, s->d);
            if (e->flow == 1) { e->flow = 0; break; }
            if (e->flow == 2) e->flow = 0;
            if (e->flow) break;
            if (s->kind == S_FOR && s->c) (void)eval(e, s->c);
            if (s->kind == S_DO) {
                Val c = eval(e, s->a);
                if (c.c[0]->kind != Y_K) { approx(e, "a loop whose end is not known here"); break; }
                if (c.c[0]->k == 0) break;
            }
        }
        e->nv = mark;
        return;
    }
    case S_RETURN:
        if (s->a) e->ret = eval(e, s->a);
        e->flow = 3;
        return;
    case S_DISCARD: {
        Sym *c = e->path ? e->path : K(e, 1);
        e->discard = e->discard ? op2(e, O_OR, e->discard, c) : c;
        if (!e->path) e->flow = 3;
        return;
    }
    case S_BREAK: if (e->path) approx(e, "a break under a condition"); else e->flow = 1; return;
    case S_CONTINUE: if (e->path) approx(e, "a continue under a condition"); else e->flow = 2; return;
    default: approx(e, "a statement of kind %d", s->kind); return;
    }
}

static Val call_user(Ev *e, Func *f, Val *args, Node **argn, int na)
{
    int mark = e->nv, i;
    Val ret;
    Sym *saved_path = e->path;
    if (++e->depth > 24) { approx(e, "recursion"); e->depth--; return val_new(e, f->ret); }
    for (i = 0; i < na && i < f->np; i++) {
        Val v = val_new(e, f->p[i].type);
        int k;
        if (!(f->p[i].qual & Q_OUT) || (f->p[i].qual & Q_INOUT)) for (k = 0; k < v.n; k++) v.c[k] = at(args[i], k);
        declare(e, f->p[i].name, v);
    }
    e->ret = val_new(e, f->ret);
    exec(e, f->body);
    if (e->flow == 3 || e->flow == 0) e->flow = 0;
    ret = e->ret;
    for (i = 0; i < na && i < f->np; i++)   /* out parameters back to the caller */
        if (f->p[i].qual & (Q_OUT | Q_INOUT)) {
            Sym **slots;
            Type t;
            Val v = e->vars[mark + i].v;
            int ns;
            e->nv = mark;   /* the caller's names */
            ns = lval(e, argn[i], &slots, &t);
            e->nv = mark + f->np;
            if (ns) store(slots, ns, v);
        }
    e->nv = mark;
    e->path = saved_path;
    e->depth--;
    return ret;
}

/* ---- a stage ------------------------------------------------------------------------------------ */

static Type mat_type(int n) { Type t = vec_type(T_FLOAT, n); t.cols = (unsigned char)n; return t; }

static Val attr_val(Ev *e, Type t, int loc)
{
    Val v = val_new(e, t);
    int i;
    for (i = 0; i < v.n && i < 4; i++) { Sym *a = sym(e, Y_ATTR); a->idx = (short)loc; a->comp = (unsigned char)i; v.c[i] = a; }
    return v;
}

static const VOut *find_vout(Ev *e, const char *name)
{
    int i;
    for (i = 0; i < e->nvout; i++) if (!strcmp(e->vout[i].name, name)) return &e->vout[i];
    return NULL;
}

static void builtins(Ev *e)
{
    static const char *mats[] = { "gl_ModelViewProjectionMatrix", "gl_ModelViewMatrix", "gl_ProjectionMatrix", "gl_TextureMatrix0" };
    Val v;
    int i, k;
    declare(e, "gl_Position", val_new(e, vec_type(T_FLOAT, 4)));
    declare(e, "gl_PointSize", val_new(e, scalar_type(T_FLOAT)));
    declare(e, "gl_FragColor", val_new(e, vec_type(T_FLOAT, 4)));
    { Type t = vec_type(T_FLOAT, 4); t.array = 4; declare(e, "gl_FragData", val_new(e, t)); }
    v = val_new(e, vec_type(T_FLOAT, 4));
    for (i = 0; i < 4; i++) { v.c[i] = sym(e, Y_FRAGCOORD); v.c[i]->comp = (unsigned char)i; }
    declare(e, "gl_FragCoord", v);
    declare(e, "gl_FrontFacing", scalar(e, K(e, 1), T_BOOL));
    v = val_new(e, vec_type(T_FLOAT, 2));
    v.c[0] = sym(e, Y_UNKNOWN); v.c[1] = sym(e, Y_UNKNOWN);
    declare(e, "gl_PointCoord", v);
    if (e->stage == GLSL_VERTEX) {   /* the old fixed-pipeline inputs */
        Val c;
        declare(e, "gl_Vertex", attr_val(e, vec_type(T_FLOAT, 4), GLSL_ATTR_VERTEX));
        declare(e, "gl_Color", attr_val(e, vec_type(T_FLOAT, 4), GLSL_ATTR_COLOR));
        declare(e, "gl_Normal", attr_val(e, vec_type(T_FLOAT, 3), GLSL_ATTR_NORMAL));
        declare(e, "gl_MultiTexCoord0", attr_val(e, vec_type(T_FLOAT, 4), GLSL_ATTR_TEXCOORD0));
        declare(e, "gl_FrontColor", val_new(e, vec_type(T_FLOAT, 4)));
        { Type t = vec_type(T_FLOAT, 4); t.array = 4; declare(e, "gl_TexCoord", val_new(e, t)); }
        c = val_new(e, vec_type(T_FLOAT, 4));
        for (i = 0; i < 4; i++) c.c[i] = K(e, 1);
        declare(e, "gl_FrontSecondaryColor", c);
        declare(e, "gl_FogFragCoord", val_new(e, scalar_type(T_FLOAT)));
    } else {
        const VOut *o = find_vout(e, "gl_FrontColor"), *t = find_vout(e, "gl_TexCoord");
        Val c = val_new(e, vec_type(T_FLOAT, 4));
        if (o) c = o->v; else for (i = 0; i < 4; i++) c.c[i] = K(e, 1);
        declare(e, "gl_Color", c);
        if (t) declare(e, "gl_TexCoord", t->v);
        else { Type tt = vec_type(T_FLOAT, 4); tt.array = 4; declare(e, "gl_TexCoord", val_new(e, tt)); }
    }
    for (k = 0; k < 4; k++) {
        float m[16];
        int j;
        memset(m, 0, sizeof(m));
        m[0] = m[5] = m[10] = m[15] = 1;
        if (g_builtins) g_builtins(mats[k], m);
        v = val_new(e, mat_type(4));
        for (j = 0; j < 16; j++) v.c[j] = K(e, m[j]);
        declare(e, k == 3 ? "gl_TextureMatrix" : mats[k], v);
        if (k == 1) {   /* gl_NormalMatrix: the modelview's upper 3x3 (its inverse transpose for a rotation) */
            Val n3 = val_new(e, mat_type(3));
            int r, c;
            for (c = 0; c < 3; c++) for (r = 0; r < 3; r++) n3.c[c * 3 + r] = K(e, m[c * 4 + r]);
            declare(e, "gl_NormalMatrix", n3);
        }
    }
}

static int run_stage(Ev *e, GlslShader *sh)
{
    int i;
    GlslProgram *p = e->prog;
    e->sh = sh;
    e->stage = sh->stage;
    builtins(e);
    for (i = 0; i < sh->ng; i++) {
        const Global *g = &sh->g[i];
        Val v;
        if (g->qual & Q_UNIFORM) {
            int k, j;
            v = val_new(e, g->type);
            for (k = 0; k < p->nuni; k++) if (!strcmp(p->uni[k].name, g->name)) {
                for (j = 0; j < v.n && j < p->uni[k].comps * p->uni[k].count; j++) v.c[j] = K(e, p->uni[k].value[j]);
                break;
            }
        } else if ((g->qual & Q_IN) && sh->stage == GLSL_VERTEX) {
            int k, loc = 0;
            for (k = 0; k < p->nattr; k++) if (!strcmp(p->attr[k].name, g->name)) loc = p->attr[k].location;
            v = attr_val(e, g->type, loc);
        } else if ((g->qual & Q_IN) && sh->stage == GLSL_FRAGMENT) {
            const VOut *o = find_vout(e, g->name);
            if (o) v = o->v;
            else { v = val_new(e, g->type); approx(e, "the input %s, which no vertex output gives", g->name); }
        } else {
            v = val_new(e, g->type);
            if (g->init) { Val init = eval(e, g->init); int k; for (k = 0; k < v.n; k++) v.c[k] = at(init, k); }
        }
        declare(e, g->name, v);
    }
    for (i = 0; i < sh->nf; i++)
        if (!strcmp(sh->f[i].name, "main")) {
            e->ret = val_new(e, scalar_type(T_FLOAT));
            exec(e, sh->f[i].body);
            e->flow = 0;
            return 0;
        }
    approx(e, "no main()");
    return -1;
}

/* ---- the plan ------------------------------------------------------------------------------------ */

/* c + sum of k x attribute[loc].comp: the terms of one attribute location, or -1 */
static int linear(const Sym *s, float scale, float *coef /* [GLSL_ATTR_ALL][4] */, float *c)
{
    if (!s) return -1;
    switch (s->kind) {
    case Y_K: *c += scale * s->k; return 0;
    case Y_ATTR: coef[s->idx * 4 + s->comp] += scale; return 0;
    case Y_OP:
        switch (s->op) {
        case O_ADD: return linear(s->a, scale, coef, c) < 0 || linear(s->b, scale, coef, c) < 0 ? -1 : 0;
        case O_SUB: return linear(s->a, scale, coef, c) < 0 || linear(s->b, -scale, coef, c) < 0 ? -1 : 0;
        case O_NEG: return linear(s->a, -scale, coef, c);
        case O_MUL:
            if (s->a->kind == Y_K) return linear(s->b, scale * s->a->k, coef, c);
            if (s->b->kind == Y_K) return linear(s->a, scale * s->b->k, coef, c);
            return -1;
        case O_DIV: if (s->b->kind == Y_K && s->b->k != 0) return linear(s->a, scale / s->b->k, coef, c); return -1;
        default: return -1;
        }
    default: return -1;
    }
}

static void factors(const Sym *s, const Sym **out, int *n)
{
    if (s && s->kind == Y_OP && s->op == O_MUL) { factors(s->a, out, n); factors(s->b, out, n); return; }
    if (*n < 16) out[(*n)++] = s;
}

static const Sym *find_texc(const Sym *s, int depth)
{
    const Sym *t;
    if (!s || depth > 64) return NULL;
    if (s->kind == Y_TEXC) return s;
    if (s->kind != Y_OP) return NULL;
    if ((t = find_texc(s->a, depth + 1))) return t;
    if ((t = find_texc(s->b, depth + 1))) return t;
    return find_texc(s->c, depth + 1);
}

/* discard when X op K: the GE's alpha test keeps the other side */
static int alpha_test(const Sym *d, unsigned *func, float *ref)
{
    int op;
    const Sym *k;
    if (!d || d->kind != Y_OP) return 0;
    op = d->op;
    if (d->b && d->b->kind == Y_K && d->a->kind != Y_K) k = d->b;
    else if (d->a && d->a->kind == Y_K && d->b && d->b->kind != Y_K) {   /* K op X: turned round */
        k = d->a;
        op = op == O_LT ? O_GT : op == O_GT ? O_LT : op == O_LE ? O_GE : op == O_GE ? O_LE : op;
    } else return 0;
    switch (op) {   /* GL comparison that passes (GL_LESS = 0x201 ...) */
    case O_LT: *func = 0x206; break;   /* discard below: pass at or above (GL_GEQUAL) */
    case O_LE: *func = 0x204; break;   /* GL_GREATER */
    case O_GT: *func = 0x203; break;   /* GL_LEQUAL */
    case O_GE: *func = 0x201; break;   /* GL_LESS */
    case O_EQ: *func = 0x205; break;   /* GL_NOTEQUAL */
    case O_NE: *func = 0x202; break;   /* GL_EQUAL */
    default: return 0;
    }
    *ref = k->k;
    return 1;
}

int glsl_plan(GlslProgram *p, GlslPlan *plan)
{
    Ev *e;
    VOut vout[48];
    int nvout = 0, i, ch, loc = -1;
    const Sym *pos[4], *fc[4], *tex = NULL;
    memset(plan, 0, sizeof(*plan));
    if (!p) return -1;
    afree(&p->plan_arena);
    if (!(e = (Ev *)calloc(1, sizeof(*e)))) return -1;
    e->a = &p->plan_arena;
    e->prog = p;
    if (p->vs->unread[0]) approx(e, "the vertex shader: %s", p->vs->unread);
    else if (p->fs->unread[0]) approx(e, "the fragment shader: %s", p->fs->unread);
    /* the vertex shader */
    run_stage(e, p->vs);
    for (i = 0; i < 4; i++) pos[i] = lookup(e, "gl_Position") ? lookup(e, "gl_Position")->v.c[i] : K(e, 0);
    for (i = 0; i < p->vs->ng && nvout < 44; i++)
        if (p->vs->g[i].qual & Q_OUT) { vout[nvout].name = p->vs->g[i].name; vout[nvout].v = lookup(e, p->vs->g[i].name)->v; nvout++; }
    if (lookup(e, "gl_FrontColor")) { vout[nvout].name = "gl_FrontColor"; vout[nvout].v = lookup(e, "gl_FrontColor")->v; nvout++; }
    if (lookup(e, "gl_TexCoord")) { vout[nvout].name = "gl_TexCoord"; vout[nvout].v = lookup(e, "gl_TexCoord")->v; nvout++; }
    /* the fragment shader, its inputs the vertex outputs */
    e->nv = 0; e->path = NULL; e->flow = 0; e->depth = 0;
    e->vout = vout; e->nvout = nvout;
    run_stage(e, p->fs);
    {
        Var *out = NULL;
        for (i = 0; i < p->fs->ng; i++)
            if ((p->fs->g[i].qual & Q_OUT) && p->fs->g[i].type.rows == 4) { out = lookup(e, p->fs->g[i].name); break; }
        if (!out) {
            Var *fcv = lookup(e, "gl_FragColor"), *fd = lookup(e, "gl_FragData");
            out = fcv;
            /* gl_FragData[0] when that is what was written */
            if (fd && fcv && isk(fcv->v.c[0], 0) && isk(fcv->v.c[3], 0) && !(isk(fd->v.c[0], 0) && isk(fd->v.c[3], 0))) out = fd;
        }
        for (i = 0; i < 4; i++) fc[i] = out ? out->v.c[i] : K(e, 1);
    }
    /* position: one matrix times one attribute? */
    {
        static float coef[GLSL_ATTR_ALL * 4];
        float c[4];
        int ok = 1, r, k, a;
        for (r = 0; r < 4 && ok; r++) {
            memset(coef, 0, sizeof(coef));
            c[r] = 0;
            if (linear(pos[r], 1.0f, coef, &c[r]) < 0) { ok = 0; break; }
            for (a = 0; a < GLSL_ATTR_ALL; a++)
                for (k = 0; k < 4; k++) if (coef[a * 4 + k] != 0) {
                    if (loc >= 0 && loc != a) ok = 0;
                    loc = a;
                }
            if (loc >= 0) for (k = 0; k < 4; k++) plan->matrix[k * 4 + r] = coef[loc * 4 + k];
            plan->matrix[12 + r] += c[r];   /* a constant: on w, which is 1 */
        }
        plan->matrix_ok = ok && loc >= 0;
        plan->pos_attr = loc;
    }
    /* the colour: per channel, one texture sample times the rest */
    {
        int textured_rgb = 0, plain_rgb = 0;
        for (ch = 0; ch < 4; ch++) {
            const Sym *f[16], *t = NULL;
            int nf = 0, k, ntex = 0;
            factors(fc[ch], f, &nf);
            for (k = 0; k < nf; k++) if (f[k]->kind == Y_TEXC) { t = f[k]; ntex++; }
            if (!t && (t = find_texc(fc[ch], 0))) { approx(e, "a texture used other than as a factor"); ntex = 1; }
            if (ntex > 1) approx(e, "a product of texture samples");
            if (t) {
                if (!tex) tex = t->a;
                else if (t->a != tex && (t->a->idx != tex->idx || t->a->a != tex->a || t->a->b != tex->b)) approx(e, "two textures");
                if (t->comp != (ch < 3 ? ch : 3)) approx(e, "a texture channel used for another");
                if (ch < 3) textured_rgb++;
                else plan->tex_alpha = 1;
            } else if (ch < 3) plain_rgb++;
        }
        plan->textured = tex != NULL;
        if (tex && plain_rgb) approx(e, "a texture on some colour channels only");
        if (tex && !textured_rgb) approx(e, "a texture's alpha only");
        if (tex) plan->sampler_unit = tex->idx;
    }
    /* discard: the alpha test */
    if (e->discard) {
        if (!alpha_test(e->discard, &plan->alpha_func, &plan->alpha_ref)) approx(e, "a discard other than an alpha test");
        else plan->alpha_test = 1;
    }
    for (i = 0; i < 4; i++) p->pos[i] = (Sym *)pos[i];
    for (i = 0; i < 4; i++) p->col[i] = (Sym *)fc[i];
    p->uv[0] = tex ? tex->a : NULL;
    p->uv[1] = tex ? tex->b : NULL;
    plan->exact = !e->approx;
    snprintf(plan->why, sizeof(plan->why), "%s", e->why);
    plan->priv = p;
    free(e);
    return 0;
}

/* ---- per vertex ------------------------------------------------------------------------------------ */

static float nev(const Sym *s, const float (*attrs)[4], int depth)
{
    if (!s || depth > 200) return 0;
    switch (s->kind) {
    case Y_K: return s->k;
    case Y_ATTR: return attrs[s->idx][s->comp];
    case Y_TEXC: return 1.0f;       /* the GE multiplies the texture in */
    case Y_OP: return fold(s->op, nev(s->a, attrs, depth + 1), s->b ? nev(s->b, attrs, depth + 1) : 0, s->c ? nev(s->c, attrs, depth + 1) : 0);
    default: return 0;
    }
}

void glsl_plan_vertex(const GlslPlan *plan, const float (*attrs)[4], float pos[4], float uv[2], float rgba[4])
{
    const GlslProgram *p = (const GlslProgram *)plan->priv;
    int i;
    if (plan->matrix_ok) memcpy(pos, attrs[plan->pos_attr], 4 * sizeof(float));
    else for (i = 0; i < 4; i++) pos[i] = nev(p->pos[i], attrs, 0);
    uv[0] = p->uv[0] ? nev(p->uv[0], attrs, 0) : 0;
    uv[1] = p->uv[1] ? nev(p->uv[1], attrs, 0) : 0;
    for (i = 0; i < 4; i++) rgba[i] = nev(p->col[i], attrs, 0);
}
