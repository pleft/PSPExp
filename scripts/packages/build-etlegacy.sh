#!/bin/sh
# ET: Legacy 2.86.0's client (GPL-3.0 with id's additional terms: the
# continuation of Wolfenstein: Enemy Territory) for a Debian 13 board, from the
# project's tag: built in a Debian 13 container of the board's architecture (as
# build-srb2kart.sh) against Debian's SDL2 and the other system libraries
# (BUNDLED_LIBS off). The project's own arm64 build has SDL linked in and not
# exported, so the SDL layer could not take it; everything else of the game
# (its mod, the bots, id's ET 2.60 data) comes from the official releases at
# install (package etlegacy). The vanilla renderer (OpenGL 1) built in, which
# the PSP's GL layer draws, patched (compatibility/etlegacy/pspexp.patch). Writes build/<arch>/etlegacy/ (etl, depends.txt)
# for scripts/packages/debs/pspexp-etlegacy/build.
#   scripts/packages/build-etlegacy.sh arm64|armhf
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64 | armhf}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; armhf) PLAT=linux/arm/v7 ;; *) echo "arm64 | armhf" >&2; exit 2 ;; esac
VER=v2.86.0
OUT="$ROOT/build/$ARCH/etlegacy"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" -v "$ROOT/compatibility/etlegacy:/patch:ro" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev \
	libsdl2-dev libgl-dev libglew-dev libcurl4-openssl-dev libjpeg-dev libpng-dev libfreetype-dev libvorbis-dev libogg-dev \
	libtheora-dev libopenal-dev libsqlite3-dev liblua5.4-dev libminizip-dev zlib1g-dev libcjson-dev >/dev/null
cd /tmp
git -c advice.detachedHead=false clone -q --depth 1 -b $VER https://github.com/etlegacy/etlegacy.git src
# compatibility/etlegacy/pspexp.patch: r_vertexLight back (one pass a surface, as ioquake3's), and
# glUseProgramObjectARB(0) only when the driver has it (it crashed the first map load on the PSP's GL)
git -C src apply /patch/pspexp.patch
cmake -S src -B b -DCMAKE_BUILD_TYPE=Release -DBUNDLED_LIBS=OFF -DBUNDLED_LIBS_DEFAULT=OFF -DBUILD_SERVER=OFF -DBUILD_MOD=OFF \
	-DCROSS_COMPILE32=OFF -DFEATURE_AUTOUPDATE=OFF -DINSTALL_EXTRA=OFF -DRENDERER_DYNAMIC=OFF -DFEATURE_RENDERER1=ON 2>&1 | grep -i -E 'error|not found|missing' || true
cmake --build b -j\$(nproc) 2>&1 | grep -m5 -B2 'error:' || true
B=\$(find b -maxdepth 1 -type f -name 'etl*' -perm -u+x | head -1); [ -n \"\$B\" ]
strip \$B && cp \$B /out/etl
mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O /out/etl 2>/dev/null | sed 's/^shlibs:Depends=//' > /out/depends.txt
echo built; cat /out/depends.txt
"
ls -l "$OUT/etl"
