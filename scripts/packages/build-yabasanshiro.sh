#!/bin/sh
# Yaba Sanshiro (GPL-2.0), the Sega Saturn emulator made for ARM boards, for a Debian 13 board:
# Debian does not package it. Built in a Debian 13 container of the board's architecture (as
# build-flycast.sh): its standalone SDL2 program (retro_arena), drawing with OpenGL ES 3.1 (on the
# Pi the host-rendered SDL layer gives it the board's GPU, PSPEXP_HOSTGL), with its SH-2
# recompiler for 64-bit ARM (dynarec_devmiyax). Its upstream (devmiyax/yabause) is gone from
# GitHub; libretro keeps the whole tree on the yabasanshiro branch of libretro/yabause, pinned below.
# Patched (scripts/packages/yabasanshiro/pspexp.patch):
#   - the window the size YABASANSHIRO_WINDOW says (480x272: less to read back each frame than the
#     display's whole size);
#   - an OpenGL ES context asked for (Mesa gave the Pi 400's V3D a desktop GL 3.1 one);
#   - its shaders round a colour to an integer (int(c*255.0+0.5)), not truncate it: on the V3D a
#     stored 128 came back as 127.99..., and sprites with palette colours (Saturn Bomberman's,
#     Panzer Dragoon's textures) were drawn as one colour; Mesa's software renderer drew them right;
#   - its menu's button the controller's "select" in keymapv2.json (it was read only from an
#     EmulationStation input file, so only the Escape key opened the menu).
# The toolchain file is its rp64.cmake for a 64-bit board, with the processor named aarch64 (its
# "arm" picked the 32-bit recompiler) and GCC 14's errors on old C (libchdr's FLAC) relaxed.
# Writes build/<arch>/yabasanshiro/ (yabasanshiro, depends.txt, LICENSE) for
# scripts/packages/debs/pspexp-yabasanshiro/build.
#   scripts/packages/build-yabasanshiro.sh arm64
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; *) echo "arm64 only: a Pi 4, 400 or 5 (the GameShell's GPU is OpenGL ES 2)" >&2; exit 2 ;; esac
YS_VER=09ed8e5b2e97e7a848ea2514545c34c7b809e399   # libretro/yabause, branch yabasanshiro, 2026-08-23 (Yaba Sanshiro 3.5.0)
OUT="$ROOT/build/$ARCH/yabasanshiro"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" -v "$ROOT/scripts/packages/yabasanshiro:/patches:ro" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev \
	libsdl2-dev libgles-dev libegl-dev zlib1g-dev \
	libboost-system-dev libboost-filesystem-dev libboost-date-time-dev libboost-locale-dev >/dev/null
cd /tmp
git clone -q --branch yabasanshiro https://github.com/libretro/yabause.git yaba && git -C yaba checkout -q $YS_VER
git -C yaba submodule update -q --init --recursive
(cd yaba && patch -p1 < /patches/pspexp.patch) >/dev/null
cat > /tmp/rp64-pspexp.cmake <<'T'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(USE_EGL True)
set(CMAKE_C_FLAGS \"\${CMAKE_C_FLAGS} -D_GNU_SOURCE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=int-conversion\")
add_definitions(-D__RP64__)
add_definitions(-D__RETORO_ARENA__)
T
cmake -S yaba/yabause -B b -DCMAKE_BUILD_TYPE=Release -DGIT_EXECUTABLE=/usr/bin/git -DYAB_PORTS=retro_arena \
	-DYAB_WANT_DYNAREC_DEVMIYAX=ON -DCMAKE_TOOLCHAIN_FILE=/tmp/rp64-pspexp.cmake > /tmp/cmake.log 2>&1 || { tail -30 /tmp/cmake.log; exit 1; }
cmake --build b -j\$(nproc) > /tmp/build.log 2>&1 || { grep -B3 -m5 'error:' /tmp/build.log; exit 1; }
strip -o /out/yabasanshiro b/src/retro_arena/yabasanshiro
cp yaba/yabause/COPYING.txt /out/LICENSE
mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O --ignore-missing-info /out/yabasanshiro > /tmp/dep 2> /tmp/dep.err || { cat /tmp/dep.err; exit 1; }
sed -n 's/^shlibs:Depends=//p' /tmp/dep > /out/depends.txt
echo built; cat /out/depends.txt
"
ls -l "$OUT"
