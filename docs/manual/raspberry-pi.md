# PSPExp on a Raspberry Pi

The Pi is the roomiest home for PSPExp: every package runs on it, the 3D
games included, and it has Bluetooth for headphones. PSPExp is tested on a
**Raspberry Pi 400**; a Pi 4 or 5 with the same system should behave the
same. The Pi needs no screen of its own: the PSP is its screen.

## What you need

- A Raspberry Pi 3, 4, 400 or 5 (64-bit) with its power supply. PSPExp is
  tested on the Pi 400.
- A microSD card of 8 GB or more.
- The PSP, [prepared](psp.md), and its mini-USB data cable.

## The PSPExp card image

The easy way: a card with everything on it, ready at the first boot.

1. Get the PSPExp card image for the Raspberry Pi.
   - The download: `pspexp-pi-<version>.img.xz` on the
     [download page](https://pspexp.dev/download), with its SHA-256 checksum.
   - From this repository: `scripts/pi/image/build-image.sh` builds it on a
     Mac (it needs Docker or OrbStack), into `build/pi-image/pspexp-pi.img`.
2. Write it to the card with Raspberry Pi Imager (**Use custom**) or your
   flashing tool, and put the card in the Pi. Imager's own settings (Wi-Fi,
   SSH, the time zone) work too if you want them; if you set a user there,
   keep the name **pi**.
3. Plug the PSP into any of the Pi's USB ports, start PSPExp on the PSP,
   and switch the Pi on.
4. **The first boot takes a minute or two**: the system grows to fill your
   card. Then the launcher appears on the PSP.
5. On the PSP: **Settings > Network > Wi-Fi network**, pick your network and
   type its password on the on-screen keyboard (or plug in an Ethernet
   cable). Then **Settings > Clock > Time zone**.

Everything else happens on the PSP: **Apps > Packages** to install games.
The card is Raspberry Pi OS Lite (64-bit) with PSPExp added; its user is
**pi**, password **pspexp**: change it (`passwd` in **Apps > Terminal**).
SSH is off unless you turned it on in Imager.

## On a Raspberry Pi OS you already have

For a Pi that already runs **Raspberry Pi OS (64-bit), the Debian 13
"trixie" release** (Lite is enough), with a network connection and SSH on:

1. Write the card with Raspberry Pi Imager (64-bit Raspberry Pi OS, with
   SSH, Wi-Fi and your user set in its settings), put it in the Pi and
   power it on. Wait for it to appear on your network.
2. Install PSPExp from this repository, on a Mac or Linux computer:

   ```sh
   HOST=<user>@<pi-address> scripts/pi/install.sh
   ```

   It sets the Pi up for PSPExp (the PSP's USB rule, the console as a
   service that starts at boot, the settings it needs) and installs
   PSPExp from PSPExp's package repository, `https://pkg.pspexp.dev`. The
   games come from there too, through **Apps > Packages**. (To use a
   repository of your own instead, put its address in `~/.pspexp/repo-url`
   on the computer: [Developing](../Developing.md#your-own-package-repository).)
   A one-command install on the Pi itself is planned.
3. Plug the PSP into any of the Pi's USB ports and start PSPExp on it. The
   launcher appears.

From then on the Pi starts the console at every boot and waits for the PSP;
you never need to log in to it again.

## Your first games

On the PSP: **Apps > Packages**. Pick a game (Freedoom, OpenTyrian, Cave
Story, Dink Smallwood, Alter A.I.L.A. Genesis, Theia, SuperTux, Quake, Quake II...) and press cross to install it; it appears under
**Games** when done. Emulators install the same way (NES, SNES, Mega Drive,
Game Boy, Game Boy Advance, Lynx, WonderSwan, and on a Pi the Nintendo 64)
and appear as their own tiles once you have ROMs for them.

**ROMs** go in folders in your home folder on the Pi: `roms/nes`,
`roms/snes`, `roms/md`, `roms/gb` and `roms/gbc`, `roms/gba`, `roms/lynx`,
`roms/ws`, `roms/n64`. Copy them with the file manager (**Apps > Files**) from a USB
stick or from the PSP's memory stick, or over the network as you would to
any Linux machine. Only use ROMs of games you own.

**Warcraft: Orcs & Humans** needs the data of your own copy (the DOS CD,
or the version GOG sells): its file `DATA.WAR`. Install Warcraft from
Packages, then copy `DATA.WAR` into the **Warcraft** folder of your home
folder on the Pi (Apps > Files; the folder appears after the first try,
and the launcher reminds you if the file is missing). The first start
prepares the game from it, a few seconds longer than usual; after that
it starts straight away, and its tile shows the game's title screen. The nub moves the pointer, cross selects and gives orders,
circle is the right mouse button, the d-pad scrolls the map, square adds to
the selection, START is the game menu.

**Heroes of Might and Magic II** plays its free demo (one scenario) out of
the box. To play your own copy (the full game, The Price of Loyalty or the
Gold edition), copy its **DATA** and **MAPS** folders, as an installed copy
or GOG's game folder has them, into the **Heroes2** folder of your home
folder on the Pi (Apps > Files; the folder appears after the first start).
The whole game folder works too. From the next start your copy is played
instead of the demo.

**Warcraft II** works the same way, with your own copy in the
**Warcraft2** folder: a disc image of the CD (a `.cue` with its `.img` or
`.bin`, as DOSBox copies have; the expansion's, Beyond the Dark Portal,
gives the whole game), the files of the DOS CD, the folder of the
Battle.net Edition, or the installer GOG sells (the `.exe` file itself).
From a disc image the music is the CD's own recordings; otherwise the
game's MIDI version. The first start prepares it all and takes a few
minutes (about 4 on a Pi 400 from a disc image); after that it starts
straight away, and its tile shows the game's title screen. Everything is drawn at the PSP's own size, pixel for
pixel, so the text is sharp, and laid out for its screen: the menus, and
in a game the minimap, the selected unit and its command buttons down a
narrow column on the left (the buttons' pictures trimmed a little at
their edges to fit), the map on the right, your gold, lumber, oil and food along the
top, the Menu button top right. **Standard Game** first asks which map
list to play: Skirmish Modern is Warcraft II's usual one; Skirmish
Classic, Front Lines and For the Motherland are Wargus's extra sets, each
with its own maps and settings. You see less of the map at once than on a
PC; scroll with the d-pad, or click the minimap. The Map Editor is left
out on the PSP: it is made for a large screen and a keyboard.

**Heroes of Might and Magic III** needs your own copy too. It plays on
VCMI, which needs **Shadow of Death** or the **Complete** edition (GOG sells
the Complete one); Restoration of Erathia alone, the first release, does not
run on it. Put one of these in the **Heroes3** folder of your home folder
(Apps > Files; the folder appears after the first start):

- **GOG's installer**: both files, the `setup_heroes_of_might_and_magic_3_complete_….exe`
  and the `….bin` beside it.
- **An installed game's folder**, the one holding the **Data**, **Maps** and
  **MP3** folders, copied from a computer where Heroes III is installed
  (from GOG, or from the Shadow of Death or Complete CD).

The CDs or their images are not read by themselves: install the game from
the CD on a computer first and copy that folder. The first start copies the
game into place: about 3 minutes from GOG's installer, a little over 1 from
a game folder; the PSP waits meanwhile. If the copy is Restoration of
Erathia, or nothing is there, the PSP says so. After that you may delete
what you put in Heroes3. It plays on VCMI, the open-source
engine for Heroes III. The game is laid out for a bigger screen and shown
shrunk, so its text is small: hold **R** to see the part around the pointer
at full size.

## What it plays like

Measured on the PSP-1000 with a Pi 400:

| | |
|---|---|
| Freedoom, OpenTyrian | 35 fps, the games' own full speed |
| Mr. Boom, the 2D emulators | 60 fps |
| Quake (shareware) | 45–60 fps, about 30 in the biggest fights |
| OpenArena, DreamChess, Emilia Pinball, SuperTux | about 30 fps |
| Media: music | MP3 decoded by the PSP; the Pi does almost nothing |
| Media: films | 720p and 1080p H.264 at 29–30 pictures a second on the PSP-1000, the film's clock exact (Pi 400; ffmpeg uses about half a core) |
| SuperTuxKart | drawn by the Pi's own graphics chip and sent to the PSP as pictures (only on a Pi); about 29 fps at most from the Pi; its menus 33-60 fps on the PSP-1000 |
| Nintendo 64 (Super Mario 64, Ocarina of Time, Banjo-Tooie, GoldenEye 007, Perfect Dark) | drawn by the Pi's graphics chip (only on a Pi); Super Mario 64 plays smoothly, Ocarina of Time well; GoldenEye and Perfect Dark have short sound gaps where the Pi falls behind; Banjo-Tooie is too slow in play on a Pi 400 (its intro runs well, its levels do not) |
| Unreal Tournament | drawn by the Pi's graphics chip (only on a Pi): 30-40 fps in a deathmatch on the PSP-1000; free with Epic's permission, its terms accepted in Packages |
| Celeste 64 | drawn by the Pi's graphics chip (only on a Pi): 54-60 fps in the level on the PSP-1000, its music and sound on the PSP |
| Wolfenstein: Enemy Territory | drawn by the PSP: on the PSP-1000 about 20 fps over Oasis's open ground, up to 60 indoors; online or against bots; id's licence agreement accepted in Packages |
| Diablo (shareware) | drawn by the PSP: 60 fps on the PSP-1000; your own full game too (DIABDAT.MPQ in ~/.local/share/diasurgical/devilution); DevilutionX's licence accepted in Packages |
| Sonic Robo Blast 2 | drawn by the PSP: 60 fps on the PSP-1000, running through Greenflower Zone |
| SRB2Kart | drawn by the PSP: 56-57 fps on the PSP-1000; races online |
| Red Alert, Tiberian Dawn on OpenRA | drawn by the Pi's graphics chip (only on a Pi): a skirmish 16-17 fps on the PSP-1000, menus 13-20; the first start installs the game's content (Quick Install) |
| Command & Conquer, Red Alert | drawn by the PSP: 25-28 fps in the emulator; EA's freeware, about 600 MB each to download |
| Lugaru HD | drawn by the PSP: about 58 fps in its menus on the PSP-1000 |
| Quake II (demo) | drawn by the PSP, like Quake: 31-60 fps in its first seconds on the PSP-1000 |
| Luanti (Minetest) | drawn by the Pi's graphics chip (only on a Pi): its menus about 58 fps on the PSP-1000 |
| Trackballs, Open Surge | drawn by the Pi's graphics chip too (only on a Pi); 51-60 fps in their first seconds on the PSP-1000 |
| LBreakout HD | breakout at the PSP's own size, 55 fps; the d-pad or nub move the paddle, cross fires, circle brings the ball back, START is the menu |
| Mirror Magic | laser puzzles; the nub is the pointer, cross / circle turn a mirror, R is the magnifier |
| Pushover | domino puzzles at the game's own 18 frames a second; the d-pad or nub walk and climb, cross carries or pushes a domino; the tutorials' help panel is small |
| C-Dogs | top-down run-and-gun at the PSP's own size, more of each map shown: 58-60 fps in a mission on the PSP-1000; the nub moves, cross fires and confirms, SELECT backs out |
| LinCity-NG | the city builder, its window shrunk to the PSP's screen (small text; R magnifies round the pointer); the nub is the mouse, cross clicks, circle the right button, the d-pad scrolls the map; the game moves at its own 10-13 frames a second |
| Taisei | the bullet-hell shooter, drawn by the Pi's graphics chip (a Pi 4, 400 or 5): 60 fps in its first stage on the PSP-1000, with fewer effects; cross shoots, square focuses, triangle bombs, circle skips dialogue, SELECT pauses |
| Kobo Deluxe | the maze shooter at the PSP's own size, its playfield widened: 30 fps; the d-pad or nub move, cross fires, circle confirms, START is its menu |
| Powermanga | the arcade shooter, shown at two thirds: 60 fps; the d-pad or nub move, cross fires, square the special weapon, START pauses |
| TecnoballZ | breakout with a shop and gadgets, in its small mode so it shows larger: about 51 fps; the nub moves the paddle and the menu's cursor, cross clicks and fires, triangle releases the ball, START pauses |
| Frozen Bubble | the bubble-popping puzzle: about 49 fps; it takes about 20 s to load; the d-pad aims, cross fires, START confirms in its menus |
| rRootage, Parsec47, Tumiki Fighters | Kenta Cho's shooters, drawn by the PSP in OpenGL: 60 fps; the d-pad or nub move, cross fires, circle the second button, START or SELECT pauses |
| GLtron | light cycles in 3D, drawn by the PSP: 56-60 fps; the d-pad turns, L and R glance, cross boosts, START pauses |
| Neverputt | minigolf on Neverball's engine at the PSP's size: 54-60 fps; the nub aims, cross putts |
| Noiz2sa | Kenta Cho's abstract shooter: 60 fps; the d-pad or nub move, cross fires |
| GNU Robbo | the Atari puzzle game, its text small: 50 fps; the d-pad moves, square with the d-pad shoots, SELECT restarts the level |
| Freeciv | Civilization-style strategy; small text, R magnifies; it loads for about half a minute and its menu shows once you move the pointer; the nub is the mouse, cross clicks, the d-pad moves a unit, square goes to the next one, triangle founds a city, START ends the turn |
| Numpty Physics | drawn by the PSP, at its own screen size; the nub is the pen (cross held draws), circle undoes a stroke, triangle restarts, L / R the previous / next level |
| Heroes of Might and Magic III | on VCMI, your own copy (above): 55-60 fps on the PSP-1000; the nub is the mouse, cross clicks, circle shows what a thing is, the d-pad scrolls the map, R the magnifier, START Escape |
| Heroes of Might and Magic II | the 1996 strategy game on fheroes2: its demo (one scenario), downloaded when installed, or your own copy (above); the nub is the mouse, cross clicks, circle held shows what a creature or building is; its screen is larger than the PSP's, so **R is a magnifier**: the game's own pixels around the pointer, for its small text, R again the whole screen; L the next hero, triangle ends the turn, square moves the hero, the d-pad scrolls, START cancels, SELECT is OK |
| Warcraft: Orcs & Humans | drawn by the PSP: 60 fps from the Pi in the menus and in a game; your own copy's DATA.WAR (above) |
| Warcraft II | drawn by the PSP: 54-60 fps from the Pi; your own copy (above); menus and game at the PSP's 480x272 |

Every game here was started on the PSP-1000 from the launcher on 6
October 2026, its sound heard, its nub and START + SELECT tried
(`tests/psp/all-games.sh`), except the Warcraft games, added on 8 October
while the PSP was away: Warcraft tried from the launcher with a stand-in for
the PSP, not yet on the PSP itself.

## Games drawn by the Pi's graphics chip

Most games are drawn by the PSP itself: the Pi only tells it what to draw.
Some 3D games ask for more than the PSP's graphics chip can do
(Celeste 64, SuperTuxKart, Trackballs, Open Surge, Luanti). On a Pi these are drawn by the Pi's
own graphics chip instead, and each finished picture is sent to the PSP,
which shows it, plays the sound and reads the buttons as for any other
game. You don't have to do anything: install them from **Apps > Packages**
as usual. Their entry there says **Board GPU** under renderer, and the
launcher calls them **Board GPU renderer**. They are offered only on a
Pi; the GameShell's graphics chip is too small for them.

## Bluetooth headphones

The Pi has Bluetooth. Put your headphones or speaker in pairing mode, then
**Settings > Sound**, wait for the scan, pick them with cross. From
then on every game's sound plays in them, the PSP's volume buttons set
their volume (a bar shows on screen), and when they go back in their case
the sound returns to the PSP by itself. Choose **The PSP** in Sound output
to let them go, for your phone or tablet. Details:
[Using PSPExp](using-pspexp.md#bluetooth-headphones).

## Good to know

- **A good power supply matters**: the PSP charges from the Pi's USB port,
  and on a weak supply (or a thin cable) the Pi slows itself down to stay
  safe. The N64 and the 3D games then stutter and their sound breaks up.
  `vcgencmd get_throttled` in the terminal says `throttled=0x0` when all is
  well; anything else means the Pi has been short of power since it started.
  Use the Pi's official supply, or charge the PSP first.
- **The Pi 400's keyboard** types into the PSP's terminal while the PSP is
  connected.
- **Music in other formats** (Ogg, FLAC, Opus, M4A) is decoded on the Pi by
  ffmpeg (on the card image already): `sudo apt install ffmpeg` if the
  player says it is missing.
- **Updates** come through **Apps > Packages** (PSPExp itself is the first
  entry under Apps); after one, the launcher offers **Settings > Restart PSPExp**.
- **Wi-Fi and the time zone** can be changed from the PSP: Settings >
  Network, Settings > Clock > Time zone.
- Packages live in `/opt/pspexp`; your settings in `~/.config/pspexp*`.
- **Getting into the Pi from a computer** (the card image; SSH is off at
  first because every card starts with the same password):
  - on the PSP, **Settings > This board > SSH** turns it on (or off); the
    launcher warns while the password is still `pspexp`, which **Apps >
    Terminal** changes: `passwd`;
  - or put the card in a computer and create an empty file named `ssh` on
    its `bootfs` drive: SSH is on from the next boot;
  - or turn SSH on in Raspberry Pi Imager's settings when writing the card.

  Then `ssh pi@raspberrypi.local`. Change the password `pspexp` first.
