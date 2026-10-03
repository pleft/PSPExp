# Using PSPExp

The same on every board. Everything here is done with the PSP's own
buttons; the board needs no screen, keyboard or mouse.

## The launcher

```
 12:04 Sat 03 Oct                  PSP 76%  |  headphones 40%  wifi  plug  board
 [clock]  | Games                                                 12 games
 [games]  |   (disc)   (DISC)   (disc)   (disc)   (disc)  >
 [GB]     |   Quake  Cave Story OpenTyrian  ...
 [NES]    | Cave Story
 [apps]   | a short description of the game
 [gear]   | Game  /  PSP native
 (X) Play  ([]) Favourite  (/\) Details                  START Search
```

Down the left is the **rail**: one tile per category. Games, every emulated
system you have ROMs for, Apps, System, and Recent and Favourites once you
have some. To its right the category's **shelf**: games and ROMs as discs
with their cover in the window, apps and settings as icon tiles; the one
you are on is larger and glows, and its description shows underneath. The
top bar shows the time, the PSP's battery, and the board's own indicators
(sound, network, power).

| Button | In the launcher |
|---|---|
| up / down | the previous / next category |
| left / right | move along the shelf (L / R: five at a time; circle: back to the first) |
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
| triangle | remove it (asks first) |
| START | look for new packages |
| circle | close |

Each game's entry says how it is drawn: **SDL** and **OpenGL** games are
drawn by the PSP, **board GPU** games by the board's own graphics chip and
sent to the PSP as pictures (only on boards that can, the Raspberry Pi).

Installing runs in the background: the progress shows, and the game
appears in the launcher when done. A few games (the RPG Maker ones) are
downloaded from their developers' own pages while they install, so those
need the board online and take a few minutes (Theia: about 6 on a Pi). PSPExp itself is updated the same way;
after such an update, **System > Restart PSPExp** starts the new version,
and **System > Update the PSP** puts the PSP's side up to date too.

## Emulators and ROMs

An emulator package adds a **system**: NES, SNES, Mega Drive (with Master
System and Game Gear), Game Boy and Game Boy Color, Game Boy Advance,
Atari Lynx, WonderSwan, and on a Raspberry Pi the Nintendo 64. Its tile appears on the rail once there are ROMs in
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

New ROMs show up within seconds (or **System > Refresh library**), with
their titles cleaned up ("Legend of Zelda, The (USA)" becomes "The Legend
of Zelda") and box art downloaded when the board is online (turn that off
in **System > Box art**). Games with battery saves keep them on the board.
Only use ROMs of games you own.

## Files

**Apps > Files** opens a two-panel file manager.

| Button | |
|---|---|
| d-pad | move (left / right: a page) |
| up from the top | the drive button: cross lists every drive |
| L / R | the left / right panel |
| cross | open: a folder, a picture, a text file, music, a ROM (plays it), a package (installs it) |
| circle | the folder above |
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

Cross on a music file in **Files** plays it, then the rest of the folder.

| Button | |
|---|---|
| cross | pause / play |
| left / right | 10 seconds back / on |
| L / R | the previous / next track |
| triangle | stop |
| circle | back to the files; the music goes on |

The player shows the title, artist, album and the cover (from the file, or
a `cover.jpg` / `folder.jpg` beside it). **MP3s are decoded by the PSP
itself**, on its second processor, the Media Engine: the board barely
works and the cable carries a few kilobytes a second. WAV plays as it is;
Ogg, FLAC, Opus and M4A are decoded by the board when it has ffmpeg.

## Bluetooth headphones

On a board with Bluetooth (the Raspberry Pi, the GameShell), **System > Sound output** sends
all sound to headphones or a speaker instead of the PSP:

1. Put the headphones in pairing mode.
2. **System > Sound output**. It scans for a few seconds and lists **The
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
- Square on a device in the list forgets it; triangle scans again.

Bluetooth always adds a little delay. PSPExp keeps its own part small;
what remains depends on the headphones.

## The terminal

Hold **SELECT** in the launcher (or **System > Console**): the board's
Linux shell on the PSP, with an on-screen keyboard. Up to four shells,
colours, scrollback; `vi` and `top` work. On a Raspberry Pi 400 its own
keyboard types into it too.

| Button | |
|---|---|
| cross / square / circle / triangle | the highlighted key / Backspace / Space / Enter |
| d-pad | move on the keyboard; arrow keys when it is hidden |
| SELECT | show / hide the keyboard (held: the launcher) |
| L held / R held | Shift / Ctrl |
| L tapped | a menu: special keys, commands, themes, fonts, Files here |
| R + d-pad | scroll back |

## System

| | |
|---|---|
| Refresh library | look for new games and ROMs now |
| Wi-Fi network | join a network (an on-screen keyboard for the password) |
| Sound output | the PSP or Bluetooth headphones (boards with Bluetooth) |
| Time zone | for the clock |
| PSP clock | let the board set the PSP's clock |
| Box art | download covers for ROMs, on or off |
| Update the PSP | shown when a newer PSPExp for the PSP is ready |
| Restart PSPExp | shown after PSPExp was updated |
| Restart / Power off | the board (asks for a second press) |
| About | the board and the PSP: models, versions, battery, the connection |

**Apps > System Monitor** shows the board's processor, memory, temperature,
storage and network, and its programs (one can be ended from there if it
hangs).
