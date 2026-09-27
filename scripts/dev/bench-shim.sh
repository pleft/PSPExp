#!/bin/sh
# Benchmark of the SDL layer's texture uploads: Chocolate Doom's timedemo
# (demo1 of Freedoom Phase 1, the same frames every run, as fast as the link
# and the host allow) in four modes of the shim:
#   old     every page, 16-bit, even page grid (the shim before 2026-09-26)
#   clut8   256-colour pages as CLUT8
#   delta   only the pages that changed
#   both    the default now
# Prints one table row per mode: frames per second, link KB per frame, pages
# sent (of them CLUT8) and kept, the host's texture work in ms per frame.
# --seconds N stops Doom after N seconds (the demo's start: slow links would
# take long for the whole demo; 0 = the whole timedemo, the default on the
# Mac); the frame rate is then the shim's average over the run.
#
#   scripts/dev/bench-shim.sh [--link usb|tcp:HOST[:PORT]] [--seconds N] [BOARD]
# BOARD is an SSH target with the freedoom package installed (GameShell, Pi:
# /opt/pspexp; Omega: the SD card), or empty for this machine (the Mac: brew's
# chocolate-doom and ../doom/freedoom-*/freedoom1.wad). The link defaults to
# usb: the PSP on the board's USB, the running console hands it over. For a
# stand-in PSP: build/host/pspexp-sdl-server on the Mac, --link tcp:<mac>.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LINK=usb
SECS=
while [ $# -gt 0 ]; do
	case "$1" in
	--link) LINK="$2"; shift 2 ;;
	--seconds) SECS="$2"; shift 2 ;;
	*) break ;;
	esac
done
BOARD="$1"
[ -n "$SECS" ] || { [ -z "$BOARD" ] && SECS=0 || SECS=60; }
# stop the game after SECS (pspexp-exec does not pass signals on: by name)
STOP='S=$1; shift; "$@" & P=$!; if [ "$S" -gt 0 ]; then (sleep "$S"; pkill -TERM -x chocolate-doom 2>/dev/null || killall -TERM chocolate-doom 2>/dev/null) & fi; wait $P'


# the command that runs one timedemo on the target, with the mode's environment
run_one() {   # $1 = environment assignments
	if [ -z "$BOARD" ]; then
		WAD="$(ls "$ROOT"/../doom/freedoom-*/freedoom1.wad 2>/dev/null | head -1)"
		sh -c "$STOP" sh "$SECS" env $1 PSPEXP="$LINK" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
			"$ROOT/scripts/dev/pspexp-run" chocolate-doom -iwad "$WAD" -timedemo demo1 -nosound -nomouse 2>&1
	else
		ssh "$BOARD" "
			if [ -d /mnt/mmcblk0p1/pspexp/opt/apps/freedoom ]; then   # Omega: static Doom, the shim linked in
				cd /mnt/mmcblk0p1/pspexp/opt/apps/freedoom; export PATH=/mnt/mmcblk0p1/pspexp:\$PATH
				D='pspexp-exec ./chocolate-doom -extraconfig ./doom-omega.cfg'
			else
				cd /opt/pspexp/apps/freedoom; export PATH=/usr/local/bin:\$PATH:/usr/games
				D='pspexp-run chocolate-doom'
			fi
			stop() { $STOP; }
			stop $SECS env $1 PSPEXP=$LINK SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \$D -iwad ./freedoom1.wad -timedemo demo1 -nosound -nomouse 2>&1"
	fi
}

printf '| mode | fps | link KB/frame | pages sent (CLUT8) | pages kept | texture ms/frame |\n'
printf '|---|---|---|---|---|---|\n'
for mode in old clut8 delta both; do
	case $mode in
	old)   E="PSPEXP_DELTA=0 PSPEXP_CLUT=0" ;;
	clut8) E="PSPEXP_DELTA=0" ;;
	delta) E="PSPEXP_CLUT=0" ;;
	both)  E="PSPEXP_BENCH=1" ;;
	esac
	out=$(run_one "$E" || true)
	tot=$(printf '%s\n' "$out" | grep 'totals:' | tail -1)
	if [ -z "$tot" ]; then
		# stopped from outside: the shim printed no totals; average its 2-second
		# reports instead (the first one is start-up: left out)
		tot=$(printf '%s\n' "$out" | awk '
			/\] [0-9.]+ fps / { for (i = 1; i <= NF; i++) if ($(i+1) == "fps") { f = $i; break } nf++; if (nf > 1) { fs += f; nfs++ } }
			/\] link [0-9.]+ KB\/frame/ { nl++; if (nl > 1) { kb += $3; sent += $7; t8 += $9 + 0; kept += $11; ms += $14; n++ } }
			END { if (n) printf "totals: %.1f fps  link %.1f KB/frame  pages sent %d (clut8 %d) kept %d  texture work %.2f ms/frame\n", fs / (nfs ? nfs : 1), kb / n, sent, t8, kept, ms / n }')
	fi
	fps=$(printf '%s\n' "$out" | sed -n 's/.*realtics (\([0-9.]*\) fps).*/\1/p' | tail -1)
	[ -n "$fps" ] || fps=$(printf '%s\n' "$tot" | sed -n 's/.*totals: \([0-9.]*\) fps.*/\1/p')
	kb=$(printf '%s\n' "$tot" | sed -n 's/.*link \([0-9.]*\) KB.*/\1/p')
	sent=$(printf '%s\n' "$tot" | sed -n 's/.*pages sent \([0-9]*\) (clut8 \([0-9]*\)).*/\1 (\2)/p')
	kept=$(printf '%s\n' "$tot" | sed -n 's/.*kept \([0-9]*\).*/\1/p')
	ms=$(printf '%s\n' "$tot" | sed -n 's/.*texture work \([0-9.]*\) ms.*/\1/p')
	[ -n "$fps" ] || { echo "| $mode | failed: $(printf '%s\n' "$out" | tail -1) | | | | |"; continue; }
	printf '| %s | %s | %s | %s | %s | %s |\n' "$mode" "$fps" "$kb" "$sent" "$kept" "$ms"
done
