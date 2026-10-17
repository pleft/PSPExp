# Release notes

## Since 1.0.0

These came as package updates (2026-10-15 on): a board gets them through
Packages (Apps > PSPExp), and the PSP's part through Settings > Update the
PSP when it is offered; the card images are unchanged.
See the games in [Part II](https://youtu.be/_rBYBBLUeNc).

- **Media** (once Music) plays **films** as well as music: MP4, MKV, WebM,
  AVI and whatever ffmpeg reads, full screen, up to 30 pictures a second
  (a Raspberry Pi 400 keeps 720p and 1080p at 29–30).
- **Media servers**: Media's Browse finds the UPnP / DLNA servers on your
  network (Plex, Jellyfin, Kodi, MiniDLNA, a NAS) and plays their songs and
  films straight from them.
- **Tracker modules** (MOD, XM, S3M, IT) show their patterns as a tracker
  does while they play: the rows scrolling, each channel's level.
- **Archives in Files**: unpack ZIP, 7z, RAR and tar archives, see what is
  in them, and compress files to a .zip.
- **Shooters with the PSP's own controls**: Unreal Tournament, Quake and
  Quake II move on the face buttons, aim with the nub and fire with R;
  Unreal Tournament's menu cursor reaches the whole screen.
- **Heroes of Might and Magic II** (on a Pi), on fheroes2: its demo, or your
  own copy in the Heroes2 folder; R is a magnifier, the game's own pixels
  round the pointer, for its small text; the game's own cursors, moving
  smoothly.
- **Numpty Physics**: draw with the nub, and what you draw
  becomes solid.
- **LBreakout HD, Mirror Magic and Pushover**, each played on the
  PSP-1000 before being offered.
- **C-Dogs** (on a Pi): top-down run-and-gun at the PSP's own size, 60 fps
  in a mission on the PSP-1000.
- **LinCity-NG** (on a Pi): the city builder, the nub as the mouse, R the
  magnifier.
- **Heroes of Might and Magic III** (on a Pi), on VCMI, with your own copy
  (GOG's installer, or the folder of an installed game) in the Heroes3
  folder; R magnifies its small text.
- **Kobo Deluxe** (on a Pi), the first of the SDL 1.2 games, which PSPExp
  now runs too.
- **Taisei** (on a Pi 4, 400 or 5): the bullet-hell shooter, drawn by the
  Pi's graphics chip, 60 fps.
- **The PSP no longer stays black after a game** (its memory stick's light
  flashing until switched off): when the board's console restarted while
  the PSP was sending, the two never found each other again. The fix is in
  the PSP's part: Settings > Update the PSP.
- **A worn nub that rests off the middle** no longer moves the pointer,
  the player or the arrows by itself.
- **AssaultCube is no longer offered**: its text was too small to read on
  the PSP's screen and it did not play well; a board that has it keeps it
  until removed in Packages.
- **OpenArena** no longer seems to hang after a run that was cut off.

## The first release

The first public release of PSPExp: the PSP as the screen, sound and
controller of a small Linux board, which runs the games. See it in the
[demo video](https://youtu.be/CQm_w-4Oqfw).

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
