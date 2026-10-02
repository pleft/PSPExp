#!/bin/sh
# title: Jazz Jackrabbit shareware (OpenJazz, drawn by the PSP)
# Jazz Jackrabbit (Epic MegaGames, 1994), its shareware episode "Turtle
# Terror", on OpenJazz at 480x272 (the PSP's screen: a wider view of the
# level). OpenJazz cross-compiled here, static for the Omega2+
# (scripts/omega/build.sh jazz). Epic's shareware license lets anyone
# pass the shareware on as long as no file is altered or removed, it is called
# shareware and Epic MegaGames is credited: data/ is the archive's 78 files as
# they are (LICENSE.DOC among them).
set -e
. "$ROOT/scripts/packages/recipes/_lib.sh"
D="$1"
case "${BOARD:-omega}" in
omega)          # static, the SDL layer built in
	BIN="$ROOT/build/omega/bin/openjazz"
	[ -x "$BIN" ] || "$ROOT/scripts/omega/build.sh" jazz
	RUN="env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy pspexp-exec ./openjazz"; REQ="pspexp-exec" ;;
*) echo "jazz: no build for $BOARD" >&2; exit 3 ;;
esac
Z=$(jazz_zip)
mkdir -p "$D/data"
unzip -q -o "$Z" -d "$D/data"
cp "$BIN" "$D/openjazz"
chmod +x "$D/openjazz"
cp "$ROOT/../jazz/openjazz/COPYING" "$D/COPYING.openjazz"
cp "$ROOT/scripts/packages/art/jazz.png" "$D/jazz.png"   # the entry's cover
cat > "$D/package" <<P
version=$(sed -n 's/^[[:space:]]*VERSION \([0-9]*\).*/\1/p' "$ROOT/../jazz/openjazz/CMakeLists.txt" | head -1)
title=Jazz Jackrabbit (shareware)
description=$(rf jazz package description)
license=GPL-2.0 (OpenJazz); shareware (Jazz Jackrabbit, Epic MegaGames, 1994: data/LICENSE.DOC)
requires=$REQ
P
cat > "$D/jazz.entry" <<E
title=Jazz Jackrabbit
type=game
description=$(rf jazz entry description)
icon=rabbit
exec=$RUN ./data
env=PSPEXP_KEYS=$JAZZ_KEYS
env=PSPEXP_NUB=keys
graphics=$(rf jazz entry graphics)
pspexp=1
E
