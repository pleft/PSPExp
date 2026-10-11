#!/bin/sh
# The PSPExp GameShell card image: a minimal Debian 13 (armhf), the mainline
# kernel from build-kernel.sh, U-Boot from mainline (build-uboot.sh), and PSPExp from its apt
# repository (pspexp-core). No games: the user adds them in Apps > Packages
# (docs/Package_Manager.md §8). Built on this machine in containers (OrbStack/Docker):
#
#   scripts/gameshell/build.sh                   (the GameShell's own files are staged from the tree)
#   scripts/gameshell/base/build-kernel.sh
#   scripts/gameshell/base/build-uboot.sh
#   scripts/gameshell/base/build-image.sh [options]
#       -> build/gameshell-base/pspexp-gameshell.img  (write it to a card)
#
# Nothing is taken from a ClockworkOS image: the Wi-Fi chip's firmware is Debian's;
# its settings (Ampak's AP6212 NVRAM) are in this folder, and its Bluetooth patch
# is fetched from ClockworkPi's own repository (github.com/clockworkpi/bluetooth).
# Options (for your own card; they never go into the repository):
#   --ssh-key FILE          allow this public key for cpi (default: none)
#   --wifi-from USER@HOST   copy the Wi-Fi connections of a running GameShell
# The package repository's address: $PSPEXP_REPO_URL or ~/.pspexp/repo-url (a local
# setting; the card's apt source points there). Login: cpi / cpi (change it),
# sudo without a password as on ClockworkOS.
set -e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="$ROOT/build/gameshell-base"
SSHKEY=""; WIFI_FROM=""
while [ $# -gt 0 ]; do
	case "$1" in
	--ssh-key) SSHKEY="$2"; shift 2 ;;
	--wifi-from) WIFI_FROM="$2"; shift 2 ;;
	*) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
	esac
done
[ -f "$OUT/kernel/uImage" ] || { echo "run scripts/gameshell/base/build-kernel.sh first" >&2; exit 1; }
[ -f "$OUT/uboot/u-boot-sunxi-with-spl.bin" ] || { echo "run scripts/gameshell/base/build-uboot.sh first" >&2; exit 1; }

IN="$OUT/in"; rm -rf "$IN"; mkdir -p "$IN/fw" "$IN/wifi"
# U-Boot (written at 8 KiB on the card) and the Wi-Fi chip's settings
cp "$OUT/uboot/u-boot-sunxi-with-spl.bin" "$IN/uboot.bin"
echo "U-Boot: $(cat "$OUT/uboot/version")"
cp "$ROOT/scripts/gameshell/base/brcmfmac43430a0-sdio.clockwork,clockworkpi-cpi3.txt" "$IN/fw/"
# its Bluetooth firmware patch: ClockworkPi publishes it (github.com/clockworkpi/bluetooth,
# GPL-3.0); fetched once, pinned to a commit and checked
BT_URL=https://raw.githubusercontent.com/clockworkpi/bluetooth/52feb7abb39a8e8b71b2a593851638c76a3e59c1/bcm43438a0.hcd
BT_SHA=58b8142caddda4559fbc400d923ee44e7dc021dc92c3cacfb4464c6a729d6d2d
BT="$OUT/cache/bcm43438a0.hcd"; mkdir -p "$OUT/cache"
[ -s "$BT" ] || curl -fsSL -o "$BT" "$BT_URL" || { echo "cannot fetch $BT_URL" >&2; exit 1; }
[ "$(shasum -a 256 "$BT" | cut -d' ' -f1)" = "$BT_SHA" ] || { rm -f "$BT"; echo "$BT_URL: checksum differs" >&2; exit 1; }
cp "$BT" "$IN/fw/BCM4343A0.hcd"
[ -n "$SSHKEY" ] && cp "$SSHKEY" "$IN/authorized_keys"
# Wi-Fi connections: fetched from a running GameShell and kept in
# build/gameshell-base/local (never in the repository), reused when it is off
LOCAL="$OUT/local/wifi"
if [ -n "$WIFI_FROM" ]; then
	mkdir -p "$LOCAL"
	if ssh -o ConnectTimeout=8 "$WIFI_FROM" 'sudo -n sh -c "cd /etc/NetworkManager/system-connections && tar -cf - *.nmconnection"' > "$IN/wifi.tar" 2>/dev/null && [ -s "$IN/wifi.tar" ]; then
		tar -C "$LOCAL" -xf "$IN/wifi.tar"
	else
		echo "($WIFI_FROM unreachable: using the Wi-Fi connections kept from before)" >&2
	fi
	rm -f "$IN/wifi.tar"
	cp "$LOCAL"/*.nmconnection "$IN/wifi/" 2>/dev/null || echo "no Wi-Fi connection: the card will use USB networking only" >&2
fi
# PSPExp: pspexp-core from the repository, then the GameShell's own files, the
# same list as every GameShell install (scripts/gameshell/stage.sh, GS_CORE=apt)
REPO_URL="${PSPEXP_REPO_URL:-$(head -1 "$HOME/.pspexp/repo-url" 2>/dev/null)}"
[ -n "$REPO_URL" ] || REPO_URL=$(head -1 "$ROOT/scripts/packages/public-repo-url" 2>/dev/null)   # else the public repository
[ -n "$REPO_URL" ] || { echo "no package repository address (\$PSPEXP_REPO_URL or ~/.pspexp/repo-url)" >&2; exit 1; }
mkdir -p "$IN/pkg"
echo "$REPO_URL" > "$IN/pkg/repo-url"
gpg --dearmor < "$ROOT/scripts/packages/pspexp-archive-key.asc" > "$IN/pkg/keyring.gpg"
cp "$ROOT/scripts/board/pspexp-apt-setup" "$ROOT/scripts/board/pspexp-pkg-apt" "$IN/pkg/"
. "$ROOT/scripts/gameshell/stage.sh"
GS_CORE=apt gs_stage "$IN/stage"
cp "$ROOT/scripts/gameshell/base/image-setup.sh" "$IN/"

# a .local repository name resolves here (mDNS), not in the container: pass its address
RH=$(echo "$REPO_URL" | sed -E 's#^[a-z]+://([^:/]+).*#\1#'); ADDHOST=""
case "$RH" in *.local)
	RIP=$(dscacheutil -q host -a name "$RH" 2>/dev/null | awk '/^ip_address/ {print $2; exit}')
	[ -n "$RIP" ] || { echo "cannot resolve $RH" >&2; exit 1; }
	ADDHOST="--add-host=$RH:$RIP" ;;
esac
docker volume create pspexp-imgbuild >/dev/null
# (PSPEXP_PLATFORM: the container's platform; linux/arm64 on the Mac, linux/amd64 in CI with QEMU)
docker run --rm --privileged --platform "${PSPEXP_PLATFORM:-linux/arm64}" $ADDHOST \
	-v pspexp-imgbuild:/work -v "$OUT":/out debian:trixie sh /out/in/image-setup.sh
ls -la "$OUT/pspexp-gameshell.img"
