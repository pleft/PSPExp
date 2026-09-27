#!/bin/sh
# Linux (Raspberry Pi and the like): install pspexp-term as a systemd service
# that runs a login shell of the invoking user on the PSP, starts at boot,
# waits for a PSP on USB and restarts 2 s after any exit.
#   scripts/pi/install-console.sh            install + start
#   scripts/pi/install-console.sh remove     stop + uninstall
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
U="${SUDO_USER:-$(id -un)}"
H="$(getent passwd "$U" | cut -d: -f6)"
S="$(getent passwd "$U" | cut -d: -f7)"
if [ "$1" = "remove" ]; then
	sudo systemctl disable --now pspexp-term 2>/dev/null || true
	sudo rm -f /etc/systemd/system/pspexp-term.service
	sudo systemctl daemon-reload
	echo "pspexp-term service removed"
	exit 0
fi
[ -x "$ROOT/build/host/pspexp-term" ] || { echo "build/host/pspexp-term not built (scripts/pi/build.sh)" >&2; exit 1; }
sed -e "s|@USER@|$U|g" -e "s|@HOME@|$H|g" -e "s|@SHELL@|${S:-/bin/bash}|g" -e "s|@ROOT@|$ROOT|g" \
	"$ROOT/scripts/pi/pspexp-term.service" | sudo tee /etc/systemd/system/pspexp-term.service >/dev/null
sudo systemctl daemon-reload
sudo systemctl enable --now pspexp-term
echo "pspexp-term running as $U: plug in the PSP and start the PSPExp runtime"
