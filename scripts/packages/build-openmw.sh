#!/bin/sh
# OpenMW 0.51.0 (GPL-3.0), the open-source engine for Morrowind, for a Debian 13
# board: Debian 13 does not package it. Built in a Debian 13 container of the
# board's architecture (as build-flycast.sh), the game only (no launcher, editor
# or wizard: PSPExp's launcher starts it), with OpenMW's own MyGUI, Recast and
# double-precision Bullet (Debian 13's are too old or single-precision), on
# Debian's OpenSceneGraph, SDL2, FFmpeg and LuaJIT. Drawn by the board's GPU (the
# Pi's V3D) through the host-rendered layer. The game data is the player's own.
# Installed as its build installs it into build/<arch>/openmw/root, with
# depends.txt and LICENSE, for scripts/packages/debs/pspexp-openmw/build.
#   scripts/packages/build-openmw.sh arm64
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; *) echo "arm64 only: drawn by a Pi's GPU" >&2; exit 2 ;; esac
VER=openmw-0.51.0
OUT="$ROOT/build/$ARCH/openmw"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev \
	libopenscenegraph-dev libsdl2-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
	libboost-program-options-dev libboost-iostreams-dev libboost-system-dev libluajit-5.1-dev libyaml-cpp-dev \
	libsqlite3-dev libicu-dev libunshield-dev liblz4-dev zlib1g-dev libfreetype-dev libgl-dev libopenal-dev >/dev/null
cd /tmp
git -c advice.detachedHead=false clone -q --depth 1 -b $VER https://github.com/OpenMW/openmw.git src
cmake -S src -B b -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
	-DBUILD_LAUNCHER=OFF -DBUILD_WIZARD=OFF -DBUILD_OPENCS=OFF -DBUILD_BSATOOL=OFF -DBUILD_ESMTOOL=OFF \
	-DBUILD_NIFTEST=OFF -DBUILD_NAVMESHTOOL=OFF -DBUILD_BULLETOBJECTTOOL=OFF -DBUILD_ESSIMPORTER=OFF -DBUILD_MWINIIMPORTER=OFF \
	-DOPENMW_USE_SYSTEM_MYGUI=OFF -DOPENMW_USE_SYSTEM_RECASTNAVIGATION=OFF -DOPENMW_USE_SYSTEM_BULLET=OFF -DOPENMW_USE_SYSTEM_SQLITE3=ON >/dev/null
cmake --build b -j\$(nproc) 2>&1 | grep -m5 -B2 'error:' || true
DESTDIR=/out/root cmake --install b >/dev/null
strip /out/root/usr/bin/openmw
cp src/LICENSE /out/LICENSE
mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O /out/root/usr/bin/openmw 2>/dev/null | sed 's/^shlibs:Depends=//' > /out/depends.txt
echo built; cat /out/depends.txt
"
ls -l "$OUT/root/usr/bin"
