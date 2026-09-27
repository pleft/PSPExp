#!/bin/sh
# title: Game Boy and Game Boy Color (pspexp-retro + Gambatte, drawn by the PSP)
# Two systems, one core: Gambatte (GPL-2.0) plays both; ROMs in ~/roms/gb and
# ~/roms/gbc. No ROMs.
set -e
. "$ROOT/scripts/packages/recipes/_lib.sh"
D="$1"
retro_core "$D" gambatte
OMEGA_VIDEO_DIV=1   # 160x144: the Omega2+ keeps up at 60 fps (Tetris tested)
retro_system "$D" gb "Game Boy" GB "gb" "Nintendo - Game Boy"
retro_system "$D" gbc "Game Boy Color" GBC "gbc" "Nintendo - Game Boy Color"
cat > "$D/package" <<P
version=$RETRO_VERSION
title=Game Boy
description=Game Boy and Game Boy Color: pspexp-retro with the Gambatte core; ROMs in ~/roms/gb and ~/roms/gbc
license=GPL-2.0 (Gambatte); pspexp-retro is part of PSPExp
requires=$RETRO_REQ
P
