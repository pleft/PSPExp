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
 *   GLDEMO_ONE=degrees [GLDEMO_TX=x]   a lighting check instead: one sphere turned
 *                              about y (then moved along x), lit from the camera:
 *                              it must look the same at any angle
 */
#include <SDL.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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

static float torus_v[(TORUS_RINGS + 1) * (TORUS_SIDES + 1)][6];   /* normal, position */
static unsigned short torus_i[TORUS_RINGS * TORUS_SIDES * 6];

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
    SDL_GL_SetSwapInterval(1);
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
