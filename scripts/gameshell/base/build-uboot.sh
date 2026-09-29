#!/bin/sh
# U-Boot for the GameShell (PSPExp base image) from mainline source, instead of
# the one taken from the ClockworkOS image, cross-compiled on this machine in an
# ARM64 Debian 13 container (OrbStack/Docker):
#   scripts/gameshell/base/build-uboot.sh      -> build/gameshell-base/uboot/
# u-boot-sunxi-with-spl.bin (written at 8 KiB on the card) and the .config.
# The board's settings: uboot.config; its device tree: the one the kernel uses;
# one small change to the power chip's setup: uboot-cpi3.patch.
# UVER picks the release (default below).
set -e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
UVER="${UVER:-2026.07}"
OUT="$ROOT/build/gameshell-base"
mkdir -p "$OUT/uboot"
TAR="$OUT/u-boot-$UVER.tar.bz2"
[ -f "$TAR" ] || curl -fL -o "$TAR" "https://ftp.denx.de/pub/u-boot/u-boot-$UVER.tar.bz2"
docker volume create pspexp-ubuild >/dev/null
docker run --rm --platform linux/arm64 \
	-v pspexp-ubuild:/work -v "$OUT":/out -v "$ROOT/scripts/gameshell/base":/base:ro \
	-e UVER="$UVER" -e JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}" debian:trixie sh -ec '
	export DEBIAN_FRONTEND=noninteractive
	apt-get -qq update && apt-get -qq install -y --no-install-recommends \
		gcc libc6-dev gcc-arm-linux-gnueabihf make bc bison flex libssl-dev libgnutls28-dev \
		python3 python3-dev python3-setuptools python3-pyelftools swig device-tree-compiler bzip2 patch >/dev/null
	cd /work
	rm -rf u-boot-$UVER && tar -xjf /out/u-boot-$UVER.tar.bz2
	cd u-boot-$UVER
	# the board: its device tree (the kernel'"'"'s) beside the upstream Allwinner ones, and its defconfig
	cp /base/sun8i-r16-clockworkpi-cpi3.dts arch/arm/dts/
	grep -q clockworkpi-cpi3 arch/arm/dts/Makefile ||
		sed -i "s|sun8i-r16-bananapi-m2m.dtb \\\\|sun8i-r16-bananapi-m2m.dtb \\\\\n\tsun8i-r16-clockworkpi-cpi3.dtb \\\\|" arch/arm/dts/Makefile
	grep -v "^#" /base/uboot.config > configs/clockworkpi-cpi3_defconfig
	patch -p1 -s < /base/uboot-cpi3.patch   # the charge LED (uboot-cpi3.patch says why)
	M="make CROSS_COMPILE=arm-linux-gnueabihf- -j$JOBS"
	$M clockworkpi-cpi3_defconfig >/dev/null
	for k in $(grep -o "^CONFIG_[A-Z0-9_]*=[^#]*" /base/uboot.config); do
		grep -qx "$k" .config || echo "not in .config as set: $k" >&2
	done
	$M 2>&1 | grep -E "error|Error|warning: .*cpi3" || true
	cp u-boot-sunxi-with-spl.bin /out/uboot/
	cp .config /out/uboot/config
	strings u-boot-sunxi-with-spl.bin | grep -m1 "^U-Boot SPL" > /out/uboot/version
	echo "$(cat /out/uboot/version) built"
'
ls -la "$OUT/uboot"
