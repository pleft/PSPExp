# PSPExp user manual

**Your PSP becomes a games console again.** PSPExp turns a Sony PSP into the
screen, the controls and the speakers of a small computer: a Raspberry Pi, a
ClockworkPi GameShell or an Onion Omega2+. The board runs the games; the PSP
draws them with its own graphics chip, plays their sound and sends back
every button you press. One USB cable is all it takes.

What you get on the PSP:

- **A launcher made for the PSP's screen**: games, emulated systems, apps
  and settings, with covers, favourites and search.
- **Games the PSP never had**: Celeste 64 (the 3D Celeste, on a Pi), Unreal Tournament (on a Pi), Doom (Freedoom), Quake, Quake II (demo), Luanti (Minetest, on a Pi), Lugaru HD, AssaultCube (online), Wolfenstein: Enemy Territory (online or against bots, on a Pi), Command & Conquer and Red Alert, Sonic Robo Blast 2 (3D Sonic), SRB2Kart (Sonic kart racing, online), Cave Story,
  Dink Smallwood, The Ur-Quan Masters (Star Control II), Hurrican, Egoboo, Zelda: Mystery of Solarus DX, SuperTux, OpenTyrian, Simutrans, OpenTTD, the RPGs Alter
  A.I.L.A. Genesis and Theia: The Crimson Eclipse, classic Revolution adventures and more, installed from the **Packages** app in a couple of presses.
- **Emulators**: NES, SNES, Mega Drive, Game Boy, Game Boy Advance, Atari
  Lynx, WonderSwan. Put your ROMs on the board and they show up with box art.
- **A music player**: MP3s are decoded by the PSP itself.
- **Bluetooth headphones**, on boards that have Bluetooth.
- **A file manager** that reaches both the board and the PSP's memory stick.
- **A real terminal**: the board's Linux shell on the PSP, typed on an
  on-screen keyboard.

It runs on the original PSP-1000 (tested) and needs custom firmware, since
it uses a small kernel driver for the USB link.

## Which board?

| | Raspberry Pi | ClockworkPi GameShell | Onion Omega2+ |
|---|---|---|---|
| Best for | the most games, the smoothest play | a handheld pair: the PSP on top of a pocket console | the smallest, cheapest setup |
| Games | all packages, OpenGL and 3D games (Quake, OpenArena) included, and SuperTuxKart, Trackballs and Open Surge, drawn by the Pi's own graphics chip | all packages; heavy 3D slower | 12 packages: Doom, OpenTyrian, Cave Story, the adventures, NES, Game Boy, SNES, Mega Drive |
| Bluetooth headphones | yes (Pi 400 tested) | yes (not yet tried with headphones) | no |
| Setting up | install Raspberry Pi OS, then PSPExp | flash a card image | install from a computer over the network |
| Guide | [Raspberry Pi](raspberry-pi.md) | [GameShell](gameshell.md) | [Omega2+](omega2.md) |

Every board shows the same launcher on the PSP and plays the same way; only
the setup and the list of packages differ.

## Getting started

1. [Prepare the PSP](psp.md): custom firmware and the PSPExp app, once.
2. Set up your board: [Raspberry Pi](raspberry-pi.md), [GameShell](gameshell.md)
   or [Omega2+](omega2.md).
3. Start PSPExp on the PSP, plug in the USB cable, and the launcher appears.
4. [Using PSPExp](using-pspexp.md): the launcher, packages, emulators,
   files, music, Bluetooth and the terminal.

> **Before the first public release.** PSPExp is not published yet: the
> downloads this manual mentions (the PSP files, the GameShell card image,
> the package repository) are marked *coming with the release*. Until
> then, everything is built from this repository with the scripts the
> board guides name.

## When something goes wrong

| What you see | What to do |
|---|---|
| The PSP says "waiting for host" | Check the USB cable (a data cable, not a charge-only one) and that the board is on. The board's console starts by itself at boot; give it a minute after power-on. |
| PSPExp does not start from the XMB | Custom firmware is not running (on many PSPs it must be re-enabled after a full power-off). |
| A game froze or will not quit | Hold **START + SELECT**: after a second the game is asked to quit, after two it is ended, and the launcher comes back. |
| The PSP itself is stuck | Hold **HOME** for 2 seconds to leave PSPExp. |
| No sound | Settings > Sound: is it on Bluetooth headphones that are not with you? Choose The PSP. |
| The launcher offers "Update the PSP" or "Restart PSPExp" | A newer version was installed: choose it, it takes seconds. |

More: the [cheatsheet](../cheatsheet.md) for command-line use, and the
[technical documents](../PSPExp_Technical_Architecture.md) for how it all
works.
