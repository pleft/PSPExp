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
| `tools/usb-bench` throughput / latency / frame-loop benchmark | done, measured on hardware 2026-09-22 |

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

## Sprite demo (the §16 proof of concept)

```sh
build/host/pspexp-sprite-demo            # real PSP over USB (waits for it)
build/host/pspexp-sprite-demo --sdl      # SDL window, arrows/IJKL move, Z X A S = cross circle square triangle
build/host/pspexp-sprite-demo --null --frames 300
```

One atlas is uploaded once; the host owns the player object and moves it from
the PSP's own controls over a field of background sprites (`--field N`, or
triangle to cycle 100/500/2000/5000). Pull the cable: the demo waits, then
replays the atlas and resumes. It prints fps, bytes per frame and the
input-to-present latency once a second.

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
tools/usb-bench/     transport benchmark
tests/               unit tests (ctest)
docs/                architecture
```
