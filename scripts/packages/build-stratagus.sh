#!/bin/sh
# Stratagus (the engine) with War1gus (Warcraft: Orcs & Humans) and Wargus
# (Warcraft II), all GPL-2.0, built in a Debian 13 container of the board's
# architecture (as build-srb2.sh) from their master branches at fixed
# commits: their newest on 2026-10-08 (the last release tags, 3.3.2, are
# from 2022). Stratagus is unmodified; War1gus's war1tool gets one fix
# (compatibility/war1gus/war1tool-optional-cutscenes.patch: it crashed with
# only DATA.WAR); Wargus's scripts play maps at the PSP's 480x272 with a
# layout for it (compatibility/wargus/wargus-small-screen.patch). The games
# run through the SDL layer like any other SDL2 program; their data is the
# player's own copy, extracted on the board by war1tool / wartool. Writes build/<arch>/stratagus/: stratagus,
# war1gus/ and wargus/ (the extraction tool, the game's scripts and maps in
# share/), depends.txt, COPYING; for scripts/packages/debs/pspexp-stratagus,
# pspexp-war1gus and pspexp-wargus.
#   scripts/packages/build-stratagus.sh arm64|armhf
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64 | armhf}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; armhf) PLAT=linux/arm/v7 ;; *) echo "arm64 | armhf" >&2; exit 2 ;; esac
STRATAGUS=3d87c93f7fd8c0b62ee1be5df0a6d9efc72ca6cc
WAR1GUS=8ec75764c7b4706a4405004438e810c050228b94
WARGUS=cde1a0718a0058cc651ecd56ff8149fc39f624e9
OUT="$ROOT/build/$ARCH/stratagus"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" -v "$ROOT/compatibility:/compat:ro" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev \
	libsdl2-dev libsdl2-mixer-dev libsdl2-image-dev liblua5.1-0-dev libtolua++5.1-dev libpng-dev zlib1g-dev libbz2-dev >/dev/null
cd /tmp
for r in stratagus:$STRATAGUS war1gus:$WAR1GUS wargus:$WARGUS; do
	n=\${r%%:*}; c=\${r#*:}
	git init -q \$n && git -C \$n fetch -q --depth 1 https://github.com/Wargus/\$n.git \$c && git -c advice.detachedHead=false -C \$n checkout -q FETCH_HEAD
done
git -C war1gus apply /compat/war1gus/war1tool-optional-cutscenes.patch
git -C wargus apply /compat/wargus/wargus-small-screen.patch
git -C stratagus submodule update -q --init --depth 1 2>/dev/null || true
cmake -S stratagus -B bs -DCMAKE_BUILD_TYPE=Release -DENABLE_DEV=ON -DCMAKE_INSTALL_PREFIX=/usr > cfg.log 2>&1 || { tail -20 cfg.log; exit 1; }
cmake --build bs -j\$(nproc) 2>&1 | grep -m5 -B2 'error:' || true
cmake --install bs --prefix /tmp/sdev >/dev/null
for g in war1gus:war1tool wargus:wartool; do
	n=\${g%%:*}; t=\${g#*:}
	cmake -S \$n -B b\$n -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DSTRATAGUS=/usr/games/stratagus \
		-DSTRATAGUS_INCLUDE_DIR=/tmp/sdev/include > cfg\$n.log 2>&1 || { tail -20 cfg\$n.log; exit 1; }
	cmake --build b\$n -j\$(nproc) 2>&1 | grep -m5 -B2 'error:' || true
	T=\$(find b\$n -maxdepth 1 -type f -name \$t -perm -u+x | head -1); [ -n \"\$T\" ]
	strip \$T && mkdir -p /out/\$n/share && cp \$T /out/\$n/
	for c in campaigns contrib maps scripts shaders; do if [ -d \$n/\$c ]; then cp -r \$n/\$c /out/\$n/share/; fi; done
	cp \$n/COPYING /out/\$n/COPYING
done
B=\$(find bs -maxdepth 1 -type f -name stratagus -perm -u+x | head -1); [ -n \"\$B\" ]
strip \$B && cp \$B /out/stratagus && cp stratagus/COPYING /out/COPYING
mkdir -p debian && printf 'Source: x\\n' > debian/control
for f in /out/stratagus /out/war1gus/war1tool /out/wargus/wartool; do
	dpkg-shlibdeps -O \$f 2>/dev/null | sed 's/^shlibs:Depends=//' > \$f.depends
done
echo built; cat /out/*.depends /out/*/*.depends
"
ls -la "$OUT"
