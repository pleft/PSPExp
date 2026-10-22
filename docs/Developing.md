# Developing PSPExp

For people working on PSPExp itself: building it, trying it from a Mac,
and where the code lives. Playing is in the [user manual](manual/README.md);
how it works inside is in the [technical architecture](PSPExp_Technical_Architecture.md).
Every document and what it is for: [docs/README.md](README.md).

## Where each piece stands

| piece | state |
|---|---|
| `protocol/` framing, opcodes, batch records + expansion, texture rules | done, unit-tested |
| `runtime/` command processor + two-tier cache (shared by every executor) | done, unit-tested |
| `host/libpspexp` application API, frame builder, pacing, reconnect replay | done, unit-tested |
| `host/executor-null` | done |
| `host/transport-usb` (libusb, reader thread) | **runs on hardware**: 25 MB/s, 0.25 ms round trip (v01-usb-bringup.md) |
| `psp/transport-usb` kernel driver `pspexp_usb.prx` | **runs on hardware** |
| `psp/runtime` runtime app + `psp/boot` loader | **runs on hardware**; 60 fps frame loop paced on PRESENT_DONE |
| `psp/executor-ge` GE executor (VRAM/RAM textures, vertex ring, GU_SPRITES) | **runs on hardware**: 60 Hz at 5,000 sprites |
| `host/executor-sdl` reference renderer (keyboard + game controller input) | done |
| `examples/sprite-demo` (§16 proof of concept; Null, SDL or USB; survives cable pull) | **done on hardware 2026-09-22**: the v0.1 decisive result |
| `tools/usb-bench` throughput / latency / frame-loop benchmark, concurrent-upload workload | done, measured on hardware 2026-09-22 |
| CLUT4/CLUT8 textures with palette resources (v0.3 item pulled forward) | done; on the PSP since 2026-09-26 through the SDL layer's 8-bit pages (Doom, OpenTyrian, the emulators, ScummVM). `sprite-demo --clut` itself not run on hardware |
| host-baked text: built-in 6x8 CLUT4 font, `pspexp_draw_text` (v0.3 item pulled forward) | done, shown in the demo HUD |
| per-batch palette, flip, 4444 alpha tints, translucent rects, lines, quad batches (v0.2 needs) | done, tested |
| audio: shared mixer, cached sounds + streamed PCM, SDL device and PSP audio thread (v0.3 item pulled forward) | done; on the PSP since 2026-09-22 (Warcraft, the SDL layer's streamed audio: the rows below) |
| TCP transport (host + PSP over Wi-Fi / PPSSPP), `DEBUG_SCREENSHOT`, `scripts/dev/run-ppsspp.sh` | done: bench, sprite demo and Warcraft run against the real PSP runtime under PPSSPP |
| **v0.2** Stratagus renderer backend (branch `pspexp` of Stratagus in the WarCraft project; no longer in PSPExp) | **Warcraft runs on the PSP over USB at 60 fps with audio**; PSP spends ~1 ms per frame |
| host-rendered OpenGL (`libsdl-pspexp-hostgl`): a game too heavy for the GE drawn by the board's own GPU, each frame sent to the PSP as a picture | OpenMW from a Mac at 60 fps on the PSP (2026-10-02); on the Pi 400's GPU, headless: SuperTuxKart, Trackballs, Open Surge packaged (2026-10-03; ~30 fps in PPSSPP, not yet measured on the PSP) |
| **v0.4** `compatibility/sdl-pspexp`: any unmodified SDL2 program on the PSP (`scripts/dev/pspexp-run`) | **runs on the PSP-1000 over USB 2026-09-22**: the test game at 60 fps, 3.7 KB/frame, PSP CPU 0.2–0.8 ms + GE 0.5–0.8 ms, audio streamed with no underruns; renderer and window-surface paths, controller/keyboard input |

## Build

```sh
scripts/build.sh          # host: build/host (CMake); PSP: dist/PSPExp if ../pspdev exists
```

Host needs CMake and libusb-1.0 (`brew install libusb`). The PSP side needs
the pspdev toolchain at `../pspdev` (or `PSPDEV=...`).

## CI (GitHub Actions)

`.github/workflows/build.yml`, on every push to main and every pull request:
- **Board side:** CMake build and the tests on Ubuntu 24.04 (libusb, SDL2),
  as `scripts/build.sh` does.
- **PSP side:** built in the pspdev toolchain's own container
  (`ghcr.io/pspdev/pspdev`, the same GCC 15.2 as `../pspdev`), then made a
  memory-stick install (`runtime-slot --install`): `PSPExp-psp-<ref>.zip`,
  unzipped at the stick's root, a download on the run's page.
- **A tag `v*`:** `.github/workflows/release.yml` makes the GitHub Release
  with everything: the PSP's zip and both card images, built there from
  public things only (`git tag v0.5 && git push origin v0.5`; or
  `gh workflow run release.yml -f tag=NAME`, kept a draft unless
  `-f publish=true`). docs/Package_Manager.md §8c.

Weekly, `.github/workflows/check-downloads.yml` checks that every
installer's game is still downloadable (`scripts/packages/check-downloads`,
docs/Package_Manager.md §4.2). **Versions and releases (the user's choice, 2026-10-11):** releases are
semantic versions, `v1.0.0` on launch day; then `v1.1.0` for new features or
games support, `v1.0.1` for fixes only, `v2.0.0` only if older boards or PSP
installs stop working with it. The packages keep their date versions
(`2026.10.11.093726+g7ba9a2f`), which boards compare for updates (a `1.0.0`
would sort below them and stop every update); a build from a release's tag
shows its name besides: the launcher's About `1.0.0, 2026.10.11 (7ba9a2f)`
(`cmake/pspexp_version.cmake`), the PSP's status page `PSPExp 1.0.0 (slot a)`
(a `release=` line in the slot's VERSION, which the loader and the board
skip). **Making a release** (as 1.1.0 and 1.2.0 were made):
1. Everything for it published as packages already, and played on the PSP.
2. The notes: CHANGELOG.md's "Since X" section named `## X.Y.Z (date)`, with
   a short introduction (what is new, that a set-up board gets it through
   Packages); the site's front page `site/index.html` gets its "New in X.Y.Z"
   section (four cards, a link to CHANGELOG.md), replacing the previous one;
   `python3 site/build.py`, the page looked at (Chromium in a Debian
   container: `chromium --headless=new --screenshot`, 1280 and 390 wide).
   Committed and pushed: the site goes live with it.
3. `git tag vX.Y.Z` on that commit, not pushed: a build of a clean tree on a
   tag shows the release's name.
4. From it, the packages that carry the name: `pspexp-core` for both boards
   (the arm64 build in the `pspexp-arm64-dev` container's `/tmp/b`, the
   GameShell's with `scripts/gameshell/build.sh`) and `pspexp-runtime`
   (`scripts/packages/debs/pspexp-runtime/build build/debs`); then
   `apt-repo build build/debs`, `apt-repo push r2`, `apt-repo push`.
5. Optional dry run: `gh workflow run release.yml -f tag=vX.Y.Z -f
   publish=false` makes a draft with both images (about 20 minutes; `gh run
   watch`), to look at before anything is public: both images' package lists
   (`*-packages.txt`) name the new `pspexp-core`. Its PSP zip lacks the
   release's name, though: the tag is not on GitHub yet, and a draft makes its
   tag only when published (1.2.0's first draft had no `release=` line).
6. `git push origin vX.Y.Z`: the tag's run builds everything again (reusing
   the draft and replacing its files), and publishes the Release.
7. Checked: `gh release view vX.Y.Z --json isDraft,assets` (not a draft, nine
   files); the PSP zip's `PSP/GAME/PSPExp/VERSION` has `release=vX.Y.Z`; the
   package lists name the new `pspexp-core`. For new images worth trying on
   hardware, write both cards (the manual has the commands) and start them.
   The site's download page reads the latest release from GitHub's API at its
   next build (and live in the browser).

Nothing in CI signs: the boards' packages are built and signed on the
maintainer's machine, and the key never leaves it. `PSPEXP_BUILD` (the
CMake build folder for `scripts/dev/pspexp-run`, default `build/host`) lets
the SDL layer's test run from CI's folder.

## The web page (pspexp.dev)

`site/`: the front page (`site/index.html`, `site/style.css`, `site/icon.svg`)
and `site/build.py`, which writes `site/out/`: the front page in its frame, the
download page (`site/download.html`: the latest release's PSP zip and card
images, sizes, SHA-256 and download counts, written from GitHub's API at
build time; its script asks the API again in the browser, live, and rolls the
counters up), the user manual as pages from `docs/manual/*.md` (its own small Markdown
converter, Python's standard library only: the manual is written once, its
links between pages follow, links out of it go to GitHub), the screenshots
(`docs/images`) and the launcher's font (the pages ask for `style.<hash>.css` and `icon.<hash>.svg`, copies named by their
content: Cloudflare lets browsers keep them 4 hours, so a changed file gets a new address; in the
name, not as `?v=<hash>`, which Cloudflare's cache once answered with the old stylesheet). The look is the launcher's: its
colours (`launcher_ui.h`) and Inter; light and dark. Cloudflare Pages builds
it on every push (build command `python3 site/build.py`, output `site/out`).
Until launch day `LAUNCHED = False`: every page carries `X-Robots-Tag:
noindex` (`_headers`, by address), and the site is reviewed at its
`pages.dev` address; on launch day pspexp.dev is attached and `LAUNCHED`
became True on launch day (2026-10-11): pspexp.dev is indexed, while
`pspexp.pages.dev` and `www.pspexp.dev` keep their noindex for good (one
address in search results). The games page (`games.html`) is made from `recipes/` at each build: every recipe a board can install (the same `[section:board]` rules as `pspexp-pkg`), its title, description, boards and notes (your own copy, terms, who draws it), its cover (the recipe's `cover.png`, a system's RetroArch icon, else its name), with filters by board and a search. `VIDEOS`:
the videos shown under "See it in action", each its embed address
(youtube-nocookie) and caption, side by side (one above the other on a phone).
Try it: `python3 site/build.py && python3 -m http.server -d site/out`.

## Measure the USB link (v0.1 first milestone)

1. Put PSPExp on the memory stick (custom firmware required: the boot module
   loads a kernel PRX). With the PSP in USB mode (Settings > USB Connection):
   `scripts/packages/runtime-slot --install /Volumes/<stick>/PSP/GAME/PSPExp`
   writes the boot loader, the runtime in `slot-a/` and `active`. This is
   needed only once: after that a board's console updates the runtime over
   the link (the cheatsheet's "Updating the PSP's runtime"); it also
   repairs a broken install. Copying `dist/PSPExp/` flat into an empty
   folder still works too; the loader then runs the runtime next to the
   EBOOT, without slots (with slot files there it ignores those modules).
2. Launch PSPExp from the XMB and connect the USB cable. The screen shows
   `waiting for host sync`.
3. On the host: `build/host/pspexp-bench` (add `--verbose` for libusb errors).

The tool prints host→PSP and PSP→host throughput for several block sizes,
round-trip latency for 16 B, 4 KiB and 40 KB messages, and a 300-frame loop
with 500 sprites paced on PRESENT_DONE. `pspexp-bench --null` runs the same
against the in-process Null executor as a CPU baseline.

## Without a PSP: the runtime under PPSSPP

```sh
scripts/dev/run-ppsspp.sh                                   # boots dist/PSPExp in the emulator (needs EnableWlan = True)
build/host/pspexp-sprite-demo --tcp 127.0.0.1:3838 --clut --shot 120
```

The real PSP runtime (GE executor, CLUTs, audio thread) runs in the emulator
and listens over TCP; screenshots land next to the EBOOT on the emulated
memory stick. See `v01-usb-bringup.md`.

## Sprite demo (the §16 proof of concept)

```sh
build/host/pspexp-sprite-demo            # real PSP over USB (waits for it)
build/host/pspexp-sprite-demo --sdl      # SDL window, arrows/IJKL move, Z X A S = cross circle square triangle
build/host/pspexp-sprite-demo --null --frames 300
```

One atlas is uploaded once; the host owns the player object and moves it from
the PSP's own controls over a field of background sprites (`--field N`, or
triangle to cycle 100/500/2000/5000). `--clut` uploads the atlas as a
swizzled CLUT8 texture with a 5551 palette instead. The HUD text is the
built-in font: a CLUT4 glyph atlas drawn as sprite batches. Pull the cable:
the demo waits, then replays palettes and atlases and resumes. It prints fps,
bytes per frame and the input-to-present latency once a second.

## v0.2: Warcraft on the PSP through PSPExp

The first real game on PSPExp was Warcraft on a Stratagus patched to draw
through PSPExp's protocol (a renderer backend: 60 fps over USB, about 1 ms
of PSP time a frame). It proved the protocol and stays in the standalone
WarCraft project (Stratagus's `pspexp` branch); PSPExp no longer carries
its patch (removed 2026-10-10). Warcraft I and II now come as packages that
run unmodified Stratagus through the SDL layer (`warcraft`, `warcraft2`;
docs/Launcher_and_Boards.md).

## v0.4: any SDL2 program on the PSP

`compatibility/sdl-pspexp` is a library injected into an unmodified SDL2
program (`DYLD_INSERT_LIBRARIES` / `LD_PRELOAD`). It mirrors the
`SDL_Render*` calls (or streams the window surface of a renderer-less
program) into PSPExp commands, streams the program's audio, and feeds the
PSP's controls back as a game controller and keyboard events:

```sh
scripts/dev/pspexp-run build/host/sdl-game        # the bundled SDL2 test game, on the PSP
scripts/dev/pspexp-run /path/to/any/sdl2/game     # yours
```

`compatibility/sdl-pspexp/README.md` lists what is mirrored (and what is
not: rotation) and the environment variables. Render targets are
replayed, so pixel-art ports that upscale through one work too. OpenGL 1.x
programs are drawn by the PSP's GE (`build/host/gl-demo` to try it;
Chromium B.S.U. and Neverball from Debian run), and GL 2 shaders are read
and turned into what the GE can do (SuperTux, Widelands, openMrac,
Numptyphysics). What has been played on the PSP, how fast and with which
options: `docs/PSPExp_Technical_Architecture.md` §12a.

Doom is the showcase: `brew install chocolate-doom`, unzip a Freedoom
release into `../doom/`, and

```sh
scripts/dev/pspexp-doom
```

puts Chocolate Doom on the PSP with its controls mapped (nub and d-pad
move, cross fires, circle uses, L / R strafe, start is the menu).
`scripts/dev/pspexp-tyrian` does the same for OpenTyrian (built from source,
freeware data; see `compatibility/sdl-pspexp/README.md`); it plays at its
full 35 fps on the PSP too. On the
PSP-1000 over USB it runs at Doom's full 35 fps: 160 KB of frame texture
per frame, GE 2.2 ms, PSP CPU 0.06 ms, audio with no underruns.

## Raspberry Pi as the host

`scripts/pi/build.sh` installs the Debian packages (including Chocolate
Doom and Freedoom), builds the host tools and the SDL layer, adds a udev rule for the PSP,
and prints the run commands. Tested on a Raspberry Pi 400 over SSH with no
desktop: the USB link measures the same 25 MB/s as on the Mac, and
`scripts/dev/pspexp-doom` plays Doom on the PSP at 35 fps with clean audio. The
launcher picks SDL's dummy drivers when there is no display, and the SDL
layer paces the game's audio itself on such a host.

The Pi also runs the PSPExp launcher: `HOST=user@host scripts/pi/install.sh`
(from the Mac) sets the Pi up for PSPExp's package repository and installs
PSPExp from it as `pspexp-core`, so later updates come through Apps >
Packages; it adds the console as a systemd service with the launcher, and
packages go to `/opt/pspexp`. Without a repository address
(`~/.pspexp/repo-url`) it builds the source on the Pi and installs into
`/usr/local` instead. Games and emulators: Apps > Packages on the PSP, or
`pspexp-pkg install freedoom opentyrian` on the Pi (recipes installed through
apt, docs/Package_Manager.md). The Pi's own files (the service, the USB and
sudo rules, `/opt/pspexp`) are `scripts/pi/board-files.sh`, shared with the
card image.

**The card image**, `scripts/pi/image/build-image.sh` (Docker or OrbStack):
Raspberry Pi OS Lite (64-bit) as Raspberry Pi publishes it, pinned by date
and SHA-256, with PSPExp installed from its apt repository in an arm64
container, the `pi` user (password `pspexp`) and the Pi's own files; written
to a card, the console starts at the first boot. Its firmware, kernel and
first boot (growing to the card, SSH keys, cloud-init for Imager's settings)
are Raspberry Pi's own (docs/Package_Manager.md §8).

### Your own package repository

Apps > Packages reads a package repository: the signed catalogue of
recipes and the packages PSPExp builds itself. The public one is
`https://pkg.pspexp.dev` (Cloudflare R2, `apt-repo push r2`; its address in
`scripts/packages/public-repo-url`, which the installers and the image
take when `~/.pspexp/repo-url` is not set). A setup of your own, for
packages you build yourself:

1. **The packages**, into `build/debs`:
   - the console: `HOST=user@<pi> scripts/pi/install.sh build`, then
     `scripts/packages/debs/pspexp-core/build build/debs arm64 user@<pi>`
     (for the GameShell: `scripts/gameshell/build.sh`, then
     `scripts/packages/debs/pspexp-core/build build/debs armhf`);
   - the PSP's runtime: `scripts/build.sh`, then
     `scripts/packages/debs/pspexp-runtime/build build/debs`;
   - the games PSPExp builds itself: `scripts/packages/debs/<name>/build
     build/debs ARCH`, after the `scripts/packages/build-*.sh` its header
     names. A recipe whose `apt=` names only Debian's packages needs nothing.
2. **The repository:** `scripts/packages/apt-repo build build/debs` writes
   `build/apt`, signed with a key in `~/.pspexp/gnupg` (made the first time;
   its public half is written into `scripts/packages/pspexp-archive-key.asc`,
   which the board installers give the boards: your key, not the project's,
   so leave that change out of commits).
3. **Serving it** over plain HTTP: from another machine
   (`scripts/packages/apt-repo push user@host` copies it to `~/pspexp-repo`
   there, for any static web server), or from the computer itself:
   `python3 -m http.server 8093 -d build/apt`.
4. **Its address** (`http://<host>:8093`) as the first line of
   `~/.pspexp/repo-url`; then `HOST=user@<pi> scripts/pi/install.sh` sets the
   Pi up for it, and Packages offers the recipes.

Without a repository the Pi installer builds PSPExp on the Pi (installing
what the build needs first) and puts the console, both SDL layers and the
launcher in `/usr/local`: the launcher works, Packages has nothing to offer.

## Onion Omega2+ as the host

`scripts/omega/build.sh` cross-compiles static soft-float MIPS binaries of
the bench and the sprite demo on the Mac with Zig (libusb built from
source), so the Omega needs nothing installed. From an Omega2+ the frame
loop and the sprite demo run at 60 fps up to 5,000 sprites, with the
Omega's CPU about 90 % idle. Large transfers reach 21 MB/s; small ones are
slower than on the Mac or the Pi because each USB transfer costs the Omega
about four times as much.

The Omega also runs the PSPExp launcher: `scripts/omega/install.sh` puts
the console (`pspexp-term --launcher`), its fonts, the package manager and
the init script on it, everything that grows on the SD card
(`/mnt/mmcblk0p1/pspexp`: packages in `opt/`, settings and box art in
`home/`). Its packages are our own static builds (a Chocolate Doom with the
SDL layer built in and tuned for the FPU-less CPU, OpenTyrian, emulators),
offered through the same recipes as on the other boards:
`HOST=root@<omega> scripts/packages/pspexp-pkg-build build freedoom ...` and
`publish` put the builds and the catalogue on it, then Apps > Packages
installs them. The
launcher starts in about 2 s there and uses under 4 MB of memory.

## Emulators on the launcher

`tools/pspexp-retro` is a libretro player that is a plain SDL2 program: one
core, one ROM, no menus. The launcher is the menu (a rail tile per system,
ROM lists, box art); the SDL layer puts the player on the PSP like any other
game, at the core's native size, scaled by the PSP's GE, 256-colour pages as
CLUT8. Packages bring the player, a core and a `.system` file: the NES with
Debian's cores where Debian has them (`recipes/nes`, `recipes/snes`, ...),
PSPExp's own builds elsewhere; ROMs go in `~/roms/<system>`.
Details: docs/Launcher_and_Boards.md, section 6.

## ClockworkPi GameShell as the host

The GameShell becomes a PSPExp console: its own launcher, games and apps are
removed, and the PSP shows the PSPExp launcher with **packages** that the
PSP draws itself (docs/Launcher_and_Boards.md).

- `scripts/gameshell/build.sh` cross-compiles the tools (static ARMv7) and the
  SDL layer (for the image's own SDL2 programs).
- Games and emulators come from the recipes in `recipes/`, installed through
  apt (Apps > Packages, or `pspexp-pkg install ID` on the GameShell); each one's
  launcher files go to `/opt/pspexp/apps`, and the launcher picks them up by
  itself (docs/Package_Manager.md).
- **The card image** replaces ClockworkOS: `scripts/gameshell/base/build-kernel.sh`,
  `build-uboot.sh` and `build-image.sh` build a minimal Debian 13 with mainline
  Linux 6.18 and U-Boot, our own device tree for the board (Wi-Fi, Bluetooth,
  the LEDs), and PSPExp from its apt repository; the console starts at boot,
  so a PSP running PSPExp connects with nothing to set up. No games: they come
  from Apps > Packages (docs/Package_Manager.md §8). Nothing is taken from a
  ClockworkOS image.
- `scripts/gameshell/install.sh` puts the same files on a running GameShell
  over SSH (the list is `scripts/gameshell/stage.sh`).

With the PSP on the OTG port the USB link measures 24–26 MB/s, the sprite
demo runs at 60 fps up to 15,000 sprites, and Freedoom and OpenTyrian play
at 35 fps. `pspexp-wifi-mode psp|home` (also in the launcher's System page)
switches between the GameShell's own access point for the PSP and the home
Wi-Fi.

## The PSP as a console: `pspexp-term`

`tools/pspexp-term` makes the PSP a host's console: a shell in a
pseudo-terminal, drawn on the PSP as an 80x34 VT100 / xterm-subset terminal
(colours, scrollback, alternate screen for `vi` and `top`), typed on with
an on-screen keyboard. The bell beeps on the PSP and raw 44.1 kHz stereo
written to `/tmp/pspexp-audio` plays there. On Linux a real keyboard and
mouse (the Raspberry Pi 400's own, or USB ones on the Omega's dock) type into
the same terminal and are grabbed while the PSP is connected. On a Pi,
`scripts/pi/install.sh` installs it with the launcher (see above), or
`scripts/pi/install-console.sh` as a plain console service for your user. On the Omega2+ it starts at boot
(`/etc/init.d/pspexp-term`) and waits for a PSP on USB; running a game from
it (`doom`) hands the PSP to the game and takes it back afterwards
(`pspexp-exec`).

Its buttons: the manual, [The terminal](manual/using-pspexp.md#the-terminal).

The console also has a two-panel file manager (Apps > Files in the
launcher, or Files here from the terminal's menu). It can copy, move,
delete, rename and create folders; it shows pictures, reads text and
hands music to Apps > Music (folders, a queue, shuffle and repeat, playing
on behind the launcher; the PSP's note button pauses it; MP3 is decoded by
the PSP itself, on its Media Engine);
Settings > Sound sends all sound to Bluetooth headphones paired with
the board instead, set with the PSP's volume buttons;
opening a ROM starts it in its emulator, and a package gets installed.
Each panel's drive button drops down every drive: the board's storage
mounts and **the PSP's own memory stick**, reached over the link while
the console runs (`tools/pspexp-ms` does the same from a shell). Buttons
and details: docs/Launcher_and_Boards.md §7.

## Layout

```text
protocol/            wire format, shared by host and PSP builds (plain C99)
runtime/             command processor and resource cache, shared by all executors
host/                the host side: libpspexp (application API), executor-null
                     (validating, no rendering), executor-sdl (a PSP in a window),
                     transport-usb (libusb), transport-tcp (Wi-Fi)
psp/                 the PSP side: runtime (pspexp_runtime.prx), executor-ge (the GE),
                     transport-usb (kernel USB driver pspexp_usb.prx), transport-tcp,
                     boot (EBOOT loader)
compatibility/       existing programs on the PSP: sdl-pspexp (the SDL layer: any
                     SDL2 program), war1gus and wargus (the Warcraft packages'
                     patches)
tools/               pspexp-term (the PSP as a console, with the launcher and a
                     file manager whose drives include the PSP's memory stick),
                     pspexp-retro (emulator player), pspexp-sdl-server (a stand-in
                     PSP for screenshots; PSPEXP_SERVER_TEXT=TEXT answers a game's
                     keyboard request with TEXT, as if typed; PSPEXP_SDL_SHOT=FILE.bmp
                     saves every 30th frame, with a %d in the name every frame,
                     numbered, or every PSPEXP_SDL_SHOT_EVERY-th), pspexp-ms (the memory stick from a
                     shell), usb-bench (pspexp-bench)
examples/            sdl-game (SDL2 test game), sprite-demo
tests/               unit tests (ctest)
scripts/             building, installing and packaging, one folder per board
                     (scripts/README.md)
docs/                architecture, the launcher and boards, the cheatsheet
roms/                test ROMs, git-ignored: never committed or bundled
```
