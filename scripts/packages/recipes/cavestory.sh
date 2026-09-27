#!/bin/sh
# title: Cave Story (NXEngine-evo, drawn by the PSP)
# Cave Story, the freeware 2004 platformer, on NXEngine-evo at 480x272 (the
# PSP's own screen, no scaling). Cross-compiled here for every board
# (scripts/packages/cross-glibc.sh for the GameShell and Pi, scripts/omega/build.sh
# cavestory), run here when missing (scripts/pi/install.sh cavestory still
# compiles it on the Pi, ~30 min against ~1 min here). The game data
# comes from the freeware Doukutsu.exe through NXEngine-evo's nxextract, made
# here (cavestory_data), the same on every board.
set -e
. "$ROOT/scripts/packages/recipes/_lib.sh"
D="$1"
case "${BOARD:-gameshell}" in
gameshell|pi)   # cross-compiled here against the board's SDL2; the SDL layer preloaded
	BIN="$ROOT/build/$BOARD/bin/nxengine-evo"
	[ -x "$BIN" ] || "$ROOT/scripts/packages/cross-glibc.sh" "$BOARD" "$ROOT/scripts/packages/build-nxengine.sh" nxengine-evo
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
