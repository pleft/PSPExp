#!/bin/sh
# Hurrican's engine (the HurricanGame fork, MIT), which Debian does not carry,
# for a Debian 13 board: built in a Debian 13 container of the board's
# architecture (as build-easyrpg.sh), with the fixed-function OpenGL renderer
# (RENDERER=GL1), the one PSPExp's OpenGL draws most directly. Hurrican reaches
# OpenGL through libepoxy, whose own lookups would go to the board's GL past
# the SDL layer: it includes only <epoxy/gl.h> and calls no epoxy function, so
# a stand-in header with the plain GL prototypes takes its place and the calls
# are ordinary GL ones. The game's data is not ours to pass on: package
# hurrican downloads it from the game's repository. Writes
# build/<arch>/hurrican/ (hurrican, depends.txt).
#   scripts/packages/build-hurrican.sh arm64|armhf
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64 | armhf}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; armhf) PLAT=linux/arm/v7 ;; *) echo "arm64 | armhf" >&2; exit 2 ;; esac
VER=0d91d1f2d40f38d21e7f41afc01369ee55649fb6   # master, 2026: v1.0.9.3 still had SDL 1.2-only code
OUT="$ROOT/build/$ARCH/hurrican"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev \
	libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev libgl-dev libmodplug-dev >/dev/null
cd /tmp
git clone -q --recurse-submodules https://github.com/HurricanGame/Hurrican.git src && git -C src checkout -q $VER && git -C src submodule update -q --init --recursive
mkdir -p /opt/epoxy/include/epoxy
printf '%s\n' '/* PSPExp: libepoxy stood in for by the plain GL prototypes (build-hurrican.sh) */' '#define GL_GLEXT_PROTOTYPES 1' '#include <GL/gl.h>' '#include <GL/glext.h>' > /opt/epoxy/include/epoxy/gl.h
sed -i 's|find_package(LibEpoxy 1.2 REQUIRED)|set(LibEpoxy_INCLUDE_DIRS /opt/epoxy/include)\nset(LibEpoxy_LIBRARIES GL)|' src/Hurrican/CMakeLists.txt
cmake -S src/Hurrican -B b -DCMAKE_BUILD_TYPE=Release -DRENDERER=GL1 -DCMAKE_INSTALL_PREFIX=/usr >/dev/null
cmake --build b -j\$(nproc) 2>&1 | grep -m3 -B2 "error:" || true; [ -x b/hurrican ]
strip b/hurrican
cp b/hurrican /out/
cp src/README.md /out/README.fork.md
mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O b/hurrican 2>/dev/null | sed 's/^shlibs:Depends=//' > /out/depends.txt
echo built; cat /out/depends.txt
"
ls -l "$OUT/hurrican"
