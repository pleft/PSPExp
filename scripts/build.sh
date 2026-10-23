#!/bin/sh
# Builds everything:
#   build/host/            host libraries, pspexp-bench, tests (CMake)
#   dist/PSPExp/           memory-stick folder: EBOOT.PBP + pspexp_usb.prx + pspexp_runtime.prx
# Toolchain: pspdev unpacked at ../pspdev (override with PSPDEV=...). The PSP
# part is skipped when psp-gcc is not found.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export PSPDEV="${PSPDEV:-$ROOT/../pspdev}"
export PATH="$PSPDEV/bin:$PATH"
JOBS="${JOBS:-8}"

cmake -S "$ROOT" -B "$ROOT/build/host" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/build/host" -j"$JOBS"
if [ "${SKIP_TESTS:-0}" != "1" ]; then
	(cd "$ROOT/build/host" && ctest --output-on-failure) || { echo "tests failed" >&2; exit 1; }
fi

if command -v psp-gcc >/dev/null 2>&1; then
	make -C "$ROOT/psp/transport-usb" -j"$JOBS"
	make -C "$ROOT/psp/runtime" -j"$JOBS"
	make -C "$ROOT/psp/boot" -j"$JOBS"
	make -C "$ROOT/psp/capture" -j"$JOBS"   # PSPExp Capture, a game.txt plugin: built, not yet in the zip
	DIST="$ROOT/dist/PSPExp"
	mkdir -p "$DIST"
	cp "$ROOT/psp/boot/EBOOT.PBP" "$ROOT/psp/transport-usb/pspexp_usb.prx" "$ROOT/psp/runtime/pspexp_runtime.prx" "$DIST/"
	echo "PSP: copy $DIST to ms0:/PSP/GAME/PSPExp/"
else
	echo "psp-gcc not found (PSPDEV=$PSPDEV): PSP build skipped" >&2
fi
echo "Done: $ROOT/build/host/pspexp-bench"
