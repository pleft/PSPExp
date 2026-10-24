# The launcher, packages and boards

How PSPExp becomes a console: games reach the PSP through the SDL layer, the
PSPExp launcher is the menu, packages bring games and emulators, and the
boards (GameShell, Omega2+, Raspberry Pi) run it all. It began on the
GameShell, hence sections 1-5; section 6 is the emulators. Scripts:
`scripts/README.md`.

## Contents

1. Where the pixels come from (the GameShell before PSPExp)
2. The SDL layer on the GameShell
3. The launcher (3.2: on every board)
4. A clean GameShell with PSPExp packages
5. A fresh base for the GameShell: Debian 13 and mainline Linux
6. Emulators: pspexp-retro
7. The terminal and the file manager
8. Integration tests on a real PSP
9. The PSP's clock from the board
10. System Monitor, Packages and Music
11. Sound on Bluetooth

---

## Origin: GameShell, games through the shim, and a new launcher

**Status:** 2026-09-26: the shim path and the launcher are done (capture for GL programs is not); the launcher runs on the GameShell, the Omega2+ and the Pi 400 (3.2); emulators through pspexp-retro (6), tested on the Mac. Two streams that together make the GameShell a
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
- **OpenGL / GL ES**: shaders cannot be translated; OpenGL 1.x can, and is
  since 2026-09-30 (`compatibility/sdl-pspexp/README.md`, "OpenGL 1.x").
- **Anything else** (X drawing, raw fbdev): capture only.

### 2.2 The shim as a preload library for glibc armhf

The Omega build links the shim statically into the game because OpenWrt has no
usable LD_PRELOAD. The GameShell is ordinary glibc (Armbian jammy, glibc 2.35)
with a dynamic `libSDL2-2.0.so.0`, so the desktop route works unchanged:
`LD_PRELOAD=libsdl-pspexp.so`. `scripts/gameshell/build.sh` now cross-builds
it with Zig (`arm-linux-gnueabihf.2.35`, PIC, libusb static inside) to
`build/gameshell/lib/libsdl-pspexp.so`, and `gameshell-prep-image.sh` installs
it with `/usr/local/bin/pspexp-run`.

`pspexp-run` on the GameShell = LD_PRELOAD + the PSP handoff: it asks
`pspexp-term` to release the PSP (SIGUSR1, via `pspexp-exec`; only for a program that takes it over USB: one sent to another PSP, `PSPEXP=tcp:...`, took the PSP from the running console until 2026-10-10), the game uses
the USB link, and the console takes it back (SIGUSR2) when the game ends.
If the program that asked is killed (`kill -9` skips `pspexp-exec`'s EXIT trap,
so no SIGUSR2 comes), the console notices within a second that the process
which sent SIGUSR1 is gone and takes the PSP back itself (2026-10-18: a run
ended that way left the console released for hours, the PSP on its waiting
screen, until a SIGUSR2 by hand).
A game whose entry has `control=` lines (docs/Package_Manager.md §4.2) shows
them first, the controls sheet: its cover and title at the left, a line per
control at the right (the face buttons' own marks, the others' names in
badges), X to play, O back (the user's suggestion, 2026-10-18). The lines are
read from the entry's file when it is chosen, not kept in the library, so the
lists of entries and ROMs stay small (`lib_controls`); a ROM shows its
system's lines, from the `.system` file; the control port's
`state` says `launcher.mode=controls` while it shows. Shown on the PSP-1000 from a Pi 400
(Morrowind's, 2026-10-18): readable at its size. The doubtful lines were checked by pressing
the button in the game (the entry's own command into the stand-in, `pad:` presses in
`PSPEXP_SCRIPT`, frames before and after): Taisei (triangle bombs, circle only backs
out), Simutrans (L out, R in, circle drags the map), Hurrican (up looks up, down ducks,
circle the lightning beam), Neverball and Neverputt (L and R turn Neverball's view,
the d-pad aims in Neverputt, START does not pause). Two recipes were wrong: Pinball's
d-pad sent nothing, so its menu could not be moved in, and SuperTux's L and R sent Page
Up and Down, which it does not use (Home and End look up and down).
Then every game's sheet the same way (three agents, a game each at a time on the
Pi 400, 2026-10-18): 29 recipes corrected (buttons that did something else or
nothing), two more key fixes (GNU Robbo's square shoots with Right Shift, GLtron
pauses on Space), and two faults in the SDL layer found on the way (Mirror Magic's
renderer, X-Moto's keyboard). Not pressed: Warcraft I and II (the player's own
data), Unreal Tournament (its terms), and the SDL 1.2 OpenGL games (Parsec47,
rRootage, Tumiki Fighters) and Lugaru, which the stand-in cannot show; their
lines are from their PSP tests and their key files.
A game started from the launcher is preceded by its "Starting" frame, which
the PSP keeps with a spinner turning on it until the game draws (the
hand-over `DISCONNECT`, docs/PSPExp_Technical_Architecture.md §9.2). SELECT
in the launcher opens the notification centre, as START opens search (the
user's choice, 2026-10-10: notifications belong to the launcher), its hint
beside START's where the hint line has room.
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

The launcher keeps **its own library** (`tools/pspexp-term/launcher_lib.c`;
its header is the reference). It reads two sources and nothing else:

```
<root>/apps/<package>/*.entry     games, apps and tools of installed packages
<root>/systems/*.system           emulated systems those packages install
~/.config/pspexp-launcher/library/*.entry     personal entries, made by hand
~/.config/pspexp-launcher/systems/*.system    personal systems
~/.config/pspexp-launcher/recent, favourites  entry ids, one per line
~/.config/pspexp-launcher/settings            the launcher's options
```

`<root>` is `/opt/pspexp` on the GameShell and the Pi and
`/mnt/mmcblk0p1/pspexp/opt` on the Omega2+ (`PSPEXP_ROOT`); packages get there
through `pspexp-pkg` (section 4.2). Both file kinds are `KEY=value` lines,
`#` starts a comment. An entry, as the OpenTyrian package installs it:

```
title=OpenTyrian
type=game                          game | app | tool
description=Tyrian 2.1: a vertical shooter, freeware since 2004
icon=rocket                        a Lucide icon
exec=./run.sh                      a shell command, run in the package directory
env=PSPEXP_KEYS=cross=space,...    NAME=value, as many lines as needed
env=PSPEXP_NUB=keys
graphics=sdl                       how it draws: sdl | opengl | opengl-shaders | board-gpu
pspexp=1                           drawn by the PSP itself: no mirror
```

Other entry keys: `dir` (where `exec` runs; default the package directory),
`art` (a PNG, relative to the package; `<entry>.png` or `cover.png` are
found by themselves), `tags`, `hidden=1`. `graphics` (systems have it too)
comes from the recipe, which must give it (`check-recipes`); the details
card and the Packages app say "SDL", "OpenGL" or "OpenGL with shaders". The
`description` (up to 220 characters in a recipe) fills up to five lines of
the details card. A system, as the NES package
installs it (section 6.2):

```
name=Nintendo Entertainment System
short=NES                          the rail tile's badge
roms=~/roms/nes                    created when missing
ext=nes
exec=pspexp-run ./pspexp-retro ./quicknes_libretro.so %ROM%
pkg=nes                            exec runs in that package's directory
env=PSPEXP_KEYS=...
thumbs=Nintendo - Nintendo Entertainment System   libretro-thumbnails folder
pspexp=1
```

Ids are `p:<package>/<stem>` for package entries, `e:<stem>` for personal
ones and `r:<system>:<rom path>` for ROMs; recent and favourites store them.
A personal system file with the same name as a package's is used instead of
it (to point it at another ROM folder, say); personal entries are listed
next to the packages' ones. The
launcher notices installs, removals and new ROMs by itself (directory times,
checked every 3 s while connected); **System > Refresh library** reloads at
once.

(The first prototype imported launchergo's menu tree once into such files.
That import is gone: since the stock launcher, games and emulators are
removed from the GameShell (section 4), packages are the only source besides
personal entries.)

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

The same launcher runs on every PSPExp board; only the installer and where
the games come from differ (docs/Package_Manager.md):

| Board | Installer (from the Mac) | Packages in | Games |
|---|---|---|---|
| GameShell (Debian 13 armhf) | `scripts/gameshell/install.sh`, or the card image | `/opt/pspexp` | recipes through apt: Debian's programs and cores, PSPExp's own packages where Debian lacks them, the SDL layer preloaded (`pspexp-run`) |
| Omega2+ (OpenWrt, MIPS, no FPU) | `scripts/omega/install.sh` | `/mnt/mmcblk0p1/pspexp/opt` (SD card) | the same recipes, from PSPExp's static builds with the SDL layer linked in (`scripts/omega/build.sh doom`, ...), run through `pspexp-exec` |
| Raspberry Pi (Debian arm64) | `scripts/pi/install.sh` (PSPExp from the package repository; without one, built on the Pi) | `/opt/pspexp` | as on the GameShell |

The installers ship what is already built: run `build-gameshell.sh` or
`build-omega.sh` first after a source change (`install-pi.sh` builds itself).
While a game runs the launcher stays resident, asleep, with the PSP handed
over: about 3 MB on the Omega, whose graphics live on the PSP as textures.

### 3.3 The look

```
 12:04 Sat 03 Oct                                        wifi  battery 76%
 [hist] | Games                                          34 games   rail down the left:
 [GAME] |  (disc) (DISC) (disc) (disc) (disc) >                     the current tile glows
 [GB]   |  Quake Cave Story ...                                     shelf: discs for games
 [NES]  | Cave Story                                                and ROMs, icon tiles for
 [apps] | Studio Pixel's 2004 platformer ...                        apps and settings; the
 [gear] | Game  /  PSP renderer                                       selection larger, lifted
 (X) Play  ([]) Favourite  (/\) Details           START Search      and glowing
```

**The redesign** (2026-10-03, from the user's concept art; step 1 of five:
the shelf and the left rail; then a home screen, console drawings for the
systems and a wave background drawn by the PSP). The rail runs down the
left (up and down change category), the category's entries along a shelf
(left and right). Games and ROMs are **discs**: the cover in the round
window of a silver shell with squared shoulders, a slot and two screws,
UMD-like but with none of Sony's marks (`gfx_disc`). Each disc is rendered
on the board at its final size with its shadow, 70 px across, and once
more at 86 px with the selection's blue glow and a light ring round the
window, both 128x128 8888 textures in an LRU of 14; the small disc fades
into the large one, lifted 4 px, as the selection arrives. A disc or cover
newly rendered takes the next texture id of its pool (40 for discs, 32
for covers) that no cached one holds: the pool is larger than the cache
so the id just replaced, still on screen that frame, is not reused at
once, but until 2026-10-10 the next id was taken whatever held it. The
selection's disc, kept in use while the shelf scrolled past some 30
others, then had its id given to another game's: its picture overwritten
with that game's, then released when the other left the cache (the user
saw now and then a selected disc that did not grow, a cover turning into
another game's or the generated one; `tests/test_launcher_gfx.c` shows
two cached pictures sharing an id from the 31st game before the fix). Apps, tools and
settings are icon tiles (54 px, 62 selected). Under the shelf the
selection's title, its description (two lines) and its detail line; a
setting's value at the right of its title. The cover column and the list
are gone. The detail line says who draws the entry: "PSP
renderer" (the PSP's GE: the SDL layer, its OpenGL) or "Board GPU renderer"
(`graphics=board-gpu`); it replaced "PSP native", which only meant the
program spoke PSPExp, as every package does now.

**Polish** (2026-10-03): Settings in groups, like Emulators: tiles for
Network, Sound, Clock, Library and This board (`g:<name>` entries, each
opening a section off the rail headed "Settings > Clock"; a group of one
opens its setting at once; a group's line lists what is in it on this
board), Update the PSP and Restart PSPExp in front of them when shown.
The palette is the new one everywhere (search, details, About, Files,
Packages, the Monitor, the keyboards): blue cards and tiles (0x16223c,
0x14203a), a blue accent (0x5aa9ff) for the old purple. The details card
shows a game or ROM in its disc.

**The buttons, made the same on every screen** (2026-10-03, after an
audit of each view's handling): cross chooses, circle goes back (closes,
cancels, the folder above; at a drive's top it closes Files, as it closes
Packages and the Monitor), triangle shows more (details, the actions menu,
a password), square marks or removes (favourite, mark, delete a letter,
remove a package, forget a device, stop the music), START searches,
refreshes or rescans. Moved: Packages' remove from triangle to square, the
music player's stop from triangle to square, the Wi-Fi and Bluetooth
lists' scan again from triangle to START, search's space off triangle.
The terminal follows too, at the user's choice: circle Esc (was Space),
triangle Space (was Enter), START Enter (was Tab), L + triangle Tab (was
Esc). SELECT: a tap closes Files, Packages and the Monitor (as circle
does) and shows or hides the terminal's keyboard; held 0.7 s it switches to
the terminal; START + SELECT held quits a game. The views now get SELECT
as a tap, on its release: Files, Packages and the Monitor had closed on the
press, so holding SELECT in them for the terminal closed them too.

**Leaving an app** (2026-10-06, the user's choice): three ways out were
one too many. Circle steps back one level everywhere and, on an app's
first screen, returns where the app was opened from (the launcher; Files
for Music opened on a song; the terminal for Files opened there). Held
SELECT is home: from any app or Files straight to the launcher (from the
launcher it is the terminal, as before). A SELECT tap closes nothing any
more; in the terminal it still shows or hides the keyboard.

**Step 2** (2026-10-03): a home screen, and the rail cut to five sections,
the same on both: Favourites, Games, Emulators, Apps, Settings (System
renamed; Recent gone, the home screen's **Play again** row starts the
last one played afresh). The launcher opens on the home screen: the five as 60 px
tiles (72 focused, lifted and glowing), what the focused one holds
underneath, Play again below (down, then cross; first called
Continue, renamed as it does not resume). Each emulated system is no
longer a rail tile: **Emulators** is a shelf of systems (entries `s:<id>`;
tiles like the home screen's with the console's icon in the system's
colour, see System icons below: discs are for games and ROMs only, the
user's call),
and cross on one opens its ROMs, a section off the rail (`hidden`), headed
"Emulators > Nintendo 64", the rail showing Emulators. Circle goes back:
from a system's ROMs to Emulators on that system, from a section to the
home screen. A generated cover inside a disc is laid out for the round
window (`CoverSpec.round`: icon and title centred, a system's short name
large). The control port's `category <name>` still reaches every section,
systems included; `launcher.mode` reports `home`.

Nothing from the ClockworkOS skin is used. Everything that is not a flat
rectangle is rendered on the host at its final pixel size, because the GE
samples with nearest filtering (`launcher_gfx.c`):

- **Text**: Inter Regular and SemiBold (SIL OFL) at five sizes in one CLUT8
  atlas whose palette is white with 256 alpha levels, tinted per string.
- **Icons**: Lucide (ISC), rendered through stb_truetype per icon and size
  into 8888 textures, tinted. Systems show their short name on the rail tile.
- **System icons**: the consoles (Game Boy, N64 pad, Mega Drive pad...) from
  RetroArch's XMB "monochrome" theme (libretro/retroarch-assets, CC BY 4.0,
  credited in `assets/systems/LICENSE.txt`), white PNGs named after the
  libretro system name (a recipe's `thumbs=`), box-filtered to 38 px and
  tinted like the font icons (`gfx_picon`); a system without one shows its
  short name.
- **Cards and shadows**: anti-aliased rounded rectangles and Gaussian
  shadows as 9-slices (corners 1:1, uniform edges stretched).
- **Backdrop**: 480x272 blue gradient with two soft glows and three bands
  of light as waves across its lower half (step 5 of the redesign), Bayer
  dithered to 5650, six textures. Over it two light ribbons (512x64 8888,
  periodic over their width, `gfx_waves`). They drifted at first, but
  moving them meant the board redrawing and sending every frame (about
  6 KB, ten a second) while the launcher was in use, where a still screen
  sends nothing; the user chose the look without that cost, so they stay
  still. (Drawing them on the PSP itself, a backdrop message and the
  launcher's commands replayed at its own 60 Hz, was considered and not
  built.)
- **Covers** (128x128 8888, an LRU of 10 on the PSP; on the shelf inside
  discs, `gfx_disc`): box art when there is
  some, else generated: a gradient chosen by the title, a large faint icon,
  a badge (GBA, GAME, APP) and the title set in SemiBold. Art that appears
  later replaces a generated cover: a box art download, or an entry's
  `art=` picture missing when the cover was drawn (Warcraft's title screen
  from the player's own copy, extracted at the first start); the cover and
  the disc remember that their picture was missing and look for it again.
- **Box art**: for ROMs of systems with `thumbs=`, fetched in the background
  from thumbnails.libretro.com (curl, or wget on OpenWrt) once the selection
  rests for 0.5 s, cached in `~/.cache/pspexp-launcher/boxart/<system>/`.
  The system's list of covers (its directory listing, 0.4-4 MB) is fetched
  once and matched locally, so ROMs named any way find theirs: titles are
  compared without tags, articles and punctuation ("Legend of Zelda, The -
  Link's Awakening (USA, Europe)" = "The Legend of Zelda - Link's
  Awakening"), the ROM's region wins among equal titles ((E), (U), (J), (W)
  or No-Intro words), betas and protos lose; a title whose subtitle the
  collection lacks finds the plain one ("Rayman 2 - The Great Escape" ->
  "Rayman 2 (USA)"), never another game of a series. A `.nomatch` file stops
  retries. With GoodTools-named test ROMs: 28 of 28 found.
  `PSPEXP_LAUNCHER_NO_FETCH=1` turns it off.
- **PSP button badges** in their own colours in the hint bar.
- **Motion**: every change is eased by time (rail scroll and lift, row
  expansion and scroll, list slide on a category change, cover cross-fade,
  toasts). A still screen sends nothing; an animating one about 6 KB per frame.

**Search** (START): an on-screen keyboard (A-Z, 0-9, space, delete, go)
under live results from the whole library, games, apps and every ROM;
matches at the start of the title rank first, then at the start of a word,
then anywhere, favourites a little higher. Cross types, square deletes,
a space from the keyboard's own key (triangle typed one until the buttons were made the same on every screen), START or the go key moves to the results (cross
plays, square favourites, circle returns to the keyboard), circle closes.

**Details** (triangle): a card over the list with the cover, the title, the
system or type, and what is known: tags, size, file, emulator for ROMs;
PSP-drawn or mirrored, folder and entry file for games and apps; when it was
played and whether it is a favourite. Cross plays, square favourites.

**System** runs Refresh library; Wi-Fi (PSP direct / home network, through
`pspexp-wifi-mode`, shown when installed); Wi-Fi network; Sound output (§11); Time zone; Box art
on/off (kept in `settings`); Console; Restart and Power off (asking for a
second press); About. Root commands go through `sudo -n` with the rule
`/etc/sudoers.d/pspexp` (GameShell: the staging list; Pi: `install.sh`),
which allows `timedatectl set-timezone`, `nmcli` and `rfkill unblock bluetooth`. The fonts and icons are
installed to `/usr/local/share/pspexp/launcher/`.

The board's own settings (`launcher_board.c`), with each board's tools:

| | Time zone | Wi-Fi network |
|---|---|---|
| GameShell, Pi | `timedatectl set-timezone` | NetworkManager: `nmcli dev wifi list --rescan yes`, `nmcli dev wifi connect` |
| Omega2+ | `uci` `system.@system[0].zonename` and `.timezone` (POSIX rule), `/etc/init.d/system reload` | `ubus call onion wifi-scan`, `wifisetup add`/`edit`, then `priority -move top` |

A scan on the Omega briefly stalls its own Wi-Fi link (one radio, access
point and station): a PSP on USB does not notice, a stand-in PSP over Wi-Fi
can lose the link.

**Time zone** lists about 30 zones (city, UTC offset), the board's own
ticked; cross sets it and the clock follows at once (the launcher sets its
own `TZ`). **Wi-Fi network** (shown when the board has NetworkManager or
Onion's wifisetup; in PSP direct mode it says to switch to the home network
first) scans in the background and lists the networks by name, strongest
first, with their signal, a lock for the secured ones and "saved" for the
ones the board already knows (NetworkManager's Wi-Fi profiles; the Omega's
`wifi-config` entries); START scans again. Cross on a saved network joins
it with what the board keeps (`nmcli connection up uuid`, or `wifisetup
priority -move top`, which restarts the Omega's Wi-Fi); square asks for a new
password, and a failed new password keeps the saved profile. A new secured
network opens a keyboard (three pages: lower case, upper
case, symbols; square deletes, triangle shows or hides the password, START or
Join joins, circle cancels). The join runs in the background and ends in a
toast. The password is handed to the command in the environment variable
`PSPEXP_WIFI_PSK`; it is never written to a file or a log by the launcher
and its buffer is wiped after the join. On NetworkManager boards (GameShell,
Pi) it reaches `nmcli --ask dev wifi connect` on its standard input, piped
by the shell's own `printf` (a builtin, so no process carries it), and is
never among a program's arguments, which every user of the board could read
in the process list (2026-10-11; nmcli 1.52.1 reads it there through its
secret agent). nmcli's prompt goes nowhere (it writes a dot a character: the
password's length), its errors to the log. Tried on a Raspberry Pi 400 from
the card image: a new network joined from the PSP, its profile saved with
the password, and a recorder of every nmcli process's arguments (ten times
a second) saw none carry it.
On the Omega it is still `wifisetup`'s argument (everything runs as root
there). The board's network tool stores it as it stores any network.

**About** (System > About): two cards, this board and the PSP. Board: model
(device tree, else /proc/cpuinfo: OpenWrt's "machine"), chip and cores, memory, storage,
system. The launcher's own version (the board's side: the console, the
launcher, the SDL layer, the `pspexp-core` package) is the board card's
last row, LAUNCHER: the source's last commit date and short hash ("2026.10.02
(244e4bf)", ", modified" for a build from changed files), made at each build
by `cmake/pspexp_version.cmake` (the Pi builds a copy without git, so
`scripts/pi/install.sh` carries it in `.pspexp-version`). Storage has one line per storage device: the root file system and
anything mounted under `/mnt`, `/media` or `/opt`, labelled by what the
device is (SD card, Internal flash, USB disk, SSD; looked up by device
number in sysfs, so the GameShell's `/dev/root` reads as its SD card),
leaving out RAM file systems and partitions under 16 MB. PSP: the link (USB,
or Wi-Fi with the PSP's address) and its time per frame; the PSPExp on the
PSP (the runtime, the `pspexp-runtime` package: its own version, often not
the launcher's, since each is built when its own code changes), its slot
and version as read from the stick on connecting ("slot b,
2026.10.01 (2ab3d0d)", and "an update waits" until the PSP restarts into a
newly installed one; "installed without slots" for an older install); the
PSP's battery (charge; charging, on the adapter or the PSP's estimate of the
time left; the battery's current, in or drawn, when the PSP reports it; its
temperature), asked every 10 s (`POWER_QUERY`: runtimes from 2026-10-02;
older ones show "needs a newer PSP runtime"). On the PSP-1000 (2026-10-02):
"21%, about 1 h 15 min left, 20 C"; it reports no current, so that part
does not show; PSP memory; the board's network addresses and power
(battery or mains, temperature, uptime). It refreshes every 3 s. The status
bar shows the PSP's charge too, once a reading has come: "PSP", a battery
icon (charging whenever the PSP is on external power) and the charge, left
of the board's own indicators behind a `|`. A new reading that differs
redraws the bar (an idle launcher draws nothing otherwise).

**Sleep.** The PSP counts only button presses as activity, so the runtime
holds it awake (`scePowerTick`) while something happens without them: at
least 5 frames in a 5 s window, or sound streaming; and for the first 5
minutes without a board, so a booting board finds it awake. Otherwise the
PSP's own Power Save settings apply, counted from the last button press or
the last busy moment: Backlight Auto-Off turns the screen off (the PSP-1000,
2026-10-02: off 2 minutes after the launcher went still, on again at a
button press). Auto Sleep does not come while the USB link is up (nothing
held it awake for 15 minutes, on battery and on the charger; the PSP seems
to count an active USB connection as use, as in its own USB mode); putting
it to sleep from the runtime after a set idle time was considered and left
out: the screen going off saves most of the battery. So the launcher
at rest sends nothing: 30 s after the last button press its scrolling text
stands still and its caret stops blinking, and so does the terminal's
cursor; the clock's redraw, one frame a minute, does not count. Until
2026-10-02 the runtime held the PSP awake always. A PSP put to sleep comes
back to the launcher on waking (the PSP-1000, 2026-10-02: the waiting
screen, then the launcher; the link is made again as after a cable pull). The runtime's log on the stick
(`pspexp-rt.log`) records "going to sleep", "woke up", the power switch,
and how a session ended ("leaving: quit from the HOME menu", "HOME held:
leaving", "leaving"); a log that stops with none of them is a crash or the
battery. A line wider than its card scrolls
(marquee: still, to its end, still, back) instead of ending in "...", and
so do long titles in the list.

### 3.4 Status (2026-09-26)

Deployed and tested on the GameShell (the PSP over USB and over Wi-Fi), the
Omega2+ and the Pi 400; `tests/test_launcher_lib.c` covers the library. A
volume page is not needed: the sound plays on the PSP, whose own buttons set
it. Next: per-entry art for games and apps, more packages, emulators.

2026-10-01: the OpenGL games' recipes on the GameShell with the PSP on its
USB: most run at 40–60 fps; Neverball and Chromium B.S.U. at 22 and Emilia
Pinball at 7 are limited by the board's CPU (rates and causes: the
architecture document, section 12a, "On the GameShell"). With the PSP on its
one USB port the GameShell is reached over Wi-Fi.

2026-10-01: the Omega2+ brought up to date (console, tools and its eight
packages rebuilt from the current sources: `scripts/omega/build.sh` and its
game targets, then `pspexp-pkg-build build`, `publish`, `install`). The
real-PSP suite on it (`tests/psp/run.py --host omega --term
/mnt/mmcblk0p1/pspexp/pspexp-term`) passed 35 of 36, the miss a section
switch slower than the suite waited (below; the launcher suite then passed
on its own). The games' first
seconds: Cave Story 50 fps, Freedoom 34-35, Jazz Jackrabbit 9 (9-21),
OpenTyrian 6 (2-35, its menus' plasma), and the first ROM of each system
59-60 (Game Boy, Game Boy Color, NES, Mega Drive), SNES 50; title screens
and openings, not play (in play on 2026-09-30: Mega Drive 43, SNES 33). The
suite now waits for a section switch to show (up to 15 s): on the Omega it
can take longer than the 2.5 s it allowed.

The same night the four ScummVM adventures came to the Omega (they were on
the Debian boards only): ScummVM 2.9.0 cross-compiled here, static with the
SDL layer, only their engines (`scripts/omega/build.sh scummvm`,
`scripts/packages/build-scummvm.sh`). The board has no FPU, so their sound
goes through integer decoders: libmad for Flight of the Amazon Queen (its
`queen.1c` is MP3) and Tremor for Drascula's Ogg music. Each recipe gained
`[omega] build=`; the package holds ScummVM (7.6 MB), the game's data from
Debian's own package and ScummVM's engine data file, only the language
folder played (Drascula's and Lure's Debian folders link the shared files
into five languages). All four start, draw and quit back on the real PSP
(the games suite, 14 of 14); the frames they send in their first seconds:
Beneath a Steel Sky 7 (6-20), Amazon Queen 8, Drascula 9, Lure 27, the
games' own pace (Beneath a Steel Sky's engine runs about 12 a second; its
first try took the board 7-11 ms of CPU a frame). Seen on the PSP: Beneath a
Steel Sky in play, pointer on the nub. Played by hand on 2026-10-01: playable, as far as tried. A package over
60 MB does not fit the Omega's /tmp, so `pspexp-pkg-build install` fails for
it: after `publish`, `pspexp-pkg install ID` on the Omega installs from the
card.

On the Pi and the GameShell (Debian's ScummVM, with OpenGL) the four went
black, or showed a corner blown up, from 2026-09-30 to 2026-10-07, found in
the screenshots of the real-PSP test (§8.4). They had been added that
morning and drew right; the OpenGL support of that afternoon lets a
program make an OpenGL window on the headless board, so ScummVM's check of
what OpenGL can do, a 32x32 window made and destroyed several times,
succeeded, and the layer took that window for the game's: everything
scaled 8.5 times from a corner. The layer now lets go of a window the
program destroys and takes the next (`SDL_DestroyWindow`); checked on the
Pi with the layer alone (no PSP): each game's 320x240 window and its
pages; on the GameShell (2026-10-08, its packages upgraded to that day's)
from the launcher to the stand-in PSP: Beneath a Steel Sky's 320x240
window taken after three probe windows, its intro drawn, START + SELECT
back to the launcher. Clearing the layer's OpenGL context on `SDL_GL_DeleteContext` as
well made Beneath a Steel Sky stop at start (`SDL_BlitSurface failed`):
left as it was. The Omega's ScummVM, built without OpenGL, never had it.

The static builds' rename header now lists only the hooks the static SDL
layer defines (its OpenGL ones are left out): ScummVM asks
`SDL_GL_GetDrawableSize` without using OpenGL and failed to link.

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
other pages; `PSPEXP_LAUNCHER_OPEN` opens a page without them: `about`,
`tz`, `wifi`, `password:<network>` (the keyboard, joining nothing
unless typed into), or `terms:<package>` (Packages with that package's
terms to accept: the box has four lines, for id's long address). Do not
replay keys into System on a slow link: a dropped press can land on Restart
or Power off. A game started with `PSPEXP=tcp:<mac>:3838` draws to the same
server, which is enough to check it runs but not its frame rate.

Games too: the temporary console passes its connection to the games it
starts, so `PSPEXP_SHOT_EVERY=60` in its environment saves a game's frames
as well, and `PSPEXP_SCRIPT` (for example `10000:pad:cross`) gets past a
title screen, since a running game has the stand-in to itself and the
control port's presses do not reach it. The stand-in is the SDL executor:
it draws what the SDL layer sends as sprites and pictures (2D games, and
games drawn by the board's GPU such as Celeste 64 and SuperTuxKart), not the
3D commands the PSP's GE draws for OpenGL games (Quake comes out black).
The README's screenshots (`docs/images/`) were taken this way on the Pi 400
(2026-10-08). When starting and stopping such a console over SSH, match it
with `pgrep -x pspexp-term`: a `pgrep -f` pattern also matches the SSH
command that carries it, and kills that shell.

### 3.6 What a game says when it cannot start (2026-10-08)

A game that ends at once used to leave only "Warcraft stopped (status 1)".
The console now gives every program it starts `PSPEXP_MESSAGE`, the path of
a file (`/tmp/pspexp-message-<uid>`, cleared before each start): a program
or its launch script may write one line there before it ends, and the
launcher shows that line instead, as a notification under the game's
name (§3.7). One line of up to 159 characters: a longer one is cut.
Warcraft's launch script uses it when DATA.WAR is missing ("Warcraft needs
your own copy's DATA.WAR: put it in the Warcraft folder of your home folder
(Apps > Files), then start it again").

### 3.7 Notifications (2026-10-08)

Until now the launcher had one toast: one line for 2.6 s, replaced by the
next, drawn only by the launcher, kept nowhere. An update noticed while the
player was in Files or Music, or in a game, was simply lost. Now
`tools/pspexp-term/notify.c` is one service for every part of the console:

- **A notification** has a level (info, done, fail, attention, warning),
  an icon, a title, a detail line, an action (where cross in the centre
  goes: `app:<id>`, `files`, `category:<name>`) and a key: one with a key
  already in the list replaces it, counted ("x2") and moved to the top,
  so a daily check or a battery does not pile up.
- **Banners**, at the top of whatever is shown (`notify_draw` before each
  view's present: the launcher, every app, Files, the terminal), one at a
  time from a queue of 8, 3-6 s by level, longer for a long text (it
  scrolls); a job's progress as a bar (`notify_job`). The main loop
  redraws the view shown while one is up: the views draw only on change.
- **Hints** are what was there before for the answer to a press ("Press X
  again to power off", "Added to favourites"): a line at the bottom, not
  kept, now also over every view.
- **The centre** (`NOTIFY_APP`, "notifications"): the last 50, newest
  first, kept across restarts in `~/.config/pspexp/notifications` (a
  tab-separated line each); reached from the bell in the top bar, next to
  the date, with the unread count (home screen: up from the tiles). Cross
  goes to the action, square removes, START twice clears, circle closes;
  opening it marks them read and clears the banners.
- **Quiet while a program has the PSP** (`notify_set_quiet`): banners
  are held, and when the launcher is back one says how many came. Showing
  something critical over a game (a battery about to run out) would need
  the SDL layer to draw it into the game's frames: left for later, by the
  user's choice, until this is settled.
- **Sources:** the console's own (package updates found by the daily
  check, PSPExp or the PSP's runtime updated, the PSP's clock set, a job
  such as a Wi-Fi join or a pairing finished or failed, a game that ended
  with an error or its `PSPEXP_MESSAGE` line, §3.6), and watchers in the
  main loop (`watch_tick`, once a second): the PSP's and the board's
  battery at 15 % and 5 % (once each until it charges or passes 25 %), the
  board offline for 15 s and back (not while it is the PSP's own access
  point), the Bluetooth device gone while still chosen, the home
  folder's storage under 300 MB or 3 % (every 5 minutes).
- **From anything on the board:** `pspexp-notify [-l LEVEL] [-i ICON] [-a
  ACTION] [-k KEY] TITLE [DETAIL]`, which sends the control port's
  `notify LEVEL|ICON|TITLE|DETAIL|ACTION|KEY`; unlike the port's other
  commands it does not put the console in test mode (no Restart).
- **Control port:** `state` has `notify.count`, `unread`, `latest`,
  `banner`, `hint`, `waiting`, `quiet`; `launcher.toast` now says what the
  banner or the hint shows.

Checked on the Mac with the stand-in PSP: banners queued and shown in
turn, the bell and its count, up to it and into the centre, cross to
Packages, square, START twice, the history after a restart (a repeated key
counted x2), and a program running: nothing shown, then "3 notifications
while you played".

### 3.8 PSPExp Capture's receiver (2026-10-24)

A native PSP game with the Capture plugin on (psp/capture, the
architecture's §12b) reaches the board under the plugin's own USB product,
0x01CB, which the console's link (0x01CA) leaves alone. The console runs
PSPExp Capture's receiver beside itself: started with the launcher over USB
when `pspexp-capture` and ffmpeg are on the PATH (both are on the card
images), as `pspexp-capture -k -q -s ~/Videos/PSPExp` (`~/Movies/PSPExp` on
a Mac), again 10 s after it ends, and stopped with the console, a clip under
way finished first. `PSPEXP_CAPTURE=0` keeps it from starting. In the game,
♪+L starts and stops a recording and ♪+R saves the last 30 s; a dot in the
PSP screen's corner confirms it (green for a clip, red while recording).
Keeping the last 30 s means recording all the time, so it is a setting,
off at first: Settings > Recording > Clips (`clips` in the
library's settings). Off, the receiver runs with `-b 0` (recordings only:
the PSP sends nothing until ♪+L); on, with `-b 30`; a change starts it
again. The Recording page also has Videos, which opens Files on that
folder. (Its first version was a group of one, and a group of one acts at
once: cross flipped the switch with only a toast. A switch alone now shows
its group's page, its state on the right.)
The console's log keeps the receiver's lines (`-q`: the connections, the
clips and what went wrong).

PSPExp's own games (2026-10-24): the receiver also listens on
a local socket, `/tmp/pspexp-capture-<uid>.sock`. The program holding the
PSP (the console, or a game through the SDL layer) finds it, at the
connection and every 2 s after, asks the PSP's runtime to capture what it
shows and plays, and passes it on; one session goes on across the console
handing the PSP to a game and back (8 s allowed between them).
`PSPEXP_CAPTURE=0` in a program's environment keeps it from asking. The PSP
reads ♪+R and ♪+L itself; while the receiver is there, ♪ alone reaches the
board only when let go (the console's music toggle), and nothing pressed
with ♪ reaches it.

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

> **Superseded (2026-09-29):** games and emulators are now the recipes in
> `recipes/`, installed through apt on the Debian boards and from our own builds on the
> Omega2+ (docs/Package_Manager.md). How the archives were, the Omega2+'s builds still
> made that way: [the superseded sections](archive/Launcher_and_Boards-superseded.md).

### 4.3 The card image and the first boot

> **Superseded (2026-09-29):** the card image is the minimal one of
> `scripts/gameshell/base/build-image.sh` (docs/Package_Manager.md §8). The ClockworkOS
> image and its clean-up as they were: [the superseded sections](archive/Launcher_and_Boards-superseded.md).

## 5. A fresh base: Debian 13 and mainline Linux (2026-09-26)

ClockworkOS runs ClockworkPi's own Linux 5.3.6 (October 2019, unsupported,
no modules) under an Armbian 22.11 / Ubuntu 22.04 system built for the
Orange Pi PC. Since the PSP is the screen, speaker and controller, none of
ClockworkPi's GameShell drivers (LCD, keypad, amplifier) are needed, so the
card now gets a current base instead, built entirely on the Mac:

| part | what | how |
|---|---|---|
| kernel | Linux 6.18 LTS, `sunxi_defconfig` + `scripts/gameshell/base/kernel.config`, and the `*.patch` files beside it (`musb-h-disable-empty-queue.patch`) | `scripts/gameshell/base/build-kernel.sh` (OrbStack/Docker, ARM64 container, cross-compiled; ~2 min) |
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
- A kernel oops in the USB host driver froze a board (2026-10-18): the PSP
  dropped off its USB with transfers still queued and `musb_h_disable`
  dereferenced a missing URB (the endpoint still the hardware's current one,
  its queue empty: its last URB had been dequeued while not ready). The oops
  happened with interrupts off: the network went, the LED stayed green, the
  PSP showed its waiting screen. `musb-h-disable-empty-queue.patch` releases
  such an endpoint as `musb_urb_dequeue` would, and the boot script adds
  `oops=panic`, so any oops reboots the board within 10 s (`panic=10`)
  instead of leaving it frozen. Tested on the GameShell with the PSP-1000 (kernel
  #8, 2026-10-18): 31 times the PSP pulled off the USB or switched off in a game,
  31 reconnects, no oops, the board up throughout; the driver warned once
  ("Could not flush host TX2 fifo", from `musb_cleanup_urb` on a disconnect) and
  carried on, and the power chip's battery readings timed out around some
  unplugs (harmless: a reading skipped).
  A kernel's modules go under `/usr/lib/modules`
  (Debian 13 is usrmerged: `/lib` is a link, and unpacking `lib/` at `/`
  replaces it, which leaves the system unable to start a program).
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

## 6. Emulators: pspexp-retro (2026-09-27)

Emulated systems use the same path as every other game: a plain SDL2
program, put on the PSP by the SDL layer. No RetroArch: the launcher is the
only menu.

### 6.1 The player

`tools/pspexp-retro` (`pspexp-retro CORE ROM`) loads one libretro core and
plays one ROM. It has no menus or settings screens; it never uses PSPExp
itself (on the Mac it plays in a window).

- **Video.** The core's frame at its own size (256x224 for the NES) goes into
  one streaming texture; `SDL_RenderSetLogicalSize` gives the core's aspect,
  so the PSP's GE scales it to the screen and the board scales nothing. A
  frame the core repeats is not updated at all; the SDL layer then sends only
  the texture pages that changed, as CLUT8 when a page has at most 256
  colours. XRGB8888 cores go through an ARGB8888 texture, the layer's fast
  path (nothing converted before comparing).
- **Audio.** Resampled in the player to 44.1 kHz stereo, the PSP's rate, with
  integer linear interpolation (no floating point per sample: FPU-less
  boards), through an SDL audio callback that the SDL layer paces. The ratio
  follows the buffer by up to +-0.5 %, so the core's clock (60.1 fps for the
  NES, 32040 Hz for the SNES) and the PSP's 60 Hz display never drift apart.
- **Input.** The PSP's controller as the SDL layer offers it: cross = B,
  circle = A, square = Y, triangle = X (a Nintendo pad's layout), L / R,
  START / SELECT, the d-pad and the nub. On a keyboard: arrows, x, z, s, a,
  q, w, Return, right Shift; Escape quits. Holding START + SELECT on the PSP
  leaves, as in every game.
- **Battery saves.** The cartridge's SRAM in
  `~/.local/share/pspexp-retro/saves/<rom>.srm`, written every 10 s when it
  changed and at exit. BIOS files for systems that need them go in
  `~/.config/pspexp-retro/system/`.
- **Core options.** The core's defaults, overridden by `KEY=value` lines in
  `~/.config/pspexp-retro/<core>.opt` or `PSPEXP_RETRO_OPT="k=v;k=v"`.
  `PSPEXP_RETRO_ASPECT=pixel` gives square pixels.
- **Cores.** A path, or a name looked up as `NAME_libretro.so` in
  `$PSPEXP_RETRO_CORES`, next to the player, `/opt/pspexp/cores` and the
  distribution's libretro directories. On the Omega2+ (static musl: no
  dlopen) the core is linked in: one program per core,
  `pspexp-retro-<core>`. Software cores only (no OpenGL).

Not in yet: save states and an in-game pause overlay.

### 6.2 Systems on the launcher

A package installs a `.system` file (section 3.1). Two keys exist for this:
`pkg=<package>` runs the command in that package's directory, so it can
start the package's own player, and `env=NAME=value` sets variables for it.
The NES package's file, as built for the GameShell:

```
name=Nintendo Entertainment System
short=NES
roms=~/roms/nes
ext=nes,fds,unf,unif
exec=pspexp-run ./pspexp-retro ./fceumm_libretro.so %ROM%
pkg=nes
env=SDL_VIDEODRIVER=dummy
env=SDL_AUDIODRIVER=dummy
thumbs=Nintendo - Nintendo Entertainment System
pspexp=1
```

The launcher shows a rail tile per system that has ROMs, lists the ROMs
with No-Intro names cleaned up, fetches box art by `thumbs=`, and starts
the command with the ROM's path in place of `%ROM%`. When it starts a
PSPExp program (`pspexp=1`) it now lets go of the PSP itself and takes it
back when the program ends, so a program started without `pspexp-exec`
does not wait for a PSP the launcher holds.

**The PSP's own games (2026-10-25, being built: the architecture's §12c).**
pspexp-core installs `/opt/pspexp/systems/psp.system`: the board's PSP games
in `~/roms/psp` (ISO, CSO, or a zip holding an ISO, as collections keep them)
on a "PSP" shelf under Emulators, with no emulator: `exec=@psp-boot` asks
the console to have the PSP start the game itself (for now a "Not yet"
notice), and the line under the shelf says "Runs on the PSP". Their covers
are their own icons, PSP_GAME/ICON0.PNG read from the disc on the board
(`pspexp_disc.c`) in the background for the game selected, as box art is
fetched, into `~/.cache/pspexp-launcher/boxart/psp/icon0/` (`.noicon`: none,
a PS1 game in the folder); box art (libretro's "Sony - PlayStation
Portable") only for a game without one. Walked in PPSSPP with six games of
the Pi 400's collection: the icons on the discs at 1:1, the PS1 one left
with its generated cover.

### 6.3 Packages and builds

| Board | Player and core | Built by |
|---|---|---|
| GameShell | `pspexp-retro` + `fceumm_libretro.so`, compiled on the board, the SDL layer preloaded (`pspexp-run`) | `scripts/gameshell/build.sh retro` |
| Raspberry Pi | the same, compiled on the Pi | `scripts/pi/install.sh retro` |
| Omega2+ | `pspexp-retro-fceumm`: one static program, core and SDL layer linked in, run through `pspexp-exec` | `scripts/omega/build.sh retro` |

`scripts/packages/build-retro-remote.sh` does the on-board builds for the GameShell
and the Pi. Core sources live next to the repository in
`../retro/libretro-<core>` (`git clone https://github.com/libretro/libretro-fceumm`;
`CORE=` picks another). The package: `scripts/packages/recipes/nes.sh`,
installed with `BOARD=<board> HOST=<host> scripts/packages/pspexp-pkg-build install nes`.
No ROMs are included; the card image can carry free homebrew later (for
example Nova the Squirrel, GPL-3.0).

### 6.4 Status (2026-09-27)

Played on the Omega2+ with the real PSP over USB (the hardest board:
580 MHz MIPS, no FPU):

| System, core | Result | Setting |
|---|---|---|
| NES, QuickNES | with the SDL layer of 2026-09-27 evening (16-bit shadow for part-texture updates, word-wise compare, one USB transfer a frame): full-speed game and sound, 46.7 fps shown on average over the whole level 1 (38-60), fewer audio clicks than at 30 fps; before, only a 30 fps picture held (20-28 while scrolling) | 60 fps, CLUT8 pages |
| Game Boy / Color, Gambatte | 59-60 fps, 56-59 shown, CPU half idle | 60 fps |
| Mega Drive, PicoDrive | Sonic, with the SDL layer of 2026-09-27 evening: 59.8 fps emulation (57 at worst), 34 fps shown on average (24 at worst), a whole level, some audio clicks; before: 33 fps emulation, ~7 shown | 60 fps |
| SNES, Snes9x 2005 | Super Mario World (PAL), with the SDL layer of 2026-09-27 evening: 50.3 fps emulation (full), 38.4 shown on average, 8 audio underruns in a session; before: 46-51, 13-25 shown, ~130 underruns in 35 s | 50 fps |

On the Raspberry Pi 400 (4x Cortex-A72 1.8 GHz, the PSP over USB), every
system at the machine's own rate, every frame shown:

| Game | Emulation / shown | Core per frame | Texture work | Audio underruns |
|---|---|---|---|---|
| Sonic the Hedgehog (Mega Drive, PicoDrive) | 59.9 / 59.9 fps | 2.2-2.5 ms | 1-3 ms | 2 (start-up) |
| Super Mario World (SNES PAL, Snes9x 2005) | 50.3 / 50.3 fps | 3.0 ms | 1.4-2 ms | 4 (start-up) |

The player takes about 8 % of one core. Box art: GoodTools-named ROMs
("(W) (REV01) [!]") do not match the libretro thumbnails' No-Intro names and
get generated covers.

On the GameShell (4x Cortex-A7 at 1 GHz; the players and cores
cross-compiled on the Mac: the card image has no compiler):

| Game | Emulation / shown | Audio underruns |
|---|---|---|
| Super Mario World (SNES PAL) | 50 / 41-50 fps | 11 a minute, isolated clicks |
| Sonic the Hedgehog (Mega Drive), one thread | 60 / ~30 fps while running fast | 9 in 35 s |
| Sonic the Hedgehog, the core on its own thread | 60 / 40-57 fps (typically ~44), a whole stage | 8, all at start-up |
| Sonic the Hedgehog, one USB transfer a frame | 60 / 57.4 fps average (never below 55.7), the whole level 1 | 7, all at start-up |

Three steps got it there, each found by measuring (`PSPEXP_PROFILE=1` in a
system's `env=` splits the SDL layer's time into compare, convert, write,
waiting for the PSP and sending):

1. **The core on its own thread.** On one thread the core (7-10 ms) and the
   SDL layer's texture work (9-10 ms) added up past the 16.7 ms frame and
   the PSP's vsync halved the picture. With more than one core the player
   runs the core one frame ahead on a worker thread while the main thread
   hands the last frame to SDL and the PSP (the Omega, one core, keeps one
   thread).
2. **The 16-bit shadow for part-texture updates.** The fast RGB565 path only
   switched on for whole-texture updates; emulators update part of a texture
   made at the core's largest size, so every frame was converted to 32 bits
   first (compare: 5-8 ms, now 1-3).
3. **One USB transfer a frame.** Each page went out as its own messages, a
   palette and a texture, about 15 transfers a frame; the GameShell's USB
   host pays dearly per transfer (8-11 ms a frame). The library now gathers
   a frame's uploads and audio (`pspexp_batch_begin` / `_flush`) and sends
   them in one transfer before waiting for the PSP: 0.5-1 ms.

A page conversion pool (the other cores convert a frame's pages, written in
order after) is in as well; it matters less than expected (the conversion
was 1-2 ms of it).

What was learned on the Omega:

- **FCEUmm is too heavy for the Omega** (about 25 ms a frame with Super
  Mario Bros.); QuickNES takes about half, so the NES package uses QuickNES.
- **Sound first.** A frame the board cannot draw in time is still run (the
  core is told it will not be shown), so the game's speed and sound hold and
  only the picture gets choppier.
- **A steady 30 fps beats a swinging one.** Letting the picture drop only
  when needed made it swing between 60 and 12 fps while scrolling; drawing
  every second frame on purpose (`PSPEXP_RETRO_VIDEO_DIV=2`, set in the
  Omega's NES, SNES and Mega Drive entries) plays better.
- **Keep CLUT8 on the Omega for the NES.** 16-bit pages doubled the bytes
  (88-133 KB a frame while scrolling) and USB transfers cost the Omega more
  CPU than the indexing: 48-54 fps emulation instead of 60, hundreds of audio
  underruns.
- **RGB565 cores go through a 16-bit shadow** in the SDL layer now (QuickNES,
  most cores): no conversion to 32 bits before comparing.
- **Keys, not a virtual joystick.** The Omega's static SDL has no virtual
  joysticks, and events pushed into SDL never reach its keyboard state
  table: the player follows key events, and every `.system` maps the PSP's
  buttons to its keys (`PSPEXP_KEYS`).

- **Measure with nothing else running.** A watcher that searched the
  game log every 2 s took about 3 s of the Omega's CPU a pass (the log had
  grown to 2.2 MB in RAM) and made several test rounds look far worse than
  they were. The launcher now starts the log afresh past 256 KB; read it
  once, after playing.
- **Compare four bytes at a time.** musl's memcmp (the Omega's static
  builds) goes byte by byte: 2-4 ms a frame for an NES picture, 1.3 now.

Compression was considered for the Omega and left for Wi-Fi: over USB a
scrolling NES frame costs it about 3 ms to send, and compressing it would
cost about as much. Next: the GameShell and the Pi, where SNES and Mega
Drive belong. (The SNES and Mega Drive were taken off the Omega on the morning of 2026-09-27 and are back on it since the evening's SDL layer.)

### 6.5 The Nintendo 64 (2026-10-03, Pi only)

Not through pspexp-retro: Mupen64Plus is its own program, and a system recipe
may now name one (`exec=/usr/games/pspexp-n64 %ROM%` in `[system]`, no
`core=`; `pspexp-pkg` writes it into the `.system` file and leaves the video
driver to `pspexp-run`). `recipes/n64`, package `pspexp-mupen64plus`
(`scripts/packages/build-mupen64plus.sh`): Mupen64Plus from its main branch
with GLideN64 asking for OpenGL ES 3.0, drawn by the Pi's GPU
(`PSPEXP_HOSTGL=1`, `graphics=board-gpu`). ROMs in `~/roms/n64` (.z64, .n64,
.v64), box art from libretro-thumbnails' "Nintendo - Nintendo 64".
`pspexp-n64` sets the controller: SDL joystick 0 with the PSP's raw buttons
(cross 0, circle 1, square 2, triangle 3, L 4, R 5, START 6, SELECT 7, d-pad
12-15): nub the stick, cross A, square B, L Z, R R, START Start, d-pad the C
buttons, triangle L, circle the N64's d-pad down; controllers 2-4 unplugged.
No keyboard keys (`keys=` all none): Escape quits Mupen64Plus. GoldenEye's
Dam mission about 20 fps into PPSSPP, the sound never short; Super Mario 64
right (architecture §12a).

## 7. The terminal and the file manager (2026-09-27)

Both are built-in apps in the launcher's **Apps** (Terminal, Files; the
category is in alphabetical order, built-in and installed apps together,
since 2026-10-10: Files, Music, Packages, System Monitor, Terminal), with covers of their own (the icon large on a fixed
gradient, `CoverSpec.hero`), favourites and recents like any app; the
System section keeps the board's settings only.

### 7.1 The terminal

Buttons were reviewed with the user for comfort on the PSP-1000 (the
Controls screen in the menu lists them all):

| | |
|---|---|
| cross / square / circle / triangle | the highlighted key / Backspace / Esc / Space |
| start | Enter |
| L held / R held | Shift / Ctrl |
| L + triangle / R + square | Tab / Ctrl-C |
| select | the on-screen keyboard (held: the launcher) |
| R + d-pad, R + nub | scroll back (a line or a page; smoothly) |
| nub, keyboard hidden | scroll back smoothly (shown: arrow keys) |
| L + select | scroll mode: scrolling with nothing held; circle leaves |
| L + R + left / right | the other shells, up to four (right past the last opens one) |
| L + square / L + circle | copy mode / paste |
| L tapped | the context menu |

The context menu (`term_menu.c`) holds keys and commands for the shell,
others while a full-screen program runs (vi, less, top), and the
terminal's own actions: Files here, copy, paste, shells, scroll mode,
keyboard, Theme..., Font size..., Controls...; `~/.config/pspexp-term/menu`
replaces any of its sections. Eight themes (`term_themes.c`) recolour
what is on screen, scrollback included (cells keep palette indexes).
Three Spleen fonts (`term_fonts.c`, generated by `scripts/dev/bdf2c.py`):
6x8 (80x34), 6x12 (80x22), 8x16 (60x17), with Latin-1; line drawing and
blocks are drawn for any cell size, and DEC line drawing (ESC ( 0) works.
Theme and font are kept in `~/.config/pspexp-term/settings`. Copy mode
moves a cursor over the screen and the scrollback (cross marks, cross
copies, triangle takes a line); the clipboard is shared by the shells. A
button still held when a mode or menu changes is ignored until released.

### 7.2 The file manager

`files.c`, a third view of the console next to the launcher and the
terminal, inspired by Midnight Commander and drawn like the launcher:
two panels (folder, path, entries with an icon by type and their size,
marks, a footer). Apps > Files opens it at home, the terminal menu's
Files here at the shell's folder; circle at a drive's top goes back there.

| | |
|---|---|
| d-pad | move (left / right: a page); up from the top row: the drive button |
| L / R | the left / right panel |
| cross | open: a folder; a picture full screen (left / right the others); music or a film in Media (§10.4); a ROM in its system's emulator; a `.tar.gz` package installed (after asking); a text file in a viewer; the hint names which |
| circle | the folder above |
| square | mark and move down |
| triangle | copy or move to the other panel, delete, rename, new folder, details |
| start | hidden files, sort (name, size, date), same folder in both, swap, terminal here, close |

Copy, move and delete run on a thread (`files_ops.c`, tested by
`tests/test_files_ops.c`) with progress and circle to cancel; a move
across file systems is a copy and a delete; nothing is replaced or deleted
without asking; a folder is never copied into itself. Renaming and new
folders use the launcher's keyboard, shared since (`launcher_ui.c`).
Pictures (PNG, JPEG, GIF, BMP, TGA) are fitted to the screen and
box-filtered, or enlarged by whole factors up to 8x when small, dithered to
5650 and sent as up to four 256x256 tiles with 64-wide frames (the GE's
strip limit). A ROM goes to the first installed system whose extensions
match (`lib_rom_entry`, `launcher_open_file`), started as from the
launcher's lists; a Starting box shows until the game has the PSP, and the
console comes back to the file manager afterwards. A package runs
`pspexp-pkg install` in the background, its message shown after.

**Archives** (2026-10-12): `bsdtar` (libarchive-tools, a dependency of
pspexp-core) run as a child, the way Music runs ffmpeg, not libarchive
linked in: it reads ZIP, 7z, RAR and tar.gz/bz2/xz/zst and writes ZIP, and
the Mac has it too. Two job kinds join copy, move and delete
(`FJOB_EXTRACT`, `FJOB_COMPRESS`), on the same thread with the same
progress box and cancel: the progress is bsdtar's `-v` lines counted
against a listing first (`-tf`, files only; creating names folders too, so
they are counted there), not bytes. Unpacking goes into a new folder named
after the archive ("game 2" when "game" is there); an archive holding one
folder becomes that folder when its name is free. A failed unpack leaves
nothing behind; a cancelled compress removes the partial .zip. Cross on an
archive opens a menu (unpack into the other panel, unpack here, what is in
it: `bsdtar -tv` as names and sizes in the text viewer), with "Install as
a PSPExp package" for a .tar.gz, which used to go straight to installing.
A zipped ROM still opens in its emulator. Tests: `tests/test_files_ops.c`
(pack, list, unpack, the one-folder case, a bad archive) and the Files
suite of `tests/psp/run.py`.

**The music player** (2026-10-03, `player.c` for the sound, `files.c` for
its screen; since 2026-10-05 an app of its own, Music, §10.4, and this
screen is gone): cross on MP3, WAV, Ogg, FLAC, Opus, M4A/AAC, WMA or a tracker
module (MOD, XM, IT, S3M) plays it, then the folder's other playable files
in order. The screen shows the title, artist and album from the tags
(ID3v1, ID3v2.2-2.4; ffprobe's for the rest), the cover (the picture in the
tags, else `cover.jpg` / `folder.jpg` beside the file), the format, where
it is decoded, and the time.

| | |
|---|---|
| cross | pause / play (at the end: again) |
| left / right | 10 s back / on |
| L / R | the previous / next track (round the folder) |
| square | stop and close |
| circle | back to the files; the music goes on (cross on the file: the player again) |

MP3 goes to the PSP as it is and the PSP decodes it on its Media Engine
(architecture §8, compressed audio): 16 KB/s at 128 kbps, next to no board
CPU. 16-bit 44.1 kHz WAV goes as PCM. Everything else, and MP3 for a PSP
whose PSPExp is older, is decoded on the board by ffmpeg when it is
installed (the Pi has it; the Omega does not) and sent as PCM. Files on the
PSP's memory stick are copied to the board first (up to 16 MB, as for
pictures). A program taking the PSP stops the music (closing the file
manager did too, until Music). Tested in PPSSPP from the Mac (all the formats above but WMA and
the modules; an MP3 with a cover and a Greek title), then on the PSP-1000
from the Pi 400 (2026-10-03): a real song as MP3 (decoded on the PSP) and
as FLAC (ffmpeg on the Pi), played by hand.

The launcher's text became UTF-8 the same day: the title, body and small
fonts carry Latin-1, Latin Extended-A, Greek and Cyrillic (the glyph atlas
grew to 512x512), so accented, Greek and Cyrillic file names and tags
show; before, every byte past ASCII was a '?'.

### 7.3 The PSP's memory stick as a drive (2026-09-28)

Each panel's header starts with a drive button (the drive's icon and name,
a chevron), then the folder's name. Up from the top row highlights it;
cross drops down the list of every drive: **Board** (`/`), each storage
mount of the board, then **PSP** (dimmed while the PSP does not share its
files), each with where it is mounted and its free space, the shown one
checked. Up / down and cross switch; circle closes; down from the button
returns to the files. Each panel remembers its folder on every drive.

The board's drives come from `/proc/self/mounts` (`getmntinfo` and
`/Volumes` on a Mac), read again each time the button is reached, so a
stick plugged in meanwhile shows. Only real devices (`/dev/...`, not loops
or zram) count, and not `/`, `/boot`, `/efi` or system folders (`/proc`,
`/sys`, `/dev`, `/run` but `/run/media`, `/var`, `/usr`, ...). Names:
the volume label under `/media` or `/run/media`, "SD card" for an
`mmcblk` device (the Omega's `/mnt/mmcblk0p1`), "USB" for `sd*` (a second
one by its folder). On the stick, the footer shows the free space. Copy, move, delete, rename
and new folder work on it and between it and the board; pictures and text
open from it directly (fetched to `/tmp/pspexp-files-view`, up to 16 MB);
ROMs and packages are copied to the board first.

It is a protocol feature, not a USB mass-storage mode: the runtime keeps
running and the link stays up.

- **Protocol:** channel `FILE` (6) with FS_LIST, READ, WRITE, MKDIR,
  REMOVE, RENAME, INFO and STAT, each answered by one FS_REPLY that
  carries the request's id. Reads and writes are at most 64 KB each, and
  a listing returns up to 64 entries (`PspExpFsRequest` / `Reply` /
  `Entry` in `pspexp_protocol.h`).
- **Runtime support:** a runtime offers the channel with the HELLO
  feature bit `PSPEXP_FEATURE_FILES`. An older runtime would drop the link
  on an unknown channel, so the host asks only when the bit is there.
  Otherwise, the file manager asks for the new EBOOT.
- **Accepted paths:** the runtime accepts `ms0:/` and `ef0:/` paths
  without `..` parts.
- **Where requests are answered:**
  - on the PSP: `psp/runtime/pspexp_fs_psp.c` (sceIo);
  - on the SDL stand-in and null executor: `runtime/pspexp_fs_posix.c`,
    where a folder named by `PSPEXP_MS0` plays the stick.
- **Host library:** blocking calls `pspexp_fs_list/stat/read/write/mkdir/remove/rename/info`,
  which pump the link while they wait (up to 15 s).
- **File manager:** `files_ops.c` sends paths that start with `ms0:` or
  `ef0:` to an `FRemote` table. `files_psp.c` implements that table over
  the link. A copy runs on its job thread, but the link belongs to the main
  loop, so each request waits until `files_psp_service()` runs it (up to
  12 ms per turn).
- **Command line:** `build/host/pspexp-ms [--tcp HOST] ls|get|put|mkdir|rm|mv|df`
  does the same from a shell. For example, `pspexp-ms put EBOOT.PBP ms0:/PSP/GAME/X/`.
- **Tests:**
  - `tests/test_fs.c` covers the channel end to end;
  - `tests/test_files_ops.c` covers copies both ways, moves, deletes, and a
    lost link, through the broker from a real thread.
- **Measured in PPSSPP:** a 5 MB round trip matches byte for byte, at about
  3 MB/s (emulated; the real PSP over USB not yet measured).

## 8. Integration tests on a real PSP (2026-09-28)

`tests/psp/run.py` drives a real PSP, with no one pressing anything, through
the console's **test-control port**.

### 8.1 The control port

This is a Unix socket for the console's own user only:
`/tmp/pspexp-term-<uid>.ctl`, or `$PSPEXP_TERM_CONTROL`, which must be a
short path. `--no-control` turns it off. `pspexp-term --ctl "COMMAND"` is
its client: it prints the reply and exits 0 (ok), 1 (error) or 2 (no
console). The commands:

| command | what it does |
|---|---|
| `press BUTTONS [ms]`, `hold ...` | buttons as if pressed on the PSP (`l+r+right`) |
| `nub X Y [ms]` | the nub (0-255) |
| `wait MS`, `idle [MS]` | a pause; until nothing is drawn for 0.4 s |
| `state` | the view; the launcher's section, entries and highlight; both file-manager panels (path, drive, entries, selection), the open menu or message; the terminal's text |
| `stats` | frames, latency, GE time, errors |
| `shot NAME` | the PSP saves its screen next to its EBOOT; the console fetches it over the FILE channel into `/tmp/pspexp-shots/NAME.bmp` (or `$PSPEXP_TERM_SHOTS`) and deletes it from the stick |
| `open launcher|terminal|files [PATH]` | switches the view |
| `select TITLE` | the launcher's highlight onto an entry of the section shown |
| `stop` | ends the program the launcher started (its process group), and answers when the launcher has the PSP back |
| `relink` | drops the link as a cable pull would, and answers when it is back |
| `clock [force]` | the PSP's clock and time zone from the board (§9) |
| `psp-restart` | PSPExp on the PSP starts itself again (runtimes with `PSPEXP_FEATURE_RESTART`); the link goes and comes back |
| `type TEXT` | writes into the shell (`\n` is Enter) |
| `fs ls|get|put|rm|rmtree|mkdir|mv|df` | the memory stick over the console's link, with timings |
| `end` | lifts test mode |

From its first command on, the console is in test mode: Restart, Power off
and the Wi-Fi mode switch are refused, so a test can never take the board
away. The PSP's own buttons keep working.

### 8.2 The runner

`tests/psp/run.py --host user@board [suites]` runs on the Mac. It talks to
the board over one shared ssh connection, and `--local` uses a console on
the Mac.

**Suites:**
- **link:** connected, no errors, a screenshot round trip.
- **stick:** free space; 4 MB up and back, identical, timed; listing,
  size and date; rename; errors; clean-up.
- **launcher:** the sections; Apps starts with Terminal and Files; a
  screenshot of every section.
- **files:** open at a board folder; the other panel onto the PSP through
  the drive list; copy two files with triangle > Copy (checked byte for
  byte); open a picture and a text file from the stick; delete with its
  question; back to the board.
- **terminal:** a command and its output.
- **relink:** the link dropped and back three times; then the clock is
  checked again, there are no errors, and the screen draws.
- **games:** what the board has, never a fixed list. Every installed game,
  and the first ROM of every emulated system that has ROMs (the launcher's
  `launcher.systems`), is started from the launcher. Each must draw frames
  with no link errors, quit (`stop`) and hand the PSP back to the launcher.
  Its frame rates come from the SDL layer's lines in
  `/tmp/pspexp-launcher.log`, or in a system's own log written since it
  started (`~/.cache/pspexp-*.log`: the Dreamcast's); a game left running
  by a failed check is stopped, so the next one finds the PSP; they differ widely between boards, so they
  are recorded, not judged. `--min-fps N` adds a floor for one board. The
  best rate counts: an intro that waits on a logo (OpenTyrian's) draws
  little on purpose.

The report goes to `build/psp-tests/<time>/report.md`: each check, what was
measured, and the PSP's screenshots as PNG.

### 8.3 First run

The first run was on the Pi 400 with a PSP-1000 over USB: 20 checks in
about 100 s.

- **The memory stick over the link:** 2.3 MB/s writing and 4.4 MB/s
  reading. Each 64 KB is one request, and the file is opened and closed
  each time, far below the link's 25 MB/s, so there is room to improve.
- **Dates:** that PSP's clock was about 15.7 years behind. Files the PSP
  writes carry its clock, so the check only asks for a real date and
  reports the offset.
- **A bug found by the run:** the file manager redrew every turn even with
  nothing pressed. Fixed.

With the relink and games suites, the Pi 400 run is 34 of 34 in about
7 minutes.

| on the Pi 400 | frame rate | audio underruns |
|---|---|---|
| Cave Story | 50 fps | 0 |
| Freedoom 1 and 2 | 35 fps | 1 |
| Jazz Jackrabbit | 41 fps | 0 |
| OpenTyrian | 48 fps at its best | 0 |
| Simutrans | 25 fps | 0 |
| Game Boy, Game Boy Color, Mega Drive and NES | 60 fps | 0 |
| SNES | 50 fps | 0 |

The link comes back about 2.4 s after a drop.

**The GameShell** (same PSP, over USB) also passes 34 of 34. How it differs
from the Pi 400:

| on the GameShell | frame rate | audio underruns |
|---|---|---|
| Cave Story | 50 fps | 0 |
| Freedoom 1 and 2 | 33-35 fps, dips to 16 | 1 |
| Jazz Jackrabbit | 14 fps typical, 33 at best | 0 |
| OpenTyrian | 46 fps at its best | 0 |
| Simutrans | 24 fps | 0 |
| Game Boy, Game Boy Color, Mega Drive and NES | 59 fps | 0 |
| SNES | 50 fps | 0 |

- **The memory stick:** 1.65 MB/s writing and 2.2 MB/s reading.
- **Reconnecting:** the link comes back in about 2.6 s.
- **Screenshots:** at first each one took 16 s. The GameShell's Wi-Fi is
  slow, and the runner copied each 391 KB BMP to the Mac as it was. It now
  compresses them on the board, to about 44 KB, which brings a screenshot
  down to 3 s, as on the Pi.

### 8.4 Every game on the real PSP (2026-10-06)

Most games had been tried in PPSSPP only, and the user found many failing
in their first steps on the PSP. `tests/psp/all-games.sh`, on the board
with the PSP connected, starts each game from the launcher as a player
would (the control port selects it and presses cross) and gives the
console's games, through a systemd drop-in for the run, input logging, a
PSP screenshot every 300 frames and a script: the nub up, down, left,
right from 15 s, cross at 20 s, START + SELECT held from 28 s. It records
whether the game starts, its frames a second (median / best), whether its
sound reaches the PSP, which inputs it takes, whether START + SELECT ends
it, and whether the PSP is still there; it stops if the PSP is gone. A ROM
is given as `"Section::Title"` (`"Nintendo 64::Super Mario 64"`).

First run, 48 games on the Pi 400 and the PSP-1000: the PSP stayed through
all of them, Hurrican included (59 fps; the user's earlier crash with it
did not come back). It found, fixed the same day and run again:

| | found | fix |
|---|---|---|
| Celeste 64 | did not start (FMOD found no sound device), then the link dropped as it loaded | ALSA's output into the sound pipe; the pipe read at real time: 60 fps, sound, quits |
| Unreal Tournament | stood at its title (found by the user): waiting to open the sound pipe, whose reader had ended | the reader survives a signal: runs and quits |
| OpenRA (both) | never started from the launcher; then silent | recipe 3: `PSPEXP_HOSTGL=1`: 13-20 fps, sound, quits |
| Luanti, Lugaru HD, Red Alert | START + SELECT: the game hung in its own cleanup, the PSP never back | `_exit` 1.5 s after the way out |
| OpenRA's Red Alert | asked to quit, stopped polling and hung | ended after 3 s without reading input |
| Diablo, Celeste 64 | the stick had no up and down (Diablo found by the user) | the controller mapping's missing comma |

Then one ROM per emulator and every N64 game: Game Boy, Game Boy Color,
Mega Drive, NES 58-60 fps, SNES 49-50, Super Mario 64 57, Banjo-Tooie 57,
Ocarina of Time 44, GoldenEye and Perfect Dark 38 (first seconds): all
started, were heard, took the nub and quit. The sound was steady in every
game (the PSP's underruns 0-4 in half a minute). What it cannot tell: a
game that reads its input as state (`SDL_GetKeyboardState`,
`SDL_GameControllerGetAxis`) or the nub as a mouse shows no input events
while working; the numbers are from the first half minute (menus mostly);
each screenshot stalls the PSP a moment, its sound queue jumping then.

The screenshots, looked at on 2026-10-07 (the PSP away): the ScummVM games
black or a corner blown up (fixed, above in §3); Command & Conquer's
dotted intro (its interlaced movies, as on 2026-10-04); Hurrican's stripes
(its intro's raster bars); Perfect Dark black (its intro); X-Moto's first
notice too small to read at 800x600 shrunk; and Celeste 64 black in the
last run: its pictures stopped 5 s in (pages sent 0, at 60 fps), not in
the run before nor with the layer alone, so the board-GPU layer now says
why a frame is not read back or sent, for the next run on the PSP.

Next run on the PSP: Warcraft (`tests/psp/all-games.sh` with its entry;
added 2026-10-08 with only the stand-in to try it on), its sound and MIDI
music heard; Warcraft II, its text at 57 % and its CD music.

## 9. The PSP's clock from the board (2026-09-28)

A PSP whose clock battery ran flat starts years in the past: the user's
PSP-1000 was about 15.7 years behind. When the PSP connects, the console now
sets the PSP's clock, time zone and summer time from the board's. Nobody has
to touch the PSP's Settings.

- **Protocol:** CLOCK_SET goes from host to PSP and CLOCK_STATUS answers
  (SYSTEM channel), only with runtimes that announce `PSPEXP_FEATURE_CLOCK`.
  The message carries:
  - the board's UTC time;
  - its standard offset and summer time;
  - `min_diff`, below which the clock is left alone;
  - `PSPEXP_CLOCK_ZONE`, which also sets the zone.

  The answer holds the PSP's clock before and after, its zone, and what
  changed.
- **On the PSP:** setting the clock (`sceRtcSetCurrentTick`) and the
  registry's `/CONFIG/DATE` `time_zone_offset` and `summer_time` are
  kernel-only, so the USB driver does them (`pspexpUsbSetClock`,
  `pspexpUsbSetZone`, `pspexpUsbGetZone`). Settings > Date & Time shows the
  result, and the XMB clock follows.
- **Clock rule:** the console checks the clock on every connection and when
  the setting is turned on. It sets the clock when the PSP is more than 90 s
  off, and only when the board's clock is trusted:
  - **Synced:** a board synced from the network (`adjtimex` without
    `STA_UNSYNC`), or a Mac.
  - **Not synced yet:** it only corrects a PSP half a year or more off.
  - **Before 2025:** a board whose own clock is still before 2025 sets
    nothing.
- **Zone rule:** the zone and summer time follow when they differ, unless
  the board's own zone was never chosen. A fresh board says UTC, and must
  not put a correct PSP on UTC.
- **What the user sees:**
  - a message at the bottom of the launcher;
  - a line in the log;
  - `psp_clock=` in the control port's state;
  - `clock [force]` on the control port.
- **Setting:** System > PSP clock (on by default) turns it off.
- **Tests:** `tests/test_fs.c` covers the message with the stand-in's clock,
  which moves inside the process only. The link suite of `tests/psp/run.py`
  checks that the real PSP's clock and zone match the board.
- **Verified on the PSP-1000 via the Pi 400:** the clock was 15.7 years
  behind and was set. Then its zone (UTC+2 without summer time) was set to
  Athens' UTC+3 with summer time, and the user saw the XMB clock right.

## 10. System Monitor, Packages and Music (2026-09-28)

Apps now holds four built-in apps: Terminal, Files, **System Monitor** and
**Packages**, each with a cover of its own. The last two are views like the
file manager, and share one small interface (`appview.h`: open, input, frame,
render, exit, state). The console shows them in place of the launcher; hold
circle closes them (held select: the launcher; a select tap also closed
them until 2026-10-06).

### 10.1 System Monitor

`monitor.c` draws the board's own figures, read by `sysinfo.c`: on Linux from
`/proc` and `/sys`, on a Mac through sysctl, mach and libproc. It refreshes
once a second, so it costs the link very little.

- **Overview:**
  - **CPU:** a bar for each core, the frequency, the temperature and the
    load.
  - **Memory and swap.**
  - **The board:** its model, name, uptime and kernel.
  - **Storage:** the board's drives from the file manager's list, plus the
    PSP's memory stick.
  - **Network:** interfaces with an address, or Wi-Fi ones, with their rates
    and Wi-Fi quality.
  - **The PSP's link:** frames per second, bytes per second and latency.
- **Programs:** the busiest first, sorted by CPU or memory (square). Cross
  ends the highlighted one after a question, with SIGTERM.
  - **What can be ended:** only the user's own programs, not root's. On a
    board where everything runs as root (the Omega), only the program the
    launcher started. Others show a lock.

### 10.2 Packages

> **Superseded (2026-09-29):** the app shows the recipe catalogue on every board
> (docs/Package_Manager.md §7, §9). Its folder and web repositories as they were:
> [the superseded sections](archive/Launcher_and_Boards-superseded.md).

### 10.3 Status

Both apps were tried on the Pi 400 and the Omega, against the stand-in PSP
(the real PSP was off):

- the monitor showed each board's figures;
- on the Omega, Packages removed SNES (with the question) and installed it
  again from the board's repository.

The first run on the Omega also found a bug: after a reconnect, the file
manager and the new apps stayed black until something changed. Both now
draw again when the link is back.

The integration tests have an `apps` suite:
- Apps lists the four apps;
- the monitor has figures and both pages;
- the Packages list matches `pspexp-pkg list`.

`--store-writes` adds a remove and reinstall of the smallest package. The
control port gained `category NAME`.

### 10.4 Music, now Media (2026-10-05; films 2026-10-12)

The file manager's player (§7.2) left the music playing behind the
launcher with nothing to show it or bring it back, and its few buttons
were all there was. Music is now a built-in app (`music.c`, `MUSIC_APP`,
the fifth tile in Apps) with three views, START going round them:

- **Now playing**: title, artist, album, cover, format and where it is
  decoded, the time, and the next track;
- **Browse**: places first (~/Music, home, the board's drives, the PSP's
  `ms0:/MUSIC` and the whole stick while PSPExp runs), then folders and
  playable files only; cross on a song plays the folder from it, circle
  goes up (from a place's top: back to the places);
- **Queue**: the folder's tracks in play order; cross plays one, square
  removes one (not the one playing).

Square in Browse (2026-10-06) adds the highlighted song, or a folder's
songs and its subfolders' (4 deep, files before folders, at most 4000 at
a time and 10000 in the queue), to the end of the queue; with nothing
playing the first one added plays. A note at the foot says what was
added. Files' actions (triangle) have **Add to the music queue** for the
marked or highlighted entries (`music_add`).

| | |
|---|---|
| cross | pause / play (Browse: open, play; Queue: play) |
| left / right | 10 s back / on (Now playing) |
| L / R | the previous / next track |
| square | stop (Browse: add to the queue; Queue: remove) |
| triangle | play order: Shuffle (the current track stays first), Repeat off / all / one |
| START | the next view |
| circle | close (Browse: up); the music goes on |

The queue lives in `music.c`, not in a view: `music_tick`, called every
main-loop turn whatever is on the screen, moves on when a track ends. A
program taking the PSP still stops it.

**Files hands songs over.** A small registry (`appview.h`:
`FileHandler`, `app_file_handler` in `apps.c`) names the app that opens a
file by its name; Files asks it on cross, and for music closes and opens
Music with the folder queued from that song (`files_take_app`); closing
Music then goes back to Files as it was left (2026-10-06; it went to the
launcher at first). Files on
the memory stick are copied to the board first (`files_fetch`), as before.
Other apps can take other kinds of file the same way. Cross on the song
already playing only shows Music (`music_is_current`; it started over at
first), and Files marks that song with a note and shows its title at the
foot of the active panel; START > Music opens the app too (2026-10-06).

**Films: Music becomes Media** (2026-10-12, the user's choice: one app
for both, not a package of its own). The app keeps its id (`music`,
`app:music`), shown as **Media** with a clapperboard. A film (by its name:
`player_is_video`) is a K_PIPE track with a picture: `probe_video`
(ffprobe) gives its size, fitted into 480x272 (even sides, the pixels'
shape kept), its rate (halved until 31 or less) and whether it has sound.
One ffmpeg writes the sound to stdout (s16le, as for music) and the
picture to fd 3 (`fps`, `scale` with error-diffusion dithering,
`bgr565le` raw: the PSP's 5650 has red in the low bits; `rgb565le` swapped
red and blue). A film without sound gets `anullsrc` for the clock, as long
as the film (`-t`), or ffmpeg never ends. The pictures go into a ring of
32 (about a second) stamped by their count over the rate; `video_show`
puts the newest one due by the sound's clock on show. Three things found
on the Pi 400 with the PSP-1000:

- **The PSP refuses an upload of a texture id it holds**
  (`PSPEXP_RERR_DUPLICATE_ID`): the first picture stayed. Each picture
  now replaces the bytes of the tile textures (`pspexp_patch_texture`,
  PSPEXP_FEATURE_PATCH; an older runtime: release and upload), low
  priority (main RAM, not VRAM).
- **One frame in flight capped films at about 20 pictures a second**
  (272 KB a picture, the round trip each time). While a film is on the
  screen Media allows two (`pspexp_set_max_in_flight`, back to one when
  it closes), with two sets of tiles used in turn, so a picture never
  changes while the PSP may still draw it.
- **The pipe and the ring**: a 64 KB pipe (`F_SETPIPE_SZ` needs
  `_GNU_SOURCE`) took four main-loop turns a picture; at 1 MB ffmpeg ran
  ahead, and a full ring that dropped its oldest pictures dropped the
  ones due next (the screen stopped). Now a full ring is not read
  (ffmpeg waits), and a film's sound is read as far ahead as ffmpeg gives
  it (`film_audio`, 4 s) and sent LEAD ahead of the clock as for music,
  so neither pipe can hold the other up.

Measured (Pi 400, PSP-1000, `tests/psp/run.py apps`): 720p and 1080p
H.264 at 29.3–29.5 pictures a second shown, the film's clock 1.00–1.01x,
ffmpeg about half a core; the first picture about a second after cross.
In Now playing the film is full screen on black; a key brings the title,
the time and the buttons for 3 s (the first press only shows them),
circle closes Media and pauses the film. The textures go when a track
without a picture plays. Not yet tried: the GameShell (armhf, slower).

**Media servers** (2026-10-12, `upnp.c`): Browse's last place, "Media
servers", searches the network by SSDP (an M-SEARCH for MediaServer:1 to
239.255.255.250:1900, sent twice, 2.5 s of answers); each answer's
description gives the name and the ContentDirectory's control address
(relative ones resolved, URLBase honoured), one entry per control address,
sorted by name. A folder is a SOAP Browse (BrowseDirectChildren, 500 at a
time, up to 5000); the DIDL-Lite inside is read by its few tags (the
container and item ids, `dc:title`, `upnp:class`, the first `res` and its
duration). Folders, songs and films are shown (pictures and the rest
left out); cross on an item plays the folder's songs and films from it
(the queue's Track has an address, `player_start_stream`: ffmpeg and
ffprobe read http as they read files; the title and the film-or-song come
from the server, not a file name), square adds one. Searches and listings
run on a thread, a spinner meanwhile; circle walks back up (on the folder
come from), out to the servers, out to the places. HTTP through curl (a
dependency of pspexp-core since). `pspexp-term --upnp [N [ID]]` lists the
servers, or a server's folder, for checking by hand. Tried: MiniDLNA on the
Pi (folders, a film at its 25 fps and an MP3, from Media on the Mac's
stand-in PSP, and the same listings from the Pi), and a Plex server's
answer to the search (its name and address read right); `tests/test_upnp.c`
reads MiniDLNA-shaped answers.

**Tracker modules** (2026-10-13, `modplay.c`): MOD, XM, S3M, IT (and MPTM,
669, MTM, STM) play through libopenmpt in the console itself (K_MOD), no
longer through ffmpeg, when the board has it: opened at run time
(`dlopen("libopenmpt.so.0")`; Debian's ffmpeg depends on it, so the card
images do; the Omega, without it, keeps ffmpeg's way). Each chunk of sound
sent (1024 frames) is stored with what the module was when it ends
(position, pattern, row, speed, tempo, every channel's VU), 128 of them;
the view looks up the one heard by the play clock, not the one decoded
0.4 s ahead. In Now playing a module shows as a tracker: the title and
time, position / pattern / row / BPM / speed, each channel's number and
level, and 13 rows of the pattern with the heard one in the middle, each
character in its own 6-pixel cell (FONT_BADGE drawn one character at a
time: Inter is not monospaced) coloured by libopenmpt's highlight letters
(note, instrument, volume column, effect). A channel is 13 characters
(`C-5 01 v64 A0F`) while they fit, narrower down to the note with many;
up / down move along channels that do not fit. Redrawn about 30 times a
second while playing. Tried on the Mac's stand-in PSP with an XM of 10
channels.

**Icons went blank** (found on the Pi the same day, after Music and
Files): the launcher kept every icon it had drawn, each size apart, in 96
textures on the PSP and never let one go; once Music's and Files' icons
had filled them, icons not drawn before (the Settings tiles) were skipped
without a word. The 96 are now reused, least recently drawn first, each
new icon under a fresh id (900 in turn) so a texture still on screen is
never overwritten, and one drawn in this frame or the last is never
taken (`icon_slot_new` in `launcher_gfx.c`, as the covers already did).
Checked with 12 slots in place of 96: Music, Files and Apps and back to
Settings, every icon right.

**The launcher shows it.** The top bar has a note while music plays, a
pause sign while paused. The home screen gains a **Now playing** (or
**Paused**) pill, next to Play again: down then cross opens Music.

**The note button.** The PSP's ♪ button pauses and resumes from anywhere
in the console (not while a program has the PSP). The runtime reads it in
kernel mode with the volume buttons and sends it as `PSPEXP_BUTTON_NOTE`
(0x800000), runtimes from 2026-10-05; an older runtime never sends it.
The control port takes it as `note` (`press note`); its `state` always has
the `music.*` lines (view, track, shuffle, repeat, playing, title, pos,
dur, paused, ended, folder, error), whichever view is open.

Tested on the Mac's console against PPSSPP (three short MP3s in a folder):
browse to the folder and play, auto-advance through the three, the queue
and play order, the launcher's pill and top-bar icon, cross on the pill,
Apps > Music, Files handing a song over, the note button through the
control port. Not yet on the real PSP.

---

## 11. Sound on Bluetooth (2026-10-03)

With Bluetooth headphones or a speaker paired with the board, the sound of
everything on the PSP (games, emulators, the music player, the terminal's
programs) plays there instead of on the PSP, and the USB link carries none.

**System > Sound output** (shown where `pspexp-bt available` says the board
can: `bluetoothctl`, PipeWire's `pw-play`, a controller) scans for 8 s and
lists The PSP first, then the devices found, sound devices first, each
"connected", "paired" or new. Cross uses one: `pspexp-bt use` pairs it when
new (it must be in pairing mode), trusts and connects it (first letting go
of the device in use and any other connected, untrusted and disconnected as
for The PSP: one at a time, since 2026-10-08), waits for its
PipeWire node and remembers it in `~/.config/pspexp/audio-out`; the first
time, its volume is set to 40 % (at the device's own 100 % earbuds were far
too loud). Square forgets a paired device, START scans again; cross on The
PSP sends the sound back and lets the device go: it stays paired but is
disconnected and no longer trusted, since trusted earbuds reconnected to the
Pi by themselves whenever they left their case (and were not free for an
iPad); using it again trusts it again. The row shows where the
sound goes: the PSP, the device's name, or "PSP (NAME away)" while the chosen
device is not connected.

**The route.** The console checks every 3 s (`pspexp-bt route`, written to
`/tmp/pspexp-audio-route-UID` whole and renamed: "pw NODE NAME", "psp - NAME"
or "psp"). Every program's libpspexp looks at that file at most twice a
second (a `stat`); while it says "pw", `pspexp_audio_stream` writes to a
`pw-play` on the board (`host/libpspexp/pspexp_local_audio.c`, through a
socket so a dead pw-play is an error, not a SIGPIPE) and the PSP's stream is
reset once. So the sound moves to the earbuds a few seconds after they
connect, and back to the PSP when they go into their case. The music player
plays MP3 on the board then (ffmpeg), since the PSP's decoder cannot reach
the earbuds, and starts the track again at the same place when the route
changes. Cached sound effects (`PLAY_SOUND`, the native WarCraft port) stay
on the PSP.

**Volume.** The PSP's own volume buttons: the runtime reads them in kernel
mode (`pspexpUsbButtons` in the USB driver; user mode never sees them) and
sends them with the other buttons (`PSPEXP_BUTTON_VOLUP` / `VOLDOWN`, runtimes
from 2026-10-03). While the sound is on Bluetooth, libpspexp turns each press
into `pspexp-bt volume +5` / `-5` (held: repeated every 0.15 s after 0.4 s)
on the device's PipeWire node, in whichever program has the PSP; with the
sound on the PSP, they set the PSP's own volume as always.

**Seeing the volume.** After each change libpspexp reads the new level back
(pspexp-bt prints it) and sends `OSD_VOLUME` (`PSPEXP_FEATURE_OSD`, runtimes
from 2026-10-03): the PSP itself draws a bar for 1.5 s over whatever is on
screen (a speaker, the bar, the number; at each `PRESENT`, so in games
too). The console, whose views draw only on change, draws a frame when the
bar comes and one when it goes. The launcher's top bar shows a headphones
icon and the volume among the board's indicators while the sound is on
Bluetooth (`/tmp/pspexp-audio-volume-UID`, written by pspexp-bt). The control
port's `volume-bar N` shows the bar (tests). Tried on the PSP-1000 from the Pi the same day:
the bar in the launcher and in games, the top bar's volume ("works
excellent"). The GameShell (2026-10-03): its image gains PipeWire, WirePlumber,
the Bluetooth plugin and rfkill, and lingering for `cpi`
(`image-setup.sh`; `stage.sh` adds the rfkill rule); installed on the
running GameShell, `pspexp-bt available` passes and System > Sound output
shows; not yet tried with headphones there. ffmpeg joined the image the
same day (the music player's other formats): on the GameShell it decodes
30 s of FLAC in 1.1 s and of Ogg Vorbis in 1.4 s. After a reboot Sound output was missing: the
console asked `pspexp-bt available` once, before the Bluetooth controller
was up, and kept the no; a no is now asked again every 20 s, and System
gains the row when the answer changes. Pairing on the GameShell then failed: its
controller was not pairable (BlueZ's default there), so a pairing was
dropped the moment it was made; `pspexp-bt` now turns pairable on with the
power. Then the earbuds paired but would not connect ("a2dp-sink profile
connect failed: Protocol not available"): WirePlumber starts its Bluetooth
monitor only for a user with an active seat, and nobody logs in at a
PSPExp board (Raspberry Pi OS ships with that check off, Debian does not).
pspexp-core installs `/etc/wireplumber/wireplumber.conf.d/80-pspexp-bluez.conf`
(`monitor.bluez.seat-monitoring = disabled`; read when WirePlumber starts,
so after a reboot or `systemctl --user restart wireplumber`). Then the
FreeBuds connected to the GameShell and took its sound. There a song then
stuttered: measured while it played, pw-play starved about 6 times in 4 s
(PipeWire's ERR count), the CPU shared between the governor's thread (38 %),
WirePlumber, the console, ffmpeg and pw-play, none of them real-time
(RTKit grants only logged-in users, and PipeWire's rlimits file is for a
group no login sets). Now `user@.service` gets `LimitRTPRIO=95` (and nice,
memlock) through a drop-in, and so does `pspexp-term.service`, whose
pw-play children inherit it (`stage.sh`, `install.sh`): PipeWire's data
threads ran at FIFO 83-88 and the Bluetooth sink's errors stopped (2 in all);
and pw-play asks for 60 ms instead of 20 on a processor under 1.5 GHz. The song had also played "clearly fast
forward" there: measured with clicks one second apart, played through the
player and recorded from the sink's monitor, 12 ms pieces vanished (clicks
0.977-0.988 s apart) while the pitch stayed right; with 60 ms, 0.998 s
each, nothing lost. A frame dropped on the board's side is now logged
("Bluetooth sound: N frames dropped").

**The board's side.** The console is a system service, so the user's
PipeWire must run without a login: `install.sh` turns on lingering
(`loginctl enable-linger`), and `pspexp-bt` sets `XDG_RUNTIME_DIR` when
missing. The Pi 400's radio came soft-blocked; `pspexp-bt` unblocks it through
the sudo rule. Tested on the Pi 400 with HUAWEI FreeBuds SE (2026-10-03): the
music player, the Mega Drive emulator and OpenJazz, played by hand; SBC codec.
Bluetooth adds its own delay (typically 150-250 ms), noticeable in action
games, not in music. Ours on top was larger at first: pw-play's default
100 ms latency set the Bluetooth sink's cycle to 2048 samples at 48 kHz
(pw-top, Aladdin and OpenJazz playing: "very noticeable"); pw-play now asks
for 20 ms, which takes the cycle to 512 (`PSPEXP_BT_LATENCY` tries another
value). What remains is the game's own buffer (OpenJazz 2048 samples, the
emulators about 50 ms), a frame, and the radio's. Aladdin on the Mega Drive emulator
afterwards: "sound fx are almost instant with the button presses". The volume buttons on the PSP-1000, same day: working
("it rocks!").

**The GameShell's Bluetooth line** (2026-10-05). Music on the GameShell's
Bluetooth paused for over a second every three and skipped ahead, on the
Edifier R1700BT and then on the FreeBuds too, though PipeWire's own errors
stayed still and a recording of what it sent to the earbuds had no gap. The
AP6212's Bluetooth sits on UART1, and our device tree gave it no
`max-speed`: the line stayed at its 115200 baud, 93 kbit/s measured on the
port's counters, a third of what SBC needs. The 60 ms latency and real-time
priority of 2026-10-03 had only made it less audible (they were measured
with clicks, not a song). With `max-speed = <1500000>` (the R16's UART at its
24 MHz clock) the firmware loads in 0.8 s instead of 4.5, a song carries 271
kbit/s and plays right (heard on the FreeBuds). A running GameShell takes it
by its `.dtb` in /boot (the old one kept as `.dtb.prev`) and a reboot. Still
open: connecting a second device while one is connected fails
(`a2dp_select_capabilities() Unable to select SEP`): Sound output should
disconnect the other first. Hitches left after the fix came with the
GameShell's Wi-Fi: where two access points reach it weakly it scans, drops
(`locally_generated=1`) and hops between them, and each time the shared AP6212
radio holds Bluetooth back (0.1 s of sound dropped at a reconnect; a hop to the
other access point stopped the music for a moment). Wi-Fi power saving off
made it worse (Wi-Fi then takes more of the radio); the CPU governor made no
difference. Left as it is by the user's choice; pinning the access point, or
Wi-Fi off while the sound is on Bluetooth, would avoid it.
