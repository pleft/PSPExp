#!/bin/sh
# PSPExp on a Raspberry Pi OS you already have, in one command (run it as your own user):
#   curl -fsSL https://pspexp.dev/install.sh | sh
# For Raspberry Pi OS (64-bit) on Debian 13 "trixie", Lite or with a desktop; Pi 3, 4, 400 or 5.
# It does what PSPExp's card image does, and nothing more: PSPExp's package repository
# (https://pkg.pspexp.dev, its signing key carried in this script), the pspexp-core package and what
# the PSP's sound and films need (PipeWire, ffmpeg), the PSP's USB rule, the console as a service
# that starts at boot, the sudo rules for its System page and Packages app only. It sets no
# password, no SSH and no Wi-Fi: those stay yours. Written by site/build.py from site/install.sh,
# scripts/pi/board-files.sh and scripts/packages/pspexp-archive-key.asc (docs/Developing.md).
# To remove PSPExp again: sudo apt remove pspexp-core, then sudo systemctl disable pspexp-term.
set -e
# all of it in main(), called on the last line: piped into sh, nothing runs before the whole script is here

say() { printf '\033[1m%s\033[0m\n' "$*"; }
die() { printf 'PSPExp install: %s\n' "$*" >&2; exit 1; }

main() {
[ "$(uname -s)" = Linux ] || die "this is for a Raspberry Pi running Raspberry Pi OS"
[ "$(id -u)" -ne 0 ] || die "run it as your own user, not as root (it asks for sudo itself): curl -fsSL https://pspexp.dev/install.sh | sh"
command -v apt-get >/dev/null 2>&1 && command -v dpkg >/dev/null 2>&1 || die "this needs Raspberry Pi OS (Debian's apt)"
[ "$(dpkg --print-architecture)" = arm64 ] || die "this needs the 64-bit Raspberry Pi OS (this system is $(dpkg --print-architecture)); write the 64-bit one, or PSPExp's own card image: https://pspexp.dev/download"
. /etc/os-release
[ "${VERSION_CODENAME:-}" = trixie ] || die "this needs Raspberry Pi OS based on Debian 13 \"trixie\" (this one is ${PRETTY_NAME:-unknown}); write a current one with Raspberry Pi Imager, or PSPExp's own card image: https://pspexp.dev/download"
U=$(id -un)
REPO=https://pkg.pspexp.dev

say "PSPExp for $U on $(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo 'this computer')"
sudo -v || die "this needs sudo"

say "1/4  PSPExp's package repository"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
base64 -d > "$T/keyring.gpg" <<'KEY'
{{KEYRING_B64}}
KEY
sudo install -m 644 "$T/keyring.gpg" /usr/share/keyrings/pspexp-archive-keyring.gpg
printf 'Types: deb\nURIs: %s\nSuites: stable\nComponents: main\nSigned-By: /usr/share/keyrings/pspexp-archive-keyring.gpg\n' "$REPO" |
	sudo tee /etc/apt/sources.list.d/pspexp.sources >/dev/null

say "2/4  PSPExp and what it needs (a few minutes)"
sudo apt-get update -q </dev/null
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
	pspexp-core gpgv pipewire pipewire-bin wireplumber libspa-0.2-bluetooth ffmpeg </dev/null

say "3/4  The Packages app and the board's settings"
sudo sh /usr/share/pspexp/pspexp-apt-setup pi "$U" "$REPO" "$T/keyring.gpg" /usr/sbin/pspexp-pkg-apt

say "4/4  The PSP's USB rule and the console's service"
cat > "$T/board-files.sh" <<'BOARD'
{{BOARD_FILES}}
BOARD
sudo sh "$T/board-files.sh" "$U" /usr/bin/pspexp-term
sudo udevadm control --reload-rules 2>/dev/null || true
sudo udevadm trigger --subsystem-match=usb 2>/dev/null || true
sudo systemctl daemon-reload
sudo systemctl restart pspexp-term

say "PSPExp is installed: $(dpkg-query -W -f='${Version}' pspexp-core)"
cat <<'DONE'
Next:
  1. The PSP's part: https://pspexp.dev/download (the PSP's zip, unzipped at the
     memory stick's root; a PSP with custom firmware).
  2. Plug the PSP into one of the Pi's USB ports and start PSPExp on it: the
     launcher appears. From now on the Pi waits for the PSP at every boot.
  3. Games: Apps > Packages, on the PSP.
The manual: https://pspexp.dev/manual/raspberry-pi.html
DONE
}

main "$@"
