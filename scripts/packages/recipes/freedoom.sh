#!/bin/sh
# title: Freedoom Phase 1 and 2 (Chocolate Doom, drawn by the PSP)
# Freedoom 0.13 (BSD licence), two entries, one per phase, on a static
# Chocolate Doom with the SDL layer linked in (the Omega2+).
set -e
. "$ROOT/scripts/packages/recipes/_lib.sh"
D="$1"; V=0.13.0
Z=$(fetch "https://github.com/freedoom/freedoom/releases/download/v$V/freedoom-$V.zip")
unzip -q -j -o "$Z" "freedoom-$V/freedoom1.wad" "freedoom-$V/freedoom2.wad" "freedoom-$V/COPYING.txt" -d "$D"
case "$BOARD" in
omega)       # a static Chocolate Doom with the SDL layer built in (build-omega.sh doom)
	BIN="$ROOT/build/omega/bin/chocolate-doom"
	[ -x "$BIN" ] || "$ROOT/scripts/omega/build.sh" doom
	cp "$BIN" "$D/chocolate-doom"
	# the FPU-less MT7688: sound at the effects' own 11 kHz (integer upsampling
	# in the SDL layer), no OPL music (a third of the CPU)
	printf 'snd_samplerate 11025\nuse_libsamplerate 0\nsnd_pitchshift 0\n' > "$D/doom-omega.cfg"
	REQ="pspexp-exec"
	RUN="exec pspexp-exec ./chocolate-doom"; ARGS=" -nomusic -extraconfig ./doom-omega.cfg -window -nomouse -nograbmouse" ;;
*) echo "freedoom: no build for $BOARD" >&2; exit 3 ;;
esac
cat > "$D/package" <<P
version=$V
title=Freedoom
description=Free Doom-compatible games: Phase 1 (episodes) and Phase 2 (a megawad)
license=BSD (freedoom.github.io)
requires=$REQ
P
for n in 1 2; do
	case $n in 1) t="Freedoom: Phase 1"; d="Four episodes of free Doom";; 2) t="Freedoom: Phase 2"; d="A 32-level megawad, Doom II style";; esac
	cat > "$D/phase$n.entry" <<E
title=$t
type=game
description=$d
icon=swords
exec=$RUN -iwad ./freedoom$n.wad$ARGS
env=PSPEXP_KEYS=$DOOM_KEYS
env=PSPEXP_NUB=keys
env=SDL_VIDEODRIVER=dummy
env=SDL_AUDIODRIVER=dummy
pspexp=1
E
	cp "$ROOT/scripts/packages/art/freedoom-phase$n.png" "$D/phase$n.png"   # the entry's cover
done
