#!/bin/sh
# Simutrans (../simutrans/simutrans), cross-compiled here with Zig: zlib,
# libpng, bzip2 and FreeType static, then Simutrans's own Makefile (SDL2
# backend; no MIDI music: no FluidSynth) compiling with the
# board's compilers, and the program linked here like the other games.
# Called by scripts/packages/cross-glibc.sh (GameShell, Pi) and
# scripts/omega/build.sh simutrans; the same variables as build-nxengine.sh:
#   W=work dir  CC=  CXX=  AR=  SDLINC=  CFLAGS_X=  LINK=  OUTBIN=
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/../simutrans/simutrans"
[ -f "$SRC/Makefile" ] || { echo "Simutrans source not found in $SRC (git clone https://github.com/simutrans/simutrans)" >&2; exit 1; }
: "${W:?}" "${CC:?}" "${CXX:?}" "${AR:?}" "${SDLINC:?}" "${OUTBIN:?}"
ZLIB_VER=1.3.1 PNG_VER=1.6.44 BZ2_VER=1.0.8 FT_VER=2.13.3
CACHE="$ROOT/build/pkg-cache"
mkdir -p "$W/src" "$W/lib" "$W/include" "$CACHE"
. "$ROOT/scripts/packages/recipes/_lib.sh"
unpack() { [ -d "$W/src/$2" ] || tar -C "$W/src" -xzf "$(fetch "$1")"; }
unpack "https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.gz" "zlib-$ZLIB_VER"
unpack "https://download.sourceforge.net/libpng/libpng-$PNG_VER.tar.gz" "libpng-$PNG_VER"
unpack "https://sourceware.org/pub/bzip2/bzip2-$BZ2_VER.tar.gz" "bzip2-$BZ2_VER"
unpack "https://download.savannah.gnu.org/releases/freetype/freetype-$FT_VER.tar.gz" "freetype-$FT_VER"
lib() {   # NAME FLAGS FILES...
	_n=$1 _f=$2; shift 2
	[ -f "$W/lib/lib$_n.a" ] && return 0
	rm -rf "$W/obj-$_n"; mkdir -p "$W/obj-$_n"
	for _c in "$@"; do $CC -O2 -c $_f "$_c" -o "$W/obj-$_n/$(basename "$_c" .c).o" || exit 1; done
	$AR rcs "$W/lib/lib$_n.a" "$W/obj-$_n"/*.o
}
Z="$W/src/zlib-$ZLIB_VER"
lib z "-DHAVE_UNISTD_H -I$Z" $(for f in adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil; do echo "$Z/$f.c"; done)
P="$W/src/libpng-$PNG_VER"
cp "$P/scripts/pnglibconf.h.prebuilt" "$P/pnglibconf.h"
lib png "-I$P -I$Z -DPNG_ARM_NEON_OPT=0 -DPNG_MIPS_MSA_OPT=0 -DPNG_MIPS_MMI_OPT=0 -DPNG_POWERPC_VSX_OPT=0 -DPNG_INTEL_SSE_OPT=0 -DPNG_LOONGARCH_LSX_OPT=0" \
	$(for f in png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil pngset pngtrans pngwio pngwrite pngwtran pngwutil; do echo "$P/$f.c"; done)
B="$W/src/bzip2-$BZ2_VER"
lib bz2 "-I$B" $(for f in blocksort huffman crctable randtable compress decompress bzlib; do echo "$B/$f.c"; done)
# FreeType (Simutrans's fonts), its default modules, as its INSTALL.ANY lists them
F="$W/src/freetype-$FT_VER"
lib freetype "-DFT2_BUILD_LIBRARY -I$F/include" \
	$(for f in ftsystem ftinit ftdebug ftbase ftbbox ftglyph ftbdf ftbitmap ftcid ftfstype ftgasp ftgxval ftmm ftotval ftpatent ftpfr ftstroke ftsynth fttype1 ftwinfnt; do echo "$F/src/base/$f.c"; done) \
	"$F/src/autofit/autofit.c" "$F/src/bdf/bdf.c" "$F/src/cff/cff.c" "$F/src/cid/type1cid.c" "$F/src/pcf/pcf.c" "$F/src/pfr/pfr.c" \
	"$F/src/psaux/psaux.c" "$F/src/pshinter/pshinter.c" "$F/src/psnames/psnames.c" "$F/src/raster/raster.c" "$F/src/sdf/sdf.c" \
	"$F/src/svg/svg.c" "$F/src/sfnt/sfnt.c" "$F/src/smooth/smooth.c" "$F/src/truetype/truetype.c" "$F/src/type1/type1.c" \
	"$F/src/type42/type42.c" "$F/src/winfonts/winfnt.c" "$F/src/gzip/ftgzip.c" "$F/src/lzw/ftlzw.c" "$F/src/bzip2/ftbzip2.c" "$F/src/cache/ftcache.c"
cp "$P/png.h" "$P/pngconf.h" "$P/pnglibconf.h" "$Z/zlib.h" "$Z/zconf.h" "$B/bzlib.h" "$W/include/"
cp -R "$F/include/." "$W/include/"
# the code includes <SDL2/SDL.h>
mkdir -p "$W/sdlinc"; ln -sfn "$SDLINC" "$W/sdlinc/SDL2"

# Simutrans's Makefile, out of tree; SDL's flags through a stand-in sdl2-config
SDLF="-I$SDLINC -I$W/sdlinc -D_REENTRANT -include stdlib.h -include string.h -include math.h -include stdio.h ${CFLAGS_X:-}"
printf '#!/bin/sh\ncase "$1" in --cflags) echo "%s" ;; *) echo ;; esac\n' "$SDLF" > "$W/sdl2-config"
chmod +x "$W/sdl2-config"
BD="$W/build"
FLAGSTAMP="$CXX $SDLF"
[ "$(cat "$W/st-flags" 2>/dev/null)" = "$FLAGSTAMP" ] || rm -rf "$BD"
echo "$FLAGSTAMP" > "$W/st-flags"
# the header the Makefile would write (macOS's make 3.81 reads its '#' as a comment)
[ -f "$SRC/src/simutrans/revision.h" ] || printf '#define REVISION\n' > "$SRC/src/simutrans/revision.h"
# the program's link fails here on purpose (no SDL to link against yet): only the objects count
CFLAGS="-I$W/include -ffile-prefix-map=$SRC/= -Wno-error=date-time" LDFLAGS="-L$W/lib" make -C "$SRC" -k -j8 \
	BACKEND=sdl2 OSTYPE=linux OPTIMISE=1 MULTI_THREAD=1 CC="$CC" CXX="$CXX" HOSTCC="$CC" HOSTCXX="$CXX" \
	SDL2_CONFIG="$W/sdl2-config" FREETYPE_CONFIG= FONTCONFIG_CONFIG= PNG_CONFIG= USE_UPNP=0 USE_ZSTD=0 \
	BUILDDIR="$BD" PROGDIR="$BD" CFG=pspexp BUILDCONFIG_FILES="Makefile common.mk" > "$W/make.log" 2>&1 || true
find "$BD" -name '*.o' | sort > "$W/st-objects.txt"
[ -s "$W/st-objects.txt" ] || { grep -m 20 -E 'error' "$W/make.log" >&2; exit 1; }
if grep -q -E ' error:|Error [0-9]' "$W/make.log" && ! grep -q 'LD ' "$W/make.log"; then
	grep -m 20 -E ' error:' "$W/make.log" >&2; exit 1
fi
# for cross-glibc.sh: the objects as one archive (it reads what they call)
rm -f "$W/lib/libsimutrans-objs.a"
$AR rcs "$W/lib/libsimutrans-objs.a" $(cat "$W/st-objects.txt")
$CXX -s -o "$OUTBIN" $(cat "$W/st-objects.txt") "$W/lib/libfreetype.a" "$W/lib/libpng.a" "$W/lib/libbz2.a" "$W/lib/libz.a" $LINK
ls -la "$OUTBIN"
