#!/bin/sh
# title: Beneath a Steel Sky (ScummVM, drawn by the PSP)
# Revolution's 1994 cyberpunk adventure, freeware since 2003, on ScummVM (static for the Omega2+: scripts/omega/build.sh scummvm,
# run here when missing). The game's data from Debian's own package, as the
# Debian boards install it (recipes/beneath-a-steel-sky).
set -e
. "$ROOT/scripts/packages/recipes/_lib.sh"
D="$1"
scummvm_game "$D" beneath-a-steel-sky sky . b/beneath-a-steel-sky/beneath-a-steel-sky_0.0372-9_all.deb
cat > "$D/package" <<P
version=0.0372-9
title=Beneath a Steel Sky
description=Revolution's 1994 cyberpunk adventure, freeware since 2003
license=Freeware (Revolution Software); ScummVM is GPL-3.0
requires=pspexp-exec
P
cat > "$D/beneath-a-steel-sky.entry" <<E
title=Beneath a Steel Sky
type=game
description=Revolution's 1994 cyberpunk adventure, freeware since 2003
icon=puzzle
exec=$SCUMMVM_EXEC
env=PSPEXP_KEYS=$SCUMMVM_KEYS
env=PSPEXP_NUB=mouse
pspexp=1
E
