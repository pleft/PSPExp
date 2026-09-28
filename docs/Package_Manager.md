# PSPExp packages: recipes over the board's own package manager

*Design, reviewed 2026-09-28: decisions in §11; operational contracts in §5.1-5.5 and §6.1 after a review. Nothing here is implemented yet except what "Today" describes.*

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

  They are ordinary `.deb` files in a repository whose metadata is signed
  (§5.1), so `pspexp-pkg upgrade` updates PSPExp too.
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

apt manages the software; the recipes manage the PSP integration. Each has
its own contract below.

```text
pspexp-pkg list                     installed recipes
pspexp-pkg available                the catalogue, with state (installed / update / new / broken)
pspexp-pkg install <id>...          the recipe's packages (apt), then its launcher files
pspexp-pkg remove <id>...           its launcher files, then the packages only it needed (apt)
pspexp-pkg update                   the catalogue (signature checked), and apt's lists
pspexp-pkg upgrade                  the packages the installed recipes use, and changed recipes
pspexp-pkg recover                  after an interruption: finish or clean up (run at every start)
```

### 5.1 Trust: the catalogue is authenticated like apt's own metadata

Recipes carry commands, data checksums and the root helper's allowlist, so
the catalogue is trusted only after the same kind of check apt makes of a
repository (apt-secure):
- **The trust root:** the project's public key,
  `/usr/share/keyrings/pspexp-archive-keyring.gpg`, installed by the image
  or the board setup. It's the only key used for PSPExp's catalogue and
  apt repository.
- **Checking a new catalogue:** the catalogue is `catalogue.tar.gz` plus a
  detached signature. The root helper fetches both into a temporary
  folder, checks the signature with `gpgv` against that keyring, and only
  then replaces the installed one.
- **Where it lives:** `/usr/share/pspexp/catalogue`, owned by root and not
  writable by anyone else. `pspexp-pkg` and the root helper read recipes
  only from there. A recipe edited elsewhere is never used.
- **No going backwards:** the signed archive carries a `serial` (build
  time). The helper refuses a catalogue older than the one installed, so an
  old signed catalogue can't be replayed to reintroduce a withdrawn recipe.
- **Checksums in recipes** (`data=... sha256=`) are trusted because the
  recipe itself is authenticated. A checksum alone would prove nothing.
- **The apt repository:** what apt verifies is the repository's signed
  metadata. `InRelease` (or `Release` + `Release.gpg`) is signed with the
  project key; it holds the hashes of the `Packages` index, which holds the
  hash of every `.deb`. The `.deb` files themselves aren't signed; they're
  trusted through that chain. The apt source names the keyring with
  `Signed-By`, so this key vouches for PSPExp's repository only, not for
  Debian's.
- **Rotating the key** comes later: a new key is shipped in a package signed
  by the old one before the old one retires.

### 5.2 Privileges: the root helper's whole contract

`pspexp-pkg` runs as the console's user. apt needs root, and gets it only
through `/usr/local/sbin/pspexp-pkg-apt`. One sudoers line allows the user
to run exactly that program, with no password. Its operations are the
complete list:

| operation | what it runs | arguments accepted |
|---|---|---|
| `update` | fetch and check the catalogue (§5.1), then `apt-get update` | none |
| `install <id>` | build and install the recipe's meta-package `pspexp-recipe-<id>` (§5.4) with `apt-get install` | a recipe id present in the trusted catalogue |
| `remove <id>` | `apt-get remove pspexp-recipe-<id>`, then remove only the packages §5.4 allows | an installed recipe id |
| `upgrade` | `apt-get install --only-upgrade` of the installed recipes' packages and `pspexp-*` | none (the list comes from the catalogue and apt's state) |
| `repair` | `dpkg --configure -a`, then `apt-get -f install` | none |

- **Package names never come from the user's side.** The helper takes
  recipe ids, which must match `[a-z0-9-]+` and exist in the root-owned
  catalogue, and reads the package names from the recipe itself.
- **Every apt call** uses `DEBIAN_FRONTEND=noninteractive`,
  `--no-install-recommends` and `-o DPkg::Lock::Timeout=120`. Output goes
  to a log. Progress is read from apt's status output
  (`APT::Status-Fd`).
- **No general `apt upgrade`.** Updating the whole Debian system is not
  PSPExp's to do. The board's owner does it, or a separate, opt-in "system
  updates" action may come later.

### 5.3 Transactions, interruptions, concurrency

A complete undo isn't possible once apt has run: dependencies change and
package scripts have run. So the contract is **forward recovery**. Every
operation either finishes, or leaves a recorded state that `recover`
finishes or cleans up, and the user sees which.

- **One operation at a time:** `pspexp-pkg` takes an exclusive lock
  (`flock` on `<root>/state/lock`). A second one, from the Packages app or
  the shell, waits or reports "busy". apt keeps its own lock, and the
  helper waits for it with a timeout.
- **A journal:** each operation writes `<root>/state/txn`, with the
  operation, the recipe id and the phase:
  1. `fetch`: pinned data downloaded into a staging folder under
     `<root>/state/stage/`, checksums checked;
  2. `apt`: the helper ran `install` / `remove`;
  3. `files`: the launcher's files are built in a staging folder, then
     swapped in with one rename per folder;
  4. `done`: the journal is removed.
- **What remains after a failure:**
  - **In `fetch`:** nothing changed; staging is deleted.
  - **In `apt`:** apt and dpkg keep their own consistency. The recipe
    shows as **broken** ("the programs were not installed completely")
    with Retry and Remove. `recover` runs the helper's `repair` first.
  - **In `files`:** the programs are installed. The launcher files are
    either the old ones or the new ones, never a mix, because of the swap.
    `recover` finishes the step.
- **Power loss:** the console runs `pspexp-pkg recover` at every start. It
  reads the journal:
  - a `dpkg` interrupted mid-unpack is completed by `repair`
    (`dpkg --configure -a`);
  - half-built staging folders and `.old` folders are removed or put back.
- **The Packages app** shows an operation's phase and result, and after a
  failure the exact recipe state (installed, broken, not installed).

### 5.4 Package ownership: apt's own records, not ours

Removing software must not rely on PSPExp's memory of what it installed.
Other tools, and the user, may have used apt since. So ownership is
expressed in apt's own terms:
- **Installing a recipe installs a tiny meta-package,
  `pspexp-recipe-<id>`.** The helper builds it on the board with
  `dpkg-deb` from the trusted recipe. Its `Depends:` lists the recipe's
  packages. Only the meta-package is marked manually installed. The
  packages it pulls in stay automatic, unless they were already installed:
  - one the user had installed themselves stays **manual**;
  - one another tool needs stays, because of that dependency.
- **Removing a recipe removes its meta-package.** Then the helper asks
  apt, with a simulated autoremove, which packages are no longer needed,
  and removes only those that are also among this recipe's own
  dependencies. It never runs a general `autoremove` that would take
  packages unrelated to PSPExp.
- **Direct apt use is respected either way:**
  - if the user later installs a recipe's program by hand, apt marks it
    manual, and removing the recipe leaves it;
  - if the user removes a recipe's program by hand, the recipe shows as
    broken, with Reinstall or Remove.

### 5.5 Launcher files: where they go and who owns them

- **Entries (games, apps):** they live in `<root>/apps/<id>/`: `package`
  (with `recipe=<id>`), the `.entry` files, covers, `run.sh` for
  first-run settings. The launcher reads them there.
- **Systems (emulators):** the launcher discovers systems only in
  `<root>/systems/`. So a system is:
  1. built in `<root>/apps/<id>/systems/<sid>.system`, the package's own
     copy;
  2. **published** by copying it to `<root>/systems/<sid>.system`, with
     `pkg=<id>` inside.
- **Ownership of published systems:** a file in `<root>/systems/` belongs
  to the package named by its `pkg=`.
  - Installing refuses to overwrite a system file owned by another package
    (a name clash is an error in the recipe).
  - Updating replaces its own files, and unpublishes those the new version
    dropped.
  - Removing unpublishes exactly the files with `pkg=<id>`.
- **The user's data is never touched by removal:** ROMs in `~/roms`, saves
  and settings in their home folder.

## 6. PSPExp's own apt repository

**What goes in, and only this:**
- `pspexp-core`: the console and launcher, the SDL layer, the emulator
  player, `pspexp-pkg`, the catalogue;
- `pspexp-runtime`: the PSP's own files (EBOOT, runtime, USB driver),
  installed on the board and copied to the PSP under the lifecycle of §6.1;
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
- **Signing:** the project key signs the repository's metadata
  (`InRelease`, `Release.gpg`) and the catalogue (§5.1). The image carries
  its public half and an apt source that names it with `Signed-By`.
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
  and `pspexp-pkg upgrade` brings it. The Packages app shows "PSPExp
  update" like any other.
- **Upgrading `pspexp-core` while its own console shows the Packages
  app:** the package's scripts never restart the console. The new files are
  in place, and the running console carries on with the old program. The
  app then says "PSPExp was updated: restart the console to use it", with a
  Restart button. The console restarts itself when the user agrees, or at
  the next boot. A restart in the middle of an install is therefore
  impossible.

### 6.1 The PSP's runtime: a lifecycle of its own

The PSP runs the runtime from its memory stick, so a new version reaches it
only by a copy over the link, and it takes effect when PSPExp is started
again on the PSP.

- **Compatibility first.**
  - The console works with any runtime newer than the first to announce
    its features in HELLO. New abilities are feature bits (as FILES and
    CLOCK are), used only when present.
  - A runtime update is therefore never required to keep working. The
    console offers it; it doesn't force it.
  - A runtime too old for a new console (a protocol change) is refused at
    HELLO with a clear message on the board. The copy below is then still
    offered, since the FILE channel is independent of the rest.
- **Staging:**
  1. The files go to the stick under temporary names
     (`EBOOT.PBP.new`, and so on) in PSPExp's folder.
  2. Each is read back and checked against the package's SHA-256 before
     anything is replaced.
- **Swapping:** the current files are renamed `.bak`, then the `.new` ones
  take their names. A small marker file (`update.txt`) records the step.
- **Recovery:**
  - **A cable pull or crash during the copy:** only `.new` files are
    affected. The next connection deletes and redoes them.
  - **An interruption during the swap:** the marker says how far it got.
    The next connection completes the swap, or restores from `.bak`.
  - **The PSP never has no runtime:** either the old or the new set is
    complete at every moment.
- **Activation:** the running runtime is unaffected. The console says
  "PSP update installed: leave PSPExp with HOME and start it again". Once
  the new runtime reports its version, the console removes the `.bak`
  files after the first successful connection.
- **Rolling back by hand:** until then the `.bak` set stays. The console
  offers "Previous PSP runtime" in System if the new one fails to connect.

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
- **Failure paths, on a board with a scratch recipe:**
  - a catalogue with a wrong signature, or an older serial, is refused and
    the installed one stays;
  - a recipe naming a missing package fails in the `apt` phase, shows as
    broken, and Remove clears it;
  - an install killed in each phase (`fetch`, `apt`, `files`), including a
    `dpkg` interrupted mid-unpack, is completed or cleaned by `recover`;
  - two operations at once: the second waits or reports busy;
  - a package the user installed by hand before stays after the recipe is
    removed, and so does one another package depends on;
  - a system file name owned by another package is refused;
  - a runtime copy interrupted by a cable pull in each step (copy, check,
    swap) leaves a complete set, and the next connection finishes it.
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
2. **`pspexp-pkg` and the privilege helper, to the contracts of §5:**
   - the signed catalogue with its serial (§5.1);
   - the helper's five operations, taking recipe ids (§5.2);
   - the lock, the journal and `recover` (§5.3);
   - meta-packages and apt-based removal (§5.4);
   - publishing systems (§5.5).

   Install OpenTyrian and NES from recipes on the Pi, then run the failure
   paths of §10.3.
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
