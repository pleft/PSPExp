# sdl-pspexp: any SDL2 program on the PSP (v0.4)

`libsdl-pspexp` is loaded into an **unmodified** SDL2 program and turns the
PSP into its display, speaker and controller. The program keeps running on
the host exactly as before (its own window, its own audio device); every
`SDL_Render*` call is mirrored into PSPExp commands, the audio the program
produces is streamed to the PSP, and the PSP's controls come back as a game
controller and/or keyboard events.

```sh
scripts/dev/pspexp-run <program> [args...]            # PSP over USB (default)
scripts/dev/pspexp-run --tcp:127.0.0.1:3838 <program> # runtime under PPSSPP
scripts/dev/pspexp-run --null <program>               # no PSP: just check it runs
PSPEXP=sdl scripts/dev/pspexp-run <program>           # PSPExp SDL executor in a 2nd window
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
scripts/dev/pspexp-run build/host/sdl-game               # renderer path
scripts/dev/pspexp-run build/host/sdl-game --surface     # window-surface path
```

## What is mirrored

| SDL2 | PSPExp |
|---|---|
| `SDL_CreateWindow`, `SDL_CreateRenderer`, `SDL_CreateWindowAndRenderer` | connect to the PSP (once); a renderer that can't be had as asked (acceleration, vsync, a driver by index) is the software one |
| `SDL_GetDesktopDisplayMode`, `SDL_GetCurrentDisplayMode`, `SDL_GetDisplayBounds`, `SDL_GetDisplayUsableBounds`; a full-screen-desktop or zero-sized `SDL_CreateWindow` | on a headless board (SDL's dummy driver, whose display has no size): the PSP's screen, 480x272 at 60 Hz, so programs that size themselves to the display lay out for the PSP |
| `SDL_RenderSetLogicalSize`, `SDL_RenderSetViewport`, `SDL_RenderSetClipRect` | the logical size (or window size) is fitted into 480x272 with letterboxing; viewport offsets and clip rectangles are applied |
| `SDL_CreateTexture`, `SDL_CreateTextureFromSurface`, `SDL_UpdateTexture`, `SDL_LockTexture` / `SDL_UnlockTexture`, `SDL_DestroyTexture`, `SDL_SetTextureBlendMode` | a shadow copy (in the program's own ARGB8888 / ABGR8888 layout, else converted), uploaded swizzled in pages cut at power-of-two sizes of up to 512x512 (256x128 for streaming textures), so there is no padding (320x200: 256 + 64 by 128 + 64 + 8); a page with at most 256 colours goes as CLUT8 with its own palette, else 5650 when the blend mode is NONE, 4444 otherwise; only the pages whose pixels changed are sent again (see "Where the bytes go") |
| `SDL_SetRenderTarget`, `SDL_RenderSetScale` | drawing into a render-target texture is recorded (copies and fills, up to 256 per target) and replayed, scaled and clipped, when the target is copied to the screen: the integer-upscaling pattern of pixel-art ports. Programs that compose whole frames or menus in targets (LTris, LBreakoutHD, Project: Starfighter, C-Dogs SDL) draw partly or not at all yet |
| `SDL_SetTextureColorMod`, `SDL_SetTextureAlphaMod` | per-quad tint (5650 when opaque, 4444 when translucent) |
| `SDL_RenderCopy`, `SDL_RenderCopyEx`, `SDL_RenderCopyF`, `SDL_RenderCopyExF` | scaled quads (`PSPEXP_BATCH_SCALED`); horizontal and vertical flips; **rotation is ignored** (drawn unrotated) |
| `SDL_RenderClear` | a full-screen rect, including the letterbox bars |
| `SDL_RenderFillRect(s)`, `SDL_RenderFillRectF`, `SDL_RenderDrawRect`, `SDL_RenderDrawPoint(s)` | `DRAW_RECT` with the draw colour and alpha (when the blend mode is not NONE) |
| `SDL_RenderDrawLine(s)` | `DRAW_LINE` |
| `SDL_RenderPresent` | flush, `PRESENT`, paced on the PSP's `PRESENT_DONE`; then input and audio |
| `SDL_UpdateWindowSurface(Rects)` (programs without a renderer) | the window surface is shrunk to the PSP size and streamed as a texture every present (≈270 KB per frame at 480x272) |
| `SDL_OpenAudio`, `SDL_OpenAudioDevice` (callback or `SDL_QueueAudio`) | the mixed output is converted to 44.1 kHz stereo and streamed (`AUDIO_STREAM`); the host device keeps playing unless `PSPEXP_LOCAL_AUDIO=0` |
| PSP controls | a virtual game controller ("PSP (PSPExp)": a/b/x/y = cross/circle/square/triangle, shoulders, start/back, d-pad, left stick = nub) when the program initialised joysticks, plus keyboard events per `PSPEXP_KEYS` |
| `SDL_GetKeyboardState`, `SDL_GetMouseState`, `SDL_GetRelativeMouseState` | answered by the layer with what the PSP's buttons and nub hold (events pushed from the PSP never reach SDL's own state: games that poll it saw a held button as one press) |
| `SDL_WarpMouseInWindow` | the nub's pointer moves there (games that recentre the mouse every frame and read the distance); mouse motion events carry `xrel` / `yrel` |
| held buttons, as keys | repeat as a keyboard does: after 400 ms, every 40 ms |
| `SDL_GetWindowFlags` (while the PSP is the screen) | the window shown and focused, never minimised or hidden (`pspexp-run` hides it): games that stop drawing or pause for a hidden or unfocused window (Quakespasm stops swapping) carry on; `PSPEXP_FOCUS=host` gives the real flags |

Not mirrored: rotation, `SDL_RenderGeometry`, lines drawn into a render
target, reading a render target back (`SDL_RenderReadPixels`), OpenGL 3
and later (core profiles), Vulkan, and SDL 1.2 programs. OpenGL 1.x and 2
(shaders, read rather than run) are drawn: see below.

The PSP is double-buffered: a program that never clears and redraws only
the parts that changed (relying on the previous frame staying put) shows
flicker between its two last frames; the window-surface path is immune
since the whole surface goes every frame, behind a black letterbox clear.

## OpenGL 1.x

`gl_pspexp.c` is an OpenGL implementation of its own, in the same library:
a program that makes an OpenGL context through SDL2 (`SDL_GL_CreateContext`)
draws on the PSP, and no GPU on the board is used (a headless board's SDL
has none: the `SDL_WINDOW_OPENGL` flag is taken off the window and the
context is ours). The PSP's GE is fixed-function hardware much like
OpenGL 1.x, so the translation is close: the board keeps GL's state and
sends it in the GE's own terms (the 3D messages, `PSPEXP_FEATURE_3D`), the
PSP transforms, lights, clips and draws. Needs a runtime with 3D (from
2026-09-30); an older one gets nothing drawn and the log says so.

| OpenGL | on the PSP |
|---|---|
| `glBegin`/`glEnd`, vertex arrays, `glDrawArrays`/`glDrawElements`, `glInterleavedArrays` | vertices in the GE's format (texture, colour, normal, position as floats), sent with the draw |
| buffer objects (`glGenBuffers`...; not `GL_STREAM_DRAW`) | **meshes**: the buffer converted once and kept on the PSP (`RESOURCE_UPLOAD_MESH`), element buffers too; a draw is then 16 bytes. Neverball: 2.1 MB to 90 KB a frame |
| display lists | recorded, replayed on the board (their vertices go every frame) |
| matrix stacks, `glFrustum`, `glOrtho`, `glRotate`... | the GE's projection and world matrices (lighting in eye space) |
| `glTexImage2D`, `glTexSubImage2D`, parameters, `glTexEnv` modulate / replace / decal / blend / add | textures resampled to powers of two of at most `PSPEXP_GL_TEXMAX` (256), 4444 with alpha or 5650, swizzled; sent before the first draw that uses them, again after a change |
| lighting (4 lights), materials, `glColorMaterial`, linear fog (exp fog as a line) | the GE's own |
| texture coordinate generation: object and eye linear, texture matrix | the GE's texture matrix (no per-vertex work here) |
| sphere maps | the GE's environment map (the normal's eye-space x and y: GL's sphere map for a view straight ahead) |
| depth, alpha test, blending (5650 framebuffer: no destination alpha), culling, scissor, `glColorMask`, `glClear` | the GE's own; a 16-bit depth buffer (VRAM for textures shrinks by 272 KB) |
| queries (`glGet*`, `glGetString`: "1.3 PSPExp"), `glGetError`, `glPushAttrib` | answered here |

Not there: a second texture unit (`GL_ARB_multitexture` is
advertised with one unit: what goes in the others is left out), stencil,
reading pixels back (`glReadPixels` gives black), copying from the
framebuffer, clip planes, polygon modes other than fill, line width,
`glBitmap`/`glDrawPixels`. Each is logged once when a program uses it.

Measured 2026-09-30 on the PSP-1000 over USB from the Pi 400:
`build/host/gl-demo` (a lit, textured, fogged scene: immediate mode, vertex
arrays, a display list, blending, 2D on top) 59.9 fps, 84 KB a frame, PSP
CPU 1.9 ms, GE 0.12 ms. Chromium B.S.U. (menu) 36 fps, 230 KB a frame (all
immediate mode). Neverball (title) 30 fps, 85 KB a frame, GE 20 ms.

### OpenGL 2: shaders

The GE runs no programs, so shaders are **read**, not run (`glsl_pspexp.c`).
A shader is parsed (GLSL 1.10-1.30 and ES 1.00: the preprocessor's
object-like macros and conditionals, functions, loops, uniform branches)
and, at a draw, evaluated symbolically with the uniforms of the moment:
`gl_Position` and the fragment colour come out as expressions of the
vertex attributes and the texture samples. The plan for the GE:

| shader | on the PSP |
|---|---|
| `gl_Position = matrix * attribute` (any product of uniforms folding to one) | the GE's projection matrix, the attribute as the position |
| another position | worked out per vertex here, sent divided by w |
| a texture sample times the rest, per channel | the GE's texture, modulated by the rest worked out per vertex |
| the colour alone | per-vertex colour |
| `if (alpha < k) discard;` | the GE's alpha test |
| `clamp(ambient + max(0, dot(normal, light)) * diffuse)`, the normal a matrix times an attribute | the GE's directional light |
| `colour + textureCube(reflected eye vector)`, or the cube map alone | a second pass, added: a sphere map made here from the cube map, as the GE's environment map |
| a discard on a texture the colour does not use | a pass before with its alpha test, the depth only |
| `texture × texture2 × K (+ texture3)` (lightmaps, glows) | passes after the colour's, each with its own coordinates: texture2 multiplied in by blending (K up to 2; more: the texture sent brighter), texture3 added |
| uniforms in `if`s and loops | the branch taken, the loop unrolled |
| other conditions (per vertex), and a function returning from inside them | both sides worked out, each value where its condition holds (Quake's models light themselves so) |
| anything else (two 2D textures, a texture channel used for another, per-pixel maths, a screen-door pattern on `gl_FragCoord`, framebuffer reads) | the nearest of the above; the program is logged once with what was left out |

`glGetString(GL_VERSION)` is "2.1 PSPExp" (`PSPEXP_GL_VERSION` to change it:
"1.3" makes such games take a fixed-function path they have); contexts of
3.x or a core profile are refused, as a driver without them would (with a
GL error pending: programs that check that way fall back). Framebuffer
objects with a colour texture are **render targets** on the PSP: the GE draws
into the texture in VRAM (at most 512 x 512, 16 or 32 bits), and it is then
sampled like any other. GLEW, which asks GLX for a display first, is answered
by a stand-in on headless boards.

Seen working in PPSSPP on 2026-09-30, from Debian on the Pi 400: Numptyphysics
(its scene through a render target), openMrac (vertex array objects, alpha
discard), SuperTux (its GL 2.0 path; it asks for 3.3 first), Widelands (its
image shader drawn as near as the GE can). A program that draws without
clearing gets a black clear first (GL leaves the buffer undefined after a
swap; PPSSPP loses track of a buffer that is never cleared).

The functions are found by name through `pspexp_gl_proc`; its table,
`gl_pspexp_procs.h`, is made by `gen-gl-procs.py` (run it after adding a
function; the tests check it is current).

## Environment

| variable | meaning |
|---|---|
| `PSPEXP=usb\|tcp[:host[:port]]\|sdl\|null` | target (the launcher's `--usb`, `--tcp`, `--null` set it) |
| `PSPEXP_KEYS=cross=z,circle=x,square=a,triangle=s,l=q,r=w,start=return,select=escape,up=up,down=down,left=left,right=right` | keyboard keys pressed for PSP buttons; this is the default. SDL key names, plus `lctrl`, `lshift`, `lalt`, `comma`, `period`, `enter`, `esc`, `pgup`, `none` and the like; unknown names are reported at start |
| `PSPEXP_AUDIO_FIFO=PATH` | sound from a named pipe (made there) to the PSP: a WAV stream, 44.1 kHz 16-bit. The board's `pspexp-run` points OpenAL Soft's "wave" output at one, so OpenAL games (SuperTux) are heard |
| `PSPEXP_SHOT_TRIGGER=FILE` | when FILE appears, the PSP's screen is copied back to FILE-N.bmp (looking at a game someone is playing) |
| `PSPEXP_SCRIPT="8000:return,16000:+up,40000:-up"` | keys pressed at those times, in ms from the first frame (+ held, - let go, else a tap): a game tested past its menus with nobody at the PSP |
| `PSPEXP_JOYSTICK=0` | no virtual game controller, the keys alone (a game that takes both gets each press twice: SuperTux) |
| `PSPEXP_NUB=mouse` | the nub moves the mouse, cross / circle click (default: `joystick`) |
| `PSPEXP_NUB=keys` | the nub presses the four arrow keys of `PSPEXP_KEYS` (for keyboard-only games such as Doom) |
| `PSPEXP_WINDOW=hide` | hide the program's own window once the PSP is connected |
| `PSPEXP_FOCUS=host` | let the game see the host window's focus loss; by default the shim hides it (and fakes a focus gain), since games pause or mute when unfocused and the host window rarely is |
| `PSPEXP_LOCAL_AUDIO=0` | silence the host's audio device |
| `PSPEXP_AUDIO=psp` | drive the game's audio callback from the shim's own thread on a precise clock instead of the host device's cadence; the default when the host audio driver is SDL's `dummy` (headless hosts: the dummy driver runs slow and jittery and the PSP underruns) |
| `PSPEXP_SHOT_EVERY=N` | ask the PSP for a framebuffer dump (`pspexp-shot-N.bmp` next to the EBOOT) every N frames |
| `PSPEXP_GL_TEXMAX=256` | OpenGL: the largest texture side sent to the PSP (a power of two up to 512) |
| `PSPEXP_GL_LOG=1` | OpenGL: each frame's draws, meshes made, unknown queries; `2`: every draw, and why a draw was not a mesh |
| `PSPEXP_GL_DUMP=DIR` | OpenGL: every texture as sent, `DIR/tex-<name>-<w>x<h>.rgba` |
| `PSPEXP_GL_NOMESH=1` | OpenGL: no meshes, every vertex every frame (to compare) |
| `PSPEXP_GL_NOCLIP=1` | OpenGL: no cutting of triangles that reach outside the GE's range (they then vanish whole) |
| `PSPEXP_GL_VERSION=1.3` | OpenGL: the version reported (default "2.1 PSPExp") |
| `PSPEXP_GL_UNITS=3` | OpenGL: the texture units reported (default 1: fixed-function multitexturing is not drawn; shader programs' textures are, in passes). Quakespasm takes its shader path only with 3 |
| `PSPEXP_GL_DEBUG=nocull,nodepth,noblend,noalpha,notex` | OpenGL: parts of the state left out (bisecting a difference) |
| `PSPEXP_GL_MAXDRAWS=N` | OpenGL: only the first N draws of each frame (bisecting) |
| `PSPEXP_TRACE_FRAMES=N` | the messages of the first N frames, in the log |
| `PSPEXP_RECORD=FILE` | every byte sent to the PSP, in order (a stream to replay) |
| `PSPEXP_DEBUG_SKIP=clear,rects,lines,quads,audio` | leave parts of the stream out (bisecting a rendering difference) |
| `PSPEXP_DELTA=0`, `PSPEXP_CLUT=0` | send every page of a changed texture / never CLUT8; both together also bring back the even page grid: the shim as it was before 2026-09-26, for `scripts/dev/bench-shim.sh` |

Stats lines are printed every 2 s: fps, bytes per frame (draw commands),
batches, sprites, PSP CPU and GE time, uploads, errors, audio buffered /
underruns; then all link bytes per frame, texture pages sent (of them
CLUT8) and kept, and the CPU time the shim spent on textures per frame
(this thread's CPU time: waiting for the link does not count). At exit a
totals line sums up the run.

## Where the bytes go

A frame of the test game is 26 batches, 95 sprites, 3.7 KB. Textures are
uploaded once; what costs is a texture the program rewrites every frame,
the whole picture of a frame-streaming game (Doom, Tyrian, emulators) or a
renderer-less program's window surface. Three things keep that small:

- **Only changed pages.** New pixels are compared with the shadow as they
  arrive; a page whose pixels are the same keeps its PSP texture.
- **CLUT8.** Most such games draw in 256 colours and convert to 32 bits
  only for SDL; a page with at most 256 distinct colours (after the
  conversion to the PSP's 16 bits) goes as 8-bit indices plus a 512-byte
  palette, half the bytes. The PSP's GE also reads it faster.
- **No padding.** Pages are cut at power-of-two sizes, which the protocol
  requires, instead of being padded up to them.

Measured 2026-09-26 with `scripts/dev/bench-shim.sh` (Chocolate Doom's timedemo
of Freedoom Phase 1, 40 s per mode; "old" is the shim before these changes):

| host, link | old | new | link data per frame |
|---|---|---|---|
| Mac, real PSP-1000 over USB | 59.8 fps | 59.8 fps (the PSP's 60 Hz) | 160.8 -> 62.5 KB |
| Omega2+, real PSP-1000 over USB | 29.9 fps | 37.2 fps (38.9 with changed pages only) | 160.8 -> 62.4 KB |
| GameShell, real PSP-1000 over USB | 31.1 fps | 38.6 fps | 160.8 -> 62.5 KB |
| GameShell, Wi-Fi to the SDL executor on the Mac | 10.4 fps | 21.1 fps | 160.8 -> 61.3 KB |
| Pi 400, Ethernet to the SDL executor on the Mac | 59.3 fps | 59.3 fps (the executor's 60 Hz) | 160.8 -> 62.4 KB |

On the Omega the CPU decides: CLUT8 costs it about 1 ms of conversion per
frame, fewer USB bytes save more than that (each transfer is expensive
there). OpenTyrian on the Omega: 177 -> 79 KB per frame, the PSP's GE time
2.3 -> 0.6 ms, about 27 fps either way (the game's own CPU). Over the
PSP-1000's Wi-Fi (about 0.25 MB/s) full-motion games stay slow: a moving
3D view changes nearly every page every frame.

## Doom

`scripts/dev/pspexp-doom` runs Chocolate Doom (`brew install chocolate-doom`)
with Freedoom's WAD (unzip a release from https://freedoom.github.io/ into
`../doom/`, or pass a WAD path) and the PSP controls mapped to Doom's keys:
nub and d-pad move and turn, cross fires, circle uses, square runs, L / R
strafe, triangle is Enter, start is Escape, select is the map. Doom's
320x200 frame is one streaming texture, six swizzled pages (256 + 64 by
128 + 64 + 8) in VRAM, CLUT8 but for the smallest: about 62 KB per frame
(160 KB before 2026-09-26), letterboxed to 362x272.

## OpenTyrian

`scripts/dev/pspexp-tyrian` runs OpenTyrian built from source
(`git clone https://github.com/opentyrian/opentyrian ../tyrian/opentyrian
&& make -C ../tyrian/opentyrian`) with the freeware Tyrian 2.1 data
(https://camanis.net/tyrian/tyrian21.zip unzipped into `../tyrian/data/`).
Nub and d-pad move, cross fires, circle / L and square / R are the
sidekicks, triangle is Enter, start pauses, select is Escape (back; at
the main menu it quits). The script
writes `~/.config/opentyrian/opentyrian.cfg` with `scaler None` the first
time: OpenTyrian's default Scale2x scaler would make the streamed frame
640x400 (12 pages, 512 KB) instead of 320x200 (4 pages, 160 KB); the PSP
does the scaling.

## Headless test

`tests/run_sdl_pspexp.sh` (run by `ctest`) runs the test game through the
shim with SDL's dummy drivers and the Null executor in the renderer,
window-surface and render-target paths and checks that every frame was
mirrored.
