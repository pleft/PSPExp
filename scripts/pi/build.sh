#!/bin/sh
# Raspberry Pi (Raspberry Pi OS, 64-bit; any Debian works) host build: the
# PSPExp host tools, the sdl-pspexp injection library and Chocolate Doom +
# Freedoom from apt, so the Pi is the headless host and the PSP the screen
# (section 13).
#
# Tested 2026-09-23 on a Raspberry Pi 400 (Debian 13 arm64): Doom on the
# PSP from the Pi at 35 fps, USB 25 MB/s, same numbers as from the Mac.
#
#   git clone <PSPExp>     ~/PSP/PSPExp
#   ~/PSP/PSPExp/scripts/pi/build.sh
#   scripts/dev/pspexp-doom                      (Debian's freedoom1.wad is found automatically)
#   scripts/dev/pspexp-run build/host/sdl-game   (the SDL2 test game)
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
JOBS="${JOBS:-$(nproc)}"

if command -v apt-get >/dev/null 2>&1 && [ "${SKIP_APT:-0}" != "1" ]; then
	sudo apt-get install -y build-essential cmake pkg-config libusb-1.0-0-dev \
		libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev libpng-dev zlib1g-dev \
		chocolate-doom freedoom
fi

# PSPExp host side (no PSP toolchain needed on the Pi; the PSP files come from dist/PSPExp)
cmake -S "$ROOT" -B "$ROOT/build/host" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build/host" -j"$JOBS"
(cd "$ROOT/build/host" && ctest --output-on-failure)
[ -f "$ROOT/build/host/libsdl-pspexp.so" ] || { echo "libsdl-pspexp.so was not built: is libsdl2-dev installed?" >&2; exit 1; }

# libusb needs access to the PSP without root: one udev rule for Sony VID / PSPExp PID
if [ -d /etc/udev/rules.d ] && [ ! -f /etc/udev/rules.d/60-pspexp.rules ]; then
	echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="054c", ATTR{idProduct}=="01ca", MODE="0666"' | sudo tee /etc/udev/rules.d/60-pspexp.rules >/dev/null
	sudo udevadm control --reload-rules || true
fi

cat <<MSG

Done. With the PSP running PSPExp and plugged into the Pi:

  cd $ROOT
  build/host/pspexp-bench --frames-only --frames 120     # link check
  scripts/dev/pspexp-doom                                    # Doom on the PSP
  scripts/dev/pspexp-run build/host/sdl-game                 # the SDL2 test game

Over SSH with no desktop session the launcher selects SDL's dummy video and
audio drivers itself; the PSP is the screen and the speaker.
MSG
