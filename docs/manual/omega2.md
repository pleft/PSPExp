# PSPExp on an Onion Omega2+

The smallest PSPExp: a board the size of a postage stamp, 580 MHz, 128 MB
of memory and no floating point unit, and still a PSP full of games,
because the PSP does the drawing. The launcher starts in about two
seconds and uses under 4 MB of memory.

## What you need

- An **Onion Omega2+** on a dock with a **USB host port** (the Expansion
  Dock or the Mini Dock), with its power supply.
- A **microSD card** in the Omega2+'s slot: PSPExp keeps its programs and
  packages there (in a `pspexp` folder; anything else on the card is left
  alone).
- The Omega on your Wi-Fi (Onion's setup wizard does this) and SSH access to
  it.
- A Mac or Linux computer for the installation.
- The PSP, [prepared](psp.md), and its mini-USB data cable.

## Installing PSPExp

> **For tinkerers, for now.** The first public release covers the
> Raspberry Pi and the GameShell (card images, and games from PSPExp's
> package repository). The Omega has no download and no public package
> feed yet: you build PSPExp and its games from this repository and put
> them on the Omega yourself, as below. A public feed for it comes later.

From this repository, on the computer:

  ```sh
  scripts/omega/build.sh                          # builds everything for the Omega (needs Zig)
  HOST=root@<omega-address> scripts/omega/install.sh
  ```

  The console then starts at every boot and waits for the PSP. Plug the PSP
  into the dock's USB port and start PSPExp on it.

The Omega's games are PSPExp's own builds, made for its processor. They
are built on the computer and put on the Omega's card with (for your own
Omega only: some, such as Jazz Jackrabbit, OpenTyrian and Cave Story,
carry their game data, which PSPExp does not pass on):

```sh
HOST=root@<omega-address> scripts/packages/pspexp-pkg-build build freedoom opentyrian nes
HOST=root@<omega-address> scripts/packages/pspexp-pkg-build publish
```

after which **Apps > Packages** on the PSP installs them (`publish` also
sends the list of games Packages shows, made from this repository's
recipes).

## The games

Twelve packages run on the Omega2+:

- **Games**: Freedoom, OpenTyrian, Cave Story, Jazz Jackrabbit (shareware).
- **Adventures** (ScummVM): Beneath a Steel Sky, Flight of the Amazon
  Queen, Lure of the Temptress, Drascula.
- **Emulators**: NES, Game Boy / Game Boy Color, SNES, Mega Drive.

The 3D and OpenGL games need a floating point unit and are not offered.
The adventures and Freedoom play well. Cave Story runs at about 30 fps
of its 50 and takes about half a minute to start (it makes its sound
effects in software, which is slow without a floating point unit).

**ROMs** go in `roms/nes`, `roms/gb`, `roms/gbc`, `roms/snes`, `roms/md`
under the console's home folder on the card. Copy them through
**Apps > Files**, for example from the PSP's memory stick. Only use ROMs of
games you own.

## Good to know

- **Media**: MP3 plays (the PSP decodes it) and 44.1 kHz WAV; other formats
  and films need ffmpeg, which the Omega does not have.
- **No Bluetooth** on the Omega2+.
- **Wi-Fi** can be changed from the PSP: Settings > Network.
- **Power**: if the Omega loses power while writing to its card, the card
  can need a check on a computer (a FAT file system check) before it is
  writable again.
