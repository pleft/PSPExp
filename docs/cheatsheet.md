# PSPExp cheatsheet

Paths are relative to the PSPExp tree; the WarCraft project is `../WarCraft`.

## Build and install

| what | command |
|---|---|
| build everything (host + PSP) and run the tests | `scripts/build.sh` |
| PSP files to copy | `dist/PSPExp/` → `ms0:/PSP/GAME/PSPExp/` (EBOOT.PBP, pspexp_usb.prx, pspexp_runtime.prx) |
| rebuild Stratagus with the backend | `cd ../WarCraft && cmake --build build/host-stratagus -j8 --target stratagus` |
| regenerate the Stratagus patch after editing the `pspexp` branch | `scripts/export-stratagus-patch.sh` |

Only `pspexp_runtime.prx` changes often; the EBOOT and the USB driver rarely.
The PSP must be in the XMB (or the runtime stopped) to be seen as a memory
stick; while the runtime runs it is the PSPExp USB device.

## On the PSP

1. Launch **PSPExp** from the XMB. Status page: `waiting for USB cable or TCP`.
2. Plug the cable: `attached, waiting for host sync`. Start a host program:
   `CONNECTED (USB)`, then the GE takes the screen.
3. **HOME** exits the runtime. Pulling the cable returns to the status page;
   plugging back in resumes any host program that is still running.
4. Diagnostics on the memory stick, next to the EBOOT: `pspexp-rt.log`
   (link state changes) and `pspexp-shot-N.bmp` (framebuffer dumps when a
   host asked for them).

The status page shows USB counters, link generation, frames, GE and cache
statistics, audio buffer state and the last log lines. It comes back after a
cable pull, so pull the cable to read it after a session.

## On the host

| goal | command |
|---|---|
| link check + throughput + latency + frame loop | `build/host/pspexp-bench` |
| frame loop only, with a concurrent texture upload stream | `build/host/pspexp-bench --frames-only --frames 600 --upload 256 --upload-kib 512` |
| the sprite demo (5551 atlas) | `build/host/pspexp-sprite-demo` |
| the sprite demo with CLUT8 atlas + CLUT4 text | `build/host/pspexp-sprite-demo --clut` |
| drop and reopen the link every 90 frames (reconnect soak) | `build/host/pspexp-sprite-demo --usb --test-reconnect 90` |
| ask the PSP to dump its framebuffer every N frames | `... --shot 120` (demo) or `PSPEXP_SHOT_EVERY=300` (Warcraft) |
| Warcraft on the PSP, main menu | `cd ../WarCraft && PSPEXP=usb build/host-stratagus/stratagus -d dist/War1gus` |
| Warcraft straight into a map | `... -r maps/4-middle-ground.smp` |
| Warcraft with the Mac window shown side by side (default hides it when a PSP is the target) | add `PSPEXP_WINDOW=show` before the command |
| Warcraft on a headless host (no display, no sound card: Raspberry Pi, Omega2+) | `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy PSPEXP_WINDOW=hide PSPEXP=usb build/host-stratagus/stratagus -d dist/War1gus` |
| Warcraft without a PSP (executor window) | `PSPEXP=sdl build/host-stratagus/stratagus -d dist/War1gus` |
| PSP runtime in PPSSPP, then any host program with `--tcp 127.0.0.1:3838` / `PSPEXP=tcp:127.0.0.1:3838` | `scripts/run-ppsspp.sh` |
| **any unmodified SDL2 program on the PSP** (v0.4) | `scripts/pspexp-run <program> [args]` |
| the SDL2 test game on the PSP (renderer path / window-surface path) | `scripts/pspexp-run build/host/sdl-game` / `... --surface` |
| an SDL2 program without a PSP (executor window) | `PSPEXP=sdl scripts/pspexp-run <program>` |
| **Doom on the PSP** (Chocolate Doom + Freedoom, PSP controls mapped) | `scripts/pspexp-doom` |
| **Tyrian on the PSP** (OpenTyrian built in ../tyrian, freeware data) | `scripts/pspexp-tyrian` |
| **the PSP as the host's console** (shell, on-screen keyboard; runs at boot on the Omega) | `build/host/pspexp-term` |
| install the console as a service on a Pi (its keyboard and mouse type into it) | `scripts/install-pspexp-term.sh` (`remove` to uninstall) |
| GameShell: build the tools and the SDL layer / OpenTyrian (compiled on the GameShell) | `scripts/build-gameshell.sh` / `GAMESHELL=cpi@<ip> scripts/build-gameshell.sh tyrian` |
| GameShell: an image SDL2 game on the PSP (headless) / Doom / Tyrian | `pspexp-run <program>` / `pspexp-run chocolate-doom -iwad ~/games/ChocoDM/freedoom1.wad` / `pspexp-tyrian` |
| GameShell: its own access point for the PSP / back to the home Wi-Fi | `sudo pspexp-wifi-mode psp` / `sudo pspexp-wifi-mode home` |

Every host program prints a stats line: fps, bytes/frame, batches, sprites,
input-to-present latency, PSP CPU and GE time, uploads, resident surfaces,
errors, audio frames sent / buffered / underruns.

Pointer speed: `PSPEXP_POINTER_SPEED=150` (pixels per second at full nub
deflection; default 200, the native port used 260). L holds slow, R fast.

## Any SDL2 game (v0.4, `compatibility/sdl-pspexp`)

```sh
scripts/pspexp-run /path/to/game            # PSP over USB; the game's window stays on the host
PSPEXP_KEYS=cross=space,circle=lctrl scripts/pspexp-run /path/to/game   # remap PSP buttons to keys
PSPEXP_NUB=mouse scripts/pspexp-run /path/to/game                        # nub = mouse, cross/circle = clicks
PSPEXP_LOCAL_AUDIO=0 scripts/pspexp-run /path/to/game                    # sound only on the PSP
```

Doom: `brew install chocolate-doom`, unzip a Freedoom release
(https://freedoom.github.io/) into `../doom/`, then `scripts/pspexp-doom`.
Nub / d-pad move and turn, cross fires, circle uses, square runs, L / R
strafe, triangle Enter, start Escape (menu), select map. Sound comes out
of the PSP only (`PSPEXP_LOCAL_AUDIO=1` for both); `PSPEXP_WINDOW=hide`
hides the Mac window. Expected stats line: 35 fps, 4 batches, GE ≈ 2.2 ms.

Tyrian: build OpenTyrian in `../tyrian/opentyrian` and unzip tyrian21.zip
into `../tyrian/data/` (see `compatibility/sdl-pspexp/README.md`), then
`scripts/pspexp-tyrian`. Cross fires, circle / L and square / R sidekicks,
triangle Enter, start pause, select Escape (quits at the main menu).

The game sees a game controller named "PSP (PSPExp)" if it initialised
joysticks, and keyboard events otherwise (defaults: cross=z, circle=x,
square=a, triangle=s, L=q, R=w, start=return, select=escape, d-pad =
arrows). Rotation, render targets and OpenGL drawing are not mirrored;
see the README in `compatibility/sdl-pspexp/`.

## Sprite demo controls (PSP)

d-pad or nub: move · cross: shape · circle: tint · square: field on/off ·
triangle: field size 100/500/2000/5000 · start: reset.

## Warcraft controls (PSP), same as the native port

nub: pointer · cross: left click · circle: right click · square: shift ·
triangle: ctrl · d-pad: scroll · L / R: slow / fast pointer · start: menu (F10) ·
select: escape.

## What is worth testing next

1. **Play Warcraft for real**: a full skirmish from the title screen, sound
   from the PSP speaker, pointer feel with the nub. Compare with the native
   build's feel. Report the stats line if anything stutters.
2. **Cable pull mid-game**, wait a few seconds, plug back: the game should
   resume with its textures replayed (`reconnected` in the host log).
3. **Long session** (20+ minutes, many unit types): watch `resident` in the
   stats line and `resources errors` on the PSP status page afterwards.
4. **Sprite demo at 5,000 sprites** (triangle twice): should stay at 60 fps;
   the GE line in the stats shows ~9 ms.

## If something goes wrong

| symptom | look at |
|---|---|
| host says `no USB device 054c:01ca` | PSP not on the status page, or cable |
| host says `no sync reply` | runtime not running (XMB?) or stuck: HOME and relaunch |
| picture quartered / wrong colours | old `pspexp_runtime.prx` on the stick |
| host killed on cable pull | fixed 2026-09-22; rebuild the host tools |
| PSP status page shows `resource errors` | memory budget: 1.5 MiB VRAM + 12 MiB RAM; the backend evicts after 15 s |
| audio `underruns` climbing on the status page | host frame loop stalled; the stream needs the host to run at ≥ 30 fps |
