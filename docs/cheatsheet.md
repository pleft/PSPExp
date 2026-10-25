# PSPExp cheatsheet

Paths are relative to the PSPExp tree.

## Build and install

| what | command |
|---|---|
| build everything (host + PSP) and run the tests | `scripts/build.sh` |
| PSP install, once (PSP in USB mode) | `scripts/packages/runtime-slot --install <stick>/PSP/GAME/PSPExp` (EBOOT.PBP, slot-a/, active) |
| PSP install without slots (older way) | `dist/PSPExp/` → `ms0:/PSP/GAME/PSPExp/` (EBOOT.PBP, pspexp_usb.prx, pspexp_runtime.prx), into an empty folder: with any slot file there the loader ignores these |
| PSP recovery, a broken install (PSP in USB mode) | `scripts/packages/runtime-slot --install <stick>/PSP/GAME/PSPExp` (removes the old slots and pending update first); without the repository: delete the folder, then copy `dist/PSPExp/` |
| a runtime set with its VERSION / the `pspexp-runtime` package | `scripts/packages/runtime-slot OUTDIR` / `scripts/packages/debs/pspexp-runtime/build OUTDIR` |
| Warcraft I and II: build Stratagus, War1gus and Wargus (Docker, about 15 min an architecture) / package them | `scripts/packages/build-stratagus.sh arm64` (or `armhf`) / `scripts/packages/debs/pspexp-stratagus/build build/debs arm64`, the same for `pspexp-war1gus` and `pspexp-wargus` |
| Warcraft II's PSP layout: change it, then the patch | a Wargus checkout at the commit `build-stratagus.sh` names, `compatibility/wargus/wargus-small-screen.patch` applied, edited; then `git diff > compatibility/wargus/wargus-small-screen.patch` (the map lists keep their Windows line endings) |

Only `pspexp_runtime.prx` changes often; the EBOOT and the USB driver rarely.
With slots, the runtime reaches the PSP over the link (below); USB mode is
only for the first install and for a new boot loader.
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
   host asked for them). An empty file `trace-messages` there (on the board:
   `: > /tmp/e; pspexp-term --ctl "fs put /tmp/e ms0:/PSP/GAME/PSPExp/trace-messages"`,
   `fs rm` to take it away; read at PSPExp's start) logs the first 64
   messages of each OpenGL program's connection, each before it is handled:
   for a PSP that stops, the log's last `msg:` line names the message.

### The runtime's two slots (docs/Package_Manager.md §6.1)

```text
ms0:/PSP/GAME/PSPExp/
  EBOOT.PBP        the boot loader: picks a slot, loads its USB driver and runtime
  slot-a/, slot-b/ each a whole runtime: pspexp_runtime.prx, pspexp_usb.prx, VERSION
  active           slot=a|b, tries=N (starts not yet confirmed by a connection)
  active.new       a finished update: the loader makes it `active` at the next start
  fallback         why the loader last booted the other slot (from=, to=, reason=);
                   the console reports it, logs it and removes it
```

- A slot boots only when every file matches its size and SHA-256 in
  `VERSION`, and `VERSION` ends with `end`.
- The runtime sets `tries` back to 0 once connected. After 3 starts without
  a connection, or with a slot that doesn't check, the loader boots the
  other slot and writes `fallback`; the console reports it.
- **Hold L** while PSPExp starts to boot the other slot by hand.
- The loader's screen (the slot and version it boots) shows for a moment
  only; `pspexp-rt.log` keeps `slot confirmed` and the connection lines.

### Updating the PSP's runtime

The board's `pspexp-runtime` package (a dependency of `pspexp-core`) holds
the current set in `/usr/share/pspexp/runtime`. When it's newer than the
PSP's, the console offers **Settings > Update the PSP**. The update fills the
slot not in use, checks every file, then writes `active.new`; leave PSPExp
with HOME and start it again to run it. An update cut short leaves the old
slot booting. A console speaking a newer protocol connects in **maintenance
mode** (files only) and installs its runtime by itself.

| goal | command |
|---|---|
| the PSP's slots, versions, pending switch, last fallback | `pspexp-ms slots` (on the Mac: `build/host/pspexp-ms --tcp HOST:3838 slots`) |
| install a runtime set into the inactive slot | `pspexp-ms runtime-update DIR` |
| the same through a running console (tests) | `pspexp-term --ctl runtime-update [DIR]`, `pspexp-term --ctl slots` |
| PSPExp on the PSP starts again (a runtime from 2026-10-24 on): the slot just installed boots | `pspexp-ms restart` / `pspexp-term --ctl psp-restart` |
| a PSP game on the stick started natively from the board, PSPExp back at its exit (plugin on; being built) | `pspexp-term --ctl "psp-boot ms0:/ISO/game.iso"` |
| every slot case end to end in PPSSPP (~10 min) | `tests/psp/slots-ppsspp.sh` |

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
| record a native PSP game (PSPExp plugin on): clips with ♪+R / ♪+L, or the whole game | `build/host/pspexp-plugin-host -k -s ~/Movies/clips` (`-b 0`: ♪+L recordings only) / `build/host/pspexp-plugin-host -o clip.mp4` (the console starts the first itself on a board, `-b` per Settings > Recording; `PSPEXP_CAPTURE=0` not) |
| the PSP's memory stick from a shell (list, copy both ways, mkdir, rm, mv, free space) | `build/host/pspexp-ms [--tcp <host>] ls ms0:/PSP/GAME` / `get ms0:/X [file]` / `put file ms0:/PSP/GAME/X/` / `mkdir` / `rm` / `mv` / `df` |
| a stand-in PSP whose memory stick is a folder (file manager, `pspexp-ms`) | `PSPEXP_MS0=<folder> build/host/pspexp-sdl-server` |
| PSPExp's apt repository and catalogue: build (signed) / push to the home server / to the public R2 bucket (keys in `~/.pspexp/r2.env`) | `scripts/packages/apt-repo build build/debs` / `scripts/packages/apt-repo push` / `scripts/packages/apt-repo push r2` |
| **integration tests on a real PSP** (on a board's console; report + the PSP's screenshots in `build/psp-tests/`) | `tests/psp/run.py --host <user@board> [link stick launcher files terminal]` |
| **every game on the real PSP** from the launcher (starts, fps, sound, nub, START + SELECT, the PSP still there; ~35 s a game; results in /tmp/pspexp-all-games) | on the board, PSP connected: `tests/psp/all-games.sh` / `tests/psp/all-games.sh "Nintendo 64::Super Mario 64" /opt/pspexp/apps/quake/quake.entry` |
| the media servers Media finds (UPnP / DLNA), a server's folder | `pspexp-term --upnp`, `pspexp-term --upnp 1 0` (server 1's top; a folder by its id) |
| drive a running console by hand (buttons, state, screenshots, the stick) | on the board: `pspexp-term --ctl "press cross"` / `--ctl state` / `--ctl "shot NAME"` / `--ctl "fs ls ms0:/"` / `--ctl "volume-bar 40"` |
| benchmark the SDL layer's texture uploads (Doom timedemo, four modes) | `scripts/dev/bench-shim.sh` / `scripts/dev/bench-shim.sh --seconds 40 <board host>` |
| Warcraft I and II on a board | Apps > Packages, or on the board `pspexp-pkg install warcraft warcraft2`; your copy's data in `~/Warcraft` (DATA.WAR) / `~/Warcraft2` |

## Any board: packages and ROMs

Every board script takes `HOST=user@host` (the board over SSH). Games and
emulators are the recipes in `recipes/` (docs/Package_Manager.md): on the
Debian boards through apt, on the Omega2+ from our own builds.

| goal | command |
|---|---|
| install / remove / list / what the catalogue offers (on the board) | `pspexp-pkg install nes gameboy` / `remove ID` / `list` / `available` |
| check the recipes | `scripts/packages/check-recipes` |
| copy the test ROMs (`roms/<system>/`, git-ignored) to a board | `scripts/packages/push-roms.sh <host> [nes gb ...]` |
| launcher screenshots from a board (stand-in PSP on the Mac) | board: `PSPEXP_LAUNCHER_OPEN=about pspexp-term --launcher --tcp <mac>:3838 --shot 1` (or `tz`, `wifi`, `password:<name>`, `terms:<package>`) |
| installers ship the existing build: after a source change | the board's `build.sh` first (the Pi's `install.sh` builds on the Pi) |
| on a board: an SDL2 program on the PSP / packages | `pspexp-run <program>` (glibc boards) / `pspexp-pkg list` |
| on a board: a notification on the PSP (a banner, kept under the bell; `-l` info, done, fail, attention, warning, hint; `-a` app:store, files, category:Settings; `-k` replaces the one with that key) | `pspexp-notify [-l LEVEL] [-i ICON] [-a ACTION] [-k KEY] TITLE [DETAIL]`, or `pspexp-term --ctl "notify LEVEL\|ICON\|TITLE\|DETAIL\|ACTION\|KEY"` |
| on a board: sound to Bluetooth (Settings > Sound does the same) | `pspexp-bt devices` / `use ADDRESS` / `psp` / `volume +5` / `route` / `forget ADDRESS` |

## GameShell (`scripts/gameshell/`)

| goal | command |
|---|---|
| build the tools and the SDL layer | `scripts/gameshell/build.sh` |
| OpenTyrian / the emulator player and a core, compiled on the GameShell | `HOST=cpi@<ip> scripts/gameshell/build.sh tyrian` / `CORE=quicknes HOST=cpi@<ip> scripts/gameshell/build.sh retro` |
| Cave Story's engine, cross-compiled here | `scripts/gameshell/build.sh cavestory` |
| any cross-built game for the GameShell or Pi | `scripts/packages/cross-glibc.sh gameshell\|pi scripts/packages/build-openjazz.sh openjazz` |
| install or update a running GameShell | `HOST=cpi@<ip> scripts/gameshell/install.sh` |
| the card image (minimal Debian 13, Linux 6.18, PSPExp from apt, no games): kernel / U-Boot / image | `scripts/gameshell/base/build-kernel.sh` / `scripts/gameshell/base/build-uboot.sh` / `scripts/gameshell/base/build-image.sh [--ssh-key F] [--wifi-from cpi@<ip>]` |
| a release: the PSP's zip and both card images, built by CI, on GitHub's Releases (a run by hand stays a draft) | `git tag vX.Y && git push origin vX.Y`, or `gh workflow run release.yml -f tag=NAME` |
| a board's release files here (image xz, SHA-256, package list, GameShell sources) | `scripts/packages/release-images pi\|gameshell VERSION OUTDIR` |
| the Raspberry Pi card image (Raspberry Pi OS Lite 64-bit, PSPExp from apt, no games) | `scripts/pi/image/build-image.sh [--ssh-key F]` → `build/pi-image/pspexp-pi.img` |
| write an image to a card (check the disk number in Disk Utility first; `build/pi-image/pspexp-pi.img` for the Pi) | `diskutil unmountDisk /dev/diskN && sudo dd if=build/gameshell-base/pspexp-gameshell.img of=/dev/rdiskN bs=4m status=progress && diskutil eject /dev/diskN` |
| on the board: its own access point for the PSP / back to the home Wi-Fi | `sudo pspexp-wifi-mode psp` / `sudo pspexp-wifi-mode home` |
| on the board: the first-boot log | `cat /var/log/pspexp-firstboot.log` |

## Omega2+ (`scripts/omega/`)

| goal | command |
|---|---|
| build the tools / Doom / OpenTyrian / an emulator (static, SDL layer linked in) | `scripts/omega/build.sh` / `... doom` / `... tyrian` / `CORE=quicknes scripts/omega/build.sh retro` |
| Cave Story's / Jazz's engine (static, SDL layer linked in) | `scripts/omega/build.sh cavestory` / `... jazz` |
| ScummVM with the four adventures' engines (static, SDL layer linked in; sources in `../scummvm`, see `scripts/packages/build-scummvm.sh`) | `scripts/omega/build.sh scummvm` |
| install or update (the console, launcher, fonts, package tool) | `HOST=omega scripts/omega/install.sh` |
| its builds of the recipes (`scripts/packages/recipes/ID.sh`) / onto it with the catalogue | `HOST=omega scripts/packages/pspexp-pkg-build build freedoom opentyrian nes gameboy` / `... publish` |
| then install (Apps > Packages, or on the Omega) | `pspexp-pkg install freedoom` |

## Raspberry Pi (`scripts/pi/`)

| goal | command |
|---|---|
| install the console and launcher (from the package repository; without one, builds on the Pi) | `HOST=user@<ip> scripts/pi/install.sh` |
| build on the Pi only, to package `pspexp-core` for arm64 | `HOST=user@<ip> scripts/pi/install.sh build` |
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
arrows). Rotation is not mirrored, SDL render targets only partly; OpenGL
1.x is drawn by the GE, and GLSL shaders are read and drawn as near as it
can. See the README in
`compatibility/sdl-pspexp/`.

After a change to the OpenGL layer, the protocol or the runtime:
`tests/psp/gl-ppsspp.sh` (a minute or two, PPSSPP on the Mac) runs
`gl-demo`'s ten modes from set frames and compares the PSP's frames with
`tests/gl-ref/`; a difference fails with `build/gl-regress/<case>-diff.png`.
`--record` takes new references after a change meant to alter the picture.

## Sprite demo controls (PSP)

d-pad or nub: move · cross: shape · circle: tint · square: field on/off ·
triangle: field size 100/500/2000/5000 · start: reset.

## Warcraft I and II controls (PSP)

nub: pointer · cross: left click · circle: right click · square: shift (add
to the selection) · triangle: ctrl · d-pad: scroll · start: menu (F10) ·
select: escape · L, R: nothing.

## What is worth testing next

1. **Warcraft I and II on the real PSP** (only the stand-in so far): a
   whole skirmish from the launcher, its sound from the PSP speaker, the
   pointer's feel with the nub, on the Pi and on the GameShell.
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
| "No valid PSPExp runtime" at start | neither slot checks: USB mode, `runtime-slot --install` again |
| the console says the PSP fell back | `fallback` on the stick says why; the new slot stays for the next update |
| the PSP keeps running the old version after an update | not restarted yet (HOME, start again), or `active.new` refused: its slot didn't check |
| host killed on cable pull | fixed 2026-09-22; rebuild the host tools |
| PSP status page shows `resource errors` | memory budget: 1.5 MiB VRAM + 12 MiB RAM; the backend evicts after 15 s |
| audio `underruns` climbing on the status page | host frame loop stalled; the stream needs the host to run at ≥ 30 fps |
