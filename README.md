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
| TCP transport (host + PSP over Wi-Fi / PPSSPP), `DEBUG_SCREENSHOT`, `scripts/run-ppsspp.sh` | done: bench, sprite demo and Warcraft run against the real PSP runtime under PPSSPP |
| **v0.2** Stratagus renderer backend (`compatibility/stratagus`, branch `pspexp` in ../WarCraft) | **Warcraft runs on the PSP over USB at 60 fps with audio**; PSP spends ~1 ms per frame |
| **v0.4** `compatibility/sdl-pspexp`: any unmodified SDL2 program on the PSP (`scripts/pspexp-run`) | **runs on the PSP-1000 over USB 2026-09-22**: the test game at 60 fps, 3.7 KB/frame, PSP CPU 0.2–0.8 ms + GE 0.5–0.8 ms, audio streamed with no underruns; renderer and window-surface paths, controller/keyboard input |

## Build

```sh
scripts/build.sh          # host: build/host (CMake); PSP: dist/PSPExp if ../pspdev exists
```

Host needs CMake and libusb-1.0 (`brew install libusb`). The PSP side needs
the pspdev toolchain at `../pspdev` (or `PSPDEV=...`).

## Measure the USB link (v0.1 first milestone)

1. Copy `dist/PSPExp/` to `ms0:/PSP/GAME/PSPExp/` (custom firmware required:
   the boot module loads a kernel PRX).
2. Launch PSPExp from the XMB and connect the USB cable. The screen shows
   `waiting for host sync`.
3. On the host: `build/host/pspexp-bench` (add `--verbose` for libusb errors).

The tool prints host→PSP and PSP→host throughput for several block sizes,
round-trip latency for 16 B, 4 KiB and 40 KB messages, and a 300-frame loop
with 500 sprites paced on PRESENT_DONE. `pspexp-bench --null` runs the same
against the in-process Null executor as a CPU baseline.

## Without a PSP: the runtime under PPSSPP

```sh
scripts/run-ppsspp.sh                                   # boots dist/PSPExp in the emulator (needs EnableWlan = True)
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
scripts/pspexp-run build/host/sdl-game        # the bundled SDL2 test game, on the PSP
scripts/pspexp-run /path/to/any/sdl2/game     # yours
```

`compatibility/sdl-pspexp/README.md` lists what is mirrored (and what is
not: rotation, OpenGL) and the environment variables. Render targets are
replayed, so pixel-art ports that upscale through one work too.

Doom is the showcase: `brew install chocolate-doom`, unzip a Freedoom
release into `../doom/`, and

```sh
scripts/pspexp-doom
```

puts Chocolate Doom on the PSP with its controls mapped (nub and d-pad
move, cross fires, circle uses, L / R strafe, start is the menu). On the
PSP-1000 over USB it runs at Doom's full 35 fps: 160 KB of frame texture
per frame, GE 2.2 ms, PSP CPU 0.06 ms, audio with no underruns.

## Raspberry Pi as the host

`scripts/build-pi.sh` (untested on a Pi yet) installs the Debian packages,
builds the host tools and Stratagus with the backend, adds a udev rule for
the PSP, and prints the headless run command.

## Layout

```text
protocol/            wire format, shared by host and PSP builds (plain C99)
runtime/             command processor and resource cache, shared by all executors
host/libpspexp/      application API
host/executor-null/  validating executor, no rendering
host/transport-usb/  libusb transport
psp/transport-usb/   kernel USB driver (pspexp_usb.prx) + user stubs
psp/runtime/         PSP runtime application (pspexp_runtime.prx)
psp/boot/            EBOOT loader
compatibility/stratagus/  Stratagus renderer backend: patch + notes (v0.2)
compatibility/sdl-pspexp/ injected library: any SDL2 program on the PSP (v0.4)
examples/sdl-game/        plain SDL2 test game for it (renderer and window-surface paths)
tools/usb-bench/     transport benchmark
tests/               unit tests (ctest)
docs/                architecture
```
