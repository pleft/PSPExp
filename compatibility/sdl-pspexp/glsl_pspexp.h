/*
 * glsl-pspexp: GLSL shaders read, not run. A shader is parsed once; at a
 * draw, with the uniforms' values of the moment, it is evaluated
 * symbolically: what comes out is gl_Position and the fragment colour as
 * expressions of the vertex attributes and the texture samples. Where those
 * are what the GE can do (a matrix times a position, a texture times a
 * colour, an alpha test), the draw is given to the GE; the vertices' other
 * inputs are worked out here.
 */
#ifndef GLSL_PSPEXP_H
#define GLSL_PSPEXP_H

#include <stddef.h>

#define GLSL_MAX_ATTRIBS  16
#define GLSL_MAX_UNIFORMS 128

typedef struct GlslShader GlslShader;
typedef struct GlslProgram GlslProgram;

enum { GLSL_VERTEX = 0, GLSL_FRAGMENT = 1 };

/* type of a uniform or attribute, as glGetActive* name them */
typedef struct GlslVar {
    char name[64];
    int  comps;          /* floats per element: 1..4, 9 (mat3), 16 (mat4); samplers 1 */
    int  count;          /* array length (1 when not an array) */
    int  is_int, is_sampler, is_matrix;
    unsigned gltype;     /* GL_FLOAT_VEC4 and so on */
    float *value;        /* uniforms: comps x count */
    int location;
} GlslVar;

GlslShader *glsl_compile(int stage, const char *src, char *log, size_t logn);
void glsl_shader_free(GlslShader *s);

/* Links a vertex and a fragment shader: 0 or -1 with the log filled. */
GlslProgram *glsl_link(GlslShader *vs, GlslShader *fs, const char *const *bound_names, const int *bound_locs, int nbound,
                       char *log, size_t logn);
void glsl_program_free(GlslProgram *p);
int glsl_attrib_count(const GlslProgram *p);
const GlslVar *glsl_attrib(const GlslProgram *p, int i);     /* location = the generic attribute index */
int glsl_uniform_count(const GlslProgram *p);
GlslVar *glsl_uniform(GlslProgram *p, int i);                /* location = i's base location */
int glsl_uniform_location(const GlslProgram *p, const char *name);   /* "a[2]", "s.x" (not structs) */
float *glsl_uniform_slot(GlslProgram *p, int location, int *comps, int *remaining, int *is_int);

/* ---- the plan for a draw -------------------------------------------------------------- */

/* Attributes the GL layer feeds for old shaders' built-ins, after the
 * program's own: gl_Vertex, gl_Color, gl_Normal, gl_MultiTexCoord0. */
enum { GLSL_ATTR_VERTEX = GLSL_MAX_ATTRIBS, GLSL_ATTR_COLOR, GLSL_ATTR_NORMAL, GLSL_ATTR_TEXCOORD0, GLSL_ATTR_ALL };

/* Old shaders' built-in uniforms (gl_ModelViewProjectionMatrix...): asked of the GL layer. */
typedef void (*GlslBuiltinFn)(const char *name, float out[16]);
void glsl_set_builtins(GlslBuiltinFn fn);

typedef struct GlslPlan {
    int exact;                  /* 1: the GE draws it as the shader would; 0: the nearest it can */
    char why[96];               /* when not exact: what was left out */
    int matrix_ok;              /* gl_Position = matrix x one attribute (x, y, z, w): the GE transforms */
    int pos_attr;
    float matrix[16];           /* column-major */
    int textured, sampler_unit;
    int tex_alpha;              /* the texture's alpha counts (else only its colour) */
    int alpha_test;             /* the fragment is dropped when alpha fails: a GL comparison (GL_LESS...) */
    unsigned alpha_func; float alpha_ref;
    /* lighting the GE does itself: colour = clamp(ambient + max(0, normal . light) x diffuse), normal an
     * attribute times a matrix: the light's direction in the attribute's own space, the colours per channel */
    int lit, lit_attr;
    float light_dir[3], light_diffuse[3], light_ambient[3];
    /* a cube map added to the colour (a reflection; env_only: it is the colour): its direction worked out
     * for an eye-space normal by glsl_plan_env_dir; env_axes the normal matrix's rows, on env_attr */
    int env, env_only, env_alpha, env_unit, env_attr;
    float env_scale[3], env_axes[3][3], env_inv[9], env_pos[4];
    /* a discard on a texture the colour does not use (alpha_func, alpha_ref): a pass before with it, for
     * the depth; the texture coordinates are its */
    int pre_test, pre_unit;
    unsigned attribs_used;      /* bit per attribute location read (the position's, colour's and coordinates') */
    unsigned long long vertex_hash;   /* what the per-vertex values are worked out from: the same hash, the same
                                         vertices for the same attributes (meshes) */
    void *priv;                 /* the expressions: per vertex, glsl_plan_vertex */
} GlslPlan;

/* The plan for the current uniforms. 0, or -1 when the program cannot be
 * drawn at all (not linked). The texture unit is the sampler uniform's value.
 * Valid until the next glsl_plan on the same program. */
int glsl_plan(GlslProgram *p, GlslPlan *plan);

/* One vertex: attrs[location] (vec4 each; GLSL_ATTR_ALL of them) ->
 * position (with matrix_ok: the position attribute as is; else clip
 * coordinates x, y, z, w), texture coordinates, colour (rgba, 0..1). */
void glsl_plan_vertex(const GlslPlan *plan, const float (*attrs)[4], float pos[4], float uv[2], float rgba[4]);

/* A cube map's direction for an eye-space normal (plan->env), the view straight ahead. */
void glsl_plan_env_dir(const GlslPlan *plan, const float n[3], float dir[3]);

#endif
