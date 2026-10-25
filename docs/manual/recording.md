# Recording your PSP games

**Your PSP games, recorded with their sound: your own UMD and ISO games, and
everything you play in PSPExp.** Press **♪ + L** to start a recording and
again to stop it; with **Clips** turned on in Settings, **♪ + R**
saves the last 30 seconds as a video. A small dot in the corner of the screen
says it heard you. The videos are kept on the board, ready to watch or share.

In PSPExp (its games, emulators, apps and the launcher) it works by itself.
For a game the PSP runs itself (started from the XMB or from PSPExp's PSP
shelf), [the PSPExp plugin](plugin.md) sends the game's picture and sound down
the USB cable, and the board makes the videos; the game runs on the PSP as
always.

## What you need

- A **board with PSPExp** (a Raspberry Pi, tested on a Pi 400; a GameShell
  too, with less smooth videos: [its page](gameshell.md)), plugged into
  the PSP with the same USB cable. The board's PSPExp starts the recorder by
  itself; it needs ffmpeg, which the card images have.
- For what you play in PSPExp: nothing more, once PSPExp on the PSP is up to
  date (Settings > Update the PSP).
- For games the PSP runs itself: [the PSPExp plugin](plugin.md), switched on
  once in **Settings > Recording > PSPExp plugin** (PRO custom firmware;
  Update the PSP puts the plugin on the memory stick).

## In PSPExp

Play as usual: ♪ + R and ♪ + L work everywhere in PSPExp, in the board's
games and emulators as in the launcher, with the same dot. **♪ on its own
still pauses and resumes the music**, when you let go of it.

## The last 30 seconds

To save what just happened, the board has to be recording all the time: the
PSP sends its picture and sound down the cable and the board encodes them,
keeping the last 30 seconds. That costs a little (a game can run a little
slower, the board works harder), so it is off at first. Turn it on in
**Settings > Recording > Clips** (cross switches it on or off; it shows
On or Off on the right); then ♪ + R saves the last 30
seconds. With it off, nothing is sent or recorded until you press ♪ + L.

## Recording

In PSPExp you can press the buttons from the start. With a game the PSP runs
itself:

1. Plug the PSP into the board and start a game from the XMB or the PSP shelf.
2. Once the game has set itself up (a few seconds, at most 20), recording begins (the first
   seconds are usually the logos). From then on:

| Press | What happens | You see, bottom right |
|---|---|---|
| **♪ + R** | the last 30 seconds are saved (with Clips on) | a green dot, for a second |
| **♪ + L** | a recording starts | a red dot, while it records |
| **♪ + L** again | the recording stops and is saved | the red dot goes |

Press ♪ first and R or L with it, briefly: the game does not see the R or L.
♪ on its own still mutes and unmutes the sound, as it always has.

The videos go to **`Videos/PSPExp`** in the board user's home folder (on a
Raspberry Pi: `/home/pi/Videos/PSPExp`), named by the date and time:
`clip-2026-10-24-16.26.43.mp4`, `recording-...mp4`. They are MP4 files, up
to 30 pictures a second in full colour, with the game's sound.

**Watching them on the PSP:** in PSPExp, **Settings > Recording > Videos**
opens them in **Files** (`Videos/PSPExp` in your home folder); choose one and
it plays full screen in **Media**, like any film.

## Good to know

- Games from the XMB recorded so far: Castlevania: The Dracula X Chronicles,
  Crisis Core, Jeanne d'Arc, Gran Turismo, Ace Combat X, Burnout Legends,
  Bomberman, Lunar: Silver Star Harmony, Phantasy Star Portable 2.
- Games that run at 60 pictures a second are recorded at 30.
- Recording costs the PSP a little: in the heaviest scenes a game can lose a
  few pictures a second. When a game starts to slow down, the PSP sends the
  video fewer pictures (down to 5 a second) until the game is back to speed,
  so the game keeps its pace and the video is less smooth for those seconds
  (Gran Turismo's heaviest stretch: about 51 instead of 20). To tell, the
  PSP now and then leaves the video one second without new pictures, often
  when the game changes scene. That is while a recording runs, or all the
  time with Clips on.
- The dot is only on the PSP's screen, never in the videos. It may flicker a
  little.
- No dot? Check that the PSP is plugged into the board and that the board's
  PSPExp is up to date (Apps > Packages); for a game the PSP runs itself, that
  [the plugin](plugin.md) is on; in PSPExp, that PSPExp on the PSP is up
  to date (Settings > Update the PSP).

## Without a board

A Mac or a Linux computer can be the recorder instead: the program is
`pspexp-plugin-host`, built from PSPExp's source
([Developing](../Developing.md)). `pspexp-plugin-host -k -s ~/Movies/PSP`
keeps the ♪ clips in that folder (with `-b 0`, only ♪ + L recordings, as with
Clips off); `pspexp-plugin-host -o game.mp4` records a
whole game into one file.
