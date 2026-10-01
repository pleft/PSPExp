#!/bin/sh
# title: Drascula: The Vampire Strikes Back (ScummVM, drawn by the PSP)
# A 1996 comedy vampire adventure, freeware since 2006, on ScummVM (static for the Omega2+: scripts/omega/build.sh scummvm,
# run here when missing). The game's data from Debian's own package, as the
# Debian boards install it (recipes/drascula).
set -e
. "$ROOT/scripts/packages/recipes/_lib.sh"
D="$1"
scummvm_game "$D" drascula drascula en d/drascula/drascula_1.0+ds4-2_all.deb d/drascula/drascula-music_1.0+ds4-2_all.deb
cat > "$D/package" <<P
version=1.0+ds4-2
title=Drascula: The Vampire Strikes Back
description=A 1996 comedy vampire adventure, freeware since 2006
license=Freeware (Alcachofa Soft); ScummVM is GPL-3.0
requires=pspexp-exec
P
cat > "$D/drascula.entry" <<E
title=Drascula: The Vampire Strikes Back
type=game
description=A 1996 comedy vampire adventure, freeware since 2006
icon=puzzle
exec=$SCUMMVM_EXEC
env=PSPEXP_KEYS=$SCUMMVM_KEYS
env=PSPEXP_NUB=mouse
pspexp=1
E
