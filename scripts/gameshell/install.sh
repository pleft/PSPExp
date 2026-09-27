#!/bin/sh
# Install or update PSPExp on a running ClockworkPi GameShell over SSH: the
# same files as scripts/gameshell/prep-stock-image.sh puts on a card (the list is
# scripts/gameshell/stage.sh), then reloads systemd and udev and restarts
# the PSP console. The Wi-Fi mode is not switched (see pspexp-wifi-mode).
#   scripts/gameshell/build.sh
#   HOST=user@host scripts/gameshell/install.sh
# HOST defaults to cpi@gameshell (GAMESHELL= is still read, its old name);
# cpi needs sudo without a password
# (the GameShell OS default). Games and apps are packages: scripts/packages/pspexp-pkg-build.
# Removing ClockworkOS's own launcher and games, once: scripts/gameshell/clean-stock.sh.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GS="${HOST:-${GAMESHELL:-cpi@gameshell}}"
. "$ROOT/scripts/gameshell/stage.sh"
ssh -o ConnectTimeout=8 "$GS" 'sudo -n true' 2>/dev/null || { echo "cannot run sudo on $GS without a password" >&2; exit 1; }

gs_fetch() { ssh "$GS" "cat '$1' 2>/dev/null" > "$2" || : ; }
# after its first boot a GameShell gets packages with scripts/packages/pspexp-pkg-build, not the bundle
ssh "$GS" 'test -f /etc/pspexp/firstboot-done' 2>/dev/null && export GS_PREINSTALL=0
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
gs_stage "$W/stage"

# the stage over, then the manifest applied as root on the GameShell
{ COPYFILE_DISABLE=1 tar --no-xattrs -C "$W/stage" -cf - . ; } 2>/dev/null |
	ssh "$GS" 'rm -rf /tmp/pspexp-stage && mkdir -p /tmp/pspexp-stage && tar -C /tmp/pspexp-stage -xf -'
ssh "$GS" 'sudo sh -s' <<'APPLY'
set -e
cd /tmp/pspexp-stage
while read -r kind a b c d; do
	case $kind in
	dir)  [ -d "$d" ] || install -d -m "$a" -o "$b" -g "$c" "$d" ;;
	file) # a new inode (install unlinks first): safe while the program runs
	      install -D -m "$a" -o "$b" -g "$c" "root$d" "$d" ;;
	link) ln -sfn "$a" "$b" ;;
	rm)   rm -rf "$a" ;;
	esac
done < manifest
systemctl daemon-reload
udevadm control --reload
udevadm trigger --subsystem-match=usb --attr-match=idVendor=054c
systemctl restart pspexp-term
echo "installed $(grep -c '^file' manifest) files"
cd /; rm -rf /tmp/pspexp-stage
APPLY
echo "PSPExp installed on $GS; the console restarted (Wi-Fi mode: $(ssh "$GS" 'cat /etc/pspexp/wifi-mode 2>/dev/null || echo home'))"
