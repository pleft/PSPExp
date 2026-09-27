#!/bin/sh
# title: Mega Drive (pspexp-retro + PicoDrive, drawn by the PSP)
# The Sega Mega Drive / Genesis with PicoDrive (MAME-style non-commercial
# licence), light enough for small CPUs; ROMs in ~/roms/md. No ROMs.
set -e
. "$ROOT/scripts/gameshell/pkg/_lib.sh"
D="$1"
# not for the Omega2+: measured there, too slow to enjoy (Super Mario World
# 46-51 of 50 fps with audio dropouts; Sonic 33 of 60 fps)
[ "$BOARD" = omega ] && { echo "megadrive: not for the Omega2+ (too slow there); GameShell or Pi" >&2; exit 3; }
retro_core "$D" picodrive
retro_system "$D" md "Mega Drive" MD "md,gen,smd,bin" "Sega - Mega Drive - Genesis"
cat > "$D/package" <<P
version=$RETRO_VERSION
title=Mega Drive
description=Sega Mega Drive / Genesis: pspexp-retro with the PicoDrive core; ROMs in ~/roms/md
license=PicoDrive licence, non-commercial; pspexp-retro is part of PSPExp
requires=$RETRO_REQ
P
