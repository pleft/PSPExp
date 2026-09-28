#!/bin/sh
# PSPExp with the launcher on a Raspberry Pi (Debian / Raspberry Pi OS,
# 64-bit), from the Mac over SSH. Same layout as the GameShell's, so the
# launcher's packages (scripts/packages/pspexp-pkg-build, BOARD=pi) install the same way:
#   /usr/local/bin      pspexp-term, pspexp-exec, pspexp-run, pspexp-pkg, ...
#   /usr/local/lib      libsdl-pspexp.so (preloaded into SDL2 games)
#   /usr/local/share/pspexp/launcher   fonts and icons
#   /opt/pspexp/{apps,systems}         packages, owned by the Pi's user
#   pspexp-term.service the launcher on the PSP at boot (USB), as that user
# The source is copied to ~/pspexp/src on the Pi and built there (its gcc, its
# SDL2); a checkout of the repo on the Pi is left alone.
#   HOST=user@host scripts/pi/install.sh          build + install + restart (PI= still read)
#   HOST=user@host scripts/pi/install.sh cavestory   Cave Story (NXEngine-evo)
#       compiled on the Pi -> build/pi/bin/nxengine-evo
#   HOST=user@host scripts/pi/install.sh tyrian   OpenTyrian (SDL2) compiled on
#       the Pi from ../tyrian/opentyrian into build/pi/bin/opentyrian, which
#       the opentyrian package ships (BOARD=pi scripts/packages/pspexp-pkg-build build opentyrian)
#   HOST=user@host scripts/pi/install.sh retro    pspexp-retro and a libretro core,
#       compiled on the Pi into build/pi/bin/, for the nes package
# Needs on the Pi: sudo without a password prompt, and the build packages
# (scripts/pi/build.sh installs them: cmake, libsdl2-dev, libusb-1.0-0-dev).
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

# the tracked files as they are in this tree (uncommitted edits included)
(cd "$ROOT" && git ls-files -z --cached --others --exclude-standard | COPYFILE_DISABLE=1 xargs -0 tar --no-xattrs -cf - 2>/dev/null) |
	ssh "$PI" 'mkdir -p ~/pspexp/src && tar -C ~/pspexp/src -xf -'
ssh "$PI" 'set -e; cd ~/pspexp/src
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
	cmake --build build -j4 --target pspexp-term pspexp-ms sdl-pspexp pspexp-bench pspexp-sprite-demo 2>&1 | grep -E "error|warning:" || true
	[ -x build/pspexp-term ] && [ -f build/libsdl-pspexp.so ]'

# a Pi whose PSPExp comes from apt (pspexp-core, docs/Package_Manager.md §6): only the build;
# then scripts/packages/debs/pspexp-core/build build/debs arm64 $PI, apt-repo build and push,
# and pspexp-pkg upgrade on the Pi (the console offers System > Restart PSPExp)
# (automatic once the Pi has pspexp-core: /usr/local copies would shadow the package's)
[ -n "${PSPEXP_CORE:-}" ] || { ssh "$PI" 'dpkg-query -W -f="\${Status}" pspexp-core 2>/dev/null' | grep -q "ok installed" && PSPEXP_CORE=apt; }
if [ "${PSPEXP_CORE:-}" = apt ]; then echo "built on $PI (~/pspexp/src/build); its PSPExp comes from apt: package it as pspexp-core"; exit 0; fi

# install; the running console is swapped for the new one at the end
ssh "$PI" 'set -e; S=~/pspexp/src; U=$(id -un); H=$HOME
	sudo install -d -m 0755 /usr/local/bin /usr/local/lib /usr/local/share/pspexp/launcher /etc/pspexp
	for f in pspexp-term pspexp-bench pspexp-sprite-demo; do sudo install -m 0755 $S/build/$f /usr/local/bin/$f.new && sudo mv /usr/local/bin/$f.new /usr/local/bin/$f; done
	sudo install -m 0755 $S/scripts/board/pspexp-exec /usr/local/bin/pspexp-exec
	sudo install -m 0755 $S/scripts/board/pspexp-run /usr/local/bin/pspexp-run
	sudo install -m 0755 $S/scripts/board/pspexp-pkg /usr/local/bin/pspexp-pkg
	sudo install -m 0644 $S/build/libsdl-pspexp.so /usr/local/lib/libsdl-pspexp.so.new && sudo mv /usr/local/lib/libsdl-pspexp.so.new /usr/local/lib/libsdl-pspexp.so
	for f in Inter-Regular.ttf Inter-SemiBold.ttf lucide.ttf Inter-LICENSE.txt lucide-LICENSE.txt; do
		sudo install -m 0644 $S/tools/pspexp-term/assets/$f /usr/local/share/pspexp/launcher/$f
	done
	sudo install -d -m 0755 -o $U -g $U /opt/pspexp /opt/pspexp/apps /opt/pspexp/systems
	echo "$U ALL=(root) NOPASSWD: /usr/sbin/reboot, /usr/sbin/poweroff, /usr/bin/timedatectl set-timezone *, /usr/bin/nmcli" | sudo tee /etc/sudoers.d/pspexp >/dev/null
	sudo chmod 0440 /etc/sudoers.d/pspexp
	echo "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"054c\", ATTR{idProduct}==\"01ca\", MODE=\"0666\"" | sudo tee /etc/udev/rules.d/60-pspexp.rules >/dev/null
	sudo udevadm control --reload-rules || true
	SH=$(getent passwd $U | cut -d: -f7)
	sudo tee /etc/systemd/system/pspexp-term.service >/dev/null <<U
[Unit]
Description=PSPExp console on the PSP (pspexp-term)
After=multi-user.target

[Service]
User=$U
WorkingDirectory=$H
Environment=PSPEXP_TERM=1 HOME=$H SHELL=${SH:-/bin/bash} PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:/usr/games
ExecStart=/usr/local/bin/pspexp-term --shell ${SH:-/bin/bash} --launcher --usb
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
U
	sudo systemctl daemon-reload
	sudo systemctl enable pspexp-term >/dev/null 2>&1
	sudo systemctl restart pspexp-term
	sleep 1; systemctl is-active pspexp-term'
# packages from recipes (docs/Package_Manager.md): the board kind, the project key, the
# apt source, the root helper and its sudo rule, the catalogue. The repository's address
# is a local setting, never in git: $PSPEXP_REPO_URL or ~/.pspexp/repo-url (http://host:port)
REPO_URL="${PSPEXP_REPO_URL:-$(head -1 "$HOME/.pspexp/repo-url" 2>/dev/null)}"
if [ -n "$REPO_URL" ]; then
	ssh "$PI" "set -e; S=~/pspexp/src; gpg --dearmor < \$S/scripts/packages/pspexp-archive-key.asc > /tmp/pspexp-keyring.gpg
		sudo sh \$S/scripts/board/pspexp-apt-setup pi \$(id -un) '$REPO_URL' /tmp/pspexp-keyring.gpg \$S/scripts/board/pspexp-pkg-apt
		rm -f /tmp/pspexp-keyring.gpg"
else
	echo "no package repository address (\$PSPEXP_REPO_URL or ~/.pspexp/repo-url): Packages keeps to folder repositories"
fi
echo "PSPExp installed on $PI; the console restarted with the launcher"
