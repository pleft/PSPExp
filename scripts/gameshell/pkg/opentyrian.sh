#!/bin/sh
# title: OpenTyrian 2.1 (SDL2 build, drawn by the PSP)
# The SDL2 OpenTyrian compiled on the GameShell (scripts/build-gameshell.sh
# tyrian, run here when the binary is missing) and the Tyrian 2.1 freeware
# data; through pspexp-run: 35 fps on the PSP.
set -e
. "$ROOT/scripts/gameshell/pkg/_lib.sh"
D="$1"
[ "${BOARD:-gameshell}" = gameshell ] || { echo "opentyrian: no build for $BOARD yet" >&2; exit 3; }
BIN="$ROOT/build/gameshell/bin/opentyrian"
[ -x "$BIN" ] || "$ROOT/scripts/build-gameshell.sh" tyrian
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
exec pspexp-run ./opentyrian --data=./data --no-joystick "$@"
R
chmod +x "$D/run.sh" "$D/opentyrian"
cat > "$D/package" <<P
version=2.1
title=OpenTyrian
description=The vertical shooter Tyrian 2.1, freeware since 2004
license=GPL-2.0 (OpenTyrian); freeware data (Tyrian 2.1)
requires=pspexp-run
P
cat > "$D/opentyrian.entry" <<E
title=OpenTyrian
type=game
description=Tyrian 2.1: a vertical shooter, freeware since 2004
icon=rocket
exec=./run.sh
env=PSPEXP_KEYS=cross=space,circle=lctrl,square=lalt,triangle=return,l=lctrl,r=lalt,start=p,select=escape,up=up,down=down,left=left,right=right
env=PSPEXP_NUB=keys
pspexp=1
E
