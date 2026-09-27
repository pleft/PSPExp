# scripts/

Everything here runs on the development machine (the Mac) unless a file
says it is installed on a board. One folder per board, the shared parts
together; every script that talks to a board takes `HOST=user@host`.

```text
build.sh           build everything on the Mac: host tools and tests (build/host/),
                   the PSP runtime (dist/PSPExp/, copy it to ms0:/PSP/GAME/PSPExp/)

dev/               run and test from the build tree, a PSP on the Mac's USB or a stand-in
  pspexp-run         any SDL2 program on the PSP (the SDL layer from build/host)
  pspexp-doom        Chocolate Doom + Freedoom on the PSP
  pspexp-tyrian      OpenTyrian on the PSP
  run-ppsspp.sh      the PSP runtime in the PPSSPP emulator
  bench-shim.sh      the SDL layer's upload benchmark (Doom timedemo), here or on a board
  export-stratagus-patch.sh   the Warcraft (Stratagus) backend as a patch

board/             installed on every board (/usr/local/bin; the Omega's SD card)
  pspexp-exec        start a PSPExp program: the console hands the PSP over and back
  pspexp-run         an SDL2 program with the SDL layer preloaded (glibc boards)
  pspexp-pkg         install, remove and list packages on the board

packages/          what the launcher offers, for any board
  pspexp-pkg-build   build packages from recipes and install them (BOARD=, HOST=)
  recipes/           one recipe per package: freedoom, opentyrian, cavestory, nes,
                     gameboy, snes, megadrive; _lib.sh holds the shared helpers
  art/               the games' 128x128 launcher pictures (sources and licenses in its README)
  build-nxengine.sh  Cave Story's engine and its libraries, cross-compiled with Zig
  build-retro-remote.sh   the emulator player and a core, compiled on a glibc board
  push-roms.sh       copy the test ROMs (roms/, git-ignored) to a board

gameshell/         ClockworkPi GameShell (Allwinner R16, Debian 13)
  build.sh           tools and the SDL layer (Zig cross build); cavestory, retro cross-built;
                     tyrian on the board
  install.sh         install or update a running GameShell
  stage.sh           the list of files every GameShell gets (install.sh, the card image)
  base/              the card image: Debian 13, Linux 6.18, device tree, kernel config
  prep-stock-image.sh     (older) PSPExp into a stock ClockworkOS image
  clean-stock.sh, pspexp-firstboot(.service)   first boot of a card: stock removed,
                     bundled packages installed
  pspexp-wifi-mode(.service), hostapd.conf, usb0.interfaces   home Wi-Fi or the
                     PSP's own access point; USB networking

omega/             Onion Omega2+ (MT7688 MIPS, OpenWrt): static builds, data on the SD card
  build.sh           tools; doom, tyrian, cavestory, retro (CORE=): static programs, SDL layer linked in
  install.sh         install or update: console, launcher, fonts, package tool, init script
  pspexp-term.init   the console at boot (/etc/init.d/pspexp-term)
  doom.sh            Doom from the console (older; the Freedoom package replaces it)

pi/                Raspberry Pi (Debian, arm64)
  install.sh         from the Mac: build on the Pi, install the console and launcher;
                     tyrian, cavestory, retro build OpenTyrian, Cave Story and the
                     emulator player on the Pi
  build.sh           on the Pi: a development build (host tools, Doom from apt)
  install-console.sh, pspexp-term.service   on the Pi: only the console, as a service
```

Build output goes to `build/<board>/` (binaries in `bin/`, packages in
`pkg/`); nothing in `build/` is committed. Sources of third-party programs
live next to the repository: `../tyrian/opentyrian`, `../cavestory/nxengine-evo`, `../doom/`,
`../retro/libretro-<core>`, `../pspdev` (the PSP toolchain).
