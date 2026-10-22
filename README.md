# PSPExp

**Exp**and your PSP by giving it a sidekick: a small Linux board, such as
a Raspberry Pi, plugged into its USB port. The board does the thinking and
runs the games; the PSP does what it is good at, drawing them with its own
graphics chip, playing their sound and taking your input. Together they play
games the PSP never had, and even a tiny board keeps up.

<p align="center"><img src="docs/images/launcher-games.png" width="640" alt="The PSPExp launcher on the PSP: the Games shelf, with Cave Story highlighted"></p>

<p align="center"><a href="https://youtu.be/CQm_w-4Oqfw">▶ Watch the demo</a>: the PSP with a Raspberry Pi 400 and a ClockworkPi GameShell, in 80 seconds.<br><a href="https://youtu.be/_rBYBBLUeNc">▶ Part II: the games</a>: Super Mario 64, Unreal Tournament, Celeste 64 and more on the PSP-1000.<br><a href="https://youtu.be/ex6nnDBRzHU">▶ Dreamcast on the PSP</a>: Power Stone 2, Sega Rally 2 and Sonic Adventure 2.</p>

What you get on the PSP:

- **A launcher made for it**: games, emulators, apps and settings, with
  covers, favourites and search, all driven by the PSP's buttons.
- **Games the PSP never had**, installed from the PSP in a couple of
  presses: Doom (Freedoom), Quake, Celeste 64, Unreal Tournament, Diablo,
  Sonic Robo Blast 2, Cave Story, OpenTTD, Red Alert, Warcraft I
  and II and Morrowind (with your own copy's data), SuperTux, and
  [dozens more](docs/manual/raspberry-pi.md#what-it-plays-like).
- **Emulators**: NES, SNES, Mega Drive, Game Boy (Color, Advance), Lynx,
  WonderSwan and, on a Pi, the Nintendo 64, the Dreamcast, the Nintendo DS and the Sega Saturn. Bring your own ROMs.
- **A media player for music and films, a file manager and a Linux terminal**, plus Bluetooth
  headphones on boards that have Bluetooth.

| | |
|---|---|
| <img src="docs/images/launcher-home.png" width="400" alt="The launcher's home screen"> | <img src="docs/images/packages.png" width="400" alt="The Packages app"> |
| The launcher's home screen | Packages: install games from the PSP |
| <img src="docs/images/celeste64.png" width="400" alt="Celeste 64 on the PSP"> | <img src="docs/images/supertuxkart.png" width="400" alt="SuperTuxKart on the PSP"> |
| Celeste 64, drawn by a Raspberry Pi's GPU | SuperTuxKart, the same way |

<sub>What the PSP shows, captured from the stream the board sends it.</sub>

## What you need

- A **PSP-1000** with **custom firmware**. PSPExp loads a small USB
  driver, which the official firmware does not allow. Other PSP models
  have not been tried.
- A **mini-USB data cable** (not a charge-only one).
- One of these boards:

| Board | Good for | Setup |
|---|---|---|
| **Raspberry Pi 3 / 4 / 400 / 5** | the most games, 3D included | a card image |
| **ClockworkPi GameShell** | a pocket pair: the PSP plus a handheld board | a card image |
| **Onion Omega2+** | the smallest and cheapest; 2D games and emulators | built and installed from a computer (for tinkerers, for now) |

## Getting started

1. [Prepare the PSP](docs/manual/psp.md): unzip PSPExp from the
   [download page](https://pspexp.dev/download) onto its memory
   stick. You only do this once.
2. Set up the board: write its card image from the same page
   ([Raspberry Pi](docs/manual/raspberry-pi.md),
   [GameShell](docs/manual/gameshell.md)), or build it
   ([Omega2+](docs/manual/omega2.md)). A Raspberry Pi that already runs
   Raspberry Pi OS (64-bit) needs only one command on it:
   `curl -fsSL https://pspexp.dev/install.sh | sh`.
3. Start PSPExp on the PSP and plug the cable into the board. The launcher appears.
4. In the launcher, open **Apps > Packages** and install a game.

[Using PSPExp](docs/manual/using-pspexp.md) covers the rest: buttons,
packages, ROMs, files, music and headphones.

## Downloads

The [download page](https://pspexp.dev/download) has the latest release's three files and how
many times each was downloaded. Each release on the
[Releases](https://github.com/pleft/PSPExp/releases) page holds the PSP's files and the card images for the Raspberry Pi and the
GameShell, built by GitHub's CI, each with its SHA-256; what is new and what
is known not to work is in the [release notes](CHANGELOG.md). The games come
from PSPExp's package repository (`https://pkg.pspexp.dev`). You can also
build everything from this repository on a Mac or a Linux computer: the
[pspdev](https://github.com/pspdev/pspdev) toolchain for the PSP side, and
the board guides name the scripts to run.

Tested on: PSP-1000, Raspberry Pi 400, GameShell, Omega2+.

## How it works, briefly

The game runs on the board, unmodified, and draws through SDL2 or OpenGL
as usual. PSPExp's layer catches those calls and sends the PSP what to
draw, not the finished pixels: textures once, then small lists of sprites
each frame. The PSP's graphics chip draws them at full speed, its sound
chip plays the mixed audio, and its buttons go back to the game as a
controller. Games too heavy for the PSP's chip are drawn by the board's GPU
instead, and each finished frame is sent as a picture.

## Documentation

| If you want to... | Read |
|---|---|
| set it up and play | [the user manual](docs/manual/README.md) |
| see what is new, and what does not work yet | [the release notes](CHANGELOG.md) |
| type commands on the board | [the cheatsheet](docs/cheatsheet.md) |
| build it, change it, run the tests | [Developing PSPExp](docs/Developing.md) |
| understand the design | [the technical architecture](docs/PSPExp_Technical_Architecture.md) |
| find any other document | [docs/README.md](docs/README.md) |

## Licence

PSPExp is under the [MIT licence](LICENSE). A few files keep their own
(patches to other projects, fonts, the GameShell's Wi-Fi settings):
[THIRD_PARTY.md](THIRD_PARTY.md) lists them. Games come from their authors, under their own
terms; none are in this repository.

PSPExp is not made by or affiliated with Sony. "PSP" names the console it
runs on.

## Contact

info@pspexp.dev, or an issue on GitHub.
