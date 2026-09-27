#!/bin/sh
# A game for a glibc board (GameShell: armhf; Pi: arm64), cross-compiled here
# with Zig, against the board's own SDL2 (the SDL layer is preloaded there):
# BUILDER (scripts/packages/build-*.sh) compiles everything, the objects name
# the SDL functions they call, and a stand-in libSDL2-2.0.so.0 with just those
# is what the program links against; on the board the image's SDL2 answers.
# glibc 2.35 (every image we have is newer), C++'s library static.
#   scripts/packages/cross-glibc.sh gameshell|pi BUILDER OUTNAME
#   -> build/<board>/bin/OUTNAME
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BOARD="$1" BUILDER="$2" NAME="$3"
case "$BOARD" in
gameshell) T="arm-linux-gnueabihf.${GLIBC_VER:-2.35}"; CPU="-mcpu=cortex_a7" ;;
pi)        T="aarch64-linux-gnu.${GLIBC_VER:-2.35}"; CPU="-mcpu=cortex_a72" ;;   # Pi 4 / 400; runs on the Pi 5 too
*) echo "cross-glibc.sh: gameshell or pi" >&2; exit 2 ;;
esac
INC="$ROOT/build/gameshell/gnu/src/SDL2-${SDL_VER:-2.30.12}/include"   # SDL's own headers, any architecture
[ -f "$INC/SDL.h" ] || { echo "SDL2 headers missing: run scripts/gameshell/build.sh shim first" >&2; exit 1; }
command -v zig >/dev/null || { echo "zig not found (brew install zig)" >&2; exit 1; }
OUT="$ROOT/build/$BOARD"
R="$OUT/cross-$NAME"; mkdir -p "$R" "$OUT/bin" "$ROOT/build/zig-cache"
for t in cc cxx; do
	case $t in cc) cmd="zig cc -target $T $CPU" ;; cxx) cmd="zig c++ -target $T $CPU" ;; esac
	printf '#!/bin/sh\nexport ZIG_GLOBAL_CACHE_DIR="%s" ZIG_LOCAL_CACHE_DIR="%s"\nexec %s "$@"\n' \
		"$ROOT/build/zig-cache" "$ROOT/build/zig-cache" "$cmd" > "$R/z$t"
	chmod +x "$R/z$t"
done
# first everything but the final link, then the stand-in SDL2 from what the objects call
W="$R/work" CC="$R/zcc" CXX="$R/zcxx" AR="zig ar" SDLINC="$INC" OUTBIN="$R/unlinked" LINK="-Wl,--version" \
	"$BUILDER" >/dev/null 2>"$R/build.log" || true
ls "$R"/work/obj-*/*.o >/dev/null 2>&1 || { tail -20 "$R/build.log" >&2; exit 1; }
nm -u "$R"/work/obj-*/*.o "$R"/work/lib/*.a 2>/dev/null | awk '{print $NF}' | grep '^SDL_' | sort -u | sed 's/.*/void &(void) {}/' > "$R/sdl-stub.c"
"$R/zcc" -shared -fPIC -Wl,-soname,libSDL2-2.0.so.0 "$R/sdl-stub.c" -o "$R/libSDL2.so"
W="$R/work" CC="$R/zcc" CXX="$R/zcxx" AR="zig ar" SDLINC="$INC" OUTBIN="$OUT/bin/$NAME" LINK="-L$R -lSDL2 -lpthread -ldl -lm" \
	"$BUILDER"
# anything the program needs must come from glibc or SDL2
U=$(nm -D --undefined-only "$OUT/bin/$NAME" | awk '{print $NF}' | grep -v '@' | grep -v '^SDL_' | grep -vE '^_|^__' || true)
[ -z "$U" ] || { echo "$NAME: undefined symbols:" >&2; echo "$U" | head >&2; exit 1; }
