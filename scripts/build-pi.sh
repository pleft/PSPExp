#!/bin/sh
# Raspberry Pi (Raspberry Pi OS, 64-bit) host build: PSPExp host tools and
# Stratagus with the PSPExp backend, for running Warcraft on a PSP over USB
# with the Pi as the headless host (section 13).
#
# Untested on a Pi as of 2026-09-22: written from the macOS build; the
# dependencies are the Debian packages for the same libraries.
#
#   git clone <PSPExp>     ~/PSP/PSPExp
#   git clone <WarCraft>   ~/PSP/WarCraft   (with upstream/stratagus on branch pspexp, or apply
#                                            compatibility/stratagus/stratagus-pspexp.patch)
#   ~/PSP/PSPExp/scripts/build-pi.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WAR="${WARCRAFT_DIR:-$ROOT/../WarCraft}"
JOBS="${JOBS:-$(nproc)}"

if command -v apt-get >/dev/null 2>&1 && [ "${SKIP_APT:-0}" != "1" ]; then
	sudo apt-get install -y build-essential cmake pkg-config libusb-1.0-0-dev \
		libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev liblua5.1-0-dev libtolua++5.1-dev \
		libpng-dev zlib1g-dev
fi

# PSPExp host side (no PSP toolchain needed on the Pi; the PSP files come from dist/PSPExp)
cmake -S "$ROOT" -B "$ROOT/build/host" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build/host" -j"$JOBS"
(cd "$ROOT/build/host" && ctest --output-on-failure)

# Stratagus with the backend
[ -d "$WAR/upstream/stratagus" ] || { echo "Stratagus clone not found at $WAR/upstream/stratagus" >&2; exit 1; }
cmake -S "$WAR/upstream/stratagus" -B "$WAR/build/host-stratagus" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_VENDORED_LUA=OFF \
	-DWITH_MNG=OFF -DWITH_THEORA=OFF -DWITH_OGGVORBIS=OFF -DWITH_STACKTRACE=OFF -DWITH_OPENMP=OFF \
	-DWITH_BZIP2=OFF -DENABLE_USEGAMEDIR=ON -DBUILD_TESTING=OFF \
	-DWITH_PSPEXP=ON -DPSPEXP_DIR="$ROOT"
cmake --build "$WAR/build/host-stratagus" -j"$JOBS" --target stratagus

# libusb needs access to the PSP without root: one udev rule for Sony VID / PSPExp PID
if [ -d /etc/udev/rules.d ] && [ ! -f /etc/udev/rules.d/60-pspexp.rules ]; then
	echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="054c", ATTR{idProduct}=="01ca", MODE="0666"' | sudo tee /etc/udev/rules.d/60-pspexp.rules >/dev/null
	sudo udevadm control --reload-rules || true
fi

cat <<MSG

Done. With the PSP running PSPExp and plugged into the Pi:

  cd $WAR
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy PSPEXP=usb build/host-stratagus/stratagus -d dist/War1gus

(dist/War1gus needs the extracted game data, see the WarCraft README.)
MSG
