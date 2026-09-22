#!/bin/sh
# Runs the PSP runtime under PPSSPP so the real PSP executor can be exercised
# without hardware (section 11, "PSP executor under PPSSPP"; section 14).
#
#   scripts/run-ppsspp.sh            install dist/PSPExp into PPSSPP's memory stick and boot it
#   scripts/run-ppsspp.sh stop       stop PPSSPP
#
# PPSSPP must have networking enabled (ppsspp.ini: EnableWlan = True). Its
# PortOffset (default 10000) is added to the PSP's listening port, so the
# host connects to 127.0.0.1:13838:
#
#   build/host/pspexp-bench --tcp 127.0.0.1:13838
#   build/host/pspexp-sprite-demo --tcp 127.0.0.1:13838
#   PSPEXP=tcp:127.0.0.1:13838 ../WarCraft/build/host-stratagus/stratagus -d ../WarCraft/dist/War1gus
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PPSSPP="${PPSSPP:-/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL}"
MEMSTICK="${PPSSPP_MEMSTICK:-$HOME/.config/ppsspp/PSP}"
GAME="$MEMSTICK/GAME/PSPExp"
if [ "$1" = "stop" ]; then pkill -f "$(basename "$PPSSPP")" || true; exit 0; fi
[ -x "$PPSSPP" ] || { echo "PPSSPP not found at $PPSSPP" >&2; exit 1; }
[ -d "$ROOT/dist/PSPExp" ] || { echo "run scripts/build.sh first" >&2; exit 1; }
if ! grep -q "^EnableWlan = True" "$MEMSTICK/SYSTEM/ppsspp.ini" 2>/dev/null; then
	echo "warning: PPSSPP networking is off; set EnableWlan = True in $MEMSTICK/SYSTEM/ppsspp.ini" >&2
fi
mkdir -p "$GAME"
cp "$ROOT/dist/PSPExp/"* "$GAME/"
"$PPSSPP" "$GAME/EBOOT.PBP" > "$ROOT/build/ppsspp.log" 2>&1 &
echo "PPSSPP started (log: build/ppsspp.log); the runtime listens on 127.0.0.1:13838"
