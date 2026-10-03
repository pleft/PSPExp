#!/bin/sh
# Solarus 1.6 (the engine, GPL-3.0), for quests made for it (Mystery of Solarus
# DX 1.12), which Debian 13 does not carry: built in a Debian 13 container of
# the board's architecture (as build-easyrpg.sh). No quest data in it: each
# quest's package downloads its own. Writes build/<arch>/solarus/
# (solarus-run, libsolarus.so*, depends.txt), the library found through RPATH.
#   scripts/packages/build-solarus.sh arm64|armhf
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64 | armhf}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; armhf) PLAT=linux/arm/v7 ;; *) echo "arm64 | armhf" >&2; exit 2 ;; esac
VER=v1.6.5
OUT="$ROOT/build/$ARCH/solarus"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev \
	libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev libgl-dev libopenal-dev libvorbis-dev libmodplug-dev libphysfs-dev libluajit-5.1-dev libglm-dev >/dev/null
cd /tmp
git clone -q --depth 1 -b $VER https://gitlab.com/solarus-games/solarus.git src
cmake -S src -B b -DCMAKE_BUILD_TYPE=Release -DSOLARUS_GUI=OFF -DSOLARUS_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/usr \
	-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_INSTALL_RPATH='\$ORIGIN' -DCMAKE_CXX_FLAGS='-include cstdint -include limits' >/dev/null
cmake --build b -j\$(nproc) 2>&1 | grep -E ' error|Error ' | head -10 || true
[ -x b/solarus-run ]
strip b/solarus-run; cp b/solarus-run /out/; cp -P b/libsolarus.so* /out/; strip /out/libsolarus.so.*.* 2>/dev/null || strip /out/libsolarus.so.* 2>/dev/null || true
cp src/license.txt /out/COPYING 2>/dev/null || true
mkdir -p debian && printf 'Source: x\n' > debian/control
# its packages: what it loads, mapped to Debian packages (dpkg-shlibdeps refuses the private library)
for f in /out/solarus-run /out/libsolarus.so.*.*; do LD_LIBRARY_PATH=/out ldd \$f; done | awk '/=> \\// {print \$3}' | grep -v '^/out' | sort -u |
	while read l; do dpkg -S \"\$(readlink -f \$l)\" 2>/dev/null || dpkg -S \"\$l\" 2>/dev/null; done | cut -d: -f1 | sort -u | grep -v -- '-dev\$' | paste -sd, - | sed 's/,/, /g' > /out/depends.txt
echo built; cat /out/depends.txt
"
ls -l "$OUT"
