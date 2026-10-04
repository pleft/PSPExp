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
  run-ppsspp.sh      the PSP runtime in the PPSSPP emulator (installed fresh into slot a)
  bench-shim.sh      the SDL layer's upload benchmark (Doom timedemo), here or on a board
  export-stratagus-patch.sh   the Warcraft (Stratagus) backend as a patch

board/             installed on every board (/usr/local/bin; the Omega's SD card)
  pspexp-exec        start a PSPExp program: the console hands the PSP over and back
  pspexp-run         an SDL2 program with the SDL layer preloaded (glibc boards)
  pspexp-pkg         install, remove and list packages on the board
  pspexp-pkg-apt     the root helper pspexp-pkg calls through sudo (recipes through apt)
  pspexp-bt          Bluetooth headphones or speakers for the sound: scan, pair, choose, volume
  80-pspexp-bluez.conf  WirePlumber: Bluetooth audio with nobody logged in (pspexp-core puts it in /etc)
  pspexp-apt-setup   set a Debian board up for the repository: board kind, key, apt source, sudo rule

packages/          what the launcher offers, for any board
  pspexp-pkg-build   the Omega2+'s builds of the recipes as archives (HOST=);
                     `publish`: the builds and the catalogue onto it, for Apps > Packages
  recipes/           the Omega2+'s build scripts, one per recipe it has: freedoom, opentyrian,
                     cavestory, jazz, nes, gameboy, snes, megadrive, and the four ScummVM adventures
                     (beneath-a-steel-sky, flight-of-the-amazon-queen, lure-of-the-temptress,
                     drascula); _lib.sh holds the shared helpers
                     (the recipes themselves are in recipes/ at the top)
  art/               the games' 128x128 launcher pictures (sources and licenses in its README)
  build-nxengine.sh  Cave Story's engine and its libraries, cross-compiled with Zig
  build-openjazz.sh  Jazz Jackrabbit's engine (OpenJazz) and its bundled libraries, the same way
  build-scummvm.sh   ScummVM for the Omega2+ (the four adventures' engines; MP3 and Vorbis through
                     integer decoders, libmad and Tremor), sources in ../scummvm
  cross-glibc.sh     a game (one of the two above) for the GameShell or the Pi: Zig,
                     linked against the board's own SDL2 (seconds, not the Pi's half hour)
  build-retro-remote.sh   the emulator player and a core, compiled on a glibc board
  build-mupen64plus.sh    Mupen64Plus (main branch) and GLideN64 for a Debian 13 board, in a
                     container (the N64; GLideN64 patched to ask for OpenGL ES 3.0)
  build-opensurge.sh, build-hurrican.sh, build-egoboo.sh, build-solarus.sh, build-easyrpg.sh, build-luanti.sh, build-assaultcube.sh, build-vanillaconquer.sh
                     engines Debian lacks, the same way
  push-roms.sh       copy the test ROMs (roms/, git-ignored) to a board
  check-recipes      check the catalogue's recipes (docs/Package_Manager.md §4)
  debs/              our own .deb packages, each with a `build` script: pspexp-core,
                     pspexp-runtime, pspexp-retro, pspexp-cavestory, tyrian-data,
                     jazz-shareware-data, quake-shareware-data, pspexp-libretro (cores Debian lacks for a
                     board: the GameShell's PicoDrive and snes9x2005, and Handy for the Lynx),
                     pspexp-mupen64plus (the N64, with its launch script pspexp-n64), and the
                     engines and installers of the games above
  mkdeb.py           a .deb without dpkg (the Mac has none)
  apt-repo           the signed apt repository and catalogue: build, push to the home server
  runtime-slot       the PSP runtime as a slot set (VERSION with checksums), or a whole
                     slotted install for a memory stick (--install)

gameshell/         ClockworkPi GameShell (Allwinner R16, Debian 13)
  build.sh           tools and the SDL layer (Zig cross build); cavestory, retro cross-built;
                     tyrian on the board
  install.sh         install or update a running GameShell; with a package repository
                     (~/.pspexp/repo-url) it sets the board up for apt and PSPExp comes
                     as pspexp-core, the GameShell's own files still from here
  stage.sh           the list of files every GameShell gets (install.sh, the card image)
  base/              the card image: a minimal Debian 13, Linux 6.18 (build-kernel.sh), U-Boot
                     from mainline (build-uboot.sh), device tree, kernel and U-Boot config;
                     PSPExp from its apt repository, no games
  pspexp-firstboot(.service)   a card's first boot: the root partition grown to the card
  pspexp-wifi-mode(.service), hostapd.conf, usb0.interfaces   home Wi-Fi or the
                     PSP's own access point; USB networking

omega/             Onion Omega2+ (MT7688 MIPS, OpenWrt): static builds, data on the SD card
  build.sh           tools; doom, tyrian, cavestory, jazz, retro (CORE=): static programs, SDL layer linked in
  install.sh         install or update: console, launcher, fonts, package tool, init script
  pspexp-term.init   the console at boot (/etc/init.d/pspexp-term)
  doom.sh            Doom from the console (older; the Freedoom package replaces it)

pi/                Raspberry Pi (Debian, arm64)
  install.sh         from the Mac: the console and launcher, as pspexp-core from the package
                     repository (without one: built on the Pi, into /usr/local); `build`
                     only builds on the Pi, to package pspexp-core;
                     tyrian, cavestory, retro build OpenTyrian, Cave Story and the
                     emulator player on the Pi
  build.sh           on the Pi: a development build (host tools, Doom from apt)
  install-console.sh, pspexp-term.service   on the Pi: only the console, as a service
```

Build output goes to `build/<board>/` (binaries in `bin/`, packages in
`pkg/`); nothing in `build/` is committed. Sources of third-party programs
live next to the repository: `../tyrian/opentyrian`, `../cavestory/nxengine-evo`,
`../jazz/openjazz`, `../simutrans/simutrans` (at its 124.5 tag), `../doom/`,
`../retro/libretro-<core>`, `../pspdev` (the PSP toolchain).
