#!/bin/sh
# melonDS DS (GPL-3.0), the Nintendo DS emulator melonDS as a libretro core, for a Debian 13
# board: Debian does not package it. Built in a Debian 13 container of the board's architecture
# (as build-flycast.sh). pspexp-retro plays it with its software renderer (the core's 3D in
# software, threaded): pspexp-retro gives no hardware rendering, and the picture is small (the
# DS's two 256x192 screens). Its JIT recompiler for 64-bit ARM is built in; OpenGL off (its networking stays
# built: switching it off leaves its sources without their headers in v1.4.0). Its
# dependencies (melonDS itself, libretro-common, glm, pntr...) are fetched by its CMake at the
# tag below. Patched (scripts/packages/melonds/): a frame pspexp-retro will not show is emulated
# without its 2D drawing (GET_AUDIO_VIDEO_ENABLE), about 30% of the emulation thread on a Pi 400.
# Writes build/<arch>/melonds/ (melondsds_libretro.so, depends.txt, LICENSE) for
# scripts/packages/debs/pspexp-melonds/build.
#   scripts/packages/build-melonds.sh arm64
# MD_CFLAGS ("-O3 -mcpu=cortex-a72") and MD_LTO=ON tune it; MD_OUT another output folder; MD_KEEP_SYMBOLS=1 keeps an unstripped copy too (perf).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; *) echo "arm64 only: a Pi 4, 400 or 5 (the GameShell is far too slow)" >&2; exit 2 ;; esac
MD_VER=v1.4.0   # 2026-09-27
OUT="${MD_OUT:-$ROOT/build/$ARCH/melonds}"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -e MD_CFLAGS -e MD_LTO -e MD_KEEP_SYMBOLS -v "$OUT:/out" -v "$ROOT/scripts/packages/melonds:/patches:ro" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev python3 >/dev/null
cd /tmp
git clone -q --depth 1 --branch $MD_VER https://github.com/JesseTG/melonds-ds.git md
cmake -S md -B b -DCMAKE_BUILD_TYPE=Release -DENABLE_OPENGL=OFF -DENABLE_JIT=ON \\
	-DCMAKE_C_FLAGS=\"\$MD_CFLAGS\" -DCMAKE_CXX_FLAGS=\"\$MD_CFLAGS\" -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=\${MD_LTO:-OFF} >/tmp/cmake.log 2>&1 || { tail -30 /tmp/cmake.log; exit 1; }
(cd b/_deps/melonds-src && patch -p1 < /patches/melonds-skip-draw.patch) >/dev/null
(cd md && patch -p1 < /patches/melondsds-skip-draw.patch) >/dev/null
cmake --build b -j\$(nproc) > /tmp/build.log 2>&1 || { grep -B3 -m5 'error:' /tmp/build.log; exit 1; }
f=\$(find b -name 'melondsds_libretro.so' | head -1); [ -n \"\$f\" ] || { echo 'no core built'; exit 1; }
strip -o /out/melondsds_libretro.so \$f
[ -n \"\$MD_KEEP_SYMBOLS\" ] && cp \$f /out/melondsds_libretro.unstripped.so
cp md/LICENSE /out/LICENSE
mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O --ignore-missing-info /out/melondsds_libretro.so > /tmp/dep 2> /tmp/dep.err || { cat /tmp/dep.err; exit 1; }
sed -n 's/^shlibs:Depends=//p' /tmp/dep > /out/depends.txt
echo built; cat /out/depends.txt
"
ls -l "$OUT"
