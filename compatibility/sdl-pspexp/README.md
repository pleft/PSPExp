# sdl-pspexp: any SDL2 program on the PSP (v0.4)

`libsdl-pspexp` is loaded into an **unmodified** SDL2 program and turns the
PSP into its display, speaker and controller. The program keeps running on
the host exactly as before (its own window, its own audio device); every
`SDL_Render*` call is mirrored into PSPExp commands, the audio the program
produces is streamed to the PSP, and the PSP's controls come back as a game
controller and/or keyboard events.

```sh
scripts/pspexp-run <program> [args...]            # PSP over USB (default)
scripts/pspexp-run --tcp:127.0.0.1:3838 <program> # runtime under PPSSPP
scripts/pspexp-run --null <program>               # no PSP: just check it runs
PSPEXP=sdl scripts/pspexp-run <program>           # PSPExp SDL executor in a 2nd window
```

The launcher sets `DYLD_INSERT_LIBRARIES` (macOS) or `LD_PRELOAD` (Linux)
to `build/host/libsdl-pspexp.{dylib,so}`. On macOS the library carries a
`__DATA,__interpose` table, so it also works with SDL2 linked as a dylib
from Homebrew; programs with the hardened runtime and no
`allow-dyld-environment-variables` entitlement cannot be injected (build
them yourself, or use the Linux route).

Try it with the test game, a plain SDL2 program that knows nothing of
PSPExp (`examples/sdl-game`): logical size 640x480, scaled and flipped
sprites, primitives, a streaming texture, queued audio, keyboard and game
controller input.

```sh
scripts/pspexp-run build/host/sdl-game               # renderer path
scripts/pspexp-run build/host/sdl-game --surface     # window-surface path
```

## What is mirrored

| SDL2 | PSPExp |
|---|---|
| `SDL_CreateWindow`, `SDL_CreateRenderer`, `SDL_CreateWindowAndRenderer` | connect to the PSP (once) |
| `SDL_RenderSetLogicalSize`, `SDL_RenderSetViewport`, `SDL_RenderSetClipRect` | the logical size (or window size) is fitted into 480x272 with letterboxing; viewport offsets and clip rectangles are applied |
| `SDL_CreateTexture`, `SDL_CreateTextureFromSurface`, `SDL_UpdateTexture`, `SDL_LockTexture` / `SDL_UnlockTexture`, `SDL_DestroyTexture` | a shadow copy in RGBA8888, uploaded as 4444 textures in pages of up to 512x512, re-uploaded when the program changes it (streaming textures cost one upload per frame) |
| `SDL_SetTextureColorMod`, `SDL_SetTextureAlphaMod` | per-quad tint (5650 when opaque, 4444 when translucent) |
| `SDL_RenderCopy`, `SDL_RenderCopyEx`, `SDL_RenderCopyF`, `SDL_RenderCopyExF` | scaled quads (`PSPEXP_BATCH_SCALED`); horizontal and vertical flips; **rotation is ignored** (drawn unrotated) |
| `SDL_RenderClear` | a full-screen rect, including the letterbox bars |
| `SDL_RenderFillRect(s)`, `SDL_RenderFillRectF`, `SDL_RenderDrawRect`, `SDL_RenderDrawPoint(s)` | `DRAW_RECT` with the draw colour and alpha (when the blend mode is not NONE) |
| `SDL_RenderDrawLine(s)` | `DRAW_LINE` |
| `SDL_RenderPresent` | flush, `PRESENT`, paced on the PSP's `PRESENT_DONE`; then input and audio |
| `SDL_UpdateWindowSurface(Rects)` (programs without a renderer) | the window surface is shrunk to the PSP size and streamed as a texture every present (≈270 KB per frame at 480x272) |
| `SDL_OpenAudio`, `SDL_OpenAudioDevice` (callback or `SDL_QueueAudio`) | the mixed output is converted to 44.1 kHz stereo and streamed (`AUDIO_STREAM`); the host device keeps playing unless `PSPEXP_LOCAL_AUDIO=0` |
| PSP controls | a virtual game controller ("PSP (PSPExp)": a/b/x/y = cross/circle/square/triangle, shoulders, start/back, d-pad, left stick = nub) when the program initialised joysticks, plus keyboard events per `PSPEXP_KEYS` |

Not mirrored: render targets (`SDL_SetRenderTarget` to a texture: drawing
into it is skipped, the texture is drawn as last uploaded), rotation,
`SDL_RenderGeometry`, OpenGL / Vulkan drawing, and SDL 1.2 programs.

The PSP is double-buffered: a program that never clears and redraws only
the parts that changed (relying on the previous frame staying put) shows
flicker between its two last frames; the window-surface path is immune
since the whole surface goes every frame, behind a black letterbox clear.

## Environment

| variable | meaning |
|---|---|
| `PSPEXP=usb\|tcp[:host[:port]]\|sdl\|null` | target (the launcher's `--usb`, `--tcp`, `--null` set it) |
| `PSPEXP_KEYS=cross=z,circle=x,square=a,triangle=s,l=q,r=w,start=return,select=escape,up=up,down=down,left=left,right=right` | keyboard keys pressed for PSP buttons (SDL key names); this is the default |
| `PSPEXP_NUB=mouse` | the nub moves the mouse, cross / circle click (default: `joystick`) |
| `PSPEXP_LOCAL_AUDIO=0` | silence the host's audio device |
| `PSPEXP_SHOT_EVERY=N` | ask the PSP for a framebuffer dump (`pspexp-shot-N.bmp` next to the EBOOT) every N frames |
| `PSPEXP_DEBUG_SKIP=clear,rects,lines,quads,audio` | leave parts of the stream out (bisecting a rendering difference) |

A stats line is printed every 2 s: fps, bytes per frame, batches, sprites,
PSP CPU and GE time, uploads, errors, audio buffered / underruns.

## Where the bytes go

A frame of the test game is 26 batches, 95 sprites, 3.7 KB. Textures are
uploaded once (4444, two bytes per texel, padded to power-of-two pages);
`SDL_TEXTUREACCESS_STREAMING` textures that change every frame are
re-uploaded every frame, which is what they cost on the wire (8 KB for the
demo's 128x32 bar). A renderer-less program streams its whole surface:
270 KB per frame, about 16 MB/s at 60 fps, which USB carries but leaves
little headroom; expect 30-60 fps depending on the program.

## Headless test

`tests/run_sdl_pspexp.sh` (run by `ctest`) runs the test game through the
shim with SDL's dummy drivers and the Null executor in both paths and checks
that every frame was mirrored.
