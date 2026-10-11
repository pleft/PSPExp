# Launcher_and_Boards.md: the superseded sections

Moved here word for word on 2026-10-11 from docs/Launcher_and_Boards.md, where
short notes now point to them: how things were before the recipes and apt
(docs/Package_Manager.md) and the minimal card image replaced them. Kept for
the record; the Omega2+'s builds are still made the §4.2 way.

### 4.2 Packages

> **Superseded (2026-09-29):** games and emulators are now the recipes in
> `recipes/`, installed through apt on the Debian boards and from our own
> builds on the Omega2+ (docs/Package_Manager.md). What follows describes the
> archives as they were; only the Omega2+'s builds are still made this way.

`scripts/packages/pspexp-pkg-build build ID` runs the recipe `scripts/packages/recipes/ID.sh`,
which fills the package folder (downloads cached in build/gameshell/pkg-cache,
helpers in `_lib.sh`) -> `build/gameshell/pkg/ID.tar.gz`. `pspexp-pkg-build install
ID` copies it over and runs `pspexp-pkg install` (unpacks next to the apps
folder, checks `requires`, replaces the old version in one rename, copies its
systems). `pspexp-pkg-build remove`, `pspexp-pkg-build list`, and `pspexp-pkg` on the GameShell
for a tarball from anywhere. Only titles the PSP draws (through pspexp-run)
are packaged; data is only what may be redistributed.

| id | contents | notes |
|---|---|---|
| freedoom | Freedoom 0.13 Phase 1 + 2 (two entries) | the GameShell's chocolate-doom package, Doom key map |
| opentyrian | OpenTyrian (SDL2, compiled on the GameShell) + Tyrian 2.1 freeware data | run.sh writes its config on first start |
| cavestory | NXEngine-evo + the freeware Cave Story (2004), its data made with NXEngine-evo's nxextract on the Mac | 480x272 by default (the PSP's screen, no scaling), 50 fps, drawn as PSP sprites (~12 KB a frame) |
| simutrans | Simutrans 124.5 (cross-compiled from its tag) + its base data from the 124.5 Linux release + pak64 124.4, all Artistic License | 480x272 natively, 25 fps (crowded; 640x362 shrunk was tried: text unreadable with the PSP's nearest-pixel scaling); the nub is the mouse, the SDL layer draws the pointer; GameShell and Pi only (memory) |
| jazz | OpenJazz + the Jazz Jackrabbit shareware episode (Epic MegaGames, 1994), all 78 files of the archive unaltered, as its license asks | 480x272 by default (a wider view of the level); called shareware, Epic credited, in the entry and package |

Cave Story and Jazz are cross-compiled on the Mac for every board: the
GameShell and Pi through `scripts/packages/cross-glibc.sh` (linked against a
stand-in libSDL2 naming only the functions used, so the board's own SDL2
answers, with the SDL layer preloaded), the Omega2+ static with the SDL layer
built in. About a minute each, where the Pi 400 took half an hour to compile
Cave Story itself.

Every game package puts a 128x128 picture next to its entry
(`scripts/packages/art/`, where the README lists each one's source and
license), which the launcher shows as the cover.

OpenGL games from Debian are recipes too (2026-09-30): SuperTux,
Chromium B.S.U., openMrac and Neverball, drawn by the PSP's GE through the
SDL layer's OpenGL (`docs/PSPExp_Technical_Architecture.md` §12a). Their
recipes carry the options found by playing them on the PSP: SuperTux at the
PSP's size and on keys only (`env=PSPEXP_JOYSTICK=0`), Chromium B.S.U.'s
ship on the nub as a mouse, openMrac without its settings window. Started
from the launcher on the Pi 400: SuperTux and openMrac 59.9 fps, Chromium
B.S.U. 43, Neverball 30.

Quake's shareware episode (recipe `quake`, 2026-09-30) runs on Debian's
Quakespasm. Its data is our package `quake-shareware-data`
(`scripts/packages/debs/quake-shareware-data`): the licence lets an end user
pass the shareware on free of charge only as a whole, so id's `quake106.zip`
goes in unaltered, and `/usr/games/quake-shareware` unpacks `id1/pak0.pak`
from it on the first start (lhasa, into `~/.local/share/quake-shareware`)
before running Quakespasm there; it also puts `r_oldwater 1` in that
folder's `id1/autoexec.cfg` (package 1.06-pspexp2, 2026-10-01: water and
slime warped by Quake itself, as its default needs framebuffer copies the PSP's
OpenGL cannot make). The recipe asks for three texture units
(`env=PSPEXP_GL_UNITS=3`), which puts Quakespasm on its shader path. Keys only,
on Quake's own bindings: the nub and d-pad move and turn, cross fires and
selects, circle jumps, square changes weapon, L / R strafe, start is the
menu, select the scores. Started from the launcher on the Pi 400: 48-59 fps
in its demos.

OpenArena (recipe `openarena`, 2026-10-01): Debian's, data included, on the
Quake 3 engine's OpenGL 1.x renderer at the PSP's size with the engine's
low-end settings (all on its command line in the recipe). Controls as the
PSP's own shooters have them, since the nub and the d-pad are both under the
left thumb (the first layout, d-pad moving and nub aiming, could not do both
at once; aiming was slow at the desktop's sensitivity): the nub aims (as a
mouse, sensitivity 25), triangle / cross move forward and back, square /
circle strafe, R fires and clicks in the menus (`PSPEXP_MOUSE_BUTTONS`), L
jumps, d-pad up / down change weapon, left crouches, start is the menu, select
the scores. Played on the PSP-1000 from the Pi 400 (2026-10-01): menus and
movement fine once the layer kept SDL's relative mouse mode.

Four more from Debian (recipes, 2026-10-01; seen drawing right in PPSSPP and,
Teeworlds, on the PSP; not yet played): X-Moto (`xmoto`: the d-pad drives,
circle turns round, cross selects), Blobby Volley 2 (`blobby`: menus by the
nub as a pointer with circle clicking, the d-pad moves the left blob, cross
jumps), Trigger Rally (`trigger-rally`: menus by the nub, R clicks; the d-pad
or cross / square drive, circle the handbrake) and Teeworlds (`teeworlds`:
online servers or a LAN, it has no bots; the nub aims, R fires, L hooks, the
d-pad runs, cross jumps, square / triangle change weapon).

Mr. Boom (`mrboom`, recipe 3, 2026-10-02): its first keyboard player, by its
manual's keys: the d-pad or nub moves, cross lays a bomb, circle ignites,
square jumps, triangle adds a bot, START starts a game, SELECT quits, L / R
pause. Recipe 2 mapped only START and SELECT (an entry's `keys=` replaces
the whole default map) and left the game controller on, which Mr. Boom
takes as one more player: in a game no button reached the player.
Checked on the PSP with scripted keys (moved, laid a bomb), then played by
hand from the Pi: 40-54 fps, 160-230 KB a frame. (A scripted run that saved
a PSP screenshot every second measured 15-20: the screenshots, not the game.)
The bytes were Mr. Boom's xBRZ smoothing, on by default on 64-bit ARM (the
Pi; not on the GameShell's 32-bit ARM): each frame its 320x200 picture
upscaled to 640x400, thousands of colours, sent as 16-bit pages and shrunk
again on the PSP. Recipe 4 starts it with `-x 1`: its own picture, 256
colours, 8-bit pages; 39-44 KB a frame on the Pi with the null executor
(was 150-170), texture work 0.65 ms a frame (was 1.6). Played by hand on the
PSP-1000 from the Pi: a steady 59.9 fps, 39-57 KB a frame, no audio
underruns ("smooth and quick").

Two more from Debian (recipes, 2026-10-02; installed on the Pi, seen in
PPSSPP, not yet played on the PSP): OpenTTD (`openttd`, with OpenGFX,
OpenSFX and OpenMSX; at 480x272 its windows crowd the screen: the nub is the
pointer, cross / circle click, the d-pad scrolls, L / R zoom out / in,
triangle closes all windows, square closes news and errors, START is Escape;
its first run's survey question answered by `first-run=`) and Flare:
Empyrean Campaign (`flare`: its own gamepad controls, so `keys=` maps
nothing; `first-run=` turns the gamepad on, the mouse off and skips its
first question, the input method). Flare draws its menu text into render
targets from textures it then destroys; the SDL layer now bakes such a
target on the board first (see the SDL layer's README), without
which the menus had no text.
On the PSP from the Pi (played, 2026-10-02): OpenTTD plays well; its pointer
was a bit fast, so recipe 3 sets `PSPEXP_NUB_SPEED=180` (the layer's default
300). Flare is unplayable: its smallest screen is 640x480, shrunk to 57% its
text cannot be read, and no button worked: it counts gamepads once at start,
before the layer had attached its controller (attached now when a game first
counts joysticks: Flare finds "PSP (PSPExp)"). Its recipe was removed for
the text.


Two more (recipes, 2026-10-03; installed on the Pi and checked in PPSSPP from
it, not yet on the PSP):

- **Dink Smallwood** (`freedink`, Debian's FreeDink and its free data): run
  with `-S`, SDL's software renderer: its OpenGL one goes through shaders
  the PSP cannot draw (a white screen). The title's pointer moves with the
  d-pad and cross clicks (Ctrl, Dink's attack key); in the game cross
  attacks, circle talks, square casts, triangle is the inventory, START the
  menu, SELECT the map; `-j` and no controller, so a press never counts
  twice. 640x480 drawn on the PSP's screen, 28 fps in PPSSPP, the status
  bar's text small but readable. Its five Ogg music tracks stay silent
  (Debian's build has no Ogg music); the twelve MIDI ones play through FreePats.
- **Alter A.I.L.A. Genesis** (`alter-aila-genesis`): Neok's 2010 RPG Maker
  2003 game, freeware, on **EasyRPG Player**, neither in Debian. Two PSPExp
  packages: `pspexp-easyrpg` (Player 0.8.1.1 with liblcf 0.8.1 built in,
  compiled in a Debian 13 container of the board's architecture by
  `scripts/packages/build-easyrpg.sh`, its Depends from dpkg-shlibdeps) and
  `alter-aila-genesis`, an **installer**: PSPExp does not pass the game's
  files on (its terms do not say they may be), so the package holds only a
  launch script, and its postinst fetches the developer's 24 Nov 2010
  release from the Internet Archive's copy of rpgmaker.net, checks its
  SHA-256 and unpacks it (unar: bsdtar stopped on a CRC error with this
  RAR) into `/usr/share/games/alter-aila-genesis`, every file as released;
  postrm removes it. It needs no RTP (FullPackageFlag=1). On the Pi: the
  upgrade from the first, file-carrying package took 108 s, a fresh install
  46 s, removal deletes the game; the downloaded game runs.
- **Theia: The Crimson Eclipse** (`theia`, 2026-10-03): MissingSeven's free
  RPG Maker 2003 JRPG (2019), its English Orihalcon edition V2-08, the same
  way: the installer asks the developer's itch.io page for a download link
  at each install (itch.io's links last a minute: a POST to the upload's
  `file/` address answers with one), fetches the 575 MB release, checks it
  and unpacks it (656 MB). On the Pi the install took 369 s. It uses DynRPG
  plugins (ATB battle overhaul, battle display, condition icons, a text
  plugin); EasyRPG says it may not run them properly. Title, new game and
  the opening scene with its dialogue right in PPSSPP at 24-26 fps; battles
  not reached by script. Both games' installers come from one builder now,
  `scripts/packages/debs/_installer.sh` (a game's build script names its
  download, checksum, the folder inside the archive and the launch command).

More (2026-10-03, on the Pi and in PPSSPP from it, not yet on the PSP):

- **The Ur-Quan Masters** (`uqm`, Debian contrib, with its music and voice
  packages): at its own 320x240 (`-r 320x240`), shown 4:3. UQM's own keys:
  cross Right Ctrl (fire, select), circle Right Shift (special, cancel),
  triangle / START Escape, square Alt, L / R Page Up / Down (the star map's
  zoom). Main menu and the intro's slides right; few frames while those
  slides stand still is UQM redrawing only on a change.
- **Hurrican** (`hurrican`): the HurricanGame fork's engine built by PSPExp
  (`scripts/packages/build-hurrican.sh`, `pspexp-hurrican`: SDL2, the OpenGL
  1.x renderer, from the fork's main branch at 0d91d1f, its v1.0.9.3 tag
  still having SDL 1.2-only code; libepoxy stood in for by a header of the
  plain GL prototypes, epoxy's own lookups would pass the layer by), and the
  game's data downloaded at install from that repository's archive at the
  same commit (`hurrican`, an installer; the builder now unpacks a .tar.gz
  with tar, unar only ungzipped it). Run windowed at low resolution (`-W -L`).
  It needed two layer fixes: a versionless "core" context accepted, and the
  keyboard table it reads kept current (no key reached its intro). The
  intro, menus and level 1 right at 25 fps in PPSSPP, 42 KB a frame in play.
  Its menus take the controller's cross / circle; in play the keyboard by
  its defaults (cross Alt jumps, square Ctrl shoots, circle Shift the
  lightning, triangle Space the power line, R / L Right Ctrl / Shift grenade
  and smart bomb, SELECT Return cycles weapons, START Escape).
  On the PSP-1000 (2026-10-10): play at 59-60 fps, about 100 KB a frame.
  Its menu (START) now and then answered no button: Hurrican waits there
  in a loop of `SDL_PumpEvents` and key states for Escape to be let go,
  and the layer read the PSP's input only at an event poll or a frame
  (found with gdb on the stuck game: its main thread running in
  `SDL_PumpEvents`, no frame drawn); `SDL_PumpEvents` reads it now. Open:
  twice (before 2026-10-06 and on 2026-10-10) the PSP switched itself off
  as Hurrican started, its log ending at "connected ... depth buffer", not
  seen again in eight starts since nor in PPSSPP; the runtime's
  `trace-messages` switch (docs/cheatsheet.md) is on that PSP to catch it.
- **Quake II** (`quake2`, 2026-10-04): id Software's demo on Debian's
  Yamagi Quake II 8.50 with its OpenGL 1 renderer, which the SDL layer draws
  on the PSP's GE (`graphics=opengl`, every glibc board): the demo's attract
  loop right in PPSSPP at 22-24 fps, 150-220 KB a frame (immediate-mode
  geometry). Its red frames are Quake II's damage flash (the GL1 renderer
  on the Pi's own GPU shows the same). The demo's data is not hosted: the
  installer `quake2-demo` downloads id's original release (39 MB) from the
  Yamagi project's mirror and checks it; its several top-level items made
  unar wrap them in a folder named after the download, so `_installer.sh`
  now passes `-D`. Keys only (Yamagi would read the controller too): d-pad
  and nub move and turn, cross fires and selects, circle jumps, triangle
  crouches, square the next weapon, L / R strafe, SELECT the inventory,
  START the menu; the first run writes `~/.yq2/baseq2/autoexec.cfg`
  binding the next weapon and crouch, which Quake II leaves unbound.
- **SRB2Kart** (`srb2kart`, 2026-10-04): Kart Krew's free kart racer with
  Sonic and friends, on no PSP (the Vita has a port). Built from the v1.6
  tag (`scripts/packages/build-srb2kart.sh`, `pspexp-srb2kart`); its build
  records the official assets' MD5s and checks them at start (skipping its
  assets step left them empty: "file is corrupt"), so the container
  fetches the release's `AssetsLinuxOnly.zip` for the build only; the
  installer `srb2kart` downloads the same zip at install (221 MB). Its
  software renderer crashes on a NULL wall drawer at heights that are not a
  multiple of 200 (480x272, 640x360; also without PSPExp): run at 640x400
  (`-width 640 -height 400`), where its HUD is drawn twice its size, and
  shrunk to the PSP's screen it stays readable. Drawn by the PSP as CLUT8
  pages: Green Hills in a race at 22-23 fps into PPSSPP (its cap 35). The
  PSP's controller is its gamepad (its SDL game-controller defaults: nub
  steers, cross accelerates, circle brakes, L item, R drift, square look
  back, triangle camera, START menu, SELECT pause); the d-pad's left / right
  also steer as the arrow keys. Online races through the board.
- **Wolfenstein: Enemy Territory** (`etlegacy`, 2026-10-04): the free team
  shooter of 2003 on ET: Legacy 2.86.0, online (the live server list works)
  or against Omni-bot bots (a game hosted from the menu fills with them).
  ET: Legacy's own arm64 build has SDL linked in and not exported, out of
  the layer's reach: the client is built against Debian's SDL2
  (`scripts/packages/build-etlegacy.sh`, `pspexp-etlegacy`) with
  `compatibility/etlegacy/pspexp.patch`: ioquake3's `r_vertexLight` back
  (one pass a surface, the map's vertex colours: ET: Legacy had dropped it,
  its collapse function left under `#if 0`), and `glUseProgramObjectARB(0)`
  only when GLEW has it (ET calls it at every map load even with no shader
  programs; GLEW loads it only with `GL_ARB_shader_objects`, which the
  layer does not report: a null call crashed the first map). The installer
  `etlegacy` downloads ET: Legacy's official AArch64 release (its mod, the
  bots) and id's ET 2.60 for Linux (pak0-2.pk3, 271 MB) from id's mirror,
  both pinned, as ET: Legacy's installer does, once id's licence agreement
  is accepted (`terms=`; the agreement kept in the game's `Docs/`). The
  launch script makes a profile the first time (ET: Legacy otherwise asks
  for a typed name, and the layer sends no text input) with the PSP's
  bindings, and starts with `com_ignorecrash` (its "crashed last time"
  question needs a desktop). Its vanilla renderer drawn by the PSP's GE:
  menus 24 fps at 21 KB a frame, Oasis in play about 20 fps into PPSSPP at
  90-160 KB a frame (indexed draws kept by content, below). Controls as
  OpenArena's; the nub moves ET's own menu cursor (small, from the top-left
  corner), R clicks; the arrow keys also walk its menus. Arm64 only (the
  official release has no 32-bit ARM build).
- **Red Alert and Tiberian Dawn on OpenRA** (`openra-ra`, `openra-cnc`,
  2026-10-04, Pi only): OpenRA built for arm64 (`build-openra.sh`,
  `pspexp-openra`, self-contained .NET; its `SDL2.so` the layer's wrapper),
  drawn by the board's GPU at 1024x576: skirmish about 10 fps into PPSSPP.
  The first start shows OpenRA's content installer: Quick Install (cross on
  it) fetches the game's freeware content from OpenRA's mirrors. Mouse on
  the nub, cross / circle its buttons, the d-pad scrolls, START the menu.
  The Vanilla Conquer recipes beside them (`cnc`, `redalert`, recipe 2):
  their titles and descriptions had come out garbled (Red Alert was listed
  as "Command & Conquer"), fixed. From the launcher neither started until
  2026-10-06 (recipe 3): they had only been tried with a test script that
  set SDL's offscreen video driver, the packages' entries did not, and
  OpenRA found only the dummy driver ("Unknown pixel format"). The entries
  now set `PSPEXP_HOSTGL=1`, which also makes `pspexp-run` preload the
  board-GPU layer the wrapper needs: with the PSP-drawn layer preloaded
  beside it, two layers each read the sound pipe and OpenRA was silent.
- **Diablo** (`devilutionx`, 2026-10-04): Blizzard's 1996 action RPG on
  DevilutionX 1.5.5, with the shareware data (the cathedral, the Warrior),
  or the player's own DIABDAT.MPQ (full game) in
  `~/.local/share/diasurgical/devilution`, which the launch script then
  plays instead. The project's own AArch64 build links the system's SDL2,
  so the layer takes it unbuilt; the installer downloads it and the
  shareware `spawn.mpq` from the project's assets release, pinned, once
  DevilutionX's licence (Sustainable Use: non-commercial, accepted by
  using it) is accepted (`terms=`). At 640x480 the picture was stretched to
  16:9: the launch script's first `diablo.ini` sets 854x480, shrunk to the
  PSP's screen, and a pad layout for the PSP (its defaults put the main
  action on circle and cancel on cross, and Character and Inventory on
  triggers the PSP lacks; Select combos fired the shoulder's own action
  too): cross acts, square picks up, triangle casts, circle cancels, L / R
  potions, the d-pad (walking is the nub's) inventory up, character left,
  spellbook right, quests down. A new hero is named by DevilutionX itself
  when the last input came from a pad (no keyboard keys mapped). Menus 24
  fps into PPSSPP, 0.4-87 KB a frame (CLUT8 pages). Tested unattended with
  `PSPEXP_SCRIPT`'s new `pad:` buttons. Arm64 only (no official 32-bit ARM
  build; a build for the GameShell is possible later).
- **Warcraft: Orcs & Humans** (`warcraft`, 2026-10-08): Blizzard's 1994
  strategy game on Stratagus and War1gus, through the SDL layer like any
  other game (not the v0.2 renderer backend, which needs the engine patched
  and stays in the WarCraft project). Debian has neither, so
  `scripts/packages/build-stratagus.sh` builds both in a container at their
  master's newest commits (2026-06-22: the last tags, 3.3.2, are from 2022):
  the engine is `pspexp-stratagus` (shared with Warcraft II), the game's
  scripts and war1tool `pspexp-war1gus`. Warcraft is sold, so the player brings DATA.WAR
  (DOS CD or GOG) into `~/Warcraft`; the launch script extracts it once
  with war1tool (6.5 s on the Pi 400) into `~/.local/share/pspexp-warcraft` beside War1gus's
  scripts, or says where it goes (§3.6). war1tool crashed twice with only
  DATA.WAR: copying the CD's cutscene archives (fixed by
  `compatibility/war1gus/war1tool-optional-cutscenes.patch`, from the
  WarCraft PSP port) and, with `-v` (videos, which War1gus's own starter
  passes), joining intro videos that are not there (so no `-v`). The first
  start also writes `~/.config/stratagus/wc1/preferences.lua`: 480x272 game
  pixels and square ones (War1gus's default, 480x300 stretched 1.2x to 4:3,
  was letterboxed). Its 8-bit screen goes as CLUT8 pages, only what
  changes: on the Pi 400 to the stand-in PSP 60 fps in the menus and in a
  skirmish, 4.5 KB a frame on a still map, 14-16 KB scrolling. The music
  is the game's MIDI through fluidsynth and TimGM6mb. The nub is the mouse
  (`PSPEXP_NUB_SPEED=500`: Stratagus's window is twice the game's size and
  the speed is in window pixels), cross / circle its buttons, the d-pad
  scrolls, square Shift, triangle Ctrl, START F10 (the game menu), SELECT
  Escape. Checked from the launcher: the missing-data message, extraction,
  the nub moving the pointer and cross clicking (`PSPEXP_LOG_EVENTS` now
  logs mouse buttons), START + SELECT back to the launcher. Not yet on the
  real PSP (away for repair) nor heard: the sound only reached the
  stand-in. On the Pi and, since 2026-10-09, the GameShell (below).
- **Warcraft II** (`warcraft2`, 2026-10-08): the same way on Wargus
  (`pspexp-wargus`, its newest commit, 2026-06-29), on the shared
  `pspexp-stratagus`. The player's copy goes in `~/Warcraft2` in any of the
  shapes wartool reads, which the launch script finds by wartool's own
  marker files: the DOS CD's (`rezdat.war`, with `snddat.war` beside it: a
  DOSBox install's DATA has the rest but not the speech, and wartool then
  stopped), the Battle.net Edition's (`support/tomes/tome.1`), the Mac
  CD's (`War Resources`); GOG's installer `.exe`, unpacked first with
  `innoextract` as Wargus's own starter does; and first of all a disc image
  (a `.cue` naming one `.img` or `.bin`, as DOSBox copies have): split with
  `bchunk` into its data track, unpacked with `bsdtar`, and its audio
  tracks, the CD's music, encoded with `oggenc` under the names wartool's
  own CD rip gives them (track 2 "Human Battle 1"...), the game then set to
  `.ogg` (`wc2-config.lua`); otherwise the MIDI music wartool takes from the
  data. Of several images the one with the most audio tracks: Beyond the
  Dark Portal's (17 songs) also gives the whole game (wartool: "expansion
  DOS CD", the four campaigns; the original CD gives two). No `-v` (videos
  need ffmpeg) nor `-r` (it rips from a real CD drive). wartool shows its
  errors in a desktop dialog (zenity): with the console's `DISPLAY=:0` it
  waited for ever, on a board with no screen, for a missing `SNDDAT.WAR`;
  it now runs with no display and a 15-minute limit (war1tool too). The
  user's copy (a DOSBox folder with both CDs' images, 2026-10-08) from the
  launcher: 4 min 7 s to the first frames on the Pi 400 (splitting 1.2 GB,
  extracting, an hour of music to OGG; later starts straight away), 244 MB
  extracted, menus 50-58 fps at 1.5 KB a frame to the stand-in PSP.
  Wargus's own size, 640x480, shrunk to 57 %: 640x360 (75 %, filling the
  screen) cut its game setup off at the left and the bottom, laid out for
  480 lines. Warcraft on the shared engine was tried again from the
  launcher (60 fps, START + SELECT). In a skirmish (2026-10-09) its text
  was unreadable on the stand-in PSP: shrunk without smoothing, the 1-pixel
  strokes of its letters are dropped with the rows and columns left out;
  and two pointers showed, the layer's white arrow beside the game's own
  (Stratagus hides the system pointer with a transparent cursor, which the
  layer now takes as hidden: one pointer, the game's). So the game now
  runs at the PSP's 480x272, pixel for pixel, the menus and the maps each
  laid out for it (`compatibility/wargus/wargus-small-screen.patch`,
  Wargus's scripts only). `wc2.preferences.FixedVideoWidth/Height`
  (480x272 by default here; 0: off) is the screen's size whatever
  VideoWidth/Height say (`pspexp.lua`, loaded before
  `SetVideoResolution`: the first size set is kept). Under 400 lines
  (`SmallScreen` in `guichan.lua`) the menus have their own places:
  - the full-screen menus' buttons in one or two columns below the title
    art (`AddSmallMenuButtons`; the main menu in two columns of four, the
    campaigns in rows of two under each series' title); the background
    kept in proportion, the bare sides black (the previous menu showed
    through them);
  - the game setup: the scenario along the top, the players down the left
    (colour, type, race, team; no AI script column, "Map" for the race's
    "Map Default"), the settings with their labels beside them on the
    right in the small font, the three buttons below;
  - the panels 352 high (scenario, map lists, mods, replays, language,
    keystroke help, servers) as high as the screen, their lists shorter;
    the in-game panels 288 high (game menu, options, sound, speeds, help,
    objectives, end scenario) squeezed to 272, what is on them moved up in
    proportion (`WarGameMenu`, `menu.squeeze`); Preferences takes the whole
    screen, its check boxes in two columns (`menu.place`);
  - the mission list across the screen, the results with the eight
    columns 60 apart and each player's line as high as there is room for,
    the multiplayer menus and the lobby (no unavailable slots listed, no
    map heading or file name: no room);
  - the map lists Standard Game offers besides Skirmish Modern
    (`scripts/lists/maps`, added 2026-10-09 after the user met them in
    PPSSPP on the GameShell): Skirmish Classic, Front Lines and For the
    Motherland bring their own game setup and scenario list, made for
    640x480, the buttons below the screen and Cancel on C, a key the PSP
    has not. Their scenario lists are now the game's own, laid out for the
    small screen, over their folders (`SelectScenarioMenu(dir)`); their
    setups placed as the game setup's (`SmallSetupScenario`,
    `SmallSetupOption`, `SmallSetupButton` in `guichan.lua`), For the
    Motherland's two teams of character and race one under the other.
    Checked on the stand-in PSP from the GameShell: each list's setup,
    scenario list, Escape and Cancel Game, a For the Motherland map with
    teams (Coastal Crusade) started; pspexp-wargus pspexp8. The lists'
    files have Windows line endings, kept in the patch;
  - left out there: Video options (the size is fixed), the Hardware
    cursor box (the layer draws no colour cursor), and the Map Editor from
    the main menu (its tools and dialogs, Player Properties 500x480, are
    for a large screen).

  The game itself (`ui_small.lua`): a 152-wide left column (122 since
  2026-10-10, below) with a 92-pixel
  minimap, the unit's panel (name, health, portrait and four short lines
  in the small font: damage, armour, range, production, supply; what a
  building makes beside its portrait with a progress bar; three portraits
  and "+N" for a group) and the 3x3 command buttons; the top line with
  gold, lumber, oil, food and a Menu button; the map 328x240 (about 10 x
  7.5 tiles, against 14 x 14 at 640x480). Wargus's own layout sized its
  button panel's background `144 + Video.Height - 480` (negative:
  Stratagus stopped with `bad_alloc`); the panel's 176x176 background is a
  1-pixel graphic (Stratagus refuses nil). Checked on the stand-in PSP
  (2026-10-09): every menu above, a skirmish (tips, the game menu, options,
  preferences, sound, speeds, help, keystroke help, objectives, save,
  surrender, the results) and orc mission 1's act title and briefing; from
  the launcher, the package upgraded, through the menus into a skirmish
  and back to the main menu; 54-60 fps, under 1 KB a frame; with the size
  set to 0, the 640x480 menus as before. The Save menu's name field keeps
  Escape (SELECT): Cancel closes it. Existing installs get the new
  scripts: the launch script copies Wargus's scripts again when the
  package's version changes (`.pspexp-scripts`; the extracted data and
  wartool's `wc2-config.lua` are kept).

  A narrower column (2026-10-10, the user on the real PSP: the column was
  a third of the screen): the command icons cut to their middle 36x30,
  pixel for pixel (the user's idea; shrinking them, 33x27 nearest
  neighbour, dropped detail), so the column is
  122 wide and the map 358x240. The launch script makes
  `graphics/tilesets/*/icons-psp.png` from the extracted `icons.png` with
  `crop-icons` (Python's standard library: an 8-bit PNG decoded,
  each 46x38 icon cut, written again; a third of a second a tileset on
  the Pi), again when the extracted sheet is newer; `icons.lua` takes it
  under 400 lines when it is there (`CanAccessFile`), else the full icons
  and the 152 column. `ui_small.lua` lays the column out from
  `IconW`/`IconH`: the full icons give exactly the earlier places. With the
  cut ones the raised frame is off (`Preference.IconsShift`: Stratagus
  draws it for 46x38 only, fixed sizes in `CIcon::DrawUnitIcon`) and the
  "icon" button style, the one every icon button shares, is redefined at
  36x30 with the hotkey letter's place given in each state (a style
  defined before keeps its states' places: the letters first showed below
  the icons). The engine's text when nothing is selected (name, cycle,
  the players' scores at fixed places for its 176-wide panel, the scores
  at x 133) is off (`Preference.ShowNoSelectionStats`): it ran onto the
  map. Checked on the real PSP from the Pi (the user: "looks correct") and
  on the stand-in: a berserker, a peon and its buttons, the empty panel;
  59.6-59.8 fps over whole games. pspexp-wargus pspexp10 (needs python3).
- **Warcraft and Warcraft II on the GameShell** (2026-10-09): both recipes
  now offer the armhf packages there too (built by
  `build-stratagus.sh armhf`, the same data and scripts). Measured with the
  stand-in PSP on the GameShell's own USB link (the Mac at the other end of
  the cable): Warcraft's first start 18 s, menus 60 fps, a battle (the
  demo its idle menu plays) 44-52 fps; Warcraft II's first start from the
  user's disc images 10 min 16 s (within wartool's 15-minute limit; 4 min
  on the Pi), menus 45-60 fps, a skirmish 58-60 fps scrolling and still,
  the game about 26 of its 30 cycles a second (the Pi's 30; read off the
  cycle counter half a minute apart), about 110 MB of memory. A first
  measurement through the Pi (the GameShell's USB to the Pi, a relay over
  Wi-Fi to the Mac: 13 ms a round trip against 1.5 ms) gave 22-30 fps and
  17 cycles a second: the game waits for the PSP's reply each frame, so
  the route, not the processor, was the limit. Warcraft's battle first
  ran at 15-30 fps with the sound skipping (about 140 underruns in two
  minutes): FluidSynth, synthesising the MIDI music as it plays, took the
  whole audio thread (`perf`: 85 % in `fluid_rvoice_dsp_interpolate` and
  `fluid_iir_filter_apply`); music off, 38-44 fps. Rendering the 104
  minutes of music to OGG once would have added about 10 minutes to the
  first start (FluidSynth renders 1.5x real time on one core), so the
  launch scripts instead set the layer's `PSPEXP_AUDIO_RATE=22050` on the
  GameShell (`/etc/pspexp/board`): SDL_mixer's `Mix_OpenAudio` lets the
  rate change, FluidSynth works at half the rate, a fifth of a core, and
  the layer upsamples by two. Warcraft II does so only with MIDI music
  (`wc2-config.lua`; the CD's OGG keeps 44.1 kHz): with its MIDI, 55-60
  fps in the skirmish, no underruns. Not yet on the real PSP. Selecting
  a unit in Warcraft II then crashed the GameShell (the user, in PPSSPP;
  every time, any map): the command buttons' bottom row was 1 pixel low,
  and its icon frame (`CIcon::DrawUnitIcon` draws 4 pixels around the
  icon, unclipped) wrote line 273 of the 272-line screen, past the
  surface; the Pi's 64-bit build survived the stray write, the GameShell's
  32-bit one did not (found with a debug build of Stratagus under gdb).
  `ui_small.lua` now raises the grid 4 pixels (last frame line 271);
  pspexp-wargus pspexp9.
- **Sonic Robo Blast 2** (`srb2`, 2026-10-04): the SRB2 team's free 3D
  Sonic fan game (2.2.15); the PSP has only an unfinished port of an old
  version. Built from the release tag (`scripts/packages/build-srb2.sh`,
  `pspexp-srb2`; its assets' MD5s are in the source, so the build needs
  none of them); the installer `srb2` downloads the release's
  `SRB2-v2215-Full.zip` (179 MB; its Windows program comes along unused).
  Unlike SRB2Kart's, SRB2 2.2's software renderer runs at the PSP's own
  480x272. Greenflower Zone at 22-23 fps into PPSSPP (its cap 35), 44 KB a
  frame as CLUT8 pages. The PSP's controller is its gamepad; SDL lists
  START and SELECT as the 7th and 8th buttons, where SRB2's defaults expect
  an Xbox pad's Back (screenshot) and Start (menu), so the first run writes
  `~/.srb2/autoexec.cfg`: START the menu, SELECT the scores, L / R turn
  the camera (no right stick). Nub moves, cross jumps, square spins; in
  menus the d-pad moves, cross chooses, circle goes back.
- **Command & Conquer and Red Alert** (`cnc`, `redalert`, 2026-10-04):
  Vanilla Conquer (the source EA published in 2020, made portable: SDL2,
  8-bit frames, `scripts/packages/build-vanillaconquer.sh`, package
  `pspexp-vanillaconquer`), drawn by the PSP as CLUT8 pages, so every glibc
  board; 25-28 fps into PPSSPP. The data is EA's freeware release (2007,
  2008) as archived from EA's page on archive.org ("Command & Conquer
  Classics Original Freeware Releases 2007-2010": the GDI disc, Red Alert's
  Allied disc, EA's licence agreement inside), downloaded at install by
  `cnc-freeware` / `redalert-freeware` (`_discinstaller.sh`: SHA-256
  pinned, the disc image out of the RAR, its MIX files out of the image
  with 7z). OpenRA's quick-install pack was tried first: it lacks the movies
  (Red Alert's first start and campaign crash on a missing movie, without
  PSPExp too) and the skirmish maps. With the discs: the campaign's video
  briefings, the skirmish map list, C&C's interlaced intro. Its 640x400
  stays whole (`PSPEXP_TEX_HALF=0`: halved to 320x200 the menus could not
  be read); the dialog text is small at 480x272. Mouse as the original: nub
  the pointer, cross / circle the buttons, d-pad scrolls (arrows), START
  the options. Play past the menus not reached by script (its dialogs want
  clicks in the game's own coordinates).
- **Unreal Tournament: GOTY** (`ut99`, Pi only, 2026-10-04): free since
  Epic allowed OldUnreal to distribute it (the disc image on archive.org,
  their patch on GitHub). The installer runs OldUnreal's own Linux
  installer (FullGameInstallers at a pinned commit, its SHA-256 checked),
  which needs the Epic Games Terms of Service accepted: the first recipe
  with `terms=` (Package Manager §4.3; Packages asks on the PSP). Their
  latest patch (469e) draws through **SDL3**, which the SDL layer does not
  hook, so their 469d arm64 patch (SDL2, pinned) is laid over it, and its
  `Default.ini` / `DefUser.ini` set a 480x272 window and E for the next
  weapon. Its OpenGLDrv wants `glCompressedTexImage3D` and three more by
  name (stubs added to the GL layer), then ARB vertex and fragment
  programs, which the PSP's layer has not: drawn by the Pi's GPU instead,
  the city fly-by intro right at 480x272, about 21 fps into PPSSPP. Keys
  and mouse: nub aims, cross fires and clicks, R alt-fires, d-pad moves
  and strafes, circle jumps, square ducks, triangle the next weapon, L the
  translocator, START the menu.
- **AssaultCube** (`assaultcube`, 2026-10-04): the free online shooter,
  no PSP or Vita port. Debian's arm64 build fails against its own local
  server ("protocol incompatible"); built from the project's v1.3.0.2 tag
  instead (`scripts/packages/build-assaultcube.sh`: its enet configured
  first, its Makefile racing under -j; GCC, not the clang++ it names), package
  `pspexp-assaultcube`, and its data from the project's Linux release at
  install (`assaultcube-release`, 52 MB; not named assaultcube, Debian's
  package would win). Drawn by the PSP's GE: 24-25 fps into PPSSPP, 19 KB a
  frame; the first-run dialog over a map in 3D. It first crashed with a
  heap corruption: the GL layer's black-reading `glReadPixels` wrote 16
  bytes for one float of depth (now exactly the bytes asked for). The
  installer's `.tar.bz2` was only decompressed by unar (it knows a
  compressed tar by name, and the download is `game.part`): tar now for
  .bz2 and .xz as for .gz. Mouse and keys: nub aims, cross fires, R the
  scope, d-pad moves, circle jumps, square reloads, triangle the previous
  weapon, L crouches, START the menu, SELECT the scores.
- **Lugaru HD** (`lugaru`, 2026-10-04): Wolfire's 3D rabbit fighter, OSS
  Lugaru 1.2 from Debian (code GPL, data CC BY-SA; on the Vita, not the
  PSP). Its fixed-function OpenGL is drawn by the PSP's GE: menus, the
  campaign map and the tutorial level right in PPSSPP at 19-21 fps, but
  **416 KB a frame** in play (its geometry sent anew each frame: the most
  the PSP renderer has carried; to be measured on the PSP). Lugaru wants a
  player name typed, through SDL's text input, which the PSP's buttons do
  not give: `pspexp-lugaru`'s launch script writes a player "Player" (the
  users file as `Account::saveFile` writes it, big-endian) and 480x272
  settings on the first run, then starts Debian's lugaru. Mouse and keys:
  nub the pointer and the camera, cross attacks and clicks, the d-pad
  WASD, circle jumps, square crouches / rolls, triangle draws (E), L throws
  (Q), START Escape. Reached by script with the new pointer commands of
  `PSPEXP_SCRIPT` (`~DXxDY`, `click`): Lugaru builds its cursor from the
  relative motion only, from its corner.
- **Celeste 64: Fragments of the Mountain** (`celeste64`, Pi only,
  2026-10-04), chosen as PSPExp's showcase: Maddy Makes Games' free 3D
  platformer (2024), on no PSP or Vita. Their own Linux arm64 build, which
  the installer downloads from their itch.io page (upload 9667963, 31 MB,
  checksum pinned); nothing hosted, not even a cover. Foster's
  `libFosterPlatform.so` has SDL2 2.32 built in, but exports its functions,
  so the preloaded layer still takes the window, the GL context and the
  controller. Its renderer wants OpenGL 3.3: Mesa told so on the Pi's V3D.
  It went full screen into the offscreen driver's 1024x768, its 16:9
  letterboxed and three times the pixels read back: `PSPEXP_WINDOW=480x272`
  (new) keeps it a window of the PSP's size. **60 fps on the Pi** with no
  PSP in the loop; 19-24 fps into PPSSPP (the emulator the limit), 133 KB a
  frame; title, menus and the first level right. The PSP's controller is
  its gamepad (nub moves, cross jumps, square / circle dash, L / R climb,
  START pauses); the first run's `controls.json` puts the camera on the
  d-pad, the PSP having no second stick. No keys. It stopped starting
  after the Pi had been rebooted with no display (2026-10-06): its sound,
  FMOD, plays through ALSA, whose only device on the Pi is HDMI, and that
  does not open without a display; FMOD did not start and the game quit
  after 4 s. `pspexp-run` now gives ALSA its own output into the sound pipe
  (SDL layer README), and Celeste 64 is heard on the PSP for the first
  time; the pipe reader keeps it to real time (taken as fast as FMOD
  wrote, its sound swamped the link and the PSP dropped it). Its
  controller had no up and down until the same day (layer README).
- **Luanti** (`luanti`, formerly Minetest, Pi only, 2026-10-04): Debian's
  5.10.0 is built for X11 (Irrlicht's own window: "Need running XServer"),
  so `scripts/packages/build-luanti.sh` builds the same version with its
  SDL2 device (the program lands in the source tree's `bin/`), package
  `pspexp-luanti` on Debian's luanti-data and Minetest Game. Under the PSP's
  layer, its legacy OpenGL driver with shaders off (Luanti warns against
  it), it crashes once in the world (framebuffer calls the layer lacks among
  the first): drawn by the Pi's GPU instead, shaders on, 20-22 fps into
  PPSSPP, 17 KB a frame standing. `/usr/games/pspexp-luanti` writes its
  settings (480x272, view 80, joysticks off) and a world "PSP" on the first
  run and goes straight in: its menus want a mouse. Keys: nub looks (as a
  mouse), d-pad walks (WASD), cross jumps, square digs and circle places
  (the mouse buttons), triangle the inventory, L / R the hotbar (B / N),
  SELECT sneaks, START the menu. Walking and jumping checked by script.
- **Open Surge** (`opensurge`, Pi only): built by
  `scripts/packages/build-opensurge.sh` (0.6.1.3, Allegro 5 with its SDL2
  backend, so its window, input, sound and GL are the layer's; Allegro's GTK
  dialogs replaced by `allegro-dialog-stub.c`, messages to the log), package
  `pspexp-opensurge` with its data (free licences, each listed in its
  `licenses/` folder). Under the PSP's layer each frame's 426x240 render
  targets did not fit in VRAM (a black screen); **drawn by the Pi's GPU**
  (`PSPEXP_HOSTGL=1`, `graphics=board-gpu`) it runs at about 30 fps in
  PPSSPP, its 426x240 enlarged to the screen. Allegro had to be built with
  GLU and desktop GL (it chose OpenGL ES, where Open Surge's GLSL 1.20
  shaders fail), and its SDL backend patched to answer `$HOME` for the
  user's home folder (it gave the program's folder: saves refused under
  /usr/lib). Its keyboard: d-pad, cross jumps (Space), square Ctrl, circle /
  triangle Shift, START Enter, SELECT Escape; the joystick off (Allegro would
  read it as well).
- **Wyrmsun**: not attempted; its engine is now Qt-based, out of the SDL
  layer's reach.
- **SuperTuxKart** (1.4), **OpenClonk** (8.1), **Trackballs** (1.3.4), from
  Debian: none starts under the PSP's layer; they ask for OpenGL 3.1, 3.2
  and 3.3 core contexts, and the layer is OpenGL 2.1. Drawn by the Pi's GPU
  instead: SuperTuxKart and Trackballs packaged (below); OpenClonk starts
  (GLEW's "no GLX display" accepted, Mesa told to offer 3.2) but crashes in
  its start menu, not packaged.
- **Trackballs** (`trackballs`, Debian's 1.3.4, Pi only): drawn by the Pi's
  GPU, Mesa told to offer OpenGL 3.3 (`MESA_GL_VERSION_OVERRIDE=3.3`,
  `MESA_GLSL_VERSION_OVERRIDE=330`; V3D has 3.1, and what Trackballs uses
  runs). A 640x480 window (`-w -r 640`, its smallest) shrunk to the screen:
  the menu readable, 26-31 fps in PPSSPP. Menus by the nub as a pointer
  (cross clicks); in play the d-pad steers, square jumps, START Escape.
- **Project: Starfighter** (2.4, Debian): drew nothing at first: its whole
  800x600 frame, one streaming texture, needed 20 of the small pages a
  streaming texture is cut into, over the 16 a texture may have, and the
  layer dropped it without a word. Now such a texture falls back to full
  512 pages (logged when even that is too many), and a streaming texture
  over 512 on a side is kept at half size on the board (`PSPEXP_TEX_HALF`):
  1.2 MB a frame and 13.6 fps in PPSSPP became 197 KB and 30.7 fps. It plays,
  but its menus' text, an 800x600 game shown at 0.45, cannot be read (as
  Flare's); not packaged.
- **Egoboo** (`egoboo`): the egoboo repository's SDL2 code (2.9.0 at
  401393e, unreleased since 2017; `scripts/packages/build-egoboo.sh`, GCC 14
  needing standard headers forced in, `pspexp-egoboo`), and its GPL-3.0
  assets downloaded at install from egoboo-assets (an installer, 167 MB, 129 s
  on the Pi). Its data path was only beside its program; `EGOBOO_DATA` is
  patched in. GLEW finds GL through `glXGetProcAddress`, which the layer
  answers. Its default 1920x1080 window showed at a quarter; the launch
  script's first settings make it 640x360 (0.75 on the PSP: the menu
  readable, 27 fps in PPSSPP). Menus take the mouse (nub, cross clicks); in
  play its keyboard (square / circle the hands' items, triangle the
  inventory, R jumps, L / SELECT grab, START the menu). Play not reached by
  script (the menus need clicks).
- **The Legend of Zelda: Mystery of Solarus DX** (`zsdx`): the Solarus Team's
  fan game, v1.12.3, on the Solarus 1.6.5 engine built by PSPExp
  (`scripts/packages/build-solarus.sh`, `pspexp-solarus`: the engine and its
  library together, found through RPATH; its Debian Depends listed from what
  it loads, dpkg-shlibdeps refusing the private library). Nothing of the
  quest is hosted, not even a cover (its title is the game's art and the
  Zelda logo; the launcher generates one): the installer downloads its
  official `.solarus` file from the GitLab release (checksum as its
  checksums.txt gives it, 21 MB, 19 s on the Pi) and keeps it whole
  (`KEEP=` in `_installer.sh`: the engine reads it as it is). It draws
  through SDL's renderer, its 320x240 shown 4:3: the language menu, title,
  and the name entry right in PPSSPP at 24-26 fps. Solarus's keys: cross
  Space (action, confirm), square C (sword), triangle / circle X / V (the two
  items), START D (pause); its sound is OpenAL's, through pspexp-run.
- **SuperTuxKart** (`supertuxkart`, Debian's 1.4, Pi only): **drawn by the
  Pi's own GPU** (architecture §12a, host-rendered OpenGL: `PSPEXP_HOSTGL=1`
  in the entry, `graphics=board-gpu`, shown as "OpenGL 3, Board GPU renderer" on the details card, "Board GPU" under renderer in Packages).
  At the PSP's 480x272; its "screen resolution is too low" message is a
  warning only (`main.cpp`: below 480 lines the menus are cramped) and is
  turned off. Its first start is answered by three `first-run=` files (a
  recipe may now have several): `config.xml` with the warning off and the
  Internet not used (SuperTuxKart fills in the rest itself), `players.xml`
  with a player "Player" (else a Create User screen), and `input.xml` with
  the PSP's controller, which it recognises by its game-controller mapping
  and numbers by raw button (cross 0, circle 1, square 2, triangle 3, L 4,
  R 5, START 6, SELECT 7, the d-pad 12-15): cross accelerates, square
  brakes, circle fires, R nitro, L drift, triangle rescue, SELECT look back,
  START pause; in menus cross selects, circle goes back (its own default put
  accelerate on triangle and look-back on cross). No keys (`keys=` all
  none), so nothing counts twice. Seen in PPSSPP through the installed
  start script: the main menu with Player, its sound streamed; a race at
  11-16 fps there (the Pi drawing 34 ms a frame). `[debian:gameshell]` with
  an empty `apt=` keeps it off the GameShell, whose GPU has OpenGL ES 2 only. `/usr/games/alter-aila-genesis` keeps the saves in the
  user's data folder (`--save-path`). 320x240, shown 4:3 in the middle;
  cross confirms (Z), circle / triangle / START the menu (X), square held
  runs (Shift). The title, a new game and its opening scene drawn right in
  PPSSPP at 26-29 fps. The GameShell's EasyRPG (armhf) is built the same way under
  emulation (about 20 min on the Mac) and published; not yet run there.

DreamChess (`dreamchess`, 2026-10-01; 30 fps on the PSP-1000 with scripted
keys, not yet played by hand): 3D chess against its engine, at 480x272. The d-pad or the nub moves the
selector, cross picks a piece and then its square, circle / start open the
menu; the pointer cannot pick pieces (DreamChess reads the pixel under it
back, which the PSP does not give). Its recipe sets `PSPEXP_GL_EXTENSIONS`.

Emilia Pinball (`pinball`, 2026-10-01; 30 fps on the PSP-1000 with scripted
keys, not yet played by hand): the Tux table to start (the Professor from its menu). L and R are the
flippers, cross launches, square / circle / triangle nudge the table, start
opens the menu.

On the real PSP over USB from the GameShell (played, 2026-09-27): Cave
Story 47-49 fps of its 50, no underruns ("felt great"); Jazz Jackrabbit
38-50 fps in the level (7-23 in the plasma menu; each frame a full 480x272
upload, ~7 ms of texture work on the GameShell), "pretty playable"; Simutrans
a steady 25 fps (its own rate), no underruns. From the Omega2+ (played the
same day): Cave Story 42-50 fps after its 35 s start ("Loading..." on the
PSP meanwhile), Jazz 22-48 fps in the levels with stalls and broken sound
at first; with its music mixed at 22050 Hz, linear, no effects (Omega build
only) a steady 25-33 fps through level 1 and clean sound: "perfect". The stand-in's numbers below were the Wi-Fi
link to the Mac, not the boards.

Measured on the stand-in PSP: Cave Story 50 fps on the Pi, 31-36 on the
GameShell, 26-32 on the Omega2+ (which also spends 35 s at start
synthesising the sound effects in software floating point); Jazz's menu 38-47
fps on the Pi, 0.1-12 on the Omega2+ (its plasma background, floating point
again). Over USB to the real PSP both play on the Omega2+ too (above).

With `PSPEXP_NUB=mouse` the SDL layer draws the mouse pointer itself on the
PSP, over the frame (a host cursor never reaches it), while the program has
not hidden its cursor (`SDL_ShowCursor`).

Cave Story needed three things of the SDL layer that any game may: a
software renderer when the program asks for an accelerated one and the board
has none; its window reported focused and shown (`SDL_GetWindowFlags`: the
game pauses otherwise, and a headless window is never focused); and the TCP
link reading while a send is stuck (a large upload with the PSP sending
back could wedge both sides).


### 4.3 The card image and the first boot

> **Superseded (2026-09-29):** the ClockworkOS image and its clean-up are gone.
> The card image is the minimal one of `scripts/gameshell/base/build-image.sh`
> (docs/Package_Manager.md §8): no bundled packages, and a first boot that only
> grows the card. What follows is how it was.

`scripts/gameshell/prep-stock-image.sh` (and `install-gameshell.sh` for a running
GameShell) stage every built package into `/opt/pspexp/preinstall` and
enable `pspexp-firstboot.service`, which runs once before the login shell
and the console: `clean-stock.sh` (idempotent; copies the small stock config
files to ~/pspexp/stock-backup first), then `pspexp-pkg install` for each
preinstalled package, then `/etc/pspexp/firstboot-done`. Verified on the
board (reboot after install-gameshell: stock removed, 26 GB free, both
packages installed, the launcher showing them) and on a copy of the stock
image (the files, owners and service link in place, filesystem check clean).
A public image still needs a release mode of the prep script: no personal
SSH key, no Wi-Fi credentials.


### 10.2 Packages

> **Superseded (2026-09-29):** the app now shows the recipe catalogue on every
> board (docs/Package_Manager.md §7, §9); the folder and web repositories
> below are gone.

`store.c` reads the repositories and compares them with what is installed
(`<root>/apps/*/package`). Each package shows as Installed, Update, or Get.

- **Buttons:** cross installs, updates or reinstalls; square removes, after
  a question; start reads the repositories again.
- **How it installs:** it runs `pspexp-pkg` in the background (log:
  `/tmp/pspexp-store.log`), and the launcher notices the change by itself.
- **Repositories:**
  - **On the board:** `<root>/repo` (`/opt/pspexp/repo`; on the Omega
    `/mnt/mmcblk0p1/pspexp/opt/repo`). A card image can carry one.
  - **Others:** one per line of `~/.config/pspexp-launcher/sources`, either a
    folder or an `http(s)` address. For an address, the app reads
    `<address>/<board>/`, where `<board>` is `gameshell`, `pi` or `omega`
    (from `uname -m`), and fetches with curl or wget into
    `~/.cache/pspexp-store`.
- **The index:** a repository holds `index`, with one line per package
  (`id|version|bytes|file|title|description`), the `.tar.gz` packages, and
  `<id>.png`, the package's own cover (its first entry's picture).
- **Filling a repository from the Mac:** `scripts/packages/pspexp-pkg-build`
  writes the index on every build (`index` on its own). `publish` copies
  `build/<board>/pkg` to the board's `<root>/repo`:

  ```sh
  BOARD=omega HOST=omega scripts/packages/pspexp-pkg-build publish
  ```

  Nothing is published on the internet; a web repository is for later.
