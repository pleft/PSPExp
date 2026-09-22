# v0.1 USB bring-up: first contact and throughput

Goal: a real host <-> PSP link over USB and the measured throughput that
replaces the 8 MB/s planning assumption in the architecture (section 3.2).

## What runs where

```text
host: pspexp-bench / sprite-demo ── libpspexp ── transport-usb (libusb) ── USB ── pspexp_usb.prx (kernel)
                                                                                      │ byte stream
                                                                                pspexp_runtime.prx (user)
                                                                                      │ runtime core
                                                                               GE executor / status page
```

`pspexp_runtime.prx` is the PSP runtime: it handshakes, validates uploads,
expands batches, renders through the GE executor and answers PRESENT_DONE.
Its status page (USB, link and runtime counters) shows while no host is
connected; the GE owns the screen once frames arrive.

## Steps

1. `scripts/build.sh` — host build + tests, then `dist/PSPExp/`.
2. Copy `dist/PSPExp/` to `ms0:/PSP/GAME/PSPExp/` (three files: `EBOOT.PBP`,
   `pspexp_usb.prx`, `pspexp_runtime.prx`). Custom firmware is required: the
   boot module runs its main thread in kernel mode to load the driver, exactly
   like PSPLink's bootstrap.
3. Launch PSPExp from the XMB. Expected screen:
   `PSPExp boot` briefly, then the runtime status page with
   `waiting for USB cable`.
4. Plug the cable into the Mac. The status should change to
   `attached, waiting for host sync` and `attach 1`. The PSP enumerates as
   Sony VID `054c` PID `01ca`; `system_profiler SPUSBDataType` lists it.
5. Run `build/host/pspexp-bench`. It prints `link synchronised`, the PSP shows
   `CONNECTED` with `sync 1`, then the four benchmark sections run
   (about 30 s at 4 MiB per run; `--mb 16` for steadier numbers).
6. Pull the cable mid-run and re-plug: the PSP should show `detach 1`, go back
   to waiting, and a fresh `pspexp-bench` run should connect again (`sync 2`).

## Second run: the sprite demo (GE executor)

Rebuild, copy `dist/PSPExp/` again (the runtime now contains the GE
executor), launch, plug in, then:

```sh
build/host/pspexp-sprite-demo
```

Expected: the PSP's status page is replaced by a dark blue field of drifting
white stars, a red disc you move with the d-pad or analog stick, and a HUD
bar at the bottom. Cross changes the shape, circle the tint, square hides the
field, triangle cycles the field size (100 / 500 / 2000 / 5000 sprites).
The host prints one line per second; `input->done` is the input-to-present
latency on the PSP clock (sample timestamp to the vblank that showed the
frame that consumed it), the number the ≤ 50 ms target is about, minus LCD
scan-out.

Pull the cable: the PSP returns to its status page (`detach 1`), the host
prints `link lost; waiting for the PSP...`; re-plug and it prints
`reconnected; atlas replayed` and the sprite continues from where it was.

## Third run: head-of-line blocking (§17 workload c, §19 question 1)

```sh
build/host/pspexp-bench --frames-only --frames 600 --upload 256 --upload-kib 512
build/host/pspexp-bench --frames-only --frames 600 --upload 1024 --upload-kib 256
```

Section [5] repeats the frame loop while a texture upload stream shares the
ordered command stream. It reports how many frames arrived later than one
vblank (a missed present) and later than two, plus `ge` time now carried in
PRESENT_DONE. The question the numbers answer: does a 512 KB upload ahead of
a frame cost one vblank at 25 MB/s (accept the single stream for v0.2) or
more (split into a bulk stream with fences, §19)?

## Reading the numbers

- `host->psp ... MB/s` is the figure section 3.3 is parameterised on. The
  block sizes 4 KiB / 16 KiB / 64 KiB bracket a frame (about 4 KB for 500
  sprites, 40 KB for 5,000) and a texture (256 KiB+).
- `echo 16 B` is the pure round trip; `echo 40000 B` is a 5,000-sprite frame
  round trip. Compare with the 1-5 ms transfer line in the latency budget.
- The frame loop's `present->PRESENT_DONE` is the host-observed time for a
  frame to be transferred, executed and reach a vblank; subtract the
  `psp execute` part to see transport + vblank wait.
- Also note `usb: ... splits` on the host: transfers that ended on a
  512-byte boundary and were split (see protocol/pspexp_usb_link.h).

Record the results in this file under "Measurements" and update section 3.2
/ 3.3 of the architecture with the real figure.

## If it does not connect

| symptom | check |
|---|---|
| `pspexpUsbStart: 0x8...` on the PSP | the kernel driver did not register; run once under PSPLink? no: PSPLink owns USB. Check CFW and that `pspexp_usb.prx` sits next to the EBOOT |
| stays at `waiting for USB cable` after plugging | `attach` never fired: try another cable, or `sceUsbActivate` failed (status printed at start) |
| host: `no USB device 054c:01ca` | device not enumerated; check `system_profiler SPUSBDataType` |
| host: `claim interface` fails | another process (usbhostfs_pc) holds it |
| host: `no sync reply` | driver received the sync but the reply did not go out; PSP `sync` counter tells which half |
| `no HELLO within 5 s` | link is up but `pspexp_runtime.prx` is not running or did not see the generation change |
| throughput far below 8 MB/s at 64 KiB blocks | full-speed enumeration (`packet 64`) or the receive ring starving: watch `rx xfers` vs `tx xfers` |

## Measurements

### 2026-09-22 — PSP-1000, MacBook Air (macOS), first hardware run

Link came up first try: hi-speed (512-byte packets), sync, HELLO, 4 MiB per run.

| direction | block | MB/s (host-timed) | MB/s (PSP-timed) |
|---|---|---|---|
| host → PSP | 4 KiB | 13.25 | 13.29 |
| host → PSP | 16 KiB | 19.64 | 19.78 |
| host → PSP | 64 KiB | 25.04 | 26.13 |
| host → PSP | 256 KiB | 25.96 | 28.37 |
| PSP → host | 4 KiB | 11.31 | 11.32 |
| PSP → host | 16 KiB | 17.33 | 17.39 |
| PSP → host | 64 KiB | 22.78 | 23.24 |
| PSP → host | 256 KiB | 24.28 | 26.26 |

| round trip | min | median | mean | p95 | max |
|---|---|---|---|---|---|
| 16 B | 209 µs | 246 µs | 249 µs | 290 µs | 408 µs |
| 4 KiB | 552 µs | 587 µs | 616 µs | 632 µs | 6.3 ms |
| 40 KB | 3.79 ms | 3.91 ms | 4.00 ms | 4.01 ms | 9.7 ms |

Frame loop, 300 frames × 500 sprites (4,144 B/frame), one frame in flight:
60.1 fps, present → PRESENT_DONE mean 16.6 ms, max 22.6 ms, PSP execute
0.17 ms, 405 input samples received (60 Hz), no errors, no timeouts.

Notes:

- Large-transfer throughput is ≈ 25 MB/s, three times the 8 MB/s planning
  figure. Small messages are dominated by per-transfer cost (~0.1–0.15 ms per
  direction), which is why 4 KiB blocks reach only 13 MB/s.
- Caveat on the 4 KiB / 16 KiB / 64 KiB rows: each benchmark message is
  exactly a 512-byte multiple, so every one was split into two transfers
  (1,286 splits reported). Real frames go out as one transfer per frame and
  are rarely on a boundary, so those rows understate the frame path slightly.
- The 16.6 ms frame figure is the vblank wait, not the link: the runtime
  presents with `sceDisplayWaitVblankStart`, so a frame that arrives just
  after a vblank waits for the next one. Section 3.3 updated accordingly.

### 2026-09-22 — sprite demo on hardware (GE executor), same setup

The §16 proof of concept ran first try: atlas uploaded once, GE rendering,
player moved from the PSP's d-pad and analog stick, field size cycled with
triangle, two cable pulls with automatic reconnect and atlas replay.

| field | bytes/frame | batches | fps | PSP execute (parse + expand + list) | input → PRESENT_DONE |
|---|---|---|---|---|---|
| 500 sprites | 4,352 | 4 | 59.9 | 0.25 ms | 16.67 ms |
| 2,000 sprites | 16,448 | 6 | 59.9 | 0.71 ms | 16.67 ms |
| 5,000 sprites | 40,384 | 5 | 59.9 | 1.85 ms | 16.67 ms |

Notes:

- 60 Hz held at every field size, including 5,000 sprites (40 KB/frame,
  2.4 MB/s, a tenth of the link). PSP-side cost is ≈ 0.37 µs per sprite,
  about 120 cycles at 333 MHz including message parsing, in line with the
  §5.4 estimate. GE render time is not in `PRESENT_DONE` yet (only on the
  PSP's status page as `last ge`).
- `input → PRESENT_DONE` is exactly one frame period because the runtime
  samples the controller right after each vblank and the host's next frame
  lands at the following vblank. Adding LCD scan-out (~8 ms) gives
  ≈ 25 ms input-to-photon for a sample, and a real button press adds 0–16 ms
  of sampling phase on top: ≈ 25–41 ms, mean ≈ 33 ms, inside the ≤ 50 ms
  target of §2.2 with margin.
- Reconnect after a cable pull took under a second; the host log line right
  after shows a low fps only because the waiting time is averaged in.
