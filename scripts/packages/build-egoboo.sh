#!/bin/sh
# Egoboo (GPL-3.0), which Debian 13 no longer carries: its current SDL2 code
# (the egoboo repository's master with its idlib, idlib-game-engine and
# egoboo-external submodules), built in a Debian 13 container of the board's
# architecture (as build-easyrpg.sh). GLEW finds its functions through
# glXGetProcAddress, which the SDL layer answers. EGOBOO_DATA names the data folder
# (patched in: it looked only beside the program). Its data (egoboo-assets) is
# not built in: package egoboo downloads it. Writes build/<arch>/egoboo/
# (egoboo, depends.txt).
#   scripts/packages/build-egoboo.sh arm64|armhf
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64 | armhf}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; armhf) PLAT=linux/arm/v7 ;; *) echo "arm64 | armhf" >&2; exit 2 ;; esac
REV=${EGOBOO_REV:-master}
OUT="$ROOT/build/$ARCH/egoboo"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev \
	libsdl2-dev libsdl2-mixer-dev libsdl2-image-dev libsdl2-ttf-dev libgl-dev libglu1-mesa-dev libenet-dev libphysfs-dev >/dev/null
cd /tmp
git clone -q https://github.com/egoboo/egoboo.git src && cd src && git checkout -q $REV
git submodule update -q --init --depth 1 idlib idlib-game-engine external
git rev-parse --short HEAD > /out/revision
# the data folder from EGOBOO_DATA when set (else beside the program, its default): the
# program is pspexp-egoboo's, the data the installer's (egoboo)
sed -i 's|char\* applicationPath = SDL_GetBasePath();|char* applicationPath = getenv(\"EGOBOO_DATA\") ? SDL_strdup(getenv(\"EGOBOO_DATA\")) : SDL_GetBasePath();|' egolib/library/src/egolib/Platform/file_linux.c
grep -q EGOBOO_DATA egolib/library/src/egolib/Platform/file_linux.c
cd /tmp
cmake -S src -B b -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS='-include stdexcept -include cstdint -include cstring -include limits -include algorithm -include functional -include memory -include array -fpermissive -w' >/dev/null   # (older code, GCC 14: missing standard headers)
cmake --build b -j\$(nproc) --target egoboo 2>&1 | grep -E 'error|Error' | head -20 || true
f=\$(find b -type f -name 'egoboo*' -perm -u+x | grep -v '\.so' | head -1)
[ -n \"\$f\" ] || { echo 'no egoboo built'; exit 1; }
strip \"\$f\"; cp \"\$f\" /out/egoboo
mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O /out/egoboo 2>/dev/null | sed 's/^shlibs:Depends=//' > /out/depends.txt
echo built; cat /out/depends.txt
"
ls -l "$OUT/egoboo"
