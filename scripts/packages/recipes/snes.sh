#!/bin/sh
# title: SNES (pspexp-retro + Snes9x 2005, drawn by the PSP)
# The Super Nintendo with Snes9x 2005, the light Snes9x for small CPUs
# (Snes9x licence: free, non-commercial); ROMs in ~/roms/snes. No ROMs.
set -e
. "$ROOT/scripts/packages/recipes/_lib.sh"
D="$1"
# not for the Omega2+: measured there, too slow to enjoy (Super Mario World
# 46-51 of 50 fps with audio dropouts; Sonic 33 of 60 fps)
[ "$BOARD" = omega ] && { echo "snes: not for the Omega2+ (too slow there); GameShell or Pi" >&2; exit 3; }
retro_core "$D" snes9x2005
retro_system "$D" snes "Super Nintendo" SNES "sfc,smc" "Nintendo - Super Nintendo Entertainment System"
cat > "$D/package" <<P
version=$RETRO_VERSION
title=SNES
description=Super Nintendo: pspexp-retro with the Snes9x 2005 core; ROMs in ~/roms/snes
license=Snes9x licence, non-commercial (Snes9x 2005); pspexp-retro is part of PSPExp
requires=$RETRO_REQ
P
