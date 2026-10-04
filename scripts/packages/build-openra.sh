#!/bin/sh
# OpenRA release-20250330 (GPL-3.0: Red Alert, Tiberian Dawn and Dune 2000
# remade) for an arm64 board: the project publishes no Linux arm64 build.
# Built in Microsoft's .NET 8 SDK container (OpenRA targets net6) for linux/arm64 as the project's
# own packaging does (packaging/functions.sh: dotnet publish, self-contained,
# so the board needs no .NET), with its engine files and the three mods.
# The original games' content is installed by OpenRA's own content installer
# (its Quick Install: OpenRA's mirrors of the freeware content). Writes
# build/arm64/openra/ (lib/, depends.txt)
# for scripts/packages/debs/pspexp-openra/build.
#   scripts/packages/build-openra.sh
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VER=release-20250330
OUT="$ROOT/build/arm64/openra"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform linux/arm64 -v "$OUT:/out" mcr.microsoft.com/dotnet/sdk:8.0 sh -ec "
export DEBIAN_FRONTEND=noninteractive DOTNET_CLI_TELEMETRY_OPTOUT=1
apt-get update -qq
apt-get install -y -qq --no-install-recommends git make curl unzip ca-certificates dpkg-dev patchelf gcc libc6-dev >/dev/null
cd /tmp
git -c advice.detachedHead=false clone -q --depth 1 -b $VER https://github.com/OpenRA/OpenRA.git src
cd src
. packaging/functions.sh
install_assemblies \$PWD /tmp/o/lib linux-arm64 net6 True True True > /tmp/build.log 2>&1 || { tail -30 /tmp/build.log; exit 1; }
install_data \$PWD /tmp/o/lib ra cnc d2k > /dev/null
echo \$VER > /tmp/o/lib/VERSION
# OpenRA loads its own SDL2.so through .NET, which looks its functions up in that file and what
# it needs, never in a preloaded library: SDL2.so becomes an empty library needing the PSPExp
# layer (the board-GPU one, which pspexp-run preloads) and then Debian's SDL2, so the layer's
# functions are found first and the rest are SDL's (its own SDL build is left out)
rm /tmp/o/lib/SDL2.so
echo '/* OpenRA SDL2.so: the PSPExp layer, then the system SDL2 */' > /tmp/w.c
gcc -shared -fPIC -o /tmp/o/lib/SDL2.so /tmp/w.c
patchelf --add-needed libSDL2-2.0.so.0 /tmp/o/lib/SDL2.so
patchelf --add-needed /usr/lib/pspexp/libsdl-pspexp-hostgl.so /tmp/o/lib/SDL2.so
rm -f /tmp/o/lib/libcoreclrtraceptprovider.so   # (tracing only: it alone wants liblttng-ust)

# (out through tar: cp into the Mac's shared folder fails on the maps' sparse files)
tar -C /tmp/o -cf - lib | tar -C /out -xf -
# what the bundled .NET runtime and natives need on Debian 13 (this container is Debian 12, so
# dpkg-shlibdeps would name its packages): ICU and OpenSSL are loaded by .NET at run time
echo libc6, libstdc++6, libgcc-s1, zlib1g, libicu76, libssl3t64, libsdl2-2.0-0 > /out/depends.txt
ls /out/lib | wc -l; readelf -d /out/lib/SDL2.so | grep NEEDED
"
