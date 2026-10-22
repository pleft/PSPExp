# Release notes

## Since 1.2.0

- **One command on a Raspberry Pi OS you already have**:
  `curl -fsSL https://pspexp.dev/install.sh | sh` on the Pi sets it up as
  the card image does, without writing a new card.

## 1.2.0 (2026-10-22)

New card images for the Raspberry Pi and the GameShell with all of the below
in, headed by two new systems on a Raspberry Pi 4, 400 or 5: the Nintendo DS
and the Sega Saturn. Every game's screens were also checked as the PSP shows
them, which brought the fixes and the three removals below. A board already set
up gets the same through Packages (Apps > PSPExp).

- **The Nintendo DS** on a Pi 4, 400 or 5 (melonDS): your own games in
  `roms/nds`; the PSP shows both screens (three layouts, SELECT + R), the
  nub is the stylus with SELECT held to touch. Mario Kart DS and New Super
  Mario Bros. at full game speed, 30 pictures a second; a Pi 4 or 400 at
  2.0 GHz for the busiest moments. Install it from Packages.
- **The Sega Saturn** on a Pi 4, 400 or 5 (Yaba Sanshiro): your own discs
  in `roms/saturn` and your console's BIOS in `roms/saturn/bios`. Panzer
  Dragoon, NiGHTS, Guardian Heroes, Saturn Bomberman and Sega Rally play at
  45-60 pictures a second from a Pi 400. Hold SELECT for the pad's X and Z;
  SELECT alone opens the emulator's menu. Install it from Packages.
- **Frozen Bubble and Kobo Deluxe start again**: since 1.1.0 they closed
  at once (the PSP keyboard's code asked SDL for something the older games'
  SDL does not have).
- **The PSP's keyboard opens only for text**: Powermanga, TecnoballZ,
  Frozen Bubble and LBreakoutHD no longer open it where nothing is typed.
- **Fixes from a check of every game on the PSP's screen**: Quake no
  longer opens the PSP's keyboard over the game; Hurrican starts from the
  launcher again; Trigger Rally's car choice takes cross (it was stuck
  there); OpenTTD's first start shows no graphics-card error; Egoboo's
  debug window is gone; Celeste 64 and Theia ask itch.io again when a
  download comes back wrong; Simutrans's first start says on its controls
  sheet that triangle closes the welcome window to play.
- **Command & Conquer removed** from the packages: part of its game files
  hid inside its disc's installer and its menus are too small for the
  PSP's screen; so is Tiberian Dawn on OpenRA, the same game. Red Alert
  stays.
- **Red Alert has a magnifier**: R shows its small menus and messages
  at full size round the pointer; R again, the whole screen.
- **X-Moto removed** from the packages: its text is too small to read on
  the PSP's screen, and its first start needs a mouse and a typed name.
  The PSP's keyboard keeps what it learned for it (a button can open it,
  text arrives as typed).
- **Commander Keen 4** has its own cover.
- **Emulators a little lighter**: the player
  now tells the emulator which frames won't be shown.

## 1.1.0 (2026-10-19)

New card images for the Raspberry Pi and the GameShell, with all of the below
already in; the GameShell's has a kernel fix (the board could freeze when the
PSP was unplugged during a game). A board already set up gets the same through
Packages (Apps > PSPExp), and the PSP's part through Settings > Update the PSP
when it is offered.
See the games in [Part II](https://youtu.be/_rBYBBLUeNc), and the Dreamcast in
[Dreamcast on the PSP](https://youtu.be/ex6nnDBRzHU) (filmed with Flycast on a Mac).

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
  of Shadow of Death or the Complete edition (GOG's installer, or the folder
  of an installed game) in the Heroes3 folder; R magnifies its small text,
  and the pointer grows with it. On the GameShell too.
- **Kobo Deluxe**, **Powermanga**, **TecnoballZ** and **Frozen Bubble** (on a
  Pi), the first of the SDL 1.2 games, which PSPExp now runs too, and
  Kenta Cho's OpenGL shooters **rRootage**, **Parsec47** and **Tumiki
  Fighters**, drawn by the PSP at 60 fps; **GLtron**, **Neverputt**, **Noiz2sa** and **GNU Robbo** too.
- On the GameShell, games that keep shared high scores (Kobo Deluxe's
  players, LBreakout HD, Mirror Magic, Powermanga, TecnoballZ) can now save
  them: the board's user joins the games group.
- **Freeciv**, the Civilization-style strategy game: the nub as the mouse,
  R to read its small text, the d-pad moves a unit, square goes to the next
  one, triangle founds a city, START ends the turn. It takes a while to
  load, and its menu shows once you move the pointer.
- **The Dreamcast** on a Pi (Flycast, drawn by the Pi's graphics chip):
  your own discs in `roms/dreamcast`; Power Stone and Rayman 2 play at
  about 50-60 frames a second. Install it from Packages.
- **Morrowind** on a Pi, on OpenMW: your own copy (Steam, GOG or the CD)
  in the Morrowind folder; the PSP's shooter controls, menus that scroll
  with the pointer, play full screen.
- **A controls sheet before a game**: choosing a game shows what each
  button does in it, for every game in Packages; an emulated game shows
  which of the console's buttons each PSP button is. Cross plays, circle
  goes back. Installed games get theirs as an update in Packages.
- **Pinball**: the d-pad moves in its menu again. **SuperTux**: L and R
  look up and down (they did nothing). **GNU Robbo**: square shoots (it
  walked). **GLtron**: START pauses (it did nothing).
- **Mirror Magic** draws again (it stopped after loading), and the PSP's
  keyboard opens once for a game's text field (X-Moto's profile name
  opened it over and over).
- **Typing in games**: when a game asks for text (a character's name), the
  PSP's own keyboard opens over it (with the PSP's part updated).
- **Commander Keen 1 and 4**: id Software's platform classics, their
  free episodes, at the PSP's own screen size (cross jumps; hold it to jump
  higher).
- **Hex-a-hop**, the hexagon puzzle (a hundred levels), and **Barrage**, a
  three-minute shooting range, on a Pi.
- **No more pointer trail** in games that draw only on change (Freeciv,
  Heroes II): the pointer no longer leaves copies of itself behind.
- **Neverball** now fills the PSP's screen (480x272) on a new install.
- **Taisei** (on a Pi 4, 400 or 5): the bullet-hell shooter, drawn by the
  Pi's graphics chip, 60 fps.
- **The PSP no longer stays black after a game** (its memory stick's light
  flashing until switched off): when the board's console restarted while
  the PSP was sending, the two never found each other again; and when its
  USB gets stuck it now restarts it by itself, as a replug would. The fix is
  in the PSP's part: Settings > Update the PSP.
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
