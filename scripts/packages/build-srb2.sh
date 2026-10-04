#!/bin/sh
# Sonic Robo Blast 2 2.2.15 (GPL-2.0: the SRB2 team's 3D Sonic fan game) for a
# Debian 13 board, from the project's release tag: built in a Debian 13
# container of the board's architecture (as build-srb2kart.sh), SDL2 with its
# software renderer and OpenGL. Its assets (srb2.pk3 and the rest, out of the
# project's SRB2-v2215-Full.zip) are downloaded at install by package srb2; their
# MD5s are written in the source, so the build needs none of them. Writes
# build/<arch>/srb2/ (srb2, depends.txt) for scripts/packages/debs/pspexp-srb2/build.
#   scripts/packages/build-srb2.sh arm64|armhf
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64 | armhf}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; armhf) PLAT=linux/arm/v7 ;; *) echo "arm64 | armhf" >&2; exit 2 ;; esac
VER=SRB2_release_2.2.15
OUT="$ROOT/build/$ARCH/srb2"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev \
	libsdl2-dev libsdl2-mixer-dev libpng-dev zlib1g-dev libcurl4-gnutls-dev libgme-dev libopenmpt-dev libminiupnpc-dev libgl-dev >/dev/null
cd /tmp
git -c advice.detachedHead=false clone -q --depth 1 -b $VER https://github.com/STJr/SRB2.git src
cmake -S src -B b -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-fsigned-char >/dev/null
cmake --build b -j\$(nproc) 2>&1 | grep -m5 -B2 'error:' || true
B=\$(find b -type f -name 'lsdlsrb2*' -perm -u+x | head -1); [ -n \"\$B\" ]
strip \$B && cp \$B /out/srb2
mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O /out/srb2 2>/dev/null | sed 's/^shlibs:Depends=//' > /out/depends.txt
echo built; cat /out/depends.txt
"
ls -l "$OUT/srb2"
