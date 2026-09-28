# PSPExp packages: recipes over the board's own package manager

*Design, reviewed 2026-09-28 (decisions in §11). Nothing here is
implemented yet except what "Today" describes.*

## Contents

1. Goals
2. Today
3. The design in one page
4. Recipes
5. On the board: `pspexp-pkg`
6. PSPExp's own apt repository
7. The Packages app
8. The minimal GameShell image
9. The Omega2+
10. Performance, licences, tests
11. Decisions needed
12. Implementation plan

---

## 1. Goals

- **A minimal image.** The GameShell card ships Debian, the PSPExp core and
  a catalogue, and no games. The user installs what they want from the PSP,
  with Apps > Packages.
- **The board's own package manager does the work.** Programs, libraries and
  updates come through `apt` (Debian boards) wherever a distribution package
  exists. PSPExp keeps as few dependencies and as little compiled code of
  its own as it can.
- **Our repository holds recipes, not binaries.** A recipe gives the
  configuration that makes a program right on the PSP: its launcher entry,
  its buttons and its settings. When needed, it also gives the build steps
  and patches.
- **Adding a package is easy.** One small folder, readable in a review, with
  no build system to learn for the common case.

**Not goals now:**
- a web store with accounts or ratings;
- ROMs or other copyrighted game data (never);
- the Omega on apt (§9).

## 2. Today

- **Build:** recipes are shell scripts (`scripts/packages/recipes/*.sh`) run
  on the Mac. Each fills a package folder, and `pspexp-pkg-build` makes
  `build/<board>/pkg/<id>.tar.gz` of it.
- **What a package holds:** the program's binary (cross-compiled with Zig, or
  compiled on a board), its data (downloaded), its launcher entry and cover.
- **Distribution:** `pspexp-pkg-build publish` copies the packages, an
  `index` and the covers to a folder on a board (`<root>/repo`). Apps >
  Packages reads that folder and installs with `pspexp-pkg`, which unpacks
  into `<root>/apps/<id>`.

**Shortcomings:**
- only the Mac can fill a repository;
- every board type needs its own binaries built by us;
- no integrity check;
- versions are sometimes git hashes, which don't compare;
- the board type is guessed from `uname -m`;
- a package's requirements aren't shown before installing;
- PSPExp itself isn't a package.

## 3. The design in one page

```text
 our git repository                       a board (GameShell, Pi)
 ------------------                       ------------------------------------------
 recipes/<id>/recipe      --(catalogue)-->  Apps > Packages  --- pspexp-pkg install <id>
 recipes/<id>/cover.png                                            |
 recipes/<id>/patches/*.patch                                      |- apt-get install <pkg>
                                                                   |      Debian's mirrors, or
 debian/ (build recipes for what             +---------------------+      PSPExp's apt repository
  Debian lacks) --(built on the Mac)-->  PSPExp apt repository     |
                                          (signed, e.g. GitHub      '- launcher entry, cover,
                                           Pages)                      PSP buttons, settings
```

- **A recipe says which package to install and how it appears on the PSP.**
  The package is usually Debian's (`apt=opentyrian`). For what Debian lacks,
  it's one of ours (`apt=pspexp-cavestory`).
- **`pspexp-pkg`** installs the package with apt, then writes the launcher
  entry, cover and settings from the recipe. `remove` reverses both.
- **PSPExp's own apt repository** holds only what Debian doesn't have:
  - the PSPExp core (console, launcher, SDL layer, emulator player,
    `pspexp-pkg`);
  - games Debian lacks (Cave Story);
  - builds made lighter or patched for these boards.

  They are ordinary signed `.deb` files, so `apt upgrade` updates PSPExp too.
- **The catalogue:** the recipes as one small archive, shipped on the image
  and refreshed from our repository.

## 4. Recipes

### 4.1 Layout

```text
recipes/
  opentyrian/
    recipe            what to install, how it shows and plays
    cover.png         128x128, its launcher cover (optional: else generated)
  cavestory/
    recipe
    cover.png
    build             only for our own packages: how the .deb is made
    patches/          only when the program needs changes for the PSP
  nes/
    recipe
```

### 4.2 The `recipe` file

A `KEY=value` file with sections, like the launcher's entry and system
files, so no new parser or dependency is needed:
- `[package]`: what it is;
- `[debian]`, `[omega]`: what to install on each kind of board;
- `[entry]` and `[system]`: what the launcher shows.

Example 1, a game that Debian has:

```ini
[package]
id=opentyrian
title=OpenTyrian
description=The vertical shooter Tyrian 2.1, freeware since 2004
category=game
license=GPL-2.0 (OpenTyrian); freeware data (Tyrian 2.1)
version=2                        # the recipe's own version: bump it when the entry changes

[debian]
apt=opentyrian                   # Debian's package (and whatever it depends on)
data=https://camanis.net/tyrian/tyrian21.zip sha256=<...> into=~/.local/share/pspexp/opentyrian
                                 # only when the distribution does not ship the data

[entry]
title=OpenTyrian
type=game
exec=opentyrian --data=~/.local/share/pspexp/opentyrian --no-joystick
keys=cross=space,circle=lctrl,square=lalt,triangle=return,l=lctrl,r=lalt,start=p,select=escape
nub=keys
config=~/.config/opentyrian/opentyrian.cfg:section video\n\titem fullscreen -1
                                 # written once, on first install, when missing
```

Example 2, an emulated system from Debian's libretro cores and our player:

```ini
[package]
id=nes
title=NES
description=Nintendo Entertainment System (ROMs in ~/roms/nes)
category=system
license=GPL-2.0 (Nestopia core)

[debian]
apt=libretro-nestopia
core=nestopia_libretro.so        # found in the distribution's libretro folder

[system]
name=Nintendo Entertainment System
short=NES
extensions=nes
roms=~/roms/nes
boxart=Nintendo - Nintendo Entertainment System
```

Example 3, a game Debian lacks: our own `.deb`, built from a recipe.

```ini
[package]
id=cavestory
title=Cave Story
category=game
license=GPL-3.0 (NXEngine-evo); freeware data (Cave Story, 2004)

[debian]
apt=pspexp-cavestory             # from PSPExp's apt repository (§6)

[entry]
title=Cave Story
type=game
exec=nxengine-evo
keys=cross=z,circle=x,square=a,triangle=s,start=escape,select=f2
```

`recipes/cavestory/build` says how `pspexp-cavestory` is made:
- the upstream source: address and exact commit;
- the patches;
- the build commands, one set per architecture (armhf for the GameShell,
  arm64 for the Pi);
- the files to package.

A shared script turns that into `.deb` files on the Mac (§6).

### 4.3 Rules

- **Commands and keys:** the entry's command runs through the SDL layer
  (`pspexp-run`) unless the recipe says `native=1`. The buttons use the
  names the SDL layer knows (`keys=`, as today).
- **Everything the recipe itself downloads is pinned** by checksum. apt
  checks its own packages.
- **Versions:** a recipe's `version` counts changes to the recipe. The
  program's version is apt's.
- **Choosing between sources:** a recipe may name a different package for a
  particular board when speed differs (§10.1):

  ```ini
  [debian]
  apt=libretro-snes9x
  [debian:gameshell]
  apt=pspexp-snes9x2005
  ```

- **Contributing:** a new package is a pull request adding one folder. A
  check script validates the file: sections, names, checksums, cover size.

## 5. On the board: `pspexp-pkg`

The same tool as today, now driven by recipes:

```text
pspexp-pkg list                     installed recipes
pspexp-pkg available                the catalogue, with state (installed / update / new)
pspexp-pkg install <id>...          apt, then the launcher files
pspexp-pkg remove <id>...           the launcher files, then apt (the program; not the user's saves)
pspexp-pkg update                   the catalogue, and apt's package lists
pspexp-pkg upgrade                  apt upgrade of what PSPExp installed, and changed recipes
```

What `install` does:
1. Read the recipe from the catalogue.
2. Check the board: kind (`/etc/pspexp/board`), free space, network.
3. Install the recipe's apt package, through the narrow privilege below.
4. Fetch pinned data, if any, and check it.
5. Write the launcher files into `<root>/apps/<id>/`: the entry or system
   file, the cover, and first-run settings.
6. The launcher notices, as today.

If any step fails, it undoes what it did.

- **Board kind:** the installer writes `/etc/pspexp/board`
  (`gameshell`, `pi`, `omega`), so nothing is guessed from the processor.
- **Privileges:** apt needs root. The image adds one sudoers rule that lets
  the PSPExp user run a small root helper, `pspexp-pkg-apt`, without a
  password. The helper only takes `install` / `remove` of package names
  found in the catalogue's recipes (plus `pspexp-*`). Nothing else runs as
  root.
- **Removing:**
  - apt removes the program only when PSPExp installed it, and no other
    installed recipe needs it. `pspexp-pkg` records that under
    `<root>/state`.
  - The user's saves and settings in their home folder stay.
- **Progress:** apt's progress (download, unpack) is read from its status
  output for the Packages app.

## 6. PSPExp's own apt repository

**What goes in, and only this:**
- `pspexp-core`: the console and launcher, the SDL layer, the emulator
  player, `pspexp-pkg`, the catalogue;
- `pspexp-runtime`: the PSP's own files (EBOOT, runtime, USB driver). The
  console copies it to the PSP over the link when it differs, as done by
  hand for the clock;
- games Debian lacks (`pspexp-cavestory`);
- lighter builds where speed needs them (`pspexp-snes9x2005` for the
  GameShell).

**How:**
- **Building:** on the Mac, for `armhf` (GameShell) and `arm64` (Pi).
  - The core is cross-compiled with Zig, as now, against the board's
    libraries.
  - Our game packages follow their `build` recipe.
  - `.deb` files are made with `dpkg-deb`, or a small script in the same
    format.
- **Signing:** a GPG key of the project. The image carries its public key
  and an apt source for the repository.
- **Hosting, on the home server until the project is published:**
  - The repository (`dists/`, `pool/`) and the catalogue are plain static
    files.
  - The Mac builds them and pushes them to the home Linux server
    (`scripts/packages/push-repo`: rsync over ssh). The Mac keeps only its
    build folder, which can be cleared once a build is pushed.
  - The server serves them over plain HTTP on the home network, with any
    static web server (whatever it already runs, or a small service).
    Signed files make plain HTTP safe there.
  - The server also keeps the older versions of each package. The Mac
    doesn't have to.
  - The boards' installers write the server's address into
    `/etc/apt/sources.list.d/pspexp.list` and into the catalogue's source.
  - **Publishing later** means copying the same files to a public static
    host (for example GitHub Pages) and changing that one line on the
    boards and in the image. Nothing else changes.
- **Updates:** a new build of the core is a new version in the repository,
  and `pspexp-pkg upgrade` (or plain `apt upgrade`) brings it. The Packages
  app shows "PSPExp update" like any other.

## 7. The Packages app

It stays as built, with these changes:
- **The catalogue:** it reads the recipes (with covers) instead of a folder
  of archives.
- **Sections:** Games, Emulators, Apps, and PSPExp itself.
- **States:** Get, Installed, Update. It says why something can't be
  installed on this board: no network, not enough space, not for this board
  kind, or a warning such as "too heavy for this board".
- **Progress:** a bar while downloading, fed by apt.
- **Notices:**
  - "Updates available" on the launcher, after a daily `pspexp-pkg update`
    when the board is online;
  - "Needs a network: join Wi-Fi in System" when offline.

## 8. The minimal GameShell image

- **What it contains:**
  - Debian 13 (as the card has now);
  - `pspexp-core`;
  - the catalogue;
  - the apt source and key;
  - the sudoers rule;
  - no games and no emulators.

  The first-boot service and Wi-Fi setup stay as they are.
- **First boot, the user's side:**
  1. join Wi-Fi from System > Wi-Fi network;
  2. open Apps > Packages;
  3. choose.

  A "Suggested" row could offer a first pick: Freedoom, OpenTyrian, NES.
- **Size:** smaller than today's card with bundled packages. The user's
  downloads go to the card, and free space shows in System Monitor and in
  Packages before installing.

## 9. The Omega2+

OpenWrt has none of these programs and the Omega has no compiler.

**Proposal:** it keeps today's system of our own static builds, published as
our archives, but reads the **same recipes**:
- a recipe's `[omega]` section names our build;
- the same Packages app shows them.

Later, the same idea as §6 with opkg: an `.ipk` feed.

## 10. Performance, licences, tests

### 10.1 Performance

Debian's builds are generic and some are heavier than ours: SNES (snes9x
against snes9x2005), and possibly Cave Story. A recipe can choose a lighter
source for a board (§4.3). The integration tests' games suite records the
frame rates for each board and flags drops. It does not fail on them,
because boards differ.

### 10.2 Licences

- **Debian's packages:** Debian has already checked them.
- **Our apt repository:** only redistributable programs and data. Each
  recipe states its licence. The check script refuses a recipe without one.
- **ROMs:** never.
- **To check before hosting:** Cave Story's freeware data, Tyrian's data
  and Jazz's shareware episode.

### 10.3 Tests

- **Build time:** `tests/test_recipes` validates every recipe (the rules in
  §4.3), and a `pspexp-pkg` test runs against a fake apt on the Mac.
- **On a board, via `tests/psp/run.py`:**
  - the apps suite: Packages lists the catalogue;
  - `--store-writes`: install and remove one small recipe through apt, check
    the launcher entry appears and disappears, and check the game starts
    (games suite).

## 11. Decisions needed

1. ~~**Hosting**~~ *Decided:* the home Linux server (`home-server.local`), on
   the home network, until the whole project is published; a public host
   comes then.
   - **Checked 2026-09-28:** Ubuntu 24.04, 55 GB free, key login from the
     Mac works, rsync and gpg present.
   - **Already in use:** Apache has port 80 and another service has 8080;
     neither is to be touched.
   - **Proposal for step 4:** a user service of its own, a small static
     server on a free port (e.g. 8093) serving `~/pspexp-repo`. It needs
     `sudo loginctl enable-linger pleft` once, so it runs without a
     login.
2. ~~**Signing key**~~ *Decided:* a project GPG key created on the Mac and
   used by the build scripts; a backup copy kept safe by the owner. Boards
   and the image carry only its public half.
3. ~~**Root**~~ *Decided:* the narrow sudoers rule for `pspexp-pkg-apt`
   (only `apt-get install/remove` of packages named in the recipes).
4. ~~**The Omega**~~ *Decided:* it keeps our own static builds, described by
   the same recipes and shown in the same Packages app. An opkg feed is for
   later.
5. ~~**Package names**~~ *Default:* `pspexp-<name>` for ours (`pspexp-core`,
   `pspexp-cavestory`).
6. ~~**The first catalogue**~~ *Decided:*
   - the current games: Freedoom, Chocolate Doom, OpenTyrian, Simutrans,
     OpenJazz, Cave Story;
   - the current systems: NES, Game Boy, Game Boy Color, Mega Drive, SNES.
7. ~~**Wesnoth, GBA**~~ *Decided:* not in the first catalogue.

## 12. Implementation plan

Each step ends tested on a board. The Pi is first, since it's online and
also runs Debian.

1. **Recipe format and check:** the format, the check script, and
   converting two recipes: OpenTyrian (Debian's) and NES (Debian's core
   with our player). Test on the Mac.
2. **`pspexp-pkg` and the privilege helper:** `pspexp-pkg` v2 (apt), the
   root helper, the board-kind file. Install OpenTyrian and NES from
   recipes on the Pi, with launcher entries.
3. **The Packages app on the catalogue:** sections, progress, states. The
   apps suite on the real PSP.
4. **Our apt repository, locally:**
   - `pspexp-core` and `pspexp-cavestory` as `.deb`;
   - a signed repository built on the Mac, pushed to the home server and
     served there;
   - the Pi installs Cave Story and upgrades the core through apt.
5. **The GameShell:** the same on armhf, and lighter cores where the games
   suite says so.
6. **Every recipe converted,** and the old archive packages retired. The
   Omega keeps its builds, read through recipes.
7. **Publishing, when the project goes public:** the same files on a public
   host, and the image built with only the core, the catalogue and the
   public address.
