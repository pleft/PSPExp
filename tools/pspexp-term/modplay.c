/* Tracker modules through libopenmpt, opened at run time: see modplay.h */
#include "modplay.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef struct openmpt_module openmpt_module;

/* libopenmpt's C API (libopenmpt.h, 0.4 and later), the calls Media uses */
static struct {
    int tried, ok;
    void *lib;
    openmpt_module *(*create)(const void *, size_t, void *, void *, void *, void *, int *, const char **, const void *);
    void (*destroy)(openmpt_module *);
    size_t (*read_stereo)(openmpt_module *, int32_t, size_t, int16_t *);
    double (*set_pos)(openmpt_module *, double);
    double (*duration)(openmpt_module *);
    const char *(*meta)(openmpt_module *, const char *);
    void (*free_string)(const char *);
    int32_t (*channels)(openmpt_module *);
    int32_t (*orders)(openmpt_module *);
    int32_t (*cur_order)(openmpt_module *);
    int32_t (*cur_pattern)(openmpt_module *);
    int32_t (*cur_row)(openmpt_module *);
    int32_t (*cur_speed)(openmpt_module *);
    double (*cur_tempo2)(openmpt_module *);
    int32_t (*cur_tempo)(openmpt_module *);
    int32_t (*pattern_rows)(openmpt_module *, int32_t);
    const char *(*format_cell)(openmpt_module *, int32_t, int32_t, int32_t, size_t, int);
    const char *(*highlight_cell)(openmpt_module *, int32_t, int32_t, int32_t, size_t, int);
    float (*vu)(openmpt_module *, int32_t);
} L;

struct ModPlay { openmpt_module *m; };

int mod_available(void)
{
    if (L.tried) return L.ok;
    L.tried = 1;
    if (!(L.lib = dlopen("libopenmpt.so.0", RTLD_NOW | RTLD_LOCAL)) && !(L.lib = dlopen("libopenmpt.0.dylib", RTLD_NOW | RTLD_LOCAL))) return 0;
#define SYM(f, n) (*(void **)&L.f = dlsym(L.lib, n))
    SYM(create, "openmpt_module_create_from_memory2"); SYM(destroy, "openmpt_module_destroy");
    SYM(read_stereo, "openmpt_module_read_interleaved_stereo"); SYM(set_pos, "openmpt_module_set_position_seconds");
    SYM(duration, "openmpt_module_get_duration_seconds"); SYM(meta, "openmpt_module_get_metadata"); SYM(free_string, "openmpt_free_string");
    SYM(channels, "openmpt_module_get_num_channels"); SYM(orders, "openmpt_module_get_num_orders");
    SYM(cur_order, "openmpt_module_get_current_order"); SYM(cur_pattern, "openmpt_module_get_current_pattern");
    SYM(cur_row, "openmpt_module_get_current_row"); SYM(cur_speed, "openmpt_module_get_current_speed");
    SYM(cur_tempo2, "openmpt_module_get_current_tempo2"); SYM(cur_tempo, "openmpt_module_get_current_tempo");
    SYM(pattern_rows, "openmpt_module_get_pattern_num_rows");
    SYM(format_cell, "openmpt_module_format_pattern_row_channel"); SYM(highlight_cell, "openmpt_module_highlight_pattern_row_channel");
    SYM(vu, "openmpt_module_get_current_channel_vu_mono");
#undef SYM
    L.ok = L.create && L.destroy && L.read_stereo && L.set_pos && L.duration && L.meta && L.free_string && L.channels && L.orders
        && L.cur_order && L.cur_pattern && L.cur_row && L.cur_speed && (L.cur_tempo2 || L.cur_tempo) && L.pattern_rows
        && L.format_cell && L.highlight_cell && L.vu;
    return L.ok;
}

ModPlay *mod_open(const void *data, size_t len, char *err, size_t en)
{
    static ModPlay one;   /* (one module plays at a time) */
    int e = 0;
    const char *msg = NULL;
    if (!mod_available()) { snprintf(err, en, "libopenmpt is not installed"); return NULL; }
    one.m = L.create(data, len, NULL, NULL, NULL, NULL, &e, &msg, NULL);
    if (!one.m) { snprintf(err, en, "%s", msg && msg[0] ? msg : "libopenmpt could not read it"); if (msg) L.free_string(msg); return NULL; }
    if (msg) L.free_string(msg);
    return &one;
}

void mod_close(ModPlay *m) { if (m && m->m) { L.destroy(m->m); m->m = NULL; } }
size_t mod_read(ModPlay *m, int rate, int16_t *out, size_t frames) { return L.read_stereo(m->m, rate, frames, out); }
void mod_seek(ModPlay *m, double s) { L.set_pos(m->m, s); }
double mod_duration(ModPlay *m) { return L.duration(m->m); }

void mod_meta(ModPlay *m, const char *key, char *out, size_t n)
{
    const char *v = L.meta(m->m, key);
    snprintf(out, n, "%s", v ? v : "");
    if (v) L.free_string(v);
}

void mod_state(ModPlay *m, ModState *s)
{
    int i;
    memset(s, 0, sizeof(*s));
    s->order = L.cur_order(m->m); s->orders = L.orders(m->m);
    s->pattern = L.cur_pattern(m->m); s->row = L.cur_row(m->m);
    s->rows = L.pattern_rows(m->m, s->pattern);
    s->speed = L.cur_speed(m->m);
    s->tempo = L.cur_tempo2 ? L.cur_tempo2(m->m) : L.cur_tempo(m->m);
    s->channels = L.channels(m->m);
    for (i = 0; i < s->channels && i < MOD_CHANNELS; i++) s->vu[i] = L.vu(m->m, i);
}

void mod_cell(ModPlay *m, int pattern, int row, int channel, int width, char *text, char *hl, size_t n)
{
    const char *t = L.format_cell(m->m, pattern, row, channel, (size_t)width, 1), *h = L.highlight_cell(m->m, pattern, row, channel, (size_t)width, 1);
    snprintf(text, n, "%s", t ? t : "");
    snprintf(hl, n, "%s", h ? h : "");
    if (t) L.free_string(t);
    if (h) L.free_string(h);
}

int mod_pattern_rows(ModPlay *m, int pattern) { return L.pattern_rows(m->m, pattern); }
