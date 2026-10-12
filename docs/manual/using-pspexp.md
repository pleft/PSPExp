# Using PSPExp

The same on every board. Everything here is done with the PSP's own
buttons; the board needs no screen, keyboard or mouse.

## The same buttons everywhere

| Button | What it does, on every screen |
|---|---|
| cross | choose: open, play, confirm, type the highlighted key |
| circle | back one step: cancel, the folder above; on an app's first screen, out to where you came from (in the terminal: Esc) |
| triangle | show more: details, the actions menu, a hidden password (in the terminal: Space) |
| square | mark or remove: favourite, mark a file, delete a letter, remove a package, forget a device, stop the music |
| START | search, refresh or rescan, options |
| SELECT, held | home: the launcher, from any app (from the launcher: the terminal) |
| L / R | the other tab, panel or page |
| ♪ (note) | pause or resume the music, from anywhere (not during a game) |

## The launcher

It opens on the **home screen**: five big tiles, **Favourites**, **Games**,
**Emulators**, **Apps** and **Settings**, with what each holds underneath
and, below, **Play again** with what you played last (it starts afresh;
the game's own saves are there as always) and, while music is loaded,
**Now playing** (or **Paused**) with the song. A note at the top shows
music playing, a pause sign that it is paused.

```
 12:04 Sat 03 Oct (bell)2          PSP 76%  |  headphones 40%  wifi  plug  board
     (star)    (GAMES)   (joystick)   (apps)   (gear)
   Favourites   Games    Emulators     Apps   Settings
                         34 games
                 (>) Play again  GoldenEye 007
 (X) Open                          SELECT Notifications  START Search
```

Left and right pick a tile, cross opens it; down then cross plays what
you were playing (or opens Media, on Now playing; left / right between
the two).

Inside a section, the same five run down the left as the **rail**, and the
section's **shelf** runs to its right: games and ROMs as discs with their
cover in the window, apps and settings as icon tiles. The one you are on
is larger and glows; its description shows underneath, and who draws it:
**PSP renderer** (the PSP's graphics chip, most games) or **Board GPU
renderer** (the board's own graphics chip, each frame sent to the PSP as a
picture: SuperTuxKart, the Nintendo 64 on a Pi). **Emulators** has
one tile per system you have ROMs for, with its console's picture (the
N64's controller, a Game Boy...), like the home screen's; cross on
one opens its ROMs ("Emulators > Nintendo 64"), circle goes back.

```
 12:04 Sat 03 Oct                  PSP 76%  |  headphones 40%  wifi  plug  board
 [star]   | Games                                                 12 games
 [GAMES]  |   (disc)   (DISC)   (disc)   (disc)   (disc)  >
 [joy]    |   Quake  Cave Story OpenTyrian  ...
 [apps]   | Cave Story
 [gear]   | a short description of the game
          | Game  /  PSP renderer
 (X) Play  ([]) Favourite  (/\) Details  (O) Home  SELECT Notifications  START Search
```

| Button | In the launcher |
|---|---|
| up / down | the previous / next section on the rail |
| left / right | move along the shelf (L / R: five at a time) |
| circle | back: from a system's ROMs to Emulators, else to the home screen |
| cross | play, or open |
| square | add to / remove from Favourites |
| triangle | details: description, how it draws, size, when last played |
| START | search everything: games, apps and every ROM, by name |
| SELECT | the notifications |
| SELECT, held | the terminal (and back) |

## Notifications

What PSPExp has to tell you shows as a **banner** at the top of the screen,
whatever you are in (the launcher, an app, Files, the terminal), one after
another when several come: updates for your packages or for the PSP, a
game that could not start, a battery running low (the PSP's or the
board's), the board losing its network, Bluetooth headphones gone,
storage nearly full, an install or a Wi-Fi join that finished.

Nothing is lost when you look away: the **bell** next to the date in the
top bar counts what you have not seen. In the launcher press **SELECT**
to open the list (or, on the home screen, **up** from the tiles to the
bell and **cross**), the last 50, newest first:

- **cross**: go where it is about (Packages for updates, Settings for the
  network or Bluetooth), when there is somewhere to go;
- **square**: remove it; **START** twice: clear them all;
- **circle**: close.

While you play, nothing interrupts the game: what came meanwhile is
summed up in one banner when you are back ("3 notifications while you
played") and waits under the bell.

Scripts and programs on the board can send their own (`pspexp-notify`:
see the cheatsheet).

## Playing

Cross on a game starts it. The launcher shows the game with "Starting" and
a turning wheel, which the PSP keeps on screen while the game loads, until
the game's first picture; when the game ends, the launcher comes back by
itself.

A game that cannot start says why at the bottom of the screen: Warcraft,
for example, when the data from your own copy is not on the board yet.

**To leave any game, hold START + SELECT.** After a second the game is
asked to quit; still held after two, it is ended, and a game that hangs
while closing is ended a moment later. Many games also have their own
Quit in their menus.

Each game comes with its buttons mapped for the PSP (the analog nub as a
mouse pointer in the point-and-click adventures and strategy games, as the
d-pad in others). Triangle on a game in the launcher shows its details.

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
| START | look for new packages |
| circle | close |

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
| circle | the folder above; at the top of a drive, close Files |
| square | mark several files |
| triangle | copy or move to the other panel, delete, rename, new folder, details, compress to a .zip |
| START | options: hidden files, sorting, the same folder in both panels, a terminal here |
| SELECT, held | the launcher |

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
sticks and the PSP's memory stick. Cross on a song or a film in **Files**
opens it here too. It has three views; **START** goes from one to the next:

- **Now playing**: the song, artist, album and cover, the time, and
  what comes next;
- **Browse**: your Music and Videos folders, home, the drives, the
  PSP's **MUSIC** and **VIDEO** folders, and **Media servers**: the ones on
  your network (Plex, Jellyfin, Kodi, MiniDLNA, a NAS or a router with
  DLNA on), found when you open it. Cross opens a folder, or plays a
  song (or film) and the rest of its folder; **square adds** the song, or a folder's songs (with its
  subfolders'), to the end of the queue; circle goes up;
- **Queue**: the songs to come. Cross plays one, square takes it out.

| Button | |
|---|---|
| cross | pause / play |
| left / right | 10 seconds back / on |
| L / R | the previous / next song |
| square | stop (Browse: add to the queue; Queue: take out) |
| triangle | play order: shuffle, repeat (off, all, one song) |
| START | the next view |
| circle | close Media (back to Files, if you came from there); the music goes on, a film pauses |
| ♪ (note) | pause / play, from anywhere |

The music goes on while you use the launcher, Files or the other apps;
**Now playing** on the home screen, or Apps > Media, brings it back. In
**Files** the song playing has a note beside it and its title shows at the
foot of the panel; cross on it (or START > Media) brings Media back
without starting the song over. Files' triangle menu has **Add to the
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
screen, up to 30 pictures a second (a 60 a second film shows every other
one); the PSP draws each picture and plays the sound. A key brings the
title, the time and the buttons over the picture for a few seconds (the
first press only shows them); cross pauses, left / right jump 10 seconds,
square stops. Closing Media pauses a film. A Raspberry Pi 400 shows a
720p or 1080p H.264 film at 29–30 pictures a second; films need ffmpeg,
which the card images have. From a media server, songs and films play
straight from it (nothing is copied to the board); the server's folders
look as it arranges them.

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
- Square on a device in the list forgets it; START scans again.

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

Settings shows its options in groups; cross on a group opens it (a group
with a single option opens that option at once), circle comes back.

| Group | |
|---|---|
| Network | Wi-Fi network: join a network (an on-screen keyboard for the password); on the GameShell also PSP direct |
| Sound | the PSP or Bluetooth headphones (boards with Bluetooth) |
| Clock | Time zone, for the clock; PSP clock: let the board set the PSP's clock (after its time zone changes, restart the PSP for the dates of files it writes to follow) |
| Library | Refresh library: look for new games and ROMs now; Box art: download covers for ROMs, on or off |
| This board | About (the board and the PSP: models, versions, battery, the connection), SSH (on or off: logging in from a computer; it warns while the password is still the card's own), Restart, Power off (they ask for a second press) |

When there is something to do, it comes first, before the groups:
**Update the PSP** (a newer PSPExp for the PSP is ready) and **Restart
PSPExp** (after PSPExp was updated).

**Apps > System Monitor** shows the board's processor, memory, temperature,
storage and network, and its programs (one can be ended from there if it
hangs).
