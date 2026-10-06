# Using PSPExp

The same on every board. Everything here is done with the PSP's own
buttons; the board needs no screen, keyboard or mouse.

## The same buttons everywhere

| Button | What it does, on every screen |
|---|---|
| cross | choose: open, play, confirm, type the highlighted key |
| circle | back: close, cancel, the folder above (in the terminal: Esc) |
| triangle | show more: details, the actions menu, a hidden password (in the terminal: Space) |
| square | mark or remove: favourite, mark a file, delete a letter, remove a package, forget a device, stop the music |
| START | search, refresh or rescan, options |
| SELECT | close an app; held, the terminal |
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
 12:04 Sat 03 Oct                  PSP 76%  |  headphones 40%  wifi  plug  board
     (star)    (GAMES)   (joystick)   (apps)   (gear)
   Favourites   Games    Emulators     Apps   Settings
                         34 games
                 (>) Play again  GoldenEye 007
 (X) Open                                                START Search
```

Left and right pick a tile, cross opens it; down then cross plays what
you were playing (or opens Music, on Now playing; left / right between
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
 (X) Play  ([]) Favourite  (/\) Details  (O) Home        START Search
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
| SELECT, held | the terminal (and back) |

## Playing

Cross on a game starts it. The launcher shows "Starting" and hands the PSP
over; when the game ends, the launcher comes back by itself.

**To leave any game, hold START + SELECT.** After a second the game is
asked to quit; still held after two, it is ended. Many games also have
their own Quit in their menus.

Each game comes with its buttons mapped for the PSP (the analog nub as a
mouse pointer in the point-and-click adventures and strategy games, as the
d-pad in others). Triangle on a game in the launcher shows its details.

## Packages: installing games

**Apps > Packages** lists what your board can install, in three sections:
Games, Emulators and Apps (L / R switch).

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
need the board online and take a few minutes (Theia: about 6 on a Pi). PSPExp itself is updated the same way;
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
| cross | open: a folder, a picture, a text file, music (in Music), a ROM (plays it), a package (installs it) |
| circle | the folder above; at the top of a drive, close Files |
| square | mark several files |
| triangle | copy or move to the other panel, delete, rename, new folder, details |
| START | options: hidden files, sorting, the same folder in both panels, a terminal here |
| SELECT | close |

The drives include the board's storage, any USB stick plugged into it, and
**the PSP's own memory stick** (drive **PSP**), while PSPExp runs. Copying
between the PSP and the board is the easy way to bring ROMs, music or
pictures over: put them on the stick with the PSP in USB mode, start
PSPExp, copy.

## Music

**Apps > Music** plays the music on your board, your USB sticks and the
PSP's memory stick. Cross on a song in **Files** opens it here too. It has
three views; **START** goes from one to the next:

- **Now playing**: the song, artist, album and cover, the time, and
  what comes next;
- **Browse**: your music folder, home, the drives, and the PSP's
  **MUSIC** folder. Cross opens a folder, or plays a song and the rest of
  its folder; circle goes up;
- **Queue**: the songs to come. Cross plays one, square takes it out.

| Button | |
|---|---|
| cross | pause / play |
| left / right | 10 seconds back / on |
| L / R | the previous / next song |
| square | stop |
| triangle | play order: shuffle, repeat (off, all, one song) |
| START | the next view |
| circle | close Music (back to Files, if you came from there); the music goes on |
| ♪ (note) | pause / play, from anywhere |

The music goes on while you use the launcher, Files or the other apps;
**Now playing** on the home screen, or Apps > Music, brings it back.
Starting a game stops it.

The cover comes from the file, or a `cover.jpg` / `folder.jpg` beside it.
**MP3s are decoded by the PSP itself**, on its second processor, the
Media Engine: the board barely works and the cable carries a few kilobytes
a second. WAV plays as it is; Ogg, FLAC, Opus and M4A are decoded by the
board when it has ffmpeg. The note button needs the PSP's PSPExp from
5 October 2026 or later (Settings > Update the PSP).

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
| Clock | Time zone, for the clock; PSP clock: let the board set the PSP's clock |
| Library | Refresh library: look for new games and ROMs now; Box art: download covers for ROMs, on or off |
| This board | About (the board and the PSP: models, versions, battery, the connection), Restart, Power off (they ask for a second press) |

When there is something to do, it comes first, before the groups:
**Update the PSP** (a newer PSPExp for the PSP is ready) and **Restart
PSPExp** (after PSPExp was updated).

**Apps > System Monitor** shows the board's processor, memory, temperature,
storage and network, and its programs (one can be ended from there if it
hangs).
