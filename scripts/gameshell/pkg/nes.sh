#!/bin/sh
# title: NES (pspexp-retro + QuickNES, drawn by the PSP)
# The Nintendo Entertainment System: the pspexp-retro player with the QuickNES
# core (GPL-2.0), built for speed: on the Omega2+ it takes about half of
# FCEUmm's time per frame (measured 12 vs 25 ms with Super Mario Bros.), the
# difference between full speed and not. The launcher shows an NES tile and
# lists ~/roms/nes. No ROMs.
set -e
. "$ROOT/scripts/gameshell/pkg/_lib.sh"
D="$1"
retro_core "$D" quicknes
retro_system "$D" nes "Nintendo Entertainment System" NES "nes" "Nintendo - Nintendo Entertainment System"
cat > "$D/package" <<P
version=$RETRO_VERSION
title=NES
description=Nintendo Entertainment System: pspexp-retro with the QuickNES core; ROMs in ~/roms/nes
license=GPL-2.0 (QuickNES); pspexp-retro is part of PSPExp
requires=$RETRO_REQ
P
