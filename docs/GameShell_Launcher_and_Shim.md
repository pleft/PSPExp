# GameShell: games through the shim, and a new launcher

**Status:** design, 2026-09-26. Two streams that together make the GameShell a
complete PSPExp host: (a) every game reaches the PSP the best way it can, and
(b) the PSP shows a launcher built for it instead of a mirrored 320x240 menu.

---

## 1. Where the pixels come from today

The GameShell image (ClockworkOS v0.6, kernel 5.3.6) runs the launcher
`launchergo` (Go, SDL2, surface-based, 320x240 hard-coded) inside an X session
started from the cpi login shell. The prepared image forces the X `fbdev`
driver so that X composes into `/dev/fb0`; `pspexp-term --mirror /dev/fb0`
polls it every 16 ms and sends the changed 64x32 tiles to the PSP. Buttons go
back as XTEST key events (the kernel has no uinput).

What that gives per kind of program:

| Program | Draws through | Reaches the PSP as |
|---|---|---|
| launchergo | SDL2 window surface -> X (fbdev) | tiles (mirror) |
| SDL2 game with `SDL_Renderer` (chocolate-doom, many ports) | X (fbdev) | tiles (mirror) |
| RetroArch | its video driver (sdl2 / gl) -> X | tiles, or nothing on the GL path |
| GL program in the Lima mode (`~/.lima`) | glamor -> DRM scanout | nothing (`/dev/fb0` is not scanned out) |
| SDL2 kmsdrm program outside X | DRM scanout | nothing |

## 2. Stream (a): the shim on the GameShell

### 2.1 What can be translated and what cannot

PSPExp is a 2D quad-and-texture protocol; the PSP GE has no shaders. So:

- **SDL2 renderer programs**: every `SDL_Render*` call becomes cached textures
  and quads (`compatibility/sdl-pspexp`). Native quality, 35-60 fps.
- **SDL2 surface programs**: the shim streams the finished window surface
  (one texture per present). Pixels, not commands; the tile mirror is cheaper
  for static screens, the shim better for full-motion.
- **OpenGL / GL ES**: cannot be translated. Only capture is possible.
- **Anything else** (X drawing, raw fbdev): capture only.

### 2.2 The shim as a preload library for glibc armhf

The Omega build links the shim statically into the game because OpenWrt has no
usable LD_PRELOAD. The GameShell is ordinary glibc (Armbian jammy, glibc 2.35)
with a dynamic `libSDL2-2.0.so.0`, so the desktop route works unchanged:
`LD_PRELOAD=libsdl-pspexp.so`. `scripts/build-gameshell.sh` now cross-builds
it with Zig (`arm-linux-gnueabihf.2.35`, PIC, libusb static inside) to
`build/gameshell/lib/libsdl-pspexp.so`, and `gameshell-prep-image.sh` installs
it with `/usr/local/bin/pspexp-run`.

`pspexp-run` on the GameShell = LD_PRELOAD + the PSP handoff: it asks
`pspexp-term` to release the PSP (SIGUSR1, via `pspexp-exec`), the game uses
the USB link, and the console takes it back (SIGUSR2) when the game ends.
The game's own X window stays on the (absent) LCD; `PSPEXP_WINDOW=hide` hides
it. Sound goes to the PSP only (`PSPEXP_LOCAL_AUDIO=0`), so it does not also
enter the console's audio FIFO.

First target: freeDM (`chocolate-doom -iwad freedoom1.wad`, already on the
image), the same game that runs at 35 fps from the Mac, the Pi and the Omega.
Then RetroArch with its `sdl2` video driver, which is `SDL_Renderer` underneath
(one streaming texture per frame plus the menu's draws) - to be measured.

### 2.3 Capture for what the shim cannot translate

The mirror grows a second backend beside `/dev/fb0`:

- **X root window readback** (GetImage on `:0`): pspexp-term already has a
  minimal X11 client for XTEST; one more request returns whatever X composed,
  including glamor/Lima output. About 300 KB per frame over a local socket,
  diffed into the same 64x32 tiles. Chosen automatically when `/dev/fb0` is
  not what X scans out.
- **kmsdrm programs outside X**: the program owns the DRM master and its
  buffers cannot be read by anyone else. Not supported; the answer for such a
  program is the preload shim (which bypasses the display entirely).

## 3. Stream (b): a launcher made for the PSP

### 3.1 Why not port launchergo one-to-one

launchergo is a Go transcription of the 2018 pygame launcher: a 320x240 grid
of icons, one page per folder, a title bar, and a plugin system in Go that
nobody extends. Porting it keeps its limits. What is worth keeping is its
**content model**, because every GameShell package targets it:

- `~/launchergo/Menu/GameShell/` and `~/apps/Menu/`: folders are categories,
  `NN_Name` sorts, `.sh` files are launchable entries, `plugin.json` marks a
  built-in page (Settings, Music, TinyCloud), `action.config` describes an
  emulator entry (ROM folder + core), skins hold one PNG icon per entry.

The new launcher reads exactly that tree, so existing menus, games and skins
show up unchanged.

### 3.2 Where it runs

The launcher becomes pspexp-term's **home view**, in its own source file
(`tools/pspexp-term/launcher.c`), next to the terminal and the mirror. This
reuses what pspexp-term already has and a separate program would have to
duplicate: the USB link and reconnect logic, the built-in font upload, the
button handling with auto-repeat, the XTEST/uinput keypad, the mirror, the
release/reacquire handoff, the systemd service. Launching an entry:

1. **PSPExp-aware entry** (its script uses `pspexp-run`): the launcher releases
   the PSP, runs the script, shows the launcher again when it exits.
2. **Any other entry**: the launcher runs the script in the X session and
   switches to the mirror view for the life of the process; the PSP's buttons
   drive it through the keypad; when it exits, back to the launcher.

Hold select still cycles views (launcher / terminal / mirror); hold start
opens the launcher's own menu.

### 3.3 The design

480x272, 16:9, one hand on the d-pad. Principles: **one screen, no pages**;
the whole library is reachable with the d-pad and cross in under 3 presses;
large art, small chrome; instant (all textures cached on the PSP once).

```
 ┌──────────────────────────────────────────────────────────────┐
 │  21:47   Wi-Fi ▂▄▆   ▮▮▮▯ 76 %                     ⌕  ⚙      │  status strip (18 px)
 │                                                              │
 │   ◀  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐   ▶     │  category rail
 │      │Retro │  │Indie │  │ Pico │  │Music │  │Tools │        │  (icons, 56 px, the
 │      └──────┘  └──────┘  └──────┘  └──────┘  └──────┘        │   selected one lifted)
 │                                                              │
 │   ┌──────────────┐   Cave Story                              │  entry list of the
 │   │              │   nxengine · last played yesterday        │  selected category:
 │   │   cover /    │ ▶ freeDM                                  │  cover on the left,
 │   │   icon       │   chocolate-doom · PSP native ✔           │  rows on the right,
 │   │              │   RetroArch                               │  selected row expanded
 │   └──────────────┘   emulator frontend                       │  with one line of detail
 │                                                              │
 │   ✕ launch   ○ back   △ options   □ favourite      L R page  │  hint bar (16 px)
 └──────────────────────────────────────────────────────────────┘
```

- **Category rail**: the top-level folders as icons on one horizontal rail;
  left/right moves, the selected icon scales up 1.25x with a soft shadow, the
  rail scrolls smoothly (eased, 150 ms) when it overflows.
- **Entry list**: the selected category's entries; up/down moves, the selected
  row expands to show its detail line; sub-folders open in place with a slide
  (circle backs out). Emulator entries (`action.config`) list the ROM folder.
- **Cover panel**: the entry's icon (skin PNG) or a generated cover: the
  entry's initials on a colour derived from its name, so nothing is blank.
- **Recent** as a pseudo-category first on the rail: the last 8 launched
  entries, kept in `~/.config/pspexp-launcher/recent`.
- **Favourites** (square toggles) shown as a small mark and grouped first.
- **Search** (triangle on the status strip, or the first letter typed on the
  on-screen keyboard of the terminal view): filters the list as you type.
- **Status strip**: clock, Wi-Fi strength, battery from
  `/sys/class/power_supply`, a PSP-link dot, storage free.
- **Settings**: the existing `plugin.json` pages are not Go plugins here; the
  launcher provides native pages for Wi-Fi (NetworkManager via `nmcli`),
  brightness/volume (`amixer`), power (sleep, reboot, off), about. Anything
  else in the old Settings folder stays reachable as scripts.
- **Look**: dark theme, one accent colour (the skin's `High`, #33a6ff by
  default), rounded cards drawn as 9-slice sprites, proportional UI font
  rasterised on the host from the skin's `VarelaRound-Regular.ttf` into a
  CLUT4 atlas (stb_truetype), bold weight for titles. Every element animates
  with short eased moves (selection, scroll, open/close); nothing blinks.
- **Cost**: a static screen sends nothing; a d-pad move sends a few quads
  (< 1 KB). Icons are uploaded once as 4444 textures (56x56, 128x128 for the
  cover) and stay in the PSP's cache; the font atlas is one CLUT4 texture.

### 3.4 Build-up order

1. Content model: read the menu tree, `plugin.json`, `action.config`, skins;
   unit-test on the Mac against a copy of the image's tree.
2. Assets: TTF -> CLUT4 atlas, PNG -> 4444 texture (stb headers vendored in
   `tools/pspexp-term/`), 9-slice cards. Test with the SDL executor on the Mac.
3. The view: rail + list + cover + strips, d-pad navigation, animation.
4. Launching with both handoffs; Recent and Favourites.
5. Status strip sources; Settings pages.
6. Image prep: the launcher view is the service's default; launchergo and X
   still start for the non-PSPExp games (mirror).

## OpenTyrian on the GameShell (measured 2026-09-26)

The launcher's OpenTyrian (Indie Games) is an SDL 1.2 build: the shim cannot
reach it, so it only shows through the mirror (≈ 14 fps). The SDL2 OpenTyrian
builds on the GameShell itself (gcc is on the image; headers from the SDL2
source, linked to the image's libSDL2-2.0.so.0):

    make WITH_NETWORK=false SDL_CPPFLAGS="-I<sdl2 include> -D_REENTRANT -include stdlib.h \
         -include string.h -include math.h -include stdio.h" SDL_LDFLAGS=-L<dir with libSDL2.so> \
         SDL_LDLIBS="-lSDL2 -lm"

(the generic SDL_config.h from the source tree includes no libc headers, hence
the `-include` flags; without them roundf/sinf are implicitly int). Installed as
`~/pspexp/opentyrian` and run by `scripts/gameshell/pspexp-tyrian` (data from
the launcher's own copy): 35 fps in play, 0 underruns, ≈ 13 % of one core. The
launcher entry `OpenTyrian.sh` (stock copy kept as `.orig`) runs pspexp-tyrian
while the PSP console is running, the stock build otherwise.
