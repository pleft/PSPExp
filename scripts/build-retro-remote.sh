#!/bin/sh
# pspexp-retro and one libretro core, compiled on a board over SSH with its
# own gcc and libSDL2 (the GameShell, a Raspberry Pi: glibc boards where the
# SDL layer is preloaded), copied back for the packages to ship.
#   scripts/build-retro-remote.sh HOST OUT [SDL_INCLUDE]
#     HOST         user@host of the board
#     OUT          local directory for pspexp-retro and <core>_libretro.so
#     SDL_INCLUDE  SDL2 headers to send when the board has none (the
#                  GameShell: the shim build's copy); the board's own
#                  libSDL2-2.0.so.0 is linked either way
# CORE=fceumm (default) picks ../retro/libretro-<core>
# (git clone https://github.com/libretro/libretro-fceumm).
# Called by scripts/build-gameshell.sh retro and scripts/install-pi.sh retro.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
H="$1"; OUT="$2"; INC="$3"
[ -n "$H" ] && [ -n "$OUT" ] || { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
CORE="${CORE:-fceumm}"
SRC="$ROOT/../retro/libretro-$CORE"
[ -f "$SRC/Makefile.libretro" ] || [ -f "$SRC/Makefile" ] || { echo "core source not found in $SRC" >&2; exit 1; }

ssh "$H" 'rm -rf ~/pspexp/retro-src && mkdir -p ~/pspexp/retro-src/core ~/pspexp/retro-src/player ~/pspexp/retro-src/sdl2'
{ COPYFILE_DISABLE=1 tar --no-xattrs -C "$SRC" --exclude .git --exclude '*.o' --exclude '*.so' --exclude '*.dylib' -cf - . ; } 2>/dev/null |
	ssh "$H" 'tar -C ~/pspexp/retro-src/core -xf -'
{ COPYFILE_DISABLE=1 tar --no-xattrs -C "$ROOT/tools/pspexp-retro" -cf - main.c libretro.h ; } 2>/dev/null |
	ssh "$H" 'tar -C ~/pspexp/retro-src/player -xf -'
[ -n "$INC" ] && { COPYFILE_DISABLE=1 tar --no-xattrs -C "$INC" -cf - . ; } 2>/dev/null |
	ssh "$H" 'mkdir -p ~/pspexp/retro-src/sdl2/SDL2 && tar -C ~/pspexp/retro-src/sdl2/SDL2 -xf -'

ssh "$H" "CORE=$CORE sh -s" <<'REMOTE'
set -e
cd ~/pspexp/retro-src
MK=Makefile.libretro; [ -f core/$MK ] || MK=Makefile
make -C core -f $MK -j4 platform=unix > core.log 2>&1 || { tail -20 core.log; exit 1; }
if [ -f sdl2/SDL2/SDL.h ]; then
	# headers sent along; link the system library by its runtime name
	SDLLIB=$(ls /lib/*/libSDL2-2.0.so.0 /usr/lib/*/libSDL2-2.0.so.0 2>/dev/null | head -1)
	[ -n "$SDLLIB" ] || { echo "no libSDL2-2.0.so.0 on this board" >&2; exit 1; }
	ln -sf "$SDLLIB" sdl2/libSDL2.so
	CFLAGS="-Isdl2/SDL2 -D_REENTRANT"; LIBS="-Lsdl2 -lSDL2"
else
	CFLAGS=$(sdl2-config --cflags); LIBS=$(sdl2-config --libs)
fi
gcc -O2 -Wall $CFLAGS -o pspexp-retro player/main.c $LIBS -ldl
ls -la pspexp-retro core/${CORE}_libretro.so
REMOTE

mkdir -p "$OUT"
scp -q "$H:pspexp/retro-src/pspexp-retro" "$OUT/pspexp-retro"
scp -q "$H:pspexp/retro-src/core/${CORE}_libretro.so" "$OUT/${CORE}_libretro.so"
echo "$OUT: pspexp-retro, ${CORE}_libretro.so"
