/*
 * sdl-game: a plain SDL2 program with no knowledge of PSPExp, used to test
 * the sdl-pspexp shim (v0.4). It draws with the ordinary SDL_Render API:
 * a tiled background texture, bouncing sprites drawn scaled and flipped
 * with SDL_RenderCopyEx, primitives, a streaming texture updated every
 * frame, a paddle you move with the keyboard or a game controller, and a
 * tone through SDL_QueueAudio.
 *
 *   sdl-game [--frames N] [--size WxH] [--surface] [--target]
 *
 * --target draws the scene into a render-target texture and copies that to
 * the window (the integer-upscaling pattern of pixel-art ports).
 * --surface uses no renderer at all: it blits into the window surface and
 * calls SDL_UpdateWindowSurface, the pattern of older 2D ports, which the
 * shim handles by streaming the surface.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LOGICAL_W 640
#define LOGICAL_H 480

typedef struct Ball { float x, y, vx, vy; int flip; float scale; } Ball;

static SDL_Texture *make_sprite(SDL_Renderer *r, int size, Uint8 cr, Uint8 cg, Uint8 cb)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ABGR8888);
    SDL_Texture *t;
    int x, y;
    Uint32 *px = (Uint32 *)s->pixels;
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++) {
            int dx = x - size / 2, dy = y - size / 2;
            int inside = dx * dx + dy * dy <= (size / 2 - 1) * (size / 2 - 1);
            int eye = (dx + size / 6) * (dx + size / 6) + (dy + size / 6) * (dy + size / 6) <= (size / 10) * (size / 10);
            Uint32 c = 0;
            if (inside) c = eye ? 0xFF000000u : (0xFF000000u | ((Uint32)cb << 16) | ((Uint32)cg << 8) | cr);
            px[y * size + x] = c;
        }
    t = SDL_CreateTextureFromSurface(r, s);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(s);
    return t;
}

static SDL_Texture *make_tiles(SDL_Renderer *r)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 64, 64, 32, SDL_PIXELFORMAT_ABGR8888);
    SDL_Texture *t;
    int x, y;
    Uint32 *px = (Uint32 *)s->pixels;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) {
            int checker = ((x / 16) + (y / 16)) & 1;
            Uint8 g = checker ? 70 : 90, b = checker ? 40 : 60;
            if (x % 16 == 0 || y % 16 == 0) { g = 110; b = 80; }
            px[y * 64 + x] = 0xFF000000u | ((Uint32)b << 16) | ((Uint32)g << 8) | 30u;
        }
    t = SDL_CreateTextureFromSurface(r, s);
    SDL_FreeSurface(s);
    return t;
}

static void queue_tone(SDL_AudioDeviceID dev, int freq, int frames, double *phase)
{
    static Sint16 buf[4096 * 2];
    int i;
    for (i = 0; i < frames; i++) {
        Sint16 v = (Sint16)(sin(*phase) * 6000.0);
        *phase += 2.0 * M_PI * freq / 44100.0;
        buf[i * 2] = v; buf[i * 2 + 1] = v;
    }
    SDL_QueueAudio(dev, buf, (Uint32)frames * 4);
}

/* The renderer-less variant: software blits into the window surface. */
static int surface_main(SDL_Window *win, int max_frames)
{
    SDL_Surface *screen = SDL_GetWindowSurface(win);
    SDL_Surface *ball = SDL_CreateRGBSurfaceWithFormat(0, 40, 40, 32, SDL_PIXELFORMAT_ABGR8888);
    int frames = 0, running = 1, x, y;
    Uint32 t0 = SDL_GetTicks();
    Uint32 *px = (Uint32 *)ball->pixels;
    for (y = 0; y < 40; y++)
        for (x = 0; x < 40; x++) {
            int dx = x - 20, dy = y - 20;
            px[y * 40 + x] = dx * dx + dy * dy <= 361 ? 0xFF60C0FFu : 0;
        }
    while (running) {
        SDL_Event ev;
        SDL_Rect r;
        int i;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) running = 0;
        }
        screen = SDL_GetWindowSurface(win);
        SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 20, 20, 60));
        for (y = 0; y < screen->h; y += 32) {
            r.x = 0; r.y = y; r.w = screen->w; r.h = 1;
            SDL_FillRect(screen, &r, SDL_MapRGB(screen->format, 60, 60, 120));
        }
        for (i = 0; i < 12; i++) {
            r.x = (int)((screen->w - 40) * (0.5 + 0.5 * sin((frames + i * 37) * 0.03))); r.y = 30 + i * 30; r.w = 40; r.h = 40;
            SDL_BlitSurface(ball, NULL, screen, &r);
        }
        r.x = 20; r.y = screen->h - 40; r.w = 100 + frames % 200; r.h = 20;
        SDL_FillRect(screen, &r, SDL_MapRGB(screen->format, 250, 220, 80));
        SDL_UpdateWindowSurface(win);
        frames++;
        if (max_frames && frames >= max_frames) running = 0;
    }
    printf("sdl-game (surface): %d frames in %.1f s\n", frames, (SDL_GetTicks() - t0) / 1000.0);
    SDL_FreeSurface(ball);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

int main(int argc, char **argv)
{
    int max_frames = 0, win_w = 640, win_h = 480, i, frames = 0, surface_mode = 0, target_mode = 0;
    SDL_Texture *target = NULL;
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tiles, *sprites[3], *stream;
    SDL_AudioSpec want, have;
    SDL_AudioDeviceID dev;
    SDL_GameController *pad = NULL;
    Ball balls[24];
    float paddle_x = LOGICAL_W / 2.0f;
    double phase = 0;
    int running = 1;
    Uint32 t0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &win_w, &win_h);
        else if (!strcmp(argv[i], "--surface")) surface_mode = 1;
        else if (!strcmp(argv[i], "--target")) target_mode = 1;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "%s\n", SDL_GetError()); return 1; }
    win = SDL_CreateWindow("sdl-game", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, 0);
    if (surface_mode) return surface_main(win, max_frames);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_RenderSetLogicalSize(ren, LOGICAL_W, LOGICAL_H);

    tiles = make_tiles(ren);
    sprites[0] = make_sprite(ren, 48, 240, 80, 60);
    sprites[1] = make_sprite(ren, 48, 70, 200, 90);
    sprites[2] = make_sprite(ren, 48, 80, 120, 250);
    stream = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 128, 32);
    if (target_mode) target = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, LOGICAL_W / 2, LOGICAL_H / 2);

    SDL_zero(want);
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);
    if (SDL_NumJoysticks() > 0 && SDL_IsGameController(0)) pad = SDL_GameControllerOpen(0);

    srand(1);
    for (i = 0; i < 24; i++) {
        balls[i].x = (float)(rand() % (LOGICAL_W - 48)); balls[i].y = (float)(rand() % (LOGICAL_H - 120));
        balls[i].vx = (float)((rand() % 300) - 150) / 60.0f; balls[i].vy = (float)((rand() % 300) - 150) / 60.0f;
        balls[i].flip = rand() & 1; balls[i].scale = 0.5f + (rand() % 100) / 100.0f;
    }

    t0 = SDL_GetTicks();
    while (running) {
        SDL_Event ev;
        const Uint8 *keys;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) running = 0;
            if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(ev.cdevice.which);
        }
        keys = SDL_GetKeyboardState(NULL);
        if (keys[SDL_SCANCODE_LEFT]) paddle_x -= 6;
        if (keys[SDL_SCANCODE_RIGHT]) paddle_x += 6;
        if (pad) {
            int ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
            if (abs(ax) > 8000) paddle_x += ax / 32767.0f * 8;
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) paddle_x -= 6;
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) paddle_x += 6;
        }
        if (paddle_x < 40) paddle_x = 40;
        if (paddle_x > LOGICAL_W - 40) paddle_x = LOGICAL_W - 40;

        for (i = 0; i < 24; i++) {
            Ball *b = &balls[i];
            float sz = 48 * b->scale;
            b->x += b->vx; b->y += b->vy;
            if (b->x < 0 || b->x + sz > LOGICAL_W) { b->vx = -b->vx; b->flip ^= 1; b->x += b->vx; }
            if (b->y < 0 || b->y + sz > LOGICAL_H - 60) { b->vy = -b->vy; b->y += b->vy; }
        }

        /* audio: keep ~100 ms queued; the tone follows the paddle */
        if (dev && SDL_GetQueuedAudioSize(dev) < 44100 / 10 * 4) queue_tone(dev, 220 + (int)paddle_x, 2205, &phase);

        /* streaming texture: a moving gradient bar */
        {
            void *px; int pitch, x, y;
            if (SDL_LockTexture(stream, NULL, &px, &pitch) == 0) {
                for (y = 0; y < 32; y++)
                    for (x = 0; x < 128; x++) {
                        int v = ((x + frames * 2) % 128) * 2;
                        ((Uint32 *)((Uint8 *)px + y * pitch))[x] = 0xFF000000u | ((Uint32)(255 - v) << 16) | ((Uint32)v << 8) | 128u;
                    }
                SDL_UnlockTexture(stream);
            }
        }

        if (target) { SDL_SetRenderTarget(ren, target); SDL_RenderSetScale(ren, 0.5f, 0.5f); }
        SDL_SetRenderDrawColor(ren, 10, 10, 30, 255);
        SDL_RenderClear(ren);
        {
            int x, y;
            for (y = 0; y < LOGICAL_H - 60; y += 64)
                for (x = 0; x < LOGICAL_W; x += 64) {
                    SDL_Rect d = { x, y, 64, 64 };
                    SDL_RenderCopy(ren, tiles, NULL, &d);
                }
        }
        for (i = 0; i < 24; i++) {
            Ball *b = &balls[i];
            SDL_Rect d = { (int)b->x, (int)b->y, (int)(48 * b->scale), (int)(48 * b->scale) };
            SDL_RenderCopyEx(ren, sprites[i % 3], NULL, &d, 0, NULL, b->flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
        }
        {
            SDL_Rect bar = { 0, LOGICAL_H - 60, LOGICAL_W, 60 };
            SDL_Rect paddle = { (int)paddle_x - 40, LOGICAL_H - 50, 80, 12 };
            SDL_Rect sd = { 20, LOGICAL_H - 36, 256, 24 };
            SDL_SetRenderDrawColor(ren, 40, 40, 60, 255);
            SDL_RenderFillRect(ren, &bar);
            SDL_SetRenderDrawColor(ren, 250, 220, 80, 255);
            SDL_RenderFillRect(ren, &paddle);
            SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
            SDL_RenderDrawRect(ren, &paddle);
            SDL_RenderCopy(ren, stream, NULL, &sd);
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren, 255, 60, 60, 120);
            SDL_RenderDrawLine(ren, (int)paddle_x, LOGICAL_H - 60, (int)paddle_x, 0);
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        }
        if (target) {
            SDL_RenderSetScale(ren, 1.0f, 1.0f);
            SDL_SetRenderTarget(ren, NULL);
            SDL_RenderCopy(ren, target, NULL, NULL);
        }
        SDL_RenderPresent(ren);
        frames++;
        if (max_frames && frames >= max_frames) running = 0;
    }
    printf("sdl-game: %d frames in %.1f s\n", frames, (SDL_GetTicks() - t0) / 1000.0);
    if (dev) SDL_CloseAudioDevice(dev);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
