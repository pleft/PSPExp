#!/bin/sh
# AssaultCube 1.3.0.2 (zlib-style licence; its content under the licences of
# its README) for a Debian 13 board, from the project's own release tag: built
# in a Debian 13 container of the board's architecture (as build-easyrpg.sh).
# Debian's arm64 build fails against its own local server ("protocol
# incompatible"); the project's tree, built as it is, does not. The game's
# fixed-function OpenGL is drawn by the PSP's GE through the SDL layer.
# Writes build/<arch>/assaultcube/ (native_client, depends.txt) for
# scripts/packages/debs/pspexp-assaultcube/build.
#   scripts/packages/build-assaultcube.sh arm64|armhf
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64 | armhf}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; armhf) PLAT=linux/arm/v7 ;; *) echo "arm64 | armhf" >&2; exit 2 ;; esac
VER=v1.3.0.2
OUT="$ROOT/build/$ARCH/assaultcube"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential git ca-certificates pkg-config dpkg-dev autoconf automake libtool \
	libsdl2-dev libsdl2-image-dev zlib1g-dev libgl-dev libopenal-dev libvorbis-dev libx11-dev >/dev/null
cd /tmp
git -c advice.detachedHead=false clone -q --depth 1 -b $VER https://github.com/assaultcube/AC.git ac
make -C ac/source/src libenet >/dev/null 2>&1; make -C ac/source/src client CXX=g++ -j\$(nproc) 2>&1 | grep -m5 -B2 'error:' || true   # (its Makefile races configuring enet under -j: enet first)
[ -x ac/source/src/ac_client ]
strip ac/source/src/ac_client && cp ac/source/src/ac_client /out/native_client
mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O /out/native_client 2>/dev/null | sed 's/^shlibs:Depends=//' > /out/depends.txt
echo built; cat /out/depends.txt
"
ls -l "$OUT/native_client"
