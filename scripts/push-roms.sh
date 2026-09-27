#!/bin/sh
# Copy the local test ROMs (roms/<system>/ in this repository, git-ignored:
# never committed, never on a card image) to a board's ROM folders, where the
# launcher lists them within seconds.
#   scripts/push-roms.sh HOST [SYSTEM...]      e.g. omega, cpi@gameshell; nes snes
# .zip and .7z archives are unpacked here first (the player takes plain ROM
# files). Destination: the Omega2+'s SD card (/mnt/mmcblk0p1/pspexp/home/roms)
# or the board user's ~/roms. Files already there are replaced, others kept.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
H="$1"; [ -n "$H" ] || { sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
shift
SRC="$ROOT/roms"
STAGE="$ROOT/build/roms-staging"
[ -d "$SRC" ] || { echo "no $SRC" >&2; exit 1; }
[ $# -gt 0 ] || set -- $(cd "$SRC" && ls -d */ 2>/dev/null | tr -d /)

rm -rf "$STAGE"; mkdir -p "$STAGE"
for sys in "$@"; do
	[ -d "$SRC/$sys" ] || { echo "no $SRC/$sys" >&2; continue; }
	mkdir -p "$STAGE/$sys"
	for f in "$SRC/$sys"/*; do
		[ -f "$f" ] || continue
		case "$f" in
		*.zip|*.ZIP|*.7z|*.7Z) bsdtar -xf "$f" -C "$STAGE/$sys" ;;
		*) ln "$f" "$STAGE/$sys/" 2>/dev/null || cp "$f" "$STAGE/$sys/" ;;
		esac
	done
	echo "$sys: $(ls "$STAGE/$sys" | wc -l | tr -d ' ') files"
done

DEST='$HOME/roms'
ssh "$H" 'test -d /mnt/mmcblk0p1/pspexp' 2>/dev/null && DEST=/mnt/mmcblk0p1/pspexp/home/roms
# GNU tar format: the long names ROM sets use; busybox tar reads it
(cd "$STAGE" && COPYFILE_DISABLE=1 bsdtar --format gnutar -cf - "$@") |
	ssh "$H" "mkdir -p \"$DEST\" && tar -o -C \"$DEST\" -xf -"   # -o: no owners (FAT cards have none)
ssh "$H" "cd \"$DEST\" && for d in $*; do echo \"\$d on the board: \$(ls \"\$d\" | wc -l) files\"; done"
