/*
 * gl-pspexp: OpenGL 1.x programs on the PSP, as part of the SDL layer.
 *
 * The PSP's GE is fixed-function hardware much like OpenGL 1.x: matrices,
 * lights, fog, a depth buffer, blending, texture environments. This file is
 * an OpenGL implementation of its own (no GPU on the board is used): it keeps
 * GL's state, and every draw becomes 3D messages in the GE's terms
 * (PSPEXP_FEATURE_3D): the render state, the two matrices and the lights
 * when they changed, and the vertices in a GE vertex format. The PSP
 * transforms, lights, clips and draws them.
 *
 * What is there: immediate mode, vertex arrays and buffer objects, display
 * lists (replayed on the board), the matrix stacks, 2D textures (resampled to
 * powers of two of at most PSPEXP_GL_TEXMAX, 16 bits a texel), lighting (the
 * GE's four lights), linear fog, blending, alpha and depth tests, culling,
 * scissor, colour mask, the queries programs make. What is not: shaders,
 * multitexture (unit 0 only), stencil, reading pixels back, copying from the
 * framebuffer, clip planes, polygon modes other than fill, line width.
 *
 * Linux: the gl* functions are exported, so with LD_PRELOAD they come before
 * libGL's. macOS: they are interposed (dyld __interpose) over the OpenGL
 * framework's. glXGetProcAddress, eglGetProcAddress and SDL_GL_GetProcAddress
 * return ours.
 *
 * Environment:
 *   PSPEXP_GL_TEXMAX=256   the largest texture side sent (a power of two, <= 512)
 *   PSPEXP_GL_LOG=1        log unknown queries and each frame's draw count; 2: every draw
 *   PSPEXP_GL_DUMP=DIR     write every texture sent, as sent, to DIR/tex-<name>-<w>x<h>.rgba
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gl_pspexp.h"
#include "glsl_pspexp.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(__APPLE__)
#include <dlfcn.h>
#endif

/* ---- types and constants (the OpenGL headers are not needed) -------------- */

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef char GLchar;
typedef ptrdiff_t GLintptr;
typedef ptrdiff_t GLsizeiptr;

enum {
    GL_FALSE = 0, GL_TRUE = 1, GL_NONE = 0,
    /* errors */
    GL_NO_ERROR = 0, GL_INVALID_ENUM = 0x0500, GL_INVALID_VALUE = 0x0501, GL_INVALID_OPERATION = 0x0502,
    GL_STACK_OVERFLOW = 0x0503, GL_STACK_UNDERFLOW = 0x0504, GL_OUT_OF_MEMORY = 0x0505,
    /* primitives */
    GL_POINTS = 0, GL_LINES = 1, GL_LINE_LOOP = 2, GL_LINE_STRIP = 3, GL_TRIANGLES = 4, GL_TRIANGLE_STRIP = 5,
    GL_TRIANGLE_FAN = 6, GL_QUADS = 7, GL_QUAD_STRIP = 8, GL_POLYGON = 9,
    /* clear bits */
    GL_DEPTH_BUFFER_BIT = 0x100, GL_ACCUM_BUFFER_BIT = 0x200, GL_STENCIL_BUFFER_BIT = 0x400, GL_COLOR_BUFFER_BIT = 0x4000,
    /* comparison */
    GL_NEVER = 0x0200, GL_LESS = 0x0201, GL_EQUAL = 0x0202, GL_LEQUAL = 0x0203, GL_GREATER = 0x0204,
    GL_NOTEQUAL = 0x0205, GL_GEQUAL = 0x0206, GL_ALWAYS = 0x0207,
    /* blending */
    GL_ZERO = 0, GL_ONE = 1, GL_SRC_COLOR = 0x0300, GL_ONE_MINUS_SRC_COLOR = 0x0301, GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_DST_ALPHA = 0x0304, GL_ONE_MINUS_DST_ALPHA = 0x0305, GL_DST_COLOR = 0x0306,
    GL_ONE_MINUS_DST_COLOR = 0x0307, GL_SRC_ALPHA_SATURATE = 0x0308,
    GL_FUNC_ADD = 0x8006, GL_MIN = 0x8007, GL_MAX = 0x8008, GL_FUNC_SUBTRACT = 0x800A, GL_FUNC_REVERSE_SUBTRACT = 0x800B,
    GL_BLEND_EQUATION = 0x8009,
    /* faces */
    GL_FRONT = 0x0404, GL_BACK = 0x0405, GL_FRONT_AND_BACK = 0x0408, GL_CW = 0x0900, GL_CCW = 0x0901,
    /* capabilities */
    GL_POINT_SMOOTH = 0x0B10, GL_LINE_SMOOTH = 0x0B20, GL_LINE_STIPPLE = 0x0B24, GL_POLYGON_SMOOTH = 0x0B41,
    GL_POLYGON_STIPPLE = 0x0B42, GL_CULL_FACE = 0x0B44, GL_LIGHTING = 0x0B50, GL_COLOR_MATERIAL = 0x0B57,
    GL_FOG = 0x0B60, GL_DEPTH_TEST = 0x0B71, GL_STENCIL_TEST = 0x0B90, GL_NORMALIZE = 0x0BA1, GL_ALPHA_TEST = 0x0BC0,
    GL_DITHER = 0x0BD0, GL_BLEND = 0x0BE2, GL_COLOR_LOGIC_OP = 0x0BF2, GL_SCISSOR_TEST = 0x0C11,
    GL_TEXTURE_GEN_S = 0x0C60, GL_TEXTURE_GEN_T = 0x0C61, GL_TEXTURE_GEN_R = 0x0C62, GL_TEXTURE_GEN_Q = 0x0C63,
    GL_TEXTURE_1D = 0x0DE0, GL_TEXTURE_2D = 0x0DE1, GL_POLYGON_OFFSET_FILL = 0x8037, GL_RESCALE_NORMAL = 0x803A,
    GL_TEXTURE_3D = 0x806F, GL_MULTISAMPLE = 0x809D, GL_TEXTURE_CUBE_MAP = 0x8513, GL_CLIP_PLANE0 = 0x3000,
    GL_LIGHT0 = 0x4000,
    /* client arrays */
    GL_VERTEX_ARRAY = 0x8074, GL_NORMAL_ARRAY = 0x8075, GL_COLOR_ARRAY = 0x8076, GL_INDEX_ARRAY = 0x8077,
    GL_TEXTURE_COORD_ARRAY = 0x8078, GL_EDGE_FLAG_ARRAY = 0x8079, GL_FOG_COORD_ARRAY = 0x8457, GL_SECONDARY_COLOR_ARRAY = 0x845E,
    /* types */
    GL_BYTE = 0x1400, GL_UNSIGNED_BYTE = 0x1401, GL_SHORT = 0x1402, GL_UNSIGNED_SHORT = 0x1403, GL_INT = 0x1404,
    GL_UNSIGNED_INT = 0x1405, GL_FLOAT = 0x1406, GL_DOUBLE = 0x140A,
    GL_UNSIGNED_SHORT_5_6_5 = 0x8363, GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033, GL_UNSIGNED_SHORT_5_5_5_1 = 0x8034,
    GL_UNSIGNED_INT_8_8_8_8 = 0x8035, GL_UNSIGNED_INT_8_8_8_8_REV = 0x8367, GL_UNSIGNED_SHORT_1_5_5_5_REV = 0x8366,
    GL_UNSIGNED_SHORT_4_4_4_4_REV = 0x8365,
    /* matrices */
    GL_MODELVIEW = 0x1700, GL_PROJECTION = 0x1701, GL_TEXTURE = 0x1702, GL_MATRIX_MODE = 0x0BA0,
    GL_MODELVIEW_MATRIX = 0x0BA6, GL_PROJECTION_MATRIX = 0x0BA7, GL_TEXTURE_MATRIX = 0x0BA8,
    GL_MODELVIEW_STACK_DEPTH = 0x0BA3, GL_PROJECTION_STACK_DEPTH = 0x0BA4, GL_TEXTURE_STACK_DEPTH = 0x0BA5,
    GL_MAX_MODELVIEW_STACK_DEPTH = 0x0D36, GL_MAX_PROJECTION_STACK_DEPTH = 0x0D38, GL_MAX_TEXTURE_STACK_DEPTH = 0x0D39,
    /* shading, lighting, materials */
    GL_FLAT = 0x1D00, GL_SMOOTH = 0x1D01, GL_SHADE_MODEL = 0x0B54,
    GL_AMBIENT = 0x1200, GL_DIFFUSE = 0x1201, GL_SPECULAR = 0x1202, GL_POSITION = 0x1203, GL_SPOT_DIRECTION = 0x1204,
    GL_SPOT_EXPONENT = 0x1205, GL_SPOT_CUTOFF = 0x1206, GL_CONSTANT_ATTENUATION = 0x1207, GL_LINEAR_ATTENUATION = 0x1208,
    GL_QUADRATIC_ATTENUATION = 0x1209, GL_EMISSION = 0x1600, GL_SHININESS = 0x1601, GL_AMBIENT_AND_DIFFUSE = 0x1602,
    GL_COLOR_INDEXES = 0x1603, GL_LIGHT_MODEL_LOCAL_VIEWER = 0x0B51, GL_LIGHT_MODEL_TWO_SIDE = 0x0B52,
    GL_LIGHT_MODEL_AMBIENT = 0x0B53, GL_LIGHT_MODEL_COLOR_CONTROL = 0x81F8,
    GL_COLOR_MATERIAL_FACE = 0x0B55, GL_COLOR_MATERIAL_PARAMETER = 0x0B56,
    /* fog */
    GL_FOG_DENSITY = 0x0B62, GL_FOG_START = 0x0B63, GL_FOG_END = 0x0B64, GL_FOG_MODE = 0x0B65, GL_FOG_COLOR = 0x0B66,
    GL_FOG_INDEX = 0x0B61, GL_EXP = 0x0800, GL_EXP2 = 0x0801, GL_LINEAR = 0x2601, GL_FOG_HINT = 0x0C54,
    GL_FOG_COORD_SRC = 0x8450,
    /* textures */
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
    GL_NEAREST = 0x2600, GL_NEAREST_MIPMAP_NEAREST = 0x2700, GL_LINEAR_MIPMAP_NEAREST = 0x2701,
    GL_NEAREST_MIPMAP_LINEAR = 0x2702, GL_LINEAR_MIPMAP_LINEAR = 0x2703, GL_REPEAT = 0x2901, GL_CLAMP = 0x2900,
    GL_CLAMP_TO_EDGE = 0x812F, GL_CLAMP_TO_BORDER = 0x812D, GL_MIRRORED_REPEAT = 0x8370,
    GL_TEXTURE_ENV = 0x2300, GL_TEXTURE_ENV_MODE = 0x2200, GL_TEXTURE_ENV_COLOR = 0x2201,
    GL_MODULATE = 0x2100, GL_DECAL = 0x2101, GL_ADD = 0x0104, GL_REPLACE = 0x1E01, GL_COMBINE = 0x8570,
    GL_TEXTURE_BINDING_2D = 0x8069, GL_PROXY_TEXTURE_2D = 0x8064, GL_TEXTURE_WIDTH = 0x1000, GL_TEXTURE_HEIGHT = 0x1001,
    GL_TEXTURE_INTERNAL_FORMAT = 0x1003, GL_TEXTURE_BORDER = 0x1005, GL_TEXTURE_RED_SIZE = 0x805C,
    GL_GENERATE_MIPMAP = 0x8191, GL_TEXTURE_MAX_LEVEL = 0x813D, GL_TEXTURE_BASE_LEVEL = 0x813C,
    GL_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE, GL_TEXTURE0 = 0x84C0, GL_ACTIVE_TEXTURE = 0x84E0,
    GL_CLIENT_ACTIVE_TEXTURE = 0x84E1, GL_MAX_TEXTURE_UNITS = 0x84E2, GL_TEXTURE_RECTANGLE = 0x84F5,
    /* pixel formats */
    GL_COLOR_INDEX = 0x1900, GL_STENCIL_INDEX = 0x1901, GL_DEPTH_COMPONENT = 0x1902, GL_RED = 0x1903, GL_GREEN = 0x1904,
    GL_BLUE = 0x1905, GL_ALPHA = 0x1906, GL_RGB = 0x1907, GL_RGBA = 0x1908, GL_LUMINANCE = 0x1909,
    GL_LUMINANCE_ALPHA = 0x190A, GL_BGR = 0x80E0, GL_BGRA = 0x80E1, GL_INTENSITY = 0x8049, GL_RG = 0x8227,
    GL_ALPHA8 = 0x803C, GL_LUMINANCE8 = 0x8040, GL_LUMINANCE8_ALPHA8 = 0x8045, GL_INTENSITY8 = 0x804B,
    GL_RGB5 = 0x8050, GL_RGB8 = 0x8051, GL_RGBA4 = 0x8056, GL_RGB5_A1 = 0x8057, GL_RGBA8 = 0x8058,
    /* pixel store */
    GL_UNPACK_SWAP_BYTES = 0x0CF0, GL_UNPACK_LSB_FIRST = 0x0CF1, GL_UNPACK_ROW_LENGTH = 0x0CF2,
    GL_UNPACK_SKIP_ROWS = 0x0CF3, GL_UNPACK_SKIP_PIXELS = 0x0CF4, GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_PACK_ALIGNMENT = 0x0D05, GL_PACK_ROW_LENGTH = 0x0D02,
    /* queries */
    GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02, GL_EXTENSIONS = 0x1F03,
    GL_CURRENT_COLOR = 0x0B00, GL_CURRENT_NORMAL = 0x0B02, GL_CURRENT_TEXTURE_COORDS = 0x0B03,
    GL_VIEWPORT = 0x0BA2, GL_SCISSOR_BOX = 0x0C10, GL_DEPTH_RANGE = 0x0B70, GL_DEPTH_WRITEMASK = 0x0B72,
    GL_DEPTH_CLEAR_VALUE = 0x0B73, GL_DEPTH_FUNC = 0x0B74, GL_COLOR_CLEAR_VALUE = 0x0C22, GL_COLOR_WRITEMASK = 0x0C23,
    GL_BLEND_SRC = 0x0BE1, GL_BLEND_DST = 0x0BE0, GL_ALPHA_TEST_FUNC = 0x0BC1, GL_ALPHA_TEST_REF = 0x0BC2,
    GL_CULL_FACE_MODE = 0x0B45, GL_FRONT_FACE = 0x0B46, GL_MAX_TEXTURE_SIZE = 0x0D33, GL_MAX_LIGHTS = 0x0D31,
    GL_MAX_CLIP_PLANES = 0x0D32, GL_MAX_VIEWPORT_DIMS = 0x0D3A, GL_MAX_ELEMENTS_VERTICES = 0x80E8,
    GL_MAX_ELEMENTS_INDICES = 0x80E9, GL_RED_BITS = 0x0D52, GL_GREEN_BITS = 0x0D53, GL_BLUE_BITS = 0x0D54,
    GL_ALPHA_BITS = 0x0D55, GL_DEPTH_BITS = 0x0D56, GL_STENCIL_BITS = 0x0D57, GL_DOUBLEBUFFER = 0x0C32,
    GL_LIST_INDEX = 0x0B33, GL_LIST_MODE = 0x0B30, GL_LIST_BASE = 0x0B32, GL_MAX_LIST_NESTING = 0x0B31,
    GL_POINT_SIZE = 0x0B11, GL_LINE_WIDTH = 0x0B21, GL_ALIASED_LINE_WIDTH_RANGE = 0x846E,
    GL_ALIASED_POINT_SIZE_RANGE = 0x846D, GL_SUBPIXEL_BITS = 0x0D50, GL_MAX_TEXTURE_IMAGE_UNITS = 0x8872,
    GL_MAX_TEXTURE_COORDS = 0x8871, GL_NUM_EXTENSIONS = 0x821D, GL_MAJOR_VERSION = 0x821B, GL_MINOR_VERSION = 0x821C,
    GL_SAMPLE_BUFFERS = 0x80A8, GL_SAMPLES = 0x80A9, GL_ARRAY_BUFFER_BINDING = 0x8894,
    GL_ELEMENT_ARRAY_BUFFER_BINDING = 0x8895, GL_SHADING_LANGUAGE_VERSION = 0x8B8C,
    GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FF, GL_CURRENT_RASTER_POSITION = 0x0B07,
    /* display lists */
    GL_COMPILE = 0x1300, GL_COMPILE_AND_EXECUTE = 0x1301,
    /* buffers */
    GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893, GL_BUFFER_SIZE = 0x8764, GL_BUFFER_USAGE = 0x8765,
    GL_READ_ONLY = 0x88B8, GL_WRITE_ONLY = 0x88B9, GL_READ_WRITE = 0x88BA,
    /* interleaved arrays */
    GL_V2F = 0x2A20, GL_V3F = 0x2A21, GL_C4UB_V2F = 0x2A22, GL_C4UB_V3F = 0x2A23, GL_C3F_V3F = 0x2A24,
    GL_N3F_V3F = 0x2A25, GL_C4F_N3F_V3F = 0x2A26, GL_T2F_V3F = 0x2A27, GL_T4F_V4F = 0x2A28,
    GL_T2F_C4UB_V3F = 0x2A29, GL_T2F_C3F_V3F = 0x2A2A, GL_T2F_N3F_V3F = 0x2A2B, GL_T2F_C4F_N3F_V3F = 0x2A2C,
    GL_T4F_C4F_N3F_V4F = 0x2A2D,
    /* misc */
    GL_POLYGON_MODE = 0x0B40, GL_FILL = 0x1B02, GL_RENDER = 0x1C00, GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    /* texture coordinate generation */
    GL_S = 0x2000, GL_T = 0x2001, GL_R = 0x2002, GL_Q = 0x2003, GL_TEXTURE_GEN_MODE = 0x2500,
    GL_OBJECT_PLANE = 0x2501, GL_EYE_PLANE = 0x2502, GL_EYE_LINEAR = 0x2400, GL_OBJECT_LINEAR = 0x2401,
    GL_SPHERE_MAP = 0x2402,
    /* GL 2 and later */
    GL_FRAGMENT_SHADER = 0x8B30, GL_VERTEX_SHADER = 0x8B31, GL_DELETE_STATUS = 0x8B80, GL_COMPILE_STATUS = 0x8B81,
    GL_LINK_STATUS = 0x8B82, GL_VALIDATE_STATUS = 0x8B83, GL_INFO_LOG_LENGTH = 0x8B84, GL_ATTACHED_SHADERS = 0x8B85,
    GL_ACTIVE_UNIFORMS = 0x8B86, GL_ACTIVE_UNIFORM_MAX_LENGTH = 0x8B87, GL_SHADER_SOURCE_LENGTH = 0x8B88,
    GL_ACTIVE_ATTRIBUTES = 0x8B89, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH = 0x8B8A, GL_SHADER_TYPE = 0x8B4F,
    GL_CURRENT_PROGRAM = 0x8B8D, GL_MAX_VERTEX_ATTRIBS = 0x8869, GL_FRAMEBUFFER = 0x8D40, GL_READ_FRAMEBUFFER = 0x8CA8,
    GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_FRAMEBUFFER_BINDING = 0x8CA6, GL_VERTEX_ARRAY_BINDING = 0x85B5,
    GL_VERTEX_ATTRIB_ARRAY_ENABLED = 0x8622, GL_VERTEX_ATTRIB_ARRAY_SIZE = 0x8623, GL_CURRENT_VERTEX_ATTRIB = 0x8626
};

/* The GE's values (pspgu.h), for the state we send. */
enum {
    GU_POINTS = 0, GU_LINES = 1, GU_LINE_STRIP = 2, GU_TRIANGLES = 3, GU_TRIANGLE_STRIP = 4, GU_TRIANGLE_FAN = 5,
    GU_NEVER = 0, GU_ALWAYS = 1, GU_EQUAL = 2, GU_NOTEQUAL = 3, GU_LESS = 4, GU_LEQUAL = 5, GU_GREATER = 6, GU_GEQUAL = 7,
    GU_ADD = 0, GU_SUBTRACT = 1, GU_REVERSE_SUBTRACT = 2, GU_MIN = 3, GU_MAX = 4, GU_ABS = 5,
    GU_BL_COLOR = 0, GU_BL_ONE_MINUS_COLOR = 1, GU_SRC_ALPHA = 2, GU_ONE_MINUS_SRC_ALPHA = 3, GU_DST_ALPHA = 4,
    GU_ONE_MINUS_DST_ALPHA = 5, GU_FIX = 10,
    GU_TFX_MODULATE = 0, GU_TFX_DECAL = 1, GU_TFX_BLEND = 2, GU_TFX_REPLACE = 3, GU_TFX_ADD = 4,
    GU_TCC_RGB = 0, GU_TCC_RGBA = 1, GU_NEAREST = 0, GU_LINEAR = 1, GU_REPEAT = 0, GU_CLAMP = 1,
    GU_CW = 0, GU_CCW = 1, GU_DIRECTIONAL = 0, GU_POINTLIGHT = 1, GU_SPOTLIGHT = 2,
    GU_AMBIENT_AND_DIFFUSE = 0, GU_DIFFUSE_AND_SPECULAR = 1,
    GU_TEXTURE_COORDS = 0, GU_TEXTURE_MATRIX = 1, GU_ENVIRONMENT_MAP = 2, GU_POSITION = 0, GU_UV = 1,
    GU_MAT_AMBIENT = 1, GU_MAT_DIFFUSE = 2, GU_MAT_SPECULAR = 4
};

/* ---- the API macros -------------------------------------------------------- */

/* GLAPI: a function every OpenGL library exports. GLEXT: one found only by
 * name (extension entry points), exported on Linux all the same. */
#ifdef __APPLE__
#define GLAPI(ret, fn, params) \
    extern ret fn params; \
    static ret my_##fn params; \
    __attribute__((used)) static const struct { const void *replacement, *original; } \
        gl_interpose_##fn __attribute__((section("__DATA,__interpose"))) = { (const void *)&my_##fn, (const void *)&fn }; \
    static ret my_##fn params
#define GLEXT(ret, fn, params) static ret my_##fn params; static ret my_##fn params
#define GLFN(fn) my_##fn
#else
#define GLAPI(ret, fn, params) __attribute__((visibility("default"))) ret fn params
#define GLEXT(ret, fn, params) __attribute__((visibility("default"))) ret fn params
#define GLFN(fn) fn
#endif
/* Our own calls between GL functions go through GLFN so that on macOS they
 * reach ours, not the framework's. */

/* ---- state ----------------------------------------------------------------- */

typedef struct Vtx { float t[2]; uint32_t c; float n[3]; float p[3]; } Vtx;   /* one vertex, all attributes */

typedef struct GlTex {
    GLuint name;
    uint16_t handle;          /* 0: not on the PSP yet */
    int w, h;                 /* level 0 as given */
    int has_alpha;
    uint32_t *rgba;           /* level 0, RGBA8 (for sub-image updates and re-sending) */
    int dirty;                /* to be sent before the next draw with it */
    GLenum min_filter, mag_filter, wrap_s, wrap_t;
    int rt, rt_w, rt_h, rt_fmt;  /* a framebuffer object's colour: a render target on the PSP, drawn there */
    uint32_t psp_bytes;          /* what it takes on the PSP now (0: not there) */
    struct GlTex *hnext;         /* in the name table */
    unsigned used;               /* the frame it was last drawn with */
    uint32_t *face[6];           /* a cube map's faces, RGBA8 (only here: shaders' reflections are made from them) */
    float prescale;              /* rgba holds its colours times this, clamped (10-bit uploads: 4, keeping their bits) */
    float scale;                 /* what the draws want them times (a lightmap multiplied in more than twice over) */
    int part, part_x0, part_x1, part_y0, part_y1;  /* dirty only in this rectangle (glTexSubImage2D): sent as a patch when the PSP can */
    int sent_alpha, sent_mul;    /* how it was sent last: 4444 or 5650, its colours' factor */
    int face_w, face_h; uint32_t face_ver;
    struct GlTex *next;
} GlTex;

typedef struct GlBuf { GLuint name; uint8_t *data; size_t size; GLenum usage; int mapped; uint32_t version; struct GlBuf *next; } GlBuf;

typedef struct Array { int enabled, size; GLenum type; int stride; const uint8_t *ptr; GLuint buffer; int bgra; } Array;

typedef struct Light {
    int enabled;
    float ambient[4], diffuse[4], specular[4], position[4], spot_dir[3];
    float spot_exp, spot_cutoff, att[3];
} Light;

typedef struct Material { float ambient[4], diffuse[4], specular[4], emission[4], shininess; } Material;

/* A display list: the calls recorded with their arguments. */
typedef struct ListOp { uint16_t op; uint16_t nf; uint32_t i[3]; } ListOp;   /* followed by nf floats */
typedef struct List { GLuint name; uint8_t *ops; size_t len, cap; struct List *next; } List;

#define MV_DEPTH 32
#define PROJ_DEPTH 4
#define TEX_DEPTH 4
#define MAX_LIGHTS 8
#define CHUNK_VERTICES 1500      /* per 3D_DRAW message: ~54 KB at the largest vertex */

static struct {
    int inited;
    GLenum error;
    int log;
    int texmax;
    /* matrices */
    float mv[MV_DEPTH][16], proj[PROJ_DEPTH][16], texm[TEX_DEPTH][16];
    int mv_top, proj_top, tex_top;
    GLenum matrix_mode;
    /* current attributes */
    float color[4], normal[3], texcoord[4];
    /* immediate mode */
    int in_begin; GLenum prim;
    Vtx *imm; int nimm, cimm;
    /* arrays and buffers */
    Array va, ca, na, ta;
    int client_unit;
    GlBuf *buffers; GLuint array_buffer, element_buffer; GLuint next_buffer;
    /* enables */
    int depth_test, cull, blend, alpha_test, texture_2d, fog, lighting, color_material, scissor_test, dither;
    int polygon_offset, normalize;
    float po_factor, po_units;
    int texgen[2]; GLenum texgen_mode[2]; float texgen_obj[2][4], texgen_eye[2][4];   /* s, t */
    int active_unit;
    /* state values */
    GLenum depth_func; int depth_mask; float depth_near, depth_far; float clear_depth;
    GLenum blend_src, blend_dst, blend_eq; float blend_color[4];
    GLuint cube_units[8];         /* cube maps bound, per unit */
    GLenum alpha_func; float alpha_ref;
    GLenum cull_mode, front_face, shade_model;
    int color_mask[4]; float clear_color[4];
    int viewport[4], scissor[4];
    GLenum fog_mode; float fog_start, fog_end, fog_density, fog_color[4];
    GLenum tex_env_mode; float tex_env_color[4];
    Light light[MAX_LIGHTS];
    Material mat;
    float scene_ambient[4];
    GLenum cm_face, cm_mode;
    int unpack_alignment, unpack_row_length, unpack_skip_rows, unpack_skip_pixels;
    float point_size, line_width;
    /* textures */
    GlTex *textures; GLuint units[8]; GLuint next_texture;   /* the texture bound on each unit */
    int proxy_w, proxy_h;
    /* display lists */
    List *lists; GLuint next_list; List *compiling; GLenum list_mode; GLuint list_base; int list_depth;
    /* what the PSP has */
    PspExp3DState sent_state; int state_sent;
    float sent_proj[16], sent_mv[16], sent_tex[16]; int matrices_sent;
    /* texture coordinates for the draw being prepared */
    int tc;                     /* TC_* */
    float ge_texmat[16];        /* with TC_GE */
    PspExp3DLights sent_lights; int lights_sent;
    int lights_dirty;
    int lights_const;   /* the lights sent carry a draw's constant colour */
    /* a draw by a shader program (shader_draw): what its plan says, over GL's fixed state */
    int sh_active, sh_tex_alpha, sh_alpha_test;
    const GlslPlan *sh_lit;     /* the plan's lighting, done by the GE (NULL: none) */
    GLenum sh_alpha_func; float sh_alpha_ref;
    float sh_proj[16];
    GlTex *sh_tex;
    int sh_env, sh_env_alpha;   /* ...a pass drawing a cube map as a sphere map (the GE's environment map) */
    int sh_uvset, sh_replace;
    float pk_scale;             /* a draw with 16-bit positions: they are sent times this, the modelview sent over it (0: floats) */
    float pk_next;              /* ...chosen by draw_vertices for the prepare that follows (which takes it) */   /* ...a pass with another set of texture coordinates (glsl_plan_uv), its texture as is */
    float sh_env_axes[2][3];    /* ...its axes: the eye's x and y on the normals */
    /* GL 2: the generic vertex attributes (of the vertex array object bound), the program in use */
    Array gattr[16]; int gnorm[16]; float gcur[16][4];
    GLuint program, vao, fbo, next_object;
    GlTex *target;              /* the render target the PSP draws into now (NULL: the screen) */
    int reconnects;
    /* per frame */
    unsigned draws, vertices, clears;
    int frame_cleared;          /* the screen was cleared (by the program or here) this frame */
    Vtx *scratch; int cscratch;
} G;

static void gl_logf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    shim_gl_log("gl: %s", buf);
}

static void unsupported(const char *what)
{
    /* each unsupported call is logged once */
    static const char *seen[64];
    static int nseen;
    int i;
    for (i = 0; i < nseen; i++) if (seen[i] == what) return;
    if (nseen < 64) seen[nseen++] = what;
    gl_logf("not supported: %s", what);
}

static void set_error(GLenum e) { if (G.error == GL_NO_ERROR) G.error = e; }

static void mat_identity(float *m) { memset(m, 0, 16 * sizeof(float)); m[0] = m[5] = m[10] = m[15] = 1.0f; }

static void mat_mul(float *out, const float *a, const float *b)   /* out = a * b, column-major */
{
    float r[16];
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            r[j * 4 + i] = a[0 * 4 + i] * b[j * 4 + 0] + a[1 * 4 + i] * b[j * 4 + 1] +
                           a[2 * 4 + i] * b[j * 4 + 2] + a[3 * 4 + i] * b[j * 4 + 3];
    memcpy(out, r, sizeof(r));
}

static void xform_point(const float *m, const float *in, float *out)   /* 4-vector */
{
    int i;
    for (i = 0; i < 4; i++) out[i] = m[i] * in[0] + m[4 + i] * in[1] + m[8 + i] * in[2] + m[12 + i] * in[3];
}

static int mat_invert(const float *m, float *out)   /* general 4x4; -1 when singular */
{
    float inv[16], det;
    int i;
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) return -1;
    det = 1.0f / det;
    for (i = 0; i < 16; i++) out[i] = inv[i] * det;
    return 0;
}

static float *cur_matrix(void)
{
    switch (G.matrix_mode) {
    case GL_PROJECTION: return G.proj[G.proj_top];
    case GL_TEXTURE: return G.texm[G.tex_top];
    default: return G.mv[G.mv_top];
    }
}

static void init_state(void)
{
    int i;
    const char *e;
    memset(&G, 0, sizeof(G));
    G.inited = 1;
    G.log = getenv("PSPEXP_GL_LOG") ? atoi(getenv("PSPEXP_GL_LOG")) : 0;
    G.texmax = 256;
    if ((e = getenv("PSPEXP_GL_TEXMAX")) && atoi(e) >= 8) {
        G.texmax = 8;
        while (G.texmax * 2 <= atoi(e) && G.texmax < PSPEXP_TEXTURE_MAX) G.texmax *= 2;
    }
    mat_identity(G.mv[0]); mat_identity(G.proj[0]); mat_identity(G.texm[0]);
    G.matrix_mode = GL_MODELVIEW;
    G.color[0] = G.color[1] = G.color[2] = G.color[3] = 1.0f;
    G.normal[2] = 1.0f;
    G.texcoord[3] = 1.0f;
    G.depth_func = GL_LESS; G.depth_mask = 1; G.depth_far = 1.0f; G.clear_depth = 1.0f;
    G.blend_src = GL_ONE; G.blend_dst = GL_ZERO; G.blend_eq = GL_FUNC_ADD;
    G.alpha_func = GL_ALWAYS;
    G.cull_mode = GL_BACK; G.front_face = GL_CCW; G.shade_model = GL_SMOOTH;
    G.color_mask[0] = G.color_mask[1] = G.color_mask[2] = G.color_mask[3] = 1;
    G.dither = 1;
    G.fog_mode = GL_EXP; G.fog_density = 1.0f; G.fog_end = 1.0f;
    G.tex_env_mode = GL_MODULATE;
    for (i = 0; i < MAX_LIGHTS; i++) {
        Light *l = &G.light[i];
        l->ambient[3] = 1.0f;
        if (i == 0) { l->diffuse[0] = l->diffuse[1] = l->diffuse[2] = 1.0f; l->specular[0] = l->specular[1] = l->specular[2] = 1.0f; }
        l->diffuse[3] = l->specular[3] = 1.0f;
        l->position[2] = 1.0f;
        l->spot_dir[2] = -1.0f;
        l->spot_cutoff = 180.0f;
        l->att[0] = 1.0f;
    }
    G.mat.ambient[0] = G.mat.ambient[1] = G.mat.ambient[2] = 0.2f; G.mat.ambient[3] = 1.0f;
    G.mat.diffuse[0] = G.mat.diffuse[1] = G.mat.diffuse[2] = 0.8f; G.mat.diffuse[3] = 1.0f;
    G.mat.specular[3] = 1.0f; G.mat.emission[3] = 1.0f;
    G.scene_ambient[0] = G.scene_ambient[1] = G.scene_ambient[2] = 0.2f; G.scene_ambient[3] = 1.0f;
    G.cm_face = GL_FRONT_AND_BACK; G.cm_mode = GL_AMBIENT_AND_DIFFUSE;
    G.unpack_alignment = 4;
    G.point_size = G.line_width = 1.0f;
    G.next_texture = 1; G.next_list = 1; G.next_buffer = 1;
    G.lights_dirty = 1;
    G.reconnects = -1;
    { int a; for (a = 0; a < 16; a++) G.gcur[a][3] = 1.0f; }
    G.next_object = 1;
    G.texgen_mode[0] = G.texgen_mode[1] = GL_EYE_LINEAR;
    G.texgen_obj[0][0] = G.texgen_eye[0][0] = 1.0f;
    G.texgen_obj[1][1] = G.texgen_eye[1][1] = 1.0f;
    {
        int ww, wh, ox, oy; float s;
        shim_gl_geometry(&ww, &wh, &s, &ox, &oy);
        G.viewport[2] = G.scissor[2] = ww; G.viewport[3] = G.scissor[3] = wh;
    }
}

void pspexp_gl_init(void)
{
    if (!G.inited) {
        init_state();
        gl_logf("OpenGL 1.x on the PSP (textures up to %d px)", G.texmax);
    }
}

static void ensure_init(void) { if (!G.inited) init_state(); }

void pspexp_gl_refused(void) { ensure_init(); G.error = GL_INVALID_OPERATION; }

/* ---- to the PSP: state ---------------------------------------------------------- */

static uint8_t clamp8(float v) { return v <= 0.0f ? 0 : v >= 1.0f ? 255 : (uint8_t)(v * 255.0f + 0.5f); }
static uint32_t abgr(const float *c) { return clamp8(c[0]) | (uint32_t)clamp8(c[1]) << 8 | (uint32_t)clamp8(c[2]) << 16 | (uint32_t)clamp8(c[3]) << 24; }
static uint32_t bgr(const float *c) { return abgr(c) & 0xFFFFFFu; }

/* Textures by name: a table (programs bind one for nearly every draw, and
 * some have thousands: SuperTux spent 95 % of its time in a list walk). */
#define TEX_HASH 4096
static GlTex *tex_hash[TEX_HASH];

static GlTex *find_texture(GLuint name)
{
    GlTex *t;
    for (t = tex_hash[name & (TEX_HASH - 1)]; t; t = t->hnext) if (t->name == name) return t;
    return NULL;
}

static void texture_unhash(GlTex *t)
{
    GlTex **pp = &tex_hash[t->name & (TEX_HASH - 1)];
    while (*pp && *pp != t) pp = &(*pp)->hnext;
    if (*pp) *pp = t->hnext;
}

/* GL's depth runs 0 near .. 1 far; the GE's, as the PSP samples set it up,
 * 65535 near .. 0 far: the comparisons turn around. */
static uint8_t depth_func(GLenum f)
{
    switch (f) {
    case GL_NEVER: return GU_NEVER;     case GL_ALWAYS: return GU_ALWAYS;
    case GL_EQUAL: return GU_EQUAL;     case GL_NOTEQUAL: return GU_NOTEQUAL;
    case GL_LESS: return GU_GREATER;    case GL_LEQUAL: return GU_GEQUAL;
    case GL_GREATER: return GU_LESS;    case GL_GEQUAL: return GU_LEQUAL;
    default: return GU_ALWAYS;
    }
}

static uint8_t alpha_func(GLenum f)
{
    switch (f) {
    case GL_NEVER: return GU_NEVER;     case GL_ALWAYS: return GU_ALWAYS;
    case GL_EQUAL: return GU_EQUAL;     case GL_NOTEQUAL: return GU_NOTEQUAL;
    case GL_LESS: return GU_LESS;       case GL_LEQUAL: return GU_LEQUAL;
    case GL_GREATER: return GU_GREATER; case GL_GEQUAL: return GU_GEQUAL;
    default: return GU_ALWAYS;
    }
}

/* One blend factor. `src`: the source slot, whose "colour" choice is the
 * destination's colour (and the other way round). The framebuffer is 5650:
 * no destination alpha, taken as 1. ONE and ZERO are fixed colours. */
static uint8_t blend_factor(GLenum f, int src, uint32_t *fix)
{
    *fix = 0;
    switch (f) {
    case GL_ZERO: return GU_FIX;
    case GL_ONE: case GL_SRC_ALPHA_SATURATE: case GL_DST_ALPHA: *fix = 0xFFFFFF; return GU_FIX;
    case GL_ONE_MINUS_DST_ALPHA: return GU_FIX;
    case GL_SRC_ALPHA: return GU_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return GU_ONE_MINUS_SRC_ALPHA;
    case GL_DST_COLOR: if (src) return GU_BL_COLOR; *fix = 0xFFFFFF; return GU_FIX;
    case GL_ONE_MINUS_DST_COLOR: if (src) return GU_BL_ONE_MINUS_COLOR; return GU_FIX;
    case GL_SRC_COLOR: if (!src) return GU_BL_COLOR; *fix = 0xFFFFFF; return GU_FIX;
    case GL_ONE_MINUS_SRC_COLOR: if (!src) return GU_BL_ONE_MINUS_COLOR; return GU_FIX;
    default: *fix = 0xFFFFFF; return GU_FIX;
    }
}

static uint8_t blend_op(GLenum e)
{
    switch (e) {
    case GL_FUNC_SUBTRACT: return GU_SUBTRACT;
    case GL_FUNC_REVERSE_SUBTRACT: return GU_REVERSE_SUBTRACT;
    case GL_MIN: return GU_MIN;
    case GL_MAX: return GU_MAX;
    default: return GU_ADD;
    }
}

/* GL window coordinates (origin bottom left) to the PSP's screen. */
static void to_psp_rect(const int *r, int16_t *x, int16_t *y, int16_t *w, int16_t *h, int centre)
{
    int ww, wh, ox, oy;
    float s, x0, y0, x1, y1;
    if (G.target) {   /* a render target: its texture's pixels scaled to the target's, row 0 first (no flip) */
        float sx = (float)G.target->rt_w / (float)G.target->w, sy = (float)G.target->rt_h / (float)G.target->h;
        x0 = r[0] * sx; x1 = (r[0] + r[2]) * sx;
        y0 = r[1] * sy; y1 = (r[1] + r[3]) * sy;
        if (centre) { *x = (int16_t)lroundf((x0 + x1) / 2); *y = (int16_t)lroundf((y0 + y1) / 2); }
        else { *x = (int16_t)lroundf(x0); *y = (int16_t)lroundf(y0); }
        *w = (int16_t)lroundf(x1 - x0); *h = (int16_t)lroundf(y1 - y0);
        if (centre) *h = (int16_t)-*h;   /* the viewport: GL's y upwards along the rows */
        return;
    }
    shim_gl_geometry(&ww, &wh, &s, &ox, &oy);
    x0 = ox + r[0] * s; x1 = ox + (r[0] + r[2]) * s;
    y0 = oy + (wh - (r[1] + r[3])) * s; y1 = oy + (wh - r[1]) * s;
    if (centre) { *x = (int16_t)lroundf((x0 + x1) / 2); *y = (int16_t)lroundf((y0 + y1) / 2); }
    else { *x = (int16_t)lroundf(x0); *y = (int16_t)lroundf(y0); }
    *w = (int16_t)lroundf(x1 - x0); *h = (int16_t)lroundf(y1 - y0);
}

static int texture_matrix_identity(void);

/* Where a draw's texture coordinates come from. TC_PLAIN: the vertices'.
 * TC_GE: the GE makes them, its texture matrix times the vertex position
 * (linear generation of s and t) or times the vertices' own (a texture
 * matrix): nothing per vertex here, and meshes stay valid. TC_HOST: made
 * here per vertex (sphere maps, one coordinate generated). */
enum { TC_PLAIN, TC_GE, TC_HOST, TC_ENV };

static int lights_enabled(void) { int i, n = 0; for (i = 0; i < MAX_LIGHTS; i++) n += G.light[i].enabled; return n; }

static int texcoord_mode(float *ge)
{
    int gen = G.texgen[0] || G.texgen[1], c, i, j;
    float g[16], t[16];
    mat_identity(ge);
    if (!gen && texture_matrix_identity()) return TC_PLAIN;
    /* a sphere map as the GE's environment map: (1 + n.x) / 2, (1 + n.y) / 2
     * of the eye-space normal, GL's own for a view straight ahead; its two
     * axes take the last two light slots */
    if (G.texgen[0] && G.texgen[1] && G.texgen_mode[0] == GL_SPHERE_MAP && G.texgen_mode[1] == GL_SPHERE_MAP &&
        texture_matrix_identity() && (!G.lighting || lights_enabled() <= PSPEXP_3D_LIGHTS - 2))
        return TC_ENV;
    if (gen && !(G.texgen[0] && G.texgen[1] && G.texgen_mode[0] != GL_SPHERE_MAP && G.texgen_mode[1] != GL_SPHERE_MAP)) return TC_HOST;
    mat_identity(g);
    if (gen) {
        const float *mv = G.mv[G.mv_top];
        memset(g, 0, sizeof(g));
        g[15] = 1.0f;
        for (c = 0; c < 2; c++) {
            float row[4];
            if (G.texgen_mode[c] == GL_OBJECT_LINEAR) memcpy(row, G.texgen_obj[c], sizeof(row));
            else for (j = 0; j < 4; j++) {   /* eye plane times the modelview: a plane on the object's coordinates */
                row[j] = 0;
                for (i = 0; i < 4; i++) row[j] += G.texgen_eye[c][i] * mv[j * 4 + i];
            }
            for (j = 0; j < 4; j++) g[j * 4 + c] = row[j];
        }
    }
    mat_mul(t, G.texm[G.tex_top], g);
    for (j = 0; j < 4; j++) {   /* the GE's rows: s, t and q (GL's fourth) */
        ge[j * 4 + 0] = t[j * 4 + 0]; ge[j * 4 + 1] = t[j * 4 + 1]; ge[j * 4 + 2] = t[j * 4 + 3]; ge[j * 4 + 3] = 0.0f;
    }
    if (!gen) ge[8] = ge[9] = ge[10] = 0.0f;   /* a vertex's (u, v): no third coordinate, whatever the GE puts there */
    return TC_GE;
}

static void build_state(PspExp3DState *s, GlTex *tex)
{
    uint32_t fix;
    memset(s, 0, sizeof(*s));
    if (G.depth_test) s->enable |= PSPEXP_3D_DEPTH_TEST;
    if (G.depth_mask) s->enable |= PSPEXP_3D_DEPTH_WRITE;
    if (G.cull && G.cull_mode != GL_FRONT_AND_BACK) s->enable |= PSPEXP_3D_CULL;
    if (G.blend) s->enable |= PSPEXP_3D_BLEND;
    if (G.alpha_test) s->enable |= PSPEXP_3D_ALPHA_TEST;
    if (tex) s->enable |= PSPEXP_3D_TEXTURE;
    if (G.fog) s->enable |= PSPEXP_3D_FOG;
    if (G.lighting) s->enable |= PSPEXP_3D_LIGHTING;
    if (G.shade_model == GL_SMOOTH) s->enable |= PSPEXP_3D_SMOOTH;
    if (G.dither) s->enable |= PSPEXP_3D_DITHER;
    s->depth_func = depth_func(G.depth_func);
    s->alpha_func = alpha_func(G.alpha_func);
    s->alpha_ref = clamp8(G.alpha_ref);
    /* The winding kept when culling: GL's front one (the GE sees it the
     * same way round, its viewport flipping y as GL's does); culling the
     * front faces keeps the other. */
    {
        int front_ccw = G.front_face == GL_CCW;
        if (G.cull_mode == GL_FRONT) front_ccw = !front_ccw;
        if (G.target) front_ccw = !front_ccw;   /* its viewport does not flip y: the winding turns round */
        s->front_face = front_ccw ? GU_CCW : GU_CW;
    }
    s->blend_op = blend_op(G.blend_eq);
    s->blend_src = blend_factor(G.blend_src, 1, &fix); s->blend_fix_src = fix;
    s->blend_dst = blend_factor(G.blend_dst, 0, &fix); s->blend_fix_dst = fix;
    switch (G.tex_env_mode) {
    case GL_REPLACE: s->tex_func = GU_TFX_REPLACE; break;
    case GL_DECAL: s->tex_func = GU_TFX_DECAL; break;
    case GL_BLEND: s->tex_func = GU_TFX_BLEND; break;
    case GL_ADD: s->tex_func = GU_TFX_ADD; break;
    default: s->tex_func = GU_TFX_MODULATE; break;
    }
    s->tex_alpha = tex && !tex->has_alpha ? GU_TCC_RGB : GU_TCC_RGBA;
    if (tex) {
        s->texture = tex->handle;
        s->tex_min = tex->min_filter == GL_NEAREST || tex->min_filter == GL_NEAREST_MIPMAP_NEAREST ||
                     tex->min_filter == GL_NEAREST_MIPMAP_LINEAR ? GU_NEAREST : GU_LINEAR;
        s->tex_mag = tex->mag_filter == GL_NEAREST ? GU_NEAREST : GU_LINEAR;
        s->tex_wrap_s = tex->wrap_s == GL_REPEAT || tex->wrap_s == GL_MIRRORED_REPEAT ? GU_REPEAT : GU_CLAMP;
        s->tex_wrap_t = tex->wrap_t == GL_REPEAT || tex->wrap_t == GL_MIRRORED_REPEAT ? GU_REPEAT : GU_CLAMP;
    }
    s->tex_env_color = bgr(G.tex_env_color);
    s->fog_color = bgr(G.fog_color);
    if (G.fog_mode == GL_LINEAR) { s->fog_near = G.fog_start; s->fog_far = G.fog_end; }
    else {   /* exponential fog, as a straight line to where it is nearly opaque */
        float d = G.fog_density > 1e-6f ? G.fog_density : 1e-6f;
        s->fog_near = 0.0f; s->fog_far = (G.fog_mode == GL_EXP2 ? 1.7f : 2.5f) / d;
    }
    if (s->fog_far <= s->fog_near) s->fog_far = s->fog_near + 1e-3f;
    s->tex_scale_u = s->tex_scale_v = 1.0f;
    s->tex_map = GU_TEXTURE_COORDS;
    s->tex_source = GU_UV;
    if (tex && G.tc == TC_GE) {
        s->tex_map = GU_TEXTURE_MATRIX;
        s->tex_source = G.texgen[0] ? GU_POSITION : GU_UV;
    } else if (tex && G.tc == TC_ENV) {   /* (u from the second light given: see PspExp3DState) */
        s->tex_map = GU_ENVIRONMENT_MAP;
        s->tex_env_lights = (PSPEXP_3D_LIGHTS - 1) | (PSPEXP_3D_LIGHTS - 2) << 4;
    }
    to_psp_rect(G.viewport, &s->viewport_x, &s->viewport_y, &s->viewport_w, &s->viewport_h, 1);
    {   /* GL's 0..1 onto the GE's 65535 (near) .. 0, a little short of 65535: glPolygonOffset has no GE
         * counterpart, so an offset draw is moved into that headroom instead (a decal on a floor, a car's
         * shadow on the road: with the 16-bit depth buffer, they flicker against it without) */
        const float room = 1024.0f, scale = (65535.0f - room) / 65535.0f;
        float n = 65535.0f * (1.0f - G.depth_near) * scale, f = 65535.0f * (1.0f - G.depth_far) * scale, bias = 0;
        if (G.polygon_offset) bias = -(G.po_factor * 128.0f + G.po_units * 16.0f);   /* negative factor, units: nearer */
        if (bias > room) bias = room;
        n += bias; f += bias;
        s->depth_near = (uint16_t)lroundf(n < 0 ? 0 : n > 65535 ? 65535 : n);
        s->depth_far = (uint16_t)lroundf(f < 0 ? 0 : f > 65535 ? 65535 : f);
    }
    if (G.scissor_test) to_psp_rect(G.scissor, &s->scissor_x, &s->scissor_y, &s->scissor_w, &s->scissor_h, 0);
    else {
        int all[4];
        int ww, wh, ox, oy; float sc;
        shim_gl_geometry(&ww, &wh, &sc, &ox, &oy);
        if (G.target) { ww = G.target->w; wh = G.target->h; }
        all[0] = all[1] = 0; all[2] = ww; all[3] = wh;
        to_psp_rect(all, &s->scissor_x, &s->scissor_y, &s->scissor_w, &s->scissor_h, 0);
    }
    s->pixel_mask = (G.color_mask[0] ? 0 : 0xFFu) | (G.color_mask[1] ? 0 : 0xFF00u) | (G.color_mask[2] ? 0 : 0xFF0000u);
    {   /* PSPEXP_GL_DEBUG=nocull,nodepth,noblend,noalpha,notex: parts left out (bisecting a difference) */
        static const char *dbg;
        static int read;
        if (!read) { read = 1; dbg = getenv("PSPEXP_GL_DEBUG"); }
        if (dbg) {
            if (strstr(dbg, "nocull")) s->enable &= ~(uint32_t)PSPEXP_3D_CULL;
            if (strstr(dbg, "nodepth")) s->enable &= ~(uint32_t)PSPEXP_3D_DEPTH_TEST;
            if (strstr(dbg, "noblend")) s->enable &= ~(uint32_t)PSPEXP_3D_BLEND;
            if (strstr(dbg, "noalpha")) s->enable &= ~(uint32_t)PSPEXP_3D_ALPHA_TEST;
            if (strstr(dbg, "notex")) s->enable &= ~(uint32_t)PSPEXP_3D_TEXTURE;
        }
    }
    if (G.sh_active) {   /* a shader's draw: its plan, not the fixed functions */
        s->enable &= ~(uint32_t)(PSPEXP_3D_LIGHTING | PSPEXP_3D_FOG);
        if (G.sh_lit) s->enable |= PSPEXP_3D_LIGHTING | PSPEXP_3D_SMOOTH;
        if (G.sh_alpha_test) {
            s->enable |= PSPEXP_3D_ALPHA_TEST;
            s->alpha_func = alpha_func(G.sh_alpha_func);
            s->alpha_ref = clamp8(G.sh_alpha_ref);
        }
        s->tex_func = GU_TFX_MODULATE;
        s->tex_alpha = tex && G.sh_tex_alpha && tex->has_alpha ? GU_TCC_RGBA : GU_TCC_RGB;
        s->tex_map = GU_TEXTURE_COORDS;
        if (G.sh_replace && tex) { s->tex_func = GU_TFX_REPLACE; s->tex_alpha = GU_TCC_RGB; }   /* blended in by the pass */
        if (G.sh_env && tex) {   /* the sphere map, from the normals: its colour as it is */
            s->tex_map = GU_ENVIRONMENT_MAP;
            s->tex_env_lights = (PSPEXP_3D_LIGHTS - 1) | (PSPEXP_3D_LIGHTS - 2) << 4;
            s->tex_func = GU_TFX_REPLACE;
            s->tex_alpha = G.sh_env_alpha ? GU_TCC_RGBA : GU_TCC_RGB;
        }
    }
}

/* `color`: vertices without a colour of their own (meshes): the current
 * colour comes as the material's, which the GE takes for such vertices
 * (the ambient one when unlit; the colour-material ones when lit). */
static void build_lights(PspExp3DLights *l, const float *color)
{
    int i, n = 0;
    memset(l, 0, sizeof(*l));
    if (G.sh_env) {   /* no lighting: the environment map's axes only */
        memcpy(l->light[PSPEXP_3D_LIGHTS - 2].position, G.sh_env_axes[0], sizeof(G.sh_env_axes[0]));
        memcpy(l->light[PSPEXP_3D_LIGHTS - 1].position, G.sh_env_axes[1], sizeof(G.sh_env_axes[1]));
        return;
    }
    if (G.sh_lit) {   /* a shader's lighting: one directional light, white material, its ambient as the scene's */
        const GlslPlan *pl = G.sh_lit;
        float amb[4] = { pl->light_ambient[0], pl->light_ambient[1], pl->light_ambient[2], 1 };
        float dif[4] = { pl->light_diffuse[0], pl->light_diffuse[1], pl->light_diffuse[2], 1 };
        static const float white[4] = { 1, 1, 1, 1 }, black[4] = { 0, 0, 0, 1 };
        l->scene_ambient = abgr(amb);
        l->emissive = abgr(black) & 0xFFFFFF;
        l->ambient = abgr(white);
        l->diffuse = abgr(white);
        l->specular = 0;
        l->light[0].enabled = 1;
        l->light[0].type = GU_DIRECTIONAL;
        l->light[0].components = GU_AMBIENT_AND_DIFFUSE;
        l->light[0].diffuse = bgr(dif);
        memcpy(l->light[0].position, pl->light_dir, sizeof(l->light[0].position));
        l->light[0].attenuation[0] = 1;
        return;
    }
    l->scene_ambient = abgr(G.scene_ambient);
    l->emissive = abgr(G.mat.emission);
    l->ambient = abgr(G.mat.ambient);
    l->diffuse = abgr(G.mat.diffuse);
    l->specular = abgr(G.mat.specular);
    l->shininess = G.mat.shininess;
    if (color) {
        uint32_t c = abgr(color);
        if (!G.lighting) l->ambient = l->diffuse = c;
        else if (G.color_material) {
            if (G.cm_mode == GL_AMBIENT || G.cm_mode == GL_AMBIENT_AND_DIFFUSE) l->ambient = c;
            if (G.cm_mode == GL_DIFFUSE || G.cm_mode == GL_AMBIENT_AND_DIFFUSE) l->diffuse = c;
            if (G.cm_mode == GL_SPECULAR) l->specular = c;
        }
    } else if (G.color_material) {
        switch (G.cm_mode) {
        case GL_AMBIENT: l->color_material = GU_MAT_AMBIENT; break;
        case GL_DIFFUSE: l->color_material = GU_MAT_DIFFUSE; break;
        case GL_SPECULAR: l->color_material = GU_MAT_SPECULAR; break;
        case GL_AMBIENT_AND_DIFFUSE: l->color_material = GU_MAT_AMBIENT | GU_MAT_DIFFUSE; break;
        default: break;
        }
    }
    if (G.tc == TC_ENV) {   /* the environment map's axes (lights that are not on) */
        l->light[PSPEXP_3D_LIGHTS - 2].position[0] = 1.0f;
        l->light[PSPEXP_3D_LIGHTS - 1].position[1] = 1.0f;
    }
    for (i = 0; i < MAX_LIGHTS && n < (G.tc == TC_ENV ? PSPEXP_3D_LIGHTS - 2 : PSPEXP_3D_LIGHTS); i++) {
        const Light *g = &G.light[i];
        PspExp3DLight *o;
        if (!g->enabled) continue;
        o = &l->light[n++];
        o->enabled = 1;
        if (g->position[3] == 0.0f) o->type = GU_DIRECTIONAL;
        else o->type = g->spot_cutoff < 180.0f ? GU_SPOTLIGHT : GU_POINTLIGHT;
        o->components = (g->specular[0] > 0 || g->specular[1] > 0 || g->specular[2] > 0) && G.mat.shininess >= 0
                        ? GU_DIFFUSE_AND_SPECULAR : GU_AMBIENT_AND_DIFFUSE;
        o->ambient = bgr(g->ambient); o->diffuse = bgr(g->diffuse); o->specular = bgr(g->specular);
        if (g->position[3] != 0.0f && g->position[3] != 1.0f) {
            o->position[0] = g->position[0] / g->position[3];
            o->position[1] = g->position[1] / g->position[3];
            o->position[2] = g->position[2] / g->position[3];
        } else memcpy(o->position, g->position, sizeof(o->position));
        memcpy(o->direction, g->spot_dir, sizeof(o->direction));
        memcpy(o->attenuation, g->att, sizeof(o->attenuation));
        o->spot_exponent = g->spot_exp;
        o->spot_cutoff = cosf(g->spot_cutoff * 3.14159265f / 180.0f);
    }
}

/* ---- to the PSP: textures --------------------------------------------------------- */

static int pow2_at_least(int v, int max)
{
    int p = 8;
    while (p < v && p < max) p *= 2;
    return p;
}

/* Level 0 resampled (box filter when shrinking, nearest when growing) to a
 * power of two, 4444 when it has alpha, else 5650, swizzled for the GE. */
static uint16_t mesh_handle(void);
static void mesh_release(uint16_t h, uint32_t bytes);
static int mesh_room(uint32_t bytes);
static void frame_restart(void);
static int texture_fits(PspExpContext *ctx, GlTex *keep, uint32_t bytes);
static uint32_t texture_psp_bytes(const GlTex *t);
static uint16_t free_handles[4096];
static int nfree_handles;
static uint32_t tex_bytes;          /* textures on the PSP (texture_room) */
static unsigned tex_frame = 1;
static void texture_room(PspExpContext *ctx, GlTex *keep, uint32_t bytes);
static GlTex *fbo_texture(GLuint fb);

/* Where the draws go: the render target of the framebuffer object bound
 * (made on the PSP the first time), or the screen. 0: nowhere (a
 * framebuffer object without a texture). */
static int select_target(PspExpContext *ctx)
{
    GlTex *want = NULL;
    if (G.fbo && !(want = fbo_texture(G.fbo))) return 0;
    if (want && !want->rt) {
        int bytes;
        want->rt = 1;
        want->rt_w = 8; while (want->rt_w < want->w && want->rt_w < PSPEXP_TEXTURE_MAX) want->rt_w *= 2;
        want->rt_h = 8; while (want->rt_h < want->h && want->rt_h < PSPEXP_TEXTURE_MAX) want->rt_h *= 2;
        want->rt_fmt = want->has_alpha ? PSPEXP_TEX_8888 : PSPEXP_TEX_5650;
        bytes = want->rt_w * want->rt_h * (want->rt_fmt == PSPEXP_TEX_8888 ? 4 : 2);
        if (bytes > 512 * 1024) want->rt_fmt = want->has_alpha ? PSPEXP_TEX_4444 : PSPEXP_TEX_5650;
        if (want->handle) mesh_release(want->handle, 0);
        tex_bytes -= want->psp_bytes;
        want->psp_bytes = (uint32_t)want->rt_w * (uint32_t)want->rt_h * (want->rt_fmt == PSPEXP_TEX_8888 ? 4u : 2u);
        texture_room(ctx, want, want->psp_bytes);
        tex_bytes += want->psp_bytes;
        want->handle = mesh_handle();
        want->dirty = 0;
        pspexp_create_target(ctx, want->handle, (uint16_t)want->rt_w, (uint16_t)want->rt_h, (uint8_t)want->rt_fmt);
        if (G.log) gl_logf("render target %u: texture %u (%dx%d) as %dx%d", want->handle, want->name, want->w, want->h, want->rt_w, want->rt_h);
    }
    if (want != G.target) {
        pspexp_3d_target(ctx, want ? want->handle : PSPEXP_TARGET_SCREEN);
        G.target = want;
        G.state_sent = 0;   /* viewport and scissor are the target's now */
    }
    return 1;
}

/* Textures on the PSP, against its memory: the least recently drawn go
 * (released now: not drawn this frame, so no draw of it is waiting) and are
 * sent again when drawn next. Meshes have their own share (MESH_BUDGET). */
static uint32_t texture_budget(PspExpContext *ctx)
{
    const PspExpHello *h = pspexp_caps(ctx);
    uint32_t total = h->ram_budget + h->vram_budget, mesh = 7u * 1024u * 1024u;
    return total > mesh + 1024u * 1024u ? total - mesh : total / 2;
}

/* Whether `bytes` more would fit once what was not drawn this frame went. */
static int texture_fits(PspExpContext *ctx, GlTex *keep, uint32_t bytes)
{
    GlTex *t;
    uint32_t freeable = 0;
    if (tex_bytes + bytes <= texture_budget(ctx)) return 1;
    for (t = G.textures; t; t = t->next)
        if (t != keep && t->handle && t->psp_bytes && !t->rt && t->used != tex_frame) freeable += t->psp_bytes;
    return tex_bytes - freeable + bytes <= texture_budget(ctx);
}

static uint32_t texture_psp_bytes(const GlTex *t)
{
    return (uint32_t)pow2_at_least(t->w, G.texmax) * (uint32_t)pow2_at_least(t->h, G.texmax) * 2u;
}

static void texture_room(PspExpContext *ctx, GlTex *keep, uint32_t bytes)
{
    uint32_t budget = texture_budget(ctx);
    while (tex_bytes + bytes > budget) {
        GlTex *t, *oldest = NULL;
        for (t = G.textures; t; t = t->next)
            if (t != keep && t->handle && t->psp_bytes && !t->rt && t->used != tex_frame && (!oldest || t->used < oldest->used)) oldest = t;
        if (!oldest) return;   /* all of it drawn this frame: the PSP says so if it cannot */
        pspexp_release(ctx, oldest->handle);
        free_handles[nfree_handles < 4096 ? nfree_handles++ : 4095] = oldest->handle;
        oldest->handle = 0;
        tex_bytes -= oldest->psp_bytes;
        oldest->psp_bytes = 0;
        oldest->dirty = 1;
    }
}

static void send_texture(PspExpContext *ctx, GlTex *t)
{
    PspExpTextureDesc d;
    int pw = pow2_at_least(t->w, G.texmax), ph = pow2_at_least(t->h, G.texmax), x, y;
    uint16_t *lin;
    uint8_t *dst;
    uint32_t bytes = (uint32_t)pw * (uint32_t)ph * 2u;
    int mul = (int)lroundf(256.0f * t->scale / (t->prescale > 0 ? t->prescale : 1)), alpha = t->has_alpha, part = t->part;
    t->dirty = 0;
    t->part = 0;
    if (t->rt || !t->rgba || t->w <= 0 || t->h <= 0) return;   /* a render target: drawn on the PSP */
    if (part && t->handle && t->psp_bytes == bytes && pw == t->w && ph == t->h && mul == t->sent_mul && pspexp_patch_supported(ctx)) {
        /* only a rectangle changed: its 8 x 8 blocks (16-bit texels: 16 bytes by 8 rows each), which the
         * swizzled layout keeps left to right in each 8-row strip: one run of bytes per strip */
        int x0 = t->part_x0 & ~7, x1 = (t->part_x1 + 7) & ~7, y0 = t->part_y0 & ~7, y1 = (t->part_y1 + 7) & ~7, ok = 1, by;
        uint32_t sent = 0;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > pw) x1 = pw;
        if (y1 > ph) y1 = ph;
        if (!t->sent_alpha)   /* sent without alpha, which it has now: whole again */
            for (y = y0; y < y1 && ok; y++) for (x = x0; x < x1; x++) if ((t->rgba[(size_t)y * (size_t)t->w + (size_t)x] >> 24) != 255) { ok = 0; break; }
        for (by = y0; ok && by < y1; by += 8) {
            uint32_t off = (uint32_t)by * (uint32_t)pw * 2u + (uint32_t)(x0 / 8) * 128u, len = (uint32_t)((x1 - x0) / 8) * 128u;
            uint16_t *o;
            int bx, r, c;
            if (!(dst = pspexp_patch_texture_begin(ctx, t->handle, off, len))) { ok = 0; break; }
            o = (uint16_t *)(void *)dst;
            for (bx = x0; bx < x1; bx += 8)
                for (r = 0; r < 8; r++)
                    for (c = 0; c < 8; c++) {
                        uint32_t px = t->rgba[(size_t)(by + r) * (size_t)t->w + (size_t)(bx + c)], rr = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF, a = px >> 24;
                        if (mul != 256) {
                            rr = rr * (uint32_t)mul >> 8; g = g * (uint32_t)mul >> 8; b = b * (uint32_t)mul >> 8;
                            rr = rr > 255 ? 255 : rr; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
                        }
                        *o++ = t->sent_alpha ? pspexp_color_4444((uint8_t)rr, (uint8_t)g, (uint8_t)b, (uint8_t)a) : pspexp_color_5650((uint8_t)rr, (uint8_t)g, (uint8_t)b);
                    }
            pspexp_patch_texture_commit(ctx);
            sent += len;
        }
        if (ok && y1 > y0 && x1 > x0) {
            if (G.log >= 2) gl_logf("texture %u patched: %d,%d .. %d,%d (%u bytes)", t->name, x0, y0, x1, y1, sent);
            return;
        }
        /* (a strip could not go: the whole texture, below) */
    }
    if (alpha) {   /* all opaque after all (lightmaps are RGBA): 5650, more bits of colour */
        size_t i, n = (size_t)t->w * (size_t)t->h;
        for (i = 0; i < n && (t->rgba[i] >> 24) == 255; i++) { }
        if (i == n) alpha = 0;
    }
    tex_bytes -= t->psp_bytes;
    t->psp_bytes = 0;
    texture_room(ctx, t, bytes);
    if (t->handle) pspexp_release(ctx, t->handle);
    else t->handle = mesh_handle();
    t->psp_bytes = bytes;
    tex_bytes += bytes;
    if (G.log >= 2) gl_logf("texture %u sent: %dx%d as %dx%d", t->name, t->w, t->h, pw, ph);
    lin = (uint16_t *)malloc(bytes);
    if (!lin) return;
    for (y = 0; y < ph; y++) {
        int sy0 = y * t->h / ph, sy1 = (y + 1) * t->h / ph;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (x = 0; x < pw; x++) {
            int sx0 = x * t->w / pw, sx1 = (x + 1) * t->w / pw, sx, sy, n = 0;
            uint32_t r = 0, g = 0, b = 0, a = 0;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            for (sy = sy0; sy < sy1; sy++)
                for (sx = sx0; sx < sx1; sx++) {
                    uint32_t p = t->rgba[sy * t->w + sx];
                    r += p & 0xFF; g += (p >> 8) & 0xFF; b += (p >> 16) & 0xFF; a += p >> 24; n++;
                }
            r /= n; g /= n; b /= n; a /= n;
            if (mul != 256) {   /* its colours as the draws want them, from what is kept */
                r = r * (uint32_t)mul >> 8; g = g * (uint32_t)mul >> 8; b = b * (uint32_t)mul >> 8;
                r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
            }
            lin[y * pw + x] = alpha ? pspexp_color_4444((uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a)
                                    : pspexp_color_5650((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
    }
    if (getenv("PSPEXP_GL_DUMP")) {
        char path[512];
        FILE *f;
        snprintf(path, sizeof(path), "%s/tex-%u-%dx%d.rgba", getenv("PSPEXP_GL_DUMP"), t->name, pw, ph);
        if ((f = fopen(path, "wb"))) {
            for (y = 0; y < pw * ph; y++) {
                uint8_t r, g, b, a = 255, px[4];
                if (alpha) { uint16_t c = lin[y]; r = (uint8_t)((c & 15) * 17); g = (uint8_t)(((c >> 4) & 15) * 17); b = (uint8_t)(((c >> 8) & 15) * 17); a = (uint8_t)((c >> 12) * 17); }
                else pspexp_color_unpack_5650(lin[y], &r, &g, &b);
                px[0] = r; px[1] = g; px[2] = b; px[3] = a;
                fwrite(px, 1, 4, f);
            }
            fclose(f);
        }
    }
    memset(&d, 0, sizeof(d));
    d.width = (uint16_t)pw; d.height = (uint16_t)ph;
    d.format = alpha ? PSPEXP_TEX_4444 : PSPEXP_TEX_5650;
    t->sent_alpha = alpha; t->sent_mul = mul;
    d.layout = PSPEXP_LAYOUT_SWIZZLED;
    d.priority = PSPEXP_PRIORITY_NORMAL;
    dst = pspexp_upload_texture_begin(ctx, t->handle, &d, bytes, NULL, 0);
    if (dst) {
        pspexp_swizzle(dst, lin, (uint32_t)pw * 2u, (uint32_t)ph);
        pspexp_upload_texture_commit(ctx, 1);
    }
    free(lin);
}

/* ---- to the PSP: draws --------------------------------------------------------------- */

/* The frame, with the state, matrices and lights the next draw needs sent. */
static PspExpContext *prepare_colour(GlTex **texp, const float *color);
static PspExpContext *prepare(GlTex **texp) { return prepare_colour(texp, NULL); }

static PspExpContext *prepare_colour(GlTex **texp, const float *color)
{
    PspExpContext *ctx = shim_gl_frame();
    GlTex *tex = NULL;
    PspExp3DState st;
    G.pk_scale = G.pk_next; G.pk_next = 0;   /* this draw's, or none */
    if (!ctx) return NULL;
    if (shim_gl_reconnects() != G.reconnects) {   /* the PSP has none of it */
        GlTex *t;
        G.reconnects = shim_gl_reconnects();
        G.state_sent = G.matrices_sent = G.lights_sent = 0;
        for (t = G.textures; t; t = t->next) if (t->handle && !t->rt) t->dirty = 1;
        (void)0;   /* (the library resends what it kept: sizes are unchanged) */
    }
    {   /* a texture to send that does not fit beside this frame's: the frame goes now (a program
         * loading draws hundreds of images into a target without ever swapping) */
        GlTex *t = G.sh_active ? G.sh_tex : G.texture_2d ? find_texture(G.units[0]) : NULL;
        if (t && t->rgba && !t->rt && (t->dirty || !t->handle) && !texture_fits(ctx, t, texture_psp_bytes(t) - t->psp_bytes)) {
            if (G.log) gl_logf("frame sent early: textures to make room for");
            shim_gl_flush();
            frame_restart();
            if (!(ctx = shim_gl_frame())) return NULL;
        }
    }
    if (!select_target(ctx)) return NULL;
    if (!G.frame_cleared && !G.target) {
        /* a frame that draws without clearing first: black under it. After a
         * swap GL leaves the buffer undefined, so nothing is lost; PPSSPP,
         * which loses track of a framebuffer that is never cleared, then
         * copies the right frame to screenshots */
        G.frame_cleared = 1;
        pspexp_3d_clear(ctx, PSPEXP_3D_CLEAR_COLOR | PSPEXP_3D_CLEAR_DEPTH, 0xFF000000u, 0);
    }
    if (G.sh_active ? (tex = G.sh_tex) != NULL && (tex->rgba || tex->rt) : G.texture_2d && (tex = find_texture(G.units[0])) && (tex->rgba || tex->rt)) {
        tex->used = tex_frame;
        if (tex->dirty || !tex->handle) send_texture(ctx, tex);
        if (!tex->handle) tex = NULL;
    } else tex = NULL;
    G.tc = tex && !G.sh_active ? texcoord_mode(G.ge_texmat) : TC_PLAIN;
    if (G.tc != TC_GE) mat_identity(G.ge_texmat);
    build_state(&st, tex);
    if (G.log >= 3 && (!G.state_sent || memcmp(&st, &G.sent_state, sizeof(st))))
        gl_logf("state: enable %x viewport %d,%d %dx%d depth %u..%u scissor %d,%d %dx%d mask %x blend %d/%d tex %u",
                st.enable, st.viewport_x, st.viewport_y, st.viewport_w, st.viewport_h, st.depth_near, st.depth_far,
                st.scissor_x, st.scissor_y, st.scissor_w, st.scissor_h, st.pixel_mask, st.blend_src, st.blend_dst, st.texture);
    if (!G.state_sent || memcmp(&st, &G.sent_state, sizeof(st))) {
        pspexp_3d_state(ctx, &st);
        G.sent_state = st;
        G.state_sent = 1;
    }
    {
    static float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    const float *proj = G.sh_active ? G.sh_proj : G.proj[G.proj_top], *mv = G.sh_active ? identity : G.mv[G.mv_top];
    float mvs[16];
    if (G.pk_scale > 0) {   /* positions sent scaled: the modelview undoes it */
        int q;
        float inv = 1.0f / G.pk_scale;
        memcpy(mvs, mv, sizeof(mvs));
        for (q = 0; q < 12; q++) mvs[q] *= inv;
        mv = mvs;
    }
    if (!G.matrices_sent || memcmp(G.sent_proj, proj, sizeof(G.sent_proj)) ||
        memcmp(G.sent_mv, mv, sizeof(G.sent_mv)) || memcmp(G.sent_tex, G.ge_texmat, sizeof(G.sent_tex))) {
        memcpy(G.sent_proj, proj, sizeof(G.sent_proj));
        memcpy(G.sent_mv, mv, sizeof(G.sent_mv));
        memcpy(G.sent_tex, G.ge_texmat, sizeof(G.sent_tex));
        pspexp_3d_matrices(ctx, G.sent_proj, G.sent_mv, G.sent_tex);
        G.matrices_sent = 1;
    }
    }
    if ((G.lighting && !G.sh_active) || G.sh_lit || G.sh_env || color || G.lights_const || G.tc == TC_ENV) {
        PspExp3DLights l;
        build_lights(&l, color);
        G.lights_const = color != NULL;
        if (!G.lights_sent || memcmp(&l, &G.sent_lights, sizeof(l))) {
            pspexp_3d_lights(ctx, &l);
            G.sent_lights = l;
            G.lights_sent = 1;
        }
        G.lights_dirty = 0;
    }
    *texp = tex;
    return ctx;
}

static int texture_matrix_identity(void)
{
    static float id[16];
    if (id[0] == 0.0f) mat_identity(id);
    return !memcmp(G.texm[G.tex_top], id, sizeof(id));
}

/* Send n vertices as one GE primitive (already split to fit). */
static int content_mesh(PspExpContext *ctx, uint8_t prim, uint32_t vt, const uint8_t *data, int n);

static int16_t s16(float f) { f *= 32768.0f; return (int16_t)(f >= 32767.0f ? 32767 : f <= -32768.0f ? -32768 : lroundf(f)); }
static uint16_t u16(float f) { f *= 32768.0f; return (uint16_t)(f >= 65535.0f ? 65535 : f <= 0 ? 0 : lroundf(f)); }

/* n vertices in the layout of `vt`: floats, or (PSPEXP_FEATURE_3D16) 16-bit coordinates and positions,
 * the positions times G.pk_scale (the matrices sent undo it) */
static void pack(uint8_t *out, uint32_t vt, const Vtx *v, int n)
{
    uint32_t size = pspexp_3d_vertex_size(vt);
    int i;
    for (i = 0; i < n; i++, out += size) {
        uint8_t *o = out;
        if ((vt & 3u) == 3u) { memcpy(o, v[i].t, 8); o += 8; }
        else if (vt & 3u) { uint16_t t[2] = { u16(v[i].t[0]), u16(v[i].t[1]) }; memcpy(o, t, 4); o += 4; }
        if (vt & PSPEXP_3D_VT_COLOR) { memcpy(o, &v[i].c, 4); o += 4; }
        if (vt & PSPEXP_3D_VT_NORMAL) { memcpy(o, v[i].n, 12); o += 12; }
        if ((vt & PSPEXP_3D_VT_POSITION) == PSPEXP_3D_VT_POSITION) memcpy(o, v[i].p, 12);
        else {
            int16_t q[3] = { s16(v[i].p[0] * G.pk_scale), s16(v[i].p[1] * G.pk_scale), s16(v[i].p[2] * G.pk_scale) };
            memcpy(o, q, 6);
            if (o + 6 < out + size) memset(o + 6, 0, (size_t)(out + size - (o + 6)));
        }
    }
}

static void send_prim(PspExpContext *ctx, uint8_t prim, uint32_t vt, const Vtx *v, int n)
{
    uint8_t *out;
    int i;
    if (n >= 512) {   /* a large draw: the same vertices as before may be a mesh on the PSP already */
        static uint8_t *buf;
        static size_t cap;
        size_t bytes = (size_t)pspexp_3d_vertex_size(vt) * (size_t)n;
        if (bytes > cap) { uint8_t *b = (uint8_t *)realloc(buf, bytes); if (b) { buf = b; cap = bytes; } }
        if (bytes <= cap) {
            pack(buf, vt, v, n);
            if (content_mesh(ctx, prim, vt, buf, n)) { G.draws++; G.vertices += (unsigned)n; return; }
            if ((out = (uint8_t *)pspexp_3d_draw(ctx, prim, vt, (uint32_t)n))) memcpy(out, buf, bytes);
            G.draws++; G.vertices += (unsigned)n;
            return;
        }
    }
    {   /* PSPEXP_GL_MAXDRAWS=N: only a frame's first N draws (bisecting) */
        static long max = -2;
        if (max == -2) max = getenv("PSPEXP_GL_MAXDRAWS") ? atol(getenv("PSPEXP_GL_MAXDRAWS")) : -1;
        if (max >= 0 && (long)G.draws >= max) { G.draws++; return; }
    }
    out = (uint8_t *)pspexp_3d_draw(ctx, prim, vt, (uint32_t)n);
    if (!out) return;
    pack(out, vt, v, n);
    (void)i;
    G.draws++;
    G.vertices += (unsigned)n;
}

static Vtx *scratch(int n)
{
    if (n > G.cscratch) {
        Vtx *p = (Vtx *)realloc(G.scratch, (size_t)n * sizeof(Vtx));
        if (!p) return NULL;
        G.scratch = p; G.cscratch = n;
    }
    return G.scratch;
}

/* Independent triangles from a strip or fan (for splitting). */
static int to_triangles(GLenum mode, const Vtx *v, int n, Vtx *out)
{
    int i, k = 0;
    for (i = 2; i < n; i++) {
        if (mode == GL_TRIANGLE_FAN || mode == GL_POLYGON) { out[k++] = v[0]; out[k++] = v[i - 1]; out[k++] = v[i]; }
        else if (i & 1) { out[k++] = v[i - 1]; out[k++] = v[i - 2]; out[k++] = v[i]; }
        else { out[k++] = v[i - 2]; out[k++] = v[i - 1]; out[k++] = v[i]; }
    }
    return k;
}


/* ---- clipping: the GE's range ---------------------------------------------------------
 *
 * The GE draws in a 4096 x 4096 space around the screen and drops a whole
 * triangle that reaches outside it (it does not cut it): large polygons
 * near the camera then vanish as the view turns (Neverball's floors). So
 * triangles are cut here, in clip space as GPUs do, against the near plane
 * and a band a little inside that space; the pieces go with the frame, the
 * rest as they were (meshes stay meshes). */

typedef struct Clip { float m[16]; float gx, gy; } Clip;

static void clip_setup(Clip *c)
{
    const PspExp3DState *s = &G.sent_state;
    float hw = fabsf((float)s->viewport_w) / 2.0f, hh = fabsf((float)s->viewport_h) / 2.0f;
    float left = 2048.0f - PSPEXP_DISPLAY_WIDTH / 2 + s->viewport_x, top = 2048.0f - PSPEXP_DISPLAY_HEIGHT / 2 + s->viewport_y;
    if (G.sh_active) memcpy(c->m, G.sh_proj, sizeof(c->m));
    else mat_mul(c->m, G.proj[G.proj_top], G.mv[G.mv_top]);
    c->gx = 0.9f * fminf(left, 4095.0f - left) / (hw > 1 ? hw : 1);
    c->gy = 0.9f * fminf(top, 4095.0f - top) / (hh > 1 ? hh : 1);
}

static void clip_xform(const Clip *c, const float *p, float *o)
{
    int i;
    for (i = 0; i < 4; i++) o[i] = c->m[i] * p[0] + c->m[4 + i] * p[1] + c->m[8 + i] * p[2] + c->m[12 + i];
}

static float clip_dist(const Clip *c, int k, const float *o)
{
    switch (k) {
    case 0: return o[2] + o[3];            /* near */
    case 1: return o[0] + c->gx * o[3];
    case 2: return -o[0] + c->gx * o[3];
    case 3: return o[1] + c->gy * o[3];
    default: return -o[1] + c->gy * o[3];
    }
}

static unsigned outcode(const Clip *c, const float *o)
{
    unsigned code = 0;
    int k;
    for (k = 0; k < 5; k++) if (clip_dist(c, k, o) < 0) code |= 1u << k;
    return code;
}

typedef struct CV { float o[4]; Vtx v; float col[4]; } CV;

static CV cv_lerp(const CV *a, const CV *b, float t)
{
    CV r;
    int i;
    for (i = 0; i < 4; i++) { r.o[i] = a->o[i] + (b->o[i] - a->o[i]) * t; r.col[i] = a->col[i] + (b->col[i] - a->col[i]) * t; }
    for (i = 0; i < 3; i++) { r.v.p[i] = a->v.p[i] + (b->v.p[i] - a->v.p[i]) * t; r.v.n[i] = a->v.n[i] + (b->v.n[i] - a->v.n[i]) * t; }
    for (i = 0; i < 2; i++) r.v.t[i] = a->v.t[i] + (b->v.t[i] - a->v.t[i]) * t;
    r.v.c = abgr(r.col);
    return r;
}

static void cv_make(CV *cv, const Clip *c, const Vtx *v)
{
    cv->v = *v;
    clip_xform(c, v->p, cv->o);
    cv->col[0] = (v->c & 0xFF) / 255.0f; cv->col[1] = ((v->c >> 8) & 0xFF) / 255.0f;
    cv->col[2] = ((v->c >> 16) & 0xFF) / 255.0f; cv->col[3] = (v->c >> 24) / 255.0f;
}

/* One triangle cut against the planes: appended to out as triangles; how many vertices. */
static int clip_triangle(const Clip *c, const Vtx *a, const Vtx *b, const Vtx *d, Vtx *out)
{
    CV poly[2][12];
    int n = 3, k, i, cur = 0, m;
    cv_make(&poly[0][0], c, a); cv_make(&poly[0][1], c, b); cv_make(&poly[0][2], c, d);
    for (k = 0; k < 5 && n >= 3; k++) {
        CV *in = poly[cur], *o = poly[cur ^ 1];
        m = 0;
        for (i = 0; i < n; i++) {
            const CV *p = &in[i], *q = &in[(i + 1) % n];
            float dp = clip_dist(c, k, p->o), dq = clip_dist(c, k, q->o);
            if (dp >= 0 && m < 12) o[m++] = *p;
            if ((dp >= 0) != (dq >= 0) && m < 12) o[m++] = cv_lerp(p, q, dp / (dp - dq));
        }
        n = m;
        cur ^= 1;
    }
    if (n < 3) return 0;
    for (i = 2, m = 0; i < n; i++) { out[m++] = poly[cur][0].v; out[m++] = poly[cur][i - 1].v; out[m++] = poly[cur][i].v; }
    return m;
}

/* The triangles of a GE primitive, as vertex numbers (strip windings kept). */
static int triangle_at(uint8_t prim, int t, int *i0, int *i1, int *i2)
{
    if (prim == GU_TRIANGLES) { *i0 = 3 * t; *i1 = 3 * t + 1; *i2 = 3 * t + 2; }
    else if (prim == GU_TRIANGLE_FAN) { *i0 = 0; *i1 = t + 1; *i2 = t + 2; }
    else if (prim == GU_TRIANGLE_STRIP) {
        if (t & 1) { *i0 = t + 1; *i1 = t; *i2 = t + 2; } else { *i0 = t; *i1 = t + 1; *i2 = t + 2; }
    } else return 0;
    return 1;
}

static int triangle_count(uint8_t prim, int n) { return prim == GU_TRIANGLES ? n / 3 : n >= 3 ? n - 2 : 0; }

/* A streamed primitive: as it is when inside, else as triangles with the
 * ones that reach out cut. 1: done here. */
static void send_prim(PspExpContext *ctx, uint8_t prim, uint32_t vt, const Vtx *v, int n);
static int clip_and_send(PspExpContext *ctx, uint8_t prim, uint32_t vt, const Vtx *v, int n)
{
    Clip c;
    Vtx *out;
    unsigned *code;
    int i, nt, k = 0, any = 0;
    if (prim < GU_TRIANGLES || getenv("PSPEXP_GL_NOCLIP")) return 0;
    clip_setup(&c);
    if (!(code = (unsigned *)malloc((size_t)n * sizeof(unsigned)))) return 0;
    for (i = 0; i < n; i++) { float o[4]; clip_xform(&c, v[i].p, o); code[i] = outcode(&c, o); any |= code[i] != 0; }
    if (!any) { free(code); return 0; }
    nt = triangle_count(prim, n);
    if (!(out = (Vtx *)malloc((size_t)nt * 21 * sizeof(Vtx) + 1))) { free(code); return 0; }
    for (i = 0; i < nt; i++) {
        int a, b, d;
        triangle_at(prim, i, &a, &b, &d);
        if (!(code[a] | code[b] | code[d])) { out[k++] = v[a]; out[k++] = v[b]; out[k++] = v[d]; }
        else if (!(code[a] & code[b] & code[d])) k += clip_triangle(&c, &v[a], &v[b], &v[d], out + k);   /* else: all beyond one plane, unseen */
    }
    for (i = 0; i < k; i += CHUNK_VERTICES / 3 * 3)
        send_prim(ctx, GU_TRIANGLES, vt, out + i, k - i < CHUNK_VERTICES / 3 * 3 ? k - i : CHUNK_VERTICES / 3 * 3);
    free(out);
    free(code);
    return 1;
}

/* Draw n finished vertices as the GL primitive `mode`. */
static void draw_vertices(GLenum mode, Vtx *v, int n)
{
    PspExpContext *ctx;
    GlTex *tex;
    uint32_t vt = PSPEXP_3D_VT_POSITION | PSPEXP_3D_VT_COLOR;
    uint8_t prim;
    int per, i;
    if (n <= 0 || (G.cull && G.cull_mode == GL_FRONT_AND_BACK && mode >= GL_TRIANGLES)) return;
    G.pk_next = 0;
    if (n >= 16 && !((G.lighting && !G.sh_active) || G.sh_lit || G.sh_env || (G.texgen[0] && G.texture_2d && !G.sh_active)) &&
        (ctx = shim_gl_frame()) && pspexp_3d16_supported(ctx) && !getenv("PSPEXP_GL_FLOATS")) {
        /* unlit (the GE does not renormalise normals): 16-bit positions, over a power of two that fits them */
        float mx = 0;
        for (i = 0; i < n; i++) {
            float a = fabsf(v[i].p[0]), b = fabsf(v[i].p[1]), c = fabsf(v[i].p[2]);
            if (a > mx) mx = a;
            if (b > mx) mx = b;
            if (c > mx) mx = c;
        }
        if (mx > 0 && mx < 1e6f && mx > 1e-6f) {
            int e;
            frexpf(mx, &e);   /* mx < 2^e */
            G.pk_next = ldexpf(1.0f, -e);
        }
    }
    if (!(ctx = prepare(&tex))) return;
    if (G.log >= 2)
        gl_logf("draw %s mode %u n %d tex %u(%s %dx%d) colour %08x blend %d %x/%x depth %d cull %d light %d alpha %d fog %d env %x uv %.3f,%.3f %.3f,%.3f pos %.2f,%.2f,%.2f",
                G.in_begin || G.nimm ? "imm" : G.array_buffer || G.va.buffer ? "vbo" : "arrays",
                mode, n, G.units[0], tex ? "on" : G.texture_2d ? "missing" : "off", tex ? tex->w : 0, tex ? tex->h : 0,
                v[0].c, G.blend, G.blend_src, G.blend_dst, G.depth_test, G.cull, G.lighting, G.alpha_test, G.fog,
                G.tex_env_mode, v[0].t[0], v[0].t[1], v[n > 2 ? 2 : 0].t[0], v[n > 2 ? 2 : 0].t[1], v[0].p[0], v[0].p[1], v[0].p[2]);
    if (tex && !(G.tc == TC_GE && G.texgen[0]) && G.tc != TC_ENV && !G.sh_env) vt |= PSPEXP_3D_VT_TEXTURE;
    if ((G.lighting && !G.sh_active) || G.sh_lit || G.sh_env || G.tc == TC_ENV) vt |= PSPEXP_3D_VT_NORMAL;
    if (G.sh_lit) vt &= ~(uint32_t)PSPEXP_3D_VT_COLOR;   /* lit: the material's colour */
    if (G.pk_scale > 0) vt = (vt & ~(uint32_t)PSPEXP_3D_VT_POSITION) | PSPEXP_3D_VT_POSITION16;
    if (tex && G.tc == TC_HOST && (G.texgen[0] || G.texgen[1])) {   /* generated coordinates, per vertex, here */
        const float *m = G.mv[G.mv_top];
        for (i = 0; i < n; i++) {
            float e[4], p4[4] = { v[i].p[0], v[i].p[1], v[i].p[2], 1.0f };
            int c, eye_done = 0;
            for (c = 0; c < 2; c++) {
                if (!G.texgen[c]) continue;
                if (G.texgen_mode[c] == GL_OBJECT_LINEAR) {
                    const float *pl = G.texgen_obj[c];
                    v[i].t[c] = pl[0] * p4[0] + pl[1] * p4[1] + pl[2] * p4[2] + pl[3];
                    continue;
                }
                if (!eye_done) { xform_point(m, p4, e); eye_done = 1; }
                if (G.texgen_mode[c] == GL_EYE_LINEAR) {
                    const float *pl = G.texgen_eye[c];
                    v[i].t[c] = pl[0] * e[0] + pl[1] * e[1] + pl[2] * e[2] + pl[3] * e[3];
                } else {   /* GL_SPHERE_MAP: the eye vector reflected about the normal */
                    float nn[3], u[3], r[3], len, d, mm;
                    nn[0] = m[0] * v[i].n[0] + m[4] * v[i].n[1] + m[8] * v[i].n[2];
                    nn[1] = m[1] * v[i].n[0] + m[5] * v[i].n[1] + m[9] * v[i].n[2];
                    nn[2] = m[2] * v[i].n[0] + m[6] * v[i].n[1] + m[10] * v[i].n[2];
                    len = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                    if (len > 0) { nn[0] /= len; nn[1] /= len; nn[2] /= len; }
                    len = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
                    u[0] = len > 0 ? e[0] / len : 0; u[1] = len > 0 ? e[1] / len : 0; u[2] = len > 0 ? e[2] / len : -1;
                    d = nn[0] * u[0] + nn[1] * u[1] + nn[2] * u[2];
                    r[0] = u[0] - 2 * nn[0] * d; r[1] = u[1] - 2 * nn[1] * d; r[2] = u[2] - 2 * nn[2] * d;
                    mm = 2.0f * sqrtf(r[0] * r[0] + r[1] * r[1] + (r[2] + 1) * (r[2] + 1));
                    v[i].t[c] = mm > 0 ? r[c] / mm + 0.5f : 0.5f;
                }
            }
        }
    }
    if (tex && G.tc == TC_HOST && !texture_matrix_identity()) {
        const float *m = G.texm[G.tex_top];
        for (i = 0; i < n; i++) {
            float s = v[i].t[0], t = v[i].t[1];
            float q = m[3] * s + m[7] * t + m[15];
            v[i].t[0] = m[0] * s + m[4] * t + m[12];
            v[i].t[1] = m[1] * s + m[5] * t + m[13];
            if (q != 0.0f && q != 1.0f) { v[i].t[0] /= q; v[i].t[1] /= q; }
        }
    }
    if (G.pk_scale > 0 && (vt & 3u) == 3u) {   /* the coordinates too, when all lie in 0 .. 2 (the GE's unsigned 16 bits) */
        for (i = 0; i < n; i++) if (!(v[i].t[0] >= 0 && v[i].t[0] < 1.999f && v[i].t[1] >= 0 && v[i].t[1] < 1.999f)) break;
        if (i == n) vt = (vt & ~3u) | PSPEXP_3D_VT_TEXTURE16;
    }
    switch (mode) {
    case GL_POINTS: prim = GU_POINTS; per = 1; break;
    case GL_LINES: prim = GU_LINES; per = 2; break;
    case GL_LINE_LOOP: case GL_LINE_STRIP: prim = GU_LINE_STRIP; per = 0; break;
    case GL_TRIANGLES: prim = GU_TRIANGLES; per = 3; break;
    case GL_TRIANGLE_STRIP: case GL_QUAD_STRIP: prim = GU_TRIANGLE_STRIP; per = 0; break;
    case GL_TRIANGLE_FAN: case GL_POLYGON: prim = GU_TRIANGLE_FAN; per = 0; break;
    case GL_QUADS: {   /* two triangles each */
        int q = n / 4, k = 0;
        Vtx *t = scratch(q * 6);
        if (!t) return;
        for (i = 0; i < q; i++) {
            const Vtx *a = v + i * 4;
            t[k++] = a[0]; t[k++] = a[1]; t[k++] = a[2];
            t[k++] = a[0]; t[k++] = a[2]; t[k++] = a[3];
        }
        v = t; n = k; prim = GU_TRIANGLES; per = 3;
        break;
    }
    default: set_error(GL_INVALID_ENUM); return;
    }
    if (mode == GL_LINE_LOOP && n > 1) {   /* a strip back to the first vertex */
        Vtx *t = scratch(n + 1);
        if (!t) return;
        if (t != v) memcpy(t, v, (size_t)n * sizeof(Vtx));
        t[n] = v[0];
        v = t; n++;
    }
    if (prim >= GU_TRIANGLES && clip_and_send(ctx, prim, vt, v, n)) return;
    if (per == 0) {
        if (n <= CHUNK_VERTICES) { send_prim(ctx, prim, vt, v, n); return; }
        if (prim == GU_LINE_STRIP) {   /* strips of lines overlap by one vertex */
            for (i = 0; i < n - 1; i += CHUNK_VERTICES - 1)
                send_prim(ctx, prim, vt, v + i, n - i < CHUNK_VERTICES ? n - i : CHUNK_VERTICES);
            return;
        }
        {
            Vtx *t = (Vtx *)malloc((size_t)(n - 2) * 3 * sizeof(Vtx));
            int k;
            if (!t) return;
            k = to_triangles(mode, v, n, t);
            for (i = 0; i < k; i += CHUNK_VERTICES / 3 * 3)
                send_prim(ctx, GU_TRIANGLES, vt, t + i, k - i < CHUNK_VERTICES / 3 * 3 ? k - i : CHUNK_VERTICES / 3 * 3);
            free(t);
        }
        return;
    }
    n -= n % per;
    for (i = 0; i < n; i += CHUNK_VERTICES / per * per)
        send_prim(ctx, prim, vt, v + i, n - i < CHUNK_VERTICES / per * per ? n - i : CHUNK_VERTICES / per * per);
}

static void meshes_frame_done(PspExpContext *ctx);

/* A new frame on the PSP: it has none of our state, and draws on the screen. */
static void frame_restart(void)
{
    G.state_sent = G.matrices_sent = G.lights_sent = 0;
    G.target = NULL;
    tex_frame++;
}

void pspexp_gl_frame_done(void)
{
    meshes_frame_done(shim_gl_frame());
    if (G.log && G.inited) gl_logf("frame: %u draws, %u vertices, %u clears", G.draws, G.vertices, G.clears);
    G.draws = G.vertices = G.clears = 0;
    G.state_sent = G.matrices_sent = G.lights_sent = 0;   /* each frame says it all again: small, and nothing gets stale */
    G.target = NULL;   /* every frame starts on the screen */
    G.frame_cleared = 0;
    tex_frame++;
}

/* ---- display lists: recording ------------------------------------------------------ */

enum {
    OP_BEGIN = 1, OP_END, OP_VERTEX, OP_COLOR, OP_NORMAL, OP_TEXCOORD, OP_ENABLE, OP_DISABLE,
    OP_MATRIX_MODE, OP_LOAD_IDENTITY, OP_LOAD_MATRIX, OP_MULT_MATRIX, OP_PUSH_MATRIX, OP_POP_MATRIX,
    OP_TRANSLATE, OP_ROTATE, OP_SCALE, OP_ORTHO, OP_FRUSTUM, OP_BLEND_FUNC, OP_DEPTH_FUNC, OP_DEPTH_MASK,
    OP_ALPHA_FUNC, OP_MATERIAL, OP_LIGHT, OP_LIGHT_MODEL, OP_COLOR_MATERIAL, OP_SHADE_MODEL, OP_BIND_TEXTURE,
    OP_TEX_ENV, OP_TEX_PARAMETER, OP_FOG, OP_CALL_LIST, OP_CULL_FACE, OP_FRONT_FACE, OP_COLOR_MASK,
    OP_LINE_WIDTH, OP_POINT_SIZE, OP_SCISSOR, OP_VIEWPORT, OP_PUSH_ATTRIB, OP_POP_ATTRIB, OP_CLEAR,
    OP_CLEAR_COLOR, OP_BLEND_EQUATION, OP_RECT, OP_CLEAR_DEPTH, OP_POLYGON_OFFSET, OP_LIST_BASE
};

static int replaying;

/* Record a call into the list being compiled; 1: the caller stops there (GL_COMPILE). */
static int rec(uint16_t op, uint32_t a, uint32_t b, uint32_t c, int nf, const float *f)
{
    List *l = G.compiling;
    size_t need = sizeof(ListOp) + (size_t)nf * sizeof(float);
    ListOp o;
    if (replaying || !l) return 0;
    if (l->len + need > l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 1024;
        uint8_t *p;
        while (cap < l->len + need) cap *= 2;
        if (!(p = (uint8_t *)realloc(l->ops, cap))) { set_error(GL_OUT_OF_MEMORY); return G.list_mode == GL_COMPILE; }
        l->ops = p; l->cap = cap;
    }
    o.op = op; o.nf = (uint16_t)nf; o.i[0] = a; o.i[1] = b; o.i[2] = c;
    memcpy(l->ops + l->len, &o, sizeof(o));
    if (nf) memcpy(l->ops + l->len + sizeof(o), f, (size_t)nf * sizeof(float));
    l->len += need;
    return G.list_mode == GL_COMPILE;
}
#define RECORD(op, a, b, c, nf, f) do { if (G.compiling && !replaying && rec(op, a, b, c, nf, f)) return; } while (0)
#define RECORD0(op) RECORD(op, 0, 0, 0, 0, NULL)

static List *find_list(GLuint name)
{
    List *l;
    for (l = G.lists; l; l = l->next) if (l->name == name) return l;
    return NULL;
}

static void play_list(GLuint name);

/* ---- matrices ------------------------------------------------------------------------ */

GLAPI(void, glMatrixMode, (GLenum mode))
{
    RECORD(OP_MATRIX_MODE, mode, 0, 0, 0, NULL);
    if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE) { set_error(GL_INVALID_ENUM); return; }
    G.matrix_mode = mode;
}

GLAPI(void, glLoadIdentity, (void)) { RECORD0(OP_LOAD_IDENTITY); ensure_init(); mat_identity(cur_matrix()); }

GLAPI(void, glLoadMatrixf, (const GLfloat *m)) { RECORD(OP_LOAD_MATRIX, 0, 0, 0, 16, m); ensure_init(); memcpy(cur_matrix(), m, 16 * sizeof(float)); }
GLAPI(void, glLoadMatrixd, (const GLdouble *m)) { float f[16]; int i; for (i = 0; i < 16; i++) f[i] = (float)m[i]; GLFN(glLoadMatrixf)(f); }
GLAPI(void, glMultMatrixf, (const GLfloat *m)) { float *c; RECORD(OP_MULT_MATRIX, 0, 0, 0, 16, m); ensure_init(); c = cur_matrix(); mat_mul(c, c, m); }
GLAPI(void, glMultMatrixd, (const GLdouble *m)) { float f[16]; int i; for (i = 0; i < 16; i++) f[i] = (float)m[i]; GLFN(glMultMatrixf)(f); }
GLAPI(void, glLoadTransposeMatrixf, (const GLfloat *m)) { float f[16]; int i; for (i = 0; i < 16; i++) f[i] = m[(i % 4) * 4 + i / 4]; GLFN(glLoadMatrixf)(f); }
GLAPI(void, glMultTransposeMatrixf, (const GLfloat *m)) { float f[16]; int i; for (i = 0; i < 16; i++) f[i] = m[(i % 4) * 4 + i / 4]; GLFN(glMultMatrixf)(f); }
GLAPI(void, glLoadTransposeMatrixd, (const GLdouble *m)) { float f[16]; int i; for (i = 0; i < 16; i++) f[i] = (float)m[(i % 4) * 4 + i / 4]; GLFN(glLoadMatrixf)(f); }
GLAPI(void, glMultTransposeMatrixd, (const GLdouble *m)) { float f[16]; int i; for (i = 0; i < 16; i++) f[i] = (float)m[(i % 4) * 4 + i / 4]; GLFN(glMultMatrixf)(f); }

GLAPI(void, glPushMatrix, (void))
{
    RECORD0(OP_PUSH_MATRIX);
    ensure_init();
    switch (G.matrix_mode) {
    case GL_PROJECTION:
        if (G.proj_top + 1 >= PROJ_DEPTH) { set_error(GL_STACK_OVERFLOW); return; }
        memcpy(G.proj[G.proj_top + 1], G.proj[G.proj_top], sizeof(G.proj[0])); G.proj_top++; break;
    case GL_TEXTURE:
        if (G.tex_top + 1 >= TEX_DEPTH) { set_error(GL_STACK_OVERFLOW); return; }
        memcpy(G.texm[G.tex_top + 1], G.texm[G.tex_top], sizeof(G.texm[0])); G.tex_top++; break;
    default:
        if (G.mv_top + 1 >= MV_DEPTH) { set_error(GL_STACK_OVERFLOW); return; }
        memcpy(G.mv[G.mv_top + 1], G.mv[G.mv_top], sizeof(G.mv[0])); G.mv_top++; break;
    }
}

GLAPI(void, glPopMatrix, (void))
{
    RECORD0(OP_POP_MATRIX);
    switch (G.matrix_mode) {
    case GL_PROJECTION: if (G.proj_top == 0) { set_error(GL_STACK_UNDERFLOW); return; } G.proj_top--; break;
    case GL_TEXTURE: if (G.tex_top == 0) { set_error(GL_STACK_UNDERFLOW); return; } G.tex_top--; break;
    default: if (G.mv_top == 0) { set_error(GL_STACK_UNDERFLOW); return; } G.mv_top--; break;
    }
}

GLAPI(void, glTranslatef, (GLfloat x, GLfloat y, GLfloat z))
{
    float m[16], f[3] = { x, y, z };
    RECORD(OP_TRANSLATE, 0, 0, 0, 3, f);
    ensure_init();
    mat_identity(m); m[12] = x; m[13] = y; m[14] = z;
    mat_mul(cur_matrix(), cur_matrix(), m);
}
GLAPI(void, glTranslated, (GLdouble x, GLdouble y, GLdouble z)) { GLFN(glTranslatef)((float)x, (float)y, (float)z); }

GLAPI(void, glScalef, (GLfloat x, GLfloat y, GLfloat z))
{
    float m[16], f[3] = { x, y, z };
    RECORD(OP_SCALE, 0, 0, 0, 3, f);
    ensure_init();
    mat_identity(m); m[0] = x; m[5] = y; m[10] = z;
    mat_mul(cur_matrix(), cur_matrix(), m);
}
GLAPI(void, glScaled, (GLdouble x, GLdouble y, GLdouble z)) { GLFN(glScalef)((float)x, (float)y, (float)z); }

GLAPI(void, glRotatef, (GLfloat angle, GLfloat x, GLfloat y, GLfloat z))
{
    float m[16], f[4] = { angle, x, y, z }, len = sqrtf(x * x + y * y + z * z), a, c, s, t;
    RECORD(OP_ROTATE, 0, 0, 0, 4, f);
    ensure_init();
    if (len == 0.0f) return;
    x /= len; y /= len; z /= len;
    a = angle * 3.14159265358979f / 180.0f; c = cosf(a); s = sinf(a); t = 1.0f - c;
    mat_identity(m);
    m[0] = x * x * t + c;     m[4] = x * y * t - z * s; m[8] = x * z * t + y * s;
    m[1] = y * x * t + z * s; m[5] = y * y * t + c;     m[9] = y * z * t - x * s;
    m[2] = x * z * t - y * s; m[6] = y * z * t + x * s; m[10] = z * z * t + c;
    mat_mul(cur_matrix(), cur_matrix(), m);
}
GLAPI(void, glRotated, (GLdouble a, GLdouble x, GLdouble y, GLdouble z)) { GLFN(glRotatef)((float)a, (float)x, (float)y, (float)z); }

GLAPI(void, glOrtho, (GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f))
{
    float m[16], a[6] = { (float)l, (float)r, (float)b, (float)t, (float)n, (float)f };
    RECORD(OP_ORTHO, 0, 0, 0, 6, a);
    ensure_init();
    if (r == l || t == b || f == n) { set_error(GL_INVALID_VALUE); return; }
    mat_identity(m);
    m[0] = (float)(2 / (r - l)); m[5] = (float)(2 / (t - b)); m[10] = (float)(-2 / (f - n));
    m[12] = (float)(-(r + l) / (r - l)); m[13] = (float)(-(t + b) / (t - b)); m[14] = (float)(-(f + n) / (f - n));
    mat_mul(cur_matrix(), cur_matrix(), m);
}

GLAPI(void, glFrustum, (GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f))
{
    float m[16], a[6] = { (float)l, (float)r, (float)b, (float)t, (float)n, (float)f };
    RECORD(OP_FRUSTUM, 0, 0, 0, 6, a);
    ensure_init();
    if (n <= 0 || f <= 0 || r == l || t == b || f == n) { set_error(GL_INVALID_VALUE); return; }
    memset(m, 0, sizeof(m));
    m[0] = (float)(2 * n / (r - l)); m[5] = (float)(2 * n / (t - b));
    m[8] = (float)((r + l) / (r - l)); m[9] = (float)((t + b) / (t - b));
    m[10] = (float)(-(f + n) / (f - n)); m[11] = -1.0f; m[14] = (float)(-2 * f * n / (f - n));
    mat_mul(cur_matrix(), cur_matrix(), m);
}

/* ---- immediate mode ------------------------------------------------------------------- */

static void current_vtx(Vtx *v, float x, float y, float z)
{
    v->t[0] = G.texcoord[0]; v->t[1] = G.texcoord[1];
    v->c = abgr(G.color);
    v->n[0] = G.normal[0]; v->n[1] = G.normal[1]; v->n[2] = G.normal[2];
    v->p[0] = x; v->p[1] = y; v->p[2] = z;
}

static void imm_add(const Vtx *v)
{
    if (G.nimm == G.cimm) {
        int c = G.cimm ? G.cimm * 2 : 256;
        Vtx *p = (Vtx *)realloc(G.imm, (size_t)c * sizeof(Vtx));
        if (!p) { set_error(GL_OUT_OF_MEMORY); return; }
        G.imm = p; G.cimm = c;
    }
    G.imm[G.nimm++] = *v;
}

GLAPI(void, glBegin, (GLenum mode))
{
    RECORD(OP_BEGIN, mode, 0, 0, 0, NULL);
    ensure_init();
    if (G.in_begin) { set_error(GL_INVALID_OPERATION); return; }
    if (mode > GL_POLYGON) { set_error(GL_INVALID_ENUM); return; }
    G.in_begin = 1; G.prim = mode; G.nimm = 0;
}

GLAPI(void, glEnd, (void))
{
    RECORD0(OP_END);
    if (!G.in_begin) { set_error(GL_INVALID_OPERATION); return; }
    G.in_begin = 0;
    draw_vertices(G.prim, G.imm, G.nimm);
    G.nimm = 0;
}

GLAPI(void, glVertex3f, (GLfloat x, GLfloat y, GLfloat z))
{
    Vtx v;
    if (G.compiling && !replaying) { float f[4] = { x, y, z, 1 }; if (rec(OP_VERTEX, 0, 0, 0, 4, f)) return; }
    if (!G.in_begin) return;
    current_vtx(&v, x, y, z);
    imm_add(&v);
}
GLAPI(void, glVertex4f, (GLfloat x, GLfloat y, GLfloat z, GLfloat w))
{
    if (w != 0.0f && w != 1.0f) { x /= w; y /= w; z /= w; }
    GLFN(glVertex3f)(x, y, z);
}
GLAPI(void, glVertex2f, (GLfloat x, GLfloat y)) { GLFN(glVertex3f)(x, y, 0); }
GLAPI(void, glVertex2d, (GLdouble x, GLdouble y)) { GLFN(glVertex3f)((float)x, (float)y, 0); }
GLAPI(void, glVertex2i, (GLint x, GLint y)) { GLFN(glVertex3f)((float)x, (float)y, 0); }
GLAPI(void, glVertex2s, (GLshort x, GLshort y)) { GLFN(glVertex3f)((float)x, (float)y, 0); }
GLAPI(void, glVertex3d, (GLdouble x, GLdouble y, GLdouble z)) { GLFN(glVertex3f)((float)x, (float)y, (float)z); }
GLAPI(void, glVertex3i, (GLint x, GLint y, GLint z)) { GLFN(glVertex3f)((float)x, (float)y, (float)z); }
GLAPI(void, glVertex3s, (GLshort x, GLshort y, GLshort z)) { GLFN(glVertex3f)((float)x, (float)y, (float)z); }
GLAPI(void, glVertex4d, (GLdouble x, GLdouble y, GLdouble z, GLdouble w)) { GLFN(glVertex4f)((float)x, (float)y, (float)z, (float)w); }
GLAPI(void, glVertex4i, (GLint x, GLint y, GLint z, GLint w)) { GLFN(glVertex4f)((float)x, (float)y, (float)z, (float)w); }
GLAPI(void, glVertex2fv, (const GLfloat *v)) { GLFN(glVertex3f)(v[0], v[1], 0); }
GLAPI(void, glVertex2dv, (const GLdouble *v)) { GLFN(glVertex3f)((float)v[0], (float)v[1], 0); }
GLAPI(void, glVertex2iv, (const GLint *v)) { GLFN(glVertex3f)((float)v[0], (float)v[1], 0); }
GLAPI(void, glVertex2sv, (const GLshort *v)) { GLFN(glVertex3f)((float)v[0], (float)v[1], 0); }
GLAPI(void, glVertex3fv, (const GLfloat *v)) { GLFN(glVertex3f)(v[0], v[1], v[2]); }
GLAPI(void, glVertex3dv, (const GLdouble *v)) { GLFN(glVertex3f)((float)v[0], (float)v[1], (float)v[2]); }
GLAPI(void, glVertex3iv, (const GLint *v)) { GLFN(glVertex3f)((float)v[0], (float)v[1], (float)v[2]); }
GLAPI(void, glVertex3sv, (const GLshort *v)) { GLFN(glVertex3f)((float)v[0], (float)v[1], (float)v[2]); }
GLAPI(void, glVertex4fv, (const GLfloat *v)) { GLFN(glVertex4f)(v[0], v[1], v[2], v[3]); }
GLAPI(void, glVertex4dv, (const GLdouble *v)) { GLFN(glVertex4f)((float)v[0], (float)v[1], (float)v[2], (float)v[3]); }

GLAPI(void, glColor4f, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))
{
    if (G.compiling && !replaying) { float f[4] = { r, g, b, a }; if (rec(OP_COLOR, 0, 0, 0, 4, f)) return; }
    ensure_init();
    G.color[0] = r; G.color[1] = g; G.color[2] = b; G.color[3] = a;
    if (G.color_material && G.lighting) G.lights_dirty = 1;
}
GLAPI(void, glColor3f, (GLfloat r, GLfloat g, GLfloat b)) { GLFN(glColor4f)(r, g, b, 1); }
GLAPI(void, glColor3d, (GLdouble r, GLdouble g, GLdouble b)) { GLFN(glColor4f)((float)r, (float)g, (float)b, 1); }
GLAPI(void, glColor4d, (GLdouble r, GLdouble g, GLdouble b, GLdouble a)) { GLFN(glColor4f)((float)r, (float)g, (float)b, (float)a); }
GLAPI(void, glColor3ub, (GLubyte r, GLubyte g, GLubyte b)) { GLFN(glColor4f)(r / 255.0f, g / 255.0f, b / 255.0f, 1); }
GLAPI(void, glColor4ub, (GLubyte r, GLubyte g, GLubyte b, GLubyte a)) { GLFN(glColor4f)(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }
GLAPI(void, glColor3i, (GLint r, GLint g, GLint b)) { GLFN(glColor4f)(r / 2147483647.0f, g / 2147483647.0f, b / 2147483647.0f, 1); }
GLAPI(void, glColor3fv, (const GLfloat *v)) { GLFN(glColor4f)(v[0], v[1], v[2], 1); }
GLAPI(void, glColor4fv, (const GLfloat *v)) { GLFN(glColor4f)(v[0], v[1], v[2], v[3]); }
GLAPI(void, glColor3dv, (const GLdouble *v)) { GLFN(glColor4f)((float)v[0], (float)v[1], (float)v[2], 1); }
GLAPI(void, glColor4dv, (const GLdouble *v)) { GLFN(glColor4f)((float)v[0], (float)v[1], (float)v[2], (float)v[3]); }
GLAPI(void, glColor3ubv, (const GLubyte *v)) { GLFN(glColor4f)(v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, 1); }
GLAPI(void, glColor4ubv, (const GLubyte *v)) { GLFN(glColor4f)(v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3] / 255.0f); }

GLAPI(void, glNormal3f, (GLfloat x, GLfloat y, GLfloat z))
{
    if (G.compiling && !replaying) { float f[3] = { x, y, z }; if (rec(OP_NORMAL, 0, 0, 0, 3, f)) return; }
    ensure_init();
    G.normal[0] = x; G.normal[1] = y; G.normal[2] = z;
}
GLAPI(void, glNormal3d, (GLdouble x, GLdouble y, GLdouble z)) { GLFN(glNormal3f)((float)x, (float)y, (float)z); }
GLAPI(void, glNormal3fv, (const GLfloat *v)) { GLFN(glNormal3f)(v[0], v[1], v[2]); }
GLAPI(void, glNormal3dv, (const GLdouble *v)) { GLFN(glNormal3f)((float)v[0], (float)v[1], (float)v[2]); }
GLAPI(void, glNormal3b, (GLbyte x, GLbyte y, GLbyte z)) { GLFN(glNormal3f)(x / 127.0f, y / 127.0f, z / 127.0f); }

GLAPI(void, glTexCoord4f, (GLfloat s, GLfloat t, GLfloat r, GLfloat q))
{
    if (G.compiling && !replaying) { float f[4] = { s, t, r, q }; if (rec(OP_TEXCOORD, 0, 0, 0, 4, f)) return; }
    ensure_init();
    if (q != 0.0f && q != 1.0f) { s /= q; t /= q; }
    G.texcoord[0] = s; G.texcoord[1] = t; G.texcoord[2] = r; G.texcoord[3] = 1.0f;
}
GLAPI(void, glTexCoord2f, (GLfloat s, GLfloat t)) { GLFN(glTexCoord4f)(s, t, 0, 1); }
GLAPI(void, glTexCoord1f, (GLfloat s)) { GLFN(glTexCoord4f)(s, 0, 0, 1); }
GLAPI(void, glTexCoord3f, (GLfloat s, GLfloat t, GLfloat r)) { GLFN(glTexCoord4f)(s, t, r, 1); }
GLAPI(void, glTexCoord2d, (GLdouble s, GLdouble t)) { GLFN(glTexCoord4f)((float)s, (float)t, 0, 1); }
GLAPI(void, glTexCoord2i, (GLint s, GLint t)) { GLFN(glTexCoord4f)((float)s, (float)t, 0, 1); }
GLAPI(void, glTexCoord2s, (GLshort s, GLshort t)) { GLFN(glTexCoord4f)((float)s, (float)t, 0, 1); }
GLAPI(void, glTexCoord2fv, (const GLfloat *v)) { GLFN(glTexCoord4f)(v[0], v[1], 0, 1); }
GLAPI(void, glTexCoord2dv, (const GLdouble *v)) { GLFN(glTexCoord4f)((float)v[0], (float)v[1], 0, 1); }
GLAPI(void, glTexCoord4fv, (const GLfloat *v)) { GLFN(glTexCoord4f)(v[0], v[1], v[2], v[3]); }
GLAPI(void, glMultiTexCoord2f, (GLenum unit, GLfloat s, GLfloat t)) { if (unit == GL_TEXTURE0) GLFN(glTexCoord4f)(s, t, 0, 1); }
GLAPI(void, glMultiTexCoord2fv, (GLenum unit, const GLfloat *v)) { if (unit == GL_TEXTURE0) GLFN(glTexCoord4f)(v[0], v[1], 0, 1); }
GLAPI(void, glMultiTexCoord2d, (GLenum unit, GLdouble s, GLdouble t)) { if (unit == GL_TEXTURE0) GLFN(glTexCoord4f)((float)s, (float)t, 0, 1); }
GLAPI(void, glMultiTexCoord2i, (GLenum unit, GLint s, GLint t)) { if (unit == GL_TEXTURE0) GLFN(glTexCoord4f)((float)s, (float)t, 0, 1); }
GLAPI(void, glMultiTexCoord4f, (GLenum unit, GLfloat s, GLfloat t, GLfloat r, GLfloat q)) { if (unit == GL_TEXTURE0) GLFN(glTexCoord4f)(s, t, r, q); }
GLAPI(void, glPointParameterf, (GLenum p, GLfloat v)) { (void)p; (void)v; }
GLAPI(void, glPointParameterfv, (GLenum p, const GLfloat *v)) { (void)p; (void)v; }

GLAPI(void, glRectf, (GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2))
{
    GLFN(glBegin)(GL_QUADS);
    GLFN(glVertex2f)(x1, y1); GLFN(glVertex2f)(x2, y1); GLFN(glVertex2f)(x2, y2); GLFN(glVertex2f)(x1, y2);
    GLFN(glEnd)();
}
GLAPI(void, glRecti, (GLint x1, GLint y1, GLint x2, GLint y2)) { GLFN(glRectf)((float)x1, (float)y1, (float)x2, (float)y2); }
GLAPI(void, glRectd, (GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2)) { GLFN(glRectf)((float)x1, (float)y1, (float)x2, (float)y2); }
GLAPI(void, glRects, (GLshort x1, GLshort y1, GLshort x2, GLshort y2)) { GLFN(glRectf)((float)x1, (float)y1, (float)x2, (float)y2); }
GLAPI(void, glRectfv, (const GLfloat *a, const GLfloat *b)) { GLFN(glRectf)(a[0], a[1], b[0], b[1]); }
GLAPI(void, glRectiv, (const GLint *a, const GLint *b)) { GLFN(glRectf)((float)a[0], (float)a[1], (float)b[0], (float)b[1]); }
GLAPI(void, glEdgeFlag, (GLboolean f)) { (void)f; }

/* ---- vertex arrays and buffer objects -------------------------------------------------- */

static void meshes_forget_buffer(GLuint name);

/* the last buffer found, and a small table: every vertex of a draw asks for its arrays' buffers */
static GlBuf *buf_hash[1024];
static GlBuf *find_buffer(GLuint name)
{
    GlBuf *b = buf_hash[name & 1023];
    if (b && b->name == name) return b;
    for (b = G.buffers; b; b = b->next) if (b->name == name) { buf_hash[name & 1023] = b; return b; }
    return NULL;
}

static const uint8_t *array_base(const Array *a)
{
    if (a->buffer) { GlBuf *b = find_buffer(a->buffer); return b && b->data ? b->data + (uintptr_t)a->ptr : NULL; }
    return a->ptr;
}

static int type_size(GLenum t)
{
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return 4;
    case GL_DOUBLE: return 8;
    default: return 0;
    }
}

static float read_comp(const uint8_t *p, GLenum t, int normalise)
{
    switch (t) {
    case GL_FLOAT: { float f; memcpy(&f, p, 4); return f; }
    case GL_DOUBLE: { double d; memcpy(&d, p, 8); return (float)d; }
    case GL_BYTE: return normalise ? *(const int8_t *)p / 127.0f : *(const int8_t *)p;
    case GL_UNSIGNED_BYTE: return normalise ? *p / 255.0f : *p;
    case GL_SHORT: { int16_t s; memcpy(&s, p, 2); return normalise ? s / 32767.0f : s; }
    case GL_UNSIGNED_SHORT: { uint16_t s; memcpy(&s, p, 2); return normalise ? s / 65535.0f : s; }
    case GL_INT: { int32_t s; memcpy(&s, p, 4); return normalise ? s / 2147483647.0f : (float)s; }
    case GL_UNSIGNED_INT: { uint32_t s; memcpy(&s, p, 4); return normalise ? s / 4294967295.0f : (float)s; }
    default: return 0;
    }
}

/* Vertex i from the enabled arrays (the current values for the others). */
static void fetch(int i, Vtx *v)
{
    const uint8_t *b;
    int k;
    float c[4];
    current_vtx(v, 0, 0, 0);
    if (G.va.enabled && (b = array_base(&G.va))) {
        int st = G.va.stride ? G.va.stride : G.va.size * type_size(G.va.type), ts = type_size(G.va.type);
        float w = 1.0f;
        b += (size_t)i * st;
        for (k = 0; k < 3; k++) v->p[k] = k < G.va.size ? read_comp(b + k * ts, G.va.type, 0) : 0.0f;
        if (G.va.size == 4) w = read_comp(b + 3 * ts, G.va.type, 0);
        if (w != 0.0f && w != 1.0f) { v->p[0] /= w; v->p[1] /= w; v->p[2] /= w; }
    }
    if (G.ca.enabled && (b = array_base(&G.ca))) {
        int st = G.ca.stride ? G.ca.stride : G.ca.size * type_size(G.ca.type), ts = type_size(G.ca.type);
        b += (size_t)i * st;
        for (k = 0; k < 4; k++) c[k] = k < G.ca.size ? read_comp(b + k * ts, G.ca.type, 1) : 1.0f;
        if (G.ca.bgra) { float t = c[0]; c[0] = c[2]; c[2] = t; }   /* size GL_BGRA */
        v->c = abgr(c);
    }
    if (G.na.enabled && (b = array_base(&G.na))) {
        int st = G.na.stride ? G.na.stride : 3 * type_size(G.na.type), ts = type_size(G.na.type);
        b += (size_t)i * st;
        for (k = 0; k < 3; k++) v->n[k] = read_comp(b + k * ts, G.na.type, 1);
    }
    if (G.ta.enabled && (b = array_base(&G.ta))) {
        int st = G.ta.stride ? G.ta.stride : G.ta.size * type_size(G.ta.type), ts = type_size(G.ta.type);
        b += (size_t)i * st;
        v->t[0] = read_comp(b, G.ta.type, 0);
        v->t[1] = G.ta.size > 1 ? read_comp(b + ts, G.ta.type, 0) : 0.0f;
        if (G.ta.size == 4) { float q = read_comp(b + 3 * ts, G.ta.type, 0); if (q != 0.0f && q != 1.0f) { v->t[0] /= q; v->t[1] /= q; } }
    }
}

static Vtx *gather(int n)
{
    static Vtx *buf;
    static int cap;
    if (n > cap) {
        Vtx *p = (Vtx *)realloc(buf, (size_t)n * sizeof(Vtx));
        if (!p) return NULL;
        buf = p; cap = n;
    }
    return buf;
}

/* In a list being compiled, arrays are read now: glBegin, the vertices, glEnd. */
static void compile_arrays(GLenum mode, const int *idx, int first, int n)
{
    int i;
    Vtx v;
    GLFN(glBegin)(mode);
    for (i = 0; i < n; i++) {
        fetch(idx ? idx[i] : first + i, &v);
        {   /* the attributes as recorded calls */
            float c[4] = { (v.c & 0xFF) / 255.0f, ((v.c >> 8) & 0xFF) / 255.0f, ((v.c >> 16) & 0xFF) / 255.0f, (v.c >> 24) / 255.0f };
            if (G.ca.enabled) GLFN(glColor4f)(c[0], c[1], c[2], c[3]);
            if (G.na.enabled) GLFN(glNormal3f)(v.n[0], v.n[1], v.n[2]);
            if (G.ta.enabled) GLFN(glTexCoord4f)(v.t[0], v.t[1], 0, 1);
            GLFN(glVertex3f)(v.p[0], v.p[1], v.p[2]);
        }
    }
    GLFN(glEnd)();
}

static void set_array(Array *a, int size, GLenum type, GLsizei stride, const void *ptr)
{
    ensure_init();
    a->size = size; a->type = type; a->stride = stride;
    a->buffer = G.array_buffer;
    a->ptr = (const uint8_t *)ptr;
}

GLAPI(void, glVertexPointer, (GLint size, GLenum type, GLsizei stride, const GLvoid *ptr)) { set_array(&G.va, size, type, stride, ptr); }
GLAPI(void, glColorPointer, (GLint size, GLenum type, GLsizei stride, const GLvoid *ptr))
{
    set_array(&G.ca, size == GL_BGRA ? 4 : size, type, stride, ptr);
    G.ca.bgra = size == GL_BGRA;
}
GLAPI(void, glNormalPointer, (GLenum type, GLsizei stride, const GLvoid *ptr)) { set_array(&G.na, 3, type, stride, ptr); }
GLAPI(void, glTexCoordPointer, (GLint size, GLenum type, GLsizei stride, const GLvoid *ptr))
{
    if (G.client_unit != 0) return;
    set_array(&G.ta, size, type, stride, ptr);
}

static Array *client_array(GLenum cap)
{
    switch (cap) {
    case GL_VERTEX_ARRAY: return &G.va;
    case GL_COLOR_ARRAY: return &G.ca;
    case GL_NORMAL_ARRAY: return &G.na;
    case GL_TEXTURE_COORD_ARRAY: return G.client_unit == 0 ? &G.ta : NULL;
    default: return NULL;
    }
}
GLAPI(void, glEnableClientState, (GLenum cap)) { Array *a; ensure_init(); if ((a = client_array(cap))) a->enabled = 1; }
GLAPI(void, glDisableClientState, (GLenum cap)) { Array *a; ensure_init(); if ((a = client_array(cap))) a->enabled = 0; }
GLAPI(void, glClientActiveTexture, (GLenum unit)) { ensure_init(); G.client_unit = (int)(unit - GL_TEXTURE0); }

GLAPI(void, glArrayElement, (GLint i))
{
    Vtx v;
    if (G.compiling && !replaying) { compile_arrays(GL_POINTS, &i, 0, 0); }
    if (!G.in_begin) return;
    fetch(i, &v);
    imm_add(&v);
}


/* ---- GL 2: shaders (glsl_pspexp.c reads them), programs, generic attributes -------------------- */

typedef struct GlShader { GLuint name; GLenum type; char *src; GlslShader *sh; char log[256]; int deleted; struct GlShader *next; } GlShader;
typedef struct GlProgram {
    GLuint name;
    GLuint att[4]; int natt;
    GlslProgram *p;
    char log[256];
    char *bnames[16]; int blocs[16]; int nb;   /* glBindAttribLocation, for the next link */
    unsigned uver, planned_ver; int planned, builtins, reported, described;
    GlslPlan plan;
    struct GlProgram *next;
} GlProgram;
typedef struct Vao { GLuint name; Array gattr[16]; int gnorm[16]; GLuint element_buffer; struct Vao *next; } Vao;

static GlShader *shaders;
static GlProgram *programs;
static Vao *vaos;

static GlShader *find_shader(GLuint n) { GlShader *s; for (s = shaders; s; s = s->next) if (s->name == n) return s; return NULL; }
static GlProgram *find_program(GLuint n) { GlProgram *p; for (p = programs; p; p = p->next) if (p->name == n) return p; return NULL; }

static void builtin_matrix(const char *name, float out[16])   /* old shaders' gl_ModelViewMatrix and the like */
{
    if (!strcmp(name, "gl_ModelViewProjectionMatrix")) mat_mul(out, G.proj[G.proj_top], G.mv[G.mv_top]);
    else if (!strcmp(name, "gl_ModelViewMatrix")) memcpy(out, G.mv[G.mv_top], 64);
    else if (!strcmp(name, "gl_ProjectionMatrix")) memcpy(out, G.proj[G.proj_top], 64);
    else if (!strcmp(name, "gl_TextureMatrix0")) memcpy(out, G.texm[G.tex_top], 64);
    else if (!strcmp(name, "gl_Fog.color")) memcpy(out, G.fog_color, 16);
    else if (!strcmp(name, "gl_Fog.density")) out[0] = G.fog_density;
    else if (!strcmp(name, "gl_Fog.start")) out[0] = G.fog_start;
    else if (!strcmp(name, "gl_Fog.end")) out[0] = G.fog_end;
    else if (!strcmp(name, "gl_Fog.scale")) out[0] = G.fog_end != G.fog_start ? 1.0f / (G.fog_end - G.fog_start) : 0.0f;
}

GLAPI(GLuint, glCreateShader, (GLenum type))
{
    GlShader *s;
    ensure_init();
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) { set_error(GL_INVALID_ENUM); return 0; }
    if (!(s = (GlShader *)calloc(1, sizeof(*s)))) { set_error(GL_OUT_OF_MEMORY); return 0; }
    s->name = G.next_object++; s->type = type;
    s->next = shaders; shaders = s;
    return s->name;
}

GLAPI(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths))
{
    GlShader *s = find_shader(shader);
    size_t total = 0, off = 0;
    int i;
    if (!s) { set_error(GL_INVALID_VALUE); return; }
    for (i = 0; i < count; i++) total += lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
    free(s->src);
    if (!(s->src = (char *)malloc(total + 1))) { set_error(GL_OUT_OF_MEMORY); return; }
    for (i = 0; i < count; i++) {
        size_t n = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
        memcpy(s->src + off, strings[i], n);
        off += n;
    }
    s->src[off] = 0;
}

GLAPI(void, glCompileShader, (GLuint shader))
{
    GlShader *s = find_shader(shader);
    if (!s || !s->src) { set_error(GL_INVALID_VALUE); return; }
    glsl_shader_free(s->sh);
    s->log[0] = 0;
    s->sh = glsl_compile(s->type == GL_VERTEX_SHADER ? GLSL_VERTEX : GLSL_FRAGMENT, s->src, s->log, sizeof(s->log));
    if (s->log[0]) gl_logf("shader %u: %s (drawn as near as the GE can)", shader, s->log);
    s->log[0] = 0;   /* the program runs all the same: no errors for it */
}

/* A deleted shader still attached to a program lives on until it is not (as GL has it: Quakespasm
 * deletes its shaders between attaching and linking) */
static void shaders_sweep(void)
{
    GlShader **pp = &shaders;
    while (*pp) {
        GlShader *s = *pp;
        GlProgram *p;
        int i, attached = 0;
        for (p = programs; p && !attached; p = p->next)
            for (i = 0; i < p->natt; i++) if (p->att[i] == s->name) attached = 1;
        if (s->deleted && !attached) { *pp = s->next; free(s->src); glsl_shader_free(s->sh); free(s); }
        else pp = &s->next;
    }
}

GLAPI(void, glDeleteShader, (GLuint shader))
{
    GlShader *s = find_shader(shader);
    if (s) { s->deleted = 1; shaders_sweep(); }
}

GLAPI(void, glGetShaderiv, (GLuint shader, GLenum p, GLint *out))
{
    GlShader *s = find_shader(shader);
    if (!s) { set_error(GL_INVALID_VALUE); return; }
    switch (p) {
    case GL_COMPILE_STATUS: *out = GL_TRUE; break;
    case GL_INFO_LOG_LENGTH: *out = 0; break;
    case GL_SHADER_TYPE: *out = (GLint)s->type; break;
    case GL_SHADER_SOURCE_LENGTH: *out = s->src ? (GLint)strlen(s->src) + 1 : 0; break;
    case GL_DELETE_STATUS: *out = GL_FALSE; break;
    default: *out = 0; break;
    }
}

GLAPI(void, glGetShaderInfoLog, (GLuint shader, GLsizei max, GLsizei *len, GLchar *log))
{
    (void)shader;
    if (len) *len = 0;
    if (log && max > 0) log[0] = 0;
}

GLAPI(void, glGetShaderSource, (GLuint shader, GLsizei max, GLsizei *len, GLchar *out))
{
    GlShader *s = find_shader(shader);
    size_t n = s && s->src ? strlen(s->src) : 0;
    if (max <= 0) return;
    if (n >= (size_t)max) n = (size_t)max - 1;
    if (s && s->src) memcpy(out, s->src, n);
    out[n] = 0;
    if (len) *len = (GLsizei)n;
}

GLAPI(GLboolean, glIsShader, (GLuint shader)) { return find_shader(shader) != NULL; }

GLAPI(GLuint, glCreateProgram, (void))
{
    GlProgram *p;
    ensure_init();
    if (!(p = (GlProgram *)calloc(1, sizeof(*p)))) { set_error(GL_OUT_OF_MEMORY); return 0; }
    p->name = G.next_object++;
    p->next = programs; programs = p;
    return p->name;
}

GLAPI(void, glDeleteProgram, (GLuint program))
{
    GlProgram **pp = &programs;
    int i;
    if (!program) return;
    while (*pp && (*pp)->name != program) pp = &(*pp)->next;
    if (!*pp) return;
    if (G.program == program) G.program = 0;
    {
        GlProgram *p = *pp;
        *pp = p->next;
        glsl_program_free(p->p);
        for (i = 0; i < p->nb; i++) free(p->bnames[i]);
        free(p);
    }
    shaders_sweep();
}

GLAPI(void, glAttachShader, (GLuint program, GLuint shader))
{
    GlProgram *p = find_program(program);
    if (!p || !find_shader(shader)) { set_error(GL_INVALID_VALUE); return; }
    if (p->natt < 4) p->att[p->natt++] = shader;
}

GLAPI(void, glDetachShader, (GLuint program, GLuint shader))
{
    GlProgram *p = find_program(program);
    int i;
    if (!p) return;
    for (i = 0; i < p->natt; i++) if (p->att[i] == shader) { p->att[i] = p->att[--p->natt]; break; }
    shaders_sweep();
}

GLAPI(void, glBindAttribLocation, (GLuint program, GLuint index, const GLchar *name))
{
    GlProgram *p = find_program(program);
    int i;
    if (!p || index >= 16) { set_error(GL_INVALID_VALUE); return; }
    for (i = 0; i < p->nb; i++) if (!strcmp(p->bnames[i], name)) { p->blocs[i] = (int)index; return; }
    if (p->nb < 16) { p->bnames[p->nb] = strdup(name); p->blocs[p->nb++] = (int)index; }
}

GLAPI(void, glLinkProgram, (GLuint program))
{
    GlProgram *p = find_program(program);
    GlShader *vs = NULL, *fs = NULL;
    int i;
    if (!p) { set_error(GL_INVALID_VALUE); return; }
    for (i = 0; i < p->natt; i++) {
        GlShader *s = find_shader(p->att[i]);
        if (s && s->type == GL_VERTEX_SHADER) vs = s;
        if (s && s->type == GL_FRAGMENT_SHADER) fs = s;
    }
    glsl_program_free(p->p);
    p->p = vs && fs ? glsl_link(vs->sh, fs->sh, (const char *const *)p->bnames, p->blocs, p->nb, p->log, sizeof(p->log)) : NULL;
    if (!p->p) gl_logf("program %u: not linked (%s)", program, p->log[0] ? p->log : "a shader missing");
    p->builtins = (vs && vs->src && (strstr(vs->src, "gl_ModelView") || strstr(vs->src, "gl_Projection") ||
                  strstr(vs->src, "ftransform") || strstr(vs->src, "gl_TextureMatrix") || strstr(vs->src, "gl_NormalMatrix")));
    p->planned = 0; p->reported = 0;
    p->uver++;
    glsl_set_builtins(builtin_matrix);
}

GLAPI(void, glUseProgram, (GLuint program)) { ensure_init(); G.program = program; }
GLAPI(void, glValidateProgram, (GLuint program)) { (void)program; }
GLAPI(GLboolean, glIsProgram, (GLuint program)) { return find_program(program) != NULL; }

GLAPI(void, glGetProgramiv, (GLuint program, GLenum pn, GLint *out))
{
    GlProgram *p = find_program(program);
    int i, m = 0;
    if (!p) { set_error(GL_INVALID_VALUE); return; }
    switch (pn) {
    case GL_LINK_STATUS: case GL_VALIDATE_STATUS: *out = p->p != NULL; break;
    case GL_INFO_LOG_LENGTH: *out = p->p ? 0 : (GLint)strlen(p->log) + 1; break;
    case GL_ATTACHED_SHADERS: *out = p->natt; break;
    case GL_ACTIVE_UNIFORMS: *out = p->p ? glsl_uniform_count(p->p) : 0; break;
    case GL_ACTIVE_ATTRIBUTES: *out = p->p ? glsl_attrib_count(p->p) : 0; break;
    case GL_ACTIVE_UNIFORM_MAX_LENGTH:
        if (p->p) for (i = 0; i < glsl_uniform_count(p->p); i++) { int l = (int)strlen(glsl_uniform(p->p, i)->name) + 4; if (l > m) m = l; }
        *out = m; break;
    case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH:
        if (p->p) for (i = 0; i < glsl_attrib_count(p->p); i++) { int l = (int)strlen(glsl_attrib(p->p, i)->name) + 1; if (l > m) m = l; }
        *out = m; break;
    case GL_DELETE_STATUS: *out = GL_FALSE; break;
    default: *out = 0; break;
    }
}

GLAPI(void, glGetProgramInfoLog, (GLuint program, GLsizei max, GLsizei *len, GLchar *log))
{
    GlProgram *p = find_program(program);
    size_t n = p && !p->p ? strlen(p->log) : 0;
    if (max <= 0) return;
    if (n >= (size_t)max) n = (size_t)max - 1;
    if (n) memcpy(log, p->log, n);
    log[n] = 0;
    if (len) *len = (GLsizei)n;
}

GLAPI(void, glGetAttachedShaders, (GLuint program, GLsizei max, GLsizei *count, GLuint *out))
{
    GlProgram *p = find_program(program);
    int i, n = 0;
    if (p) for (i = 0; i < p->natt && i < max; i++) out[n++] = p->att[i];
    if (count) *count = n;
}

GLAPI(GLint, glGetAttribLocation, (GLuint program, const GLchar *name))
{
    GlProgram *p = find_program(program);
    int i;
    if (!p || !p->p) return -1;
    for (i = 0; i < glsl_attrib_count(p->p); i++) if (!strcmp(glsl_attrib(p->p, i)->name, name)) return glsl_attrib(p->p, i)->location;
    return -1;
}

GLAPI(GLint, glGetUniformLocation, (GLuint program, const GLchar *name))
{
    GlProgram *p = find_program(program);
    return p && p->p ? glsl_uniform_location(p->p, name) : -1;
}

GLAPI(void, glGetActiveUniform, (GLuint program, GLuint index, GLsizei max, GLsizei *len, GLint *size, GLenum *type, GLchar *name))
{
    GlProgram *p = find_program(program);
    GlslVar *u;
    if (!p || !p->p || (int)index >= glsl_uniform_count(p->p)) { set_error(GL_INVALID_VALUE); return; }
    u = glsl_uniform(p->p, (int)index);
    if (size) *size = u->count;
    if (type) *type = u->gltype;
    if (max > 0) { int n = snprintf(name, (size_t)max, u->count > 1 ? "%s[0]" : "%s", u->name); if (len) *len = n < max ? n : max - 1; }
}

GLAPI(void, glGetActiveAttrib, (GLuint program, GLuint index, GLsizei max, GLsizei *len, GLint *size, GLenum *type, GLchar *name))
{
    GlProgram *p = find_program(program);
    const GlslVar *a;
    if (!p || !p->p || (int)index >= glsl_attrib_count(p->p)) { set_error(GL_INVALID_VALUE); return; }
    a = glsl_attrib(p->p, (int)index);
    if (size) *size = 1;
    if (type) *type = a->gltype;
    if (max > 0) { int n = snprintf(name, (size_t)max, "%s", a->name); if (len) *len = n < max ? n : max - 1; }
}

/* glUniform*: into the program in use (its plan is made again) */
static void set_uniform(GLint loc, int comps, GLsizei count, const float *f, const GLint *iv, int transpose)
{
    GlProgram *p = find_program(G.program);
    float *slot;
    int uc, rem, isint, e, k;
    if (loc < 0) return;
    if (!p || !p->p || !(slot = glsl_uniform_slot(p->p, loc, &uc, &rem, &isint))) { set_error(GL_INVALID_OPERATION); return; }
    if (count > rem) count = rem;
    for (e = 0; e < count; e++)
        for (k = 0; k < comps && k < uc; k++) {
            int src = k;
            if (transpose) { int d = comps == 16 ? 4 : comps == 9 ? 3 : 2; src = (k % d) * d + k / d; }
            slot[e * uc + k] = f ? f[e * comps + src] : (float)iv[e * comps + src];
        }
    p->uver++;
}

GLAPI(void, glUniform1f, (GLint l, GLfloat a)) { float v[1] = { a }; set_uniform(l, 1, 1, v, NULL, 0); }
GLAPI(void, glUniform2f, (GLint l, GLfloat a, GLfloat b)) { float v[2] = { a, b }; set_uniform(l, 2, 1, v, NULL, 0); }
GLAPI(void, glUniform3f, (GLint l, GLfloat a, GLfloat b, GLfloat c)) { float v[3] = { a, b, c }; set_uniform(l, 3, 1, v, NULL, 0); }
GLAPI(void, glUniform4f, (GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d)) { float v[4] = { a, b, c, d }; set_uniform(l, 4, 1, v, NULL, 0); }
GLAPI(void, glUniform1i, (GLint l, GLint a)) { GLint v[1] = { a }; set_uniform(l, 1, 1, NULL, v, 0); }
GLAPI(void, glUniform2i, (GLint l, GLint a, GLint b)) { GLint v[2] = { a, b }; set_uniform(l, 2, 1, NULL, v, 0); }
GLAPI(void, glUniform3i, (GLint l, GLint a, GLint b, GLint c)) { GLint v[3] = { a, b, c }; set_uniform(l, 3, 1, NULL, v, 0); }
GLAPI(void, glUniform4i, (GLint l, GLint a, GLint b, GLint c, GLint d)) { GLint v[4] = { a, b, c, d }; set_uniform(l, 4, 1, NULL, v, 0); }
GLAPI(void, glUniform1fv, (GLint l, GLsizei n, const GLfloat *v)) { set_uniform(l, 1, n, v, NULL, 0); }
GLAPI(void, glUniform2fv, (GLint l, GLsizei n, const GLfloat *v)) { set_uniform(l, 2, n, v, NULL, 0); }
GLAPI(void, glUniform3fv, (GLint l, GLsizei n, const GLfloat *v)) { set_uniform(l, 3, n, v, NULL, 0); }
GLAPI(void, glUniform4fv, (GLint l, GLsizei n, const GLfloat *v)) { set_uniform(l, 4, n, v, NULL, 0); }
GLAPI(void, glUniform1iv, (GLint l, GLsizei n, const GLint *v)) { set_uniform(l, 1, n, NULL, v, 0); }
GLAPI(void, glUniform2iv, (GLint l, GLsizei n, const GLint *v)) { set_uniform(l, 2, n, NULL, v, 0); }
GLAPI(void, glUniform3iv, (GLint l, GLsizei n, const GLint *v)) { set_uniform(l, 3, n, NULL, v, 0); }
GLAPI(void, glUniform4iv, (GLint l, GLsizei n, const GLint *v)) { set_uniform(l, 4, n, NULL, v, 0); }
GLAPI(void, glUniformMatrix2fv, (GLint l, GLsizei n, GLboolean t, const GLfloat *v)) { set_uniform(l, 4, n, v, NULL, t); }
GLAPI(void, glUniformMatrix3fv, (GLint l, GLsizei n, GLboolean t, const GLfloat *v)) { set_uniform(l, 9, n, v, NULL, t); }
GLAPI(void, glUniformMatrix4fv, (GLint l, GLsizei n, GLboolean t, const GLfloat *v)) { set_uniform(l, 16, n, v, NULL, t); }

GLAPI(void, glGetUniformfv, (GLuint program, GLint loc, GLfloat *out))
{
    GlProgram *p = find_program(program);
    float *slot;
    int uc, rem, isint;
    if (p && p->p && (slot = glsl_uniform_slot(p->p, loc, &uc, &rem, &isint))) memcpy(out, slot, (size_t)uc * sizeof(float));
}
GLAPI(void, glGetUniformiv, (GLuint program, GLint loc, GLint *out))
{
    float f[16];
    GlProgram *p = find_program(program);
    float *slot;
    int uc, rem, isint, i;
    if (p && p->p && (slot = glsl_uniform_slot(p->p, loc, &uc, &rem, &isint))) { memcpy(f, slot, (size_t)uc * sizeof(float)); for (i = 0; i < uc; i++) out[i] = (GLint)f[i]; }
}

/* generic vertex attributes */
GLAPI(void, glVertexAttribPointer, (GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const GLvoid *ptr))
{
    ensure_init();
    if (i >= 16) { set_error(GL_INVALID_VALUE); return; }
    set_array(&G.gattr[i], size == GL_BGRA ? 4 : size, type, stride, ptr);
    G.gattr[i].bgra = size == GL_BGRA;
    G.gnorm[i] = norm != 0;
}
GLAPI(void, glEnableVertexAttribArray, (GLuint i)) { ensure_init(); if (i < 16) G.gattr[i].enabled = 1; }
GLAPI(void, glDisableVertexAttribArray, (GLuint i)) { ensure_init(); if (i < 16) G.gattr[i].enabled = 0; }
GLAPI(void, glVertexAttrib4f, (GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w)) { ensure_init(); if (i < 16) { G.gcur[i][0] = x; G.gcur[i][1] = y; G.gcur[i][2] = z; G.gcur[i][3] = w; } }
GLAPI(void, glVertexAttrib1f, (GLuint i, GLfloat x)) { GLFN(glVertexAttrib4f)(i, x, 0, 0, 1); }
GLAPI(void, glVertexAttrib2f, (GLuint i, GLfloat x, GLfloat y)) { GLFN(glVertexAttrib4f)(i, x, y, 0, 1); }
GLAPI(void, glVertexAttrib3f, (GLuint i, GLfloat x, GLfloat y, GLfloat z)) { GLFN(glVertexAttrib4f)(i, x, y, z, 1); }
GLAPI(void, glVertexAttrib1fv, (GLuint i, const GLfloat *v)) { GLFN(glVertexAttrib4f)(i, v[0], 0, 0, 1); }
GLAPI(void, glVertexAttrib2fv, (GLuint i, const GLfloat *v)) { GLFN(glVertexAttrib4f)(i, v[0], v[1], 0, 1); }
GLAPI(void, glVertexAttrib3fv, (GLuint i, const GLfloat *v)) { GLFN(glVertexAttrib4f)(i, v[0], v[1], v[2], 1); }
GLAPI(void, glVertexAttrib4fv, (GLuint i, const GLfloat *v)) { GLFN(glVertexAttrib4f)(i, v[0], v[1], v[2], v[3]); }
GLAPI(void, glGetVertexAttribiv, (GLuint i, GLenum p, GLint *out))
{
    *out = 0;
    if (i >= 16) return;
    if (p == GL_VERTEX_ATTRIB_ARRAY_ENABLED) *out = G.gattr[i].enabled;
    else if (p == GL_VERTEX_ATTRIB_ARRAY_SIZE) *out = G.gattr[i].size;
}
GLAPI(void, glGetVertexAttribfv, (GLuint i, GLenum p, GLfloat *out)) { if (i < 16 && p == GL_CURRENT_VERTEX_ATTRIB) memcpy(out, G.gcur[i], 16); }

/* vertex array objects: the generic arrays and the element buffer, swapped in and out */
static Vao *find_vao(GLuint n) { Vao *v; for (v = vaos; v; v = v->next) if (v->name == n) return v; return NULL; }
static Vao *vao_slot(GLuint n)
{
    Vao *v = find_vao(n);
    if (!v && (v = (Vao *)calloc(1, sizeof(*v)))) { v->name = n; v->next = vaos; vaos = v; }
    return v;
}
GLAPI(void, glGenVertexArrays, (GLsizei n, GLuint *out)) { int i; ensure_init(); for (i = 0; i < n; i++) { out[i] = G.next_object++; vao_slot(out[i]); } }
GLAPI(void, glBindVertexArray, (GLuint name))
{
    Vao *old, *nw;
    ensure_init();
    if (name == G.vao) return;
    if (!(old = vao_slot(G.vao)) || !(nw = vao_slot(name))) { set_error(GL_OUT_OF_MEMORY); return; }
    memcpy(old->gattr, G.gattr, sizeof(G.gattr)); memcpy(old->gnorm, G.gnorm, sizeof(G.gnorm)); old->element_buffer = G.element_buffer;
    memcpy(G.gattr, nw->gattr, sizeof(G.gattr)); memcpy(G.gnorm, nw->gnorm, sizeof(G.gnorm)); G.element_buffer = nw->element_buffer;
    G.vao = name;
}
GLAPI(void, glDeleteVertexArrays, (GLsizei n, const GLuint *names))
{
    int i;
    for (i = 0; i < n; i++) {
        Vao **pp = &vaos;
        if (names[i] == G.vao) GLFN(glBindVertexArray)(0);
        while (*pp && (*pp)->name != names[i]) pp = &(*pp)->next;
        if (*pp && names[i]) { Vao *v = *pp; *pp = v->next; free(v); }
    }
}
GLAPI(GLboolean, glIsVertexArray, (GLuint name)) { return name && find_vao(name) != NULL; }

/* framebuffer objects: a colour texture becomes a render target on the PSP
 * (select_target); depth and stencil attachments are not kept (the screen's
 * depth buffer serves) */
typedef struct Fbo { GLuint name, tex; struct Fbo *next; } Fbo;
static Fbo *fbos;
static Fbo *find_fbo(GLuint n) { Fbo *f; for (f = fbos; f; f = f->next) if (f->name == n) return f; return NULL; }
static GlTex *fbo_texture(GLuint fb) { Fbo *f = find_fbo(fb); return f && f->tex ? find_texture(f->tex) : NULL; }

GLAPI(void, glGenFramebuffers, (GLsizei n, GLuint *out))
{
    int i;
    ensure_init();
    for (i = 0; i < n; i++) {
        Fbo *f = (Fbo *)calloc(1, sizeof(*f));
        out[i] = G.next_object++;
        if (f) { f->name = out[i]; f->next = fbos; fbos = f; }
    }
}
GLAPI(void, glBindFramebuffer, (GLenum target, GLuint fb))
{
    ensure_init();
    if (target == GL_READ_FRAMEBUFFER) return;
    if (fb && !find_fbo(fb)) { Fbo *f = (Fbo *)calloc(1, sizeof(*f)); if (f) { f->name = fb; f->next = fbos; fbos = f; } }
    G.fbo = fb;
}
GLAPI(void, glDeleteFramebuffers, (GLsizei n, const GLuint *fbs))
{
    int i;
    for (i = 0; i < n; i++) {
        Fbo **pp = &fbos;
        if (fbs[i] == G.fbo) G.fbo = 0;
        while (*pp && (*pp)->name != fbs[i]) pp = &(*pp)->next;
        if (*pp) { Fbo *f = *pp; *pp = f->next; free(f); }
    }
}
GLAPI(void, glFramebufferTexture2D, (GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l))
{
    Fbo *f = find_fbo(G.fbo);
    (void)t; (void)tt; (void)l;
    if (f && a == 0x8CE0 /* GL_COLOR_ATTACHMENT0 */) f->tex = tex;
}
GLAPI(GLenum, glCheckFramebufferStatus, (GLenum t)) { (void)t; return GL_FRAMEBUFFER_COMPLETE; }
GLAPI(GLboolean, glIsFramebuffer, (GLuint fb)) { return fb != 0; }
GLAPI(void, glGenRenderbuffers, (GLsizei n, GLuint *out)) { int i; ensure_init(); for (i = 0; i < n; i++) out[i] = G.next_object++; }
GLAPI(void, glBindRenderbuffer, (GLenum t, GLuint rb)) { (void)t; (void)rb; }
GLAPI(void, glDeleteRenderbuffers, (GLsizei n, const GLuint *rb)) { (void)n; (void)rb; }
GLAPI(void, glRenderbufferStorage, (GLenum t, GLenum f, GLsizei w, GLsizei h)) { (void)t; (void)f; (void)w; (void)h; }
GLAPI(void, glFramebufferRenderbuffer, (GLenum t, GLenum a, GLenum rt, GLuint rb)) { (void)t; (void)a; (void)rt; (void)rb; }
GLAPI(void, glBlitFramebuffer, (GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield m, GLenum fl))
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)m; (void)fl; }
GLAPI(void, glDrawBuffers, (GLsizei n, const GLenum *b)) { (void)n; (void)b; }
GLAPI(void, glSampleCoverage, (GLclampf v, GLboolean inv)) { (void)v; (void)inv; }
GLEXT(void, glReleaseShaderCompiler, (void)) { }
GLEXT(void, glGetShaderPrecisionFormat, (GLenum s, GLenum p, GLint *range, GLint *precision))
{ (void)s; (void)p; range[0] = 127; range[1] = 127; *precision = 23; }

/* ---- a shader's cube map: a sphere map made from it ---------------------------------------
 *
 * The GE has no cube maps and one texture. A shader's reflection (a cube
 * map along the eye vector reflected about the normal, added to the
 * colour) is drawn in a pass of its own as the GE's environment map: a
 * sphere map, indexed by the eye-space normal's x and y, made here from
 * the cube map through the shader's own expression for the direction, the
 * view straight ahead. It is made again when that changes (the camera
 * turns), a few kept for the directions of the moment. */

#define ENV_SIZE 64
#define ENV_GRID 17        /* directions worked out on a grid this size */
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6; }
static GlTex *texture_object(GLuint name);
#define ENV_MAPS 4

typedef struct EnvMap { GLuint tex, cube; uint32_t cube_ver; float scale[3]; int alpha; float probe[5][3]; unsigned used; } EnvMap;
static EnvMap envmaps[ENV_MAPS];
static unsigned env_frame;

static void cube_sample(const GlTex *c, const float d[3], float out[4])
{
    float ax = fabsf(d[0]), ay = fabsf(d[1]), az = fabsf(d[2]), sc, tc, ma;
    int f, x, y;
    uint32_t p;
    if (ax >= ay && ax >= az) { ma = ax; f = d[0] >= 0 ? 0 : 1; sc = d[0] >= 0 ? -d[2] : d[2]; tc = -d[1]; }
    else if (ay >= az) { ma = ay; f = d[1] >= 0 ? 2 : 3; sc = d[0]; tc = d[1] >= 0 ? d[2] : -d[2]; }
    else { ma = az; f = d[2] >= 0 ? 4 : 5; sc = d[2] >= 0 ? d[0] : -d[0]; tc = -d[1]; }
    if (ma < 1e-12f || !c->face[f]) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    x = (int)((sc / ma + 1.0f) * 0.5f * (float)c->face_w);
    y = (int)((tc / ma + 1.0f) * 0.5f * (float)c->face_h);
    x = x < 0 ? 0 : x >= c->face_w ? c->face_w - 1 : x;
    y = y < 0 ? 0 : y >= c->face_h ? c->face_h - 1 : y;
    p = c->face[f][(size_t)y * (size_t)c->face_w + (size_t)x];
    out[0] = (float)(p & 0xFF); out[1] = (float)((p >> 8) & 0xFF); out[2] = (float)((p >> 16) & 0xFF); out[3] = (float)(p >> 24);
}

static GlTex *env_map(const GlslPlan *pl)
{
    static const float pn[5][3] = { { 0, 0, 1 }, { 0.8f, 0, 0.6f }, { -0.8f, 0, 0.6f }, { 0, 0.8f, 0.6f }, { 0, -0.8f, 0.6f } };
    GlTex *cube = find_texture(G.cube_units[pl->env_unit & 7]), *t;
    EnvMap *e, *slot = NULL;
    float probe[5][3];
    int i, j, k;
    double t0 = now_ms();
    if (!cube || !cube->face[0]) return NULL;
    for (i = 0; i < 5; i++) glsl_plan_env_dir(pl, pn[i], probe[i]);
    for (e = envmaps; e < envmaps + ENV_MAPS; e++) {
        int same = e->tex && e->cube == cube->name && e->cube_ver == cube->face_ver && e->alpha == pl->env_alpha &&
                   !memcmp(e->scale, pl->env_scale, sizeof(e->scale));
        float d = 0;
        if (!same) continue;
        for (i = 0; i < 5; i++) for (k = 0; k < 3; k++) if (fabsf(e->probe[i][k] - probe[i][k]) > d) d = fabsf(e->probe[i][k] - probe[i][k]);
        if (d < 0.01f && (t = find_texture(e->tex))) { e->used = env_frame; return t; }
        if (e->used != env_frame) slot = e;   /* the same map, the view turned: made again in place */
    }
    if (!slot)
        for (e = envmaps; e < envmaps + ENV_MAPS; e++)
            if (e->used != env_frame && (!slot || e->used < slot->used || !e->tex)) slot = e;
    if (!slot) slot = &envmaps[0];
    if (!slot->tex || !(t = find_texture(slot->tex))) {
        while (find_texture(G.next_texture)) G.next_texture++;
        slot->tex = G.next_texture++;
        if (!(t = texture_object(slot->tex))) return NULL;
        t->min_filter = t->mag_filter = GL_LINEAR;
        t->wrap_s = t->wrap_t = GL_CLAMP_TO_EDGE;
    }
    if (!t->rgba || t->w != ENV_SIZE || t->h != ENV_SIZE) {
        uint32_t *p = (uint32_t *)realloc(t->rgba, ENV_SIZE * ENV_SIZE * 4);
        if (!p) return NULL;
        t->rgba = p; t->w = t->h = ENV_SIZE;
    }
    t->has_alpha = pl->env_alpha;
    {   /* the directions on a coarser grid, worked out through the shader; between, interpolated */
        static float grid[ENV_GRID][ENV_GRID][3];
        for (j = 0; j < ENV_GRID; j++)
            for (i = 0; i < ENV_GRID; i++) {
                float n[3], r2;
                n[0] = (float)i * (2.0f / (ENV_GRID - 1)) - 1.0f;
                n[1] = (float)j * (2.0f / (ENV_GRID - 1)) - 1.0f;
                r2 = n[0] * n[0] + n[1] * n[1];
                if (r2 > 1.0f) { float f = 1.0f / sqrtf(r2); n[0] *= f; n[1] *= f; r2 = 1.0f; }
                n[2] = sqrtf(1.0f - r2);
                glsl_plan_env_dir(pl, n, grid[j][i]);
            }
    for (j = 0; j < ENV_SIZE; j++)
        for (i = 0; i < ENV_SIZE; i++) {
            float d[3], c[4];
            float fx = ((float)i + 0.5f) * ((float)(ENV_GRID - 1) / ENV_SIZE), fy = ((float)j + 0.5f) * ((float)(ENV_GRID - 1) / ENV_SIZE);
            int gx = (int)fx, gy = (int)fy;
            uint32_t px = 0;
            fx -= (float)gx; fy -= (float)gy;
            for (k = 0; k < 3; k++)
                d[k] = (grid[gy][gx][k] * (1 - fx) + grid[gy][gx + 1][k] * fx) * (1 - fy) +
                       (grid[gy + 1][gx][k] * (1 - fx) + grid[gy + 1][gx + 1][k] * fx) * fy;
            cube_sample(cube, d, c);
            for (k = 0; k < 3; k++) {
                float v = c[k] * pl->env_scale[k];
                px |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f) << (k * 8);
            }
            px |= (uint32_t)c[3] << 24;
            t->rgba[j * ENV_SIZE + i] = px;
        }
    }
    t->dirty = 1;
    slot->cube = cube->name; slot->cube_ver = cube->face_ver; slot->alpha = pl->env_alpha;
    memcpy(slot->scale, pl->env_scale, sizeof(slot->scale));
    memcpy(slot->probe, probe, sizeof(probe));
    slot->used = env_frame;
    if (G.log >= 2) gl_logf("sphere map %u made from cube map %u in %.1f ms", slot->tex, cube->name, now_ms() - t0);
    return t;
}

/* A shader's draw in passes. SH_PRE: before, when the discard is on a texture the colour does not
 * use: that texture's alpha test, only the depth written; SH_MAIN: its colour; SH_MOD: a second
 * texture multiplied in (once or twice over: blended with the colour drawn); SH_ADD: a third one
 * added; SH_ENV: a cube map added (or the colour itself), as a sphere map. The passes after the
 * colour's land only where it drew (depth EQUAL). sh_pass sets up a pass over GL's state (0: not
 * needed), sh_pass_end puts that back. */
enum { SH_PRE, SH_MAIN, SH_MOD, SH_ADD, SH_ENV, SH_PASSES };
static struct { int blend, depth_mask, cm[4]; GLenum bsrc, bdst, beq, dfunc; } sh_gl;

static int sh_pass(const GlslPlan *pl, int k)
{
    if (k == 0) {
        sh_gl.blend = G.blend; sh_gl.depth_mask = G.depth_mask; memcpy(sh_gl.cm, G.color_mask, sizeof(sh_gl.cm));
        sh_gl.bsrc = G.blend_src; sh_gl.bdst = G.blend_dst; sh_gl.beq = G.blend_eq; sh_gl.dfunc = G.depth_func;
    } else {
        G.blend = sh_gl.blend; G.depth_mask = sh_gl.depth_mask; memcpy(G.color_mask, sh_gl.cm, sizeof(sh_gl.cm));
        G.blend_src = sh_gl.bsrc; G.blend_dst = sh_gl.bdst; G.blend_eq = sh_gl.beq; G.depth_func = sh_gl.dfunc;
    }
    G.sh_active = 1;
    G.sh_env = 0; G.sh_lit = NULL; G.sh_tex = NULL; G.sh_alpha_test = 0; G.sh_uvset = 0; G.sh_replace = 0;
    G.sh_tex_alpha = pl->tex_alpha;
    G.sh_alpha_func = pl->alpha_func; G.sh_alpha_ref = pl->alpha_ref;
    switch (k) {
    case SH_PRE:
        if (!pl->pre_test) return 0;
        G.sh_tex = find_texture(G.units[pl->pre_unit & 7]);
        G.sh_tex_alpha = 1;
        G.sh_alpha_test = 1;
        memset(G.color_mask, 0, sizeof(G.color_mask));
        G.blend = 0;
        G.depth_mask = 1;
        if (G.depth_func == GL_LESS) G.depth_func = GL_LEQUAL;
        else if (G.depth_func == GL_GREATER) G.depth_func = GL_GEQUAL;
        return 1;
    case SH_MAIN:
        if (pl->env_only) return 0;
        G.sh_tex = pl->textured ? find_texture(G.units[pl->sampler_unit & 7]) : NULL;
        G.sh_alpha_test = pl->alpha_test;
        G.sh_lit = pl->lit ? pl : NULL;
        if (pl->pre_test) G.depth_func = GL_EQUAL;
        return 1;
    case SH_MOD: case SH_ADD:
        if (pl->env_only || !(k == SH_MOD ? pl->mod_tex : pl->add_tex)) return 0;
        if (!(G.sh_tex = find_texture(G.units[(k == SH_MOD ? pl->mod_unit : pl->add_unit) & 7])) || (!G.sh_tex->rgba && !G.sh_tex->rt)) return 0;
        G.sh_uvset = k == SH_MOD ? 1 : 2;
        G.sh_replace = 1;
        G.blend = 1; G.blend_eq = GL_FUNC_ADD;
        if (k == SH_MOD) {   /* x1 or x2 by the blend; more: the texture sent brighter (clamped) */
            float want = pl->mod_scale >= 2 ? pl->mod_scale / 2 : pl->mod_scale;
            G.blend_src = GL_DST_COLOR; G.blend_dst = pl->mod_scale >= 2 ? GL_SRC_COLOR : GL_ZERO;
            if (G.sh_tex->scale != want) { G.sh_tex->scale = want; G.sh_tex->dirty = 1; }
        }
        else G.blend_src = G.blend_dst = GL_ONE;
        if (G.depth_mask) G.depth_func = GL_EQUAL;
        G.depth_mask = 0;
        return 1;
    case SH_ENV:
        if (!pl->env || !(G.sh_tex = env_map(pl))) return 0;
        G.sh_env = 1;
        G.sh_env_alpha = pl->env_alpha;
        memcpy(G.sh_env_axes, pl->env_axes, sizeof(G.sh_env_axes));
        if (pl->env_only) {
            G.sh_alpha_test = pl->alpha_test;
            if (pl->pre_test) G.depth_func = GL_EQUAL;
        } else {   /* added to what pass 1 drew */
            G.blend = 1; G.blend_src = G.blend_dst = GL_ONE; G.blend_eq = GL_FUNC_ADD;
            if (G.depth_mask) G.depth_func = GL_EQUAL;
            G.depth_mask = 0;
        }
        return 1;
    default:
        return 0;
    }
}

static void sh_pass_end(void)
{
    G.blend = sh_gl.blend; G.depth_mask = sh_gl.depth_mask; memcpy(G.color_mask, sh_gl.cm, sizeof(sh_gl.cm));
    G.blend_src = sh_gl.bsrc; G.blend_dst = sh_gl.bdst; G.blend_eq = sh_gl.beq; G.depth_func = sh_gl.dfunc;
    G.sh_active = 0; G.sh_lit = NULL; G.sh_env = 0; G.sh_uvset = 0; G.sh_replace = 0;
}

/* A draw by a shader program: its plan for the uniforms of now, each
 * vertex's attributes through it, the GE drawing what the plan says. */
static int shader_mesh(GlProgram *pr, GLenum mode, const int *idx, int first, int count, GLuint ibo, uintptr_t ioffset, GLenum itype);

static void shader_draw(GLenum mode, const int *idx, int first, int count, GLuint ibo, uintptr_t ioffset, GLenum itype)
{
    GlProgram *p = find_program(G.program);
    GlslPlan *pl;
    Vtx *v;
    float attrs[GLSL_ATTR_ALL][4], *uvs = NULL;
    int i, a, fixed = G.va.enabled || G.ca.enabled || G.na.enabled || G.ta.enabled, set = 0;
    if (!p || !p->p || count <= 0) return;
    if (!p->planned || p->planned_ver != p->uver || p->builtins) {
        glsl_plan(p->p, &p->plan);
        p->planned = 1; p->planned_ver = p->uver;
        if (!p->plan.exact && !p->reported) { p->reported = 1; gl_logf("program %u: drawn as near as the GE can (%s)", G.program, p->plan.why); }
    }
    pl = &p->plan;
    if (G.log >= 2 && !p->described) {
        p->described = 1;
        gl_logf("program %u first drawn: blend %d %x %x, depth %d %x mask %d, cull %d, units %u %u %u cubes %u %u %u, textured %d unit %d, lit %d, alpha test %d",
                G.program, G.blend, G.blend_src, G.blend_dst, G.depth_test, G.depth_func, G.depth_mask, G.cull,
                G.units[0], G.units[1], G.units[2], G.cube_units[0], G.cube_units[1], G.cube_units[2], pl->textured, pl->sampler_unit, pl->lit, pl->alpha_test);
    }
    if (shader_mesh(p, mode, idx, first, count, ibo, ioffset, itype)) return;
    if (!(v = gather(count))) { set_error(GL_OUT_OF_MEMORY); return; }
    if (pl->mod_tex || pl->add_tex) {
        static float *keep;
        static int cap;
        if (count > cap) { float *n2 = (float *)realloc(keep, (size_t)count * 6 * sizeof(float)); if (!n2) return; keep = n2; cap = count; }
        uvs = keep;
    }
    {   /* the arrays, found once for the draw: base, stride, element size (-1: the current value) */
        static const uint8_t *base[16];
        static int stride[16], ts[16], live[16], nlive;
        nlive = 0;
        for (a = 0; a < 16; a++) {
            memcpy(attrs[a], G.gcur[a], 16);
            if (!G.gattr[a].enabled || !(base[a] = array_base(&G.gattr[a]))) continue;
            ts[a] = type_size(G.gattr[a].type);
            stride[a] = G.gattr[a].stride ? G.gattr[a].stride : G.gattr[a].size * ts[a];
            live[nlive++] = a;
        }
    for (i = 0; i < count; i++) {
        int k = idx ? idx[i] : first + i, l, cmp;
        float pos[4], uv[2], c[4];
        for (l = 0; l < nlive; l++) {
            const Array *ar;
            const uint8_t *b;
            a = live[l];
            ar = &G.gattr[a];
            b = base[a] + (size_t)k * (size_t)stride[a];
            if (ar->type == GL_FLOAT && !G.gnorm[a]) {
                float f[4] = { 0, 0, 0, 1 };
                memcpy(f, b, (size_t)(ar->size < 4 ? ar->size : 4) * 4);
                memcpy(attrs[a], f, 16);
            } else {
                for (cmp = 0; cmp < 4; cmp++) attrs[a][cmp] = cmp < ar->size ? read_comp(b + cmp * ts[a], ar->type, G.gnorm[a]) : cmp == 3 ? 1.0f : 0.0f;
                if (ar->bgra) { float t = attrs[a][0]; attrs[a][0] = attrs[a][2]; attrs[a][2] = t; }
            }
        }
        if (fixed) {   /* old shaders' gl_Vertex, gl_Color...: the fixed-function arrays */
            Vtx f;
            fetch(k, &f);
            attrs[GLSL_ATTR_VERTEX][0] = f.p[0]; attrs[GLSL_ATTR_VERTEX][1] = f.p[1]; attrs[GLSL_ATTR_VERTEX][2] = f.p[2]; attrs[GLSL_ATTR_VERTEX][3] = 1;
            attrs[GLSL_ATTR_COLOR][0] = (f.c & 0xFF) / 255.0f; attrs[GLSL_ATTR_COLOR][1] = ((f.c >> 8) & 0xFF) / 255.0f;
            attrs[GLSL_ATTR_COLOR][2] = ((f.c >> 16) & 0xFF) / 255.0f; attrs[GLSL_ATTR_COLOR][3] = (f.c >> 24) / 255.0f;
            memcpy(attrs[GLSL_ATTR_NORMAL], f.n, 12); attrs[GLSL_ATTR_NORMAL][3] = 0;
            attrs[GLSL_ATTR_TEXCOORD0][0] = f.t[0]; attrs[GLSL_ATTR_TEXCOORD0][1] = f.t[1];
            attrs[GLSL_ATTR_TEXCOORD0][2] = 0; attrs[GLSL_ATTR_TEXCOORD0][3] = 1;
        }
        glsl_plan_vertex(pl, (const float (*)[4])attrs, pos, uv, c);
        if (!pl->matrix_ok && pos[3] != 0.0f && pos[3] != 1.0f) { pos[0] /= pos[3]; pos[1] /= pos[3]; pos[2] /= pos[3]; }
        v[i].p[0] = pos[0]; v[i].p[1] = pos[1]; v[i].p[2] = pos[2];
        v[i].t[0] = uv[0]; v[i].t[1] = uv[1];
        if (uvs) {   /* the other passes' coordinates */
            uvs[(size_t)i * 6] = uv[0]; uvs[(size_t)i * 6 + 1] = uv[1];
            if (pl->mod_tex) glsl_plan_uv(pl, 1, (const float (*)[4])attrs, uvs + (size_t)i * 6 + 2);
            if (pl->add_tex) glsl_plan_uv(pl, 2, (const float (*)[4])attrs, uvs + (size_t)i * 6 + 4);
        }
        v[i].c = abgr(c);
        if (pl->lit) memcpy(v[i].n, attrs[pl->lit_attr], 12);
        else if (pl->env) memcpy(v[i].n, attrs[pl->env_attr], 12);
        else { v[i].n[0] = v[i].n[1] = 0; v[i].n[2] = 1; }
    }
    }
    if (G.log >= 3) {
        const float *m = pl->matrix;
        float x = v[0].p[0], y = v[0].p[1], z = v[0].p[2];
        gl_logf("program %u: %s attr %d  clip of vertex 0: %.2f %.2f %.2f %.2f  viewport %d %d %d %d", G.program,
                pl->matrix_ok ? "matrix" : "per vertex", pl->pos_attr,
                m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13],
                m[2] * x + m[6] * y + m[10] * z + m[14], m[3] * x + m[7] * y + m[11] * z + m[15],
                G.viewport[0], G.viewport[1], G.viewport[2], G.viewport[3]);
        if (G.log >= 4) {
            int r;
            for (r = 0; r < 4; r++) gl_logf("  matrix row %d: %g %g %g %g", r, m[r], m[4 + r], m[8 + r], m[12 + r]);
            for (r = 0; r < count && r < 4; r++) gl_logf("  vertex %d: %g %g %g", r, v[r].p[0], v[r].p[1], v[r].p[2]);
        }
    }
    if (pl->matrix_ok) memcpy(G.sh_proj, pl->matrix, sizeof(G.sh_proj));
    else mat_identity(G.sh_proj);
    for (i = 0; i < SH_PASSES; i++) {
        if (!sh_pass(pl, i)) continue;
        if (uvs && G.sh_uvset != set) {
            int j;
            set = G.sh_uvset;
            for (j = 0; j < count; j++) { v[j].t[0] = uvs[(size_t)j * 6 + set * 2]; v[j].t[1] = uvs[(size_t)j * 6 + set * 2 + 1]; }
        }
        draw_vertices(mode, v, count);
    }
    sh_pass_end();
}

/* ---- meshes: buffer objects kept on the PSP ------------------------------------------
 *
 * A draw whose arrays all come from buffer objects is converted once into
 * the GE's vertex format, the whole buffer at a time, and kept on the PSP
 * (a vertex mesh); element buffers the same way (an index mesh). A frame
 * then sends a few bytes per draw. The key is everything the conversion
 * read: the arrays, the buffers' contents (a version each), the vertex type,
 * and the current normal / texture coordinate when there is no array for
 * them. The current colour is not in it: vertices without a colour array
 * take it from the material (prepare_colour). */

#define MESH_BUDGET (7u * 1024u * 1024u)   /* of the PSP's RAM for resources */

typedef struct Mesh {
    uint64_t hash;              /* a mesh by content (content_mesh): its vertices' hash; 0 for a buffer's */
    int seen;                   /* ...drawn that many times; made on the PSP from the second */
    Array a[4];                 /* va, ca, na, ta as they were */
    uint32_t version[4];
    uint32_t vt;
    float normal[3], texcoord[2];
    int count;                  /* vertices converted, from 0 */
    uint16_t handle;
    uint32_t bytes;
    unsigned used;              /* the frame it was last drawn in */
    float *pos;                 /* the positions, here too: where they fall is worked out each frame (clipping) */
    Vtx *vx;                    /* a shader's mesh: its vertices as worked out (the cut ones are made from them) */
    uint64_t shkey;             /* ...and its key: the plan's per-vertex work, the arrays and buffers it read */
    GLuint bufs[8]; int nbufs;  /* ...the buffers it was made from */
    unsigned *code, code_frame; int code_any; float code_m[16];
    struct Mesh *next;
} Mesh;

static void mesh_free(Mesh *m) { free(m->pos); free(m->code); free(m->vx); free(m); }
static int mesh_clipped(PspExpContext *ctx, Mesh *m, const Clip *c, GLenum mode, const int *idx, int first, int count, uint32_t vt);

typedef struct IndexMesh {
    GLuint buffer; uint32_t version; uintptr_t offset; GLenum type; int count; GLenum mode;
    int n; uint8_t prim;        /* after conversion (quads to triangles, loops closed) */
    uint16_t handle;
    uint32_t bytes;
    unsigned used;
    struct IndexMesh *next;
} IndexMesh;

static Mesh *meshes;
static IndexMesh *index_meshes;
static uint32_t mesh_bytes;
static unsigned mesh_frame = 1;
/* Dropped meshes are released a frame late: this frame's display list,
 * which the GE runs at present, may still read them. */
static uint16_t releases[2][2048];
static int nreleases[2], rel_cur;

static uint16_t mesh_handle(void) { return nfree_handles ? free_handles[--nfree_handles] : shim_gl_handle(); }

static void mesh_release(uint16_t h, uint32_t bytes)
{
    mesh_bytes -= bytes;
    if (nreleases[rel_cur] < 2048) releases[rel_cur][nreleases[rel_cur]++] = h;
}

/* Every mesh made from this buffer, dropped (its contents changed, or it went). */
static void meshes_forget_buffer(GLuint name)
{
    Mesh **pp = &meshes;
    IndexMesh **ip = &index_meshes;
    while (*pp) {
        Mesh *m = *pp;
        int k, hit = 0;
        for (k = 0; k < 4; k++) if (!m->hash && !m->shkey && m->a[k].enabled && m->a[k].buffer == name) hit = 1;
        for (k = 0; k < m->nbufs; k++) if (m->bufs[k] == name) hit = 1;
        if (hit) { *pp = m->next; mesh_release(m->handle, m->bytes); mesh_free(m); } else pp = &m->next;
    }
    while (*ip) {
        IndexMesh *m = *ip;
        if (m->buffer == name) { *ip = m->next; mesh_release(m->handle, m->bytes); free(m); } else ip = &m->next;
    }
}

/* Room for `bytes` more: the least recently drawn go first (never this frame's). */
static int mesh_room(uint32_t bytes)
{
    while (mesh_bytes + bytes > MESH_BUDGET) {
        Mesh **pp, **oldest = NULL;
        IndexMesh **ip, **ioldest = NULL;
        for (pp = &meshes; *pp; pp = &(*pp)->next) if ((*pp)->used != mesh_frame && (!oldest || (*pp)->used < (*oldest)->used)) oldest = pp;
        for (ip = &index_meshes; *ip; ip = &(*ip)->next) if ((*ip)->used != mesh_frame && (!ioldest || (*ip)->used < (*ioldest)->used)) ioldest = ip;
        if (oldest && (!ioldest || (*oldest)->used <= (*ioldest)->used)) {
            Mesh *m = *oldest; *oldest = m->next; if (m->handle) mesh_release(m->handle, m->bytes); mesh_free(m);
        } else if (ioldest) {
            IndexMesh *m = *ioldest; *ioldest = m->next; mesh_release(m->handle, m->bytes); free(m);
        } else return 0;
    }
    return 1;
}

static uint32_t buffer_version(GLuint name) { GlBuf *b = find_buffer(name); return b ? b->version : 0; }

/* How many whole vertices the enabled arrays' buffers hold. */
static int buffer_vertices(void)
{
    const Array *arr[4] = { &G.va, &G.ca, &G.na, &G.ta };
    int k, n = 1 << 30;
    for (k = 0; k < 4; k++) {
        const Array *a = arr[k];
        GlBuf *b;
        int size, stride, elem;
        if (!a->enabled) continue;
        if (!a->buffer || !(b = find_buffer(a->buffer)) || !b->data) return 0;
        size = k == 2 ? 3 : a->size;
        elem = size * type_size(a->type);
        stride = a->stride ? a->stride : elem;
        if (elem <= 0 || stride <= 0 || (uintptr_t)a->ptr + (size_t)elem > b->size) return 0;
        {
            int c = (int)((b->size - (uintptr_t)a->ptr - (size_t)elem) / (size_t)stride) + 1;
            if (c < n) n = c;
        }
    }
    return n == 1 << 30 ? 0 : n;
}

static void pack_vertex(uint8_t **out, uint32_t vt, const Vtx *v)
{
    uint8_t *o = *out;
    if (vt & PSPEXP_3D_VT_TEXTURE) { memcpy(o, v->t, 8); o += 8; }
    if (vt & PSPEXP_3D_VT_COLOR) { memcpy(o, &v->c, 4); o += 4; }
    if (vt & PSPEXP_3D_VT_NORMAL) { memcpy(o, v->n, 12); o += 12; }
    memcpy(o, v->p, 12); o += 12;
    *out = o;
}

static Mesh *vertex_mesh(PspExpContext *ctx, uint32_t vt, int need)
{
    Mesh *m;
    Array a[4] = { G.va, G.ca, G.na, G.ta };
    uint32_t ver[4];
    int k, n, i;
    uint8_t *out;
    for (k = 0; k < 4; k++) ver[k] = a[k].enabled ? buffer_version(a[k].buffer) : 0;
    for (m = meshes; m; m = m->next) {
        int same = !m->hash && !m->shkey && m->vt == vt && m->count >= need;
        for (k = 0; same && k < 4; k++)
            same = m->a[k].enabled == a[k].enabled && (!a[k].enabled ||
                   (m->a[k].buffer == a[k].buffer && m->a[k].ptr == a[k].ptr && m->a[k].size == a[k].size &&
                    m->a[k].type == a[k].type && m->a[k].stride == a[k].stride && m->a[k].bgra == a[k].bgra && m->version[k] == ver[k]));
        if (same && !a[2].enabled && (vt & PSPEXP_3D_VT_NORMAL)) same = !memcmp(m->normal, G.normal, sizeof(m->normal));
        if (same && !a[3].enabled && (vt & PSPEXP_3D_VT_TEXTURE)) same = !memcmp(m->texcoord, G.texcoord, sizeof(m->texcoord));
        if (same) return m;
    }
    n = buffer_vertices();
    if (n < need || n <= 0) return NULL;
    if (n > (int)PSPEXP_3D_MAX_VERTICES + 1) n = PSPEXP_3D_MAX_VERTICES + 1;
    if (n < need) return NULL;
    if (!mesh_room(pspexp_3d_vertex_size(vt) * (uint32_t)n) || !(m = (Mesh *)calloc(1, sizeof(*m)))) return NULL;
    memcpy(m->a, a, sizeof(m->a)); memcpy(m->version, ver, sizeof(ver));
    m->vt = vt; m->count = n;
    memcpy(m->normal, G.normal, sizeof(m->normal)); memcpy(m->texcoord, G.texcoord, sizeof(m->texcoord));
    m->handle = mesh_handle();
    m->bytes = pspexp_3d_vertex_size(vt) * (uint32_t)n;
    if (!(out = (uint8_t *)pspexp_upload_mesh_begin(ctx, m->handle, PSPEXP_MESH_VERTICES, vt, (uint32_t)n))) {
        free_handles[nfree_handles++] = m->handle; free(m); return NULL;
    }
    m->pos = (float *)malloc((size_t)n * 3 * sizeof(float));
    for (i = 0; i < n; i++) { Vtx v; fetch(i, &v); pack_vertex(&out, vt, &v); if (m->pos) memcpy(m->pos + i * 3, v.p, 12); }
    pspexp_upload_mesh_commit(ctx);
    mesh_bytes += m->bytes;
    m->next = meshes; meshes = m;
    if (G.log) gl_logf("mesh %u: %d vertices of buffer %u (%u KB, %u KB kept)", m->handle, n, a[0].buffer, m->bytes / 1024, mesh_bytes / 1024);
    return m;
}

/* A streamed draw's vertices by content: drawn once, remembered (hash);
 * drawn again, made a mesh on the PSP; from then on 16 bytes a frame. For
 * geometry a program rebuilds in client memory every frame unchanged. */
static int content_mesh(PspExpContext *ctx, uint8_t prim, uint32_t vt, const uint8_t *data, int n)
{
    uint64_t h = 1469598103934665603ull;   /* FNV-1a */
    size_t i, bytes = (size_t)pspexp_3d_vertex_size(vt) * (size_t)n;
    Mesh *m;
    uint8_t *out;
    if (getenv("PSPEXP_GL_NOMESH")) return 0;
    for (i = 0; i < bytes; i += 8) { uint64_t w; memcpy(&w, data + i, bytes - i >= 8 ? 8 : bytes - i); h = (h ^ w) * 1099511628211ull; }
    h ^= (uint64_t)vt << 32 ^ (uint64_t)n;
    if (!h) h = 1;
    for (m = meshes; m; m = m->next) if (m->hash == h && m->vt == vt && m->count == n) break;
    if (!m) {   /* first seen: remembered, sent as it is */
        if (!(m = (Mesh *)calloc(1, sizeof(*m)))) return 0;
        m->hash = h; m->vt = vt; m->count = n; m->seen = 1; m->used = mesh_frame;
        m->next = meshes; meshes = m;
        return 0;
    }
    m->used = mesh_frame;
    if (!m->handle) {
        if (++m->seen < 2 || !mesh_room((uint32_t)bytes)) return 0;
        m->handle = mesh_handle();
        m->bytes = (uint32_t)bytes;
        if (!(out = (uint8_t *)pspexp_upload_mesh_begin(ctx, m->handle, PSPEXP_MESH_VERTICES, vt, (uint32_t)n))) {
            free_handles[nfree_handles < 4096 ? nfree_handles++ : 4095] = m->handle; m->handle = 0; m->bytes = 0;
            return 0;
        }
        memcpy(out, data, bytes);
        pspexp_upload_mesh_commit(ctx);
        mesh_bytes += m->bytes;
        if (G.log) gl_logf("mesh %u: %d vertices drawn again unchanged (%u KB)", m->handle, n, m->bytes / 1024);
    }
    pspexp_3d_draw_mesh(ctx, m->handle, PSPEXP_MESH_NONE, prim, 0, (uint32_t)n);
    return 1;
}

/* GL primitive -> GE one, and the indices to draw it with (quads as
 * triangles, a loop closed). n_out: how many. */
static int convert_indices(GLenum mode, const int *idx, int n, uint16_t *out, uint8_t *prim)
{
    int i, k = 0;
    switch (mode) {
    case GL_POINTS: *prim = GU_POINTS; break;
    case GL_LINES: *prim = GU_LINES; n -= n % 2; break;
    case GL_LINE_STRIP: case GL_LINE_LOOP: *prim = GU_LINE_STRIP; break;
    case GL_TRIANGLES: *prim = GU_TRIANGLES; n -= n % 3; break;
    case GL_TRIANGLE_STRIP: case GL_QUAD_STRIP: *prim = GU_TRIANGLE_STRIP; break;
    case GL_TRIANGLE_FAN: case GL_POLYGON: *prim = GU_TRIANGLE_FAN; break;
    case GL_QUADS:
        *prim = GU_TRIANGLES;
        for (i = 0; i + 3 < n; i += 4) {
            out[k++] = (uint16_t)idx[i]; out[k++] = (uint16_t)idx[i + 1]; out[k++] = (uint16_t)idx[i + 2];
            out[k++] = (uint16_t)idx[i]; out[k++] = (uint16_t)idx[i + 2]; out[k++] = (uint16_t)idx[i + 3];
        }
        return k;
    default: return 0;
    }
    for (i = 0; i < n; i++) out[k++] = (uint16_t)idx[i];
    if (mode == GL_LINE_LOOP && n > 1) out[k++] = (uint16_t)idx[0];
    return k;
}

static int converted_max(GLenum mode, int n) { return mode == GL_QUADS ? n / 4 * 6 : n + 1; }

/* The mesh path; 0 when the draw does not qualify (it is then streamed). */
static int mesh_emit(PspExpContext *ctx, Mesh *m, GLenum mode, const int *idx, int first, int count,
                     GLuint ibo, uintptr_t ioffset, GLenum itype, uint32_t vt);

static int draw_mesh(GLenum mode, const int *idx, int first, int count, GLuint ibo, uintptr_t ioffset, GLenum itype)
{
    PspExpContext *ctx;
    GlTex *tex;
    Mesh *m;
    uint32_t vt = PSPEXP_3D_VT_POSITION;
    int k, hi = 0;
    const Array *arr[4] = { &G.va, &G.ca, &G.na, &G.ta };
    GlBuf *vb;
#define NOT_MESH(why) do { if (G.log >= 2) gl_logf("streamed: %s", why); return 0; } while (0)
    if (G.compiling || getenv("PSPEXP_GL_NOMESH")) return 0;
    if (!G.va.enabled || !G.va.buffer || !(vb = find_buffer(G.va.buffer))) return 0;
    if (vb->usage == 0x88E0 /* GL_STREAM_DRAW */) NOT_MESH("a stream buffer");
    for (k = 0; k < 4; k++) if (arr[k]->enabled && !arr[k]->buffer) NOT_MESH("an array in client memory");   /* may change unseen */
    if (G.texture_2d && (G.texgen[0] || G.texgen[1])) {
        float ge[16];
        if (texcoord_mode(ge) == TC_HOST) NOT_MESH("texture coordinates made per vertex");
    }
    if (G.cull && G.cull_mode == GL_FRONT_AND_BACK && mode >= GL_TRIANGLES) return 1;
    if (idx) { for (k = 0; k < count; k++) if (idx[k] > hi) hi = idx[k]; }
    else hi = first + count - 1;
    if (hi > (int)PSPEXP_3D_MAX_VERTICES) return 0;
    if (!(ctx = shim_gl_frame())) return 1;   /* no PSP: nothing to draw anyway */
    {
        float c[4];
        memcpy(c, G.color, sizeof(c));
        if (!(ctx = prepare_colour(&tex, G.ca.enabled ? NULL : c))) return 1;
    }
    if (tex && !(G.tc == TC_GE && G.texgen[0]) && G.tc != TC_ENV) vt |= PSPEXP_3D_VT_TEXTURE;
    if (G.ca.enabled) vt |= PSPEXP_3D_VT_COLOR;
    if (G.lighting || G.tc == TC_ENV) vt |= PSPEXP_3D_VT_NORMAL;
    if (!(m = vertex_mesh(ctx, vt, hi + 1))) NOT_MESH("no vertex mesh");
    return mesh_emit(ctx, m, mode, idx, first, count, ibo, ioffset, itype, vt);
}

/* A mesh drawn: cut where it reaches outside the GE's range, else by
 * index (a buffer's indices as an index mesh too) or in order. */
static int mesh_emit(PspExpContext *ctx, Mesh *m, GLenum mode, const int *idx, int first, int count,
                     GLuint ibo, uintptr_t ioffset, GLenum itype, uint32_t vt)
{
    int k;
    m->used = mesh_frame;
    if (m->pos && mode >= GL_TRIANGLES && !getenv("PSPEXP_GL_NOCLIP")) {   /* where its vertices fall, this frame */
        Clip c;
        clip_setup(&c);
        if (!m->code) m->code = (unsigned *)malloc((size_t)m->count * sizeof(unsigned));
        if (m->code && (m->code_frame != mesh_frame || memcmp(m->code_m, c.m, sizeof(c.m)))) {
            int v;
            m->code_any = 0;
            for (v = 0; v < m->count; v++) { float o[4]; clip_xform(&c, m->pos + v * 3, o); m->code[v] = outcode(&c, o); m->code_any |= m->code[v] != 0; }
            m->code_frame = mesh_frame;
            memcpy(m->code_m, c.m, sizeof(c.m));
        }
        if (m->code && m->code_any && mesh_clipped(ctx, m, &c, mode, idx, first, count, vt)) return 1;
    }
    if (ibo && idx) {   /* the indices from a buffer: an index mesh too */
        IndexMesh *im;
        for (im = index_meshes; im; im = im->next)
            if (im->buffer == ibo && im->version == buffer_version(ibo) && im->offset == ioffset && im->type == itype &&
                im->count == count && im->mode == mode) break;
        if (!im && (im = (IndexMesh *)calloc(1, sizeof(*im)))) {
            uint16_t *tmp = (uint16_t *)malloc((size_t)converted_max(mode, count) * 2u), *out;
            im->buffer = ibo; im->version = buffer_version(ibo); im->offset = ioffset; im->type = itype; im->count = count; im->mode = mode;
            im->n = tmp ? convert_indices(mode, idx, count, tmp, &im->prim) : 0;
            im->bytes = (uint32_t)im->n * 2u;
            if (im->n <= 0 || !mesh_room(im->bytes) ||
                !(out = (uint16_t *)pspexp_upload_mesh_begin(ctx, (im->handle = mesh_handle()), PSPEXP_MESH_INDICES16, 0, (uint32_t)im->n))) {
                free(tmp); free(im); im = NULL;
            } else {
                memcpy(out, tmp, im->bytes);
                pspexp_upload_mesh_commit(ctx);
                free(tmp);
                mesh_bytes += im->bytes;
                im->next = index_meshes; index_meshes = im;
            }
        }
        if (im) {
            im->used = mesh_frame;
            for (k = 0; k < im->n; k += 65535) {   /* the GE's count: 16 bits */
                int c = im->n - k < 65535 ? im->n - k : 65535;
                if (im->prim >= GU_TRIANGLES) c -= im->prim == GU_TRIANGLES ? c % 3 : 0;
                pspexp_3d_draw_mesh(ctx, m->handle, im->handle, im->prim, (uint32_t)k, (uint32_t)c);
                if (im->prim == GU_TRIANGLE_STRIP || im->prim == GU_TRIANGLE_FAN || im->prim == GU_LINE_STRIP) break;
            }
            G.draws++; G.vertices += (unsigned)im->n;
            return 1;
        }
    }
    if (idx || mode == GL_QUADS || mode == GL_LINE_LOOP) {   /* indices in the frame */
        int *seq = NULL, n;
        uint16_t *tmp, *out;
        uint8_t prim;
        if (!idx) {
            if (!(seq = (int *)malloc((size_t)count * sizeof(int)))) return 0;
            for (k = 0; k < count; k++) seq[k] = first + k;
            idx = seq;
        }
        if (!(tmp = (uint16_t *)malloc((size_t)converted_max(mode, count) * 2u))) { free(seq); return 0; }
        n = convert_indices(mode, idx, count, tmp, &prim);
        if (n > 0 && n <= (int)PSPEXP_3D_MAX_VERTICES && (out = pspexp_3d_draw_mesh(ctx, m->handle, PSPEXP_MESH_INLINE, prim, 0, (uint32_t)n)))
            memcpy(out, tmp, (size_t)n * 2u);
        free(tmp); free(seq);
        G.draws++; G.vertices += (unsigned)n;
        return 1;
    }
    {
        uint8_t prim;
        uint16_t dummy;
        int one = 0;
        convert_indices(mode, &one, 0, &dummy, &prim);
        pspexp_3d_draw_mesh(ctx, m->handle, PSPEXP_MESH_NONE, prim, (uint32_t)first, (uint32_t)count);
        G.draws++; G.vertices += (unsigned)count;
    }
    return 1;
}

/* A shader's draw as a mesh: its position a matrix times one attribute (the
 * GE's projection), everything it reads in buffers or constant. The
 * vertices are worked out through the plan once and kept on the PSP;
 * the key is the plan's per-vertex work and the arrays it read. openMrac's
 * track: every vertex every frame before. 0: not such a draw. */
static unsigned vt_hint(const GlslPlan *pl) { return (unsigned)pl->textured; }

/* Draws whose meshes would be made again frame after frame (a uniform in
 * their per-vertex work that moves, a buffer rewritten each frame) are
 * streamed instead for a while: making and sending a mesh a frame costs
 * more than the vertices (openMrac: 684 meshes of one draw in 40 s). */
typedef struct Churn { uint64_t sig; unsigned last, streak, until; } Churn;
static Churn churn[4096];

static Churn *churn_slot(uint64_t sig)
{
    Churn *c = &churn[sig & 4095];
    int i;
    for (i = 0; i < 8 && c->sig && c->sig != sig; i++) c = &churn[(sig + (uint64_t)i + 1) & 4095];   /* a few probes */
    if (c->sig != sig) { memset(c, 0, sizeof(*c)); c->sig = sig; }
    return c;
}

/* A shader draw's vertices through the plan, the whole buffer, as a mesh on the PSP; the texture
 * coordinates of the set `set` (glsl_plan_uv). NULL: no room or no link. */
static Mesh *shader_mesh_make(GlProgram *pr, const GlslPlan *pl, int set, uint64_t key, uint32_t vt, int n, unsigned used, PspExpContext *ctx)
{
    uint8_t *out;
    float attrs[GLSL_ATTR_ALL][4];
    int i, a, nb = 0;
    Mesh *m;
    if (!mesh_room(pspexp_3d_vertex_size(vt) * (uint32_t)n) || !(m = (Mesh *)calloc(1, sizeof(*m)))) return NULL;
    m->shkey = key; m->vt = vt; m->count = n;
    m->handle = mesh_handle();
    m->bytes = pspexp_3d_vertex_size(vt) * (uint32_t)n;
    m->pos = (float *)malloc((size_t)n * 3 * sizeof(float));
    m->vx = (Vtx *)malloc((size_t)n * sizeof(Vtx));
    for (a = 0; a < 16 && nb < 8; a++) if ((used & (1u << a)) && G.gattr[a].enabled) m->bufs[nb++] = G.gattr[a].buffer;
    m->nbufs = nb;
    if (!m->pos || !m->vx ||
        !(out = (uint8_t *)pspexp_upload_mesh_begin(ctx, m->handle, PSPEXP_MESH_VERTICES, vt, (uint32_t)n))) {
        free_handles[nfree_handles < 4096 ? nfree_handles++ : 4095] = m->handle;
        mesh_free(m);
        return NULL;
    }
    for (a = 0; a < 16; a++) memcpy(attrs[a], G.gcur[a], 16);
    for (i = 0; i < n; i++) {
        float pos[4], uv[2], c[4];
        Vtx *v = &m->vx[i];
        for (a = 0; a < 16; a++) {
            const Array *ar = &G.gattr[a];
            const uint8_t *b;
            int cmp, ts;
            if (!(used & (1u << a)) || !ar->enabled || !(b = array_base(ar))) continue;
            ts = type_size(ar->type);
            b += (size_t)i * (size_t)(ar->stride ? ar->stride : ar->size * ts);
            for (cmp = 0; cmp < 4; cmp++) attrs[a][cmp] = cmp < ar->size ? read_comp(b + cmp * ts, ar->type, G.gnorm[a]) : cmp == 3 ? 1.0f : 0.0f;
            if (ar->bgra) { float t = attrs[a][0]; attrs[a][0] = attrs[a][2]; attrs[a][2] = t; }
        }
        glsl_plan_vertex(pl, (const float (*)[4])attrs, pos, uv, c);
        if (set) glsl_plan_uv(pl, set, (const float (*)[4])attrs, uv);
        v->p[0] = pos[0]; v->p[1] = pos[1]; v->p[2] = pos[2];
        v->t[0] = uv[0]; v->t[1] = uv[1];
        v->c = abgr(c);
        if (pl->lit) memcpy(v->n, attrs[pl->lit_attr], 12);
        else if (pl->env) memcpy(v->n, attrs[pl->env_attr], 12);
        else { v->n[0] = v->n[1] = 0; v->n[2] = 1; }
        memcpy(m->pos + i * 3, v->p, 12);
        pack_vertex(&out, vt, v);
    }
    pspexp_upload_mesh_commit(ctx);
    mesh_bytes += m->bytes;
    m->next = meshes; meshes = m;
    if (G.log) gl_logf("mesh %u: program %u, %d vertices, coordinates %d (%u KB, %u KB kept)", m->handle, pr->name, n, set, m->bytes / 1024, mesh_bytes / 1024);
    if (G.log >= 3) {
        int q, off = 0;
        char vers[160];
        for (q = 0; q < 16 && off < 150; q++)
            if ((used & (1u << q)) && G.gattr[q].enabled) off += snprintf(vers + off, sizeof(vers) - (size_t)off, " a%d:b%u v%u", q, G.gattr[q].buffer, buffer_version(G.gattr[q].buffer));
        gl_logf("  key: plan %016llx%s", (unsigned long long)pl->vertex_hash, vers);
    }
    return m;
}

static int shader_mesh(GlProgram *pr, GLenum mode, const int *idx, int first, int count, GLuint ibo, uintptr_t ioffset, GLenum itype)
{
    GlslPlan *pl = &pr->plan;
    uint64_t sig = (uint64_t)pr->name * 2654435761u;
    Churn *ch;
    PspExpContext *ctx;
    GlTex *tex;
    Mesh *m;
    uint64_t h;
    unsigned used = pl->attribs_used;
    uint32_t vt = PSPEXP_3D_VT_POSITION | PSPEXP_3D_VT_COLOR;
    int a, k, hi = 0, n = 1 << 30, any = 0, pass;
    if (G.compiling || getenv("PSPEXP_GL_NOMESH") || !pl->matrix_ok || !pl->vertex_hash || (used >> 16)) return 0;
    if (pl->pos_n > 1) return 0;   /* poses blended: other weights every frame (the sum is made per vertex) */
    if (count < 192) return 0;   /* small draws (text, 2D: often from buffers made each frame) are cheaper streamed */
    h = pl->vertex_hash;
    for (a = 0; a < 16; a++) {
        const Array *ar = &G.gattr[a];
        if (!(used & (1u << a))) continue;
        if (ar->enabled) {
            GlBuf *b;
            int size = ar->size, elem = size * type_size(ar->type), stride = ar->stride ? ar->stride : elem, c;
            if (!ar->buffer || !(b = find_buffer(ar->buffer)) || !b->data || b->usage == 0x88E0 /* GL_STREAM_DRAW */) return 0;
            if (elem <= 0 || (uintptr_t)ar->ptr + (size_t)elem > b->size) return 0;
            c = (int)((b->size - (uintptr_t)ar->ptr - (size_t)elem) / (size_t)stride) + 1;
            if (c < n) n = c;
            h = (h ^ ((uint64_t)ar->buffer << 32 | b->version)) * 1099511628211ull;
            h = (h ^ ((uint64_t)(uintptr_t)ar->ptr << 16 ^ (uint64_t)stride << 8 ^ (uint64_t)ar->type ^ (uint64_t)size << 40 ^ (uint64_t)G.gnorm[a] << 48 ^ (uint64_t)ar->bgra << 49)) * 1099511628211ull;
            h = (h ^ (uint64_t)a) * 1099511628211ull;
            sig = (sig ^ ((uint64_t)ar->buffer << 20 ^ (uint64_t)(uintptr_t)ar->ptr ^ (uint64_t)a << 56)) * 1099511628211ull;
            any = 1;
        } else {
            uint64_t w[2];
            memcpy(w, G.gcur[a], 16);
            h = (h ^ w[0] ^ (uint64_t)a << 56) * 1099511628211ull;
            h = (h ^ w[1]) * 1099511628211ull;
        }
    }
    if (!any || n == 1 << 30) return 0;
    sig ^= (uint64_t)vt_hint(pl) << 60;
    ch = churn_slot(sig ? sig : 1);
    if (ch->until > mesh_frame) return 0;   /* changing too often: streamed for now */
    if (n > (int)PSPEXP_3D_MAX_VERTICES + 1) n = PSPEXP_3D_MAX_VERTICES + 1;
    if (idx) { for (k = 0; k < count; k++) if (idx[k] > hi) hi = idx[k]; }
    else hi = first + count - 1;
    if (hi >= n) return 0;
    if (G.cull && G.cull_mode == GL_FRONT_AND_BACK && mode >= GL_TRIANGLES) return 1;
    if ((pl->textured && G.units[pl->sampler_unit & 7]) || pl->pre_test || pl->mod_tex || pl->add_tex) vt |= PSPEXP_3D_VT_TEXTURE;
    if (pl->lit) vt = (vt & ~(uint32_t)PSPEXP_3D_VT_COLOR) | PSPEXP_3D_VT_NORMAL;
    if (pl->env) vt |= PSPEXP_3D_VT_NORMAL;
    h ^= (uint64_t)vt << 20;
    if (!h) h = 1;
    {   /* a mesh for each set of texture coordinates the passes use: found or made first, so a draw
         * that would make them frame after frame is still streamed whole */
        Mesh *mm[3] = { NULL, NULL, NULL };
        int set, need[3] = { 1, pl->mod_tex, pl->add_tex }, missing = 0;
        uint64_t hs[3];
        for (set = 0; set < 3; set++) {
            if (!need[set]) continue;
            hs[set] = set ? (h ^ pl->uv_hash[set] * 0x9E3779B97F4A7C15ull ^ (uint64_t)set) : h;
            if (!hs[set]) hs[set] = 1;
            for (m = meshes; m; m = m->next) if (m->shkey == hs[set] && m->vt == vt && m->count >= hi + 1) break;
            if (!(mm[set] = m)) missing = 1;
        }
        if (missing) {   /* made for this draw: a third frame running, and it is streamed for 5 s */
            ch->streak = ch->last + 2 >= mesh_frame && ch->last ? ch->streak + 1 : 1;
            ch->last = mesh_frame;
            if (ch->streak >= 2) {
                ch->until = mesh_frame + 300;
                ch->streak = 0;
                if (G.log) gl_logf("program %u: a draw that changes every frame: streamed", pr->name);
                return 0;
            }
            if (!(ctx = shim_gl_frame())) return 1;
            for (set = 0; set < 3; set++)
                if (need[set] && !mm[set] && !(mm[set] = shader_mesh_make(pr, pl, set, hs[set], vt, n, used, ctx))) return 0;
        }
        memcpy(G.sh_proj, pl->matrix, sizeof(G.sh_proj));
        k = 0;
        for (pass = 0; pass < SH_PASSES; pass++) {
            if (!sh_pass(pl, pass) || !(ctx = prepare_colour(&tex, NULL))) continue;
            if (mesh_emit(ctx, mm[G.sh_uvset], mode, idx, first, count, ibo, ioffset, itype, vt)) k = 1;
            else if (!k) break;   /* (not drawn from the mesh: streamed instead, all of it) */
        }
        sh_pass_end();
    }
    return k || pass == SH_PASSES;
}

/* At the end of a frame: the meshes dropped during the one before (whose
 * list the GE has run) are released. */
/* A mesh draw that reaches outside the GE's range: its safe triangles by
 * index from the mesh, the others cut and sent with the frame. */
static int convert_indices(GLenum mode, const int *idx, int n, uint16_t *out, uint8_t *prim);
static int converted_max(GLenum mode, int n);

static int mesh_clipped(PspExpContext *ctx, Mesh *m, const Clip *c, GLenum mode, const int *idx, int first, int count, uint32_t vt)
{
    uint16_t *list, *safe;
    Vtx *out;
    int *seq = NULL, n, nt, i, ns = 0, k = 0;
    uint8_t prim;
    if (!idx) {
        if (!(seq = (int *)malloc((size_t)count * sizeof(int)))) return 0;
        for (i = 0; i < count; i++) seq[i] = first + i;
        idx = seq;
    }
    list = (uint16_t *)malloc((size_t)converted_max(mode, count) * 2u);
    n = list ? convert_indices(mode, idx, count, list, &prim) : 0;
    free(seq);
    if (n <= 0 || prim < GU_TRIANGLES) { free(list); return 0; }
    nt = triangle_count(prim, n);
    safe = (uint16_t *)malloc((size_t)nt * 3 * 2u + 2);
    out = (Vtx *)malloc((size_t)nt * 21 * sizeof(Vtx) + 1);
    if (!safe || !out) { free(list); free(safe); free(out); return 0; }
    for (i = 0; i < nt; i++) {
        int a, b, d;
        unsigned ca, cb, cd;
        triangle_at(prim, i, &a, &b, &d);
        a = list[a]; b = list[b]; d = list[d];
        ca = m->code[a]; cb = m->code[b]; cd = m->code[d];
        if (!(ca | cb | cd)) { safe[ns++] = (uint16_t)a; safe[ns++] = (uint16_t)b; safe[ns++] = (uint16_t)d; }
        else if (!(ca & cb & cd)) {
            Vtx va, vb, vd;
            if (m->vx) { va = m->vx[a]; vb = m->vx[b]; vd = m->vx[d]; }
            else { fetch(a, &va); fetch(b, &vb); fetch(d, &vd); }
            k += clip_triangle(c, &va, &vb, &vd, out + k);
        }
    }
    for (i = 0; i < ns; i += 65535 / 3 * 3) {
        int cnt = ns - i < 65535 / 3 * 3 ? ns - i : 65535 / 3 * 3;
        uint16_t *dst = pspexp_3d_draw_mesh(ctx, m->handle, PSPEXP_MESH_INLINE, GU_TRIANGLES, 0, (uint32_t)cnt);
        if (dst) memcpy(dst, safe + i, (size_t)cnt * 2u);
    }
    for (i = 0; i < k; i += CHUNK_VERTICES / 3 * 3)
        send_prim(ctx, GU_TRIANGLES, vt, out + i, k - i < CHUNK_VERTICES / 3 * 3 ? k - i : CHUNK_VERTICES / 3 * 3);
    G.draws++; G.vertices += (unsigned)(ns + k);
    free(list); free(safe); free(out);
    return 1;
}

static void meshes_frame_done(PspExpContext *ctx)
{
    int i, old = rel_cur ^ 1;
    if (ctx) for (i = 0; i < nreleases[old]; i++) {
        pspexp_release(ctx, releases[old][i]);
        if (nfree_handles < (int)(sizeof(free_handles) / sizeof(free_handles[0]))) free_handles[nfree_handles++] = releases[old][i];
    }
    nreleases[old] = 0;
    rel_cur = old;
    env_frame++;
    {   /* remembered contents not seen again for a few frames: forgotten */
        Mesh **pp = &meshes;
        while (*pp) {
            Mesh *m = *pp;
            if (m->hash && !m->handle && m->used + 3 < mesh_frame) { *pp = m->next; mesh_free(m); }
            else pp = &m->next;
        }
    }
    mesh_frame++;
}

GLAPI(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))
{
    Vtx *v;
    int i;
    ensure_init();
    if (count <= 0) return;
    if (G.program) { shader_draw(mode, NULL, first, count, 0, 0, 0); return; }
    if (G.compiling && !replaying) { compile_arrays(mode, NULL, first, count); return; }
    if (draw_mesh(mode, NULL, first, count, 0, 0, 0)) return;
    if (!(v = gather(count))) { set_error(GL_OUT_OF_MEMORY); return; }
    for (i = 0; i < count; i++) fetch(first + i, &v[i]);
    draw_vertices(mode, v, count);
}

static int *read_indices(GLsizei count, GLenum type, const GLvoid *indices)
{
    static int *idx;
    static int cap;
    const uint8_t *p = (const uint8_t *)indices;
    int i;
    if (G.element_buffer) { GlBuf *b = find_buffer(G.element_buffer); if (!b || !b->data) return NULL; p = b->data + (uintptr_t)indices; }
    if (!p) return NULL;
    if (count > cap) { int *n = (int *)realloc(idx, (size_t)count * sizeof(int)); if (!n) return NULL; idx = n; cap = count; }
    for (i = 0; i < count; i++)
        idx[i] = type == GL_UNSIGNED_BYTE ? p[i] : type == GL_UNSIGNED_SHORT ? ((const uint16_t *)p)[i] : (int)((const uint32_t *)p)[i];
    return idx;
}

GLAPI(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const GLvoid *indices))
{
    Vtx *v;
    int *idx, i;
    ensure_init();
    if (count <= 0) return;
    if (!(idx = read_indices(count, type, indices))) { set_error(GL_INVALID_OPERATION); return; }
    if (G.program) { shader_draw(mode, idx, 0, count, G.element_buffer, (uintptr_t)indices, type); return; }
    if (G.compiling && !replaying) { compile_arrays(mode, idx, 0, count); return; }
    if (draw_mesh(mode, idx, 0, count, G.element_buffer, (uintptr_t)indices, type)) return;
    if (!(v = gather(count))) { set_error(GL_OUT_OF_MEMORY); return; }
    for (i = 0; i < count; i++) fetch(idx[i], &v[i]);
    draw_vertices(mode, v, count);
}
GLAPI(void, glDrawRangeElements, (GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid *indices))
{
    (void)start; (void)end;
    GLFN(glDrawElements)(mode, count, type, indices);
}
GLAPI(void, glMultiDrawArrays, (GLenum mode, const GLint *first, const GLsizei *count, GLsizei n))
{
    int i;
    for (i = 0; i < n; i++) GLFN(glDrawArrays)(mode, first[i], count[i]);
}

GLAPI(void, glInterleavedArrays, (GLenum format, GLsizei stride, const GLvoid *pointer))
{
    const uint8_t *p = (const uint8_t *)pointer;
    int tc = 0, cc = 0, nn = 0, vc = 3, off = 0, str;
    GLenum ctype = GL_FLOAT;
    switch (format) {
    case GL_V2F: vc = 2; break;
    case GL_V3F: break;
    case GL_C4UB_V2F: cc = 4; ctype = GL_UNSIGNED_BYTE; vc = 2; break;
    case GL_C4UB_V3F: cc = 4; ctype = GL_UNSIGNED_BYTE; break;
    case GL_C3F_V3F: cc = 3; break;
    case GL_N3F_V3F: nn = 1; break;
    case GL_C4F_N3F_V3F: cc = 4; nn = 1; break;
    case GL_T2F_V3F: tc = 2; break;
    case GL_T4F_V4F: tc = 4; vc = 4; break;
    case GL_T2F_C4UB_V3F: tc = 2; cc = 4; ctype = GL_UNSIGNED_BYTE; break;
    case GL_T2F_C3F_V3F: tc = 2; cc = 3; break;
    case GL_T2F_N3F_V3F: tc = 2; nn = 1; break;
    case GL_T2F_C4F_N3F_V3F: tc = 2; cc = 4; nn = 1; break;
    case GL_T4F_C4F_N3F_V4F: tc = 4; cc = 4; nn = 1; vc = 4; break;
    default: set_error(GL_INVALID_ENUM); return;
    }
    str = tc * 4 + (cc ? (ctype == GL_UNSIGNED_BYTE ? 4 : cc * 4) : 0) + nn * 12 + vc * 4;
    if (stride) str = stride;
    G.ta.enabled = tc != 0; G.ca.enabled = cc != 0; G.na.enabled = nn; G.va.enabled = 1;
    if (tc) { set_array(&G.ta, tc, GL_FLOAT, str, p); off += tc * 4; }
    if (cc) { set_array(&G.ca, cc, ctype, str, p + off); off += ctype == GL_UNSIGNED_BYTE ? 4 : cc * 4; }
    if (nn) { set_array(&G.na, 3, GL_FLOAT, str, p + off); off += 12; }
    set_array(&G.va, vc, GL_FLOAT, str, p + off);
}

GLEXT(void, glLockArraysEXT, (GLint first, GLsizei count)) { (void)first; (void)count; }
GLEXT(void, glUnlockArraysEXT, (void)) { }

GLAPI(void, glGenBuffers, (GLsizei n, GLuint *out))
{
    int i;
    ensure_init();
    for (i = 0; i < n; i++) {
        GlBuf *b = (GlBuf *)calloc(1, sizeof(*b));
        if (!b) { set_error(GL_OUT_OF_MEMORY); return; }
        b->name = G.next_buffer++;
        b->next = G.buffers; G.buffers = b;
        out[i] = b->name;
    }
}
GLAPI(void, glDeleteBuffers, (GLsizei n, const GLuint *names))
{
    int i;
    for (i = 0; i < n; i++) {
        GlBuf **pp = &G.buffers;
        while (*pp && (*pp)->name != names[i]) pp = &(*pp)->next;
        if (*pp) { GlBuf *b = *pp; *pp = b->next; if (buf_hash[b->name & 1023] == b) buf_hash[b->name & 1023] = NULL; free(b->data); free(b); meshes_forget_buffer(names[i]); }
        if (G.array_buffer == names[i]) G.array_buffer = 0;
        if (G.element_buffer == names[i]) G.element_buffer = 0;
    }
}
GLAPI(void, glBindBuffer, (GLenum target, GLuint name))
{
    ensure_init();
    if (name && !find_buffer(name)) {   /* a name never generated: made now, as GL does */
        GlBuf *b = (GlBuf *)calloc(1, sizeof(*b));
        if (!b) { set_error(GL_OUT_OF_MEMORY); return; }
        b->name = name; b->next = G.buffers; G.buffers = b;
        if (name >= G.next_buffer) G.next_buffer = name + 1;
    }
    if (target == GL_ARRAY_BUFFER) G.array_buffer = name;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) G.element_buffer = name;
}
static GlBuf *bound_buffer(GLenum target)
{
    return find_buffer(target == GL_ARRAY_BUFFER ? G.array_buffer : target == GL_ELEMENT_ARRAY_BUFFER ? G.element_buffer : 0);
}
GLAPI(void, glBufferData, (GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage))
{
    GlBuf *b = bound_buffer(target);
    uint8_t *p;
    if (!b) { set_error(GL_INVALID_OPERATION); return; }
    if (!(p = (uint8_t *)realloc(b->data, size ? (size_t)size : 1))) { set_error(GL_OUT_OF_MEMORY); return; }
    b->data = p; b->size = (size_t)size; b->usage = usage;
    if (data) memcpy(p, data, (size_t)size); else memset(p, 0, (size_t)size);
    b->version++;
    meshes_forget_buffer(b->name);
}
GLAPI(void, glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data))
{
    GlBuf *b = bound_buffer(target);
    if (!b || offset < 0 || (size_t)(offset + size) > b->size) { set_error(GL_INVALID_VALUE); return; }
    memcpy(b->data + offset, data, (size_t)size);
    b->version++;
    meshes_forget_buffer(b->name);
}
GLAPI(void *, glMapBuffer, (GLenum target, GLenum access))
{
    GlBuf *b = bound_buffer(target);
    (void)access;
    if (!b) { set_error(GL_INVALID_OPERATION); return NULL; }
    b->mapped = 1;
    return b->data;
}
GLAPI(GLboolean, glUnmapBuffer, (GLenum target)) { GlBuf *b = bound_buffer(target); if (b) { b->mapped = 0; b->version++; meshes_forget_buffer(b->name); } return GL_TRUE; }
GLAPI(GLboolean, glIsBuffer, (GLuint name)) { return find_buffer(name) != NULL; }
GLAPI(void, glGetBufferParameteriv, (GLenum target, GLenum pname, GLint *out))
{
    GlBuf *b = bound_buffer(target);
    *out = !b ? 0 : pname == GL_BUFFER_SIZE ? (GLint)b->size : pname == GL_BUFFER_USAGE ? (GLint)b->usage : 0;
}

/* ---- render state ------------------------------------------------------------------------ */

static void set_cap(GLenum cap, int on)
{
    ensure_init();
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + MAX_LIGHTS) { G.light[cap - GL_LIGHT0].enabled = on; G.lights_dirty = 1; return; }
    switch (cap) {
    case GL_DEPTH_TEST: G.depth_test = on; break;
    case GL_CULL_FACE: G.cull = on; break;
    case GL_BLEND: G.blend = on; break;
    case GL_ALPHA_TEST: G.alpha_test = on; break;
    case GL_TEXTURE_2D: if (G.active_unit == 0) G.texture_2d = on; break;
    case GL_FOG: G.fog = on; break;
    case GL_LIGHTING: G.lighting = on; G.lights_dirty = 1; break;
    case GL_COLOR_MATERIAL: G.color_material = on; G.lights_dirty = 1; break;
    case GL_SCISSOR_TEST: G.scissor_test = on; break;
    case GL_DITHER: G.dither = on; break;
    case GL_NORMALIZE: case GL_RESCALE_NORMAL: G.normalize = on; break;   /* the GE normalises anyway */
    case GL_POLYGON_OFFSET_FILL: G.polygon_offset = on; break;
    case GL_LINE_SMOOTH: case GL_POINT_SMOOTH: case GL_POLYGON_SMOOTH: case GL_MULTISAMPLE:
    case GL_LINE_STIPPLE: case GL_POLYGON_STIPPLE: case GL_COLOR_LOGIC_OP: case GL_TEXTURE_1D:
        break;   /* quietly without */
    case GL_STENCIL_TEST:
        if (on) unsupported("stencil test");
        break;
    case GL_TEXTURE_GEN_S: G.texgen[0] = on; break;
    case GL_TEXTURE_GEN_T: G.texgen[1] = on; break;
    case GL_TEXTURE_GEN_R: case GL_TEXTURE_GEN_Q: break;   /* 2D textures: s and t are all that count */
    default:
        if (cap >= GL_CLIP_PLANE0 && cap < GL_CLIP_PLANE0 + 6) { if (on) unsupported("clip planes"); break; }
        if (G.log) gl_logf("glEnable/glDisable 0x%x ignored", cap);
        break;
    }
}

GLAPI(void, glEnable, (GLenum cap)) { RECORD(OP_ENABLE, cap, 0, 0, 0, NULL); set_cap(cap, 1); }
GLAPI(void, glDisable, (GLenum cap)) { RECORD(OP_DISABLE, cap, 0, 0, 0, NULL); set_cap(cap, 0); }
GLAPI(GLboolean, glIsEnabled, (GLenum cap))
{
    ensure_init();
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + MAX_LIGHTS) return (GLboolean)G.light[cap - GL_LIGHT0].enabled;
    switch (cap) {
    case GL_DEPTH_TEST: return (GLboolean)G.depth_test;
    case GL_CULL_FACE: return (GLboolean)G.cull;
    case GL_BLEND: return (GLboolean)G.blend;
    case GL_ALPHA_TEST: return (GLboolean)G.alpha_test;
    case GL_TEXTURE_2D: return (GLboolean)G.texture_2d;
    case GL_FOG: return (GLboolean)G.fog;
    case GL_LIGHTING: return (GLboolean)G.lighting;
    case GL_COLOR_MATERIAL: return (GLboolean)G.color_material;
    case GL_SCISSOR_TEST: return (GLboolean)G.scissor_test;
    case GL_DITHER: return (GLboolean)G.dither;
    case GL_NORMALIZE: return (GLboolean)G.normalize;
    case GL_POLYGON_OFFSET_FILL: return (GLboolean)G.polygon_offset;
    case GL_TEXTURE_GEN_S: return (GLboolean)G.texgen[0];
    case GL_TEXTURE_GEN_T: return (GLboolean)G.texgen[1];
    case GL_VERTEX_ARRAY: return (GLboolean)G.va.enabled;
    case GL_COLOR_ARRAY: return (GLboolean)G.ca.enabled;
    case GL_NORMAL_ARRAY: return (GLboolean)G.na.enabled;
    case GL_TEXTURE_COORD_ARRAY: return (GLboolean)G.ta.enabled;
    default: return GL_FALSE;
    }
}

GLAPI(void, glDepthFunc, (GLenum f)) { RECORD(OP_DEPTH_FUNC, f, 0, 0, 0, NULL); ensure_init(); G.depth_func = f; }
GLAPI(void, glDepthMask, (GLboolean m)) { RECORD(OP_DEPTH_MASK, m, 0, 0, 0, NULL); ensure_init(); G.depth_mask = m != 0; }
GLAPI(void, glDepthRange, (GLclampd n, GLclampd f)) { ensure_init(); G.depth_near = (float)n; G.depth_far = (float)f; }
GLAPI(void, glDepthRangef, (GLclampf n, GLclampf f)) { ensure_init(); G.depth_near = n; G.depth_far = f; }
GLAPI(void, glClearDepth, (GLclampd d)) { float f = (float)d; RECORD(OP_CLEAR_DEPTH, 0, 0, 0, 1, &f); ensure_init(); G.clear_depth = f; }
GLAPI(void, glClearDepthf, (GLclampf d)) { GLFN(glClearDepth)(d); }
GLAPI(void, glBlendFunc, (GLenum s, GLenum d)) { RECORD(OP_BLEND_FUNC, s, d, 0, 0, NULL); ensure_init(); G.blend_src = s; G.blend_dst = d; }
GLAPI(void, glBlendFuncSeparate, (GLenum s, GLenum d, GLenum sa, GLenum da)) { (void)sa; (void)da; GLFN(glBlendFunc)(s, d); }
GLAPI(void, glBlendEquation, (GLenum e)) { RECORD(OP_BLEND_EQUATION, e, 0, 0, 0, NULL); ensure_init(); G.blend_eq = e; }
GLAPI(void, glBlendColor, (GLclampf r, GLclampf g, GLclampf b, GLclampf a)) { ensure_init(); G.blend_color[0] = r; G.blend_color[1] = g; G.blend_color[2] = b; G.blend_color[3] = a; }
GLAPI(void, glAlphaFunc, (GLenum f, GLclampf ref)) { RECORD(OP_ALPHA_FUNC, f, 0, 0, 1, &ref); ensure_init(); G.alpha_func = f; G.alpha_ref = ref; }
GLAPI(void, glCullFace, (GLenum m)) { RECORD(OP_CULL_FACE, m, 0, 0, 0, NULL); ensure_init(); G.cull_mode = m; }
GLAPI(void, glFrontFace, (GLenum m)) { RECORD(OP_FRONT_FACE, m, 0, 0, 0, NULL); ensure_init(); G.front_face = m; }
GLAPI(void, glShadeModel, (GLenum m)) { RECORD(OP_SHADE_MODEL, m, 0, 0, 0, NULL); ensure_init(); G.shade_model = m; }
GLAPI(void, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))
{
    RECORD(OP_COLOR_MASK, (uint32_t)(r != 0) | (uint32_t)(g != 0) << 1 | (uint32_t)(b != 0) << 2 | (uint32_t)(a != 0) << 3, 0, 0, 0, NULL);
    ensure_init();
    G.color_mask[0] = r != 0; G.color_mask[1] = g != 0; G.color_mask[2] = b != 0; G.color_mask[3] = a != 0;
}
GLAPI(void, glClearColor, (GLclampf r, GLclampf g, GLclampf b, GLclampf a))
{
    float f[4] = { r, g, b, a };
    RECORD(OP_CLEAR_COLOR, 0, 0, 0, 4, f);
    ensure_init();
    memcpy(G.clear_color, f, sizeof(f));
}
GLAPI(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))
{
    RECORD(OP_VIEWPORT, (uint32_t)x, (uint32_t)y, (uint32_t)w, 1, (float[]){ (float)h });
    ensure_init();
    G.viewport[0] = x; G.viewport[1] = y; G.viewport[2] = w; G.viewport[3] = h;
}
GLAPI(void, glScissor, (GLint x, GLint y, GLsizei w, GLsizei h))
{
    RECORD(OP_SCISSOR, (uint32_t)x, (uint32_t)y, (uint32_t)w, 1, (float[]){ (float)h });
    ensure_init();
    G.scissor[0] = x; G.scissor[1] = y; G.scissor[2] = w; G.scissor[3] = h;
}
GLAPI(void, glPolygonOffset, (GLfloat f, GLfloat u)) { ensure_init(); G.po_factor = f; G.po_units = u; }
GLAPI(void, glLineWidth, (GLfloat w)) { ensure_init(); G.line_width = w; }
GLAPI(void, glPointSize, (GLfloat s)) { ensure_init(); G.point_size = s; }
GLAPI(void, glPolygonMode, (GLenum face, GLenum mode)) { (void)face; if (mode != GL_FILL) unsupported("polygon modes other than fill"); }
GLAPI(void, glLineStipple, (GLint f, GLushort p)) { (void)f; (void)p; }
GLAPI(void, glPolygonStipple, (const GLubyte *m)) { (void)m; }
GLAPI(void, glHint, (GLenum t, GLenum m)) { (void)t; (void)m; }
GLAPI(void, glLogicOp, (GLenum op)) { (void)op; }
GLAPI(void, glDrawBuffer, (GLenum b)) { (void)b; }
GLAPI(void, glReadBuffer, (GLenum b)) { (void)b; }
GLAPI(void, glFlush, (void)) { }
GLAPI(void, glFinish, (void)) { }
GLAPI(void, glClearStencil, (GLint s)) { (void)s; }
GLAPI(void, glStencilFunc, (GLenum f, GLint r, GLuint m)) { (void)f; (void)r; (void)m; }
GLAPI(void, glStencilOp, (GLenum a, GLenum b, GLenum c)) { (void)a; (void)b; (void)c; }
GLAPI(void, glStencilMask, (GLuint m)) { (void)m; }
GLAPI(void, glClearAccum, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) { (void)r; (void)g; (void)b; (void)a; }
GLAPI(void, glClipPlane, (GLenum p, const GLdouble *eq)) { (void)p; (void)eq; }
GLAPI(void, glPixelStorei, (GLenum p, GLint v))
{
    ensure_init();
    switch (p) {
    case GL_UNPACK_ALIGNMENT: G.unpack_alignment = v; break;
    case GL_UNPACK_ROW_LENGTH: G.unpack_row_length = v; break;
    case GL_UNPACK_SKIP_ROWS: G.unpack_skip_rows = v; break;
    case GL_UNPACK_SKIP_PIXELS: G.unpack_skip_pixels = v; break;
    default: break;
    }
}
GLAPI(void, glPixelStoref, (GLenum p, GLfloat v)) { GLFN(glPixelStorei)(p, (GLint)v); }
GLAPI(void, glPixelTransferf, (GLenum p, GLfloat v)) { (void)p; (void)v; }
GLAPI(void, glPixelTransferi, (GLenum p, GLint v)) { (void)p; (void)v; }
GLAPI(void, glPixelZoom, (GLfloat x, GLfloat y)) { (void)x; (void)y; }
GLAPI(void, glRasterPos2f, (GLfloat x, GLfloat y)) { (void)x; (void)y; }
GLAPI(void, glRasterPos2i, (GLint x, GLint y)) { (void)x; (void)y; }
GLAPI(void, glRasterPos3f, (GLfloat x, GLfloat y, GLfloat z)) { (void)x; (void)y; (void)z; }
GLAPI(void, glBitmap, (GLsizei w, GLsizei h, GLfloat xo, GLfloat yo, GLfloat xm, GLfloat ym, const GLubyte *b))
{ (void)w; (void)h; (void)xo; (void)yo; (void)xm; (void)ym; (void)b; unsupported("glBitmap"); }
GLAPI(void, glDrawPixels, (GLsizei w, GLsizei h, GLenum f, GLenum t, const GLvoid *p))
{ (void)w; (void)h; (void)f; (void)t; (void)p; unsupported("glDrawPixels"); }
GLAPI(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, GLvoid *p))
{
    (void)x; (void)y; (void)f;
    unsupported("glReadPixels (reads black)");
    if (p && w > 0 && h > 0) memset(p, 0, (size_t)w * (size_t)h * (size_t)(t == GL_FLOAT ? 16 : 4));
}

GLAPI(void, glFogf, (GLenum p, GLfloat v))
{
    RECORD(OP_FOG, p, 0, 0, 1, &v);
    ensure_init();
    switch (p) {
    case GL_FOG_MODE: G.fog_mode = (GLenum)v; break;
    case GL_FOG_DENSITY: G.fog_density = v; break;
    case GL_FOG_START: G.fog_start = v; break;
    case GL_FOG_END: G.fog_end = v; break;
    default: break;
    }
}
GLAPI(void, glFogi, (GLenum p, GLint v)) { GLFN(glFogf)(p, (float)v); }
GLAPI(void, glFogfv, (GLenum p, const GLfloat *v))
{
    if (p == GL_FOG_COLOR) {
        RECORD(OP_FOG, p, 0, 0, 4, v);
        ensure_init();
        memcpy(G.fog_color, v, sizeof(G.fog_color));
    } else GLFN(glFogf)(p, v[0]);
}
GLAPI(void, glFogiv, (GLenum p, const GLint *v))
{
    if (p == GL_FOG_COLOR) { float f[4] = { v[0] / 2147483647.0f, v[1] / 2147483647.0f, v[2] / 2147483647.0f, v[3] / 2147483647.0f }; GLFN(glFogfv)(p, f); }
    else GLFN(glFogf)(p, (float)v[0]);
}

/* glClear: GL clears the whole window, or the scissor box. The letterbox
 * bars around the window are cleared black. */
GLAPI(void, glClear, (GLbitfield mask))
{
    PspExpContext *ctx;
    GlTex *tex;
    uint32_t flags = 0;
    RECORD(OP_CLEAR, mask, 0, 0, 0, NULL);
    ensure_init();
    if (mask & GL_COLOR_BUFFER_BIT) flags |= PSPEXP_3D_CLEAR_COLOR;
    if ((mask & GL_DEPTH_BUFFER_BIT) && G.depth_mask) flags |= PSPEXP_3D_CLEAR_DEPTH;
    if (!flags) return;
    {
        int ww, wh, ox, oy; float s;
        shim_gl_geometry(&ww, &wh, &s, &ox, &oy);
        if ((flags & PSPEXP_3D_CLEAR_COLOR) && !G.scissor_test && !G.fbo && (ox > 0 || oy > 0) && (ctx = shim_gl_frame())) {
            PspExp3DState st;
            G.state_sent = 0;
            if (!(ctx = prepare(&tex))) return;
            st = G.sent_state;
            st.scissor_x = 0; st.scissor_y = 0; st.scissor_w = PSPEXP_DISPLAY_WIDTH; st.scissor_h = PSPEXP_DISPLAY_HEIGHT;
            st.pixel_mask = 0;
            pspexp_3d_state(ctx, &st);
            pspexp_3d_clear(ctx, PSPEXP_3D_CLEAR_COLOR, 0xFF000000u, 0);
            G.state_sent = 0;
        }
    }
    G.frame_cleared |= !G.fbo;   /* the program's own: no black one first */
    if (!(ctx = prepare(&tex))) return;
    {
        float c[4];
        int k;
        for (k = 0; k < 4; k++) c[k] = G.clear_color[k];
        pspexp_3d_clear(ctx, flags, abgr(c), (uint16_t)lroundf(65535.0f * (1.0f - (G.clear_depth < 0 ? 0 : G.clear_depth > 1 ? 1 : G.clear_depth))));
    }
    G.clears++;
}

/* ---- lighting ---------------------------------------------------------------------------- */


GLAPI(void, glLightfv, (GLenum light, GLenum p, const GLfloat *v))
{
    Light *l;
    int n = p == GL_POSITION || p == GL_AMBIENT || p == GL_DIFFUSE || p == GL_SPECULAR ? 4 : p == GL_SPOT_DIRECTION ? 3 : 1;
    RECORD(OP_LIGHT, light, p, 0, n, v);
    ensure_init();
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + MAX_LIGHTS) { set_error(GL_INVALID_ENUM); return; }
    l = &G.light[light - GL_LIGHT0];
    G.lights_dirty = 1;
    switch (p) {
    case GL_AMBIENT: memcpy(l->ambient, v, 16); break;
    case GL_DIFFUSE: memcpy(l->diffuse, v, 16); break;
    case GL_SPECULAR: memcpy(l->specular, v, 16); break;
    case GL_POSITION: xform_point(G.mv[G.mv_top], v, l->position); break;   /* eye space, as GL keeps it */
    case GL_SPOT_DIRECTION: {
        float d[4] = { v[0], v[1], v[2], 0 }, o[4];
        xform_point(G.mv[G.mv_top], d, o);
        memcpy(l->spot_dir, o, 12);
        break;
    }
    case GL_SPOT_EXPONENT: l->spot_exp = v[0]; break;
    case GL_SPOT_CUTOFF: l->spot_cutoff = v[0]; break;
    case GL_CONSTANT_ATTENUATION: l->att[0] = v[0]; break;
    case GL_LINEAR_ATTENUATION: l->att[1] = v[0]; break;
    case GL_QUADRATIC_ATTENUATION: l->att[2] = v[0]; break;
    default: set_error(GL_INVALID_ENUM); break;
    }
}
GLAPI(void, glLightf, (GLenum light, GLenum p, GLfloat v)) { GLFN(glLightfv)(light, p, &v); }
GLAPI(void, glLighti, (GLenum light, GLenum p, GLint v)) { float f = (float)v; GLFN(glLightfv)(light, p, &f); }
GLAPI(void, glLightiv, (GLenum light, GLenum p, const GLint *v))
{
    float f[4];
    int i;
    for (i = 0; i < 4; i++) f[i] = p == GL_POSITION || p == GL_SPOT_DIRECTION ? (float)v[i] : v[i] / 2147483647.0f;
    if (p != GL_POSITION && p != GL_SPOT_DIRECTION && p != GL_AMBIENT && p != GL_DIFFUSE && p != GL_SPECULAR) f[0] = (float)v[0];
    GLFN(glLightfv)(light, p, f);
}

GLAPI(void, glLightModelfv, (GLenum p, const GLfloat *v))
{
    RECORD(OP_LIGHT_MODEL, p, 0, 0, p == GL_LIGHT_MODEL_AMBIENT ? 4 : 1, v);
    ensure_init();
    if (p == GL_LIGHT_MODEL_AMBIENT) { memcpy(G.scene_ambient, v, 16); G.lights_dirty = 1; }
}
GLAPI(void, glLightModelf, (GLenum p, GLfloat v)) { GLFN(glLightModelfv)(p, &v); }
GLAPI(void, glLightModeli, (GLenum p, GLint v)) { float f = (float)v; GLFN(glLightModelfv)(p, &f); }
GLAPI(void, glLightModeliv, (GLenum p, const GLint *v))
{
    float f[4] = { v[0] / 2147483647.0f, v[1] / 2147483647.0f, v[2] / 2147483647.0f, v[3] / 2147483647.0f };
    if (p != GL_LIGHT_MODEL_AMBIENT) f[0] = (float)v[0];
    GLFN(glLightModelfv)(p, f);
}

GLAPI(void, glMaterialfv, (GLenum face, GLenum p, const GLfloat *v))
{
    RECORD(OP_MATERIAL, face, p, 0, p == GL_SHININESS ? 1 : 4, v);
    ensure_init();
    if (face == GL_BACK) return;   /* one-sided lighting: the front material */
    G.lights_dirty = 1;
    switch (p) {
    case GL_AMBIENT: memcpy(G.mat.ambient, v, 16); break;
    case GL_DIFFUSE: memcpy(G.mat.diffuse, v, 16); break;
    case GL_SPECULAR: memcpy(G.mat.specular, v, 16); break;
    case GL_EMISSION: memcpy(G.mat.emission, v, 16); break;
    case GL_SHININESS: G.mat.shininess = v[0]; break;
    case GL_AMBIENT_AND_DIFFUSE: memcpy(G.mat.ambient, v, 16); memcpy(G.mat.diffuse, v, 16); break;
    default: break;
    }
}
GLAPI(void, glMaterialf, (GLenum face, GLenum p, GLfloat v)) { GLFN(glMaterialfv)(face, p, &v); }
GLAPI(void, glMateriali, (GLenum face, GLenum p, GLint v)) { float f = (float)v; GLFN(glMaterialfv)(face, p, &f); }
GLAPI(void, glMaterialiv, (GLenum face, GLenum p, const GLint *v))
{
    float f[4] = { v[0] / 2147483647.0f, v[1] / 2147483647.0f, v[2] / 2147483647.0f, v[3] / 2147483647.0f };
    if (p == GL_SHININESS) f[0] = (float)v[0];
    GLFN(glMaterialfv)(face, p, f);
}
GLAPI(void, glColorMaterial, (GLenum face, GLenum mode)) { RECORD(OP_COLOR_MATERIAL, face, mode, 0, 0, NULL); ensure_init(); G.cm_face = face; G.cm_mode = mode; G.lights_dirty = 1; }

/* ---- textures ---------------------------------------------------------------------------- */

GLAPI(void, glGenTextures, (GLsizei n, GLuint *out))
{
    int i;
    ensure_init();
    for (i = 0; i < n; i++) {
        while (find_texture(G.next_texture)) G.next_texture++;
        out[i] = G.next_texture++;
    }
}

static GlTex *texture_object(GLuint name)
{
    GlTex *t = find_texture(name);
    if (t || !name) return t;
    if (!(t = (GlTex *)calloc(1, sizeof(*t)))) return NULL;
    t->name = name;
    t->min_filter = GL_NEAREST_MIPMAP_LINEAR; t->mag_filter = GL_LINEAR;
    t->wrap_s = t->wrap_t = GL_REPEAT;
    t->prescale = t->scale = 1;
    t->next = G.textures; G.textures = t;
    t->hnext = tex_hash[name & (TEX_HASH - 1)]; tex_hash[name & (TEX_HASH - 1)] = t;
    if (name >= G.next_texture) G.next_texture = name + 1;
    return t;
}

GLAPI(void, glBindTexture, (GLenum target, GLuint name))
{
    RECORD(OP_BIND_TEXTURE, target, name, 0, 0, NULL);
    ensure_init();
    if (target == GL_TEXTURE_CUBE_MAP && G.active_unit >= 0 && G.active_unit < 8) { G.cube_units[G.active_unit] = name; texture_object(name); return; }
    if (target != GL_TEXTURE_2D || G.active_unit < 0 || G.active_unit >= 8) return;
    texture_object(name);
    G.units[G.active_unit] = name;
}

GLAPI(void, glDeleteTextures, (GLsizei n, const GLuint *names))
{
    int i;
    for (i = 0; i < n; i++) {
        GlTex **pp = &G.textures;
        while (*pp && (*pp)->name != names[i]) pp = &(*pp)->next;
        if (*pp) {
            GlTex *t = *pp;
            *pp = t->next;
            texture_unhash(t);
            /* released after this frame: releases go out ahead of its draws, which may use it */
            if (t->handle) mesh_release(t->handle, 0);
            tex_bytes -= t->psp_bytes;
            if (G.target == t) G.target = NULL;
            free(t->rgba);
            { int f; for (f = 0; f < 6; f++) free(t->face[f]); }
            free(t);
        }
        { int u; for (u = 0; u < 8; u++) { if (G.units[u] == names[i]) G.units[u] = 0; if (G.cube_units[u] == names[i]) G.cube_units[u] = 0; } }
    }
}
GLAPI(GLboolean, glIsTexture, (GLuint name)) { return find_texture(name) != NULL; }
GLAPI(void, glActiveTexture, (GLenum unit)) { ensure_init(); G.active_unit = (int)(unit - GL_TEXTURE0); }

/* One pixel of client memory as RGBA8. */
static uint32_t read_pixel(const uint8_t *p, GLenum format, GLenum type)
{
    uint32_t r = 0, g = 0, b = 0, a = 255;
    if (type == GL_UNSIGNED_BYTE) {
        switch (format) {
        case GL_RGBA: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
        case GL_BGRA: b = p[0]; g = p[1]; r = p[2]; a = p[3]; break;
        case GL_RGB: r = p[0]; g = p[1]; b = p[2]; break;
        case GL_BGR: b = p[0]; g = p[1]; r = p[2]; break;
        case GL_LUMINANCE: case GL_RED: r = g = b = p[0]; break;
        case GL_LUMINANCE_ALPHA: r = g = b = p[0]; a = p[1]; break;
        case GL_ALPHA: r = g = b = 255; a = p[0]; break;
        case GL_INTENSITY: r = g = b = a = p[0]; break;
        default: break;
        }
    } else if (type == GL_UNSIGNED_SHORT_5_6_5) {
        uint16_t v; memcpy(&v, p, 2);
        r = (v >> 11) * 255 / 31; g = ((v >> 5) & 63) * 255 / 63; b = (v & 31) * 255 / 31;
    } else if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
        uint16_t v; memcpy(&v, p, 2);
        r = (v >> 12) * 17; g = ((v >> 8) & 15) * 17; b = ((v >> 4) & 15) * 17; a = (v & 15) * 17;
    } else if (type == GL_UNSIGNED_SHORT_5_5_5_1) {
        uint16_t v; memcpy(&v, p, 2);
        r = (v >> 11) * 255 / 31; g = ((v >> 6) & 31) * 255 / 31; b = ((v >> 1) & 31) * 255 / 31; a = (v & 1) * 255;
    } else if (type == GL_UNSIGNED_INT_8_8_8_8_REV && format == GL_BGRA) {
        uint32_t v; memcpy(&v, p, 4);
        b = v & 0xFF; g = (v >> 8) & 0xFF; r = (v >> 16) & 0xFF; a = v >> 24;
    } else if (type == GL_UNSIGNED_INT_8_8_8_8 && format == GL_RGBA) {
        uint32_t v; memcpy(&v, p, 4);
        a = v & 0xFF; b = (v >> 8) & 0xFF; g = (v >> 16) & 0xFF; r = v >> 24;
    } else if (type == 0x8036 || type == 0x8368) {   /* GL_UNSIGNED_INT_10_10_10_2, _2_10_10_10_REV: stored x4 */
        uint32_t v; memcpy(&v, p, 4);
        if (type == 0x8036) { r = v >> 22; g = (v >> 12) & 0x3FF; b = (v >> 2) & 0x3FF; a = (v & 3) * 85; }
        else { r = v & 0x3FF; g = (v >> 10) & 0x3FF; b = (v >> 20) & 0x3FF; a = (v >> 30) * 85; }
        if (format == GL_BGRA) { uint32_t t = r; r = b; b = t; }
        r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
    } else if (type == GL_FLOAT) {
        float f[4] = { 0, 0, 0, 1 };
        int n = format == GL_RGBA || format == GL_BGRA ? 4 : format == GL_RGB || format == GL_BGR ? 3 : format == GL_LUMINANCE_ALPHA ? 2 : 1;
        memcpy(f, p, (size_t)n * 4);
        if (format == GL_LUMINANCE || format == GL_RED) f[1] = f[2] = f[0];
        if (format == GL_LUMINANCE_ALPHA) { f[3] = f[1]; f[1] = f[2] = f[0]; }
        if (format == GL_ALPHA) { f[3] = f[0]; f[0] = f[1] = f[2] = 1; }
        if (format == GL_BGR || format == GL_BGRA) { float t = f[0]; f[0] = f[2]; f[2] = t; }
        r = clamp8(f[0]); g = clamp8(f[1]); b = clamp8(f[2]); a = clamp8(f[3]);
    }
    return r | g << 8 | b << 16 | a << 24;
}

static int pixel_bytes(GLenum format, GLenum type)
{
    int comps;
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1: return 2;
    case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV: case 0x8036: case 0x8368: return 4;
    default: break;
    }
    switch (format) {
    case GL_RGBA: case GL_BGRA: comps = 4; break;
    case GL_RGB: case GL_BGR: comps = 3; break;
    case GL_LUMINANCE_ALPHA: comps = 2; break;
    default: comps = 1; break;
    }
    return comps * (type == GL_FLOAT ? 4 : 1);
}

static int format_has_alpha(GLint internal, GLenum format)
{
    switch (internal) {
    case 3: case GL_RGB: case GL_RGB5: case GL_RGB8: case GL_LUMINANCE: case GL_LUMINANCE8: return 0;
    case 1: return format == GL_ALPHA || format == GL_INTENSITY;
    default: break;
    }
    return 1;
}

/* Copy a client rectangle into the texture's RGBA8 level 0. */
/* The texels written; `changed` (x0, y0, x1, y1), when given, the rectangle that differs from before
 * (x1 <= x0: nothing did). */
static void store_pixels_changed(GlTex *t, int x0, int y0, int w, int h, GLenum format, GLenum type, const void *pixels, int *changed);
static void store_pixels(GlTex *t, int x0, int y0, int w, int h, GLenum format, GLenum type, const void *pixels)
{
    store_pixels_changed(t, x0, y0, w, h, format, type, pixels, NULL);
}
static void store_pixels_changed(GlTex *t, int x0, int y0, int w, int h, GLenum format, GLenum type, const void *pixels, int *changed)
{
    const uint8_t *base = (const uint8_t *)pixels;
    int bpp = pixel_bytes(format, type), rowlen = G.unpack_row_length > 0 ? G.unpack_row_length : w, x, y;
    size_t row = (size_t)rowlen * (size_t)bpp;
    int al = G.unpack_alignment > 0 ? G.unpack_alignment : 1;
    if (!base) return;
    row = (row + (size_t)al - 1) / (size_t)al * (size_t)al;
    base += (size_t)G.unpack_skip_rows * row + (size_t)G.unpack_skip_pixels * (size_t)bpp;
    if (changed) { changed[0] = x0 + w; changed[1] = y0 + h; changed[2] = x0; changed[3] = y0; }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t *d = &t->rgba[(size_t)(y0 + y) * (size_t)t->w + (size_t)(x0 + x)];
            uint32_t px = read_pixel(base + (size_t)y * row + (size_t)x * (size_t)bpp, format, type);
            if (changed && px != *d) {
                if (x0 + x < changed[0]) changed[0] = x0 + x;
                if (y0 + y < changed[1]) changed[1] = y0 + y;
                if (x0 + x + 1 > changed[2]) changed[2] = x0 + x + 1;
                if (y0 + y + 1 > changed[3]) changed[3] = y0 + y + 1;
            }
            *d = px;
        }
}

/* A cube map's face (GL_TEXTURE_CUBE_MAP_POSITIVE_X + f), level 0: kept here only. The GE has
 * no cube maps; a shader's reflection is drawn from a sphere map made of them (env_map). */
static void cube_face(int f, GLint level, GLsizei w, GLsizei h, GLenum format, GLenum type, const GLvoid *pixels)
{
    GlTex *t;
    uint32_t *p, *keep;
    int kw, kh;
    if (level != 0 || G.active_unit < 0 || G.active_unit >= 8 || !(t = texture_object(G.cube_units[G.active_unit]))) return;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) { set_error(GL_INVALID_VALUE); return; }
    if (!(p = (uint32_t *)realloc(t->face[f], (size_t)w * (size_t)h * 4))) { set_error(GL_OUT_OF_MEMORY); return; }
    t->face[f] = p; t->face_w = w; t->face_h = h;
    if (pixels) {   /* read as a 2D image would be */
        keep = t->rgba; kw = t->w; kh = t->h;
        t->rgba = p; t->w = w; t->h = h;
        store_pixels(t, 0, 0, w, h, format, type, pixels);
        t->rgba = keep; t->w = kw; t->h = kh;
    } else memset(p, 0, (size_t)w * (size_t)h * 4);
    t->face_ver++;
    if (getenv("PSPEXP_GL_DUMP")) {
        char path[512];
        FILE *o;
        snprintf(path, sizeof(path), "%s/cube-%u-%d-%dx%d.rgba", getenv("PSPEXP_GL_DUMP"), t->name, f, w, h);
        if ((o = fopen(path, "wb"))) { fwrite(p, 4, (size_t)w * (size_t)h, o); fclose(o); }
    }
}

GLAPI(void, glTexImage2D, (GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border,
                           GLenum format, GLenum type, const GLvoid *pixels))
{
    GlTex *t;
    (void)border;
    ensure_init();
    if (target == GL_PROXY_TEXTURE_2D) {
        int ok = w <= 4096 && h <= 4096 && w >= 0 && h >= 0;
        G.proxy_w = ok ? w : 0; G.proxy_h = ok ? h : 0;
        return;
    }
    if (target >= 0x8515 && target <= 0x851A) { cube_face((int)(target - 0x8515), level, w, h, format, type, pixels); return; }
    if (target != GL_TEXTURE_2D || G.active_unit < 0 || G.active_unit >= 8) return;
    if (level != 0) return;   /* mipmaps: the GE is given level 0 only */
    if (!(t = texture_object(G.units[G.active_unit]))) { set_error(GL_INVALID_OPERATION); return; }
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) { set_error(GL_INVALID_VALUE); return; }
    if (t->w != w || t->h != h || !t->rgba) {
        uint32_t *p = (uint32_t *)realloc(t->rgba, (size_t)w * (size_t)h * 4);
        if (!p) { set_error(GL_OUT_OF_MEMORY); return; }
        t->rgba = p; t->w = w; t->h = h;
    }
    t->has_alpha = format_has_alpha(internal, format);
    if (t->rt && (pixels || t->rt_w < w || t->rt_h < h)) {   /* no longer that render target */
        if (t->handle) mesh_release(t->handle, 0);
        tex_bytes -= t->psp_bytes;
        t->psp_bytes = 0;
        t->handle = 0; t->rt = 0;
    }
    if (pixels) store_pixels(t, 0, 0, w, h, format, type, pixels);
    else memset(t->rgba, 0, (size_t)w * (size_t)h * 4);
    t->prescale = type == 0x8036 || type == 0x8368 ? 4.0f : 1.0f;
    t->dirty = 1;
}
GLAPI(void, glTexImage1D, (GLenum target, GLint level, GLint internal, GLsizei w, GLint border, GLenum format, GLenum type, const GLvoid *pixels))
{
    (void)target; (void)level; (void)internal; (void)w; (void)border; (void)format; (void)type; (void)pixels;
    unsupported("1D textures");
}

GLAPI(void, glTexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                              GLenum format, GLenum type, const GLvoid *pixels))
{
    GlTex *t;
    ensure_init();
    if (target != GL_TEXTURE_2D || level != 0 || G.active_unit < 0 || G.active_unit >= 8) return;
    if (!(t = find_texture(G.units[G.active_unit])) || !t->rgba) { set_error(GL_INVALID_OPERATION); return; }
    if (x < 0 || y < 0 || x + w > t->w || y + h > t->h) { set_error(GL_INVALID_VALUE); return; }
    {
        int ch[4];   /* what really changed: Quake uploads whole rows of a lightmap for a few texels */
        store_pixels_changed(t, x, y, w, h, format, type, pixels, ch);
        if (ch[2] <= ch[0]) return;   /* the same texels again */
        if (!t->dirty || t->part) {   /* only this rectangle changed since it was sent (so far) */
            if (!t->part) { t->part = 1; t->part_x0 = ch[0]; t->part_y0 = ch[1]; t->part_x1 = ch[2]; t->part_y1 = ch[3]; }
            else {
                if (ch[0] < t->part_x0) t->part_x0 = ch[0];
                if (ch[1] < t->part_y0) t->part_y0 = ch[1];
                if (ch[2] > t->part_x1) t->part_x1 = ch[2];
                if (ch[3] > t->part_y1) t->part_y1 = ch[3];
            }
        }
    }
    t->dirty = 1;
}

GLAPI(void, glCopyTexImage2D, (GLenum target, GLint level, GLenum internal, GLint x, GLint y, GLsizei w, GLsizei h, GLint border))
{ (void)target; (void)level; (void)internal; (void)x; (void)y; (void)w; (void)h; (void)border; unsupported("glCopyTexImage2D"); }
GLAPI(void, glCopyTexSubImage2D, (GLenum target, GLint level, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h))
{ (void)target; (void)level; (void)xo; (void)yo; (void)x; (void)y; (void)w; (void)h; unsupported("glCopyTexSubImage2D"); }
GLAPI(void, glCompressedTexImage2D, (GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLint border, GLsizei size, const GLvoid *data))
{ (void)target; (void)level; (void)internal; (void)w; (void)h; (void)border; (void)size; (void)data; unsupported("compressed textures"); }
GLAPI(void, glGenerateMipmap, (GLenum target)) { (void)target; }

GLAPI(void, glTexParameteri, (GLenum target, GLenum p, GLint v))
{
    GlTex *t;
    float f = (float)v;
    RECORD(OP_TEX_PARAMETER, target, p, 0, 1, &f);
    ensure_init();
    if (target != GL_TEXTURE_2D || G.active_unit < 0 || G.active_unit >= 8 || !(t = texture_object(G.units[G.active_unit]))) return;
    switch (p) {
    case GL_TEXTURE_MIN_FILTER: t->min_filter = (GLenum)v; break;
    case GL_TEXTURE_MAG_FILTER: t->mag_filter = (GLenum)v; break;
    case GL_TEXTURE_WRAP_S: t->wrap_s = (GLenum)v; break;
    case GL_TEXTURE_WRAP_T: t->wrap_t = (GLenum)v; break;
    default: break;
    }
}
GLAPI(void, glTexParameterf, (GLenum target, GLenum p, GLfloat v)) { GLFN(glTexParameteri)(target, p, (GLint)v); }
GLAPI(void, glTexParameteriv, (GLenum target, GLenum p, const GLint *v)) { GLFN(glTexParameteri)(target, p, v[0]); }
GLAPI(void, glTexParameterfv, (GLenum target, GLenum p, const GLfloat *v)) { GLFN(glTexParameteri)(target, p, (GLint)v[0]); }
GLAPI(void, glGetTexParameteriv, (GLenum target, GLenum p, GLint *v))
{
    GlTex *t = find_texture(G.units[G.active_unit & 7]);
    (void)target;
    *v = 0;
    if (!t) return;
    switch (p) {
    case GL_TEXTURE_MIN_FILTER: *v = (GLint)t->min_filter; break;
    case GL_TEXTURE_MAG_FILTER: *v = (GLint)t->mag_filter; break;
    case GL_TEXTURE_WRAP_S: *v = (GLint)t->wrap_s; break;
    case GL_TEXTURE_WRAP_T: *v = (GLint)t->wrap_t; break;
    default: break;
    }
}
GLAPI(void, glGetTexLevelParameteriv, (GLenum target, GLint level, GLenum p, GLint *v))
{
    GlTex *t = target == GL_PROXY_TEXTURE_2D ? NULL : find_texture(G.units[G.active_unit & 7]);
    int w = target == GL_PROXY_TEXTURE_2D ? G.proxy_w : t ? t->w : 0, h = target == GL_PROXY_TEXTURE_2D ? G.proxy_h : t ? t->h : 0;
    if (level > 0) { w >>= level; h >>= level; }
    switch (p) {
    case GL_TEXTURE_WIDTH: *v = w; break;
    case GL_TEXTURE_HEIGHT: *v = h; break;
    case GL_TEXTURE_INTERNAL_FORMAT: *v = t && !t->has_alpha ? GL_RGB : GL_RGBA; break;
    default: *v = 0; break;
    }
}
GLAPI(void, glGetTexLevelParameterfv, (GLenum target, GLint level, GLenum p, GLfloat *v)) { GLint i; GLFN(glGetTexLevelParameteriv)(target, level, p, &i); *v = (float)i; }
GLAPI(void, glGetTexImage, (GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels))
{ (void)target; (void)level; (void)format; (void)type; (void)pixels; unsupported("glGetTexImage"); }

GLAPI(void, glTexEnvfv, (GLenum target, GLenum p, const GLfloat *v))
{
    RECORD(OP_TEX_ENV, target, p, 0, p == GL_TEXTURE_ENV_COLOR ? 4 : 1, v);
    ensure_init();
    if (target != GL_TEXTURE_ENV || G.active_unit != 0) return;
    if (p == GL_TEXTURE_ENV_MODE) {
        G.tex_env_mode = (GLenum)v[0];
        if (G.tex_env_mode == GL_COMBINE) unsupported("GL_COMBINE texture environment (modulate instead)");
    } else if (p == GL_TEXTURE_ENV_COLOR) memcpy(G.tex_env_color, v, 16);
}
GLAPI(void, glTexEnvf, (GLenum target, GLenum p, GLfloat v)) { GLFN(glTexEnvfv)(target, p, &v); }
GLAPI(void, glTexEnvi, (GLenum target, GLenum p, GLint v)) { float f = (float)v; GLFN(glTexEnvfv)(target, p, &f); }
GLAPI(void, glTexEnviv, (GLenum target, GLenum p, const GLint *v))
{
    float f[4] = { v[0] / 2147483647.0f, v[1] / 2147483647.0f, v[2] / 2147483647.0f, v[3] / 2147483647.0f };
    if (p != GL_TEXTURE_ENV_COLOR) f[0] = (float)v[0];
    GLFN(glTexEnvfv)(target, p, f);
}
static int mat_invert(const float *m, float *out);

GLAPI(void, glTexGenfv, (GLenum coord, GLenum p, const GLfloat *v))
{
    int c = coord == GL_S ? 0 : coord == GL_T ? 1 : -1;
    ensure_init();
    if (c < 0) return;   /* r, q: not for 2D textures */
    switch (p) {
    case GL_TEXTURE_GEN_MODE:
        G.texgen_mode[c] = (GLenum)v[0];
        if (G.texgen_mode[c] != GL_SPHERE_MAP && G.texgen_mode[c] != GL_OBJECT_LINEAR && G.texgen_mode[c] != GL_EYE_LINEAR)
            unsupported("texture coordinate generation other than sphere map and linear");
        break;
    case GL_OBJECT_PLANE: memcpy(G.texgen_obj[c], v, 16); break;
    case GL_EYE_PLANE: {   /* kept as GL does: times the inverse of the modelview of the moment */
        float inv[16];
        int i;
        if (mat_invert(G.mv[G.mv_top], inv) < 0) { memcpy(G.texgen_eye[c], v, 16); break; }
        for (i = 0; i < 4; i++) G.texgen_eye[c][i] = v[0] * inv[i * 4 + 0] + v[1] * inv[i * 4 + 1] + v[2] * inv[i * 4 + 2] + v[3] * inv[i * 4 + 3];
        break;
    }
    default: break;
    }
}
GLAPI(void, glTexGenf, (GLenum c, GLenum p, GLfloat v)) { GLFN(glTexGenfv)(c, p, &v); }
GLAPI(void, glTexGeni, (GLenum c, GLenum p, GLint v)) { float f = (float)v; GLFN(glTexGenfv)(c, p, &f); }
GLAPI(void, glTexGeniv, (GLenum c, GLenum p, const GLint *v)) { float f[4] = { (float)v[0], (float)v[1], (float)v[2], (float)v[3] }; GLFN(glTexGenfv)(c, p, f); }
GLAPI(void, glTexGendv, (GLenum c, GLenum p, const GLdouble *v)) { float f[4] = { (float)v[0], (float)v[1], (float)v[2], (float)v[3] }; GLFN(glTexGenfv)(c, p, f); }
GLAPI(void, glPrioritizeTextures, (GLsizei n, const GLuint *t, const GLclampf *p)) { (void)n; (void)t; (void)p; }
GLAPI(GLboolean, glAreTexturesResident, (GLsizei n, const GLuint *t, GLboolean *r)) { int i; for (i = 0; i < n; i++) r[i] = GL_TRUE; (void)t; return GL_TRUE; }

/* ---- display lists ---------------------------------------------------------------------- */

GLAPI(GLuint, glGenLists, (GLsizei range))
{
    GLuint first;
    ensure_init();
    if (range <= 0) return 0;
    first = G.next_list;
    G.next_list += (GLuint)range;
    return first;
}

GLAPI(GLboolean, glIsList, (GLuint name)) { return find_list(name) != NULL; }

GLAPI(void, glDeleteLists, (GLuint first, GLsizei range))
{
    List **pp = &G.lists;
    while (*pp) {
        List *l = *pp;
        if (l->name >= first && l->name < first + (GLuint)range && l != G.compiling) { *pp = l->next; free(l->ops); free(l); }
        else pp = &l->next;
    }
}

GLAPI(void, glNewList, (GLuint name, GLenum mode))
{
    List *l;
    ensure_init();
    if (G.compiling) { set_error(GL_INVALID_OPERATION); return; }
    if (!name) { set_error(GL_INVALID_VALUE); return; }
    if ((l = find_list(name))) l->len = 0;
    else {
        if (!(l = (List *)calloc(1, sizeof(*l)))) { set_error(GL_OUT_OF_MEMORY); return; }
        l->name = name; l->next = G.lists; G.lists = l;
        if (name >= G.next_list) G.next_list = name + 1;
    }
    G.compiling = l;
    G.list_mode = mode;
}

GLAPI(void, glEndList, (void))
{
    if (!G.compiling) { set_error(GL_INVALID_OPERATION); return; }
    G.compiling = NULL;
}

GLAPI(void, glCallList, (GLuint name))
{
    RECORD(OP_CALL_LIST, name, 0, 0, 0, NULL);
    play_list(name);
}

GLAPI(void, glListBase, (GLuint base)) { RECORD(OP_LIST_BASE, base, 0, 0, 0, NULL); ensure_init(); G.list_base = base; }

GLAPI(void, glCallLists, (GLsizei n, GLenum type, const GLvoid *lists))
{
    int i;
    for (i = 0; i < n; i++) {
        GLuint id;
        switch (type) {
        case GL_UNSIGNED_BYTE: id = ((const GLubyte *)lists)[i]; break;
        case GL_BYTE: id = (GLuint)((const GLbyte *)lists)[i]; break;
        case GL_UNSIGNED_SHORT: id = ((const GLushort *)lists)[i]; break;
        case GL_SHORT: id = (GLuint)((const GLshort *)lists)[i]; break;
        case GL_UNSIGNED_INT: case GL_INT: id = ((const GLuint *)lists)[i]; break;
        case GL_FLOAT: id = (GLuint)((const GLfloat *)lists)[i]; break;
        default: set_error(GL_INVALID_ENUM); return;
        }
        GLFN(glCallList)(G.list_base + id);
    }
}

/* push/pop of the attribute groups: the whole render state at once */
typedef struct Attrib {
    float color[4], normal[3], texcoord[4];
    int depth_test, cull, blend, alpha_test, texture_2d, fog, lighting, color_material, scissor_test, dither, polygon_offset, normalize;
    GLenum depth_func; int depth_mask; float depth_near, depth_far, clear_depth;
    GLenum blend_src, blend_dst, blend_eq; GLenum alpha_func; float alpha_ref;
    GLenum cull_mode, front_face, shade_model; int color_mask[4]; float clear_color[4];
    int viewport[4], scissor[4];
    GLenum fog_mode; float fog_start, fog_end, fog_density, fog_color[4];
    GLenum tex_env_mode; float tex_env_color[4];
    Light light[MAX_LIGHTS]; Material mat; float scene_ambient[4]; GLenum cm_face, cm_mode;
    GLuint units[8]; GLenum matrix_mode; float point_size, line_width;
} Attrib;
static Attrib attrib_stack[16];
static int attrib_top;

#define ATTR_COPY(dst, src) do { \
    memcpy((dst).color, (src).color, sizeof((dst).color)); memcpy((dst).normal, (src).normal, sizeof((dst).normal)); \
    memcpy((dst).texcoord, (src).texcoord, sizeof((dst).texcoord)); \
    (dst).depth_test = (src).depth_test; (dst).cull = (src).cull; (dst).blend = (src).blend; (dst).alpha_test = (src).alpha_test; \
    (dst).texture_2d = (src).texture_2d; (dst).fog = (src).fog; (dst).lighting = (src).lighting; \
    (dst).color_material = (src).color_material; (dst).scissor_test = (src).scissor_test; (dst).dither = (src).dither; \
    (dst).polygon_offset = (src).polygon_offset; (dst).normalize = (src).normalize; \
    (dst).depth_func = (src).depth_func; (dst).depth_mask = (src).depth_mask; (dst).depth_near = (src).depth_near; \
    (dst).depth_far = (src).depth_far; (dst).clear_depth = (src).clear_depth; \
    (dst).blend_src = (src).blend_src; (dst).blend_dst = (src).blend_dst; (dst).blend_eq = (src).blend_eq; \
    (dst).alpha_func = (src).alpha_func; (dst).alpha_ref = (src).alpha_ref; \
    (dst).cull_mode = (src).cull_mode; (dst).front_face = (src).front_face; (dst).shade_model = (src).shade_model; \
    memcpy((dst).color_mask, (src).color_mask, sizeof((dst).color_mask)); memcpy((dst).clear_color, (src).clear_color, sizeof((dst).clear_color)); \
    memcpy((dst).viewport, (src).viewport, sizeof((dst).viewport)); memcpy((dst).scissor, (src).scissor, sizeof((dst).scissor)); \
    (dst).fog_mode = (src).fog_mode; (dst).fog_start = (src).fog_start; (dst).fog_end = (src).fog_end; \
    (dst).fog_density = (src).fog_density; memcpy((dst).fog_color, (src).fog_color, sizeof((dst).fog_color)); \
    (dst).tex_env_mode = (src).tex_env_mode; memcpy((dst).tex_env_color, (src).tex_env_color, sizeof((dst).tex_env_color)); \
    memcpy((dst).light, (src).light, sizeof((dst).light)); (dst).mat = (src).mat; \
    memcpy((dst).scene_ambient, (src).scene_ambient, sizeof((dst).scene_ambient)); \
    (dst).cm_face = (src).cm_face; (dst).cm_mode = (src).cm_mode; memcpy((dst).units, (src).units, sizeof((dst).units)); \
    (dst).matrix_mode = (src).matrix_mode; (dst).point_size = (src).point_size; (dst).line_width = (src).line_width; \
} while (0)

GLAPI(void, glPushAttrib, (GLbitfield mask))
{
    RECORD(OP_PUSH_ATTRIB, mask, 0, 0, 0, NULL);
    ensure_init();
    if (attrib_top >= 16) { set_error(GL_STACK_OVERFLOW); return; }
    ATTR_COPY(attrib_stack[attrib_top], G);
    attrib_top++;
}
GLAPI(void, glPopAttrib, (void))
{
    RECORD0(OP_POP_ATTRIB);
    if (attrib_top <= 0) { set_error(GL_STACK_UNDERFLOW); return; }
    attrib_top--;
    ATTR_COPY(G, attrib_stack[attrib_top]);
    G.lights_dirty = 1;
}

typedef struct ClientAttrib { Array va, ca, na, ta; int client_unit; GLuint array_buffer, element_buffer; int unpack[4]; } ClientAttrib;
static ClientAttrib client_stack[16];
static int client_top;
GLAPI(void, glPushClientAttrib, (GLbitfield mask))
{
    ClientAttrib *c;
    (void)mask;
    ensure_init();
    if (client_top >= 16) { set_error(GL_STACK_OVERFLOW); return; }
    c = &client_stack[client_top++];
    c->va = G.va; c->ca = G.ca; c->na = G.na; c->ta = G.ta; c->client_unit = G.client_unit;
    c->array_buffer = G.array_buffer; c->element_buffer = G.element_buffer;
    c->unpack[0] = G.unpack_alignment; c->unpack[1] = G.unpack_row_length; c->unpack[2] = G.unpack_skip_rows; c->unpack[3] = G.unpack_skip_pixels;
}
GLAPI(void, glPopClientAttrib, (void))
{
    ClientAttrib *c;
    if (client_top <= 0) { set_error(GL_STACK_UNDERFLOW); return; }
    c = &client_stack[--client_top];
    G.va = c->va; G.ca = c->ca; G.na = c->na; G.ta = c->ta; G.client_unit = c->client_unit;
    G.array_buffer = c->array_buffer; G.element_buffer = c->element_buffer;
    G.unpack_alignment = c->unpack[0]; G.unpack_row_length = c->unpack[1]; G.unpack_skip_rows = c->unpack[2]; G.unpack_skip_pixels = c->unpack[3];
}

static void play_list(GLuint name)
{
    List *l = find_list(name);
    size_t pos = 0;
    if (!l || G.list_depth >= 64) return;
    G.list_depth++;
    replaying++;
    while (pos + sizeof(ListOp) <= l->len) {
        ListOp o;
        const float *f;
        memcpy(&o, l->ops + pos, sizeof(o));
        f = (const float *)(const void *)(l->ops + pos + sizeof(o));
        pos += sizeof(o) + (size_t)o.nf * sizeof(float);
        switch (o.op) {
        case OP_BEGIN: GLFN(glBegin)(o.i[0]); break;
        case OP_END: GLFN(glEnd)(); break;
        case OP_VERTEX: GLFN(glVertex3f)(f[0], f[1], f[2]); break;
        case OP_COLOR: GLFN(glColor4f)(f[0], f[1], f[2], f[3]); break;
        case OP_NORMAL: GLFN(glNormal3f)(f[0], f[1], f[2]); break;
        case OP_TEXCOORD: GLFN(glTexCoord4f)(f[0], f[1], f[2], f[3]); break;
        case OP_ENABLE: GLFN(glEnable)(o.i[0]); break;
        case OP_DISABLE: GLFN(glDisable)(o.i[0]); break;
        case OP_MATRIX_MODE: GLFN(glMatrixMode)(o.i[0]); break;
        case OP_LOAD_IDENTITY: GLFN(glLoadIdentity)(); break;
        case OP_LOAD_MATRIX: GLFN(glLoadMatrixf)(f); break;
        case OP_MULT_MATRIX: GLFN(glMultMatrixf)(f); break;
        case OP_PUSH_MATRIX: GLFN(glPushMatrix)(); break;
        case OP_POP_MATRIX: GLFN(glPopMatrix)(); break;
        case OP_TRANSLATE: GLFN(glTranslatef)(f[0], f[1], f[2]); break;
        case OP_ROTATE: GLFN(glRotatef)(f[0], f[1], f[2], f[3]); break;
        case OP_SCALE: GLFN(glScalef)(f[0], f[1], f[2]); break;
        case OP_ORTHO: GLFN(glOrtho)(f[0], f[1], f[2], f[3], f[4], f[5]); break;
        case OP_FRUSTUM: GLFN(glFrustum)(f[0], f[1], f[2], f[3], f[4], f[5]); break;
        case OP_BLEND_FUNC: GLFN(glBlendFunc)(o.i[0], o.i[1]); break;
        case OP_DEPTH_FUNC: GLFN(glDepthFunc)(o.i[0]); break;
        case OP_DEPTH_MASK: GLFN(glDepthMask)((GLboolean)o.i[0]); break;
        case OP_ALPHA_FUNC: GLFN(glAlphaFunc)(o.i[0], f[0]); break;
        case OP_MATERIAL: GLFN(glMaterialfv)(o.i[0], o.i[1], f); break;
        case OP_LIGHT: GLFN(glLightfv)(o.i[0], o.i[1], f); break;
        case OP_LIGHT_MODEL: GLFN(glLightModelfv)(o.i[0], f); break;
        case OP_COLOR_MATERIAL: GLFN(glColorMaterial)(o.i[0], o.i[1]); break;
        case OP_SHADE_MODEL: GLFN(glShadeModel)(o.i[0]); break;
        case OP_BIND_TEXTURE: GLFN(glBindTexture)(o.i[0], o.i[1]); break;
        case OP_TEX_ENV: GLFN(glTexEnvfv)(o.i[0], o.i[1], f); break;
        case OP_TEX_PARAMETER: GLFN(glTexParameteri)(o.i[0], o.i[1], (GLint)f[0]); break;
        case OP_FOG: if (o.nf == 4) GLFN(glFogfv)(o.i[0], f); else GLFN(glFogf)(o.i[0], f[0]); break;
        case OP_CALL_LIST: play_list(o.i[0]); break;
        case OP_CULL_FACE: GLFN(glCullFace)(o.i[0]); break;
        case OP_FRONT_FACE: GLFN(glFrontFace)(o.i[0]); break;
        case OP_COLOR_MASK: GLFN(glColorMask)(o.i[0] & 1, (o.i[0] >> 1) & 1, (o.i[0] >> 2) & 1, (o.i[0] >> 3) & 1); break;
        case OP_SCISSOR: GLFN(glScissor)((GLint)o.i[0], (GLint)o.i[1], (GLsizei)o.i[2], (GLsizei)f[0]); break;
        case OP_VIEWPORT: GLFN(glViewport)((GLint)o.i[0], (GLint)o.i[1], (GLsizei)o.i[2], (GLsizei)f[0]); break;
        case OP_PUSH_ATTRIB: GLFN(glPushAttrib)(o.i[0]); break;
        case OP_POP_ATTRIB: GLFN(glPopAttrib)(); break;
        case OP_CLEAR: GLFN(glClear)(o.i[0]); break;
        case OP_CLEAR_COLOR: GLFN(glClearColor)(f[0], f[1], f[2], f[3]); break;
        case OP_CLEAR_DEPTH: GLFN(glClearDepth)(f[0]); break;
        case OP_BLEND_EQUATION: GLFN(glBlendEquation)(o.i[0]); break;
        case OP_LIST_BASE: GLFN(glListBase)(o.i[0]); break;
        default: break;
        }
    }
    replaying--;
    G.list_depth--;
}

/* ---- queries ------------------------------------------------------------------------------ */

/* GL_ARB_multitexture: one unit (the GE's); programs that need it at all
 * still run, what they put in other units is left out. */
static const char EXTENSIONS[] =
    "GL_ARB_multitexture GL_ARB_point_parameters GL_ARB_vertex_buffer_object GL_EXT_bgra GL_EXT_compiled_vertex_array GL_EXT_texture_edge_clamp "
    "GL_SGIS_texture_edge_clamp GL_EXT_draw_range_elements GL_ARB_transpose_matrix GL_EXT_blend_minmax "
    "GL_EXT_blend_subtract GL_ARB_texture_env_add GL_EXT_texture_env_add GL_EXT_packed_pixels";

int pspexp_gl_extension(const char *name)
{
    const char *p = EXTENSIONS;
    size_t n = strlen(name);
    while ((p = strstr(p, name))) {
        if ((p == EXTENSIONS || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return 1;
        p += n;
    }
    return 0;
}

GLAPI(const GLubyte *, glGetString, (GLenum name))
{
    switch (name) {
    case GL_VENDOR: return (const GLubyte *)"PSPExp";
    case GL_RENDERER: return (const GLubyte *)"PSP GE (PSPExp)";
    case GL_VERSION: return (const GLubyte *)(getenv("PSPEXP_GL_VERSION") ? getenv("PSPEXP_GL_VERSION") : "2.1 PSPExp");
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"1.20";
    case GL_EXTENSIONS: return (const GLubyte *)EXTENSIONS;
    default: set_error(GL_INVALID_ENUM); return NULL;
    }
}

GLAPI(GLenum, glGetError, (void)) { GLenum e = G.error; G.error = GL_NO_ERROR; return e; }

/* Fills up to 16 values; returns how many, 0 for an unknown name. */
static int get_values(GLenum p, float *f)
{
    int ww, wh, ox, oy; float s;
    ensure_init();
    switch (p) {
    case GL_MAX_TEXTURE_SIZE: f[0] = 4096; return 1;   /* taken, and scaled to the PSP's 512 at most on the way */
    case GL_MAX_LIGHTS: f[0] = MAX_LIGHTS; return 1;
    case GL_MAX_CLIP_PLANES: f[0] = 6; return 1;
    case GL_MAX_MODELVIEW_STACK_DEPTH: f[0] = MV_DEPTH; return 1;
    case GL_MAX_PROJECTION_STACK_DEPTH: f[0] = PROJ_DEPTH; return 1;
    case GL_MAX_TEXTURE_STACK_DEPTH: f[0] = TEX_DEPTH; return 1;
    case GL_MAX_TEXTURE_UNITS: case GL_MAX_TEXTURE_COORDS:   /* (PSPEXP_GL_UNITS: more, for shader games that ask) */
        f[0] = getenv("PSPEXP_GL_UNITS") && atoi(getenv("PSPEXP_GL_UNITS")) > 1 ? (float)atoi(getenv("PSPEXP_GL_UNITS")) : 1;
        if (f[0] > 8) f[0] = 8;
        return 1;
    case GL_MAX_TEXTURE_IMAGE_UNITS: case 0x8B4D /* GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS */: f[0] = 8; return 1;
    case 0x8B4C /* GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS */: f[0] = 0; return 1;
    case GL_MAX_VERTEX_ATTRIBS: f[0] = 16; return 1;
    case 0x8DFB /* GL_MAX_VERTEX_UNIFORM_VECTORS */: f[0] = 256; return 1;
    case 0x8DFD /* GL_MAX_FRAGMENT_UNIFORM_VECTORS */: f[0] = 64; return 1;
    case 0x8DFC /* GL_MAX_VARYING_VECTORS */: f[0] = 8; return 1;
    case 0x8B4A /* GL_MAX_VERTEX_UNIFORM_COMPONENTS */: f[0] = 1024; return 1;
    case 0x8B49 /* GL_MAX_FRAGMENT_UNIFORM_COMPONENTS */: f[0] = 256; return 1;
    case 0x8B4B /* GL_MAX_VARYING_FLOATS */: f[0] = 32; return 1;
    case GL_CURRENT_PROGRAM: f[0] = (float)G.program; return 1;
    case GL_FRAMEBUFFER_BINDING: f[0] = (float)G.fbo; return 1;
    case GL_VERTEX_ARRAY_BINDING: f[0] = (float)G.vao; return 1;
    case GL_MAX_ELEMENTS_VERTICES: case GL_MAX_ELEMENTS_INDICES: f[0] = 65536; return 1;
    case GL_MAX_LIST_NESTING: f[0] = 64; return 1;
    case GL_MAX_VIEWPORT_DIMS: f[0] = f[1] = 4096; return 2;
    case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: f[0] = 1; return 1;
    case GL_SUBPIXEL_BITS: f[0] = 4; return 1;
    case GL_RED_BITS: case GL_BLUE_BITS: f[0] = 5; return 1;
    case GL_GREEN_BITS: f[0] = 6; return 1;
    case GL_ALPHA_BITS: case GL_STENCIL_BITS: case GL_SAMPLE_BUFFERS: case GL_SAMPLES: f[0] = 0; return 1;
    case GL_DEPTH_BITS: f[0] = 16; return 1;
    case GL_DOUBLEBUFFER: f[0] = 1; return 1;
    case GL_MAJOR_VERSION: f[0] = 2; return 1;
    case GL_MINOR_VERSION: f[0] = 1; return 1;
    case GL_NUM_EXTENSIONS: f[0] = 0; return 1;
    case GL_VIEWPORT: f[0] = (float)G.viewport[0]; f[1] = (float)G.viewport[1]; f[2] = (float)G.viewport[2]; f[3] = (float)G.viewport[3]; return 4;
    case GL_SCISSOR_BOX: f[0] = (float)G.scissor[0]; f[1] = (float)G.scissor[1]; f[2] = (float)G.scissor[2]; f[3] = (float)G.scissor[3]; return 4;
    case GL_MATRIX_MODE: f[0] = (float)G.matrix_mode; return 1;
    case GL_MODELVIEW_MATRIX: memcpy(f, G.mv[G.mv_top], 64); return 16;
    case GL_PROJECTION_MATRIX: memcpy(f, G.proj[G.proj_top], 64); return 16;
    case GL_TEXTURE_MATRIX: memcpy(f, G.texm[G.tex_top], 64); return 16;
    case GL_MODELVIEW_STACK_DEPTH: f[0] = (float)(G.mv_top + 1); return 1;
    case GL_PROJECTION_STACK_DEPTH: f[0] = (float)(G.proj_top + 1); return 1;
    case GL_TEXTURE_STACK_DEPTH: f[0] = (float)(G.tex_top + 1); return 1;
    case GL_CURRENT_COLOR: memcpy(f, G.color, 16); return 4;
    case GL_CURRENT_NORMAL: memcpy(f, G.normal, 12); return 3;
    case GL_CURRENT_TEXTURE_COORDS: memcpy(f, G.texcoord, 16); return 4;
    case GL_COLOR_CLEAR_VALUE: memcpy(f, G.clear_color, 16); return 4;
    case GL_DEPTH_CLEAR_VALUE: f[0] = G.clear_depth; return 1;
    case GL_DEPTH_RANGE: f[0] = G.depth_near; f[1] = G.depth_far; return 2;
    case GL_DEPTH_FUNC: f[0] = (float)G.depth_func; return 1;
    case GL_DEPTH_WRITEMASK: f[0] = (float)G.depth_mask; return 1;
    case GL_COLOR_WRITEMASK: f[0] = (float)G.color_mask[0]; f[1] = (float)G.color_mask[1]; f[2] = (float)G.color_mask[2]; f[3] = (float)G.color_mask[3]; return 4;
    case GL_BLEND_SRC: f[0] = (float)G.blend_src; return 1;
    case GL_BLEND_DST: f[0] = (float)G.blend_dst; return 1;
    case GL_BLEND_EQUATION: f[0] = (float)G.blend_eq; return 1;
    case GL_ALPHA_TEST_FUNC: f[0] = (float)G.alpha_func; return 1;
    case GL_ALPHA_TEST_REF: f[0] = G.alpha_ref; return 1;
    case GL_CULL_FACE_MODE: f[0] = (float)G.cull_mode; return 1;
    case GL_FRONT_FACE: f[0] = (float)G.front_face; return 1;
    case GL_SHADE_MODEL: f[0] = (float)G.shade_model; return 1;
    case GL_TEXTURE_BINDING_2D: f[0] = (float)G.units[G.active_unit & 7]; return 1;
    case GL_ACTIVE_TEXTURE: f[0] = (float)(GL_TEXTURE0 + G.active_unit); return 1;
    case GL_CLIENT_ACTIVE_TEXTURE: f[0] = (float)(GL_TEXTURE0 + G.client_unit); return 1;
    case GL_ARRAY_BUFFER_BINDING: f[0] = (float)G.array_buffer; return 1;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: f[0] = (float)G.element_buffer; return 1;
    case GL_UNPACK_ALIGNMENT: f[0] = (float)G.unpack_alignment; return 1;
    case GL_UNPACK_ROW_LENGTH: f[0] = (float)G.unpack_row_length; return 1;
    case GL_PACK_ALIGNMENT: f[0] = 4; return 1;
    case GL_UNPACK_SKIP_ROWS: f[0] = (float)G.unpack_skip_rows; return 1;
    case GL_UNPACK_SKIP_PIXELS: f[0] = (float)G.unpack_skip_pixels; return 1;
    case GL_UNPACK_SWAP_BYTES: case GL_UNPACK_LSB_FIRST: case 0x0D00: case 0x0D01: case 0x0D02: case 0x0D03: case 0x0D04:
        f[0] = 0; return 1;   /* the pack state: nothing is ever read back */
    case GL_LIST_INDEX: f[0] = G.compiling ? (float)G.compiling->name : 0; return 1;
    case GL_LIST_MODE: f[0] = (float)G.list_mode; return 1;
    case GL_LIST_BASE: f[0] = (float)G.list_base; return 1;
    case GL_FOG_MODE: f[0] = (float)G.fog_mode; return 1;
    case GL_FOG_DENSITY: f[0] = G.fog_density; return 1;
    case GL_FOG_START: f[0] = G.fog_start; return 1;
    case GL_FOG_END: f[0] = G.fog_end; return 1;
    case GL_FOG_COLOR: memcpy(f, G.fog_color, 16); return 4;
    case GL_LIGHT_MODEL_AMBIENT: memcpy(f, G.scene_ambient, 16); return 4;
    case GL_POINT_SIZE: f[0] = G.point_size; return 1;
    case GL_LINE_WIDTH: f[0] = G.line_width; return 1;
    case GL_ALIASED_LINE_WIDTH_RANGE: case GL_ALIASED_POINT_SIZE_RANGE: f[0] = f[1] = 1; return 2;
    case GL_POLYGON_MODE: f[0] = f[1] = GL_FILL; return 2;
    default: break;
    }
    if (p == 0x0B20 || p == GL_DEPTH_TEST || p == GL_BLEND || p == GL_CULL_FACE || p == GL_LIGHTING || p == GL_TEXTURE_2D ||
        p == GL_FOG || p == GL_ALPHA_TEST || p == GL_SCISSOR_TEST || p == GL_COLOR_MATERIAL || (p >= GL_LIGHT0 && p < GL_LIGHT0 + MAX_LIGHTS)) {
        f[0] = GLFN(glIsEnabled)(p); return 1;
    }
    (void)ww; (void)wh; (void)ox; (void)oy; (void)s;
    if (G.log) gl_logf("glGet 0x%x: unknown, 0", p);
    f[0] = 0;
    return 1;
}

static int is_enum_value(GLenum p)
{
    return p == GL_MATRIX_MODE || p == GL_DEPTH_FUNC || p == GL_BLEND_SRC || p == GL_BLEND_DST || p == GL_BLEND_EQUATION ||
           p == GL_ALPHA_TEST_FUNC || p == GL_CULL_FACE_MODE || p == GL_FRONT_FACE || p == GL_SHADE_MODEL ||
           p == GL_ACTIVE_TEXTURE || p == GL_CLIENT_ACTIVE_TEXTURE || p == GL_FOG_MODE || p == GL_LIST_MODE || p == GL_POLYGON_MODE;
}

GLAPI(void, glGetFloatv, (GLenum p, GLfloat *out)) { float f[16]; int n = get_values(p, f); memcpy(out, f, (size_t)n * sizeof(float)); }
GLAPI(void, glGetDoublev, (GLenum p, GLdouble *out)) { float f[16]; int i, n = get_values(p, f); for (i = 0; i < n; i++) out[i] = f[i]; }
GLAPI(void, glGetIntegerv, (GLenum p, GLint *out))
{
    float f[16];
    int i, n = get_values(p, f);
    int colour = p == GL_CURRENT_COLOR || p == GL_COLOR_CLEAR_VALUE || p == GL_FOG_COLOR || p == GL_LIGHT_MODEL_AMBIENT;
    for (i = 0; i < n; i++) out[i] = colour ? (GLint)(f[i] * 2147483647.0f) : is_enum_value(p) ? (GLint)(GLuint)f[i] : (GLint)lroundf(f[i]);
}
GLAPI(void, glGetBooleanv, (GLenum p, GLboolean *out)) { float f[16]; int i, n = get_values(p, f); for (i = 0; i < n; i++) out[i] = f[i] != 0.0f; }
GLAPI(void, glGetPointerv, (GLenum p, GLvoid **out))
{
    (void)p;
    *out = NULL;
}
GLAPI(void, glGetLightfv, (GLenum light, GLenum p, GLfloat *v))
{
    Light *l;
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + MAX_LIGHTS) return;
    l = &G.light[light - GL_LIGHT0];
    switch (p) {
    case GL_AMBIENT: memcpy(v, l->ambient, 16); break;
    case GL_DIFFUSE: memcpy(v, l->diffuse, 16); break;
    case GL_SPECULAR: memcpy(v, l->specular, 16); break;
    case GL_POSITION: memcpy(v, l->position, 16); break;
    default: v[0] = 0; break;
    }
}
GLAPI(void, glGetMaterialfv, (GLenum face, GLenum p, GLfloat *v))
{
    (void)face;
    switch (p) {
    case GL_AMBIENT: memcpy(v, G.mat.ambient, 16); break;
    case GL_DIFFUSE: memcpy(v, G.mat.diffuse, 16); break;
    case GL_SPECULAR: memcpy(v, G.mat.specular, 16); break;
    case GL_EMISSION: memcpy(v, G.mat.emission, 16); break;
    case GL_SHININESS: v[0] = G.mat.shininess; break;
    default: v[0] = 0; break;
    }
}
GLAPI(GLint, glRenderMode, (GLenum mode)) { (void)mode; return 0; }
GLAPI(void, glSelectBuffer, (GLsizei n, GLuint *b)) { (void)n; (void)b; unsupported("selection (glSelectBuffer)"); }
GLAPI(void, glInitNames, (void)) { }
GLAPI(void, glPushName, (GLuint n)) { (void)n; }
GLAPI(void, glPopName, (void)) { }
GLAPI(void, glLoadName, (GLuint n)) { (void)n; }

/* ---- by name ------------------------------------------------------------------------------ */

#include "gl_pspexp_procs.h"

void *pspexp_gl_proc(const char *name)
{
    size_t i;
    if (!name) return NULL;
    for (i = 0; i < sizeof(GL_PROCS) / sizeof(GL_PROCS[0]); i++)
        if (!strcmp(GL_PROCS[i].name, name)) return GL_PROCS[i].fn;
    if (!strncmp(name, "gl", 2) && strncmp(name, "glX", 3)) gl_logf("no %s", name);
    return NULL;
}

#if !defined(__APPLE__)
/* Programs (and GLEW) that ask GLX or EGL for entry points get ours; other
 * names (glX*, egl*) from the real library. */
static void *real_proc(const char *sym, const char *name)
{
    void *(*fn)(const char *) = (void *(*)(const char *))(uintptr_t)dlsym(RTLD_NEXT, sym);
    return fn ? fn(name) : NULL;
}
static void *own_glx(const char *name);

__attribute__((visibility("default"))) void *glXGetProcAddress(const GLubyte *name)
{
    void *p = pspexp_gl_proc((const char *)name);
    if (!p) p = own_glx((const char *)name);
    return p ? p : strncmp((const char *)name, "gl", 2) || !strncmp((const char *)name, "glX", 3) ? real_proc("glXGetProcAddress", (const char *)name) : NULL;
}
__attribute__((visibility("default"))) void *glXGetProcAddressARB(const GLubyte *name) { return glXGetProcAddress(name); }

/* GLEW asks GLX for the current display and its version before it hands
 * out anything: a headless board has no X display, so ours answers with a
 * stand-in (zeroed: DefaultScreen() of it is 0) once our context exists. */
static char fake_display[4096] __attribute__((aligned(16)));
#define REAL_GLX(ret, name, ...) ((ret (*)(__VA_ARGS__))(uintptr_t)dlsym(RTLD_NEXT, name))
__attribute__((visibility("default"))) void *glXGetCurrentDisplay(void)
{
    void *(*real)(void) = REAL_GLX(void *, "glXGetCurrentDisplay", void);
    void *d = real ? real() : NULL;
    return d ? d : G.inited ? (void *)fake_display : NULL;
}
__attribute__((visibility("default"))) void *glXGetCurrentContext(void)
{
    void *(*real)(void) = REAL_GLX(void *, "glXGetCurrentContext", void);
    void *c = real ? real() : NULL;
    return c ? c : G.inited ? (void *)fake_display : NULL;
}
__attribute__((visibility("default"))) unsigned long glXGetCurrentDrawable(void)
{
    unsigned long (*real)(void) = REAL_GLX(unsigned long, "glXGetCurrentDrawable", void);
    unsigned long d = real ? real() : 0;
    return d ? d : G.inited ? 1 : 0;
}
__attribute__((visibility("default"))) int glXQueryVersion(void *dpy, int *major, int *minor)
{
    int (*real)(void *, int *, int *) = REAL_GLX(int, "glXQueryVersion", void *, int *, int *);
    if (dpy == (void *)fake_display) { if (major) *major = 1; if (minor) *minor = 4; return 1; }
    return real ? real(dpy, major, minor) : 0;
}
__attribute__((visibility("default"))) const char *glXQueryExtensionsString(void *dpy, int screen)
{
    const char *(*real)(void *, int) = REAL_GLX(const char *, "glXQueryExtensionsString", void *, int);
    if (dpy == (void *)fake_display) return "";
    return real ? real(dpy, screen) : "";
}
__attribute__((visibility("default"))) const char *glXGetClientString(void *dpy, int name)
{
    const char *(*real)(void *, int) = REAL_GLX(const char *, "glXGetClientString", void *, int);
    if (dpy == (void *)fake_display) return name == 2 /* GLX_VERSION */ ? "1.4" : "";
    return real ? real(dpy, name) : "";
}
__attribute__((visibility("default"))) const char *glXQueryServerString(void *dpy, int screen, int name)
{
    const char *(*real)(void *, int, int) = REAL_GLX(const char *, "glXQueryServerString", void *, int, int);
    if (dpy == (void *)fake_display) return name == 2 ? "1.4" : "";
    return real ? real(dpy, screen, name) : "";
}

static void *own_glx(const char *name)
{
    static const struct { const char *n; void *f; } T[] = {
        { "glXGetCurrentDisplay", (void *)(uintptr_t)&glXGetCurrentDisplay },
        { "glXGetCurrentContext", (void *)(uintptr_t)&glXGetCurrentContext },
        { "glXGetCurrentDrawable", (void *)(uintptr_t)&glXGetCurrentDrawable },
        { "glXQueryVersion", (void *)(uintptr_t)&glXQueryVersion },
        { "glXQueryExtensionsString", (void *)(uintptr_t)&glXQueryExtensionsString },
        { "glXGetClientString", (void *)(uintptr_t)&glXGetClientString },
        { "glXQueryServerString", (void *)(uintptr_t)&glXQueryServerString } };
    size_t i;
    for (i = 0; i < sizeof(T) / sizeof(T[0]); i++) if (!strcmp(T[i].n, name)) return T[i].f;
    return NULL;
}
__attribute__((visibility("default"))) void *eglGetProcAddress(const char *name)
{
    void *p = pspexp_gl_proc(name);
    return p ? p : strncmp(name, "gl", 2) ? real_proc("eglGetProcAddress", name) : NULL;
}
#endif
