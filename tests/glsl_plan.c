/*
 * The GLSL reader on its own: a vertex and a fragment shader, uniform
 * values, and what the PSP would draw.
 *   glsl_plan VERTEX FRAGMENT [name=v,v,...]...
 */
#include "../compatibility/sdl-pspexp/glsl_pspexp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *s;
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    s = (char *)malloc((size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) { perror(path); exit(2); }
    s[n] = 0;
    fclose(f);
    return s;
}

int main(int argc, char **argv)
{
    char log[256];
    GlslShader *vs, *fs;
    GlslProgram *p;
    GlslPlan plan;
    int i;
    float attrs[GLSL_ATTR_ALL][4], pos[4], uv[2], rgba[4];
    if (argc < 3) { fprintf(stderr, "usage: glsl_plan VERTEX FRAGMENT [name=v,...]\n"); return 2; }
    vs = glsl_compile(GLSL_VERTEX, slurp(argv[1]), log, sizeof(log));
    if (log[0]) printf("vertex: %s\n", log);
    fs = glsl_compile(GLSL_FRAGMENT, slurp(argv[2]), log, sizeof(log));
    if (log[0]) printf("fragment: %s\n", log);
    p = glsl_link(vs, fs, NULL, NULL, 0, log, sizeof(log));
    if (!p) { printf("link: %s\n", log); return 1; }
    for (i = 0; i < glsl_attrib_count(p); i++) printf("attribute %s at %d (%d)\n", glsl_attrib(p, i)->name, glsl_attrib(p, i)->location, glsl_attrib(p, i)->comps);
    for (i = 0; i < glsl_uniform_count(p); i++) {
        GlslVar *u = glsl_uniform(p, i);
        if (u->is_matrix) { int k; for (k = 0; k < u->count; k++) { int d = u->comps == 16 ? 4 : u->comps == 9 ? 3 : 2, j; for (j = 0; j < d; j++) u->value[k * u->comps + j * d + j] = 1; } }
    }
    for (i = 3; i < argc; i++) {
        char *eq = strchr(argv[i], '='), *v;
        int loc, comps, rem, isint, k = 0;
        float *slot;
        if (!eq) continue;
        *eq = 0;
        loc = glsl_uniform_location(p, argv[i]);
        if (loc < 0 || !(slot = glsl_uniform_slot(p, loc, &comps, &rem, &isint))) { printf("no uniform %s\n", argv[i]); continue; }
        for (v = strtok(eq + 1, ","); v && k < comps * rem; v = strtok(NULL, ",")) slot[k++] = (float)atof(v);
    }
    for (i = 0; i < glsl_uniform_count(p); i++) printf("uniform %s: %d x %d%s\n", glsl_uniform(p, i)->name, glsl_uniform(p, i)->comps, glsl_uniform(p, i)->count, glsl_uniform(p, i)->is_sampler ? " sampler" : "");
    glsl_plan(p, &plan);
    printf("plan: %s%s%s\n", plan.exact ? "exact" : "nearest: ", plan.exact ? "" : plan.why, "");
    printf("  position: %s attribute %d\n", plan.matrix_ok ? "matrix x" : "per vertex from", plan.pos_attr);
    printf("  texture: %s unit %d, alpha %s\n", plan.textured ? "yes" : "no", plan.sampler_unit, plan.tex_alpha ? "yes" : "no");
    if (plan.alpha_test) printf("  alpha test: 0x%x %.3f\n", plan.alpha_func, plan.alpha_ref);
    if (plan.lit) printf("  lit: normal attribute %d, light %.2f %.2f %.2f, diffuse %.2f %.2f %.2f, ambient %.2f %.2f %.2f\n", plan.lit_attr,
                         plan.light_dir[0], plan.light_dir[1], plan.light_dir[2], plan.light_diffuse[0], plan.light_diffuse[1],
                         plan.light_diffuse[2], plan.light_ambient[0], plan.light_ambient[1], plan.light_ambient[2]);
    if (plan.mod_tex) printf("  multiplied in: unit %d, %g times over\n", plan.mod_unit, plan.mod_scale);
    if (plan.add_tex) printf("  added: unit %d\n", plan.add_unit);
    if (plan.pre_test) printf("  pre-pass: unit %d, alpha test 0x%x %.3f\n", plan.pre_unit, plan.alpha_func, plan.alpha_ref);
    if (plan.env) {
        float n[3] = { 0, 0, 1 }, d[3];
        glsl_plan_env_dir(&plan, n, d);
        printf("  cube map: unit %d%s%s, normal attribute %d, scale %.2f %.2f %.2f, facing: %.2f %.2f %.2f\n", plan.env_unit,
               plan.env_only ? ", the colour" : ", added", plan.env_alpha ? ", its alpha" : "", plan.env_attr,
               plan.env_scale[0], plan.env_scale[1], plan.env_scale[2], d[0], d[1], d[2]);
    }
    for (i = 0; i < GLSL_ATTR_ALL; i++) { attrs[i][0] = 0.1f * (float)i; attrs[i][1] = 0.2f; attrs[i][2] = 0.3f; attrs[i][3] = 0.5f; }
    glsl_plan_vertex(&plan, (const float (*)[4])attrs, pos, uv, rgba);
    printf("  a vertex: pos %.2f %.2f %.2f %.2f  uv %.2f %.2f  rgba %.2f %.2f %.2f %.2f\n", pos[0], pos[1], pos[2], pos[3], uv[0], uv[1], rgba[0], rgba[1], rgba[2], rgba[3]);
    return 0;
}
