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
   - *The download: coming with the release (a compressed `.img.xz` and its
     SHA-256 checksum).*
   - From this repository: `scripts/gameshell/base/build-kernel.sh`,
     `build-uboot.sh` and `build-image.sh` build it on a Mac (they need
     Docker or OrbStack); see the script headers.
2. Write it to the card with your flashing tool, put the card in the
   GameShell.
3. Plug the PSP into the GameShell's OTG port, start PSPExp on the PSP, and
   switch the GameShell on. The green LED lights when the system is up.
4. **The first boot takes a minute or two**: the system grows to fill your
   card. Then the launcher appears on the PSP.
5. On the PSP: **System > Wi-Fi network**, pick your network and type its
   password on the on-screen keyboard. Then **System > Time zone**.

Everything else happens on the PSP: **Apps > Packages** to install games.

## Your first games

**Apps > Packages**: Freedoom, OpenTyrian, Cave Story, SuperTux, the
Revolution adventures, the emulators and more. Each appears under
**Games** (or as its system's tile) once installed. The card image comes
with none, so you choose what takes space.

**ROMs** go in folders in the home folder: `roms/nes`, `roms/snes`,
`roms/md` (and `roms/sms`, `roms/gg`), `roms/gb`, `roms/gbc`, `roms/gba`,
`roms/lynx`, `roms/ws`, `roms/wsc`. The easiest way: copy them to the PSP's
memory stick, then in **Apps > Files** copy them from the **PSP** drive to
the folder. Only use ROMs of games you own.

## What it plays like

Measured on the PSP-1000:

| | |
|---|---|
| Freedoom, OpenTyrian | 35 fps, full speed |
| 2D emulators | full speed |
| Most OpenGL games | 40–60 fps |
| Neverball, Chromium B.S.U. | about 22 fps |
| Emilia Pinball | too slow to enjoy (7 fps): the GameShell's processor, not the PSP |

## Good to know

- **The GameShell's battery** shows in the launcher's top bar beside the
  PSP's; the orange LED lights while it charges.
- **Updates** come through **Apps > Packages**; after one, the launcher
  offers **System > Restart PSPExp**.
- **Logging in** (for tinkerers): the user is `cpi`, password `cpi`, reachable
  over SSH as `gameshell.local` once on Wi-Fi. Change the password
  (`passwd`) if your network is not only yours.
- **Bluetooth headphones**: the GameShell has Bluetooth and the image turns
  it on, but sound to headphones has only been tried on the Raspberry Pi so
  far.
- Packages live in `/opt/pspexp`.
