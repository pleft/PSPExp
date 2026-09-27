#!/bin/sh
# Jazz Jackrabbit's engine, OpenJazz (../jazz/openjazz), cross-compiled here
# with Zig: its bundled libraries (argparse, miniz, stb_rect_pack, the xmp
# module player) and the game, SDL2, no network, 480x272 by default (the
# PSP's screen: OpenJazz shows more of the level rather than scaling);
# settings in ~/.config/openjazz, the game data given as its argument.
# Called by scripts/packages/cross-glibc.sh (GameShell, Pi) and
# scripts/omega/build.sh jazz with the board's compilers; the same variables
# as build-nxengine.sh:
#   W=work dir  CC=  CXX=  AR=  SDLINC=  CFLAGS_X=  LINK=  OUTBIN=
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/../jazz/openjazz"
[ -f "$SRC/CMakeLists.txt" ] || { echo "OpenJazz source not found in $SRC (git clone https://github.com/AlisterT/openjazz)" >&2; exit 1; }
: "${W:?}" "${CC:?}" "${CXX:?}" "${AR:?}" "${SDLINC:?}" "${OUTBIN:?}"
mkdir -p "$W/lib"

# the SDL headers' generic config includes no libc headers: forced in
SDLF="-I$SDLINC -D_REENTRANT -include stdlib.h -include string.h -include math.h -include stdio.h ${CFLAGS_X:-}"
lib() {   # NAME FLAGS FILES...: one archive, rebuilt when missing
	_n=$1 _f=$2; shift 2
	[ -f "$W/lib/lib$_n.a" ] && return 0
	rm -rf "$W/obj-$_n"; mkdir -p "$W/obj-$_n"
	for _c in "$@"; do
		$CC -O2 -std=gnu99 -c $_f "$_c" -o "$W/obj-$_n/$(basename "$_c" .c).o" || exit 1
	done
	$AR rcs "$W/lib/lib$_n.a" "$W/obj-$_n"/*.o
}
E="$SRC/ext"
lib argparse "" "$E/argparse/argparse.c"
lib miniz "-DMINIZ_NO_ARCHIVE_APIS" "$E/miniz/miniz.c"
lib stb "" "$E/stb/stb_rect_pack.c"
lib xmp "-I$E/xmp -DLIBXMP_CORE_PLAYER -DLIBXMP_CORE_DISABLE_IT -DLIBXMP_STATIC -DHAVE_POWF" \
	$(ls "$E"/xmp/src/*.c | grep -v '/win32\.c$') "$E/xmp/src/loaders/common.c" "$E/xmp/src/loaders/iff.c" \
	"$E/xmp/src/loaders/masi_load.c" "$E/xmp/src/loaders/sample.c"

# the game: the sources its CMakeLists lists (network code compiles to
# nothing without USE_SOCKETS), every platform file (each guards itself)
sed -n '/^set(OJ_SOURCES/,/)/p' "$SRC/CMakeLists.txt" | grep -o 'src/[^ )]*\.cpp' | sed 's|//|/|g' > "$W/oj-sources.txt"
(cd "$SRC" && ls src/platforms/*.cpp) >> "$W/oj-sources.txt"
V=$(sed -n 's/^[[:space:]]*VERSION \([0-9]*\).*/\1/p' "$SRC/CMakeLists.txt" | head -1)
OJF="-O2 -std=c++14 -ffile-prefix-map=$SRC/= -DOJ_SDL2 -DDEFAULT_SCREEN_WIDTH=480 -DDEFAULT_SCREEN_HEIGHT=272
	-DOJ_VERSION='\"$V\"' -DOJ_DATE='\"$(git -C "$SRC" log -1 --format=%cs 2>/dev/null || echo unknown)\"'
	-DOJ_URL='\"http://alister.eu/jazz/oj/\"' -DOJ_BUGREPORT='\"https://github.com/AlisterT/openjazz/issues\"' -DLIBXMP_STATIC
	-I$SRC/src -I$E/argparse -I$E/miniz -I$E/stb -I$E/xmp $SDLF"
OJF=$(echo $OJF)   # one line
mkdir -p "$W/obj-oj"
[ "$(cat "$W/oj-flags" 2>/dev/null)" = "$CXX $OJF" ] || rm -f "$W"/obj-oj/*.o
echo "$CXX $OJF" > "$W/oj-flags"
rm -f "$W/oj-errors.log"
export CXX OJF SRC W
xargs -P 8 -n 1 sh -c 'o="$W/obj-oj/$(echo "$1" | tr / _ | sed "s/\.cpp$/.o/")"; [ "$o" -nt "$SRC/$1" ] && exit 0; eval "$CXX $OJF -c \"$SRC/$1\" -o \"$o\"" 2>>"$W/oj-errors.log" || { echo "failed: $1" >> "$W/oj-errors.log"; exit 255; }' sh < "$W/oj-sources.txt" \
	|| { grep -m 20 -E 'error|failed' "$W/oj-errors.log" >&2; exit 1; }
$CXX -s -o "$OUTBIN" "$W"/obj-oj/*.o "$W/lib/libargparse.a" "$W/lib/libminiz.a" "$W/lib/libstb.a" "$W/lib/libxmp.a" $LINK
ls -la "$OUTBIN"
