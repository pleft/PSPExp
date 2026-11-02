# Release notes

## Since 1.4.0

- **Celeste 64 and Theia** install every time: their download from itch.io
  sometimes fetched a web page instead of the game, and after three tries the
  install gave up ("no source gave the expected file").
- **Frozen Bubble** starts again on the GameShell (it ended at once).
- **Commander Keen 1** no longer ends at its start now and then on the
  GameShell: Commander Genius is built without its own downloader, which
  the episodes as packages never needed.
- **A game that cannot be installed on a board** says so in Packages and no
  longer stops the next installs. The Ur-Quan Masters is no longer offered on
  the GameShell (its data is in Debian's non-free, which the GameShell's card
  does not use): 65 packages there.
- **PSPExp no longer crashes** now and then when a package is installed or
  removed while the launcher is open (seen on the GameShell).
- **A game that hangs as it quits** no longer leaves the PSP frozen on its
  last picture: PSPExp ends what is left of it within a few seconds and
  takes the PSP back (seen with GLtron).
- **The four ScummVM adventures** start every time: Flight of the Amazon
  Queen and Lure of the Temptress ended at once about one start in two.
- **Button sounds** are sent to the PSP once a connection, not twice: no
  more resource errors in the log after the PSP reconnects.

## 1.4.0 (2026-11-01)

A new launcher, four more systems and Quick controls: Games, Apps, Media and
Settings on L and R, every platform with its box art and each game's story;
Sega CD, 32X, PC Engine and Neo Geo; START for recording, clips and the
board's state from anywhere; and themes. New card images for the Raspberry Pi
and the GameShell have all of it in; a board already set up gets the same
through Apps > Packages, then Settings > Update PSP for the PSP's part.

- **A new launcher**, XMB-like: four categories along the top, **Games**,
  **Apps**, **Media** and **Settings**, that **L / R** move between from
  anywhere, each coming back where you left it. The d-pad moves along one
  list, the highlight staying on its second row while the list moves;
  **cross** goes in, **circle** back one level. A line under the categories
  says where you are ("Games › Super Nintendo", "2 / 6"); the top line how
  the PSP is connected and to what ("USB · Raspberry Pi 400") and the PSP's
  battery. PSPExp opens on the game you played last, in its platform.
  Games lists your platforms (Favourites, PSP, your board's own games, one
  per system with ROMs), each its games, favourites first, then what you
  played last. Media holds the Gallery, Now playing, and Music and Videos,
  which open the Media app in those folders. Settings lists its groups,
  each its settings: on / off flips in place, a choice (Clip length,
  Quality) opens its values with the current one ticked. **START** is
  Quick controls (Notifications, Terminal; the PSP link, the board's
  network and Wi-Fi strength, the sound and the headphones' volume, the
  board's battery; Restart PSPExp, Power off),
  **SELECT** Search in Games; holding SELECT no longer opens the terminal
  (Apps or Quick controls do).
- **Games with their art**: a platform's games show their box art at its
  own shape, a PSP game in its disc. **Square** switches a platform between
  Focus (one game large) and a Grid of six; **triangle** opens a game's
  options: Add to favourites, Game details, and Sort by (Favourites and
  recent, Recently played, Title A-Z); each platform keeps its view and its
  order. Square no longer adds a favourite.
- **Record from Quick controls**: **Start recording** / **Stop recording**
  (as ♪ + L) and, with Clips on, **Save the last 30 seconds** (as ♪ + R),
  the PSP's dot answering as for the buttons. Needs Settings > Update the
  PSP. A screenshot's white dot (♪ + L twice) now shows just after the
  picture is taken, so it is no longer in it on a still screen.
- **When the board goes**, the cable pulled or the board stopped, the PSP
  keeps its last picture, darkened, under a card: "Connection lost",
  waiting for it to come back. **Triangle** shows the status page
  (the link's details) and goes back to the card; **HOME** quits. The
  launcher comes back by itself with the board, as before. Needs Settings
  > Update PSP.
- **Themes**: Settings > PSP > Theme colours every screen, the
  launcher and the apps: Midnight (the blue, as before), Crimson, Emerald,
  Violet, Amber or Graphite, each previewed as you move to it. What the
  PSP draws itself follows it too (the "HOME again to quit" bar, the
  volume bar, the card when the board goes): needs Settings > Update PSP.
- **Sega CD and 32X**, with the Mega Drive package: Sega CD discs in
  `roms/segacd` (.chd, or .cue with its tracks), with your console's BIOS
  in `roms/segacd/bios`; 32X games in `roms/32x` (unzipped), both played
  by PicoDrive (now on the Raspberry Pi too), at full speed on a Pi 400
  and on a GameShell (whose PicoDrive now has its recompiler). Apps > Packages updates Mega Drive to
  bring them.
- **PC Engine and Neo Geo**: Apps > Packages has **PC Engine** (HuCards
  in `roms/pce`, CD games in `roms/pcecd` with your System Card in
  `roms/pcecd/bios`; US HuCards dumped bit-reversed play too) and **Neo Geo** (FinalBurn Neo's zips in
  `roms/neogeo`, with the BIOS `neogeo.zip` beside them; each game shows by
  its full name, "Metal Slug - Super Vehicle-001", and the BIOS not at all).
  Game details finds their descriptions too (the board makes its database
  again once, for the new systems).
- **Descriptions of your ROMs** in Game details, with who made them, the
  year and the genre: from the LaunchBox Games Database, which the board
  downloads once (110 MB, kept as 40 MB) and then reads offline, every
  system PSPExp plays included (Dreamcast and PSP too; all 35 test ROMs
  across ten systems found by name). Up / down scroll the whole description;
  square shows the file's facts (size, file, emulator) instead.
- **Search, Game details and About** in the same look as the rest: Search
  keeps the categories over it, its results a list like a platform's with
  the covers; Game details is a screen of its own (the art, the whole title,
  the description, what is known), back where you were with circle; About
  is a list under Settings > your board (Raspberry Pi 400, GameShell...),
  one fact a row.
- **Button sounds**: a soft click answers each press in the launcher and
  the apps (a tick as you move, rising for cross, falling for circle, a pop
  for a panel), played by the PSP over the music; **Settings > Sound >
  Button sounds** turns them off.
- **The same buttons in every app**: **triangle** opens the highlighted
  thing's options in a panel at the right (Details first, then what you can
  do: copy, delete, remove, add to the queue, stop...) in Files, the
  Gallery, Media, Packages, the System Monitor and Notifications; **START**
  opens **Quick controls** over any screen (Notifications, Terminal, Restart
  PSPExp, Power off). Square no longer deletes, removes or marks straight
  away: those are in triangle's options. Files marks several files with
  triangle > **Select items** (cross marks, circle ends it). The terminal
  keeps its keyboard (START is Enter).
- **Every app has a title bar** with its name: an app no longer passes for
  the launcher. The hints at the bottom sit where the PSP's buttons are:
  "HOME Close" first at the left (the way out, the same as for games),
  START at the right end. Circle only steps back inside an app; the
  Notifications list no longer closes on it either.
- **Labels without "the" and "this"**: Settings' groups are **PSP** and
  your board's own name (**Raspberry Pi 400**, **GameShell**...) where
  they were This PSP and This board; **Update PSP**; lines say "Mac's
  drives", "Restart Raspberry Pi 400", "Power off GameShell" rather than
  "the board". On the PSP, its card says **Connection lost**.
- **Plainer words**: the line under a game says what it is and whether you
  played it lately (sizes, files and how it draws are in Details).
- **HOME leaves what is on screen**: in an app (Files, Media, Packages,
  the terminal...) or a game, press it twice ("HOME again to quit" shows
  for 3 seconds after the first) to go back to the launcher; in the
  launcher it is the PSP's own "Quit?" as before. **START + SELECT no longer
  quits a game** (it still does until Settings > Update PSP). Holding
  HOME 2 s still quits PSPExp from anywhere.
- **Screenshots**: hold ♪ and tap **L twice**, quickly: a white dot flashes
  and the board saves the screen as a PNG. In PSPExp and in your own PSP
  games, while recording too. ♪ + L on its own now starts or stops a
  recording a third of a second after the press (once a second L is not
  coming), the recording counting from the press.
- **The Gallery, a page for each game**: recordings, clips and screenshots
  go to `Gallery` in the board's home folder, in a folder named after the
  game (a PSP game's own title, the launcher's name for PSPExp's games).
  Videos from before, in `Videos/PSPExp`, move once into `Gallery/Earlier`.
  **Media > Gallery** shows them: the
  games, each with its latest picture, then a game's pictures newest first,
  a video's length on it; L / R show all, the screenshots or the videos.
  Cross shows a screenshot full screen (left / right the next) or plays a
  video, back in the Gallery when it ends; square deletes; START's menu
  copies them to the PSP's memory stick, where the PSP's own Photo and Video
  menus show them (videos made ready for its player on the way).
- **Recordings and clips no longer vanish after PSPExp has been on for 36
  minutes**: from then on every ♪ + L recording and ♪ + R clip was dropped
  (screenshots were kept), however short; the PSP's clock was misread.
- **Videos the PSP itself plays**: recordings and clips are now made in
  H.264 Main profile (High before), which the PSP's own Video menu plays.
- **Clip length and quality**: Settings > Recording > Clip length makes ♪ + R
  save 15, 30 or 60 seconds; Quality (High, Standard, Small) makes the videos
  smaller, down to about a third.
- A recording's first moment, before the PSP's first picture arrives, shows
  that picture instead of black.
- **Media, made simpler**: L / R switch its tabs (Now playing, Library,
  Queue); left / right change song (held: seek); a folder starts with
  **Play all** and **Shuffle**, and choosing one song plays only that one (in Files too); songs show their titles and artists, the one
  playing marked; a **Gallery** place with your recordings; the play-order
  menu can clear the queue. It opens on the Library unless something plays.
- **Circle never closes an app** any more: it steps back (the folder above,
  Media's Browse) and does nothing at an app's top; HOME closes. Media's
  Now playing and Queue go back to Browse on circle. Holding SELECT no
  longer jumps from an app or Files to the launcher: SELECT is the app's.
- **START opens a menu**, everywhere: the highlighted item's actions first
  (Play, favourites, Details...), then the screen's (Search and Notifications
  in the launcher, Restart PSPExp and Power off on the home screen). In
  Files it holds the file's actions and "Folder and view"; triangle shows a
  file's details there, as Details elsewhere. Media, Packages, the System
  Monitor, Notifications and the Wi-Fi and Bluetooth lists have it too, and
  triangle shows a song's, a package's or a program's details.
- **Up / down stay inside a Settings group or a system's ROMs** (they
  jumped to another section); the rail is dimmed there, lit where up / down
  move along it.
- **Hints that match the buttons**: menus and dialogs no longer show the
  screen's hints under them; "Details" only where triangle opens them;
  Search's circle goes back to where you opened it; smaller fixes (Files'
  Compress keyboard's title, the Wi-Fi picker).
- **Clips switched on take effect at once**: before, ♪ + R (and Quick
  controls' Save the last seconds) waited until PSPExp next connected.
- **Game Boy Advance**: no more dark one-pixel line near the right edge of
  every game (the picture's pieces now meet exactly).
- **Saturn**: leaving a game no longer ends with a "Stopped with status 138"
  notice (a fault in Yaba Sanshiro's own thread code, patched), nor shows
  the picture upside down for a moment.
- **Battery saves** of the cartridge emulators are written also when HOME
  has to end an emulator that does not close in time.
- **A PSP game left in its first seconds** (HOME › Exit Game) comes back to
  PSPExp instead of the XMB.

## 1.3.0 (2026-10-25)

Two big additions: recording what you play, and your own PSP games kept on
the board and played on the PSP. New card images for the Raspberry Pi and the
GameShell have all of it in; a board already set up gets the same through
Apps > Packages, then Settings > Update the PSP for the PSP's part.

- **Record what you play**: **♪ + L** starts and stops a recording, with its
  sound; a red dot in the screen's corner says it is on. It works in
  everything PSPExp runs and in your own PSP games (a UMD, an ISO, the PSP
  section). With **Settings > Recording > Clips** on, **♪ + R** saves the
  last 30 seconds (off at first: the board then records all the time). The
  videos go to `Videos/PSPExp` on the board, up to 30 frames a second;
  Settings > Recording > Videos plays them on the PSP. In a game's heaviest
  scenes the video gets fewer frames for a moment, so that the game keeps its
  own speed. On a GameShell the videos are less smooth (its USB is slower).
- **Your PSP games, kept on the board**: ISO, CSO or zipped games in
  `roms/psp` appear in the launcher's new **PSP** section, with their own
  icons; the PSP runs them itself, reading the disc from the board over the
  cable, with no copying to the memory stick, and PSPExp comes back when you
  leave the game. They start as fast and play as they do from the memory
  stick. Tested with Ace Combat X, Bomberman, Burnout Legends, Despicable Me,
  Gran Turismo and Pro Evolution Soccer 2014. Needs PRO custom firmware.
- **The PSPExp plugin** does both for your own PSP games. It comes with the
  PSP's download and with Settings > Update the PSP, and **Settings > This
  PSP > PSPExp plugin** switches it on: there is nothing to edit by hand. The
  first time PSPExp finds it off, a notification says so once.
- **The launcher**: PSP first after Favourites, where the launcher opens;
  a **This PSP** group in Settings with the plugin and PSP clock (Clock keeps
  the time zone).
- **Update the PSP restarts PSPExp by itself** after installing: it never
  did on a real PSP (it went to the XMB, and PSPExp had to be started by
  hand).
- **A link that drops in a game comes back**: if the board stops hearing the
  PSP while a PSP game runs (a GameShell's USB did, mid-recording), the PSP
  reconnects after a few seconds instead of staying silent until the game
  ends.
- **A recording no longer stops by itself**: once, the PSP read ♪ and L
  pressed together though neither was touched, and ended a recording; a ♪
  pair now counts only with ♪ held first, as it is pressed.
- **The PSP's download carries the licences** of the code and libraries
  inside PSPExp's PSP programs (PSPLINK, PSPSDK, newlib): `THIRD_PARTY.txt`
  and `licenses/`.
- **A film paused in Media while a game ran** came back with sound and a
  black picture: its picture is sent again now.
- **One command on a Raspberry Pi OS you already have**:
  `curl -fsSL https://pspexp.dev/install.sh | sh` on the Pi sets it up as
  the card image does, without writing a new card.
- Known limits: the cartridge emulators do not open zipped ROMs yet (unpack
  them into the system's folder).

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
