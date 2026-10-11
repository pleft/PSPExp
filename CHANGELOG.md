# Release notes

## The first release

The first public release of PSPExp: the PSP as the screen, sound and
controller of a small Linux board, which runs the games.

### Downloads

On the [Releases](https://github.com/pleft/PSPExp/releases) page, each file
with its SHA-256:

- **The PSP:** `PSPExp-psp-<version>.zip`. Unzip it at the root of the
  memory stick (it holds `PSP/GAME/PSPExp`). The PSP needs custom firmware.
- **Raspberry Pi 3, 4, 400, 5 (64-bit):** `pspexp-pi-<version>.img.xz`.
- **ClockworkPi GameShell:** `pspexp-gameshell-<version>.img.xz`.

Write a card image with Raspberry Pi Imager (*Use custom*) or balenaEtcher;
both take the `.img.xz` as it is. Plug the PSP into the board, start PSPExp
on the PSP, and power the board on: the launcher appears after a minute or
two on the first boot. The [user manual](docs/manual/README.md) walks
through it.

### What it does

- **A launcher on the PSP** for games, emulators, apps and settings: covers,
  favourites, search, notifications, all with the PSP's buttons.
- **Games installed from the PSP** (Apps > Packages), from PSPExp's package
  repository at `https://pkg.pspexp.dev`: Freedoom, Quake, Celeste 64,
  Unreal Tournament, Diablo, Sonic Robo Blast 2, Cave Story, OpenTTD,
  Command & Conquer, Warcraft I and II (with your own copy's data), SuperTux
  and more. Games whose authors publish them are downloaded from their own
  pages when installed; PSPExp hosts no game data.
- **Emulators:** NES, SNES, Mega Drive, Game Boy (Color, Advance), Lynx,
  WonderSwan and, on a Pi, the Nintendo 64. Bring your own ROMs.
- **Apps:** a music player, a file manager (the PSP's memory stick
  included), a system monitor and a Linux terminal.
- **Bluetooth headphones** on the Raspberry Pi and the GameShell.
- **Updates from the PSP:** the launcher says when a new PSPExp is out;
  Packages updates it, and the PSP's own part with Settings > Update the PSP.

### Boards

| Board | Packages | Setting up |
|---|---|---|
| Raspberry Pi (64-bit) | all 58, 3D included, and the ones drawn by the Pi's GPU | the card image |
| ClockworkPi GameShell | 47: not the nine drawn by a Pi's GPU, nor Diablo and Wolfenstein: Enemy Territory | the card image |
| Onion Omega2+ | 12: Doom, OpenTyrian, Cave Story, Jazz Jackrabbit, the adventures, NES, Game Boy, SNES, Mega Drive | for tinkerers: built and installed from this repository |

Tested on a PSP-1000, a Raspberry Pi 400, a GameShell and an Omega2+.

### First steps on a new card

- **Logins:** Raspberry Pi `pi` / `pspexp`, GameShell `cpi` / `cpi`. Every
  card starts with the same password: change it (`passwd` in Apps >
  Terminal).
- **SSH is off** until Settings > This board > SSH; it warns while the
  password is still the card's.
- **Wi-Fi** is joined from the PSP: Settings > Network > Wi-Fi network.

### Known problems

- **Hurrican** sometimes turns the PSP off when it starts (rarely; being
  looked into). Start it again.
- **The Omega2+** has no download or package feed yet: it is for tinkerers,
  built from this repository.
- **Other PSP models** than the PSP-1000 have not been tried.
- **The Raspberry Pi 2 v1.1** (32-bit only) is not supported by the image.

### Licences

PSPExp is under the [MIT licence](LICENSE); [THIRD_PARTY.md](THIRD_PARTY.md)
lists the files under their own. Every program PSPExp compiles and hosts
carries its copyright file, and its complete source is at
`https://pkg.pspexp.dev/sources/`. The card images' package lists and the
GameShell's kernel and boot loader sources are attached to each release.
Games belong to their authors, under their own terms.

PSPExp is not made by or affiliated with Sony. "PSP" names the console it
runs on.
