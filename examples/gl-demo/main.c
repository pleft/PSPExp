/*
 * gl-demo: a plain SDL2 + OpenGL 1.x program, for the SDL layer's OpenGL part
 * (compatibility/sdl-pspexp/gl_pspexp.c). Nothing in it knows about the PSP.
 *
 * A lit, textured cube (immediate mode), a torus (vertex arrays and
 * glDrawElements), a ring of spheres from a display list, a see-through
 * quad (blending), fog, and a 2D overlay (glOrtho) on top.
 *
 *   gl-demo [frames [first]]   quit after that many frames (0: run until closed);
 *                              start the animation at frame `first`
 *   GLDEMO_SHADER=1            a GLSL program (a matrix uniform, texture x colour,
 *                              an alpha discard) instead: GL 2's path
 *   GLDEMO_ONE=degrees [GLDEMO_TX=x]   a lighting check instead: one sphere turned
 *                              about y (then moved along x), lit from the camera:
 *                              it must look the same at any angle
 *   GLDEMO_CUBE=1              a reflection instead: a torus from buffer objects, lit and
 *                              textured, a cube map added along the reflected eye vector
 *                              (each face its own colour, a white band along its first
 *                              row, a dark one along its first column)
 *   GLDEMO_SPHERE=1            the same torus through OpenGL 1.x's sphere map
 *                              (glTexGen GL_SPHERE_MAP; a texture of four coloured quarters)
 *   GLDEMO_MULTI=1             multitexturing (fixed function): a checker on unit 0, a light
 *                              gradient on unit 1; left modulated, right GL_COMBINE x2 (Quake's
 *                              overbright lightmaps); top immediate mode, bottom client arrays
 *   GLDEMO_UV=offset           a long checkered floor seen low, its texture coordinates plus
 *                              `offset` (whole numbers: the same picture on a repeating texture;
 *                              how large coordinates fare on the GE: Quake's long ledges)
 *   GLDEMO_READ=file.ppm       the last frame's pixels written out (a reference, with a
 *                              real OpenGL)
 */
#define GL_GLEXT_PROTOTYPES   /* GL 2's functions declared (GLDEMO_SHADER) */
#include <SDL.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TORUS_RINGS 24
#define TORUS_SIDES 12

static GLuint make_texture(void)
{
    static unsigned char px[64 * 64 * 3];
    GLuint t;
    int x, y;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) {
            int on = ((x / 8) ^ (y / 8)) & 1;
            unsigned char *p = px + (y * 64 + x) * 3;
            p[0] = on ? 240 : 40; p[1] = on ? 200 : 60; p[2] = on ? 60 : 160;
        }
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 64, 64, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
    return t;
}

/* GL 2: a textured, coloured quad through a program */
static void shader_frame(GLuint tex, int n, int w, int h)
{
    static GLuint prog;
    static const char *vs =
        "uniform mat4 uMvp;\n"
        "attribute vec3 aPos;\n"
        "attribute vec4 aColor;\n"
        "attribute vec2 aTex;\n"
        "varying vec2 vTex;\n"
        "varying vec4 vColor;\n"
        "void main() { vTex = aTex; vColor = aColor; gl_Position = uMvp * vec4(aPos, 1.0); }\n";
    static const char *fs =
        "uniform sampler2D uTex;\n"
        "uniform int uDiscard;\n"
        "varying vec2 vTex;\n"
        "varying vec4 vColor;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(uTex, vTex) * vColor;\n"
        "    if (uDiscard != 0 && gl_FragColor.a < 0.5) discard;\n"
        "}\n";
    static const float quad[4][9] = {   /* x y z  r g b a  u v */
        { -1, -1, 0,  1, 0.3f, 0.3f, 1,  0, 1 }, {  1, -1, 0,  0.3f, 1, 0.3f, 1,  1, 1 },
        {  1,  1, 0,  0.3f, 0.3f, 1, 1,  1, 0 }, { -1,  1, 0,  1, 1, 1, 1,  0, 0 } };
    float a = n * 0.02f, c = cosf(a), s = sinf(a), asp = (float)w / (float)h;
    float mvp[16] = { c / asp, s, 0, 0,  -s / asp, c, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    if (!prog) {
        GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(v, 1, &vs, NULL); glCompileShader(v);
        glShaderSource(f, 1, &fs, NULL); glCompileShader(f);
        prog = glCreateProgram();
        glAttachShader(prog, v); glAttachShader(prog, f);
        glBindAttribLocation(prog, 0, "aPos");
        glBindAttribLocation(prog, 1, "aColor");
        glBindAttribLocation(prog, 2, "aTex");
        glLinkProgram(prog);
    }
    glViewport(0, 0, w, h);
    glClearColor(0.1f, 0.12f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMvp"), 1, GL_FALSE, mvp);
    glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
    glUniform1i(glGetUniformLocation(prog, "uDiscard"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), &quad[0][0]);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 9 * sizeof(float), &quad[0][3]);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 9 * sizeof(float), &quad[0][7]);
    {   /* a strip through indices: glDrawElements */
        static const GLushort order[4] = { 0, 1, 3, 2 };
        glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, order);
    }
    glDisableVertexAttribArray(0); glDisableVertexAttribArray(1); glDisableVertexAttribArray(2);
    glUseProgram(0);
}

static float torus_v[(TORUS_RINGS + 1) * (TORUS_SIDES + 1)][6];   /* normal, position */
static unsigned short torus_i[TORUS_RINGS * TORUS_SIDES * 6];

static void cube(void)
{
    static const float n[6][3] = { {0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0} };
    static const float v[6][4][3] = {
        { {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1} },
        { { 1,-1,-1}, {-1,-1,-1}, {-1, 1,-1}, { 1, 1,-1} },
        { { 1,-1, 1}, { 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1} },
        { {-1,-1,-1}, {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1} },
        { {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1}, {-1, 1,-1} },
        { {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1}, {-1,-1, 1} } };
    static const float t[4][2] = { {0,1}, {1,1}, {1,0}, {0,0} };
    int f, k;
    glBegin(GL_QUADS);
    for (f = 0; f < 6; f++) {
        glNormal3fv(n[f]);
        for (k = 0; k < 4; k++) { glTexCoord2fv(t[k]); glVertex3fv(v[f][k]); }
    }
    glEnd();
}

static void make_torus(void)
{
    int r, s, k = 0;
    for (r = 0; r <= TORUS_RINGS; r++)
        for (s = 0; s <= TORUS_SIDES; s++) {
            float a = r * 6.2831853f / TORUS_RINGS, b = s * 6.2831853f / TORUS_SIDES;
            float *v = torus_v[r * (TORUS_SIDES + 1) + s];
            v[0] = cosf(a) * cosf(b); v[1] = sinf(a) * cosf(b); v[2] = sinf(b);
            v[3] = cosf(a) * (1.0f + 0.35f * cosf(b)); v[4] = sinf(a) * (1.0f + 0.35f * cosf(b)); v[5] = 0.35f * sinf(b);
        }
    for (r = 0; r < TORUS_RINGS; r++)
        for (s = 0; s < TORUS_SIDES; s++) {
            unsigned short a = (unsigned short)(r * (TORUS_SIDES + 1) + s), b = (unsigned short)(a + TORUS_SIDES + 1);
            torus_i[k++] = a; torus_i[k++] = b; torus_i[k++] = (unsigned short)(a + 1);
            torus_i[k++] = (unsigned short)(a + 1); torus_i[k++] = b; torus_i[k++] = (unsigned short)(b + 1);
        }
}

static GLuint make_ring_list(void)
{
    GLuint l = glGenLists(1);
    int i, j, k;
    glNewList(l, GL_COMPILE);
    for (i = 0; i < 8; i++) {
        glPushMatrix();
        glRotatef(i * 45.0f, 0, 1, 0);
        glTranslatef(3.2f, 0, 0);
        glColor3f(0.4f + 0.075f * i, 0.9f - 0.1f * i, 0.5f);
        for (j = 0; j < 6; j++) {   /* a small sphere, latitude strips */
            float a0 = -1.5708f + j * 3.14159f / 6, a1 = a0 + 3.14159f / 6;
            glBegin(GL_TRIANGLE_STRIP);
            for (k = 0; k <= 10; k++) {
                float b = k * 6.2831853f / 10;
                glNormal3f(cosf(a0) * cosf(b), sinf(a0), cosf(a0) * sinf(b));   /* counter-clockwise from outside */
                glVertex3f(0.3f * cosf(a0) * cosf(b), 0.3f * sinf(a0), 0.3f * cosf(a0) * sinf(b));
                glNormal3f(cosf(a1) * cosf(b), sinf(a1), cosf(a1) * sinf(b));
                glVertex3f(0.3f * cosf(a1) * cosf(b), 0.3f * sinf(a1), 0.3f * cosf(a1) * sinf(b));
            }
            glEnd();
        }
        glPopMatrix();
    }
    glEndList();
    return l;
}

/* GL 2: a torus reflecting a cube map, lit and textured (openMrac's car bodies are drawn so) */
static void reflect_frame(GLuint tex, int n, int w, int h)
{
    static GLuint prog, cubemap, vbo, ibo;
    static const char *vs =
        "uniform mat4 uMvp;\n"
        "uniform mat4 uMv;\n"
        "uniform mat3 uNorm;\n"
        "attribute vec3 aPos;\n"
        "attribute vec3 aNormal;\n"
        "varying vec2 vTex;\n"
        "varying vec3 vN;\n"
        "varying vec3 vRay;\n"
        "void main() {\n"
        "    vTex = aPos.xy * 0.5;\n"
        "    vN = uNorm * aNormal;\n"
        "    gl_Position = uMvp * vec4(aPos, 1.0);\n"
        "    vec3 eye = vec3(uMv * vec4(aPos, 1.0));\n"
        "    vRay = normalize(reflect(eye, vN));\n"
        "}\n";
    static const char *fs =
        "uniform sampler2D uTex;\n"
        "uniform samplerCube uCube;\n"
        "uniform vec4 uLight;\n"
        "varying vec2 vTex;\n"
        "varying vec3 vN;\n"
        "varying vec3 vRay;\n"
        "void main() {\n"
        "    vec4 shade = vec4(vec3(max(0.0, dot(vN, uLight.xyz))), 1.0) * vec4(0.35, 0.35, 0.35, 1.0) + vec4(0.08, 0.08, 0.08, 0.0);\n"
        "    shade.rgb = clamp(shade.rgb, 0.0, 1.0);\n"
        "    shade.a = 1.0;\n"
        "    vec4 sky = textureCube(uCube, vRay);\n"
        "    gl_FragColor = texture2D(uTex, vTex) * shade + vec4(sky.rgb * 0.7, 0.0);\n"
        "}\n";
    float asp = (float)w / (float)h, f = 2.4142f, zn = 0.5f, zf = 20.0f;
    float proj[16] = { f / asp, 0, 0, 0,  0, f, 0, 0,  0, 0, (zf + zn) / (zn - zf), -1,  0, 0, 2 * zf * zn / (zn - zf), 0 };
    float mv[16], mvp[16], nm[9], a = n * 0.02f, ca = cosf(a), sa = sinf(a), cb = 0.5f, sb = 0.866f;
    float light[4] = { 0.3f, 0.5f, 0.81f, 0 };
    int i, j, k;
    if (!prog) {
        static const unsigned char col[6][3] = { { 220, 40, 40 }, { 40, 200, 200 }, { 60, 220, 60 }, { 200, 40, 200 }, { 50, 70, 230 }, { 230, 220, 50 } };
        static unsigned char px[32 * 32 * 3];
        GLuint v = glCreateShader(GL_VERTEX_SHADER), fr = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(v, 1, &vs, NULL); glCompileShader(v);
        glShaderSource(fr, 1, &fs, NULL); glCompileShader(fr);
        prog = glCreateProgram();
        glAttachShader(prog, v); glAttachShader(prog, fr);
        glBindAttribLocation(prog, 0, "aPos");
        glBindAttribLocation(prog, 1, "aNormal");
        glLinkProgram(prog);
        glGenTextures(1, &cubemap);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cubemap);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (k = 0; k < 6; k++) {
            for (j = 0; j < 32; j++)
                for (i = 0; i < 32; i++) {
                    unsigned char *p = px + (j * 32 + i) * 3;
                    int c;
                    for (c = 0; c < 3; c++) p[c] = j < 5 ? 255 : i < 5 ? col[k][c] / 4 : col[k][c];
                }
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + k, 0, GL_RGB, 32, 32, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
        }
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(torus_v), torus_v, GL_STATIC_DRAW);
        glGenBuffers(1, &ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(torus_i), torus_i, GL_STATIC_DRAW);
    }
    /* the modelview: turned about x by 60 degrees, then about y by a, 4 away */
    {
        float r[9] = { ca, 0, -sa,  sa * sb, cb, ca * sb,  sa * cb, -sb, ca * cb };   /* columns */
        memset(mv, 0, sizeof(mv));
        for (k = 0; k < 3; k++) for (j = 0; j < 3; j++) { mv[k * 4 + j] = r[k * 3 + j]; nm[k * 3 + j] = r[k * 3 + j]; }
        mv[14] = -4.0f; mv[15] = 1.0f;
    }
    for (k = 0; k < 4; k++) for (j = 0; j < 4; j++) {
        float t = 0;
        for (i = 0; i < 4; i++) t += proj[i * 4 + j] * mv[k * 4 + i];
        mvp[k * 4 + j] = t;
    }
    glViewport(0, 0, w, h);
    glClearColor(0.1f, 0.12f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMvp"), 1, GL_FALSE, mvp);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMv"), 1, GL_FALSE, mv);
    glUniformMatrix3fv(glGetUniformLocation(prog, "uNorm"), 1, GL_FALSE, nm);
    glUniform4fv(glGetUniformLocation(prog, "uLight"), 1, light);
    glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
    glUniform1i(glGetUniformLocation(prog, "uCube"), 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cubemap);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (const void *)(3 * sizeof(float)));
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (const void *)0);
    glDrawElements(GL_TRIANGLES, TORUS_RINGS * TORUS_SIDES * 6, GL_UNSIGNED_SHORT, (const void *)0);
    glDisableVertexAttribArray(0); glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

/* GL 1.x: the torus with a sphere map (glTexGen), the texture four coloured quarters */
static void sphere_frame(int n, int w, int h)
{
    static GLuint tex;
    float a = n * 0.02f;
    if (!tex) {
        static unsigned char px[64 * 64 * 3];
        static const unsigned char q[4][3] = { { 220, 40, 40 }, { 60, 220, 60 }, { 50, 70, 230 }, { 230, 220, 50 } };
        int x, y, c;
        for (y = 0; y < 64; y++)
            for (x = 0; x < 64; x++)
                for (c = 0; c < 3; c++)
                    px[(y * 64 + x) * 3 + c] = (x > 29 && x < 34) || (y > 29 && y < 34) ? 255 : q[(y >= 32) * 2 + (x >= 32)][c];
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 64, 64, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
    }
    glViewport(0, 0, w, h);
    glClearColor(0.1f, 0.12f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-0.2071 * w / h, 0.2071 * w / h, -0.2071, 0.2071, 0.5, 20.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, 0, -4);
    glRotatef(60, 1, 0, 0);
    glRotatef(a * 57.29578f, 0, 1, 0);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
    glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
    glEnable(GL_TEXTURE_GEN_S);
    glEnable(GL_TEXTURE_GEN_T);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glNormalPointer(GL_FLOAT, 6 * sizeof(float), &torus_v[0][0]);
    glVertexPointer(3, GL_FLOAT, 6 * sizeof(float), &torus_v[0][3]);
    glDrawElements(GL_TRIANGLES, TORUS_RINGS * TORUS_SIDES * 6, GL_UNSIGNED_SHORT, torus_i);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisable(GL_TEXTURE_GEN_S);
    glDisable(GL_TEXTURE_GEN_T);
    glDisable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

/* GL 1.3: two texture units, the second a lightmap */
/* GLDEMO_UV: a floor 4 units wide and 80 long under a camera 0.5 above it, 1 repeat a unit */
static void uv_frame(GLuint checker, float off)
{
    glClearColor(0.1f, 0.1f, 0.15f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING); glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_FOG);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, checker);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glLoadIdentity();
    glRotatef(8, 1, 0, 0);
    glTranslatef(0, -0.5f, 0);
    glBegin(GL_QUADS);
    glTexCoord2f(off - 2, off + 2); glVertex3f(-2, 0, 2);
    glTexCoord2f(off + 2, off + 2); glVertex3f(2, 0, 2);
    glTexCoord2f(off + 2, off - 78); glVertex3f(2, 0, -78);
    glTexCoord2f(off - 2, off - 78); glVertex3f(-2, 0, -78);
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

static void multi_frame(GLuint checker, int w, int h)
{
    static GLuint light;
    int half;
    if (!light) {
        static unsigned char px[32 * 32 * 3];
        int x, y;
        for (y = 0; y < 32; y++)
            for (x = 0; x < 32; x++) {
                unsigned char *p = px + (y * 32 + x) * 3;
                p[0] = (unsigned char)(x * 8); p[1] = (unsigned char)(y * 8); p[2] = 96;
            }
        glGenTextures(1, &light);
        glBindTexture(GL_TEXTURE_2D, light);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 32, 32, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
    }
    glViewport(0, 0, w, h);
    glClearColor(0.1f, 0.12f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 2, 0, 2, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glColor4f(1, 1, 1, 1);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, checker);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glActiveTexture(GL_TEXTURE1);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, light);
    for (half = 0; half < 2; half++) {
        float x0 = (float)half;
        if (half == 0) glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        else {
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_TEXTURE);
            glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 2.0f);
        }
        /* top: immediate mode */
        glBegin(GL_QUADS);
        glMultiTexCoord2f(GL_TEXTURE0, 0, 0); glMultiTexCoord2f(GL_TEXTURE1, 0, 0); glVertex2f(x0 + 0.05f, 1.05f);
        glMultiTexCoord2f(GL_TEXTURE0, 2, 0); glMultiTexCoord2f(GL_TEXTURE1, 1, 0); glVertex2f(x0 + 0.95f, 1.05f);
        glMultiTexCoord2f(GL_TEXTURE0, 2, 2); glMultiTexCoord2f(GL_TEXTURE1, 1, 1); glVertex2f(x0 + 0.95f, 1.95f);
        glMultiTexCoord2f(GL_TEXTURE0, 0, 2); glMultiTexCoord2f(GL_TEXTURE1, 0, 1); glVertex2f(x0 + 0.05f, 1.95f);
        glEnd();
        {   /* bottom: client arrays */
            float pos[8] = { x0 + 0.05f, 0.05f, x0 + 0.95f, 0.05f, x0 + 0.95f, 0.95f, x0 + 0.05f, 0.95f };
            float t0[8] = { 0, 0, 2, 0, 2, 2, 0, 2 }, t1[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };
            glEnableClientState(GL_VERTEX_ARRAY);
            glVertexPointer(2, GL_FLOAT, 0, pos);
            glClientActiveTexture(GL_TEXTURE0);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, 0, t0);
            glClientActiveTexture(GL_TEXTURE1);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, 0, t1);
            glDrawArrays(GL_QUADS, 0, 4);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glClientActiveTexture(GL_TEXTURE0);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glDisableClientState(GL_VERTEX_ARRAY);
        }
    }
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

static void read_frame(const char *path, int w, int h)
{
    unsigned char *px = (unsigned char *)malloc((size_t)w * (size_t)h * 3);
    FILE *f;
    int y;
    if (!px || !(f = fopen(path, "wb"))) { free(px); return; }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = h - 1; y >= 0; y--) fwrite(px + (size_t)y * (size_t)w * 3, 3, (size_t)w, f);
    fclose(f);
    free(px);
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 0, first = argc > 2 ? atoi(argv[2]) : 0, n = 0, running = 1, w = 640, h = 360;
    SDL_Window *win;
    SDL_GLContext gl;
    GLuint tex, ring;
    float light_dir[4] = { 0.4f, 0.8f, 1.0f, 0.0f }, white[4] = { 1, 1, 1, 1 }, fog[4] = { 0.1f, 0.12f, 0.2f, 1 };
    float spec[4] = { 0.6f, 0.6f, 0.6f, 1 };
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    win = SDL_CreateWindow("gl-demo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_OPENGL);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    gl = SDL_GL_CreateContext(win);
    if (!gl) { fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(getenv("GLDEMO_READ") ? 0 : 1);   /* a reference frame: no wait for a display that may be asleep */
    printf("GL_RENDERER: %s\nGL_VERSION: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    tex = make_texture();
    make_torus();
    ring = make_ring_list();
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glShadeModel(GL_SMOOTH);
    glEnable(GL_LIGHT0);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spec);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 24.0f);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, white);
    glLightfv(GL_LIGHT0, GL_SPECULAR, white);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_COLOR_MATERIAL);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 8.0f);
    glFogf(GL_FOG_END, 16.0f);
    glFogfv(GL_FOG_COLOR, fog);
    glClearColor(fog[0], fog[1], fog[2], 1);

    while (running) {
        SDL_Event ev;
        float t = (n + first) / 60.0f;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)) running = 0;
        SDL_GL_GetDrawableSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glFrustum(-0.1 * w / h, 0.1 * w / h, -0.1, 0.1, 0.2, 40.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        if (getenv("GLDEMO_UV")) {
            uv_frame(tex, (float)atof(getenv("GLDEMO_UV")));
            if (n + 1 == frames && getenv("GLDEMO_READ")) read_frame(getenv("GLDEMO_READ"), w, h);
            SDL_GL_SwapWindow(win);
            if (++n == frames) running = 0;
            continue;
        }
        if (getenv("GLDEMO_MULTI")) {
            multi_frame(tex, w, h);
            if (n + 1 == frames && getenv("GLDEMO_READ")) read_frame(getenv("GLDEMO_READ"), w, h);
            SDL_GL_SwapWindow(win);
            if (++n == frames) running = 0;
            continue;
        }
        if (getenv("GLDEMO_SPHERE")) {
            sphere_frame(n + first, w, h);
            if (n + 1 == frames && getenv("GLDEMO_READ")) read_frame(getenv("GLDEMO_READ"), w, h);
            SDL_GL_SwapWindow(win);
            if (++n == frames) running = 0;
            continue;
        }
        if (getenv("GLDEMO_CUBE")) {
            reflect_frame(tex, n + first, w, h);
            if (n + 1 == frames && getenv("GLDEMO_READ")) read_frame(getenv("GLDEMO_READ"), w, h);
            SDL_GL_SwapWindow(win);
            if (++n == frames) running = 0;
            continue;
        }
        if (getenv("GLDEMO_SHADER")) {
            shader_frame(tex, n + first, w, h);
            SDL_GL_SwapWindow(win);
            if (++n == frames) running = 0;
            continue;
        }
        if (getenv("GLDEMO_ONE")) {   /* a lighting test: one sphere turned by GLDEMO_ONE degrees about y, lit from the camera */
            static const float front[4] = { 0, 0, 1, 0 };
            int j, k;
            glLightfv(GL_LIGHT0, GL_POSITION, front);
            glTranslatef(0, 0, -5);
            glRotatef((float)atof(getenv("GLDEMO_ONE")), 0, 1, 0);
            if (getenv("GLDEMO_TX")) glTranslatef((float)atof(getenv("GLDEMO_TX")), 0, 0);
            glEnable(GL_LIGHTING);
            glDisable(GL_FOG);
            glColor3f(0.9f, 0.9f, 0.9f);
            for (j = 0; j < 12; j++) {
                float a0 = -1.5708f + j * 3.14159f / 12, a1 = a0 + 3.14159f / 12;
                glBegin(GL_TRIANGLE_STRIP);
                for (k = 0; k <= 24; k++) {
                    float b = k * 6.2831853f / 24;
                    glNormal3f(cosf(a0) * cosf(b), sinf(a0), cosf(a0) * sinf(b));
                    glVertex3f(1.5f * cosf(a0) * cosf(b), 1.5f * sinf(a0), 1.5f * cosf(a0) * sinf(b));
                    glNormal3f(cosf(a1) * cosf(b), sinf(a1), cosf(a1) * sinf(b));
                    glVertex3f(1.5f * cosf(a1) * cosf(b), 1.5f * sinf(a1), 1.5f * cosf(a1) * sinf(b));
                }
                glEnd();
            }
            SDL_GL_SwapWindow(win);
            if (++n == frames) running = 0;
            continue;
        }
        glLightfv(GL_LIGHT0, GL_POSITION, light_dir);   /* in eye space: fixed to the camera */
        glTranslatef(0, -0.3f, -8.0f);
        glRotatef(20, 1, 0, 0);
        glEnable(GL_LIGHTING);
        glEnable(GL_FOG);

        /* the cube: textured, lit, immediate mode */
        glPushMatrix();
        glRotatef(t * 50, 0, 1, 0);
        glRotatef(t * 30, 1, 0, 0);
        glColor3f(1, 1, 1);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, tex);
        cube();
        glDisable(GL_TEXTURE_2D);
        glPopMatrix();

        /* the torus: vertex arrays */
        glPushMatrix();
        glTranslatef(0, 0, 0);
        glRotatef(90, 1, 0, 0);
        glRotatef(t * 20, 0, 0, 1);
        glScalef(1.6f, 1.6f, 1.6f);
        glColor3f(0.9f, 0.3f, 0.3f);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_NORMAL_ARRAY);
        glNormalPointer(GL_FLOAT, 6 * sizeof(float), &torus_v[0][0]);
        glVertexPointer(3, GL_FLOAT, 6 * sizeof(float), &torus_v[0][3]);
        glDrawElements(GL_TRIANGLES, TORUS_RINGS * TORUS_SIDES * 6, GL_UNSIGNED_SHORT, torus_i);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);
        glPopMatrix();

        /* the ring of spheres: a display list */
        glPushMatrix();
        glRotatef(-t * 35, 0, 1, 0);
        glCallList(ring);
        glPopMatrix();

        /* a see-through floor, unlit, blended */
        glDisable(GL_LIGHTING);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);
        glColor4f(0.3f, 0.6f, 1.0f, 0.35f);
        glBegin(GL_QUADS);
        glVertex3f(-6, -1.8f, -6); glVertex3f(6, -1.8f, -6); glVertex3f(6, -1.8f, 6); glVertex3f(-6, -1.8f, 6);
        glEnd();
        glEnable(GL_CULL_FACE);
        glDisable(GL_FOG);

        /* 2D on top: a bar that grows with the frames (y down: clockwise, not culled) */
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, w, h, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glColor4f(0, 0, 0, 0.5f);
        glRectf(10, 10, 10 + 200, 26);
        glColor4f(1, 0.8f, 0.2f, 1);
        glRectf(12, 12, 12 + (float)(((n + first) % 120) * 196 / 119), 24);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);

        SDL_GL_SwapWindow(win);
        if (++n == frames) running = 0;
        if (glGetError() != GL_NO_ERROR) fprintf(stderr, "GL error at frame %d\n", n);
    }
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
