#!/bin/sh
# PSPExp with the launcher on a Raspberry Pi (Debian / Raspberry Pi OS,
# 64-bit), from the Mac over SSH.
# With a package repository (its address in $PSPEXP_REPO_URL or ~/.pspexp/repo-url,
# a local setting), PSPExp comes from apt like any other package:
#   the board set up for the repository (pspexp-apt-setup pi), then pspexp-core
#   (/usr/bin, /usr/lib/pspexp, /usr/share/pspexp) and its updates through
#   Apps > Packages; this script adds only the Pi's own files: the service, the
#   USB rule, the sudo rule, /opt/pspexp; older /usr/local copies are removed.
# Without one (or PSPEXP_CORE=local), the older way: the source built on the Pi
# and copied into /usr/local (/usr/local/bin, /usr/local/lib/libsdl-pspexp.so,
# /usr/local/share/pspexp/launcher).
# Either way the launcher's packages go to /opt/pspexp/{apps,systems}, owned by
# the Pi's user, and pspexp-term.service runs the launcher on the PSP at boot (USB).
#   HOST=user@host scripts/pi/install.sh          install + restart (PI= still read)
#   HOST=user@host scripts/pi/install.sh build    only build on the Pi (~/pspexp/src/build), to
#       package: scripts/packages/debs/pspexp-core/build OUTDIR arm64 user@host
#       (the source is copied to ~/pspexp/src; a checkout of the repo on the Pi is left alone)
#   HOST=user@host scripts/pi/install.sh cavestory   Cave Story (NXEngine-evo)
#       compiled on the Pi -> build/pi/bin/nxengine-evo
#   HOST=user@host scripts/pi/install.sh tyrian   OpenTyrian (SDL2) compiled on
#       the Pi from ../tyrian/opentyrian into build/pi/bin/opentyrian, which
#       the opentyrian package ships (BOARD=pi scripts/packages/pspexp-pkg-build build opentyrian)
#   HOST=user@host scripts/pi/install.sh retro    pspexp-retro and a libretro core,
#       compiled on the Pi into build/pi/bin/, for the nes package
# Needs on the Pi: sudo without a password prompt. Building there, it first
# installs what the build needs (build-essential, cmake, pkg-config,
# libusb-1.0-0-dev, libsdl2-dev, zlib1g-dev). Without a repository Apps > Packages has no
# catalogue: games come only through one (docs/Package_Manager.md §6).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PI="${HOST:-${PI:-pi@raspberrypi.local}}"

if [ "$1" = retro ]; then   # pspexp-retro and a libretro core (CORE=fceumm), compiled on the Pi
	exec "$ROOT/scripts/packages/build-retro-remote.sh" "$PI" "$ROOT/build/pi/bin"
fi

if [ "$1" = tyrian ]; then
	SRC="$ROOT/../tyrian/opentyrian"
	[ -f "$SRC/Makefile" ] || { echo "OpenTyrian source not found in $SRC (git clone https://github.com/opentyrian/opentyrian)" >&2; exit 1; }
	{ COPYFILE_DISABLE=1 tar --no-xattrs -C "$SRC" --exclude .git --exclude obj --exclude opentyrian -cf - . ; } 2>/dev/null |
		ssh "$PI" 'rm -rf ~/pspexp/tyrian-src && mkdir -p ~/pspexp/tyrian-src && tar -C ~/pspexp/tyrian-src -xf -'
	ssh "$PI" 'set -e; make -C ~/pspexp/tyrian-src -j4 WITH_NETWORK=false 2>&1 | grep -E "error|implicit" || true
		ls -la ~/pspexp/tyrian-src/opentyrian'
	mkdir -p "$ROOT/build/pi/bin"; scp -q "$PI:pspexp/tyrian-src/opentyrian" "$ROOT/build/pi/bin/opentyrian"
	echo "build/pi/bin/opentyrian: package it with BOARD=pi scripts/packages/pspexp-pkg-build build opentyrian"
	exit 0
fi

if [ "$1" = cavestory ]; then
	# NXEngine-evo (../cavestory/nxengine-evo) compiled on the Pi, portable
	# (data/ next to the binary), 480x272 by default -> build/pi/bin/nxengine-evo
	# (the game data is made on the Mac: cavestory_data in the recipes' _lib.sh)
	SRC="$ROOT/../cavestory/nxengine-evo"
	[ -f "$SRC/CMakeLists.txt" ] || { echo "NXEngine-evo source not found in $SRC (git clone https://github.com/nxengine/nxengine-evo)" >&2; exit 1; }
	. "$ROOT/scripts/packages/recipes/_lib.sh"
	{ COPYFILE_DISABLE=1 tar --no-xattrs -C "$SRC" --exclude .git --exclude build -cf - . ; } 2>/dev/null |
		ssh "$PI" 'rm -rf ~/pspexp/cavestory-src && mkdir -p ~/pspexp/cavestory-src && tar -C ~/pspexp/cavestory-src -xf -'
	ssh "$PI" "set -e; cd ~/pspexp/cavestory-src; $(cavestory_patch)
		mkdir -p build && cd build
		cmake .. -DCMAKE_BUILD_TYPE=Release -DPORTABLE=ON > ../cmake.log 2>&1 || { tail -20 ../cmake.log; exit 1; }
		make -j4 nx > ../make.log 2>&1 || { grep -E 'error' ../make.log | head -20; exit 1; }"
	mkdir -p "$ROOT/build/pi/bin"
	scp -q "$PI:pspexp/cavestory-src/build/nxengine-evo" "$ROOT/build/pi/bin/nxengine-evo"
	echo "build/pi/bin/nxengine-evo: package with BOARD=pi scripts/packages/pspexp-pkg-build build cavestory"
	exit 0
fi

REPO_URL="${PSPEXP_REPO_URL:-$(head -1 "$HOME/.pspexp/repo-url" 2>/dev/null)}"
[ -n "$REPO_URL" ] || REPO_URL=$(head -1 "$ROOT/scripts/packages/public-repo-url" 2>/dev/null)   # else the public repository
[ -n "$REPO_URL" ] || PSPEXP_CORE=local
[ "$1" = build ] && PSPEXP_CORE=build

if [ "${PSPEXP_CORE:-apt}" != apt ]; then
	# the tracked files as they are in this tree (uncommitted edits included), built on the Pi
	(cd "$ROOT" && git ls-files -z --cached --others --exclude-standard | COPYFILE_DISABLE=1 xargs -0 tar --no-xattrs -cf - 2>/dev/null) |
		ssh "$PI" 'mkdir -p ~/pspexp/src && tar -C ~/pspexp/src -xf -'
	# the copy has no git: its version (About) goes with it, worked out here
	VF=$(mktemp); rm -f "$VF"
	cmake -DSRC="$ROOT" -DOUT="$VF" -P "$ROOT/cmake/pspexp_version.cmake" >/dev/null 2>&1
	V=$(sed -n 's/^#define PSPEXP_VERSION "\(.*\)"$/\1/p' "$VF" 2>/dev/null); rm -f "$VF"
	[ -n "$V" ] && echo "$V" | ssh "$PI" 'cat > ~/pspexp/src/.pspexp-version'
	# what the build needs (a fresh Raspberry Pi OS has none of it)
	ssh "$PI" 'sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends build-essential cmake pkg-config \
		libusb-1.0-0-dev libsdl2-dev zlib1g-dev >/dev/null'
	ssh "$PI" 'set -e; cd ~/pspexp/src
		cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
		cmake --build build -j4 --target pspexp-term pspexp-ms pspexp-capture sdl-pspexp sdl-pspexp-hostgl sdl-pspexp-sdl12 pspexp-bench pspexp-sprite-demo 2>&1 | grep -E "error|warning:" || true
		[ -x build/pspexp-term ] && [ -x build/pspexp-capture ] && [ -f build/libsdl-pspexp.so ]'
	if [ "$PSPEXP_CORE" = build ]; then
		echo "built on $PI (~/pspexp/src/build): package it with scripts/packages/debs/pspexp-core/build OUTDIR arm64 $PI"
		exit 0
	fi
	[ -n "$REPO_URL" ] || echo "no package repository address (\$PSPEXP_REPO_URL or ~/.pspexp/repo-url): PSPExp from this build, into /usr/local; Apps > Packages has no catalogue to offer without one (docs/Package_Manager.md §6)"
	ssh "$PI" 'set -e; S=~/pspexp/src
		sudo install -d -m 0755 /usr/local/bin /usr/local/lib /usr/local/share/pspexp/launcher
		for f in pspexp-term pspexp-capture pspexp-bench pspexp-sprite-demo; do sudo install -m 0755 $S/build/$f /usr/local/bin/$f.new && sudo mv /usr/local/bin/$f.new /usr/local/bin/$f; done
		sudo install -m 0755 $S/scripts/board/pspexp-exec /usr/local/bin/pspexp-exec
		sudo install -m 0755 $S/scripts/board/pspexp-run /usr/local/bin/pspexp-run
		sudo install -m 0755 $S/scripts/board/pspexp-pkg /usr/local/bin/pspexp-pkg
		sudo install -m 0755 $S/scripts/board/pspexp-bt /usr/local/bin/pspexp-bt
		sudo install -m 0644 $S/build/libsdl-pspexp.so /usr/local/lib/libsdl-pspexp.so.new && sudo mv /usr/local/lib/libsdl-pspexp.so.new /usr/local/lib/libsdl-pspexp.so
		# the board-GPU layer (pspexp-run PSPEXP_HOSTGL=1: SuperTuxKart, Celeste 64...), as the package installs it
		if [ -f $S/build/libsdl-pspexp-hostgl.so ]; then sudo install -m 0644 $S/build/libsdl-pspexp-hostgl.so /usr/local/lib/libsdl-pspexp-hostgl.so.new && sudo mv /usr/local/lib/libsdl-pspexp-hostgl.so.new /usr/local/lib/libsdl-pspexp-hostgl.so; fi
		if [ -f $S/build/libsdl-pspexp-sdl12.so ]; then sudo install -m 0644 $S/build/libsdl-pspexp-sdl12.so /usr/local/lib/libsdl-pspexp-sdl12.so.new && sudo mv /usr/local/lib/libsdl-pspexp-sdl12.so.new /usr/local/lib/libsdl-pspexp-sdl12.so; fi
		for f in Inter-Regular.ttf Inter-SemiBold.ttf lucide.ttf Inter-LICENSE.txt lucide-LICENSE.txt; do
			sudo install -m 0644 $S/tools/pspexp-term/assets/$f /usr/local/share/pspexp/launcher/$f
		done
		sudo mkdir -p /usr/local/share/pspexp/launcher/systems && sudo cp $S/tools/pspexp-term/assets/systems/* /usr/local/share/pspexp/launcher/systems/'
	CONSOLE=/usr/local/bin/pspexp-term
else
	# the board set up for the repository, then PSPExp itself from apt
	W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
	gpg --dearmor < "$ROOT/scripts/packages/pspexp-archive-key.asc" > "$W/keyring.gpg"
	scp -q "$W/keyring.gpg" "$ROOT/scripts/board/pspexp-apt-setup" "$ROOT/scripts/board/pspexp-pkg-apt" "$PI:/tmp/"
	ssh "$PI" "set -e; sudo sh /tmp/pspexp-apt-setup pi \$(id -un) '$REPO_URL' /tmp/keyring.gpg /tmp/pspexp-pkg-apt
		sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends pspexp-core >/dev/null
		rm -f /tmp/keyring.gpg /tmp/pspexp-apt-setup /tmp/pspexp-pkg-apt
		# copies from the older way would come first in PATH and shadow the package's
		for f in pspexp-term pspexp-capture pspexp-bench pspexp-sprite-demo pspexp-exec pspexp-run pspexp-pkg pspexp-bt pspexp-ms; do sudo rm -f /usr/local/bin/\$f; done
		sudo rm -rf /usr/local/lib/libsdl-pspexp.so /usr/local/lib/libsdl-pspexp-hostgl.so /usr/local/lib/libsdl-pspexp-sdl12.so /usr/local/share/pspexp/launcher
		dpkg-query -W pspexp-core pspexp-runtime"
	CONSOLE=/usr/bin/pspexp-term
fi

# the Pi's own files (scripts/pi/board-files.sh, as the card image has them: the package
# folders, the sudo rule for the System page, the PSP's USB rule, the service); the
# running console is swapped for the new one
ssh "$PI" "sudo sh -s \$(id -un) $CONSOLE" < "$ROOT/scripts/pi/board-files.sh"
ssh "$PI" 'sudo udevadm control --reload-rules || true; sudo systemctl daemon-reload; sudo systemctl restart pspexp-term; sleep 1; systemctl is-active pspexp-term'
echo "PSPExp installed on $PI ($CONSOLE); the console restarted with the launcher"
