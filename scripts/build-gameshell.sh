#!/bin/sh
# ClockworkPi GameShell (Allwinner R16: 4x Cortex-A7, ARMv7 hard-float) host
# build, cross-compiled on this machine with Zig:
#   1. static musl binaries that run on any GameShell image:
#      pspexp-term, pspexp-bench, pspexp-sprite-demo  ->  build/gameshell/bin/
#   2. the sdl-pspexp shim as a glibc armhf shared library for LD_PRELOAD
#      into the image's own SDL2 programs (chocolate-doom, RetroArch's sdl2
#      driver, ...)                                   ->  build/gameshell/lib/
#      libusb is built PIC and linked in; SDL symbols stay undefined and
#      resolve from the program's libSDL2-2.0.so.0 at load time.
#   scripts/build-gameshell.sh          both
#   scripts/build-gameshell.sh tools    only the static tools
#   scripts/build-gameshell.sh shim     only the shim
#   scripts/build-gameshell.sh tyrian   OpenTyrian (SDL2) for the PSP, compiled
#      on the GameShell itself (its gcc, its libSDL2) from ../tyrian/opentyrian,
#      installed with pspexp-tyrian; GAMESHELL=user@host (default cpi@gameshell).
#      The launcher's own OpenTyrian is SDL 1.2, which the shim cannot reach.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/gameshell"
WHAT="${1:-all}"

if [ "$WHAT" = tyrian ]; then
	GS="${GAMESHELL:-cpi@gameshell}"
	SRC="$ROOT/../tyrian/opentyrian"
	INC="$OUT/gnu/src/SDL2-${SDL_VER:-2.30.12}/include"
	[ -f "$SRC/Makefile" ] || { echo "OpenTyrian source not found in $SRC (git clone https://github.com/opentyrian/opentyrian)" >&2; exit 1; }
	[ -f "$INC/SDL.h" ] || { echo "SDL2 headers missing: run scripts/build-gameshell.sh shim first" >&2; exit 1; }
	# the source tree's generic SDL_config.h includes no libc headers (math,
	# stdlib: roundf/sinf would be implicitly int), hence the -include flags
	{ COPYFILE_DISABLE=1 tar --no-xattrs -C "$SRC" --exclude .git --exclude obj --exclude opentyrian -cf - . ; } 2>/dev/null |
		ssh "$GS" 'rm -rf ~/pspexp/tyrian-src && mkdir -p ~/pspexp/tyrian-src && tar -C ~/pspexp/tyrian-src -xf -'
	{ COPYFILE_DISABLE=1 tar --no-xattrs -C "$INC" -cf - . ; } 2>/dev/null |
		ssh "$GS" 'rm -rf ~/pspexp/sdl2-include && mkdir -p ~/pspexp/sdl2-include/SDL2 && tar -C ~/pspexp/sdl2-include/SDL2 -xf -'
	scp -q "$ROOT/scripts/gameshell/pspexp-tyrian" "$GS:/tmp/pspexp-tyrian"
	ssh "$GS" 'set -e; ln -sf /lib/arm-linux-gnueabihf/libSDL2-2.0.so.0 ~/pspexp/libSDL2.so
		make -C ~/pspexp/tyrian-src -j4 WITH_NETWORK=false \
			SDL_CPPFLAGS="-I$HOME/pspexp/sdl2-include/SDL2 -D_REENTRANT -include stdlib.h -include string.h -include math.h -include stdio.h" \
			SDL_LDFLAGS="-L$HOME/pspexp" SDL_LDLIBS="-lSDL2 -lm" 2>&1 | grep -E "error|implicit" || true
		cp ~/pspexp/tyrian-src/opentyrian ~/pspexp/opentyrian
		sudo install -m 755 /tmp/pspexp-tyrian /usr/local/bin/pspexp-tyrian
		ls -la ~/pspexp/opentyrian'
	echo "on the GameShell: pspexp-tyrian"
	exit 0
fi

if [ "$WHAT" = all ] || [ "$WHAT" = tools ]; then
	env ZIG_TARGET=arm-linux-musleabihf ZIG_CPU=cortex_a7 CROSS_NAME=gameshell \
		CROSS_HOST=arm-linux-musleabihf CROSS_PROC=arm "$ROOT/scripts/build-omega.sh"
fi
[ "$WHAT" = all ] || [ "$WHAT" = shim ] || exit 0

# ---- the shim: glibc 2.35 (Armbian jammy), PIC, Cortex-A7 ----------------------
GNU="$OUT/gnu"
GLIBC="${GLIBC_VER:-2.35}"
TARGET="arm-linux-gnueabihf.$GLIBC"
LIBUSB_VER="${LIBUSB_VER:-1.0.29}"
SDL_VER="${SDL_VER:-2.30.12}"
command -v zig >/dev/null || { echo "zig not found (brew install zig)" >&2; exit 1; }
mkdir -p "$GNU/src" "$GNU/prefix" "$OUT/lib" "$ROOT/build/zig-cache"
for t in cc ar ranlib; do
	case $t in
		cc) cmd="zig cc -target $TARGET -mcpu=cortex_a7" ;;
		ar) cmd='zig ar' ;;
		ranlib) cmd='zig ranlib' ;;
	esac
	cat > "$GNU/z$t" <<W
#!/bin/sh
export ZIG_GLOBAL_CACHE_DIR="$ROOT/build/zig-cache" ZIG_LOCAL_CACHE_DIR="$ROOT/build/zig-cache"
exec $cmd "\$@"
W
	chmod +x "$GNU/z$t"
done

# libusb, static but position-independent (it ends up inside the .so)
if [ ! -f "$GNU/prefix/lib/libusb-1.0.a" ]; then
	cd "$GNU/src"
	[ -d "libusb-$LIBUSB_VER" ] || curl -sL "https://github.com/libusb/libusb/releases/download/v$LIBUSB_VER/libusb-$LIBUSB_VER.tar.bz2" | tar xjf -
	cd "libusb-$LIBUSB_VER"
	./configure --host=arm-linux-gnueabihf --prefix="$GNU/prefix" --disable-udev --enable-static --disable-shared \
		CC="$GNU/zcc" AR="$GNU/zar" RANLIB="$GNU/zranlib" CFLAGS="-O2 -fPIC" >"$GNU/libusb-configure.log" 2>&1
	make -j8 install >"$GNU/libusb-make.log" 2>&1
	cd "$ROOT"
fi

# SDL2 headers only (the shim is compiled against the API; the library is the image's)
SDLINC="$GNU/src/SDL2-$SDL_VER/include"
if [ ! -f "$SDLINC/SDL.h" ]; then
	curl -sL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz" | tar -C "$GNU/src" -xzf -
fi

# PSPExp host libraries (PIC) and the shim
cat > "$GNU/toolchain.cmake" <<T
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER "$GNU/zcc")
set(CMAKE_AR "$GNU/zar")
set(CMAKE_RANLIB "$GNU/zranlib")
set(CMAKE_FIND_ROOT_PATH "$GNU/prefix")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
T
PKG_CONFIG_LIBDIR="$GNU/prefix/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR= \
cmake -S "$ROOT" -B "$GNU/cmake" -DCMAKE_TOOLCHAIN_FILE="$GNU/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
	-DPSPEXP_BUILD_TESTS=OFF -DPSPEXP_BUILD_SDL=OFF >/dev/null
PKG_CONFIG_LIBDIR="$GNU/prefix/lib/pkgconfig" cmake --build "$GNU/cmake" -j8 --target pspexp pspexp_runtime pspexp_null pspexp_tcp pspexp_usb >/dev/null
C="$GNU/cmake"
"$GNU/zcc" -O2 -fPIC -shared -Wl,-soname,libsdl-pspexp.so -D_GNU_SOURCE -DPSPEXP_HAVE_USB -DPSPEXP_NO_SDL_EXECUTOR -D_REENTRANT \
	-I"$SDLINC" -I"$GNU/prefix/include" "$ROOT/compatibility/sdl-pspexp/sdl_pspexp.c" \
	"$C/libpspexp_usb.a" "$C/libpspexp_tcp.a" "$C/libpspexp_null.a" "$C/libpspexp.a" "$C/libpspexp_runtime.a" "$C/libpspexp_protocol.a" \
	"$GNU/prefix/lib/libusb-1.0.a" -ldl -lm -lpthread -o "$OUT/lib/libsdl-pspexp.so"
cp "$ROOT/scripts/gameshell/pspexp-run" "$OUT/lib/"
ls -la "$OUT/lib"
echo "shim for $TARGET in build/gameshell/lib"
