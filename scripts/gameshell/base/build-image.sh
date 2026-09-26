#!/bin/sh
# The PSPExp GameShell card image on a fresh base: Debian 13 (armhf), the
# mainline kernel from build-kernel.sh, the board's own U-Boot, and PSPExp
# with its packages. Built on this machine in containers (OrbStack/Docker):
#
#   scripts/build-gameshell.sh                   (the tools and the SDL layer)
#   scripts/pspexp-pkg-build build freedoom opentyrian     (the packages to bundle)
#   scripts/gameshell/base/build-kernel.sh
#   scripts/gameshell/base/build-image.sh STOCK.img [options]
#       -> build/gameshell-base/pspexp-gameshell.img  (write it to a card)
#
# STOCK.img is ClockworkOS v0.6 (clockworkos_v0.6.img): only its U-Boot and
# the Wi-Fi chip's firmware and calibration file are taken from it.
# Options (for your own card; they never go into the repository):
#   --ssh-key FILE          allow this public key for cpi (default: none)
#   --wifi-from USER@HOST   copy the Wi-Fi connections of a running GameShell
# Login: cpi / cpi (change it), sudo without a password as on ClockworkOS.
set -e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="$ROOT/build/gameshell-base"
STOCK="$1"; [ $# -gt 0 ] && shift
[ -f "$STOCK" ] || { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
SSHKEY=""; WIFI_FROM=""
while [ $# -gt 0 ]; do
	case "$1" in
	--ssh-key) SSHKEY="$2"; shift 2 ;;
	--wifi-from) WIFI_FROM="$2"; shift 2 ;;
	*) echo "unknown option $1" >&2; exit 2 ;;
	esac
done
[ -f "$OUT/kernel/uImage" ] || { echo "run scripts/gameshell/base/build-kernel.sh first" >&2; exit 1; }
DF="$(brew --prefix e2fsprogs 2>/dev/null)/sbin/debugfs"
[ -x "$DF" ] || { echo "debugfs not found: brew install e2fsprogs" >&2; exit 1; }

IN="$OUT/in"; rm -rf "$IN"; mkdir -p "$IN/fw" "$IN/wifi"
# from the stock image: U-Boot (everything before the first partition, after the MBR)
dd if="$STOCK" of="$IN/uboot.bin" bs=512 skip=1 count=8191 2>/dev/null
OFF=$(python3 -c "import struct,sys; d=open(sys.argv[1],'rb').read(512); print(struct.unpack('<I', d[446+16+8:446+16+12])[0]*512)" "$STOCK")
for f in brcmfmac43430a0-sdio.bin brcmfmac43430a0-sdio.txt bcm43438a0.hcd; do
	"$DF" -R "dump /lib/firmware/brcm/$f $IN/fw/$f" "$STOCK?offset=$OFF" 2>/dev/null
	[ -s "$IN/fw/$f" ] || { echo "stock image: /lib/firmware/brcm/$f missing" >&2; exit 1; }
done
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
# PSPExp: the same file list as every GameShell install (with the built packages)
. "$ROOT/scripts/gameshell/stage.sh"
gs_fetch() { : > "$2"; }
gs_stage "$IN/stage"
cp "$ROOT/scripts/gameshell/base/image-setup.sh" "$IN/"

docker volume create pspexp-imgbuild >/dev/null
docker run --rm --privileged --platform linux/arm64 \
	-v pspexp-imgbuild:/work -v "$OUT":/out debian:trixie sh /out/in/image-setup.sh
ls -la "$OUT/pspexp-gameshell.img"
