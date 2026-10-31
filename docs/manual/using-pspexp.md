# Using PSPExp

The same on every board. Everything here is done with the PSP's own
buttons; the board needs no screen, keyboard or mouse.

## The same buttons everywhere

| Button | What it does, on every screen |
|---|---|
| HOME | leave what is on screen: in an app or a game, press it twice (the first shows "HOME again to quit" for 3 seconds), and the launcher comes back; in the launcher, the PSP's own "Quit?" (to the XMB); in your own PSP games, the PSP's HOME menu (Exit Game: back to PSPExp); held 2 s, PSPExp quits from anywhere |
| cross | choose: open, play, confirm, type the highlighted key |
| circle | back one step: cancel, the folder above; it never closes an app (HOME does; in the terminal: Esc) |
| triangle | show more: details, a hidden password (in the terminal: Space) |
| square | mark or remove: favourite, mark a file, delete a letter, remove a package, forget a device, stop the music |
| START | the menu: what you can do with the highlighted item, then the screen's own (in a keyboard: done; in the terminal: Enter) |
| SELECT, held | in the launcher: the terminal (and back) |
| L / R | the other section, tab, panel or page |
| ♪ (note) | pause or resume the music, from anywhere (not during a game); ♪ + L records a video, ♪ + L twice a screenshot, ♪ + R saves the last 30 seconds (Settings > Recording) ([Recording](recording.md)) |

## The launcher

Three sections run along the top: **Games**, **Apps** and **Settings**;
**L and R** move between them (round from the last to the first). Each
screen shows one thing to choose from.

**Games** is a grid of your platforms, each with its console's picture and
how many games it holds: **Favourites** (the games you marked with
**square**, a yellow star before their name wherever they are), **PSP**,
your board's own games (named after it: Raspberry Pi 400, GameShell), then
each system you have ROMs for. The d-pad moves around the grid; **cross**
opens a platform: its games as a shelf of discs, each with its cover in the
window (left / right), the favourites first, then what you played last,
then the rest A to Z. **Cross** plays, **triangle** shows the details,
**circle** goes back to the platforms. PSPExp opens inside the platform of
the game you played last, on it: switch on, press cross, and you are playing
again.

- **PSP** holds your own PSP games kept on the board (ISO, CSO, or a zip
  holding one, in `roms/psp`), each with its own icon from the disc. The PSP
  runs them itself, not an emulator, and PSPExp comes back when you leave
  the game ([the PSPExp plugin](plugin.md) must be on: Settings > This PSP).
- Under a game a line says what it is and whether you played it lately; its
  size, the file and who draws it are in **Details**.

```
 12:04 (bell)2      L  [Games]  Apps  Settings  R        wifi  76%
  [ * ]          [ PSP ]       [ Pi 400 ]     [ Lynx ]
  Favourites     PSP           Raspberry Pi   Atari Lynx
  2 favourites   10 games      3 games        3 games
  [ DC ]         [ GB ]        [ GBA ]        [ MD ]
 (X) Open                                               START Menu
```

**Apps** is the same grid of tiles; cross opens one.
 **Settings** is one list
(below).

The top bar keeps to what tells you something: the clock; the **bell**, only
while there is news (with how many); the sections; on the right a note while
music is loaded, Bluetooth headphones and their volume, the board's network,
the board's own battery when it has one (the GameShell's, its icon before
it), and the PSP's battery. The bottom row shows at most three buttons;
square (favourites) and SELECT (notifications) work everywhere, and START's
menu has them too.

| Button | In the launcher |
|---|---|
| L / R | the previous / next section: Games, Apps, Settings |
| d-pad | around the grid; along a platform's shelf; up / down Settings' options, left / right a choice's value |
| cross | open, play, or change a setting |
| circle | from a platform's games back to the platforms |
| square | a game or ROM: make it a favourite, or no longer one |
| triangle | details: description, how it draws, size, when last played |
| START | the menu: Play, Add to favourites, Details, **Search** (everything: games, apps and every ROM, by name), Notifications, Restart PSPExp and Power off the board (each asked again) |
| SELECT | the notifications |
| SELECT, held | the terminal (and back) |

## Notifications

What PSPExp has to tell you shows as a **banner** at the top of the screen,
whatever you are in (the launcher, an app, Files, the terminal), one after
another when several come: updates for your packages or for the PSP, a
game that could not start, a battery running low (the PSP's or the
board's), the board losing its network, Bluetooth headphones gone,
storage nearly full, an install or a Wi-Fi join that finished.

Nothing is lost when you look away: the **bell** appears next to the clock
in the top bar while there is something you have not seen, with how many. In the launcher press **SELECT**
to open the list (or START > Notifications), the last 50, newest first:

- **cross**: go where it is about (Packages for updates, Settings for the
  network or Bluetooth), when there is somewhere to go;
- **square**: remove it; **START**: the menu (go there, remove, clear them all);
- **circle**: close.

While you play, nothing interrupts the game: what came meanwhile is
summed up in one banner when you are back ("3 notifications while you
played") and waits under the bell.

Scripts and programs on the board can send their own (`pspexp-notify`:
see the cheatsheet).

## Playing

Cross on a game starts it. A game that has a controls sheet shows it
first: what each of the PSP's buttons does in that game (for an emulated
game, which of the console's buttons each one is). Cross again
plays, circle goes back. The launcher then shows the game with "Starting" and
a turning wheel, which the PSP keeps on screen while the game loads, until
the game's first picture; when the game ends, the launcher comes back by
itself.

A game that cannot start says why at the bottom of the screen: Warcraft,
for example, when the data from your own copy is not on the board yet.

**To leave any game, press HOME twice.** The first press shows "HOME
again to quit" at the top for 3 seconds, and the game goes on; press it
again meanwhile and the game is asked to quit and the launcher comes back; a game still there two seconds later (one that asks
"are you sure?") is ended, and so is one that hangs while closing. Many
games also have their own Quit in their menus.

Each game comes with its buttons mapped for the PSP (the analog nub as a
mouse pointer in the point-and-click adventures and strategy games, as the
d-pad in others). Triangle on a game in the launcher shows its details.

**Typing in a game.** When a game asks you to type something (a
character's name), the PSP's own keyboard opens over the game: type as
you would in the PSP's menus and confirm, and the text goes into the game
(followed by Enter); cancel it to type nothing. It needs the PSP's part
from October 18, 2026 or later (Settings > Update the PSP).

## Packages: installing games

**Apps > Packages** lists what your board can install, in three sections:
Games, Emulators and Apps (L / R switch). **PSPExp** itself is the first
entry under Apps: when it says **Update**, cross installs the new PSPExp
(and the PSP's runtime with it); then **Settings > Restart PSPExp**, and
**Settings > Update the PSP** when the launcher offers it. When a new
PSPExp is out, the launcher says so ("A new PSPExp is available"; choosing
the notification opens Packages on it), and a section with updates shows
how many next to its name. It opens with
"Reading the packages" for a moment while it reads the list.

| Button | |
|---|---|
| up / down | a package |
| cross | install it (or update it, when a newer version is there) |
| square | remove it (asks first) |
| triangle | its whole description, the versions |
| START | the menu: install, update or remove it; look for new packages |
| HOME, twice | close Packages |

Each game's entry says its **renderer**: **PSP** (SDL or OpenGL games, drawn
by the PSP's graphics chip) or **Board GPU** (drawn by the board's own
graphics chip and sent to the PSP as pictures; only on boards that can, the
Raspberry Pi).

A few games come with terms to accept first (Unreal Tournament: the Epic
Games Terms of Service; Wolfenstein: Enemy Territory: id Software's licence
agreement; Diablo: DevilutionX's licence). Packages shows them with where to read them; cross
accepts and installs, circle declines and nothing is downloaded.

Installing runs in the background: the progress shows, and the game
appears in the launcher when done. A few games (the RPG Maker ones) are
downloaded from their developers' own pages while they install, so those
need the board online and take a few minutes (Theia: about 6 on a Pi).
If a game's download fails everywhere (its site gone), Packages says so:
put the game's file, as its makers released it, in `/opt/pspexp/downloads`
on the board (with **Apps > Files** from a USB stick, for example) and
install again; PSPExp checks it is the right file. PSPExp itself is updated the same way;
after such an update, **Settings > Restart PSPExp** starts the new version,
and **Settings > Update the PSP** puts the PSP's side up to date too.

## Emulators and ROMs

An emulator package adds a **system**: NES, SNES, Mega Drive (with Master
System and Game Gear), Game Boy and Game Boy Color, Game Boy Advance,
Atari Lynx, WonderSwan, and on a Raspberry Pi the Nintendo 64. It appears
under **Emulators** once there are ROMs in
its folder in the board's home folder:

| System | Folder |
|---|---|
| NES | `roms/nes` |
| SNES | `roms/snes` |
| Mega Drive / Master System / Game Gear | `roms/md`, `roms/sms`, `roms/gg` |
| Game Boy / Color | `roms/gb`, `roms/gbc` |
| Game Boy Advance | `roms/gba` |
| Atari Lynx | `roms/lynx` |
| WonderSwan / Color | `roms/ws`, `roms/wsc` |
| Nintendo 64 (Raspberry Pi) | `roms/n64` (.z64, .n64, .v64) |

On the Nintendo 64 the nub is the stick, cross A, square B, L the Z
trigger, R the R button, START Start, and the d-pad the four C buttons
(triangle is the N64's L).

New ROMs show up within seconds (or **Settings > Library > Refresh library**), with
their titles cleaned up ("Legend of Zelda, The (USA)" becomes "The Legend
of Zelda") and box art downloaded when the board is online (turn that off
in **Settings > Library > Box art**). Games with battery saves keep them on the board.
Only use ROMs of games you own.

## Files

**Apps > Files** opens a two-panel file manager.

| Button | |
|---|---|
| d-pad | move (left / right: a page) |
| up from the top | the drive button: cross lists every drive |
| L / R | the left / right panel |
| cross | open: a folder, a picture, a text file, music or a film (in Media), a ROM (plays it), an archive (unpack it, or see what is in it), a package (installs it) |
| circle | the folder above |
| HOME, twice | close Files |
| square | mark several files |
| triangle | details: size, where it is, when it changed |
| START | the menu: copy or move to the other panel, delete, rename, add to the Media queue, compress to a .zip; **Folder and view**: new folder, a terminal here, the same folder in both panels, swap the panels, hidden files, sorting, Media |

The drives include the board's storage, any USB stick plugged into it, and
**the PSP's own memory stick** (drive **PSP**), while PSPExp runs. Copying
between the PSP and the board is the easy way to bring ROMs, music or
pictures over: put them on the stick with the PSP in USB mode, start
PSPExp, copy.

**Archives** (ZIP, 7z, RAR, and tar with .gz, .bz2, .xz or .zst): cross on
one offers to unpack it into the other panel or here, or to show what is in
it first. It unpacks into a folder named after it (an archive holding one
folder unpacks as that folder). Triangle > **Compress to a .zip** packs the
marked files and folders, or the highlighted one, into a .zip beside them.
Archives open on the board: one on the PSP's memory stick is copied over
first. A zipped ROM still goes straight to its emulator.

## Media

**Apps > Media** plays the music and the films on your board, your USB
sticks and the PSP's memory stick, and your captures. Cross on a song or a
film in **Files** opens it here too. It has three tabs; **L / R** go from
one to the next. It opens on **Now playing** while something plays, else on
the **Library**:

- **Now playing**: the song, artist, album and cover, the time, and
  what comes next;
- **Library**: **Music**, **Videos**, **Gallery** (your recordings, clips
  and screenshots, a folder for each game), the drives, the **PSP**'s
  memory stick, and **Media servers**: the ones on your network (Plex,
  Jellyfin, Kodi, MiniDLNA, a NAS or a router with DLNA on), found when you
  open it. A folder with songs starts with **Play all** and **Shuffle**;
  songs show their title and artist, and the one playing is marked. Cross
  opens a folder, or plays that song (or film) alone: **Play all** and
  **Shuffle** play the folder;
  **square adds** the song, or a folder's songs (with its subfolders'), to
  the end of the queue; circle goes up;
- **Queue**: the songs to come. Cross plays one, square takes it out.

| Button | |
|---|---|
| L / R | the other tab |
| cross | pause / play (Library: open, play; Queue: play that one) |
| left / right | Now playing: the previous / next song; held: back / on in it |
| square | Now playing: stop; Library: add to the queue; Queue: take out |
| triangle | what the song is: title, artist, where the file is, its size or format |
| START | the menu: play, add to the queue, remove, stop; shuffle and repeat (off, all, one song); in the Queue also clear it |
| circle | Library: the folder above; Now playing and Queue: back to the Library (a film pauses) |
| HOME, twice | close Media (back to Files, if you came from there); the music goes on, a film pauses |
| ♪ (note) | pause / play, from anywhere |

The music goes on while you use the launcher, Files or the other apps;
**Apps > Media** brings it back. In
**Files** the song playing has a note beside it and its title shows at the
foot of the panel; cross on it (or START > Folder and view > Media) brings Media back
without starting the song over. Files' START menu has **Add to the
Media queue**, for the highlighted or marked songs, films and folders.
Starting a game stops it.

The cover comes from the file, or a `cover.jpg` / `folder.jpg` beside it.
**MP3s are decoded by the PSP itself**, on its second processor, the
Media Engine: the board barely works and the cable carries a few kilobytes
a second. WAV plays as it is; Ogg, FLAC, Opus and M4A are decoded by the
board when it has ffmpeg. The note button needs the PSP's PSPExp from
5 October 2026 or later (Settings > Update the PSP).

**Films** (MP4, MKV, WebM, AVI, MOV, MPEG and more: whatever ffmpeg
reads, H.264, H.265, VP9, AV1, with any sound) play full screen in
**Now playing**. The board decodes them and fits the picture to the PSP's
screen, up to 30 frames a second (a 60 a second film shows every other
one); the PSP draws each picture and plays the sound. A key brings the
title, the time and the buttons over the picture for a few seconds (the
first press only shows them); cross pauses, left / right go to the
previous or next film (held: back or on in it), square stops, circle goes
back to the Library. Closing Media pauses a film. A Raspberry Pi 400 shows a
720p or 1080p H.264 film at 29–30 frames a second; films need ffmpeg,
which the card images have.

**Tracker modules** (MOD, XM, S3M, IT) play as a tracker shows them: the
song's patterns scroll with the row you hear in the middle, each channel's
notes, instruments, volumes and effects in their colours, and each
channel's level above; up / down move along the channels when they do not
all fit. (The board needs libopenmpt, which comes with ffmpeg on the card
images; without it modules still play, without the patterns.)

From a media server, songs and films play
straight from it (nothing is copied to the board); the server's folders
look as it arranges them.

## Gallery

**Apps > Gallery** shows your screenshots, recordings and clips, a tile for
each game, then a game's pictures newest first. Cross shows a screenshot
full screen or plays a video; L / R show all, only the screenshots or only
the videos; square deletes; START's menu copies them to the PSP's memory
stick, for the PSP's own Photo and Video menus. More in
[Recording and screenshots](recording.md#the-gallery).

## Bluetooth headphones

On a board with Bluetooth (the Raspberry Pi, the GameShell), **Settings > Sound** sends
all sound to headphones or a speaker instead of the PSP:

1. Put the headphones in pairing mode.
2. **Settings > Sound**. It scans for a few seconds and lists **The
   PSP** and the devices it found.
3. Cross on your headphones. They pair and connect, and the sound moves to
   them, starting at 40 % volume.

From then on:

- **The PSP's volume buttons** set the headphones' volume, in every game; a
  bar on the PSP's screen shows it, and the top bar shows a headphones icon
  with the level.
- Headphones back in their case: the sound returns to the PSP within a few
  seconds; out again, it moves back to them.
- **The PSP** in Sound output brings the sound back for good and lets the
  headphones go, so your phone or tablet can have them.
- **Other headphones** chosen in Sound output take over: the ones before are
  let go first, as with The PSP. One device plays at a time.
- Square on a device in the list forgets it; START's menu scans again.

Bluetooth always adds a little delay. PSPExp keeps its own part small;
what remains depends on the headphones.

## The terminal

Hold **SELECT** in the launcher (or **Apps > Terminal**): the board's
Linux shell on the PSP, with an on-screen keyboard. Up to four shells,
colours, scrollback; `vi` and `top` work. On a Raspberry Pi 400 its own
keyboard types into it too.

| Button | |
|---|---|
| cross / square / circle / triangle | the highlighted key / Backspace / Esc / Space |
| START | Enter |
| L + triangle / R + square | Tab (completes names) / Ctrl-C (stops a program) |
| d-pad | move on the keyboard; arrow keys when it is hidden |
| SELECT | show / hide the keyboard (held: the launcher) |
| L held / R held | Shift / Ctrl |
| L tapped | a menu: special keys, commands, themes, fonts, Files here |
| R + d-pad | scroll back |

## Settings

Settings is one list, its options under group headings, each with what it
is set to on the right. **Up and down** move along it; **cross** changes the
highlighted one, and the hint at the bottom says how:

- **on / off** (Clips, Box art, SSH...) is a switch: cross flips it;
- a **choice** (Clip length, Quality) shows all its values side by side
  while highlighted, the current one filled: **left / right** or cross move
  it;
- an option with **›** opens its own page (a list of networks, the time
  zones, About);
- the rest do what they say (Refresh library; Restart and Power off ask for
  a second press).

When something is waiting, an update for the PSP or a restart for a new
PSPExp, it comes first, under **Ready**.

| Group | |
|---|---|
| Network | Wi-Fi network: join a network (an on-screen keyboard for the password); on the GameShell also PSP direct |
| Sound | the PSP or Bluetooth headphones (boards with Bluetooth) |
| Recording | Clips: ♪ + R saves the last 30 seconds, the board then recording all the time (off at first; ♪ + L recordings work either way); Clip length: 15, 30 or 60 seconds; Quality: High, Standard or Small, for the videos' size ([Recording](recording.md); what they made is in Apps > Gallery) |
| Clock | Time zone, for the clock |
| Library | Refresh library: look for new games and ROMs now; Box art: download covers for ROMs, on or off |
| This PSP | PSPExp plugin: on to start the PSP list's games and record your own PSP games ([the plugin](plugin.md)); PSP clock: let the board set the PSP's clock (after its time zone changes, restart the PSP for the dates of files it writes to follow) |
| This board | About (the board and the PSP: models, versions, battery, the connection), SSH (on or off: logging in from a computer; it warns while the password is still the card's own), Restart, Power off (they ask for a second press) |

When there is something to do, it comes first, before the groups:
**Update the PSP** (a newer PSPExp or PSPExp plugin for the PSP is ready) and **Restart
PSPExp** (after PSPExp was updated).

**Apps > System Monitor** shows the board's processor, memory, temperature,
storage and network, and its programs (one can be ended from there if it
hangs: cross, or START's menu; triangle shows a program's details).
