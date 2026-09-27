#!/bin/sh
# Cave Story's engine, NXEngine-evo (../cavestory/nxengine-evo), cross-compiled
# here with Zig for a board with no compiler to spare (GameShell, Omega2+).
# Called by scripts/gameshell/build.sh cavestory and scripts/omega/build.sh
# cavestory with the board's compilers; builds, all static and small:
#   zlib, libpng           NXEngine reads and writes PNGs itself
#   SDL2_image             PNG and BMP through its built-in stb_image
#   SDL2_mixer             WAV only (the game synthesises its own music)
# from their sources (no configure or CMake for four libraries), then the
# game with the 480x272 default (cavestory_patch). SDL2 itself is the board's:
# SDLINC has its headers, LINK what the program links against for it.
#
#   W=work dir  CC=  CXX=  AR=  SDLINC=  CFLAGS_X=(extra, e.g. -include a rename
#   header)  LINK=(SDL and system libraries)  OUTBIN=program path
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/../cavestory/nxengine-evo"
[ -f "$SRC/CMakeLists.txt" ] || { echo "NXEngine-evo source not found in $SRC (git clone https://github.com/nxengine/nxengine-evo)" >&2; exit 1; }
: "${W:?}" "${CC:?}" "${CXX:?}" "${AR:?}" "${SDLINC:?}" "${OUTBIN:?}"
ZLIB_VER=1.3.1 PNG_VER=1.6.44 IMG_VER=2.8.2 MIX_VER=2.8.1
CACHE="$ROOT/build/pkg-cache"
mkdir -p "$W/src" "$W/lib" "$W/include" "$CACHE"
. "$ROOT/scripts/packages/recipes/_lib.sh"

unpack() {   # URL DIR: the tarball, cached, unpacked once into $W/src
	[ -d "$W/src/$2" ] || tar -C "$W/src" -xzf "$(fetch "$1")"
}
unpack "https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.gz" "zlib-$ZLIB_VER"
unpack "https://download.sourceforge.net/libpng/libpng-$PNG_VER.tar.gz" "libpng-$PNG_VER"
unpack "https://github.com/libsdl-org/SDL_image/releases/download/release-$IMG_VER/SDL2_image-$IMG_VER.tar.gz" "SDL2_image-$IMG_VER"
unpack "https://github.com/libsdl-org/SDL_mixer/releases/download/release-$MIX_VER/SDL2_mixer-$MIX_VER.tar.gz" "SDL2_mixer-$MIX_VER"

# every C file of a library into one archive: NAME FLAGS FILES...
lib() {
	_n=$1 _f=$2; shift 2
	[ -f "$W/lib/lib$_n.a" ] && return 0
	rm -rf "$W/obj-$_n"; mkdir -p "$W/obj-$_n"
	for _c in "$@"; do
		$CC -O2 -c $_f "$_c" -o "$W/obj-$_n/$(basename "$_c" .c).o" || exit 1
	done
	$AR rcs "$W/lib/lib$_n.a" "$W/obj-$_n"/*.o
}
Z="$W/src/zlib-$ZLIB_VER"
lib z "-DHAVE_UNISTD_H -I$Z" $(for f in adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil; do echo "$Z/$f.c"; done)
P="$W/src/libpng-$PNG_VER"
cp "$P/scripts/pnglibconf.h.prebuilt" "$P/pnglibconf.h"
# no SIMD paths (they need their own source directories)
PNGF="-I$P -I$Z -DPNG_ARM_NEON_OPT=0 -DPNG_MIPS_MSA_OPT=0 -DPNG_MIPS_MMI_OPT=0 -DPNG_POWERPC_VSX_OPT=0 -DPNG_INTEL_SSE_OPT=0 -DPNG_LOONGARCH_LSX_OPT=0"
lib png "$PNGF" $(for f in png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil pngset pngtrans pngwio pngwrite pngwtran pngwutil; do echo "$P/$f.c"; done)
cp "$P/png.h" "$P/pngconf.h" "$P/pnglibconf.h" "$Z/zlib.h" "$Z/zconf.h" "$W/include/"
# the SDL libraries see what the game sees (CFLAGS_X: the Omega's rename header)
SDLF="-I$SDLINC -D_REENTRANT -include stdlib.h -include string.h -include math.h -include stdio.h ${CFLAGS_X:-}"
I="$W/src/SDL2_image-$IMG_VER"
lib SDL2_image "$SDLF -I$I/include -DLOAD_BMP -DLOAD_PNG -DUSE_STBIMAGE" "$I"/src/IMG*.c
M="$W/src/SDL2_mixer-$MIX_VER"
lib SDL2_mixer "$SDLF -I$M/include -I$M/src -I$M/src/codecs -DMUSIC_WAV" "$M"/src/*.c "$M"/src/codecs/*.c
cp "$I/include/SDL_image.h" "$M/include/SDL_mixer.h" "$W/include/"

# the game: its sources as its CMakeLists lists them, compiled 8 at a time
G="$W/nxengine"
rm -rf "$G"; mkdir -p "$G"
{ COPYFILE_DISABLE=1 tar --no-xattrs -C "$SRC" --exclude .git --exclude build -cf - src deps ; } 2>/dev/null | tar -C "$G" -xf -
(cd "$G" && eval "$(cavestory_patch)")
mkdir -p "$W/obj-nx"
(cd "$G" && ls src/*.cpp src/TextBox/*.cpp src/autogen/*.cpp src/common/*.cpp src/Utils/*.cpp src/endgame/*.cpp \
	src/graphics/*.cpp src/intro/*.cpp src/pause/*.cpp src/siflib/*.cpp src/sound/*.cpp src/i18n/*.cpp; find src/ai -name '*.cpp') |
	grep -v '/extract/' > "$W/nx-sources.txt"
NXF="-O2 -ffile-prefix-map=$G/= -std=c++14 -DHAVE_UNIX_LIKE -DPORTABLE -I$G -I$G/deps -I$W/include -I$SDLINC -D_REENTRANT -include stdlib.h -include string.h -include math.h -include stdio.h ${CFLAGS_X:-}"
# new flags: every object again
[ "$(cat "$W/nx-flags" 2>/dev/null)" = "$CXX $NXF" ] || rm -f "$W"/obj-nx/*.o
echo "$CXX $NXF" > "$W/nx-flags"
export CXX NXF G W
rm -f "$W/nx-errors.log"
xargs -P 8 -n 1 sh -c 'o="$W/obj-nx/$(echo "$1" | tr / _ | sed "s/\.cpp$/.o/")"; [ "$o" -nt "$G/$1" ] && exit 0; $CXX $NXF -c "$G/$1" -o "$o" 2>>"$W/nx-errors.log" || { echo "failed: $1" >> "$W/nx-errors.log"; exit 255; }' sh < "$W/nx-sources.txt" \
	|| { grep -m 20 -E 'error|failed' "$W/nx-errors.log" >&2; exit 1; }
$CXX -s -o "$OUTBIN" "$W"/obj-nx/*.o "$W/lib/libSDL2_image.a" "$W/lib/libSDL2_mixer.a" "$W/lib/libpng.a" "$W/lib/libz.a" $LINK
ls -la "$OUTBIN"
