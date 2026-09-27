#!/bin/sh
# title: Mega Drive (pspexp-retro + PicoDrive, drawn by the PSP)
# The Sega Mega Drive / Genesis with PicoDrive (MAME-style non-commercial
# licence), light enough for small CPUs; ROMs in ~/roms/md. No ROMs.
set -e
. "$ROOT/scripts/gameshell/pkg/_lib.sh"
D="$1"
retro_core "$D" picodrive
retro_system "$D" md "Mega Drive" MD "md,gen,smd,bin" "Sega - Mega Drive - Genesis"
cat > "$D/package" <<P
version=$RETRO_VERSION
title=Mega Drive
description=Sega Mega Drive / Genesis: pspexp-retro with the PicoDrive core; ROMs in ~/roms/md
license=PicoDrive licence, non-commercial; pspexp-retro is part of PSPExp
requires=$RETRO_REQ
P
