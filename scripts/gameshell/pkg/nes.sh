#!/bin/sh
# title: NES (pspexp-retro + FCEUmm, drawn by the PSP)
# The Nintendo Entertainment System for the launcher: the pspexp-retro player
# with the FCEUmm core (GPL-2.0) and nes.system, which makes the launcher show
# an NES tile and list the ROMs in ~/roms/nes. No ROMs are included.
#   gameshell, pi  player and core compiled on the board (build-gameshell.sh
#                  retro / install-pi.sh retro); the SDL layer preloaded
#   omega          one static program, core and SDL layer linked in
#                  (build-omega.sh retro)
set -e
. "$ROOT/scripts/gameshell/pkg/_lib.sh"
D="$1"
CORE=fceumm
SRC="$ROOT/../retro/libretro-$CORE"
V=$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo unknown)
case "$BOARD" in
gameshell|pi)
	B="$ROOT/build/$BOARD/bin"
	if [ ! -x "$B/pspexp-retro" ] || [ ! -f "$B/${CORE}_libretro.so" ]; then
		if [ "$BOARD" = pi ]; then PI="$HOST" "$ROOT/scripts/install-pi.sh" retro
		else HOST="$HOST" "$ROOT/scripts/build-gameshell.sh" retro; fi
	fi
	cp "$B/pspexp-retro" "$B/${CORE}_libretro.so" "$D/"
	EXEC="pspexp-run ./pspexp-retro ./${CORE}_libretro.so %ROM%"
	REQ="pspexp-run" ;;
omega)
	BIN="$ROOT/build/omega/bin/pspexp-retro-$CORE"
	[ -x "$BIN" ] || CORE=$CORE "$ROOT/scripts/build-omega.sh" retro
	cp "$BIN" "$D/pspexp-retro-$CORE"
	EXEC="pspexp-exec ./pspexp-retro-$CORE $CORE %ROM%"
	REQ="pspexp-exec" ;;
*) echo "nes: no build for $BOARD" >&2; exit 3 ;;
esac
cp "$SRC/Copying" "$D/COPYING.$CORE" 2>/dev/null || cp "$SRC/COPYING" "$D/COPYING.$CORE" 2>/dev/null || true
chmod +x "$D"/pspexp-retro*

mkdir -p "$D/systems"
cat > "$D/systems/nes.system" <<S
name=Nintendo Entertainment System
short=NES
icon=gamepad-2
roms=~/roms/nes
ext=nes,fds,unf,unif
exec=$EXEC
pkg=nes
env=SDL_VIDEODRIVER=dummy
env=SDL_AUDIODRIVER=dummy
thumbs=Nintendo - Nintendo Entertainment System
pspexp=1
S
cat > "$D/package" <<P
version=$V
title=NES
description=Nintendo Entertainment System: pspexp-retro with the FCEUmm core; ROMs in ~/roms/nes
license=GPL-2.0 (FCEUmm); pspexp-retro is part of PSPExp
requires=$REQ
P
