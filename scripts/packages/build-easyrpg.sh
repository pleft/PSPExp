#!/bin/sh
# EasyRPG Player (RPG Maker 2000 / 2003 games; GPL-3.0) for a Debian 13 board,
# which Debian does not carry: built in a Debian 13 container of the board's
# architecture (Docker or OrbStack on the Mac; arm64 runs natively there,
# armhf through emulation, slowly), against Debian's own libraries, liblcf
# built in. Writes build/<arch>/easyrpg/easyrpg-player and depends.txt (the
# Debian packages it needs, from dpkg-shlibdeps) for
# scripts/packages/debs/pspexp-easyrpg/build.
#   scripts/packages/build-easyrpg.sh arm64|armhf
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64 | armhf}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; armhf) PLAT=linux/arm/v7 ;; *) echo "arm64 | armhf" >&2; exit 2 ;; esac
PLAYER_VER=0.8.1.1 LCF_VER=0.8.1
OUT="$ROOT/build/$ARCH/easyrpg"
mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev \
	libsdl2-dev libpng-dev zlib1g-dev libfmt-dev libpixman-1-dev libfreetype-dev libharfbuzz-dev \
	libmpg123-dev libsndfile1-dev libvorbis-dev libopusfile-dev libspeexdsp-dev libxmp-dev libwildmidi-dev \
	liblhasa-dev libexpat1-dev libinih-dev nlohmann-json3-dev >/dev/null
cd /tmp
git clone -q --depth 1 -b $PLAYER_VER https://github.com/EasyRPG/Player.git player
git clone -q --depth 1 -b $LCF_VER https://github.com/EasyRPG/liblcf.git player/lib/liblcf
cmake -S player -B build -DCMAKE_BUILD_TYPE=Release -DPLAYER_BUILD_LIBLCF=ON -DLIBLCF_WITH_ICU=OFF \
	-DPLAYER_WITH_FLUIDSYNTH=OFF -DPLAYER_WITH_FLUIDLITE=OFF -DPLAYER_WITH_OPUS=ON -DPLAYER_WITH_XMP=ON >/dev/null
cmake --build build -j\$(nproc) >/dev/null
strip build/easyrpg-player
cp build/easyrpg-player /out/
mkdir -p /tmp/deb/DEBIAN && cp player/COPYING /out/COPYING 2>/dev/null || true
cd /tmp && mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O build/easyrpg-player 2>/dev/null | sed 's/^shlibs:Depends=//' > /out/depends.txt
echo built; cat /out/depends.txt
"
ls -l "$OUT/easyrpg-player"
