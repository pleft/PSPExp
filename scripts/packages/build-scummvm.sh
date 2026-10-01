#!/bin/sh
# ScummVM for the Omega2+, cross-compiled here with Zig: static, the SDL layer
# built in, only the engines of the freeware adventures the catalogue has
# (Beneath a Steel Sky, Flight of the Amazon Queen, Lure of the Temptress,
# Drascula). Their sound through integer decoders, the MT7688 having no FPU:
# libmad (fixed point; Amazon Queen's queen.1c is MP3) and Tremor (Vorbis,
# Drascula's music), with libogg. Sources in ../scummvm:
#   scummvm-2.9.0/   https://downloads.scummvm.org/frs/scummvm/2.9.0/scummvm-2.9.0.tar.xz
#   libogg-1.3.5/    https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.gz
#   libmad-0.15.1b/  https://downloads.sourceforge.net/project/mad/libmad/0.15.1b/libmad-0.15.1b.tar.gz
#   tremor/          git clone https://gitlab.xiph.org/xiph/tremor.git
# Called by scripts/omega/build.sh scummvm with:
#   W=work dir  CC=  CXX=  AR=  RANLIB=  SDLPREFIX=  CFLAGS_X=  LINK=  OUTBIN=
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
S="$ROOT/../scummvm"
SRC="$S/scummvm-2.9.0"
for d in "$SRC/configure" "$S/libogg-1.3.5/src/framing.c" "$S/libmad-0.15.1b/layer3.c" "$S/tremor/vorbisfile.c"; do
	[ -f "$d" ] || { echo "build-scummvm: $d missing (sources: see the top of this script)" >&2; exit 1; }
done
: "${W:?}" "${CC:?}" "${CXX:?}" "${AR:?}" "${RANLIB:?}" "${SDLPREFIX:?}" "${OUTBIN:?}"
D="$W/deps"

if [ ! -f "$D/lib/libvorbisidec.a" ]; then
	rm -rf "$D"; mkdir -p "$D/include/ogg" "$D/include/tremor" "$D/lib" "$D/obj"
	O="$S/libogg-1.3.5"
	sed -e 's/@INCLUDE_INTTYPES_H@/1/;s/@INCLUDE_STDINT_H@/1/;s/@INCLUDE_SYS_TYPES_H@/1/' \
	    -e 's/@SIZE16@/int16_t/;s/@USIZE16@/uint16_t/;s/@SIZE32@/int32_t/;s/@USIZE32@/uint32_t/;s/@SIZE64@/int64_t/;s/@USIZE64@/uint64_t/' \
	    "$O/include/ogg/config_types.h.in" > "$D/include/ogg/config_types.h"
	cp "$O/include/ogg/ogg.h" "$O/include/ogg/os_types.h" "$D/include/ogg/"
	for c in framing bitwise; do $CC -O2 -I"$D/include" -c "$O/src/$c.c" -o "$D/obj/ogg-$c.o"; done
	$AR rcs "$D/lib/libogg.a" "$D"/obj/ogg-*.o
	# libmad: its MIPS assembly uses a constraint ("=h") clang refuses; the
	# 64-bit C multiply compiles to the same mult
	M="$S/libmad-0.15.1b"
	cp "$M/mad.h" "$D/include/"
	for c in bit decoder fixed frame huffman layer12 layer3 stream synth timer version; do
		$CC -O2 -DFPM_64BIT -DSIZEOF_INT=4 -DSIZEOF_LONG=4 -DSIZEOF_LONG_LONG=8 -include stdlib.h -include string.h \
			-I"$M" -c "$M/$c.c" -o "$D/obj/mad-$c.o"
	done
	$AR rcs "$D/lib/libmad.a" "$D"/obj/mad-*.o
	T="$S/tremor"
	cp "$T/ivorbiscodec.h" "$T/ivorbisfile.h" "$D/include/tremor/"
	for c in block codebook floor0 floor1 info mapping0 mdct registry res012 sharedbook synthesis vorbisfile window; do
		[ -f "$T/$c.c" ] || continue
		$CC -O2 -I"$D/include" -I"$T" -c "$T/$c.c" -o "$D/obj/tremor-$c.o"
	done
	$AR rcs "$D/lib/libvorbisidec.a" "$D"/obj/tremor-*.o
fi

B="$W/build"
mkdir -p "$B"
cd "$B"
# configure sees only the board's prefix (not this machine's Homebrew
# libraries), no OpenGL (the layer draws ScummVM's 8-bit surface), nothing
# from the network; the SDL surface mode and its plain 1x scaler only
if [ ! -f config.mk ] || [ "$(cat flags 2>/dev/null)" != "$CXX $CFLAGS_X $LINK" ]; then
	PKG_CONFIG_LIBDIR="$SDLPREFIX/lib/pkgconfig" PKG_CONFIG_PATH= \
	CXX="$CXX" CC="$CC" AR="$AR" RANLIB="$RANLIB" CXXFLAGS="-O2 -Wno-error=date-time ${CFLAGS_X:-}" LDFLAGS="-static -s" LIBS="${LINK:-}" \
	"$SRC/configure" --host=mipsel-linux --backend=sdl --enable-static --enable-release --disable-debug \
		--disable-all-engines --enable-engine=sky,queen,lure,drascula --disable-detection-full \
		--with-sdl-prefix="$SDLPREFIX" --with-ogg-prefix="$D" --with-tremor-prefix="$D" --with-mad-prefix="$D" \
		--disable-vorbis --disable-flac --disable-png --disable-jpeg --disable-gif --disable-theoradec --disable-vpx \
		--disable-faad --disable-mpeg2 --disable-a52 --disable-fluidsynth --disable-fluidlite --disable-mt32emu \
		--disable-lua --disable-cloud --disable-libcurl --disable-sdlnet --disable-enet --disable-freetype2 \
		--disable-fribidi --disable-tts --opengl-mode=none --disable-opengl-game --disable-hq-scalers \
		--disable-edge-scalers --disable-bink --disable-eventrecorder --disable-discord --disable-imgui \
		--disable-taskbar --disable-system-dialogs --disable-readline --disable-alsa \
		--disable-sndio --disable-sonivox --disable-libunity --disable-translation >configure.log 2>&1 \
		|| { tail -20 configure.log >&2; exit 1; }
	echo "$CXX $CFLAGS_X $LINK" > flags
fi
for f in USE_MAD USE_TREMOR; do grep -q "^$f = 1" config.mk || { echo "build-scummvm: $f not configured (see $B/config.log)" >&2; exit 1; }; done
make -j8 scummvm >make.log 2>&1 || { grep -m 20 -E "error" make.log >&2; exit 1; }
cp scummvm "$OUTBIN"
ls -la "$OUTBIN"
