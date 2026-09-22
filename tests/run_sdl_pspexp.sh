#!/bin/sh
# CTest: run the unmodified test game through the sdl-pspexp shim headlessly
# (SDL dummy drivers, Null executor) in both the renderer and the
# window-surface and render-target paths, and check that frames were mirrored.
#   run_sdl_pspexp.sh <build dir>
set -e
BUILD="$1"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy PSPEXP=null
for mode in "" "--surface" "--target"; do
	OUT="$("$ROOT/scripts/pspexp-run" "$BUILD/sdl-game" --frames 40 $mode 2>&1)" || { echo "$OUT"; exit 1; }
	echo "$OUT" | grep -q "exit: 40 frames mirrored" || { echo "$OUT"; echo "expected 40 mirrored frames ($mode)"; exit 1; }
done
echo "sdl-pspexp: ok"
