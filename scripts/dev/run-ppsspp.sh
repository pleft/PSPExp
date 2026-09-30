#!/bin/sh
# Runs the PSP runtime under PPSSPP so the real PSP executor can be exercised
# without hardware (section 11, "PSP executor under PPSSPP"; section 14).
#
#   scripts/dev/run-ppsspp.sh            install dist/PSPExp into PPSSPP's memory stick (slot a) and boot it
#   scripts/dev/run-ppsspp.sh stop       stop PPSSPP
#
# PPSSPP must have networking enabled (ppsspp.ini: EnableWlan = True). The
# runtime's port 3838 is exposed on the host as is (PortOffset does not
# apply to listening sockets):
#
#   build/host/pspexp-bench --tcp 127.0.0.1:3838
#   build/host/pspexp-sprite-demo --tcp 127.0.0.1:3838 --shot 60
#   scripts/dev/pspexp-run --tcp:127.0.0.1:3838 build/host/sdl-game
#
# Notes: pspexp-shot-N.bmp framebuffer dumps (written next to the EBOOT in
# the memory stick folder) are only faithful for the FIRST client after the
# emulator boots; after a disconnect PPSSPP stops copying the GPU frame
# back to VRAM and the dumps show the stale status page. The JIT with
# FastMemoryAccess=True occasionally crashes PPSSPP right after a client
# connects (EmuThread SIGSEGV at the emulated address 0); restart it.
# Emulator timings (fps, latency) are not representative of the hardware.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PPSSPP="${PPSSPP:-/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL}"
MEMSTICK="${PPSSPP_MEMSTICK:-$HOME/.config/ppsspp/PSP}"
GAME="$MEMSTICK/GAME/PSPExp"
if [ "$1" = "stop" ]; then pkill -f "$(basename "$PPSSPP")" || true; exit 0; fi
[ -x "$PPSSPP" ] || { echo "PPSSPP not found at $PPSSPP" >&2; exit 1; }
[ -d "$ROOT/dist/PSPExp" ] || { echo "run scripts/build.sh first" >&2; exit 1; }
if ! grep -q "^EnableWlan = True" "$MEMSTICK/SYSTEM/ppsspp.ini" 2>/dev/null; then
	echo "warning: PPSSPP networking is off; set EnableWlan = True in $MEMSTICK/SYSTEM/ppsspp.ini" >&2
fi
# a fresh slotted install of this build (the loader runs the active slot, so
# copying the modules next to it would leave an older slot running)
rm -rf "$GAME/slot-a" "$GAME/slot-b" "$GAME/active" "$GAME/active.new" "$GAME/fallback" \
	"$GAME/pspexp_runtime.prx" "$GAME/pspexp_usb.prx" "$GAME/pspexp-rt.log"
"$ROOT/scripts/packages/runtime-slot" --install "$GAME" >/dev/null
"$PPSSPP" "$GAME/EBOOT.PBP" > "$ROOT/build/ppsspp.log" 2>&1 &
echo "PPSSPP started (log: build/ppsspp.log); the runtime listens on 127.0.0.1:3838"
