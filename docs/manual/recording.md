# Recording your PSP games

**Your PSP games, recorded with their sound: your own UMD and ISO games, and
everything you play in PSPExp.** Press **♪ + R** and the last 30 seconds are
saved as a video; press **♪ + L** to start a recording and again to stop it.
A small dot in the corner of the screen says it heard you. The videos are
kept on the board, ready to watch or share.

This is PSPExp Capture. In PSPExp (its games, emulators, apps and the
launcher) it works by itself. For a game started from the XMB, a small
plugin for the PSP's custom firmware sends the game's picture and sound down
the USB cable, and the board makes the videos; the game runs on the PSP as
always.

## What you need

- A **board with PSPExp** (a Raspberry Pi, tested on a Pi 400), plugged into
  the PSP with the same USB cable. The board's PSPExp starts the recorder by
  itself; it needs ffmpeg, which the card images have.
- For what you play in PSPExp: nothing more, once PSPExp on the PSP is up to
  date (Settings > Update the PSP).
- For games started from the XMB: a PSP with **PRO custom firmware** (tested
  on a PSP-1000 with 6.60 PRO-C; ARK has not been tried yet) and the **PSPExp
  download for the PSP**: the plugin comes with it, in
  `PSP/GAME/PSPExp/capture/` on the memory stick, switched on once as below.

## In PSPExp

Play as usual: ♪ + R and ♪ + L work everywhere in PSPExp, in the board's
games and emulators as in the launcher, with the same dot. **♪ on its own
still pauses and resumes the music**, when you let go of it.

## Switching on the plugin, for games from the XMB

The plugin is switched on in custom firmware's list of game plugins, once:

1. Put the PSP in USB mode (Settings > USB Connection on the XMB).
2. On the memory stick, open `seplugins/game.txt` (make the folder and the
   file if they are not there) and add this line:
   `ms0:/PSP/GAME/PSPExp/capture/pspexp_capture.prx 1`
3. Leave USB mode.

The `1` at the end switches it on; a `0` switches it off. PRO's recovery menu
(from its VSH menu: SELECT on the XMB) lists it under Plugins too. It stays out of
PSPExp itself, so it can stay on all the time.

## Recording

In PSPExp you can press the buttons from the start. With a game from the XMB:

1. Plug the PSP into the board and start a game from the XMB.
2. About 20 seconds after the game starts, recording begins (the first
   seconds are usually the logos). From then on:

| Press | What happens | You see, bottom right |
|---|---|---|
| **♪ + R** | the last 30 seconds are saved | a green dot, for a second |
| **♪ + L** | a recording starts | a red dot, while it records |
| **♪ + L** again | the recording stops and is saved | the red dot goes |

Press ♪ first and R or L with it, briefly: the game does not see the R or L.
♪ on its own still mutes and unmutes the sound, as it always has.

The videos go to **`Videos/PSPExp`** in the board user's home folder (on a
Raspberry Pi: `/home/pi/Videos/PSPExp`), named by the date and time:
`clip-2026-10-24-16.26.43.mp4`, `recording-...mp4`. They are MP4 files, up
to 30 pictures a second in full colour, with the game's sound.

**Watching them on the PSP:** start PSPExp, open **Files**, go to
`Videos/PSPExp` in your home folder and choose a video: it plays full screen
in **Media**, like any film.

## Good to know

- Games from the XMB recorded so far: Castlevania: The Dracula X Chronicles,
  Crisis Core, Jeanne d'Arc, Gran Turismo, Ace Combat X, Burnout Legends,
  Bomberman, Lunar: Silver Star Harmony, Phantasy Star Portable 2.
- Games that run at 60 pictures a second are recorded at 30.
- Recording costs the PSP a little: in the heaviest scenes a game can lose a
  few pictures a second (Burnout Legends' races: 25 instead of 30).
- In PSPExp the recorder keeps the last 30 seconds all the time, so the
  board's processor encodes video while you play.
- The dot is only on the PSP's screen, never in the videos. It may flicker a
  little.
- No dot? Check that the PSP is plugged into the board and that the board's
  PSPExp is up to date (Apps > Packages); for a game from the XMB, that the
  line is in `game.txt` with a `1`; in PSPExp, that PSPExp on the PSP is up
  to date (Settings > Update the PSP).

## Without a board

A Mac or a Linux computer can be the recorder instead: the program is
`pspexp-capture`, built from PSPExp's source
([Developing](../Developing.md)). `pspexp-capture -k -s ~/Movies/PSP`
keeps the ♪ clips in that folder; `pspexp-capture -o game.mp4` records a
whole game into one file.
