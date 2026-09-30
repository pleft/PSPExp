# PSPExp — PSP Expansion Port

Turns a Sony PSP into a hardware-accelerated graphics, audio, display and
input processor for an application running on another computer. The host
streams *rendering intent* (compact sprite batches), the PSP renders it with
its own GE. Architecture: [docs/PSPExp_Technical_Architecture.md](docs/PSPExp_Technical_Architecture.md).

## Status (v0.1, in progress)

| piece | state |
|---|---|
| `protocol/` framing, opcodes, batch records + expansion, texture rules | done, unit-tested |
| `runtime/` command processor + two-tier cache (shared by every executor) | done, unit-tested |
| `host/libpspexp` application API, frame builder, pacing, reconnect replay | done, unit-tested |
| `host/executor-null` | done |
| `host/transport-usb` (libusb, reader thread) | **runs on hardware**: 25 MB/s, 0.25 ms round trip (docs/v01-usb-bringup.md) |
| `psp/transport-usb` kernel driver `pspexp_usb.prx` | **runs on hardware** |
| `psp/runtime` runtime app + `psp/boot` loader | **runs on hardware**; 60 fps frame loop paced on PRESENT_DONE |
| `psp/executor-ge` GE executor (VRAM/RAM textures, vertex ring, GU_SPRITES) | **runs on hardware**: 60 Hz at 5,000 sprites |
| `host/executor-sdl` reference renderer (keyboard + game controller input) | done |
| `examples/sprite-demo` (§16 proof of concept; Null, SDL or USB; survives cable pull) | **done on hardware 2026-09-22**: the v0.1 decisive result |
| `tools/usb-bench` throughput / latency / frame-loop benchmark, concurrent-upload workload | done, measured on hardware 2026-09-22 |
| CLUT4/CLUT8 textures with palette resources (v0.3 item pulled forward) | done on Null/SDL, **not yet run on hardware** (`sprite-demo --clut`) |
| host-baked text: built-in 6x8 CLUT4 font, `pspexp_draw_text` (v0.3 item pulled forward) | done, shown in the demo HUD |
| per-batch palette, flip, 4444 alpha tints, translucent rects, lines, quad batches (v0.2 needs) | done, tested |
| audio: shared mixer, cached sounds + streamed PCM, SDL device and PSP audio thread (v0.3 item pulled forward) | done on Null/SDL, **not yet run on hardware** |
| TCP transport (host + PSP over Wi-Fi / PPSSPP), `DEBUG_SCREENSHOT`, `scripts/dev/run-ppsspp.sh` | done: bench, sprite demo and Warcraft run against the real PSP runtime under PPSSPP |
| **v0.2** Stratagus renderer backend (`compatibility/stratagus`, branch `pspexp` in ../WarCraft) | **Warcraft runs on the PSP over USB at 60 fps with audio**; PSP spends ~1 ms per frame |
| **v0.4** `compatibility/sdl-pspexp`: any unmodified SDL2 program on the PSP (`scripts/dev/pspexp-run`) | **runs on the PSP-1000 over USB 2026-09-22**: the test game at 60 fps, 3.7 KB/frame, PSP CPU 0.2–0.8 ms + GE 0.5–0.8 ms, audio streamed with no underruns; renderer and window-surface paths, controller/keyboard input |

## Build

```sh
scripts/build.sh          # host: build/host (CMake); PSP: dist/PSPExp if ../pspdev exists
```

Host needs CMake and libusb-1.0 (`brew install libusb`). The PSP side needs
the pspdev toolchain at `../pspdev` (or `PSPDEV=...`).

## Measure the USB link (v0.1 first milestone)

1. Put PSPExp on the memory stick (custom firmware required: the boot module
   loads a kernel PRX). With the PSP in USB mode (Settings > USB Connection):
   `scripts/packages/runtime-slot --install /Volumes/<stick>/PSP/GAME/PSPExp`
   writes the boot loader, the runtime in `slot-a/` and `active`. This is
   needed only once: after that a board's console updates the runtime over
   the link (the cheatsheet's "Updating the PSP's runtime"). Copying
   `dist/PSPExp/` flat still works too; the loader then runs the runtime
   next to the EBOOT, without slots.
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
memory stick. See `docs/v01-usb-bringup.md`.

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

`compatibility/stratagus/README.md` describes the Stratagus renderer backend
and how to build the engine with it. Then, with the PSP running `dist/PSPExp`:

```sh
cd ../WarCraft
PSPEXP=usb build/host-stratagus/stratagus -d dist/War1gus -r "maps/4-middle-ground.smp"
```

The game runs on the Mac; the PSP renders it, plays its sound, and its
controls play it, from the title screen on.

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
not: rotation, shaders) and the environment variables. Render targets are
replayed, so pixel-art ports that upscale through one work too. OpenGL 1.x
programs are drawn by the PSP's GE (`build/host/gl-demo` to try it;
Chromium B.S.U. and Neverball from Debian run).

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
Doom and Freedoom), builds the host tools and the SDL layer (and Stratagus
when a WarCraft checkout is next to PSPExp), adds a udev rule for the PSP,
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
apt, docs/Package_Manager.md).

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

| PSP | console |
|---|---|
| cross / square / circle / triangle | the highlighted key / Backspace / Space / Enter |
| start | Tab |
| L held / R held | Shift / Ctrl |
| L + triangle / R + square | Esc / Ctrl-C |
| select | show / hide the keyboard (held: the launcher) |
| d-pad | move on the keyboard; arrow keys when it is hidden |
| R + d-pad, R + nub | scroll back (a line or a page) |
| L + R + left / right | the other shells (up to four) |
| L + square / L + circle | copy mode / paste |
| L tapped | the context menu: keys, commands, Files here, themes, fonts, the button list |
| host keyboard (Linux) | types directly (US layout); Shift + PgUp / PgDn scroll back |
| host mouse (Linux) | wheel scrolls back; clicks go to programs that ask for the mouse |

The console also has a two-panel file manager (Apps > Files in the
launcher, or Files here from the terminal's menu). It can copy, move,
delete, rename and create folders; it shows pictures and reads text;
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
                     SDL2 program), stratagus (Warcraft's renderer backend)
tools/               pspexp-term (the PSP as a console, with the launcher and a
                     file manager whose drives include the PSP's memory stick),
                     pspexp-retro (emulator player), pspexp-sdl-server (a stand-in
                     PSP for screenshots), pspexp-ms (the memory stick from a
                     shell), usb-bench (pspexp-bench)
examples/            sdl-game (SDL2 test game), sprite-demo
tests/               unit tests (ctest)
scripts/             building, installing and packaging, one folder per board
                     (scripts/README.md)
docs/                architecture, the launcher and boards, the cheatsheet
roms/                test ROMs, git-ignored: never committed or bundled
```
