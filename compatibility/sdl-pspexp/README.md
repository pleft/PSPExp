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
| `SDL_DestroyWindow` | the layer lets go of the window and takes the next one made (ScummVM makes and destroys a 32x32 OpenGL window several times to see what OpenGL can do: the layer kept the first, and its games were scaled 8.5 times from a corner) |
| `SDL_CreateWindow`, `SDL_CreateRenderer`, `SDL_CreateWindowAndRenderer` | connect to the PSP (once); a renderer that can't be had as asked (acceleration, vsync, a driver by index) is the software one; `SDL_CreateWindowAndRenderer` with `SDL_WINDOW_OPENGL` (Emilia Pinball, which then draws with OpenGL) makes the window, a software renderer and the layer's OpenGL context |
| `SDL_GetDesktopDisplayMode`, `SDL_GetCurrentDisplayMode`, `SDL_GetDisplayBounds`, `SDL_GetDisplayUsableBounds`; a full-screen-desktop or zero-sized `SDL_CreateWindow` | on a headless board (SDL's dummy driver, whose display has no size): the PSP's screen, 480x272 at 60 Hz, so programs that size themselves to the display lay out for the PSP |
| `SDL_RenderSetLogicalSize`, `SDL_RenderSetViewport`, `SDL_RenderSetClipRect` | the logical size (or window size) is fitted into 480x272 with letterboxing; viewport offsets and clip rectangles are applied |
| `SDL_CreateTexture`, `SDL_CreateTextureFromSurface`, `SDL_UpdateTexture`, `SDL_LockTexture` / `SDL_UnlockTexture`, `SDL_DestroyTexture`, `SDL_SetTextureBlendMode` | a shadow copy (in the program's own ARGB8888 / ABGR8888 layout, else converted), uploaded swizzled in pages cut at power-of-two sizes of up to 512x512 (256x128 for streaming textures), so there is no padding (320x200: 256 + 64 by 128 + 64 + 8); a page with at most 256 colours goes as CLUT8 with its own palette, else 5650 when the blend mode is NONE, 4444 otherwise; only the pages whose pixels changed are sent again (see "Where the bytes go") |
| `SDL_SetRenderTarget`, `SDL_RenderSetScale` | drawing into a render-target texture is recorded (copies and fills, up to 256 per target) and replayed, scaled and clipped, when the target is copied to the screen: the integer-upscaling pattern of pixel-art ports. When a texture a target's recorded copies use is destroyed, the target is first baked: its recorded draws composed into its pixels on the board (nearest scaling, flips, colour and alpha mod, blend none / add / blend), and later draws into it composed there too, the result sent as an ordinary texture (Flare's menu text). Programs that compose whole frames or menus in targets (LTris, LBreakoutHD, Project: Starfighter, C-Dogs SDL) draw partly or not at all yet |
| `SDL_SetTextureColorMod`, `SDL_SetTextureAlphaMod` | per-quad tint (5650 when opaque, 4444 when translucent) |
| `SDL_RenderCopy`, `SDL_RenderCopyEx`, `SDL_RenderCopyF`, `SDL_RenderCopyExF` | scaled quads (`PSPEXP_BATCH_SCALED`); horizontal and vertical flips; **rotation is ignored** (drawn unrotated) |
| `SDL_RenderClear` | a full-screen rect, including the letterbox bars |
| a letterboxed frame (any) | the bars painted black at its start: a game that draws a full background and never clears (Blobby Volley) left them showing what the PSP had there |
| a full-screen window on a headless board | the PSP's size, 480x272, whatever SDL's dummy driver makes it (1024x768): Trigger Rally goes full screen at the desktop's size and set its viewport for 480x272 |
| `SDL_RenderFillRect(s)`, `SDL_RenderFillRectF`, `SDL_RenderDrawRect`, `SDL_RenderDrawPoint(s)` | `DRAW_RECT` with the draw colour and alpha (when the blend mode is not NONE) |
| `SDL_RenderDrawLine(s)` | `DRAW_LINE` |
| `SDL_RenderPresent` | flush, `PRESENT`, paced on the PSP's `PRESENT_DONE`; then input and audio |
| `SDL_UpdateWindowSurface(Rects)` (programs without a renderer) | the window surface is shrunk to the PSP size and streamed as a texture every present (≈270 KB per frame at 480x272) |
| `SDL_OpenAudio`, `SDL_OpenAudioDevice` (callback or `SDL_QueueAudio`) | the mixed output is converted to 44.1 kHz stereo and streamed (`AUDIO_STREAM`); the host device keeps playing unless `PSPEXP_LOCAL_AUDIO=0` |
| PSP controls | a virtual game controller ("PSP (PSPExp)": a/b/x/y = cross/circle/square/triangle, shoulders, start/back, d-pad, left stick = nub) when the program initialised joysticks, plus keyboard events per `PSPEXP_KEYS`. Its mapping string must end in a comma: SDL appends its own `,crc:` field after one, or else straight on, and until 2026-10-06 `lefty:a1crc:db13` left the controller with no up and down (Diablo and Celeste 64 went only left and right; the joystick's own axis was right). The nub as a stick has a dead zone: within 8000 of the middle it is the middle, the rest scaled to the full range (a PSP-1000's nub rests off the middle, one measured at -5400 / +2000, and trembles) |
| `SDL_GetKeyboardState`, `SDL_GetMouseState`, `SDL_GetRelativeMouseState` | answered by the layer with what the PSP's buttons and nub hold (events pushed from the PSP never reach SDL's own state: games that poll it saw a held button as one press). One table, handed out from the first call even before the layer is up and kept current at every key and every `SDL_PumpEvents` (hooked for that): Hurrican asks once at start and keeps reading the pointer |
| `SDL_WarpMouseInWindow` | the nub's pointer moves there (games that recentre the mouse every frame and read the distance); mouse motion events carry `xrel` / `yrel` |
| `SDL_SetRelativeMouseMode`, `SDL_GetRelativeMouseMode` (with `PSPEXP_NUB=mouse`) | kept by the layer (a headless board's dummy driver refuses it): while on, the game gets the nub's whole motion, never stopped at an edge, and no pointer is drawn (the game draws its own: OpenArena showed two, and its cursor stuck where the layer's met the edge) |
| `SDL_ShowCursor`, `SDL_CreateCursor`, `SDL_SetCursor` (with `PSPEXP_NUB=mouse`) | the layer draws the nub's pointer (a white arrow) unless the program hides the system one: with `SDL_ShowCursor(SDL_DISABLE)`, or with a cursor that has no visible pixel set (Stratagus sets a 1x1 transparent one and draws its own: both pointers showed); a program's own colour cursor (`SDL_CreateColorCursor`) is not drawn |
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
| `glBegin`/`glEnd`, vertex arrays, `glDrawArrays`/`glDrawElements`, `glInterleavedArrays` | vertices in the GE's format (texture, colour, normal, position as floats), sent with the draw; a list of triangles (or lines, points) drawn right after another with nothing sent between them (the same state, matrices and vertex format) goes on as the same `3D_DRAW`, a lone triangle as a strip or fan counting as a list (Emilia Pinball, polygon by polygon: 7,800 draws a frame, 666 to 465 KB) |
| buffer objects (`glGenBuffers`...; not `GL_STREAM_DRAW`) | **meshes**: the buffer converted once and kept on the PSP (`RESOURCE_UPLOAD_MESH`), element buffers too; a draw is then 16 bytes. Neverball: 2.1 MB to 90 KB a frame |
| a Quake engine's model shader (two poses mixed by `Blend`, lit against `ShadeVector`: recognised by its names) | each pose pair a mesh on the PSP with two morph targets (16-bit coordinates, 8-bit normal, 16-bit position), mixed by the GE with the draw's weights (`3D_MORPH`), lit by a GE light fitted to the shader's shade; the colour within 0.025 on average, positions exact. Needs a runtime with `PSPEXP_FEATURE_3D_MORPH` (2026-10-01) |
| `GL_QUADS` facing the camera (Quake's particles: upright on the screen, the texture upright on them or turned a quarter on a texture the same mirrored across its diagonal, one colour each, unlit) | the GE's sprites (`PSPEXP_FEATURE_3D_SPRITES`, 2026-10-01): two vertices a quad, its opposite corners, instead of two triangles' six |
| display lists | recorded, replayed on the board; a run of `glBegin`/`glEnd` blocks with nothing but vertices between them is drawn as one strip (its strips joined by triangles of no area: the GE then transforms a vertex once, not three times) and one list of the other triangles, each kept on the PSP as a mesh by content, the same each frame (DreamChess's pieces, hundreds of short strips each: 9,400 draws and 4.4 MB a frame down to 220 draws and 41 KB; 30 fps on the PSP-1000) |
| framebuffer objects (`GL_ARB_framebuffer_object`, reported only when asked for: `PSPEXP_GL_EXTENSIONS`) | a texture's colour: a render target in the PSP's VRAM (sides up to 512); a colour renderbuffer the window's size: the screen itself, so copying it onto the window (`glBlitFramebuffer`) costs nothing; other colour renderbuffers: a texture of their own; depth and stencil renderbuffers: the GE's one depth buffer; `glBlitFramebuffer` otherwise: the colour drawn as a textured quad. A target the PSP has no room for, or one drawn into for 120 frames and never textured from (it could only be read back, which gives black here), is let go and its draws dropped |
| matrix stacks, `glFrustum`, `glOrtho`, `glRotate`... | the GE's projection and world matrices (lighting in eye space) |
| `glTexImage2D`, `glTexSubImage2D`, parameters, `glTexEnv` modulate / replace / decal / blend / add | textures resampled to powers of two of at most `PSPEXP_GL_TEXMAX` (256), 4444 with alpha or 5650, swizzled; sent before the first draw that uses them, again after a change |
| lighting (4 lights), materials, `glColorMaterial`, linear fog (exp fog as a line) | the GE's own |
| `glTexImage3D`, `GL_TEXTURE_3D` | a 2D atlas, the slices in a grid; each vertex's (s, t, r) mapped into r's cell on the board (the GE has no 3D textures: Teeworlds keeps its map tiles so, one a slice) |
| texture coordinate generation: object and eye linear, texture matrix | the GE's texture matrix (no per-vertex work here) |
| sphere maps | the GE's environment map (the normal's eye-space x and y: GL's sphere map for a view straight ahead) |
| depth, alpha test, blending (5650 framebuffer: no destination alpha), culling, scissor, `glColorMask`, `glClear` | the GE's own; a 16-bit depth buffer (VRAM for textures shrinks by 272 KB) |
| queries (`glGet*`, `glGetString`: "1.3 PSPExp"), `glGetError`, `glPushAttrib` | answered here |

Not there: texture units past the second (`GL_ARB_multitexture` is
advertised with two: what goes in the others is left out), a unit
combined as one minus its texture's alpha (Neverball's shadow: left out unless
`PSPEXP_GL_SHADOW`), stencil, reading pixels back (`glReadPixels` gives black),
copying from the framebuffer other than from the screen at the PSP's scale into a
texture (`glCopyTexSubImage2D`, done by the GE, `PSPEXP_FEATURE_3D_COPY`), clip planes, polygon modes other than fill, line width,
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
| `gl_Position = matrix * mix(pose1, pose2, blend)` (a weighted sum of attributes) | the sum made per vertex here, the matrix the GE's (which clips: vertices behind the eye stay right) |
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
3.x are refused (a core profile asked for without a version of 3.2 or more
is not one, as drivers have it: Hurrican asks for "core" for its OpenGL 1.x), as a driver without them would (with a
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
| `PSPEXP_AUDIO_FIFO=PATH` | sound from a named pipe (made there) to the PSP: a WAV stream, 44.1 kHz 16-bit, taken at 44.1 kHz by the clock (at most 100 ms ahead), which holds a writer that does not keep time to real time. The board's `pspexp-run` points OpenAL Soft's "wave" output and ALSA's default output at one, so OpenAL games (SuperTux) and ALSA ones (FMOD: Celeste 64) are heard. A signal that cuts the reader's wait short no longer ends it: the program's next open of the pipe then waited for ever (Unreal Tournament stood at its title) |
| `PSPEXP_SHOT_TRIGGER=FILE` | when FILE appears, the PSP's screen is copied back to FILE-N.bmp (looking at a game someone is playing) |
| `PSPEXP_WINDOW=480x272` | the program's window this size and never full screen, whatever it asks (`SDL_CreateWindow`, `SDL_SetWindowFullscreen`): a program that goes full screen gets the headless board's display size otherwise (the offscreen driver's 1024x768: Celeste 64's 16:9 letterboxed in it, three times the pixels read back) |
| `PSPEXP_QUANT=1` / `=0` | a changed page of more than 256 colours reduced to its 256 best (median cut on its 5650 colours, no dithering) and sent as CLUT8: about half its bytes, nothing new on the PSP. On by default in `libsdl-pspexp-hostgl` (frames from the board's GPU: SuperTuxKart's race 230 to 137 KB a frame for 0.4 ms more of the board's CPU, the picture alike even enlarged), off in the PSP-rendered layer |
| `PSPEXP_SCRIPT="8000:return,16000:+up,40000:-up"` | keys pressed at those times, in ms from the first frame or wait for an event (+ held, - let go, else a tap): a game tested past its menus with nobody at the PSP; a game blocked in `SDL_WaitEvent` gets them too (Emilia Pinball draws its menus only when a key comes); `@XxY` moves the pointer there (window pixels), `~DXxDY` by that much (games that make their own cursor from the motion), `click` / `+click` / `-click` the left mouse button, for menus that want a mouse (Lugaru); `pad:cross` (any PSP button's name, `+pad:` / `-pad:` held and let go) a PSP button as if pressed on the PSP, its keys and the virtual controller both: games that tell a pad from a keyboard (DevilutionX names a new hero only for a pad); `nub:XxY` holds the nub there (-127..127, right and down positive; `nub:0x0` lets go) |
| `PSPEXP_JOYSTICK=0` | no virtual game controller, the keys alone (a game that takes both gets each press twice: SuperTux) |
| `PSPEXP_NUB=mouse` | the nub moves the mouse, cross / circle click (default: `joystick`) |
| `PSPEXP_NUB=keys` | the nub presses the four arrow keys of `PSPEXP_KEYS` (for keyboard-only games such as Doom) |
| `PSPEXP_NUB_SPEED=N` | with `PSPEXP_NUB=mouse`: the pointer's speed in window pixels a second with the nub fully over (default 300; OpenTTD's recipe 180) |
| `PSPEXP_TEX_HALF=0` | a streaming texture larger than 512 on a side is kept and sent at half size (the PSP shows such a whole frame at under half anyway: Project: Starfighter's 800x600, 1.2 MB a frame and 13 fps in PPSSPP, 197 KB and 31 fps halved); 0: at full size |
| `PSPEXP_BT_LATENCY=20ms` | (libpspexp, every program) with the sound routed to a Bluetooth device (System > Sound output): the latency pw-play asks PipeWire for (default 20ms, 60ms on a processor under 1.5 GHz such as the GameShell's; pw-play's own 100ms made games' sound lag) |
| `PSPEXP_MOUSE_BUTTONS=left=r,right=none` | with the nub as the mouse: the PSP buttons that are its left and right buttons (default `left=cross,right=circle`); the others stay keys (OpenArena: R fires, the face buttons move) |
| `PSPEXP_LOG_EVENTS=1` | every input event the program takes from SDL in the log (keys, mouse buttons with where the pointer was, joystick buttons, axes past half, hats, controller buttons and sticks past half, devices added and removed), and on each press of the PSP whether SDL's joystick events are on: which input reaches a game |
| (joystick focus) | the layer sets `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1` as it loads: SDL drops joystick events while no window of the program has keyboard focus, which a headless board's never has (Sonic Robo Blast 2 got no PSP button) |
| `PSPEXP_FRAME_DROP=1` | board-GPU layer: a frame is not read back nor sent while the PSP still has the one before, instead of waited for (a program that keeps time by its own clock then keeps its speed); off by default; the totals say how many were left out |
| (audio devices) | a device the program closes is let go (Mupen64Plus closes and reopens its own when a game starts: the old slot, still paced, had the plugin's callback called twice as often, the PSP's buffer full and dropping, a crackle); a paused device sends silence once it has played, as a sound card plays (sending nothing let the PSP's buffer run dry and wait 100 ms to refill); in the board-GPU layer the pacing thread sends its sound itself; the totals report the silence sent against the sound |
| (OpenAL) | `pspexp-run` points OpenAL Soft's wave output at a pipe (`PSPEXP_AUDIO_FIFO`) and makes the pipe before the program starts; the layer reads it from the moment it loads (OpenAL's open waits for a reader) and lets go of what comes before the PSP's link. A program that opened OpenAL before its window (Vanilla Conquer's Red Alert) had found no pipe and OpenAL wrote a plain file: no sound |
| (ALSA) | `pspexp-run` also gives ALSA a default output of its own (`ALSA_CONFIG_PATH`: the system's `alsa.conf` and a file output, WAV, into the same pipe), for programs that play through ALSA itself. Celeste 64's FMOD found only the Pi's HDMI device, which does not open with no display, and did not start at all (2026-10-06) |
| START + SELECT | held 1 s: the program is asked to quit (`SDL_QUIT`); held 2 s: `exit(0)`. Two watchdogs (2026-10-06): a program still there 1.5 s after the `exit` (hung in its own cleanup: Luanti, Lugaru HD, Red Alert) is ended with `_exit`, and so is one that, asked to quit, reads no input for 3 s (OpenRA stopped polling and hung on the way out, so the 2 s step, which runs as it polls, never came). One still reading its input (an "are you sure?") is left |
| (threads) | a program that draws on a thread of its own while another polls events (OpenRA) is served: one recursive lock around the link's event side and frame side (the swap's wait for the PSP's reply and the poll had taken each other's messages) |
| (.NET programs with their own SDL) | .NET looks functions up in the SDL library it loaded, never in the preload: replace that library with an empty one needing the layer and then the system SDL2 (`patchelf --add-needed`), as `pspexp-openra`'s `SDL2.so` |
| `PSPEXP_WINDOW=hide` | hide the program's own window once the PSP is connected |
| `PSPEXP_FOCUS=host` | let the game see the host window's focus loss; by default the shim hides it (and fakes a focus gain), since games pause or mute when unfocused and the host window rarely is |
| `PSPEXP_LOCAL_AUDIO=0` | silence the host's audio device |
| `PSPEXP_AUDIO=psp` | drive the game's audio callback from the shim's own thread on a precise clock instead of the host device's cadence; the default when the host audio driver is SDL's `dummy` (headless hosts: the dummy driver runs slow and jittery and the PSP underruns) |
| `PSPEXP_AUDIO_RATE=22050` | a program that opens its audio device letting the rate change (`SDL_AUDIO_ALLOW_FREQUENCY_CHANGE`, as SDL_mixer's `Mix_OpenAudio` does) mixes at this rate instead (22050 or 11025, below what it asked for); the layer upsamples it to the PSP's 44.1 kHz by whole numbers. For music synthesised as it plays: Warcraft's MIDI through FluidSynth took a whole core of the GameShell's Cortex-A7 at 44.1 kHz, the sound skipped (about 140 times in two minutes of battle) and the game fell to 15-30 fps; at 22050 Hz a fifth of a core, no skips, 44-52 fps (the war1gus and wargus launch scripts set it on the GameShell) |
| `PSPEXP_SHOT_EVERY=N` | ask the PSP for a framebuffer dump (`pspexp-shot-N.bmp` next to the EBOOT) every N frames |
| `PSPEXP_GL_TEXMAX=256` | OpenGL: the largest texture side sent to the PSP (a power of two up to 512) |
| `PSPEXP_GL_LOG=1` | OpenGL: each frame's draws, meshes made, unknown queries; `2`: every draw, and why a draw was not a mesh |
| `libsdl-pspexp-hostgl` (a second build, not a variable) | the layer without its OpenGL: the program draws with the host's GPU and each finished frame goes to the PSP as a picture (`glReadPixels` at `SDL_GL_SwapWindow`), for programs too heavy for the GE (OpenMW: 60 fps from a Mac); keep OpenSceneGraph single-threaded (`OSG_THREADING=SingleThreaded`) and the host window visible (macOS). On a headless Linux board (the Pi 400's V3D: OpenGL 3.1, ES 3.1) `pspexp-run` with `PSPEXP_HOSTGL=1` preloads it with SDL's `offscreen` video driver (EGL, no display; it answers `SDL_GetWindowWMInfo`, which offscreen has not and SuperTuxKart needs); under offscreen `glewInit`'s "no GLX display" (error 4) counts as success and `glXGetProcAddress(ARB)` goes to `SDL_GL_GetProcAddress` (OpenClonk, GLEW built for GLX); a game asking OpenGL above V3D's 3.1 can be given it with `MESA_GL_VERSION_OVERRIDE`/`MESA_GLSL_VERSION_OVERRIDE` (Trackballs: 3.3/330); frames are read back through two pixel buffer objects, a frame late, without stalling the GPU (`PSPEXP_HOSTGL_PBO=0`: `glReadPixels` straight) |
| `PSPEXP_GL_PLAN_REUSE=1` | OpenGL: a shader that reads GL's own matrices keeps its plan when only the modelview changed, its position matrix recomputed (per-vertex lighting the shader works out keeps the earlier object's: an approximation, so asked for); without it the plan is made again whenever a uniform or a built-in value actually changes |
| `PSPEXP_USB_VERBOSE=1` | why a USB transfer failed (a "link lost"), in the log |
| `PSPEXP_GL_DUMP=DIR` | OpenGL: every texture as sent, `DIR/tex-<name>-<w>x<h>.rgba`, and every shader source as the program gave it, `DIR/shader-<id>.glsl` |
| `PSPEXP_GL_NOMESH=1` | OpenGL: no meshes, every vertex every frame (to compare) |
| `PSPEXP_GL_NOMESH_IX=1` | OpenGL: an indexed draw from client memory (Quake 3's surfaces) streamed whole each frame, not kept by content (to compare): by default its vertices (64 or more) become a mesh on the PSP once drawn twice unchanged, and its list of triangles that need no cutting too while it stays the same; ET: Legacy's Oasis 1.3 MB to 90-160 KB a frame, OpenArena about 300 KB to 150 KB. Meshes on the PSP are also capped at 448 (its table of 1024 resources is shared with the textures) |
| `PSPEXP_GL_NOCLIP=1` | OpenGL: no cutting of triangles that reach outside the GE's range (they then vanish whole) |
| `PSPEXP_GL_VERSION=1.3` | OpenGL: the version reported (default "2.1 PSPExp") |
| `PSPEXP_GL_UNITS=3` | OpenGL: the texture units reported (default 2). Fixed-function texture unit 1 is drawn as a second pass blended as its environment says (modulate, `GL_COMBINE` modulate x2, add, decal, replace), from buffer objects as a mesh like the first; with unit 0 off, as the draw's only texture; its coordinate generation and texture matrix are its own. `1`: what a game does with one unit. Quakespasm takes its shader path only with 3 |
| `PSPEXP_GL_NOMESH1=1` | OpenGL: texture unit 1's pass streamed, not a mesh (to compare) |
| `PSPEXP_GL_SHADOW=1` | OpenGL: a texture unit 0 combined as the colour times one minus its alpha (Neverball's ball's shadow, with two units) drawn as a pass of its own after each surface, blended as the framebuffer times one minus the shadow's alpha; off by default: Neverball 30 to 28 fps on the PSP-1000 |
| `PSPEXP_GL_NOCOPY=1` | OpenGL: `glCopyTexSubImage2D` left out as before (to compare) |
| `PSPEXP_GL_NOSPRITES=1` | OpenGL: quads facing the camera drawn as triangles, not the GE's sprites (to compare) |
| `PSPEXP_GL_NOMORPH=1` | OpenGL: Quake engines' models streamed as before, not morphed on the GE (to compare) |
| `PSPEXP_GL_MORPH_CHECK=1` | OpenGL: Quake's models: the GE's colour and position against the shader's, vertex by vertex, logged every 20,000 vertices |
| `PSPEXP_GL_EXTENSIONS=GL_ARB_framebuffer_object,GL_ARB_texture_non_power_of_two` | OpenGL: extension names reported besides the layer's own, for a game that will not start without one the layer covers in part (DreamChess); spaces or commas between them (a launcher entry's `env=` takes no spaces). Told to every program, others would take paths they did not before: hence per game |
| `PSPEXP_GL_NOJOIN=1` | OpenGL: every draw its own `3D_DRAW`, none joined onto the one before (to compare) |
| `PSPEXP_GL_NOMERGE=1` | OpenGL: a display list's `glBegin`/`glEnd` blocks drawn one by one, not merged (to compare) |
| `PSPEXP_GL_RB_TARGETS=1` | OpenGL: a window-sized colour renderbuffer drawn into a render target of its own and copied onto the window, as GL does it, rather than drawn on the screen (to compare; needs the VRAM) |
| `PSPEXP_GL_KEEP_TARGETS=1` | OpenGL: render targets never textured from are kept and drawn into, not let go after 120 frames |
| `PSPEXP_GL_DEBUG=nocull,nodepth,noblend,noalpha,notex` | OpenGL: parts of the state left out (bisecting a difference); also `nopatch` (textures sent whole), `nomod` / `noadd` (a shader's multiplied / added texture's pass left out), `ramtex` (every texture in the PSP's main RAM), `ids` (each draw a flat colour of its texture's, the colours named in the log: which draw made a pixel) |
| `PSPEXP_GL_HIDE_TEX=N` | OpenGL: the draws with texture N left out |
| `PSPEXP_GL_TRACE_TEX=N` | OpenGL: every upload into texture N and its neighbours, and every bind of N, in the log |
| `PSPEXP_GL_MAXDRAWS=N` | OpenGL: only the first N draws of each frame (bisecting) |
| `PSPEXP_TRACE_FRAMES=N` | the messages of the first N frames, in the log |
| `PSPEXP_TRACE_BYTES=1` | what the bytes sent were, by message, averaged over 300 frames, and the frame rate over them, in the log every 300 frames (`=N` above 1: every N frames) |
| `PSPEXP_GL_NOINDEX=1` | OpenGL: indexed triangle lists always sent as triangles (by default, when the PSP takes indices and nothing needs cutting, each shared vertex goes once, with 16-bit indices) |
| `PSPEXP_GL_FLOATS=1` | OpenGL: streamed vertices always as floats (by default unlit draws go with 16-bit positions and coordinates when the PSP takes them: those of 16 vertices or more, and smaller ones that are small beside where they lie, the scale of the draw before shared where it fits so that the matrices stay and draws join; a large small draw, a floor quad, as floats times that scale) |
| `PSPEXP_PROFILE=1` | where a frame's time goes on the board, in ms a frame every 2 s: the program and its OpenGL here between frames (wall and CPU), texture work, the batch flush, the wait for the PSP's last frame, the send |
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
