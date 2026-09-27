#!/bin/sh
# title: Simutrans with pak64 (drawn by the PSP)
# Simutrans 124.5, the transport simulation, with its standard graphics pak64:
# both under the Artistic License, shipped whole. The program cross-compiled
# here (scripts/packages/build-simutrans.sh, from the 124.5 tag of
# ../simutrans/simutrans); its base data (settings, texts, fonts, themes) from
# the official 124.5 Linux release, its x86 program left out; pak64 124.4.
# The analog nub is the mouse (cross: left button, circle: right). Its desktop
# is 640x362, shrunk to the PSP's 480x272 by the SDL layer: at 480x272 its
# toolbars and windows crowd the screen.
set -e
. "$ROOT/scripts/packages/recipes/_lib.sh"
D="$1"
case "${BOARD:-gameshell}" in
gameshell|pi)   # cross-compiled here against the board's SDL2; the SDL layer preloaded
	BIN="$ROOT/build/$BOARD/bin/simutrans"
	[ -x "$BIN" ] || "$ROOT/scripts/packages/cross-glibc.sh" "$BOARD" "$ROOT/scripts/packages/build-simutrans.sh" simutrans
	RUN="pspexp-run ./simutrans"; REQ="pspexp-run" ;;
*) echo "simutrans: no build for $BOARD (the Omega2+ has too little memory)" >&2; exit 3 ;;
esac
BASE=$(fetch "https://github.com/simutrans/simutrans/releases/download/124.5/simulinux-x64-124-5.zip")
PAK=$(fetch "https://downloads.sourceforge.net/project/simutrans/pak64/124-4/simupak64-124-4.zip")
check_sha256 "$BASE" 4e41a54806833ff4fb0471a06dae75b43c893e3a27d23912ca011e3abef1711d
check_sha256 "$PAK" edc6f9ca8d94af7bfcc9628ce1e7ddf468b07118cde0a50a8b5d0d30c22218ee
T="$D/.unpack"; rm -rf "$T"; mkdir -p "$T"
unzip -q "$BASE" -d "$T" -x 'simutrans/simutrans' 'simutrans/lib/*'
unzip -q "$PAK" -d "$T"
mv "$T/simutrans" "$D/data"; rm -rf "$T"
cp "$BIN" "$D/simutrans"
chmod +x "$D/simutrans"
cp "$ROOT/scripts/packages/art/simutrans.png" "$D/simutrans.png"   # the entry's cover
cat > "$D/package" <<P
version=124.5
title=Simutrans
description=Simutrans 124.5, the transport simulation, with the pak64 graphics
license=Artistic-1.0 (Simutrans and pak64; data/license.txt)
requires=$REQ
P
cat > "$D/simutrans.entry" <<E
title=Simutrans
type=game
description=Build a transport empire: trains, trucks, ships and planes
icon=train-front
exec=$RUN -set_basedir ./data -objects pak -screensize 640x362 -lang en
env=PSPEXP_KEYS=$SIMUTRANS_KEYS
env=PSPEXP_NUB=mouse
pspexp=1
E
