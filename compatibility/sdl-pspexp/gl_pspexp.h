/*
 * Between the SDL layer (sdl_pspexp.c) and its OpenGL part (gl_pspexp.c).
 */
#ifndef GL_PSPEXP_H
#define GL_PSPEXP_H

#include "../../host/libpspexp/pspexp.h"

/* The SDL layer's side. */
PspExpContext *shim_gl_frame(void);   /* the context with a frame open, or NULL: no PSP, or no 3D */
void shim_gl_geometry(int *win_w, int *win_h, float *scale, int *ox, int *oy);
uint16_t shim_gl_handle(void);        /* a resource handle of our own */
void shim_gl_flush(void);             /* the frame so far to the PSP now (no game frame: a program still loading) */
int shim_gl_reconnects(void);         /* changes when the PSP's state was lost */
void shim_gl_log(const char *fmt, ...);

/* The OpenGL part's side. */
void  pspexp_gl_init(void);           /* a context was made current */
void  pspexp_gl_refused(void);        /* a context was refused: glGetError says so once (programs check that way) */
void  pspexp_gl_frame_done(void);     /* SDL_GL_SwapWindow: before the frame goes */
void *pspexp_gl_proc(const char *name);   /* our function by name, or NULL */
int   pspexp_gl_extension(const char *name);

#endif
