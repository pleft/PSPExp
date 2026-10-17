# PSPExp on a ClockworkPi GameShell

A pocket console driving a PSP: put them side by side, or the GameShell in a
bag, and play on the PSP. PSPExp replaces the GameShell's own system
(ClockworkOS) with a small, current Debian 13 that starts straight into
PSPExp. The GameShell's screen, buttons and speaker are not used: the PSP
is all of that.

## What you need

- A ClockworkPi GameShell (the CPI v3 board).
- A microSD card, 8 GB or more. Flashing it **erases** what is on it,
  ClockworkOS included: keep the old card if you want to go back.
- A computer to write the card (balenaEtcher, Raspberry Pi Imager or `dd`).
- The PSP, [prepared](psp.md), its mini-USB data cable, and a **USB OTG
  adapter** for the GameShell's micro-USB port (the PSP plugs in there, as a
  USB device of the GameShell).

## Installing PSPExp

1. Get the PSPExp card image.
   - The download: `pspexp-gameshell-<version>.img.xz` on the
     [download page](https://pspexp.dev/download), with its SHA-256 checksum.
   - From this repository: `scripts/gameshell/base/build-kernel.sh`,
     `build-uboot.sh` and `build-image.sh` build it on a Mac (they need
     Docker or OrbStack); see the script headers.
2. Write it to the card with your flashing tool, put the card in the
   GameShell.
3. Plug the PSP into the GameShell's OTG port, start PSPExp on the PSP, and
   switch the GameShell on. The green LED lights when the system is up.
4. **The first boot takes a minute or two**: the system grows to fill your
   card. Then the launcher appears on the PSP.
5. On the PSP: **Settings > Network > Wi-Fi network**, pick your network and type its
   password on the on-screen keyboard. Then **Settings > Clock > Time zone**.

Everything else happens on the PSP: **Apps > Packages** to install games.

## Your first games

**Apps > Packages**: Freedoom, OpenTyrian, Cave Story, SuperTux, the
Revolution adventures, the emulators and more. Each appears under
**Games** (or as its system's tile) once installed. The card image comes
with none, so you choose what takes space. Packages offers the GameShell 63
of PSPExp's 80 packages: not the ten drawn by a Raspberry Pi's graphics
chip (SuperTuxKart, Celeste 64, Nintendo 64, Unreal Tournament,
Luanti, Trackballs, Open Surge, OpenRA's Red Alert and Tiberian Dawn,
Taisei: its own chip is too small for them),
nor Diablo and Wolfenstein: Enemy Territory, nor Heroes II, LinCity-NG and
C-Dogs: too slow on it, nor Hex-a-hop and Barrage, not yet tried on it.
Kenta Cho's shooters run slower than on a Pi but fluidly (rRootage the
slowest). Heroes III
plays on it at 14-20 frames a second; its first start takes about 4 minutes.

**ROMs** go in folders in the home folder: `roms/nes`, `roms/snes`,
`roms/md` (and `roms/sms`, `roms/gg`), `roms/gb`, `roms/gbc`, `roms/gba`,
`roms/lynx`, `roms/ws`, `roms/wsc`. The easiest way: copy them to the PSP's
memory stick, then in **Apps > Files** copy them from the **PSP** drive to
the folder. Only use ROMs of games you own.

**Warcraft: Orcs & Humans** and **Warcraft II** need your own copy, as on
the Raspberry Pi: `DATA.WAR` in the **Warcraft** folder of the home folder,
Warcraft II's files or disc image in **Warcraft2** (what each accepts and
how they play: [the Raspberry Pi page](raspberry-pi.md#your-first-games)).
The first start prepares the game: about 20 seconds for Warcraft, about
10 minutes for Warcraft II from a disc image. The GameShell plays their
MIDI music at a lower quality than the Pi (22 kHz), or the sound would
skip in battles; Warcraft II's music from a disc image keeps the CD's
quality.

## What it plays like

Measured on the PSP-1000:

| | |
|---|---|
| Freedoom, OpenTyrian | 35 fps, full speed |
| 2D emulators | full speed |
| Most OpenGL games | 40–60 fps |
| Neverball, Chromium B.S.U. | about 22 fps |
| Emilia Pinball | too slow to enjoy (7 fps): the GameShell's processor, not the PSP |
| Warcraft: Orcs & Humans | menus 60 fps, battles 44–52 fps |
| Warcraft II | 58–60 fps in a game, which runs a little slower than on a Pi (about 26 of its 30 steps a second) |

The Warcraft figures were measured on 9 October 2026 with a stand-in for
the PSP on the GameShell's USB cable, while the PSP was away; not yet on
the PSP itself.

## Good to know

- **The GameShell's battery** shows in the launcher's top bar beside the
  PSP's; the orange LED lights while it charges.
- **Updates** come through **Apps > Packages**; after one, the launcher
  offers **Settings > Restart PSPExp**.
- **Logging in** (for tinkerers): the user is `cpi`, password `cpi`. SSH is
  off on a new card: **Settings > This board > SSH** turns it on (or off),
  then `ssh cpi@gameshell.local` once on Wi-Fi. Change the password
  (`passwd` in **Apps > Terminal**): every card starts with the same one,
  and the launcher warns while it does.
- **Bluetooth headphones**: the GameShell has Bluetooth, and **Settings >
  Sound** pairs headphones as on the Raspberry Pi (see
  [Using PSPExp](using-pspexp.md#bluetooth-headphones)). Music plays right on
  Bluetooth earbuds (tried with HUAWEI FreeBuds) since the GameShell's
  Bluetooth line runs at full speed (2026-10-05; a card made before then needs
  its boot files updated). To switch to another speaker or headphones,
  disconnect the one in use first.
- **Media**: MP3s are decoded by the PSP; Ogg, FLAC, Opus and M4A by the
  GameShell (ffmpeg comes with the image), at a few percent of its processor.
  Films play too; how smoothly on the GameShell's slower processor is not
  measured yet.
- Packages live in `/opt/pspexp`.
