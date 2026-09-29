# PSPExp packages: recipes over the board's own package manager

*Design, reviewed on 2026-09-28 (findings closed at the design level): decisions in §11; operational contracts in §5.1-5.6 and §6.1. Implementation and hardware failure tests must still show these guarantees hold. Nothing here is implemented yet except what "Today" describes.*

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

- **Any section can change per board:** `[package:omega]` gives the Omega2+
  its own `description`, for instance, when it plays the title with a
  different core.
- **The Omega2+:** `[omega]` names our static build (`build=`, made by
  `scripts/packages/recipes/ID.sh`), and `warn=` says what the board can't
  do well; the Packages app shows it first ("Too heavy for the Omega2+: ...").
  A recipe without `[omega]` isn't offered there.
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
| `upgrade` | for each installed recipe: its meta-package rebuilt when the recipe changed and installed with `apt-get install` (new dependencies come in); then `apt-get install --only-upgrade` of the packages the installed meta-packages depend on, and `pspexp-*` (§5.6) | none (the list comes from the catalogue and apt's state) |
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
  (`flock` on `<root>/state/lock`), held for the whole operation, recovery
  included. A second one, from the Packages app or the shell, waits or
  reports "busy". apt keeps its own lock, and the helper waits for it with
  a timeout.
- **The journal:** `<root>/state/txn`, rewritten through a temporary file
  and a rename at each checkpoint. It names:
  - the operation, the recipe id, the recipe version and the catalogue
    serial it is carrying out;
  - the checksum manifest of what was staged;
  - the last checkpoint reached.

  `recover` works from the journal and the staged files alone, never from
  the catalogue, which may have changed meanwhile.
- **Every action is bracketed by two checkpoints:** `<action>-started`
  is written before it begins, and `<action>-done` after it ends. A crash
  can fall between the action and its `-done`. So recovery never trusts
  the last checkpoint alone at a `-started`; it **reconciles**, inspecting
  the real state (dpkg's records, the files on disk) to learn whether the
  action happened.
- **Staging is kept until the operation ends.** It is deleted only at
  `done`, or when the user abandons a broken operation (Remove), or when
  recovery finds that nothing past `staged` began.
- **Checkpoints, in this order:**
  1. **`staged`:** everything the operation will publish is built under
     `<root>/state/stage/<id>/`, and its manifest is recorded:
     - pinned data (checksums checked);
     - the new app folder;
     - the new system files;
     - the list of system files to unpublish;
     - the meta-package `.deb`.
  2. **`apt-started`,** written before the helper is called, then
     **`apt-done`** after it succeeds. apt and dpkg keep their own
     consistency.
  3. **`publishing`:** a marker `<root>/state/publishing` is created before
     the first visible change. Its steps are bracketed the same way:
     1. `app-swap-started` / `app-swapped`: `apps/<id>` renamed to
        `apps/.<id>.old`, then the staged folder renamed to `apps/<id>`;
     2. `systems-started` / `systems-published`: each new system file
        written as `.tmp`, then renamed into `systems/`, and dropped ones
        deleted.
  4. **`done`:** `.old` deleted, the marker removed, the journal removed.
- **The launcher never sees a half-published install.** While
  `<root>/state/publishing` exists, it doesn't rescan its library; it keeps
  showing what it showed and rescans once the marker is gone. That's a
  small change to the launcher's library watcher.
- **Recovery, by checkpoint** (the journal names the operation, recipe
  version and catalogue serial, and the manifest):

  | last checkpoint | how `recover` reconciles | outcome |
  |---|---|---|
  | none, or `staged` | nothing past staging began | staging deleted; nothing changed |
  | `apt-started` | runs `repair` if dpkg reports anything half-installed, then asks dpkg whether the journal's meta-package is in the state the operation wanted: installed and configured at the journal's version, or absent for a removal | yes: continues at publishing; no: the recipe shows as **broken** (Retry uses the kept staging; Remove abandons it) |
  | `apt-done` | none needed | publishes from staging |
  | `app-swap-started` | inspects `apps/<id>` and `apps/.<id>.old` against the manifest to see which renames happened | finishes the swap, or undoes the half of it that happened |
  | `app-swapped`, `systems-started` | compares each system file in `systems/` with the manifest; every step can be redone | finishes publishing |
  | a manifest that doesn't check | staging is damaged | rolls back: `.old` returns, and its own system files are republished; the recipe shows as broken |

- **Power loss:** the console runs `pspexp-pkg recover` at every start,
  under the lock, before the launcher first reads its library. A `dpkg`
  interrupted mid-unpack is completed by `repair` (`dpkg --configure -a`).
- **The Packages app** shows an operation's phase and result, and after a
  failure the recipe's exact state: installed, broken, or not installed.

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

### 5.6 When a recipe changes: upgrading its meta-package

A recipe's meta-package carries the recipe's dependencies, so a changed
recipe needs a new meta-package, not just newer packages.

- **Versioning:** `pspexp-recipe-<id>` has version `<recipe version>`, the
  recipe's own counter. The catalogue changing a recipe means a higher
  number.
- **`upgrade`, for each installed recipe with a newer version in the
  catalogue:** one transaction of §5.3 with the operation `upgrade`:
  1. **`staged`:** the new app folder and system files, and the new
     meta-package (same name, new version, new `Depends:`), built from the
     new recipe.
  2. **`apt-done`:** `apt-get install ./pspexp-recipe-<id>_<new>.deb`. A
     plain install, not `--only-upgrade`, so dependencies the new recipe
     adds (B, replacing A) are installed. Then the limited removal of §5.4:
     packages that were dependencies of the old meta-package and that apt
     now reports unneeded (A) are removed.
  3. **Publishing and `done`,** as for an install. The launcher files for B
     appear only after B is installed.
- **Then the packages themselves:** `apt-get install --only-upgrade` of the
  packages the installed meta-packages depend on, plus `pspexp-*`. These
  are all installed already, by construction, so `--only-upgrade` skips
  none of them.
- **A recipe removed from the catalogue:** its install stays as it is
  (nothing breaks). The app marks it "no longer offered", with Remove.

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

The PSP runs PSPExp from its memory stick. A new version reaches it only by
a copy over the link, and takes effect when PSPExp is started again. The
update must never be able to leave the PSP unable to connect, since the
connection is what repairs it. So recovery never depends on the component
being replaced.

**Three layers, updated differently:**
- **The boot loader** (`EBOOT.PBP`: `pspexp_boot.prx` and the XMB
  pictures). It's small, and changes almost never. Its only jobs are to
  choose a slot and load the USB driver and runtime from it, and it's
  never replaced by the automatic update (below).
- **Two complete slots:** `ms0:/PSP/GAME/PSPExp/slot-a/` and `slot-b/`.
  Each holds a whole runtime set: `pspexp_runtime.prx`, `pspexp_usb.prx`,
  and a `VERSION` file with the set's version and checksums.
- **The choice:** `active`, a small file naming the slot to boot (`a` or
  `b`), plus a `tries` counter.

**What makes a slot valid:** its `VERSION` file lists every file of the
set with its size and SHA-256. A slot is valid only when:
- `VERSION` is present and well-formed, ending with the line `end` (a
  `VERSION` cut short never has it);
- every file it lists is present;
- each file's size and SHA-256 match.

The loader checks this in full before booting a slot, every time, whether
it was chosen by `active`, by fallback, or forced with L. A slot that fails
is never booted. Hashing the whole set, about 250 KB, adds a fraction of a
second.

**Updating (always the inactive slot):**
1. **Invalidate first:** delete the inactive slot's `VERSION`, so a
   previous release's marker can never vouch for half-written files. Only
   then write anything.
2. Copy the new set into the inactive slot, over the FILE channel.
3. Read every file back and check it against the `pspexp-runtime`
   package's SHA-256. Write `VERSION` last, as its "complete" mark, and
   read it back too.
4. Switch: write `active.new` naming the updated slot with `tries=0`, and
   read it back. A complete, valid `active.new` is the only switch point:
   the PSP's FAT can't rename over an existing file, so the loader, at the
   next start, makes it `active` (remove, then rename) once it has checked
   the slot it names. An `active.new` cut short, or naming a slot that
   doesn't check, is removed and never chosen. If the link breaks around
   the switch, the next connection reads `active` and `active.new` back to
   learn which side of it it's on.
5. The console says "PSP update installed: leave PSPExp with HOME and start
   it again".

The running runtime is untouched throughout. A power loss or cable pull at
any moment leaves one complete, bootable slot (the old one), plus possibly
a half-written inactive slot that nothing boots.

**Booting, with automatic fallback:**
- The loader reads `active`. A missing or unreadable one means the first
  valid slot, `a` first, in the full sense above.
- It raises `tries` before loading.
- The runtime resets `tries` to 0 once it has a working connection to a
  host.
- If `tries` reaches 3, or the chosen slot isn't valid, or its modules
  fail to load, the loader boots the other slot, if that one is valid, and
  records the fallback. With neither valid, it shows a message pointing to
  the manual route. The console then reports "the new PSP runtime did not start;
  the previous one is running".
- Holding **L** while PSPExp starts forces the other slot, by hand.

**The protocol's frozen core, and maintenance mode:** so that any console
can talk to any runtime at least enough to repair it, these are frozen and
never changed incompatibly:
- the stream framing (the message header, whose `version` byte means the
  framing);
- the first fields of HELLO: the protocol version and the feature bits;
- the FILE channel's messages. New file operations are new opcodes, never
  changed ones.

With those frozen:
- **A matching protocol version:** a normal connection.
- **A different protocol version, when the runtime has
  `PSPEXP_FEATURE_FILES`:** the host doesn't reject the HELLO as today. It
  answers with a HELLO_ACK marked **maintenance**. The link is then
  connected with only SYSTEM (HELLO, DISCONNECT, ERROR) and FILE in use,
  and no graphics. The console uses it only to install a matching runtime
  into the inactive slot, then asks for a restart.
- **A different version, without FILES:** rejected as today, with the
  manual route below.

This is a change to today's connection contract (today, a mismatch is
rejected and file operations need a normal connection). It's part of
implementing this section.

**The manual route, needing nothing of PSPExp.** When nothing else works, a
new loader included, the PSP's own **USB mode in the XMB** (Settings > USB
Connection) shows its memory stick to any computer. Copy a known-good
`dist/PSPExp` folder to `ms0:/PSP/GAME/PSPExp/`. This is the documented
last resort, and the only way the boot loader itself is replaced.

**The console's side:** after a successful connection to an updated slot,
the console keeps the other slot as it is (the previous version, for the
fallback) until the next runtime update overwrites it.

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

**Built (2026-09-29):** `scripts/gameshell/base/build-image.sh` makes it:
- **Debian 13 armhf, as small as the board allows:** 254 packages
  (ClockworkOS-era card: 359), 519 MB used; Chocolate Doom, htop, the sound
  tools, ping, iptables, unzip and bash completion left out.
- **PSPExp from its apt repository:** the build runs `pspexp-apt-setup
  gameshell` in the image (key, source, root helper, sudo rule, the signed
  catalogue) and installs `pspexp-core` (with `pspexp-runtime`); the
  GameShell's own files come from `stage.sh` with `GS_CORE=apt`. The console
  service is enabled: a PSP running PSPExp connects on USB, or on the
  GameShell's own access point, with nothing to set up.
- **U-Boot from mainline source** (`scripts/gameshell/base/build-uboot.sh`,
  U-Boot 2026.07): ClockworkPi never published the source of theirs, so the
  board's settings were found from it. Its SPL holds 1200000000 (the DRAM
  PLL at twice the memory clock) and not 864000000, and a running board's
  PLL_DDR1 is at 1200 MHz: DRAM 600 MHz; ZQ and ODT as the community
  configs have them; the AXP223's voltages as the running board reports
  them (`uboot.config`). One small patch hands the orange charge LED to the
  charger (REG 32H bit 3, as the datasheet has it); the boot script and the
  device tree light the green one (PB7). Booted on a GameShell: the green
  LED on, the PSP connected; the orange one lights while charging.
- **The release build, as a user gets it** (no SSH key, no Wi-Fi), tried end
  to end on a GameShell (2026-09-29): the PSP connected at first boot, green
  LED on; Wi-Fi joined and the time zone chosen from the PSP; the default
  password let an SSH key in; the Wi-Fi password never reached the log; all
  nine recipes installed through apt from a card with no apt lists (900 MB
  used after); the real-PSP suite passed 38 of 38, the Packages app removing
  and reinstalling Cave Story itself, every system at full speed.
- **Nothing from ClockworkOS any more:** the Wi-Fi chip's firmware is
  Debian's (firmware-brcm80211, 7.13.53.9; Wi-Fi joined and worked). Its
  settings file was never ClockworkPi's own: it is Ampak's generic AP6212
  NVRAM (as Armbian and LibreELEC carry it, one line added), which
  linux-firmware carries under GPL-2.0; it is in `scripts/gameshell/base`.
- **Bluetooth, which the board has and our base had never enabled:** the
  kernel's hci_uart with the Broadcom protocol, the AP6212's Bluetooth node
  on UART1 in the device tree (wired as on the Banana Pi M2M: PL8, PL9,
  PL10, as ClockworkOS drives them), BlueZ in the image, and the
  firmware patch as `brcm/BCM4343A0.hcd`: not in linux-firmware or Debian,
  but ClockworkPi publishes it under GPL-3.0 (github.com/clockworkpi/bluetooth),
  so the build fetches it from there, pinned to a commit and checked by
  SHA-256. Armbian's AP6212 patch (build 0242) was tried too: it doesn't
  read the chip's own address, so BlueZ refused the controller;
  ClockworkPi's (build 0253) does. On the GameShell: patch loaded, the controller's own address,
  a scan found 8 devices.
- **The final release build** (nothing from ClockworkOS, Bluetooth on, no
  SSH key) tried end to end the same way: Bluetooth up at first boot (7
  devices in a 10 s scan), all nine recipes through apt, the real-PSP suite
  38 of 38.
- **Tried on a GameShell:** first boot grew the card and finished; the PSP
  connected and got its clock; Wi-Fi joined from System > Wi-Fi network;
  all nine recipes installed through apt (903 MB used after).
- **Found on it:** the image ships no apt lists, so the first install found
  nothing; the root helper now fetches them when missing. The launcher's
  Wi-Fi join passed the password to `nmcli` as an argument, which `sudo`
  logged to the journal: the sudo rule now keeps `nmcli` out of the log
  (`Defaults!/usr/bin/nmcli !log_allowed`).

## 9. The Omega2+

OpenWrt has none of these programs and the Omega has no compiler.

It keeps our own static builds, as archives, but reads the **same recipes**:
- **A recipe's `[omega]` section** names our build (`build=nes`), and may
  warn (`warn=`) when the board plays it badly.
- **Publishing, from the Mac over SSH:** `scripts/packages/pspexp-pkg-build
  build ID...` makes the archives; `publish` puts them in `<root>/repo` and
  the same catalogue the Debian boards get (`build/apt/catalogue.tar.gz`,
  refused when the recipes changed since) in `<root>/catalogue`. SSH from the
  Mac is the trusted path, so the Omega checks no signature of its own (it
  has no gpgv).
- **Installing:** `pspexp-pkg` (`PSPEXP_BOARD=omega`, set by its wrapper)
  installs a recipe from the build it names, as any archive, and records the
  recipe, its version, the catalogue and the build in the package file.
  Installed, Update and Get come from that record against the published
  build's version. No network is needed: `update` only reports the
  catalogue the Mac last published.
- **The Packages app** is the same as on the Debian boards; it finds the
  catalogue in `<root>/catalogue`.

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
  - an operation killed at each checkpoint of §5.3 (`staged`,
    `apt-started`, `apt-done`, `app-swap-started`, `app-swapped`,
    `systems-started`), and **between each action and the recording of its
    `-done`**:
    - after apt succeeded, before `apt-done`;
    - between the two renames of the app swap, and after them, before
      `app-swapped`;
    - after a system file's rename, before `systems-published`;
    - a `dpkg` interrupted mid-unpack.

    `recover` must reach the outcome the table promises, and the kept
    staging must let Retry finish;
  - two operations at once: the second waits or reports busy;
  - a package the user installed by hand before stays after the recipe is
    removed, and so does one another package depends on;
  - a system file name owned by another package is refused;
  - a runtime update interrupted in each step (invalidate, copy, check,
    `VERSION`, switch), and between writing `active.new` and the console's
    reading it back, always leaves a complete, verified slot that
    boots:
    - the old one, when the interruption came before the switch;
    - the new one, when it came after, with the old one kept for fallback;
  - a slot with a stale `VERSION` and one damaged file is never booted:
    not by `active`, not by fallback, not with L;
  - a runtime that doesn't connect: `tries` reaches its limit and the
    loader falls back to the other slot; holding L forces it;
  - a console with a different protocol version reaches a FILES runtime in
    maintenance mode and installs a matching runtime;
  - a recipe changing its dependency from A to B: B is installed before
    the new launcher files appear, and A is removed only if apt no longer
    needs it;
  - an install killed at each sub-checkpoint of publishing: `recover`
    rolls forward, or rolls back to exactly the old install, and the
    launcher never lists a half-published one.
- **On a board, via `tests/psp/run.py`:**
  - the apps suite: Packages lists the catalogue;
  - `--store-writes`: install and remove one small recipe through apt, check
    the launcher entry appears and disappears, and check the game starts
    (games suite).

## 11. Decisions needed

1. ~~**Hosting**~~ *Decided:* a home Linux server, on the home network,
   until the whole project is published; a public host comes then. It
   serves `~/pspexp-repo` with a small user service of its own, a static
   server on a free port (`loginctl enable-linger` once, so it runs without
   a login). Its address is a local setting (`~/.pspexp/repo-host` on the
   Mac, `/etc/pspexp/repo` on the boards), never in the repository.
2. ~~**Signing key**~~ *Decided:* a project GPG key created on the Mac and
   used by the build scripts; a backup copy kept safe by the owner. Boards
   and the image carry only its public half.
3. ~~**Root**~~ *Decided:* the narrow sudoers rule for `pspexp-pkg-apt`,
   with exactly the five operations of §5.2 (update, install, remove,
   upgrade, repair), taking recipe ids only.
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

**Status (2026-09-29):**
- **Steps 1 to 3 are done on the Pi 400:**
  - recipes and their check;
  - `pspexp-pkg` and the root helper to the contracts of §5, with 28
    failure-path checks passing (`tests/pkg/run.py`);
  - the Packages app on the catalogue (sections, states, progress), tried
    on the real PSP.
- **The repository** is signed and served from the home server. The Pi's
  installer sets the board up when `~/.pspexp/repo-url` exists on the Mac.
- **Found along the way:** Debian's libretro cores accept "retroarch or any
  libretro frontend", and Freedoom accepts "dsda-doom or any Doom engine".
  With both choices new in one install, apt takes its first choice. So the
  helper installs a recipe's packages one at a time, in the order the
  recipe lists them (`pspexp-retro`, or `chocolate-doom`, first).
- **Step 4 is done on the Pi 400:**
  - **Our packages:** `pspexp-core` (the console, SDL layer, tools, root
    helper, fonts), `pspexp-cavestory`, `jazz-shareware-data`,
    `tyrian-data` and `pspexp-retro`, built on the Mac by
    `scripts/packages/debs/*/build` and published with `apt-repo`.
  - **The catalogue** has nine recipes: OpenTyrian, Freedoom (two
    entries), Cave Story, Jazz, Simutrans, NES, Game Boy (GB and GBC), Mega
    Drive and SNES. All installed through apt on the Pi, without RetroArch
    or dsda-doom.
  - **They play on the real PSP:** the games suite passes 11 of 11, at the
    same speeds as the older builds.
  - **The Pi's PSPExp is now `pspexp-core`** from apt; its old
    `/usr/local` copies are gone.
  - **A self-update, end to end:** `pspexp-pkg upgrade` installed a newer
    core; the running console noticed and offered System > Restart PSPExp;
    choosing it started the new version.
  - **The Pi installer only builds** once the Pi has `pspexp-core`
    (`PSPEXP_CORE=apt`, detected automatically).
- **Found by that test:** the launcher's System entries lived in a fixed
  array of 8. The PSP clock setting had already made 9 on the GameShell
  (with its Wi-Fi mode entry), and "Restart PSPExp" made 9 on the Pi. The
  overflow corrupted the section names. Now it's 16, with a guard.

- **Step 5, in the PPSSPP emulator and partly on the real PSP:**
  - **The boot loader** keeps two slots next to the EBOOT, checks a slot's
    whole set against its `VERSION` (SHA-256, shared with the console:
    `protocol/pspexp_sha256.c`) before booting it, counts attempts in
    `active`, falls back after 3 or on a slot that doesn't check (recorded
    in `fallback`), and boots the other slot while L is held. With no slot
    files at all it loads the runtime next to the EBOOT, as before.
  - **The runtime** sets `tries` back to 0 once it's connected.
  - **Maintenance mode:** a console speaking another protocol version
    answers a runtime that has FILE with a HELLO_ACK marked maintenance;
    only SYSTEM and FILE are used then, and the console installs its own
    runtime into the inactive slot.
  - **The console's side:** `pspexp_slots_update` (the four steps above,
    each file read back and checked), `pspexp-ms slots` / `runtime-update
    DIR`, and in the launcher System > Update the PSP, offered when the
    `pspexp-runtime` package holds a newer set than the PSP's.
    `pspexp-term --ctl runtime-update` does the same for tests.
  - **The package:** `pspexp-runtime` (arch all) puts the set in
    `/usr/share/pspexp/runtime`, with the loader for the manual route.
  - **The tests:** `tests/psp/slots-ppsspp.sh` runs the real loader and
    runtime in PPSSPP through every case of §10.3's runtime paths: an
    update and the switch, a stop after each of the update's six steps,
    three starts without a connection, a stale `VERSION` over a damaged
    file, an `active.new` naming that slot, and maintenance mode. Holding
    L can't be scripted in the emulator: it's a real-PSP check.
  - **On the real PSP (PSP-1000 on the Pi 400, over USB):** the slotted
    loader was installed once by the manual route and booted slot a. Then:
    - the Pi upgraded `pspexp-core` and `pspexp-runtime` through apt; its
      console offered System > Update the PSP, which filled slot b; the next
      start booted slot b and the runtime confirmed it;
    - holding L at start booted slot a;
    - one byte changed in slot b's runtime under its old `VERSION`: the
      loader refused slot b, booted slot a, wrote `from=b to=a reason=not
      valid`, and the console reported it and offered the update again,
      which repaired slot b.
  - **Found on the PSP:** once the update was done, "Update the PSP" left
    the System list and the cursor slid onto Restart; the launcher now keeps
    the cursor on the entry above one that disappears. The notice was too
    long for the screen and is shorter. Each new console reported the same
    fallback again: the console now removes `fallback` from the stick once
    it has reported it (its log keeps the record).

- **Step 6, on the GameShell (without the PSP so far):**
  - **armhf packages:** `pspexp-core`, `pspexp-retro` and `pspexp-cavestory`
    from the Zig cross build (`scripts/gameshell/build.sh`), in the same
    repository as the Pi's.
  - **Cores Debian armhf lacks:** Debian 13 armhf has no Mega Drive
    (Genesis Plus GX) or SNES (Snes9x) core. `pspexp-libretro-picodrive`
    and `pspexp-libretro-snes9x2005` (`scripts/packages/debs/pspexp-libretro`)
    put the GameShell's lighter cores in Debian's libretro folder, each with
    its licence, and the two recipes choose them in `[debian:gameshell]`.
    NES (Nestopia) and Game Boy (Gambatte) are Debian's, as on the Pi.
  - **The installer:** with a repository address set,
    `scripts/gameshell/install.sh` runs `pspexp-apt-setup gameshell`,
    installs `pspexp-core` from apt, and stages only the GameShell's own
    files (Wi-Fi mode, first boot, the service now on `/usr/bin/pspexp-term`),
    removing the `/usr/local` copies. The setup installs `gpgv` when a card
    lacks it.
  - **All nine recipes installed through apt** (26 to 41 s each), replacing
    the archive installs of the same names; no RetroArch or dsda-doom.
  - **The failure suite** (`tests/pkg/run.py`) passes 28 of 28 on the
    GameShell.
  - **The Pi's installer does the same now:** with a repository address it
    sets the Pi up, installs `pspexp-core` from apt and adds only the Pi's
    own files; `scripts/pi/install.sh build` only builds, to package the
    arm64 core. Run on the Pi 400 (already moved by hand in step 4), it
    changed nothing but two stale developer tools in `/usr/local/bin`.
  - **With the PSP on the GameShell:** the whole real-PSP suite passes
    (37 checks). Games: Cave Story 49 fps (the game's own 50), Freedoom 35,
    Simutrans 24, and every system at full speed: Game Boy and Game Boy
    Color 60, Mega Drive (PicoDrive) 59, NES (Debian's Nestopia) and SNES
    (snes9x2005) 50 with European ROMs. Debian's Nestopia is enough, so no
    QuickNES package.
  - **Found there:** Cave Story's package starts the game through a shell
    script, and the GameShell's SDL layer, built without naming SDL2 as a
    library it needs, could not load into that shell. It's now linked
    against a stand-in SDL2 naming the functions it calls, like the Pi's
    build, so it loads anywhere and resolves them from the board's SDL2.
  - **A runtime update from the GameShell:** a newer `pspexp-runtime`
    reached it through apt, its console offered System > Update the PSP,
    and the update filled slot a while the PSP ran slot b; the next start
    booted slot a and the runtime confirmed it. Step 6 is done.

- **Step 7:**
  - **The Omega2+ on recipes:** eight recipes gained `[omega] build=` (all
    but Simutrans, which the Omega can't run), Mega Drive and SNES with a
    warning; the systems' descriptions name the Omega's cores through
    `[package:omega]` (and the GameShell's, which also differ, through
    `[package:gameshell]`). On the Omega, `pspexp-pkg-build publish` put the
    builds and the catalogue on it; all eight installed from their recipes,
    replacing the archive installs; remove and reinstall work. The apps
    suite, the Omega's console on a stand-in PSP (the Mac's
    `pspexp-sdl-server`), passes 4 of 4, the Packages app removing and
    reinstalling Cave Story itself. (`tests/psp/run.py --term` names a
    console off the PATH, as the Omega's is.) The failure suite still
    passes 28 of 28 on the Pi with the new lock. With the real PSP on the
    Omega the apps suite passes 4 of 4 too, the warnings showing in
    Emulators. Step 7 is done.
  - **Found there:** BusyBox's `flock` has no `-w`, so every operation said
    "busy"; the lock now retries `flock -n` each second (both kinds of
    board). GNU tar as root can't set owners on the FAT card
    (`--no-same-owner`). `upgrade` and `recover` exited 1 on the Omega.
  - **Archives retired from the Debian boards:** `pspexp-pkg-build` builds
    for the Omega only; the build scripts lost their GameShell and Pi
    branches (Simutrans's script and `build-simutrans.sh` went: Debian's
    Simutrans replaced them); the Packages app lost its folder and web
    repositories and reads only the catalogue; the Pi's old repository
    folder is gone. Titles installed from an archive stay removable.
  - **Not yet:** the GameShell card image's first boot still installs the
    bundled archives; it moves to recipes with the image work.

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
5. **The PSP runtime's lifecycle (§6.1), before any runtime is delivered
   by package:**
   - the boot loader's two slots, `active` / `tries` and fallback, and the
     L override;
   - the protocol's frozen core and maintenance mode in HELLO / HELLO_ACK;
   - the console's slot update over the FILE channel.

   The real PSP gets the new loader once, by the manual route. Then run the
   runtime failure paths of §10.3.
6. **The GameShell:** the same on armhf, and lighter cores where the games
   suite says so.
7. **Every recipe converted,** and the old archive packages retired. The
   Omega keeps its builds, read through recipes.
8. **Publishing, when the project goes public:** the same files on a public
   host, and the image built with only the core, the catalogue and the
   public address.
