#!/bin/sh
# title: Cave Story (NXEngine-evo, drawn by the PSP)
# Cave Story, the freeware 2004 platformer, on NXEngine-evo at 480x272 (the
# PSP's own screen, no scaling). GameShell and Omega2+: cross-compiled here
# (scripts/{gameshell,omega}/build.sh cavestory); Pi: compiled on the Pi
# (scripts/pi/install.sh cavestory); each run here when missing. The game data
# comes from the freeware Doukutsu.exe through NXEngine-evo's nxextract, made
# here (cavestory_data), the same on every board.
set -e
. "$ROOT/scripts/packages/recipes/_lib.sh"
D="$1"
case "${BOARD:-gameshell}" in
gameshell)   # cross-compiled here, the image's SDL2; the SDL layer preloaded
	BIN="$ROOT/build/gameshell/bin/nxengine-evo"
	[ -x "$BIN" ] || "$ROOT/scripts/gameshell/build.sh" cavestory
	RUN="pspexp-run ./nxengine-evo"; REQ="pspexp-run" ;;
pi)          # compiled on the Pi against its SDL2; preloaded
	BIN="$ROOT/build/pi/bin/nxengine-evo"
	[ -x "$BIN" ] || HOST="$HOST" "$ROOT/scripts/pi/install.sh" cavestory
	RUN="pspexp-run ./nxengine-evo"; REQ="pspexp-run" ;;
omega)       # static, the SDL layer built in
	BIN="$ROOT/build/omega/bin/nxengine-evo"
	[ -x "$BIN" ] || "$ROOT/scripts/omega/build.sh" cavestory
	RUN="env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy pspexp-exec ./nxengine-evo"; REQ="pspexp-exec" ;;
*) echo "cavestory: no build for $BOARD" >&2; exit 3 ;;
esac
DATA=$(cavestory_data)
mkdir -p "$D/data"
cp -R "$DATA/." "$D/data/"
cp "$BIN" "$D/nxengine-evo"
chmod +x "$D/nxengine-evo"
cp "$ROOT/../cavestory/nxengine-evo/LICENSE" "$D/COPYING.nxengine-evo"
cp "$ROOT/scripts/packages/art/cavestory.png" "$D/cavestory.png"   # the entry's cover
cat > "$D/package" <<P
version=$(git -C "$ROOT/../cavestory/nxengine-evo" rev-parse --short HEAD 2>/dev/null || echo unknown)
title=Cave Story
description=The freeware platformer Cave Story (2004), on NXEngine-evo
license=GPL-3.0 (NXEngine-evo); freeware data (Cave Story, Studio Pixel; English translation by Aeon Genesis)
requires=$REQ
P
cat > "$D/cavestory.entry" <<E
title=Cave Story
type=game
description=Studio Pixel's 2004 platformer: explore a floating island
icon=gamepad-2
exec=$RUN
env=PSPEXP_KEYS=$CAVESTORY_KEYS
env=PSPEXP_NUB=keys
pspexp=1
E
