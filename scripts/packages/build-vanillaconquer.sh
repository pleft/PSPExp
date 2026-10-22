#!/bin/sh
# Vanilla Conquer (GPL-3.0: the Command & Conquer and Red Alert source EA
# published in 2020, made portable by the Assembly Armada) for a Debian 13
# board: VanillaTD and VanillaRA with their SDL2 video and OpenAL sound, built
# in a Debian 13 container of the board's architecture (as build-easyrpg.sh).
# 8-bit frames drawn on the CPU: the SDL layer sends them as CLUT8 pages, as
# Doom's. The games' data is not in it: EA's freeware, downloaded at install.
# Writes build/<arch>/vanillaconquer/ (vanillatd, vanillara, depends.txt).
#   scripts/packages/build-vanillaconquer.sh arm64|armhf
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64 | armhf}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; armhf) PLAT=linux/arm/v7 ;; *) echo "arm64 | armhf" >&2; exit 2 ;; esac
VER=ce83b59edd99cccac6cebaae054ca04962c744b7   # main, 2026-07: the project tags no releases
OUT="$ROOT/build/$ARCH/vanillaconquer"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev libsdl2-dev libopenal-dev >/dev/null
cd /tmp
git -c advice.detachedHead=false clone -q https://github.com/TheAssemblyArmada/Vanilla-Conquer.git src && git -C src checkout -q $VER
cmake -S src -B b -DCMAKE_BUILD_TYPE=Release -DBUILD_VANILLATD=ON -DBUILD_VANILLARA=ON -DSDL2=ON -DOPENAL=ON -DBUILD_TOOLS=OFF >/dev/null
cmake --build b -j\$(nproc) 2>&1 | grep -m3 -B2 'error:' || true
for g in vanillatd vanillara; do [ -x b/\$g ]; strip b/\$g; cp b/\$g /out/; done
mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O /out/vanillatd /out/vanillara 2>/dev/null | sed 's/^shlibs:Depends=//' > /out/depends.txt
echo built; cat /out/depends.txt
"
ls -l "$OUT"
