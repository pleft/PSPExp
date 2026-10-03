#!/bin/sh
# Mupen64Plus (GPL-2.0; its main branch, 2026: 2.6.0 of 2024 refuses ROMs with
# the newer open-source boot code, today's homebrew), the Nintendo 64 emulator, for a Debian 13
# board: Debian lists it but builds none of it for arm64. Built in a Debian 13
# container of the board's architecture (as build-easyrpg.sh): the core (its
# dynamic recompiler for 64-bit ARM), the console front end, and the plugins:
# RSP HLE (with cxd4's low-level RSP behind it for microcode HLE does not know:
# homebrew made with libdragon), SDL audio, SDL input, and GLideN64 (GPL-2.0) for the graphics.
# GLideN64 asks the core (whose window and context are SDL's) for OpenGL ES
# 3.0 here: on the Pi the host-rendered SDL layer gives it the board's GPU
# (PSPEXP_HOSTGL). Everything goes in one
# folder, the front end finding the core and plugins beside it. Writes
# build/<arch>/mupen64plus/ (mupen64plus, libmupen64plus.so.2, plugins/,
# data/, depends.txt) for scripts/packages/debs/pspexp-mupen64plus/build.
#   scripts/packages/build-mupen64plus.sh arm64|armhf
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64 | armhf}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; armhf) PLAT=linux/arm/v7 ;; *) echo "arm64 | armhf" >&2; exit 2 ;; esac
# each part at a commit of its main branch
C_core=ba95bab92a76744753bfe61470823a4937850ab0 C_ui=c8ac4862a019d7885b24927d9b4db5dd3e42a528
C_rsp=8a7a472a7172eb2c8725b305eae26818ed7b51a2 C_audio=2faed1c7e62c5f292948e7cd2398c184970cf794
C_input=842c39e89749aa3a8d02202b2afddd20b29cdfdb C_cxd4=00906a92641c540a64e4e7505012157315314810
GLN_VER=41c7ba273a6c9afb43c0574cf3cf5d139182d070   # master, 2026-08: GLideN64 has made no release since 4.0 (2019)
OUT="$ROOT/build/$ARCH/mupen64plus"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev nasm \
	libsdl2-dev libgl-dev libegl-dev libpng-dev zlib1g-dev libfreetype-dev libsamplerate0-dev libspeexdsp-dev libzstd-dev >/dev/null
cd /tmp
get() { git clone -q https://github.com/mupen64plus/mupen64plus-\$1.git \$1 && git -C \$1 checkout -q \$2; }
get core $C_core; get ui-console $C_ui; get rsp-hle $C_rsp; get audio-sdl $C_audio; get input-sdl $C_input; get rsp-cxd4 $C_cxd4
J=\$(nproc)
D=/out
mkdir -p \$D/plugins \$D/data
make -s -C core/projects/unix all OSD=0 VULKAN=0 -j\$J >/dev/null
cp core/projects/unix/libmupen64plus.so.2.0.0 \$D/libmupen64plus.so.2
cp core/data/* \$D/data/
make -s -C ui-console/projects/unix all APIDIR=/tmp/core/src/api COREDIR=./ PLUGINDIR=./plugins -j\$J >/dev/null
cp ui-console/projects/unix/mupen64plus \$D/
for p in rsp-hle rsp-cxd4 audio-sdl input-sdl; do
	make -s -C \$p/projects/unix all APIDIR=/tmp/core/src/api -j\$J >/dev/null
	cp \$p/projects/unix/mupen64plus-\$p.so \$D/plugins/
done
cp input-sdl/data/InputAutoCfg.ini \$D/data/
git clone -q https://github.com/gonetz/GLideN64.git gliden64 && git -C gliden64 checkout -q $GLN_VER
# OpenGL ES 3.0 asked of the core instead of OpenGL 3.3 core: the Pi's V3D has ES 3.1
# but desktop GL 3.1 only (told 3.3 by Mesa, GLideN64 drew no 3D and its frame never
# reached the window); GLideN64 tells ES from GL by the version string at run time
sed -i 's/M64P_GL_CONTEXT_PROFILE_MASK, M64P_GL_CONTEXT_PROFILE_CORE/M64P_GL_CONTEXT_PROFILE_MASK, M64P_GL_CONTEXT_PROFILE_ES/; s/M64P_GL_CONTEXT_MINOR_VERSION, 3)/M64P_GL_CONTEXT_MINOR_VERSION, 0)/' gliden64/src/Graphics/OpenGLContext/mupen64plus/mupen64plus_DisplayWindow.cpp
grep -q PROFILE_ES gliden64/src/Graphics/OpenGLContext/mupen64plus/mupen64plus_DisplayWindow.cpp
sh -c 'cd gliden64/src && ./getRevision.sh' >/dev/null 2>&1 || true
# GLideN64 looks for ZSTDConfig.cmake; Debian's is zstdConfig.cmake
mkdir -p /opt/zstd && echo \"include(/usr/lib/\$(gcc -dumpmachine)/cmake/zstd/zstdConfig.cmake)\" > /opt/zstd/ZSTDConfig.cmake
cmake -S gliden64/src -B gl -DZSTD_DIR=/opt/zstd -DCMAKE_BUILD_TYPE=Release -DMUPENPLUSAPI=On -DMESA=On -DUSE_SYSTEM_LIBS=On -DNO_OSD=On >/dev/null
cmake --build gl -j\$J 2>&1 | grep -m3 -B2 'error:' || true
cp gl/plugin/Release/mupen64plus-video-GLideN64.so \$D/plugins/
cp gliden64/ini/GLideN64.custom.ini \$D/data/
strip \$D/mupen64plus \$D/libmupen64plus.so.2 \$D/plugins/*.so
cp core/LICENSES \$D/COPYING 2>/dev/null || true
mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O --ignore-missing-info -l\$D \$D/mupen64plus \$D/libmupen64plus.so.2 \$D/plugins/*.so > /tmp/dep 2> /tmp/dep.err || { cat /tmp/dep.err; exit 1; }
sed -n 's/^shlibs:Depends=//p' /tmp/dep > \$D/depends.txt
echo built; cat \$D/depends.txt
"
ls -l "$OUT" "$OUT/plugins"
