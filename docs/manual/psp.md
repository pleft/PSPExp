# Preparing the PSP

You do this once. After that, the board keeps the PSP's side of PSPExp up to
date over the USB cable.

## What you need

- A **PSP-1000** (the model PSPExp is tested on). Other models may work but
  have not been tried.
- **Custom firmware** running. PSPExp loads a small driver of its own for
  the USB link, which the official firmware does not allow. Any custom
  firmware that runs homebrew with kernel access will do; if the PSP runs
  other homebrew, it is ready.
- A **mini-USB data cable**: the kind that carries data, not one made only
  for charging.
- A memory stick with a few megabytes free.

## Installing PSPExp on the PSP

1. On the PSP: **Settings > USB Connection**, and connect the PSP to a
   computer. Its memory stick appears as a drive.
2. Copy the `PSPExp` folder into `PSP/GAME/` on the stick, so it ends up as
   `PSP/GAME/PSPExp/EBOOT.PBP` (and the files beside it).
   - The download: `PSPExp-psp-<version>.zip` on PSPExp's
     [Releases](https://github.com/pleft/PSPExp/releases) page: unzip it at the stick's root (it holds
     `PSP/GAME/PSPExp`).
   - From this repository: `scripts/build.sh`, then
     `scripts/packages/runtime-slot --install /Volumes/<stick>/PSP/GAME/PSPExp`
     (macOS; on Linux, wherever the stick is mounted).
3. Leave USB mode. **PSPExp** is now under **Game > Memory Stick**.

## Starting it

Start **PSPExp** from the Game menu. It shows a status page (its first
line: PSPExp's version on the PSP, and its slot) while it waits
for a board; plug the USB cable into the board (the board must be set up
first, see its guide) and the launcher appears within a few seconds.

You can start PSPExp before or after plugging the cable in, and unplug and
plug it again at any time: the launcher comes back by itself.

To leave PSPExp, press **HOME** and quit as from any game, or hold **HOME**
for 2 seconds.

## Things the PSP does by itself

- **Updates.** When the board has a newer PSPExp for the PSP, the launcher's
  System page shows **Update the PSP**. It installs the new version beside
  the old one and restarts into it; if the new one ever fails to start, the
  PSP goes back to the previous one by itself.
- **Battery.** The launcher's top bar shows the PSP's battery (and when it
  charges) next to the board's own indicators.
- **Saving power.** While nothing happens on screen the PSP turns its
  backlight off; any button turns it back on.
- **The clock.** Settings > Clock > PSP clock lets the board set the PSP's clock,
  handy when its clock battery has run flat.
- **Volume.** The PSP's volume buttons work as always; with Bluetooth
  headphones on the board, they set the headphones' volume instead, and a
  bar on screen shows it.

## The memory stick from the board

While PSPExp runs, the board can reach the PSP's memory stick: the file
manager lists it as a drive (**PSP**), so you can copy saves, music or
pictures in either direction without USB mode. See
[Using PSPExp](using-pspexp.md#files).
