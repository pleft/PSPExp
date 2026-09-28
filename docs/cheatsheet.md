# PSPExp cheatsheet

Paths are relative to the PSPExp tree; the WarCraft project is `../WarCraft`.

## Build and install

| what | command |
|---|---|
| build everything (host + PSP) and run the tests | `scripts/build.sh` |
| PSP files to copy | `dist/PSPExp/` → `ms0:/PSP/GAME/PSPExp/` (EBOOT.PBP, pspexp_usb.prx, pspexp_runtime.prx) |
| rebuild Stratagus with the backend | `cd ../WarCraft && cmake --build build/host-stratagus -j8 --target stratagus` |
| regenerate the Stratagus patch after editing the `pspexp` branch | `scripts/dev/export-stratagus-patch.sh` |

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

## On the Mac (development: the build tree, a PSP on USB or a stand-in)

| goal | command |
|---|---|
| link check + throughput + latency + frame loop | `build/host/pspexp-bench` |
| frame loop only, with a concurrent texture upload stream | `build/host/pspexp-bench --frames-only --frames 600 --upload 256 --upload-kib 512` |
| the sprite demo (5551 atlas) / with CLUT8 atlas + CLUT4 text | `build/host/pspexp-sprite-demo` / `... --clut` |
| drop and reopen the link every 90 frames (reconnect soak) | `build/host/pspexp-sprite-demo --usb --test-reconnect 90` |
| ask the PSP to dump its framebuffer every N frames | `... --shot 120` (demo) or `PSPEXP_SHOT_EVERY=300` (SDL programs) |
| PSP runtime in PPSSPP, then any host program with `--tcp 127.0.0.1:3838` | `scripts/dev/run-ppsspp.sh` |
| a stand-in PSP (SDL executor on TCP 3838; `SDL_VIDEODRIVER=dummy`: screenshots only) | `build/host/pspexp-sdl-server` |
| **any unmodified SDL2 program on the PSP** | `scripts/dev/pspexp-run <program> [args]` (`--tcp:<host>:3838` for the stand-in) |
| the SDL2 test game (renderer path / window-surface path) | `scripts/dev/pspexp-run build/host/sdl-game` / `... --surface` |
| an SDL2 program without a PSP (executor window) | `PSPEXP=sdl scripts/dev/pspexp-run <program>` |
| **Doom / Tyrian on the PSP** | `scripts/dev/pspexp-doom` / `scripts/dev/pspexp-tyrian` |
| an emulator: one core, one ROM | `build/host/pspexp-retro CORE ROM` (through `scripts/dev/pspexp-run` for the PSP) |
| **the PSP as this machine's console** | `build/host/pspexp-term` |
| the PSP's memory stick from a shell (list, copy both ways, mkdir, rm, mv, free space) | `build/host/pspexp-ms [--tcp <host>] ls ms0:/PSP/GAME` / `get ms0:/X [file]` / `put file ms0:/PSP/GAME/X/` / `mkdir` / `rm` / `mv` / `df` |
| a stand-in PSP whose memory stick is a folder (file manager, `pspexp-ms`) | `PSPEXP_MS0=<folder> build/host/pspexp-sdl-server` |
| **integration tests on a real PSP** (on a board's console; report + the PSP's screenshots in `build/psp-tests/`) | `tests/psp/run.py --host <user@board> [link stick launcher files terminal]` |
| drive a running console by hand (buttons, state, screenshots, the stick) | on the board: `pspexp-term --ctl "press cross"` / `--ctl state` / `--ctl "shot NAME"` / `--ctl "fs ls ms0:/"` |
| benchmark the SDL layer's texture uploads (Doom timedemo, four modes) | `scripts/dev/bench-shim.sh` / `scripts/dev/bench-shim.sh --seconds 40 <board host>` |
| Warcraft on the PSP, main menu / straight into a map | `cd ../WarCraft && PSPEXP=usb build/host-stratagus/stratagus -d dist/War1gus` / `... -r maps/4-middle-ground.smp` |
| Warcraft with the Mac window shown / without a PSP | `PSPEXP_WINDOW=show ...` / `PSPEXP=sdl ...` |

## Any board: packages and ROMs

Every board script takes `HOST=user@host` (the board over SSH) and, for
packages, `BOARD=gameshell|omega|pi` (which build to package).

| goal | command |
|---|---|
| what can be packaged (recipes in `scripts/packages/recipes/`) | `scripts/packages/pspexp-pkg-build recipes` |
| build / install / remove / list packages | `BOARD=<board> HOST=<host> scripts/packages/pspexp-pkg-build install nes gameboy` / `remove ID` / `list` |
| copy the test ROMs (`roms/<system>/`, git-ignored) to a board | `scripts/packages/push-roms.sh <host> [nes gb ...]` |
| launcher screenshots from a board (stand-in PSP on the Mac) | board: `PSPEXP_LAUNCHER_OPEN=about pspexp-term --launcher --tcp <mac>:3838 --shot 1` (or `tz`, `wifi`, `password:<name>`) |
| installers ship the existing build: after a source change | the board's `build.sh` first (the Pi's `install.sh` builds on the Pi) |
| on a board: an SDL2 program on the PSP / packages | `pspexp-run <program>` (glibc boards) / `pspexp-pkg list` |

## GameShell (`scripts/gameshell/`)

| goal | command |
|---|---|
| build the tools and the SDL layer | `scripts/gameshell/build.sh` |
| OpenTyrian / the emulator player and a core, compiled on the GameShell | `HOST=cpi@<ip> scripts/gameshell/build.sh tyrian` / `CORE=quicknes HOST=cpi@<ip> scripts/gameshell/build.sh retro` |
| Cave Story's engine, cross-compiled here | `scripts/gameshell/build.sh cavestory` |
| any cross-built game for the GameShell or Pi | `scripts/packages/cross-glibc.sh gameshell\|pi scripts/packages/build-openjazz.sh openjazz` |
| install or update a running GameShell | `HOST=cpi@<ip> scripts/gameshell/install.sh` |
| the card image on the fresh base: kernel / image (Debian 13, Linux 6.18) | `scripts/gameshell/base/build-kernel.sh` / `scripts/gameshell/base/build-image.sh clockworkos_v0.6.img [--ssh-key F] [--wifi-from cpi@<ip>]` |
| (older) prepare a stock ClockworkOS image | `scripts/gameshell/prep-stock-image.sh <image> [--wifi]` |
| on the board: its own access point for the PSP / back to the home Wi-Fi | `sudo pspexp-wifi-mode psp` / `sudo pspexp-wifi-mode home` |
| on the board: the first-boot log | `cat /var/log/pspexp-firstboot.log` |

## Omega2+ (`scripts/omega/`)

| goal | command |
|---|---|
| build the tools / Doom / OpenTyrian / an emulator (static, SDL layer linked in) | `scripts/omega/build.sh` / `... doom` / `... tyrian` / `CORE=quicknes scripts/omega/build.sh retro` |
| Cave Story's / Jazz's engine (static, SDL layer linked in) | `scripts/omega/build.sh cavestory` / `... jazz` |
| install or update (the console, launcher, fonts, package tool) | `HOST=omega scripts/omega/install.sh` |
| packages that suit it: Freedoom, OpenTyrian, NES (30 fps), Game Boy | `BOARD=omega HOST=omega scripts/packages/pspexp-pkg-build install freedoom opentyrian nes gameboy` |

## Raspberry Pi (`scripts/pi/`)

| goal | command |
|---|---|
| install the console and launcher (builds on the Pi) | `HOST=user@<ip> scripts/pi/install.sh` |
| OpenTyrian / the emulator player and a core, compiled on the Pi | `HOST=user@<ip> scripts/pi/install.sh tyrian` / `CORE=quicknes HOST=user@<ip> scripts/pi/install.sh retro` |
| Cave Story's engine, compiled on the Pi (~30 min) | `HOST=user@<ip> scripts/pi/install.sh cavestory` |
| on the Pi itself: a development build (host tools, Doom from apt) | `scripts/pi/build.sh` |
| on the Pi itself: only the console as a service (no launcher) | `scripts/pi/install-console.sh` (`remove` to uninstall) |

Every host program prints a stats line: fps, bytes/frame, batches, sprites,
input-to-present latency, PSP CPU and GE time, uploads, resident surfaces,
errors, audio frames sent / buffered / underruns.

Pointer speed: `PSPEXP_POINTER_SPEED=150` (pixels per second at full nub
deflection; default 200, the native port used 260). L holds slow, R fast.

## Any SDL2 game (v0.4, `compatibility/sdl-pspexp`)

```sh
scripts/dev/pspexp-run /path/to/game            # PSP over USB; the game's window stays on the host
PSPEXP_KEYS=cross=space,circle=lctrl scripts/dev/pspexp-run /path/to/game   # remap PSP buttons to keys
PSPEXP_NUB=mouse scripts/dev/pspexp-run /path/to/game                        # nub = mouse, cross/circle = clicks
PSPEXP_LOCAL_AUDIO=0 scripts/dev/pspexp-run /path/to/game                    # sound only on the PSP
```

Doom: `brew install chocolate-doom`, unzip a Freedoom release
(https://freedoom.github.io/) into `../doom/`, then `scripts/dev/pspexp-doom`.
Nub / d-pad move and turn, cross fires, circle uses, square runs, L / R
strafe, triangle Enter, start Escape (menu), select map. Sound comes out
of the PSP only (`PSPEXP_LOCAL_AUDIO=1` for both); `PSPEXP_WINDOW=hide`
hides the Mac window. Expected stats line: 35 fps, 4 batches, GE ≈ 2.2 ms.

Tyrian: build OpenTyrian in `../tyrian/opentyrian` and unzip tyrian21.zip
into `../tyrian/data/` (see `compatibility/sdl-pspexp/README.md`), then
`scripts/dev/pspexp-tyrian`. Cross fires, circle / L and square / R sidekicks,
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
