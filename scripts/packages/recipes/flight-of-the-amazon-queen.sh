#!/bin/sh
# title: Flight of the Amazon Queen (ScummVM, drawn by the PSP)
# A 1995 jungle adventure with a pilot and a mad scientist, freeware since 2004, on ScummVM (static for the Omega2+: scripts/omega/build.sh scummvm,
# run here when missing). The game's data from Debian's own package, as the
# Debian boards install it (recipes/flight-of-the-amazon-queen).
set -e
. "$ROOT/scripts/packages/recipes/_lib.sh"
D="$1"
scummvm_game "$D" flight-of-the-amazon-queen queen . f/flight-of-the-amazon-queen/flight-of-the-amazon-queen_1.0.0-11_all.deb@234beaf4e576006068b5bd3e85e153a3cc97cd78d7927e33284d675fda392c8a
cat > "$D/package" <<P
version=1.0.0-11
title=Flight of the Amazon Queen
description=A 1995 jungle adventure with a pilot and a mad scientist, freeware since 2004
license=Freeware (John Passfield, Steven Stamatiadis); ScummVM is GPL-3.0
requires=pspexp-exec
P
cat > "$D/flight-of-the-amazon-queen.entry" <<E
title=Flight of the Amazon Queen
type=game
description=A 1995 jungle adventure with a pilot and a mad scientist, freeware since 2004
icon=puzzle
exec=$SCUMMVM_EXEC
env=PSPEXP_KEYS=$SCUMMVM_KEYS
env=PSPEXP_NUB=mouse
pspexp=1
E
