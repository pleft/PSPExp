#!/bin/sh
# Warcraft: Orcs & Humans on Stratagus + War1gus (both GPL-2.0), built in a
# Debian 13 container of the board's architecture (as build-srb2.sh) from their
# master branches at fixed commits (2026-06-22, still their newest on
# 2026-10-08; the last release tags, 3.3.2, are from 2022). Stratagus is
# unmodified; war1tool gets one fix (compatibility/war1gus/
# war1tool-optional-cutscenes.patch: it crashed with only DATA.WAR). The game
# runs through the SDL layer like any other SDL2 program; its data is the
# player's own DATA.WAR, extracted on the board by war1tool. Writes
# build/<arch>/war1gus/ (stratagus, war1tool, War1gus's scripts and maps in
# share/, depends.txt) for scripts/packages/debs/pspexp-war1gus/build.
#   scripts/packages/build-war1gus.sh arm64|armhf
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64 | armhf}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; armhf) PLAT=linux/arm/v7 ;; *) echo "arm64 | armhf" >&2; exit 2 ;; esac
STRATAGUS=3d87c93f7fd8c0b62ee1be5df0a6d9efc72ca6cc
WAR1GUS=8ec75764c7b4706a4405004438e810c050228b94
OUT="$ROOT/build/$ARCH/war1gus"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" -v "$ROOT/compatibility/war1gus:/patches:ro" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev \
	libsdl2-dev libsdl2-mixer-dev libsdl2-image-dev liblua5.1-0-dev libtolua++5.1-dev libpng-dev zlib1g-dev libbz2-dev >/dev/null
cd /tmp
for r in stratagus:$STRATAGUS war1gus:$WAR1GUS; do
	n=\${r%%:*}; c=\${r#*:}
	git init -q \$n && git -C \$n fetch -q --depth 1 https://github.com/Wargus/\$n.git \$c && git -c advice.detachedHead=false -C \$n checkout -q FETCH_HEAD
done
git -C war1gus apply /patches/war1tool-optional-cutscenes.patch
git -C stratagus submodule update -q --init --depth 1 2>/dev/null || true
cmake -S stratagus -B bs -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-fsigned-char -DCMAKE_C_FLAGS=-fsigned-char -DENABLE_DEV=ON -DCMAKE_INSTALL_PREFIX=/usr > cfg.log 2>&1 || { tail -20 cfg.log; exit 1; }
cmake --build bs -j\$(nproc) 2>&1 | grep -m5 -B2 'error:' || true
cmake --install bs --prefix /tmp/sdev >/dev/null
cmake -S war1gus -B bw -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-fsigned-char -DCMAKE_C_FLAGS=-fsigned-char -DCMAKE_INSTALL_PREFIX=/usr -DSTRATAGUS=/usr/games/stratagus \
	-DSTRATAGUS_INCLUDE_DIR=/tmp/sdev/include > cfgw.log 2>&1 || { tail -20 cfgw.log; exit 1; }
cmake --build bw -j\$(nproc) 2>&1 | grep -m5 -B2 'error:' || true
B=\$(find bs -maxdepth 1 -type f -name stratagus -perm -u+x | head -1); [ -n \"\$B\" ]
T=\$(find bw -maxdepth 1 -type f -name war1tool -perm -u+x | head -1); [ -n \"\$T\" ]
strip \$B \$T && cp \$B /out/stratagus && cp \$T /out/war1tool
mkdir -p /out/share && cp -r war1gus/campaigns war1gus/contrib war1gus/maps war1gus/scripts war1gus/shaders /out/share/ 2>/dev/null || true
cp war1gus/COPYING /out/COPYING
mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O /out/stratagus /out/war1tool 2>/dev/null | sed 's/^shlibs:Depends=//' > /out/depends.txt
echo built; cat /out/depends.txt
"
ls -la "$OUT"
