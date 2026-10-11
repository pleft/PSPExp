#!/bin/sh
# Mainline kernel for the GameShell (PSPExp base image), cross-compiled on this
# machine in an ARM64 Debian 13 container (OrbStack/Docker):
#   scripts/gameshell/base/build-kernel.sh      -> build/gameshell-base/kernel/
# uImage (for the board's own U-Boot: bootm, at most 8 MB), the device tree,
# modules.tar.gz, the .config. KVER picks the kernel (default: the 6.18
# long-term release below), KBASE the base configuration (sunxi_defconfig).
set -e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
KVER="${KVER:-6.18.54}"
OUT="$ROOT/build/gameshell-base"
mkdir -p "$OUT/kernel"
TAR="$OUT/linux-$KVER.tar.xz"
[ -f "$TAR" ] || curl -fL -o "$TAR" "https://cdn.kernel.org/pub/linux/kernel/v${KVER%%.*}.x/linux-$KVER.tar.xz"
# the source and build tree live in a volume: the kernel has file names that
# differ only in case, which the Mac's disk cannot hold
docker volume create pspexp-kbuild >/dev/null
# (PSPEXP_PLATFORM: the container's platform; linux/arm64 on the Mac, linux/amd64 in CI with QEMU)
docker run --rm --platform "${PSPEXP_PLATFORM:-linux/arm64}" \
	-v pspexp-kbuild:/work -v "$OUT":/out -v "$ROOT/scripts/gameshell/base":/base:ro \
	-e KVER="$KVER" -e JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}" debian:trixie sh -ec '
	export DEBIAN_FRONTEND=noninteractive
	apt-get -qq update && apt-get -qq install -y --no-install-recommends \
		gcc libc6-dev gcc-arm-linux-gnueabihf make bc bison flex libssl-dev libelf-dev u-boot-tools \
		xz-utils cpio kmod python3 >/dev/null
	cd /work
	[ -d linux-$KVER ] || tar -xJf /out/linux-$KVER.tar.xz
	cd linux-$KVER
	cp /base/sun8i-r16-clockworkpi-cpi3.dts arch/arm/boot/dts/allwinner/
	grep -q clockworkpi-cpi3 arch/arm/boot/dts/allwinner/Makefile ||
		sed -i "s|sun8i-r16-bananapi-m2m.dtb|sun8i-r16-bananapi-m2m.dtb \\\\\n\tsun8i-r16-clockworkpi-cpi3.dtb|" arch/arm/boot/dts/allwinner/Makefile
	M="make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j$JOBS"
	$M ${KBASE:-sunxi_defconfig} >/dev/null
	scripts/kconfig/merge_config.sh -m .config /base/kernel.config >/dev/null
	$M olddefconfig >/dev/null
	$M zImage modules allwinner/sun8i-r16-clockworkpi-cpi3.dtb 2>&1 | grep -E "error|warning: .*cpi3|Error" || true
	R=$(cat include/config/kernel.release)
	rm -rf /tmp/mod && $M INSTALL_MOD_PATH=/tmp/mod INSTALL_MOD_STRIP=1 modules_install >/dev/null
	mkimage -A arm -O linux -T kernel -C none -a 0x40008000 -e 0x40008000 -n "Linux $R" -d arch/arm/boot/zImage /out/kernel/uImage >/dev/null
	cp arch/arm/boot/zImage /out/kernel/
	cp arch/arm/boot/dts/allwinner/sun8i-r16-clockworkpi-cpi3.dtb /out/kernel/
	cp .config /out/kernel/config
	SZ=$(stat -c %s arch/arm/boot/zImage)
	[ "$SZ" -lt 8000000 ] || { echo "zImage is $SZ bytes: the board U-Boot loads at most 8 MB" >&2; exit 1; }
	tar -C /tmp/mod -czf /out/kernel/modules.tar.gz lib
	echo "$R" > /out/kernel/release
	echo "kernel $R built"
'
ls -la "$OUT/kernel"
