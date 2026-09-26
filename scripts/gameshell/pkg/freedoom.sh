#!/bin/sh
# title: Freedoom Phase 1 and 2 (Chocolate Doom, drawn by the PSP)
# Freedoom 0.13 (BSD licence) run by the GameShell's Chocolate Doom package
# through pspexp-run: 35 fps on the PSP. Two entries, one per phase.
set -e
. "$ROOT/scripts/gameshell/pkg/_lib.sh"
D="$1"; V=0.13.0
Z=$(fetch "https://github.com/freedoom/freedoom/releases/download/v$V/freedoom-$V.zip")
unzip -q -j -o "$Z" "freedoom-$V/freedoom1.wad" "freedoom-$V/freedoom2.wad" "freedoom-$V/COPYING.txt" -d "$D"
cat > "$D/package" <<P
version=$V
title=Freedoom
description=Free Doom-compatible games: Phase 1 (episodes) and Phase 2 (a megawad)
license=BSD (freedoom.github.io)
requires=chocolate-doom pspexp-run
P
for n in 1 2; do
	case $n in 1) t="Freedoom: Phase 1"; d="Four episodes of free Doom";; 2) t="Freedoom: Phase 2"; d="A 32-level megawad, Doom II style";; esac
	cat > "$D/phase$n.entry" <<E
title=$t
type=game
description=$d
icon=swords
exec=exec pspexp-run chocolate-doom -iwad ./freedoom$n.wad
env=PSPEXP_KEYS=$DOOM_KEYS
env=PSPEXP_NUB=keys
pspexp=1
E
done
