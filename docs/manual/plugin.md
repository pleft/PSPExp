# The PSPExp plugin

**One small plugin for the PSP's custom firmware, switched on once.** It is
what PSPExp uses when the PSP runs one of its own games (a UMD, an ISO, or a
game from PSPExp's PSP shelf) instead of PSPExp:

- **Recording**: it sends the game's picture and sound down the USB cable, so
  **♪ + L** and **♪ + R** record it on the board
  ([Recording your PSP games](recording.md)).
- **PSP games kept on the board**: a game started from PSPExp's PSP shelf
  (Emulators > PSP) stays on the board, and the plugin reads its disc over the
  cable while you play. Without the plugin those games cannot start: PSPExp
  says "Switch on the PSPExp plugin first".

Inside PSPExp itself it does nothing (PSPExp records its own games by
itself), so it can stay on all the time.

## What you need

- A PSP with **PRO custom firmware** (tested on a PSP-1000 with 6.60 PRO-C;
  ARK has not been tried yet).
- The **PSPExp download for the PSP**: the plugin comes with it, as
  `PSP/GAME/PSPExp/plugin/pspexp_plugin.prx` on the memory stick.
- The PSP plugged into the board with the USB cable, as for PSPExp.

## Switching it on, once

1. Put the PSP in USB mode (Settings > USB Connection on the XMB).
2. On the memory stick, open `seplugins/game.txt` (make the folder and the
   file if they are not there) and add this line:
   `ms0:/PSP/GAME/PSPExp/plugin/pspexp_plugin.prx 1`
3. Leave USB mode.

The `1` at the end switches it on; a `0` switches it off. PRO's recovery menu
(from its VSH menu: SELECT on the XMB) lists it under Plugins too.

## If something does not work

- **No dot when you press ♪ + R or ♪ + L in a game from the XMB**: check the
  line in `game.txt` ends with `1`, the cable is in, and the board's PSPExp is
  up to date (Apps > Packages).
- **"Switch on the PSPExp plugin first"** when you start a game from the PSP
  shelf: the line is missing from `game.txt`, or ends with `0`.
- **A game from the shelf stays black**: its disc comes from the board, so the
  board must be on and plugged in. If it is not there when the game starts,
  the PSP waits a minute and a half, then goes back to PSPExp; if the cable
  comes out while you play, the game waits until it is back.
