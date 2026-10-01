#!/bin/sh
# title: Lure of the Temptress (ScummVM, drawn by the PSP)
# Revolution's 1992 fantasy adventure, freeware since 2003, on ScummVM (static for the Omega2+: scripts/omega/build.sh scummvm,
# run here when missing). The game's data from Debian's own package, as the
# Debian boards install it (recipes/lure-of-the-temptress).
set -e
. "$ROOT/scripts/packages/recipes/_lib.sh"
D="$1"
scummvm_game "$D" lure-of-the-temptress lure en l/lure-of-the-temptress/lure-of-the-temptress_1.1+ds3-1_all.deb
cat > "$D/package" <<P
version=1.1+ds3-1
title=Lure of the Temptress
description=Revolution's 1992 fantasy adventure, freeware since 2003
license=Freeware (Revolution Software); ScummVM is GPL-3.0
requires=pspexp-exec
P
cat > "$D/lure-of-the-temptress.entry" <<E
title=Lure of the Temptress
type=game
description=Revolution's 1992 fantasy adventure, freeware since 2003
icon=puzzle
exec=$SCUMMVM_EXEC
env=PSPEXP_KEYS=$SCUMMVM_KEYS
env=PSPEXP_NUB=mouse
pspexp=1
E
