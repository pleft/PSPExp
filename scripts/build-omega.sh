#!/bin/sh
# Onion Omega2 / Omega2+ (MT7688, MIPS32r2 little-endian, no FPU, OpenWrt)
# host build, cross-compiled on this machine with Zig: static soft-float
# musl binaries that run on the Omega with nothing installed.
#
#   brew install zig        (or any Zig >= 0.13 on PATH)
#   scripts/build-omega.sh
#   scp build/omega/bin/* root@omega:/tmp/    (or the SD card)
#
# Builds libusb 1.0 from source, then pspexp-bench and pspexp-sprite-demo.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/omega"
LIBUSB_VER="${LIBUSB_VER:-1.0.29}"
command -v zig >/dev/null || { echo "zig not found (brew install zig)" >&2; exit 1; }
mkdir -p "$OUT/src" "$OUT/prefix" "$OUT/bin" "$ROOT/build/zig-cache"

# compiler wrappers: one path each, as autotools and CMake want
for t in cc ar ranlib; do
	case $t in
		cc) cmd='zig cc -target mipsel-linux-musleabi -mcpu=mips32r2' ;;
		ar) cmd='zig ar' ;;
		ranlib) cmd='zig ranlib' ;;
	esac
	cat > "$OUT/z$t" <<W
#!/bin/sh
export ZIG_GLOBAL_CACHE_DIR="$ROOT/build/zig-cache" ZIG_LOCAL_CACHE_DIR="$ROOT/build/zig-cache"
exec $cmd "\$@"
W
	chmod +x "$OUT/z$t"
done

# libusb (static, no udev)
if [ ! -f "$OUT/prefix/lib/libusb-1.0.a" ]; then
	cd "$OUT/src"
	[ -d "libusb-$LIBUSB_VER" ] || curl -sL "https://github.com/libusb/libusb/releases/download/v$LIBUSB_VER/libusb-$LIBUSB_VER.tar.bz2" | tar xjf -
	cd "libusb-$LIBUSB_VER"
	./configure --host=mipsel-linux-musl --prefix="$OUT/prefix" --disable-udev --enable-static --disable-shared \
		CC="$OUT/zcc" AR="$OUT/zar" RANLIB="$OUT/zranlib" >"$OUT/libusb-configure.log" 2>&1
	make -j8 install >"$OUT/libusb-make.log" 2>&1
fi

# PSPExp host tools
cat > "$OUT/toolchain.cmake" <<T
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mipsel)
set(CMAKE_C_COMPILER "$OUT/zcc")
set(CMAKE_AR "$OUT/zar")
set(CMAKE_RANLIB "$OUT/zranlib")
set(CMAKE_FIND_ROOT_PATH "$OUT/prefix")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -s")
T
PKG_CONFIG_LIBDIR="$OUT/prefix/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR= \
cmake -S "$ROOT" -B "$OUT/cmake" -DCMAKE_TOOLCHAIN_FILE="$OUT/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
	-DPSPEXP_BUILD_TESTS=OFF -DPSPEXP_BUILD_SDL=OFF >/dev/null
PKG_CONFIG_LIBDIR="$OUT/prefix/lib/pkgconfig" cmake --build "$OUT/cmake" -j8 --target pspexp-bench pspexp-sprite-demo
cp "$OUT/cmake/pspexp-bench" "$OUT/cmake/pspexp-sprite-demo" "$OUT/bin/"
"$OUT/zcc" --version >/dev/null 2>&1 || true
ls -la "$OUT/bin"
echo "copy build/omega/bin/* to the Omega (static, soft-float MIPS32r2)"
