#!/bin/sh
# SRB2Kart 1.6 (GPL-2.0: Kart Krew's kart racer on Sonic Robo Blast 2's engine)
# for a Debian 13 board, from the project's v1.6 tag: built in a Debian 13
# container of the board's architecture (as build-easyrpg.sh), SDL2 with its
# software renderer and OpenGL. Its assets (the project's AssetsLinuxOnly.zip)
# are downloaded at install by package srb2kart. Writes build/<arch>/srb2kart/
# (srb2kart, depends.txt) for scripts/packages/debs/pspexp-srb2kart/build.
#   scripts/packages/build-srb2kart.sh arm64|armhf
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64 | armhf}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; armhf) PLAT=linux/arm/v7 ;; *) echo "arm64 | armhf" >&2; exit 2 ;; esac
VER=v1.6
OUT="$ROOT/build/$ARCH/srb2kart"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev \
	libsdl2-dev libsdl2-mixer-dev libpng-dev zlib1g-dev libcurl4-gnutls-dev libgme-dev libgl-dev curl unzip >/dev/null
cd /tmp
git -c advice.detachedHead=false clone -q --depth 1 -b $VER https://github.com/STJr/Kart-Public.git src
# the project's assets beside the source for the build only: it records their MD5s (checked when
# the game starts); the package carries none of them (package srb2kart downloads the same zip)
curl -fsSL -o assets.zip https://github.com/STJr/Kart-Public/releases/download/v1.6/AssetsLinuxOnly.zip
mkdir -p src/assets/installer && (cd src/assets/installer && unzip -q /tmp/assets.zip)
cmake -S src -B b -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-fsigned-char -DSRB2_CONFIG_HAVE_DISCORDRPC=OFF -DSRB2_CONFIG_USEASM=OFF >/dev/null
cmake --build b -j\$(nproc) 2>&1 | grep -m5 -B2 'error:' || true
B=\$(find b -type f -name 'srb2kart*' -perm -u+x | head -1); [ -n \"\$B\" ]
strip \$B && cp \$B /out/srb2kart
mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O /out/srb2kart 2>/dev/null | sed 's/^shlibs:Depends=//' > /out/depends.txt
echo built; cat /out/depends.txt
"
ls -l "$OUT/srb2kart"
