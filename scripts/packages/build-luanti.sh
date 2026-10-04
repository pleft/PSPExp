#!/bin/sh
# Luanti 5.10.0 (LGPL-2.1+; formerly Minetest) with its SDL2 device, for a
# Debian 13 board: Debian's own 5.10.0 is built for X11 (Irrlicht's own
# window: none on a headless board, and out of the SDL layer's reach). Built in
# a Debian 13 container of the board's architecture (as build-easyrpg.sh); the
# same version as Debian's luanti-data and luanti-game-minetest, which the
# package depends on for its data (share directory /usr/share/luanti). Writes
# build/<arch>/luanti/ (luanti, depends.txt) for
# scripts/packages/debs/pspexp-luanti/build.
#   scripts/packages/build-luanti.sh arm64|armhf
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64 | armhf}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; armhf) PLAT=linux/arm/v7 ;; *) echo "arm64 | armhf" >&2; exit 2 ;; esac
VER=5.10.0
OUT="$ROOT/build/$ARCH/luanti"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev \
	libsdl2-dev libgl-dev libegl-dev libpng-dev libjpeg-dev libfreetype-dev libluajit-5.1-dev libsqlite3-dev libcurl4-gnutls-dev \
	libzstd-dev libgmp-dev libjsoncpp-dev libopenal-dev libvorbis-dev libogg-dev zlib1g-dev gettext libleveldb-dev libspatialindex-dev >/dev/null
cd /tmp
git -c advice.detachedHead=false clone -q --depth 1 -b $VER https://github.com/luanti-org/luanti.git src
cmake -S src -B b -DCMAKE_BUILD_TYPE=Release -DRUN_IN_PLACE=FALSE -DCMAKE_INSTALL_PREFIX=/usr -DUSE_SDL2=ON -DBUILD_SERVER=FALSE -DBUILD_CLIENT=TRUE \
	-DENABLE_GETTEXT=ON -DENABLE_CURL=ON -DENABLE_SOUND=ON -DENABLE_LUAJIT=ON -DENABLE_SYSTEM_JSONCPP=ON -DENABLE_SYSTEM_GMP=ON >/dev/null
cmake --build b -j\$(nproc) 2>&1 | grep -m3 -B2 'error:' || true
B=src/bin/luanti; [ -x \$B ]   # (Luanti puts its program in the source tree's bin/)
strip \$B && cp \$B /out/luanti
mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O /out/luanti 2>/dev/null | sed 's/^shlibs:Depends=//' > /out/depends.txt
echo built; cat /out/depends.txt
"
ls -l "$OUT/luanti"
