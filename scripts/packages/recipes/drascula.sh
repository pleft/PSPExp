#!/bin/sh
# title: Drascula: The Vampire Strikes Back (ScummVM, drawn by the PSP)
# A 1996 comedy vampire adventure, freeware since 2006, on ScummVM (static for the Omega2+: scripts/omega/build.sh scummvm,
# run here when missing). The game's data from Debian's own package, as the
# Debian boards install it (recipes/drascula).
set -e
. "$ROOT/scripts/packages/recipes/_lib.sh"
D="$1"
scummvm_game "$D" drascula drascula en d/drascula/drascula_1.0+ds4-2_all.deb@ed4af8e2d41ad446e2f3b73533daa1804e2f13f62f596c0b02b8dc39544f22b4 d/drascula/drascula-music_1.0+ds4-2_all.deb@340e0beb1bacf005cbabebc07ebdd88adf6aee363d437f8a7d73cb0c77a93e79
cat > "$D/package" <<P
version=1.0+ds4-2
title=Drascula: The Vampire Strikes Back
description=$(rf drascula package description)
license=Freeware (Alcachofa Soft); ScummVM is GPL-3.0
requires=pspexp-exec
P
cat > "$D/drascula.entry" <<E
title=Drascula: The Vampire Strikes Back
type=game
description=$(rf drascula entry description)
icon=puzzle
exec=$SCUMMVM_EXEC
env=PSPEXP_KEYS=$SCUMMVM_KEYS
env=PSPEXP_NUB=mouse
graphics=$(rf drascula entry graphics)
pspexp=1
E
