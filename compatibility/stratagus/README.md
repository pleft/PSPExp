# Stratagus renderer backend (v0.2)

> The boards' Warcraft package (`recipes/warcraft`, 2026-10-08) does not use
> this backend: it runs unmodified Stratagus through the SDL layer, which
> sends its 8-bit screen as CLUT8 pages and holds 60 fps from a Pi. This
> backend stays as the v0.2 experiment and a reference.

Stratagus (the engine behind War1gus / Warcraft: Orcs & Humans) is a pure
software renderer: every tile, unit, glyph, fog tile and primitive is an
`SDL_BlitSurface` or a pixel loop into one 32-bit CPU surface, `TheScreen`,
which is uploaded to the window once per frame. There is no draw-call
abstraction to hook, so the backend hooks the *leaves*: the one blit each
`CGraphic::Draw*` path ends in, the glyph blit in `font.cpp`, the `CVideo`
primitive wrappers, the legacy fog shroud, and the minimap blit. Each hook
mirrors the draw into PSPExp commands **and** lets the software path run,
so the desktop window keeps showing the engine's own frame next to what the
PSP renders.

The code lives in the Stratagus tree (branch `pspexp` of the clone under
`../WarCraft/upstream/stratagus`, on top of the `psp` branch):

```text
src/include/pspexp_video.h     backend interface
src/video/pspexp_video.cpp     backend: textures, palettes, batching, input, present
src/video/graphic.cpp          leaf hooks: DrawSub, DrawSubClip, DrawSubCustomMod, DrawFrame*X, FreeSurface
src/video/font.cpp             VideoDrawChar hook (glyph blit after the palette swap)
src/video/linedraw.cpp         31 CVideo primitive wrappers -> rects, lines, circles
src/map/fow.cpp                DrawFullShroudOfFog -> translucent fill
src/map/minimap.cpp            minimap blit -> texture quad; re-upload after Update()
src/video/sdl.cpp              Init at end of InitVideoSdl, Present in RealizeVideoMemory, PumpInput
src/ui/widgets.cpp             GUI graphics object replaced by the mirroring subclass
src/sound/sound_server.cpp     AudioInstall after Mix_OpenAudio
CMakeLists.txt                 WITH_PSPEXP / PSPEXP_DIR: add_subdirectory(PSPExp), link, -DUSE_PSPEXP
```

`stratagus-pspexp.patch` here mirrors that branch (regenerate with
`scripts/dev/export-stratagus-patch.sh`).

## How the mapping works

| engine | PSPExp |
|---|---|
| `SDL_Surface` (8-bit palettised PNG, or 32-bit) | pages of <= 512x512: CLUT8 swizzled textures, or 4444 for 32-bit; one 256-entry 5551 palette |
| player colours, font colours, colour cycling (all palette mutations before a blit) | a hash of the surface palette selects an uploaded palette variant (up to 24 per graphic), passed per batch |
| `DrawFrameX` (pre-mirrored `SurfaceFlip`) | same texture, mirrored quad (`u0 > u1`); the flipped surface copy is not uploaded |
| `DrawSubTrans(alpha)`, `CopyWithSrcAlphaKey` modifier | 4444 vertex tint with alpha |
| fog shroud darkening, `FillTransRectangle` | translucent `DRAW_RECT` |
| lines, rectangle outlines, circles | `DRAW_LINE` / thin rects / line loops |
| clipping (`ClipX1..Y2`) | `SET_CLIP`, emitted lazily when it changes |
| `RealizeVideoMemory` | `PRESENT`, paced one frame in flight on `PRESENT_DONE` |
| PSP controller (INPUT samples) | the native PSP port's mapping, as synthetic SDL mouse/key events |

Draw calls are batched: consecutive quads with the same texture page,
palette and vertex format go out as one `DRAW_QUAD_BATCH` (16 bytes per
sprite). Frame-table sprites (8 bytes) are a later optimisation for the
tileset and unit sheets.

Menus: guichan draws through a `gcn::SDLGraphics` subclass installed by
`initGuichan` (`src/ui/widgets.cpp`) that mirrors `drawImage`,
`drawSDLSurface`, `fillRectangle`, `drawRectangle`, `drawLine` and
`drawPoint` with the widget clip rectangle; text goes through the engine's
own glyph blit. The whole game, from the title screen on, is on the PSP.

Audio: Stratagus mixes inside SDL_mixer, so `InitSound` installs a post-mix
tap (`Mix_SetPostMix`). The final 44.1 kHz stereo mix is copied into a ring
on the audio thread, silenced locally, and streamed from the main thread as
`AUDIO_STREAM` messages (about 176 KB/s). The PSP plays it through the
runtime mixer with a 100 ms jitter buffer and reports `AUDIO_STATUS`.
Cached sound effects (`PLAY_SOUND`) exist in the protocol but the backend
uses the stream for everything; that is section 8's "pre-mixed on the host"
mode and costs nothing in engine changes.

Eviction: every second the backend releases surfaces the game has not drawn
for 15 s (the engine keeps them; the next draw re-uploads). The minimap is
re-uploaded only when its pixels change. In-game the PSP holds about 19
surfaces / 0.5 MiB for Warcraft; palette variants (up to 64 per graphic)
stop being uploaded once colour cycling has been seen once.

Verified without hardware: with `PSPEXP=tcp:127.0.0.1:3838` and the PSP
runtime under PPSSPP (`scripts/dev/run-ppsspp.sh`), the emulated GE renders the
game; `PSPEXP_SHOT_EVERY=N` saves its framebuffer every N frames.

Not yet mirrored: the enhanced/bilinear fog modes (War1gus uses the legacy
tiled mode) and `Video.DrawEllipseClip` (approximated as a circle).

## Running

Host build of Stratagus with the backend (macOS, Homebrew SDL2, SDL2_image,
SDL2_mixer; Lua/tolua++ from the WarCraft project's host build):

```sh
cd ../WarCraft
cmake -S upstream/stratagus -B build/host-stratagus -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_VENDORED_LUA=OFF \
  -DLUA_INCLUDE_DIR=$PWD/build/host-lua -DLUA_LIBRARIES=$PWD/build/host-lua/liblua51.a \
  -DLUA_LIBRARY=$PWD/build/host-lua/liblua51.a -DTOLUA++_INCLUDE_DIR=$PWD/build/host-lua \
  -DTOLUA++_LIBRARY=$PWD/build/host-lua/libtoluapp51.a -DTOLUA++_APP=$PWD/build/host-lua/toluapp \
  -DWITH_MNG=OFF -DWITH_THEORA=OFF -DWITH_OGGVORBIS=OFF -DWITH_STACKTRACE=OFF -DWITH_OPENMP=OFF \
  -DWITH_BZIP2=OFF -DENABLE_USEGAMEDIR=ON -DBUILD_TESTING=OFF \
  -DWITH_PSPEXP=ON -DPSPEXP_DIR=$PWD/../PSPExp
cmake --build build/host-stratagus -j8 --target stratagus
```

Then, with the PSP running `dist/PSPExp` and plugged in:

```sh
PSPEXP=usb build/host-stratagus/stratagus -d dist/War1gus     # PSP renders, desktop window shows the software frame
PSPEXP=sdl build/host-stratagus/stratagus -d dist/War1gus     # no PSP: second window shows the PSPExp render
```

Headless hosts (section 13: Raspberry Pi, GameShell, Omega2+): the engine
still needs an SDL window and audio device objects, so run it on SDL's dummy
drivers; the backend adds a software-renderer fallback for that case and
`PSPEXP_WINDOW=hide` skips the host present entirely:

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy PSPEXP_WINDOW=hide PSPEXP=usb \
  build/host-stratagus/stratagus -d dist/War1gus
```

Input then comes only from the PSP (its samples become the engine's mouse
and keyboard events) and sound only from the PSP.

The backend prints one line every two seconds: fps, bytes per frame,
batches, sprites, input-to-present latency, PSP CPU and GE time, uploads,
errors, and audio frames sent / buffered on the PSP / underruns. The native PSP build of the same game (`build-psp.sh`) is the control
measurement the architecture asks for in section 18.
