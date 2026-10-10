#!/bin/sh
# CTest: run the unmodified test game through the sdl-pspexp shim headlessly
# (SDL dummy drivers, Null executor) in both the renderer and the
# window-surface and render-target paths, and the OpenGL demo, and check
# that frames were mirrored.
#   run_sdl_pspexp.sh <build dir>
set -e
BUILD="$1"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy PSPEXP=null PSPEXP_BUILD="$BUILD"   # its SDL layer, not build/host's
for mode in "" "--surface" "--target"; do
	OUT="$("$ROOT/scripts/dev/pspexp-run" "$BUILD/sdl-game" --frames 40 $mode 2>&1)" || { echo "$OUT"; exit 1; }
	echo "$OUT" | grep -q "exit: 40 frames mirrored" || { echo "$OUT"; echo "expected 40 mirrored frames ($mode)"; exit 1; }
done
# OpenGL: the demo's frames reach the null executor as 3D draws, no errors
"$ROOT/compatibility/sdl-pspexp/gen-gl-procs.py" --check
if [ -x "$BUILD/gl-demo" ]; then
	OUT="$(PSPEXP_GL_LOG=1 "$ROOT/scripts/dev/pspexp-run" "$BUILD/gl-demo" 30 2>&1)" || { echo "$OUT"; exit 1; }
	echo "$OUT" | grep -q "exit: 30 frames mirrored" || { echo "$OUT"; echo "gl-demo: expected 30 mirrored frames"; exit 1; }
	echo "$OUT" | grep -qE "gl: frame: [1-9][0-9]* draws" || { echo "$OUT"; echo "gl-demo: no 3D draws"; exit 1; }
	if echo "$OUT" | grep -qE "runtime reported error|GL error"; then echo "$OUT"; echo "gl-demo: errors"; exit 1; fi
	# GL 2: a GLSL program, read and drawn by the GE
	OUT="$(GLDEMO_SHADER=1 PSPEXP_GL_LOG=1 "$ROOT/scripts/dev/pspexp-run" "$BUILD/gl-demo" 10 2>&1)" || { echo "$OUT"; exit 1; }
	echo "$OUT" | grep -qE "gl: frame: 1 draws" || { echo "$OUT"; echo "gl-demo (shader): no draw"; exit 1; }
	if echo "$OUT" | grep -qE "runtime reported error|GL error|drawn as near"; then echo "$OUT"; echo "gl-demo (shader): errors"; exit 1; fi
fi
echo "sdl-pspexp: ok"
