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
- PSPExp on the PSP and the board. The plugin comes with both: the board's
  **Settings > Update the PSP** puts it on the memory stick
  (`PSP/GAME/PSPExp/plugin/pspexp_plugin.prx`), and the PSPExp download for
  the PSP has it in the same place.
- The PSP plugged into the board with the USB cable, as for PSPExp.

## Switching it on, once

In PSPExp: **Settings > Recording > PSPExp plugin**, and press cross. It
shows **On** on the right. Cross again switches it off. If the plugin is not
on the memory stick yet ("Not on the PSP"), the same cross puts it there
first.

Without a board, by hand:

1. Put the PSP in USB mode (Settings > USB Connection on the XMB).
2. On the memory stick, open `seplugins/game.txt` (make the folder and the
   file if they are not there) and add this line:
   `ms0:/PSP/GAME/PSPExp/plugin/pspexp_plugin.prx 1`
3. Leave USB mode.

The `1` at the end switches it on; a `0` switches it off (that is all the
setting changes). PRO's recovery menu (from its VSH menu: SELECT on the XMB)
lists it under Plugins too.

## If something does not work

- **No dot when you press ♪ + R or ♪ + L in a game from the XMB**: check that
  Settings > Recording > PSPExp plugin says On, the cable is in, and the
  board's PSPExp is up to date (Apps > Packages).
- **The red dot goes out by itself** while you record: the board stopped
  hearing the PSP. What was recorded until then is saved. After 5 seconds the
  plugin connects to the board again by itself; press ♪ + L to record again.
- **"Switch on the PSPExp plugin first"** when you start a game from the PSP
  shelf: Settings > Recording > PSPExp plugin is Off.
- **A game from the shelf stays black**: its disc comes from the board, so the
  board must be on and plugged in. If it is not there when the game starts,
  the PSP waits a minute and a half, then goes back to PSPExp; if the cable
  comes out while you play, the game waits until it is back.
