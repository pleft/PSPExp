# GameShell: games through the shim, and a new launcher

**Status:** 2026-09-26: the shim path and the launcher are done (capture for GL programs is not); the launcher runs on the GameShell, the Omega2+ and the Pi 400 (3.2). Two streams that together make the GameShell a
complete PSPExp host: (a) every game reaches the PSP the best way it can, and
(b) the PSP shows a launcher built for it instead of a mirrored 320x240 menu.

---

## 1. Where the pixels come from today

The GameShell image (ClockworkOS v0.6, kernel 5.3.6) runs the launcher
`launchergo` (Go, SDL2, surface-based, 320x240 hard-coded) inside an X session
started from the cpi login shell. The prepared image forces the X `fbdev`
driver so that X composes into `/dev/fb0`; `pspexp-term --mirror /dev/fb0`
polls it every 16 ms and sends the changed 64x32 tiles to the PSP. Buttons go
back as XTEST key events (the kernel has no uinput).

What that gives per kind of program:

| Program | Draws through | Reaches the PSP as |
|---|---|---|
| launchergo | SDL2 window surface -> X (fbdev) | tiles (mirror) |
| SDL2 game with `SDL_Renderer` (chocolate-doom, many ports) | X (fbdev) | tiles (mirror) |
| RetroArch | its video driver (sdl2 / gl) -> X | tiles, or nothing on the GL path |
| GL program in the Lima mode (`~/.lima`) | glamor -> DRM scanout | nothing (`/dev/fb0` is not scanned out) |
| SDL2 kmsdrm program outside X | DRM scanout | nothing |

## 2. Stream (a): the shim on the GameShell

### 2.1 What can be translated and what cannot

PSPExp is a 2D quad-and-texture protocol; the PSP GE has no shaders. So:

- **SDL2 renderer programs**: every `SDL_Render*` call becomes cached textures
  and quads (`compatibility/sdl-pspexp`). Native quality, 35-60 fps.
- **SDL2 surface programs**: the shim streams the finished window surface
  (one texture per present). Pixels, not commands; the tile mirror is cheaper
  for static screens, the shim better for full-motion.
- **OpenGL / GL ES**: cannot be translated. Only capture is possible.
- **Anything else** (X drawing, raw fbdev): capture only.

### 2.2 The shim as a preload library for glibc armhf

The Omega build links the shim statically into the game because OpenWrt has no
usable LD_PRELOAD. The GameShell is ordinary glibc (Armbian jammy, glibc 2.35)
with a dynamic `libSDL2-2.0.so.0`, so the desktop route works unchanged:
`LD_PRELOAD=libsdl-pspexp.so`. `scripts/build-gameshell.sh` now cross-builds
it with Zig (`arm-linux-gnueabihf.2.35`, PIC, libusb static inside) to
`build/gameshell/lib/libsdl-pspexp.so`, and `gameshell-prep-image.sh` installs
it with `/usr/local/bin/pspexp-run`.

`pspexp-run` on the GameShell = LD_PRELOAD + the PSP handoff: it asks
`pspexp-term` to release the PSP (SIGUSR1, via `pspexp-exec`), the game uses
the USB link, and the console takes it back (SIGUSR2) when the game ends.
The game's own X window stays on the (absent) LCD; `PSPEXP_WINDOW=hide` hides
it. Sound goes to the PSP only (`PSPEXP_LOCAL_AUDIO=0`), so it does not also
enter the console's audio FIFO.

First target: freeDM (`chocolate-doom -iwad freedoom1.wad`, already on the
image), the same game that runs at 35 fps from the Mac, the Pi and the Omega.
Then RetroArch with its `sdl2` video driver, which is `SDL_Renderer` underneath
(one streaming texture per frame plus the menu's draws) - to be measured.

### 2.3 Capture for what the shim cannot translate

The mirror grows a second backend beside `/dev/fb0`:

- **X root window readback** (GetImage on `:0`): pspexp-term already has a
  minimal X11 client for XTEST; one more request returns whatever X composed,
  including glamor/Lima output. About 300 KB per frame over a local socket,
  diffed into the same 64x32 tiles. Chosen automatically when `/dev/fb0` is
  not what X scans out.
- **kmsdrm programs outside X**: the program owns the DRM master and its
  buffers cannot be read by anyone else. Not supported; the answer for such a
  program is the preload shim (which bypasses the display entirely).

## 3. Stream (b): a launcher made for the PSP

### 3.1 Its own library, not launchergo's folders

The first prototype mirrored launchergo's menu tree and skin. That kept the
2018 look (the skin's PNG icons) and the tree's mess: numbered folders, one
folder per game, plugin folders, emulators nested inside categories. The
launcher now keeps **its own library** (`tools/pspexp-term/launcher_lib.c`),
flat files in `~/.config/pspexp-launcher/`:

```
library/<id>.entry     one game, app or tool
    title=OpenTyrian
    type=game                      game | app | tool
    exec=/bin/sh '/home/cpi/apps/Menu/21_Indie Games/34_OpenTyrian/OpenTyrian.sh'
    dir=/home/cpi/apps/Menu/21_Indie Games/34_OpenTyrian
    pspexp=1                       hands the PSP over itself (no mirror)
    icon=rocket  art=/path.png  tags=shooter  hidden=1     optional
systems/<id>.system    one emulated system
    name=Game Boy Advance
    short=GBA
    roms=/home/cpi/games/MGBA
    ext=gb,gbc,gba,gbx
    exec=retroarch -L /home/cpi/apps/emulators/mgba_libretro.so %ROM%
    icon=gamepad-directional
    thumbs=Nintendo - Game Boy Advance     libretro-thumbnails folder (box art)
recent, favourites     entry ids, one per line
imported               import sources already seen
```

The **first start imports** the launchergo menus once: scripts and
one-script folders become entries (typed by where they sat: Indie/Games ->
game, Utils -> tool, RetroArch/TIC-80/PICO-8 -> app), `action.config`
emulators become systems (known cores map to a proper name, badge, icon and
thumbnail folder; the first emulator of a system wins, so MGBA beats GPSP),
launchergo-only pages (Settings, Warehouse, TinyCloud, Music, Reload UI) are
left out. After that the folders are not read again: entries are edited,
added or deleted as files. **System > Rescan library** imports only sources
not seen before (a deleted entry stays deleted) and rescans ROM folders.

Scripts that only start a program known to run through the SDL layer (the
Chocolate Doom family, e.g. launchergo's freeDM) are imported as
`exec=pspexp-run chocolate-doom ...` with `env=PSPEXP_KEYS=...` and
`pspexp=1`: the PSP draws them at 35 fps instead of the mirror at ~20.
Entries may carry any number of `env=NAME=value` lines.

ROMs are listed per system from its folder (one level deep, a `.cue`/`.m3u`
hides its `.bin`/`.img`), titled from the No-Intro name ("Legend of Zelda,
The - The Minish Cap (USA)" -> "The Legend of Zelda - The Minish Cap", tags
"USA").

### 3.2 Where it runs

The launcher is pspexp-term's **home view** (`--launcher [ROOTS]
[--assets DIR]`, on by default in the GameShell service), next to the
terminal and the mirror, reusing the USB/Wi-Fi link, button handling, keypad,
mirror and handoff. Launching: an entry with `pspexp=1` (or a command that
uses `pspexp-run`) keeps the launcher's "running" screen while the PSP is
handed over; anything else runs in the X session and the mirror shows it
until it exits. Hold select cycles launcher / terminal / mirror; System >
Console opens the terminal.

The same launcher runs on every PSPExp board; only the installer and the
package builds differ (`BOARD=` for `scripts/pspexp-pkg-build`):

| Board | Installer (from the Mac) | Packages in | Games |
|---|---|---|---|
| GameShell (Debian 13 armhf) | `scripts/install-gameshell.sh`, or the card image | `/opt/pspexp` | Debian's Chocolate Doom and an OpenTyrian compiled on the board, the SDL layer preloaded (`pspexp-run`) |
| Omega2+ (OpenWrt, MIPS, no FPU) | `scripts/install-omega.sh` | `/mnt/mmcblk0p1/pspexp/opt` (SD card) | static builds with the SDL layer linked in (`build-omega.sh doom`, `build-omega.sh tyrian`), run through `pspexp-exec` |
| Raspberry Pi (Debian arm64) | `scripts/install-pi.sh` (builds on the Pi) | `/opt/pspexp` | as on the GameShell; OpenTyrian compiled on the Pi (`install-pi.sh tyrian`) |

The installers ship what is already built: run `build-gameshell.sh` or
`build-omega.sh` first after a source change (`install-pi.sh` builds itself).
While a game runs the launcher stays resident, asleep, with the PSP handed
over: about 3 MB on the Omega, whose graphics live on the PSP as textures.

### 3.3 The look

```
 12:04 Sat 26 Sep                                        wifi  battery 76%
 [Recent] [Fav] [GAMES] [GBA] [SNES] [MD] [Apps] [System]     rail: rounded tiles,
                                                              the focused one lifted,
 Game Boy Advance                                   5 games   filled with its colour
 +-------------+  Advance Wars                                and glowing
 |  box art or |  | Golden Sun                                list: the focused row
 |  generated  |  | GBA / USA, Europe / 8.0 MB                expands with a detail
 |  cover      |  Metroid Fusion                              line, accent bar
 +-------------+  ...
 (X) Play  ([]) Favourite  (/\) Details           Hold SELECT: console
```

Nothing from the ClockworkOS skin is used. Everything that is not a flat
rectangle is rendered on the host at its final pixel size, because the GE
samples with nearest filtering (`launcher_gfx.c`):

- **Text**: Inter Regular and SemiBold (SIL OFL) at five sizes in one CLUT8
  atlas whose palette is white with 256 alpha levels, tinted per string.
- **Icons**: Lucide (ISC), rendered through stb_truetype per icon and size
  into 8888 textures, tinted. Systems show their short name on the rail tile.
- **Cards and shadows**: anti-aliased rounded rectangles and Gaussian
  shadows as 9-slices (corners 1:1, uniform edges stretched).
- **Backdrop**: 480x272 dark gradient with three soft colour glows, Bayer
  dithered to 5650, six textures.
- **Covers** (128x128 8888, an LRU of 10 on the PSP): box art when there is
  some, else generated: a gradient chosen by the title, a large faint icon,
  a badge (GBA, GAME, APP) and the title set in SemiBold.
- **Box art**: for ROMs of systems with `thumbs=`, fetched in the background
  from thumbnails.libretro.com with curl once the selection rests for 0.5 s,
  cached in `~/.cache/pspexp-launcher/boxart/<system>/` (a `.miss` file
  stops retries). `PSPEXP_LAUNCHER_NO_FETCH=1` turns it off.
- **PSP button badges** in their own colours in the hint bar.
- **Motion**: every change is eased by time (rail scroll and lift, row
  expansion and scroll, list slide on a category change, cover cross-fade,
  toasts). A still screen sends nothing; an animating one about 6 KB per frame.

**Search** (START): an on-screen keyboard (A-Z, 0-9, space, delete, go)
under live results from the whole library, games, apps and every ROM;
matches at the start of the title rank first, then at the start of a word,
then anywhere, favourites a little higher. Cross types, square deletes,
triangle adds a space, START or the go key moves to the results (cross
plays, square favourites, circle returns to the keyboard), circle closes.

**Details** (triangle): a card over the list with the cover, the title, the
system or type, and what is known: tags, size, file, emulator for ROMs;
PSP-drawn or mirrored, folder and entry file for games and apps; when it was
played and whether it is a favourite. Cross plays, square favourites.

**System** runs Rescan library; Wi-Fi (PSP direct / home network, through
`pspexp-wifi-mode`, shown when installed); Box art on/off (kept in
`settings`); Console; Restart and Power off (asking for a second press);
About. Root commands go through `sudo -n` with the rule
`/etc/sudoers.d/pspexp` from the staging list. The fonts and icons are installed to
`/usr/local/share/pspexp/launcher/`.

**About** (System > About): two cards, this board and the PSP. Board: model
(device tree, else /proc/cpuinfo: OpenWrt's "machine"), chip and cores, memory, storage,
system. Storage has one line per storage device: the root file system and
anything mounted under `/mnt`, `/media` or `/opt`, labelled by what the
device is (SD card, Internal flash, USB disk, SSD; looked up by device
number in sysfs, so the GameShell's `/dev/root` reads as its SD card),
leaving out RAM file systems and partitions under 16 MB. PSP: the link (USB,
or Wi-Fi with the PSP's address), latency, runtime and screen, PSP memory,
the board's network addresses, power (battery or mains, temperature,
uptime). It refreshes every 3 s. A line wider than its card scrolls
(marquee: still, to its end, still, back) instead of ending in "...", and
so do long titles in the list.

### 3.4 Status (2026-09-26)

Deployed and tested on the GameShell (the PSP over USB and over Wi-Fi), the
Omega2+ and the Pi 400; `tests/test_launcher_lib.c` covers the library. A
volume page is not needed: the sound plays on the PSP, whose own buttons set
it. Next: per-entry art for games and apps, more packages, emulators.

### 3.5 Screenshots from any board

`build/host/pspexp-sdl-server` is the SDL executor behind TCP port 3838, a
stand-in for a PSP on Wi-Fi (`SDL_VIDEODRIVER=dummy` for no window). A
temporary console on the board draws to it and asks for frames, which land
as `pspexp-shot-N.bmp` in the server's directory:

```sh
# on the Mac
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy build/host/pspexp-sdl-server
# on the board (the running console keeps its own PSP)
PSPEXP_LAUNCHER_OPEN=about PSPEXP_TERM_KEYS=wait \
  pspexp-term --launcher --tcp <mac>:3838 --shot 1
```

`--shot N` saves every Nth frame sent; a still launcher sends few, so use
1. `PSPEXP_TERM_KEYS` replays buttons (`left,down,cross,wait,...`) to reach
other pages; `PSPEXP_LAUNCHER_OPEN=about` opens About without them. Do not
replay keys into System on a slow link: a dropped press can land on Restart
or Power off. A game started with `PSPEXP=tcp:<mac>:3838` draws to the same
server, which is enough to check it runs but not its frame rate.

## 4. A clean GameShell with PSPExp packages (done 2026-09-26)

The goal is a card image: the PSPExp launcher with preinstalled games,
emulators and apps, nothing of ClockworkOS's own launcher. The stock content
(launchergo and its menus, the Python launcher, the games, emulator cores,
NW.js, skins, sample music, RetroArch, LOVE, mpd, aria2, shairport-sync and
their now unused libraries) is removed; what the launcher shows comes only
from **packages** and the user's own entry files. Nothing is mirrored any
more (the mirror stays for the console and debugging).

### 4.1 On the GameShell

```
/opt/pspexp/apps/<id>/          one installed package (owned by cpi)
    package                     version, title, description, license,
                                requires (commands that must exist)
    <name>.entry                the launcher's entries (section 3.1 keys);
                                exec runs in this folder
    <name>.png / cover.png      optional art
    systems/<id>.system         emulated systems (copied to /opt/pspexp/systems)
    ...                         the program and its freely distributable data
/opt/pspexp/systems/            systems from packages; roms=~/roms/<id> is
                                created by the launcher
/opt/pspexp/preinstall/         packages a card installs on its first boot
~/roms/<system>/                the user's ROMs
~/.config/pspexp-launcher/      recent, favourites, settings, library/, systems/
```

The launcher checks every 3 s whether a package, entry, system or ROM
folder changed and reloads ("Library updated"); System > Refresh library
does it at once.

### 4.2 Packages

`scripts/pspexp-pkg-build build ID` runs the recipe `scripts/gameshell/pkg/ID.sh`,
which fills the package folder (downloads cached in build/gameshell/pkg-cache,
helpers in `_lib.sh`) -> `build/gameshell/pkg/ID.tar.gz`. `pspexp-pkg-build install
ID` copies it over and runs `pspexp-pkg install` (unpacks next to the apps
folder, checks `requires`, replaces the old version in one rename, copies its
systems). `pspexp-pkg-build remove`, `pspexp-pkg-build list`, and `pspexp-pkg` on the GameShell
for a tarball from anywhere. Only titles the PSP draws (through pspexp-run)
are packaged; data is only what may be redistributed.

| id | contents | notes |
|---|---|---|
| freedoom | Freedoom 0.13 Phase 1 + 2 (two entries) | the GameShell's chocolate-doom package, Doom key map |
| opentyrian | OpenTyrian (SDL2, compiled on the GameShell) + Tyrian 2.1 freeware data | run.sh writes its config on first start |

Next: Cave Story (NXEngine-evo), SuperTux (SDL renderer), Wolf4SDL and
OpenJazz shareware, then `pspexp-retro` (a small libretro frontend drawing
through PSPExp) with one package per system.

### 4.3 The card image and the first boot

`scripts/gameshell-prep-image.sh` (and `install-gameshell.sh` for a running
GameShell) stage every built package into `/opt/pspexp/preinstall` and
enable `pspexp-firstboot.service`, which runs once before the login shell
and the console: `clean-stock.sh` (idempotent; copies the small stock config
files to ~/pspexp/stock-backup first), then `pspexp-pkg install` for each
preinstalled package, then `/etc/pspexp/firstboot-done`. Verified on the
board (reboot after install-gameshell: stock removed, 26 GB free, both
packages installed, the launcher showing them) and on a copy of the stock
image (the files, owners and service link in place, filesystem check clean).
A public image still needs a release mode of the prep script: no personal
SSH key, no Wi-Fi credentials.

## 5. A fresh base: Debian 13 and mainline Linux (2026-09-26)

ClockworkOS runs ClockworkPi's own Linux 5.3.6 (October 2019, unsupported,
no modules) under an Armbian 22.11 / Ubuntu 22.04 system built for the
Orange Pi PC. Since the PSP is the screen, speaker and controller, none of
ClockworkPi's GameShell drivers (LCD, keypad, amplifier) are needed, so the
card now gets a current base instead, built entirely on the Mac:

| part | what | how |
|---|---|---|
| kernel | Linux 6.18 LTS, `sunxi_defconfig` + `scripts/gameshell/base/kernel.config` | `scripts/gameshell/base/build-kernel.sh` (OrbStack/Docker, ARM64 container, cross-compiled; ~2 min) |
| board | `scripts/gameshell/base/sun8i-r16-clockworkpi-cpi3.dts` | written from the running board's device tree; wired like the Banana Pi M2M (AXP223 on RSB, AP6212 Wi-Fi on mmc1 with reset PL6, SD card-detect PB3, USB OTG ID PL4, serial console on PB0/PB1) |
| system | Debian 13 armhf, minimal, NetworkManager, hostapd, SSH, SDL2, Chocolate Doom | `scripts/gameshell/base/build-image.sh STOCK.img` (mmdebstrap in a container; ~3 min) |
| boot | the board's own U-Boot 2018.01 and Wi-Fi firmware and calibration, taken from the stock image | not redistributed by the repository |

`build-image.sh` applies the same PSPExp file list as every GameShell
install (`scripts/gameshell/stage.sh`) and bundles the built packages; the
first boot grows the root file system to the card and installs them. For
your own card, `--ssh-key FILE` and `--wifi-from cpi@HOST` add your key and
Wi-Fi connections (kept in `build/gameshell-base/local`, never committed).
Login cpi/cpi. The board answers as `gameshell.local` over Wi-Fi and over
USB (fixed 192.168.10.1, a DHCP address from the computer's Internet Sharing,
IPv6 link-local).

Lessons from the bring-up:

- The stock U-Boot refuses kernel images over 8 MB (`CONFIG_SYS_BOOTM_LEN`);
  `multi_v7_defconfig` gave 12 MB and the board sat with its green LED
  blinking. `build-kernel.sh` now refuses such a kernel; a steady green LED
  means Linux is up.
- No serial console: the boot script leaves markers on the boot partition
  (`pspexp-1-script`, `pspexp-2-loaded`, `pspexp-3-bootm-failed`) and Linux
  writes `pspexp-boot-log.txt` there a minute after each boot; put the card
  in a computer to read how far a boot got.
- Programs started by the launcher inherited its TCP connection to the PSP
  runtime; the one-client runtime then refused them (a game stuck on "is
  running", seen with the runtime under PPSSPP). TCP sockets are now
  close-on-exec and the launcher closes everything else before starting a
  program. USB was not affected (libusb marks its handles).
- Testing without the PSP: run the runtime under PPSSPP on the Mac and point
  the console at it with a systemd drop-in (`Environment=PSPEXP=tcp:MAC:3838`
  plus `ExecStart=... --tcp MAC:3838`).

Verified on the board: boot, systemd with no failed units, first boot, the
power chip and battery, USB gadget networking, Wi-Fi, the launcher, Freedoom
through the SDL layer and the START + SELECT exit (against PPSSPP). Not yet:
the PSP over USB host mode on this kernel, the PSP-direct Wi-Fi mode.
