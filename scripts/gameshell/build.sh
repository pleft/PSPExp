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
#   scripts/gameshell/build.sh          both
#   scripts/gameshell/build.sh tools    only the static tools
#   scripts/gameshell/build.sh shim     only the shim
#   scripts/gameshell/build.sh tyrian   OpenTyrian (SDL2) for the PSP, compiled
#      on the GameShell itself (its gcc, its libSDL2) from ../tyrian/opentyrian
#      into build/gameshell/bin/opentyrian, which the opentyrian package
#      (scripts/packages/pspexp-pkg-build) ships; HOST=user@host (default cpi@gameshell).
#      The launcher's own OpenTyrian is SDL 1.2, which the shim cannot reach.
#   scripts/gameshell/build.sh retro    pspexp-retro and a libretro core (CORE=<core>),
#      cross-compiled here -> build/gameshell/bin/, for the emulator packages
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/gameshell"
WHAT="${1:-all}"

if [ "$WHAT" = retro ]; then
	# Cross-compiled here like the shim (the card image has no compiler):
	# the core as a shared library, the player linked against a stand-in
	# libSDL2-2.0.so.0 that only names the functions it uses; on the board the
	# image's own SDL2 answers them. CORE=<core> (../retro/libretro-<core>).
	CORE="${CORE:-quicknes}"
	SRC="$ROOT/../retro/libretro-$CORE"
	INC="$OUT/gnu/src/SDL2-${SDL_VER:-2.30.12}/include"
	[ -f "$INC/SDL.h" ] || { echo "SDL2 headers missing: run scripts/gameshell/build.sh shim first" >&2; exit 1; }
	[ -f "$SRC/Makefile.libretro" ] || [ -f "$SRC/Makefile" ] || { echo "core source not found in $SRC" >&2; exit 1; }
	command -v zig >/dev/null || { echo "zig not found (brew install zig)" >&2; exit 1; }
	R="$OUT/retro"; mkdir -p "$R" "$OUT/bin" "$ROOT/build/zig-cache"
	T="arm-linux-gnueabihf.${GLIBC_VER:-2.35}"
	for t in cc cxx; do
		case $t in cc) cmd="zig cc -target $T -mcpu=cortex_a7" ;; cxx) cmd="zig c++ -target $T -mcpu=cortex_a7" ;; esac
		printf '#!/bin/sh\nexport ZIG_GLOBAL_CACHE_DIR="%s" ZIG_LOCAL_CACHE_DIR="%s"\nexec %s "$@"\n' \
			"$ROOT/build/zig-cache" "$ROOT/build/zig-cache" "$cmd" > "$R/z$t"
		chmod +x "$R/z$t"
	done
	# the player, and the stand-in SDL2 from the functions it calls
	"$R/zcc" -O2 -c -D_REENTRANT -I"$INC" "$ROOT/tools/pspexp-retro/main.c" -o "$R/pspexp-retro.o"
	nm -u "$R/pspexp-retro.o" | awk '{print $NF}' | grep '^SDL_' | sort -u | sed 's/.*/void &(void) {}/' > "$R/sdl-stub.c"
	"$R/zcc" -shared -fPIC -Wl,-soname,libSDL2-2.0.so.0 "$R/sdl-stub.c" -o "$R/libSDL2.so"
	"$R/zcc" -o "$OUT/bin/pspexp-retro" "$R/pspexp-retro.o" -L"$R" -lSDL2 -ldl -lm
	# the core: its objects by its own makefile, linked here (Zig's linker
	# lacks some flags the core makefiles pass)
	W="$R/src-$CORE"; rm -rf "$W"; mkdir -p "$W"
	{ COPYFILE_DISABLE=1 tar --no-xattrs -C "$SRC" --exclude .git -cf - . ; } 2>/dev/null | tar -C "$W" -xf -
	find "$W" \( -name '*.o' -o -name '*.so' -o -name '*.dylib' \) -delete
	MK=Makefile.libretro; [ -f "$W/$MK" ] || MK=Makefile
	# per core: PicoDrive picks ARM assembly (Cyclone, DrZ80, renderers) from
	# the compiler's target name, which Zig's triple with a glibc version
	# breaks: its portable C cores instead
	case "$CORE" in
	picodrive) EXTRA="ARCH=generic" ;;   # its C 68000 (FAME) and Z80 (CZ80) cores, no assembly
	*) EXTRA= ;;
	esac
	make -C "$W" -f "$MK" -j8 platform=unix CC="$R/zcc" CXX="$R/zcxx" $EXTRA >"$R/core-$CORE.log" 2>&1 || true
	OBJS=$(find "$W" -name '*.o' | sort)
	[ -n "$OBJS" ] || { tail -20 "$R/core-$CORE.log"; exit 1; }
	"$R/zcxx" -shared -fPIC -o "$OUT/bin/${CORE}_libretro.so" $OBJS -lm
	# every symbol the core needs must come from the C library: anything else
	# is an object its build left out (it would fail only when loaded)
	U=$(nm -D --undefined-only "$OUT/bin/${CORE}_libretro.so" 2>/dev/null | awk '{print $NF}' | grep -vE '@|^_|^(__cxa|__gxx|__aeabi|__stack_chk)' || true)
	[ -z "$U" ] || { echo "${CORE}_libretro.so: undefined symbols (objects missing, see $R/core-$CORE.log):" >&2; echo "$U" | head -10 >&2; exit 1; }
	ls -la "$OUT/bin/pspexp-retro" "$OUT/bin/${CORE}_libretro.so"
	exit 0
fi

if [ "$WHAT" = tyrian ]; then
	GS="${HOST:-${GAMESHELL:-cpi@gameshell}}"
	SRC="$ROOT/../tyrian/opentyrian"
	INC="$OUT/gnu/src/SDL2-${SDL_VER:-2.30.12}/include"
	[ -f "$SRC/Makefile" ] || { echo "OpenTyrian source not found in $SRC (git clone https://github.com/opentyrian/opentyrian)" >&2; exit 1; }
	[ -f "$INC/SDL.h" ] || { echo "SDL2 headers missing: run scripts/gameshell/build.sh shim first" >&2; exit 1; }
	# the source tree's generic SDL_config.h includes no libc headers (math,
	# stdlib: roundf/sinf would be implicitly int), hence the -include flags
	{ COPYFILE_DISABLE=1 tar --no-xattrs -C "$SRC" --exclude .git --exclude obj --exclude opentyrian -cf - . ; } 2>/dev/null |
		ssh "$GS" 'rm -rf ~/pspexp/tyrian-src && mkdir -p ~/pspexp/tyrian-src && tar -C ~/pspexp/tyrian-src -xf -'
	{ COPYFILE_DISABLE=1 tar --no-xattrs -C "$INC" -cf - . ; } 2>/dev/null |
		ssh "$GS" 'rm -rf ~/pspexp/sdl2-include && mkdir -p ~/pspexp/sdl2-include/SDL2 && tar -C ~/pspexp/sdl2-include/SDL2 -xf -'
	ssh "$GS" 'set -e; ln -sf /lib/arm-linux-gnueabihf/libSDL2-2.0.so.0 ~/pspexp/libSDL2.so
		make -C ~/pspexp/tyrian-src -j4 WITH_NETWORK=false \
			SDL_CPPFLAGS="-I$HOME/pspexp/sdl2-include/SDL2 -D_REENTRANT -include stdlib.h -include string.h -include math.h -include stdio.h" \
			SDL_LDFLAGS="-L$HOME/pspexp" SDL_LDLIBS="-lSDL2 -lm" 2>&1 | grep -E "error|implicit" || true
		ls -la ~/pspexp/tyrian-src/opentyrian'
	mkdir -p "$OUT/bin"; scp -q "$GS:pspexp/tyrian-src/opentyrian" "$OUT/bin/opentyrian"
	echo "build/gameshell/bin/opentyrian: package it with scripts/packages/pspexp-pkg-build build opentyrian"
	exit 0
fi

if [ "$WHAT" = all ] || [ "$WHAT" = tools ]; then
	env ZIG_TARGET=arm-linux-musleabihf ZIG_CPU=cortex_a7 CROSS_NAME=gameshell \
		CROSS_HOST=arm-linux-musleabihf CROSS_PROC=arm "$ROOT/scripts/omega/build.sh"
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
cp "$ROOT/scripts/board/pspexp-run" "$OUT/lib/"
ls -la "$OUT/lib"
echo "shim for $TARGET in build/gameshell/lib"
