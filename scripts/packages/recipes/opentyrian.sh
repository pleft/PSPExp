#!/bin/sh
# title: OpenTyrian 2.1 (SDL2 build, drawn by the PSP)
# OpenTyrian (SDL2) and the Tyrian 2.1 freeware data, static with the SDL
# layer linked in (the Omega2+).
set -e
. "$ROOT/scripts/packages/recipes/_lib.sh"
D="$1"
case "${BOARD:-omega}" in
omega)       # static, the SDL layer built in (build-omega.sh tyrian)
	BIN="$ROOT/build/omega/bin/opentyrian"
	[ -x "$BIN" ] || "$ROOT/scripts/omega/build.sh" tyrian
	RUN="env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy pspexp-exec ./opentyrian"; REQ="pspexp-exec"
	# 16-bit pages, not CLUT8: over USB the MT7688's CPU is the limit, and
	# indexing costs it more than the extra bytes (measured: 21-25 -> 25 fps)
	EXTRA_ENV="env=PSPEXP_CLUT=0" ;;
*) echo "opentyrian: no build for $BOARD" >&2; exit 3 ;;
esac
Z=$(fetch "https://camanis.net/tyrian/tyrian21.zip")
mkdir -p "$D/data"
unzip -q -j -o "$Z" -d "$D/data"
cp "$BIN" "$D/opentyrian"
cat > "$D/run.sh" <<'R'
#!/bin/sh
# windowed-off config on first start, then the game with the PSP as its screen
CFG="${XDG_CONFIG_HOME:-$HOME/.config}/opentyrian"
mkdir -p "$CFG"
[ -f "$CFG/opentyrian.cfg" ] || printf 'section video\n\titem fullscreen -1\n\titem scaler None\n\titem scaling_mode integer\n\n' > "$CFG/opentyrian.cfg"
exec RUN_CMD --data=./data --no-joystick "$@"
R
sed -i.bak "s|RUN_CMD|$RUN|" "$D/run.sh" && rm -f "$D/run.sh.bak"
chmod +x "$D/run.sh" "$D/opentyrian"
cp "$ROOT/scripts/packages/art/opentyrian.png" "$D/opentyrian.png"   # the entry's cover
cat > "$D/package" <<P
version=2.1
title=OpenTyrian
description=$(rf opentyrian package description)
license=GPL-2.0 (OpenTyrian); freeware data (Tyrian 2.1)
requires=$REQ
P
cat > "$D/opentyrian.entry" <<E
title=OpenTyrian
type=game
description=$(rf opentyrian entry description)
icon=rocket
exec=./run.sh
env=PSPEXP_KEYS=cross=space,circle=lctrl,square=lalt,triangle=return,l=lctrl,r=lalt,start=p,select=escape,up=up,down=down,left=left,right=right
env=PSPEXP_NUB=keys
${EXTRA_ENV:-}
graphics=$(rf opentyrian entry graphics)
pspexp=1
E
