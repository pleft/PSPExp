#!/bin/sh
# Install or update PSPExp on a running ClockworkPi GameShell over SSH: the
# same files as the card image (scripts/gameshell/base/build-image.sh) has (the list is
# scripts/gameshell/stage.sh), then reloads systemd and udev and restarts
# the PSP console. The Wi-Fi mode is not switched (see pspexp-wifi-mode).
#   scripts/gameshell/build.sh
#   HOST=user@host scripts/gameshell/install.sh
# With a package repository (~/.pspexp/repo-url) PSPExp itself comes from apt as
# pspexp-core (build it first: scripts/packages/debs/pspexp-core/build OUTDIR armhf,
# then scripts/packages/apt-repo build and push); the GameShell's own files still come from here.
# HOST defaults to cpi@gameshell (GAMESHELL= is still read, its old name);
# cpi needs sudo without a password
# (the GameShell OS default). Games and apps are packages: scripts/packages/pspexp-pkg-build.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GS="${HOST:-${GAMESHELL:-cpi@gameshell}}"
. "$ROOT/scripts/gameshell/stage.sh"
ssh -o ConnectTimeout=8 "$GS" true 2>/dev/null || { echo "cannot reach $GS over SSH (off, or not on the network?)" >&2; exit 1; }
ssh -o ConnectTimeout=8 "$GS" 'sudo -n true' 2>/dev/null || { echo "cannot run sudo on $GS without a password" >&2; exit 1; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

# packages from recipes (docs/Package_Manager.md), when the repository's address is
# set (a local setting, never in git: $PSPEXP_REPO_URL or ~/.pspexp/repo-url): the
# board set up for apt (kind, key, source, root helper, sudo rule, catalogue), then
# PSPExp itself as the pspexp-core package, and the stage without its own copies
# (GS_CORE=apt). GS_CORE=local keeps the older /usr/local install.
REPO_URL="${PSPEXP_REPO_URL:-$(head -1 "$HOME/.pspexp/repo-url" 2>/dev/null)}"
[ -n "$REPO_URL" ] || REPO_URL=$(head -1 "$ROOT/scripts/packages/public-repo-url" 2>/dev/null)   # else the public repository
if [ -n "$REPO_URL" ] && [ "${GS_CORE:-apt}" = apt ]; then
	gpg --dearmor < "$ROOT/scripts/packages/pspexp-archive-key.asc" > "$W/keyring.gpg"
	scp -q "$W/keyring.gpg" "$ROOT/scripts/board/pspexp-apt-setup" "$ROOT/scripts/board/pspexp-pkg-apt" "$GS:/tmp/"
	ssh "$GS" "set -e; sudo sh /tmp/pspexp-apt-setup gameshell cpi '$REPO_URL' /tmp/keyring.gpg /tmp/pspexp-pkg-apt
		sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends pspexp-core >/dev/null
		rm -f /tmp/keyring.gpg /tmp/pspexp-apt-setup /tmp/pspexp-pkg-apt
		dpkg-query -W pspexp-core pspexp-runtime"
	export GS_CORE=apt
else
	[ -n "$REPO_URL" ] || echo "no package repository address (\$PSPEXP_REPO_URL or ~/.pspexp/repo-url): PSPExp from this build, into /usr/local"
	export GS_CORE=local
fi
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
