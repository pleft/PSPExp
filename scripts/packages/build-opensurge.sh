#!/bin/sh
# Open Surge (GPL-3.0; its art and music under free licences), which Debian 13
# does not carry, for a Debian 13 board: built in a Debian 13 container of the
# board's architecture (as build-easyrpg.sh). Allegro 5 is built here too, with
# its SDL2 backend (ALLEGRO_SDL): Debian's draws with OpenGL through its own
# X11 window, which the SDL layer cannot reach; through SDL its window, input,
# sound and OpenGL context are the layer's, as any SDL game's. Allegro and
# SurgeScript go with it as private libraries (lib/, found through RPATH
# \$ORIGIN/lib): Open Surge links statically only on Windows. Writes build/<arch>/opensurge/
# (opensurge, its data in share/, depends.txt) for
# scripts/packages/debs/pspexp-opensurge/build.
#   scripts/packages/build-opensurge.sh arm64|armhf
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="${1:?arm64 | armhf}"
case "$ARCH" in arm64) PLAT=linux/arm64 ;; armhf) PLAT=linux/arm/v7 ;; *) echo "arm64 | armhf" >&2; exit 2 ;; esac
OS_VER=v0.6.1.3 SS_VER=v0.6.1 AL_VER=5.2.11.3
OUT="$ROOT/build/$ARCH/opensurge"
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform "$PLAT" -v "$OUT:/out" -v "$ROOT/scripts/packages:/stub:ro" debian:trixie sh -ec "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates pkg-config dpkg-dev \
	libsdl2-dev libgl-dev libpng-dev libjpeg-dev libwebp-dev libfreetype-dev libogg-dev libvorbis-dev libopusfile-dev libphysfs-dev >/dev/null
cd /tmp
git clone -q --depth 1 -b $AL_VER https://github.com/liballeg/allegro5.git allegro
cmake -S allegro -B al -DCMAKE_BUILD_TYPE=Release -DALLEGRO_SDL=ON -DSHARED=ON -DWANT_DOCS=OFF -DWANT_EXAMPLES=OFF \
	-DWANT_TESTS=OFF -DWANT_DEMO=OFF -DWANT_VIDEO=OFF -DWANT_NATIVE_DIALOG=OFF -DWANT_OPENAL=OFF -DWANT_PULSEAUDIO=OFF \
	-DWANT_ALSA=OFF -DWANT_FLAC=OFF -DWANT_MODAUDIO=OFF -DWANT_OPUS=ON -DCMAKE_INSTALL_PREFIX=/opt/al >/dev/null
cmake --build al -j\$(nproc) >/dev/null && cmake --install al >/dev/null
# the dialog add-on: a stand-in (allegro-dialog-stub.c), not GTK's
cp allegro/addons/native_dialog/allegro5/allegro_native_dialog.h /opt/al/include/allegro5/
gcc -O2 -fPIC -shared -Wl,-soname,liballegro_dialog.so.5.2 -I/opt/al/include -o /opt/al/lib/liballegro_dialog.so.5.2.11 /stub/allegro-dialog-stub.c -L/opt/al/lib -lallegro
ln -sf liballegro_dialog.so.5.2.11 /opt/al/lib/liballegro_dialog.so.5.2 && ln -sf liballegro_dialog.so.5.2 /opt/al/lib/liballegro_dialog.so
sed 's/allegro_font/allegro_dialog/g; s/Allegro font/Allegro dialog stub/' /opt/al/lib/pkgconfig/allegro_font-5.pc > /opt/al/lib/pkgconfig/allegro_dialog-5.pc
git clone -q --depth 1 -b $SS_VER https://github.com/alemart/surgescript.git surgescript
cmake -S surgescript -B ss -DCMAKE_BUILD_TYPE=Release -DWANT_SHARED=ON -DWANT_STATIC=OFF -DCMAKE_INSTALL_PREFIX=/opt/ss >/dev/null
cmake --build ss -j\$(nproc) >/dev/null && cmake --install ss >/dev/null
git clone -q --depth 1 -b $OS_VER https://github.com/alemart/opensurge.git opensurge
PKG_CONFIG_PATH=/opt/al/lib/pkgconfig:/opt/ss/lib/pkgconfig:/opt/ss/share/pkgconfig \
cmake -S opensurge -B os -DCMAKE_BUILD_TYPE=Release -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_INSTALL_RPATH='\$ORIGIN/lib' \
	-DCMAKE_PREFIX_PATH='/opt/al;/opt/ss' -DGAME_BINDIR=/usr/games -DGAME_DATADIR=/usr/share/games/opensurge \
	-DDESKTOP_INSTALL=OFF -DCMAKE_INSTALL_PREFIX=/usr >/dev/null
cmake --build os -j\$(nproc) >/dev/null
strip os/opensurge
cp os/opensurge /out/
mkdir -p /out/lib && cp -P /opt/al/lib/liballegro*.so* /opt/ss/lib/libsurgescript*.so* /out/lib/ && strip /out/lib/*.so.*.* 2>/dev/null || true
mkdir -p /out/share && cp -R opensurge/* /out/share/ && cd /out/share && rm -rf src docs CMakeLists.txt *.md build* .git* 2>/dev/null; cd /tmp
cp opensurge/LICENSE /out/COPYING 2>/dev/null || true
mkdir -p debian && printf 'Source: x\n' > debian/control
dpkg-shlibdeps -O -l/out/lib os/opensurge /out/lib/*.so.*.* 2>/dev/null | sed 's/^shlibs:Depends=//' > /out/depends.txt
echo built; cat /out/depends.txt
"
ls -l "$OUT/opensurge"; du -sh "$OUT/share"
