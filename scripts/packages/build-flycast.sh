#!/bin/sh
# Flycast (GPL-2.0), the Sega Dreamcast emulator, for a Debian 13 board: Debian
# does not package it. Built in a Debian 13 container of the board's
# architecture (as build-mupen64plus.sh): the standalone program, not the
# libretro core (which asks for hardware rendering, SET_HW_RENDER, that
# pspexp-retro does not give). Its own SDL2 front end on Debian's SDL2, drawing
# with OpenGL ES 3 (the Pi 400's V3D has ES 3.1 but desktop GL 3.1 only): on the
# Pi the host-rendered SDL layer gives it the board's GPU (PSPEXP_HOSTGL). Its
# dynamic recompiler for 64-bit ARM (vixl) is built in. Off: Vulkan, Breakpad,
# Lua, libao, PulseAudio (sound goes through SDL2 to the layer), OpenMP.
# Its bundled libraries (libchdr, libzip, freetype, ImGui, libjuice, miniupnpc,
# vixl, picotcp...) are its git submodules, at the commit below.
# Writes build/<arch>/flycast/ (flycast, depends.txt, LICENSE) for
# scripts/packages/debs/pspexp-flycast/build.
#   scripts/packages/build-flycast.sh arm64
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; *) echo "arm64 only: the Pi 400 (the GameShell's GPU is OpenGL ES 2)" >&2; exit 2 ;; esac
FC_VER=6bb0fc3e18dac4b01694f488c7a71f5930e8a648   # master, 2026-10-17
OUT="$ROOT/build/$ARCH/flycast"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev \
	libsdl2-dev libgles-dev libegl-dev libasound2-dev libudev-dev libcurl4-openssl-dev zlib1g-dev >/dev/null
cd /tmp
git clone -q https://github.com/flyinghead/flycast.git flycast && git -C flycast checkout -q $FC_VER
git -C flycast submodule update -q --init --recursive --depth 1
cmake -S flycast -B b -DCMAKE_BUILD_TYPE=Release -DUSE_HOST_SDL=ON -DUSE_GLES=ON -DUSE_VULKAN=OFF \
	-DUSE_BREAKPAD=OFF -DUSE_LUA=OFF -DUSE_LIBAO=OFF -DUSE_PULSEAUDIO=OFF -DUSE_HOST_LIBZIP=OFF -DUSE_OPENMP=OFF >/dev/null
cmake --build b -j\$(nproc) 2>&1 | grep -m3 -B2 'error:' || true
strip -o /out/flycast b/flycast
cp flycast/LICENSE /out/LICENSE
mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O --ignore-missing-info /out/flycast > /tmp/dep 2> /tmp/dep.err || { cat /tmp/dep.err; exit 1; }
sed -n 's/^shlibs:Depends=//p' /tmp/dep > /out/depends.txt
echo built; cat /out/depends.txt
"
ls -l "$OUT"
