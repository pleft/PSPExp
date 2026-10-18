#!/bin/sh
# Commander Genius 3.6.4 (GPL-2.0+), the open-source engine for Commander Keen
# 1-6, for a Debian 13 board: Debian does not package it. Built in a Debian 13
# container of the board's architecture (as build-srb2.sh), on Debian's SDL2;
# its own downloader (DOWNLOADER) fetches the freely distributed episodes (Keen 1,
# Keen 4) from inside the game. Installed as its build installs it (the program
# and its data, /usr/share/games/commandergenius) into build/<arch>/commandergenius/root,
# with depends.txt and LICENSE, for scripts/packages/debs/pspexp-commandergenius/build.
#   scripts/packages/build-commandergenius.sh arm64|armhf
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64 | armhf}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; armhf) PLAT=linux/arm/v7 ;; *) echo "arm64 | armhf" >&2; exit 2 ;; esac
CG_VER=58a3c4fd0782fa2a087e38a039599b2ecf69ccc9   # master, 3.6.4 (2026-09-30)
OUT="$ROOT/build/$ARCH/commandergenius"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev \
	libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev libsdl2-ttf-dev libcurl4-openssl-dev zlib1g-dev libboost-dev >/dev/null
cd /tmp
git clone -q https://github.com/gerstrong/Commander-Genius.git src && git -C src checkout -q $CG_VER
cmake -S src -B b -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DUSE_VIRTUALPAD=OFF >/dev/null
cmake --build b -j\$(nproc) 2>&1 | grep -m5 -B2 'error:' || true
DESTDIR=/out/root cmake --install b >/dev/null
B=\$(find /out/root -type f -perm -u+x -name 'CGenius*' | head -1); [ -n \"\$B\" ]; strip \"\$B\"
cp src/COPYRIGHT /out/LICENSE
mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O \"\$B\" 2>/dev/null | sed 's/^shlibs:Depends=//' > /out/depends.txt
echo built \$B; cat /out/depends.txt
"
find "$OUT/root" -maxdepth 4 | head -20
