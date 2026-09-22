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

Every host program prints a stats line: fps, bytes/frame, batches, sprites,
input-to-present latency, PSP CPU and GE time, uploads, resident surfaces,
errors, audio frames sent / buffered / underruns.

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
