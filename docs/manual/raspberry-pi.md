# PSPExp on a Raspberry Pi

The Pi is the roomiest home for PSPExp: every package runs on it, the 3D
games included, and it has Bluetooth for headphones. PSPExp is tested on a
**Raspberry Pi 400**; a Pi 4 or 5 with the same system should behave the
same. The Pi needs no screen of its own: the PSP is its screen.

## What you need

- A Raspberry Pi 400 (or 4 / 5) with its power supply.
- A microSD card with **Raspberry Pi OS (64-bit), the Debian 13 "trixie"
  release**. The Lite version is enough.
- A network connection for the setup (Wi-Fi or Ethernet), and SSH turned
  on: Raspberry Pi Imager can set both, and your user name, before it
  writes the card.
- The PSP, [prepared](psp.md), and its mini-USB data cable.

## Installing PSPExp

1. Write the card with Raspberry Pi Imager (64-bit Raspberry Pi OS, with
   SSH, Wi-Fi and your user set in its settings), put it in the Pi and
   power it on. Wait for it to appear on your network.
2. Install PSPExp:
   - *With the release: one command on the Pi adds PSPExp's package
     repository and installs it (coming with the release).*
   - From this repository, on a Mac or Linux computer:

     ```sh
     HOST=<user>@<pi-address> scripts/pi/install.sh
     ```

     It sets the Pi up for PSPExp (the PSP's USB rule, the console as a
     service that starts at boot, the settings it needs) and installs
     PSPExp: from the package repository when one is configured
     (`~/.pspexp/repo-url`), otherwise built on the Pi itself.
3. Plug the PSP into any of the Pi's USB ports and start PSPExp on it. The
   launcher appears.

From then on the Pi starts the console at every boot and waits for the PSP;
you never need to log in to it again.

## Your first games

On the PSP: **Apps > Packages**. Pick a game (Freedoom, OpenTyrian, Cave
Story, Dink Smallwood, Alter A.I.L.A. Genesis, Theia, SuperTux, Quake, Quake II...) and press cross to install it; it appears under
**Games** when done. Emulators install the same way (NES, SNES, Mega Drive,
Game Boy, Game Boy Advance, Lynx, WonderSwan, and on a Pi the Nintendo 64)
and appear as their own tiles once you have ROMs for them.

**ROMs** go in folders in your home folder on the Pi: `roms/nes`,
`roms/snes`, `roms/md`, `roms/gb` and `roms/gbc`, `roms/gba`, `roms/lynx`,
`roms/ws`, `roms/n64`. Copy them with the file manager (**Apps > Files**) from a USB
stick or from the PSP's memory stick, or over the network as you would to
any Linux machine. Only use ROMs of games you own.

## What it plays like

Measured on the PSP-1000 with a Pi 400:

| | |
|---|---|
| Freedoom, OpenTyrian | 35 fps, the games' own full speed |
| Mr. Boom, the 2D emulators | 60 fps |
| Quake (shareware) | 45–60 fps, about 30 in the biggest fights |
| OpenArena, DreamChess, Emilia Pinball, SuperTux | about 30 fps |
| Music player | MP3 decoded by the PSP; the Pi does almost nothing |
| SuperTuxKart | drawn by the Pi's own graphics chip and sent to the PSP as pictures (only on a Pi); about 29 fps at most from the Pi, not yet measured on the PSP |
| Nintendo 64 (GoldenEye 007, Super Mario 64) | drawn by the Pi's graphics chip (only on a Pi); GoldenEye's first mission at about 20 fps in the emulator, the game at full speed; not yet measured on the PSP |
| Unreal Tournament | drawn by the Pi's graphics chip (only on a Pi): about 21 fps in the emulator; free with Epic's permission, its terms accepted in Packages |
| Celeste 64 | drawn by the Pi's graphics chip (only on a Pi): the Pi itself runs it at 60 fps; about 20 fps in the emulator, not yet measured on the PSP |
| Command & Conquer, Red Alert | drawn by the PSP: 25-28 fps in the emulator; EA's freeware, about 600 MB each to download |
| AssaultCube | drawn by the PSP: about 25 fps in the emulator, online matches; not yet measured on the PSP |
| Lugaru HD | drawn by the PSP: about 20 fps in the emulator, not yet measured on the PSP |
| Quake II (demo) | drawn by the PSP, like Quake: 22-24 fps in the emulator, not yet measured on the PSP |
| Luanti (Minetest) | drawn by the Pi's graphics chip (only on a Pi): 20-22 fps in the emulator |
| Trackballs, Open Surge | drawn by the Pi's graphics chip too (only on a Pi); about 30 fps in the emulator, not yet measured on the PSP |

## Games drawn by the Pi's graphics chip

Most games are drawn by the PSP itself: the Pi only tells it what to draw.
Some 3D games ask for more than the PSP's graphics chip can do
(Celeste 64, SuperTuxKart, Trackballs, Open Surge, Luanti). On a Pi these are drawn by the Pi's
own graphics chip instead, and each finished picture is sent to the PSP,
which shows it, plays the sound and reads the buttons as for any other
game. You don't have to do anything: install them from **Apps > Packages**
as usual. Their entry there says **Board GPU** under renderer, and the
launcher calls them **Board GPU renderer**. They are offered only on a
Pi; the GameShell's graphics chip is too small for them.

## Bluetooth headphones

The Pi has Bluetooth. Put your headphones or speaker in pairing mode, then
**Settings > Sound**, wait for the scan, pick them with cross. From
then on every game's sound plays in them, the PSP's volume buttons set
their volume (a bar shows on screen), and when they go back in their case
the sound returns to the PSP by itself. Choose **The PSP** in Sound output
to let them go, for your phone or tablet. Details:
[Using PSPExp](using-pspexp.md#bluetooth-headphones).

## Good to know

- **The Pi 400's keyboard** types into the PSP's terminal while the PSP is
  connected.
- **Music in other formats** (Ogg, FLAC, Opus, M4A) is decoded on the Pi by
  ffmpeg: `sudo apt install ffmpeg` if the player says it is missing.
- **Updates** come through **Apps > Packages** (PSPExp itself is a package
  too); after one, the launcher offers **Settings > Restart PSPExp**.
- **Wi-Fi and the time zone** can be changed from the PSP: Settings >
  Network, Settings > Clock > Time zone.
- Packages live in `/opt/pspexp`; your settings in `~/.config/pspexp*`.
