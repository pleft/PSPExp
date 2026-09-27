#!/bin/sh
# title: NES (pspexp-retro + FCEUmm, drawn by the PSP)
# The Nintendo Entertainment System: the pspexp-retro player with the FCEUmm
# core (GPL-2.0); the launcher shows an NES tile and lists ~/roms/nes. No ROMs.
set -e
. "$ROOT/scripts/gameshell/pkg/_lib.sh"
D="$1"
retro_core "$D" fceumm
retro_system "$D" nes "Nintendo Entertainment System" NES "nes,fds,unf,unif" "Nintendo - Nintendo Entertainment System"
cat > "$D/package" <<P
version=$RETRO_VERSION
title=NES
description=Nintendo Entertainment System: pspexp-retro with the FCEUmm core; ROMs in ~/roms/nes
license=GPL-2.0 (FCEUmm); pspexp-retro is part of PSPExp
requires=$RETRO_REQ
P
