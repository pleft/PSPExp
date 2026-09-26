#!/bin/sh
# PSPExp with the launcher on a Raspberry Pi (Debian / Raspberry Pi OS,
# 64-bit), from the Mac over SSH. Same layout as the GameShell's, so the
# launcher's packages (scripts/gs-pkg, BOARD=pi) install the same way:
#   /usr/local/bin      pspexp-term, pspexp-exec, pspexp-run, pspexp-pkg, ...
#   /usr/local/lib      libsdl-pspexp.so (preloaded into SDL2 games)
#   /usr/local/share/pspexp/launcher   fonts and icons
#   /opt/pspexp/{apps,systems}         packages, owned by the Pi's user
#   pspexp-term.service the launcher on the PSP at boot (USB), as that user
# The source is copied to ~/pspexp/src on the Pi and built there (its gcc, its
# SDL2); a checkout of the repo on the Pi is left alone.
#   PI=user@host scripts/install-pi.sh          build + install + restart
#   PI=user@host scripts/install-pi.sh tyrian   OpenTyrian (SDL2) compiled on
#       the Pi from ../tyrian/opentyrian into build/pi/bin/opentyrian, which
#       the opentyrian package ships (BOARD=pi scripts/gs-pkg build opentyrian)
# Needs on the Pi: sudo without a password prompt, and the build packages
# (scripts/build-pi.sh installs them: cmake, libsdl2-dev, libusb-1.0-0-dev).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PI="${PI:-${GAMESHELL:-pi@raspberrypi.local}}"

if [ "$1" = tyrian ]; then
	SRC="$ROOT/../tyrian/opentyrian"
	[ -f "$SRC/Makefile" ] || { echo "OpenTyrian source not found in $SRC (git clone https://github.com/opentyrian/opentyrian)" >&2; exit 1; }
	{ COPYFILE_DISABLE=1 tar --no-xattrs -C "$SRC" --exclude .git --exclude obj --exclude opentyrian -cf - . ; } 2>/dev/null |
		ssh "$PI" 'rm -rf ~/pspexp/tyrian-src && mkdir -p ~/pspexp/tyrian-src && tar -C ~/pspexp/tyrian-src -xf -'
	ssh "$PI" 'set -e; make -C ~/pspexp/tyrian-src -j4 WITH_NETWORK=false 2>&1 | grep -E "error|implicit" || true
		ls -la ~/pspexp/tyrian-src/opentyrian'
	mkdir -p "$ROOT/build/pi/bin"; scp -q "$PI:pspexp/tyrian-src/opentyrian" "$ROOT/build/pi/bin/opentyrian"
	echo "build/pi/bin/opentyrian: package it with BOARD=pi scripts/gs-pkg build opentyrian"
	exit 0
fi

# the tracked files as they are in this tree (uncommitted edits included)
(cd "$ROOT" && git ls-files -z | COPYFILE_DISABLE=1 xargs -0 tar --no-xattrs -cf - 2>/dev/null) |
	ssh "$PI" 'mkdir -p ~/pspexp/src && tar -C ~/pspexp/src -xf -'
ssh "$PI" 'set -e; cd ~/pspexp/src
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
	cmake --build build -j4 --target pspexp-term sdl-pspexp pspexp-bench pspexp-sprite-demo 2>&1 | grep -E "error|warning:" || true
	[ -x build/pspexp-term ] && [ -f build/libsdl-pspexp.so ]'

# install; the running console is swapped for the new one at the end
ssh "$PI" 'set -e; S=~/pspexp/src; U=$(id -un); H=$HOME
	sudo install -d -m 0755 /usr/local/bin /usr/local/lib /usr/local/share/pspexp/launcher /etc/pspexp
	for f in pspexp-term pspexp-bench pspexp-sprite-demo; do sudo install -m 0755 $S/build/$f /usr/local/bin/$f.new && sudo mv /usr/local/bin/$f.new /usr/local/bin/$f; done
	sudo install -m 0755 $S/scripts/pspexp-exec /usr/local/bin/pspexp-exec
	sudo install -m 0755 $S/scripts/gameshell/pspexp-run /usr/local/bin/pspexp-run
	sudo install -m 0755 $S/scripts/gameshell/pspexp-pkg /usr/local/bin/pspexp-pkg
	sudo install -m 0644 $S/build/libsdl-pspexp.so /usr/local/lib/libsdl-pspexp.so.new && sudo mv /usr/local/lib/libsdl-pspexp.so.new /usr/local/lib/libsdl-pspexp.so
	for f in Inter-Regular.ttf Inter-SemiBold.ttf lucide.ttf Inter-LICENSE.txt lucide-LICENSE.txt; do
		sudo install -m 0644 $S/tools/pspexp-term/assets/$f /usr/local/share/pspexp/launcher/$f
	done
	sudo install -d -m 0755 -o $U -g $U /opt/pspexp /opt/pspexp/apps /opt/pspexp/systems
	echo "$U ALL=(root) NOPASSWD: /usr/sbin/reboot, /usr/sbin/poweroff" | sudo tee /etc/sudoers.d/pspexp >/dev/null
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
echo "PSPExp installed on $PI; the console restarted with the launcher"
