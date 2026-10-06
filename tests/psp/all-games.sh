#!/bin/sh
# Every game on the real PSP, started from the launcher as a player would (on the board, with the
# PSP connected and nobody using it). For each: does it start, how many frames (median / best a
# second), does its sound reach the PSP, do the nub's four directions and cross reach it, does
# holding START + SELECT end it, is the PSP still there afterwards. The console's games get the
# test's settings from a systemd drop-in for the run (removed at the end): input logged
# (PSPEXP_LOG_EVENTS), a PSP screenshot every 300 frames, and PSPEXP_SCRIPT: the nub up, down,
# left, right from 15 s, cross at 20 s, START + SELECT held from 28 s.
#   tests/psp/all-games.sh                     every installed game (type=game entries)
#   tests/psp/all-games.sh ENTRY... "Nintendo 64::Super Mario 64"...   those (a ROM: section::title)
# Results in $OUT (default /tmp/pspexp-all-games): results.tsv, logs/<id>.log, shots/<id>.bmp.
# Stops at the first game after which the PSP is gone (it crashed or switched off).
# Limits: the input columns count the events a program takes from SDL's queue; one that reads the
# state instead (SDL_GetKeyboardState, SDL_GameControllerGetAxis), or the nub as a mouse, shows 0
# while working. Each screenshot stalls the PSP a moment: its sound queue jumps then.
OUT=${OUT:-/tmp/pspexp-all-games}; L=/tmp/pspexp-launcher.log; R=$OUT/results.tsv
DROP=/etc/systemd/system/pspexp-term.service.d/all-games-test.conf
SCRIPT="15000:nub:0x-120,16000:nub:0x120,17000:nub:-120x0,18000:nub:120x0,19000:nub:0x0,20000:pad:cross,28000:+pad:start,28000:+pad:select"
C() { pspexp-term --ctl "$*" 2>/dev/null; }
st() { C state | grep "^$1=" | cut -d= -f2-; }
ready() { i=0; until [ "$(st connected)" = 1 ] && [ -z "$(st running)" ] || [ $i -ge 40 ]; do i=$((i+1)); sleep 1; done; }
clear_shots() { for f in $(C "fs ls ms0:/PSP/GAME/PSPExp" | grep -o "pspexp-shot-[0-9]*\.bmp"); do C "fs rm ms0:/PSP/GAME/PSPExp/$f" >/dev/null; done; }
console_restart() { sudo systemctl daemon-reload && sudo systemctl restart pspexp-term; sleep 2; ready; }
finish() { C end >/dev/null; sudo rm -f "$DROP"; console_restart; }

[ $# -gt 0 ] || set -- $(for f in /opt/pspexp/apps/*/*.entry; do grep -q "^type=game" "$f" && echo "$f"; done)
mkdir -p "$OUT/logs" "$OUT/shots"
[ -f "$R" ] || printf "id\ttitle\tstarted\tfps\taudio_max\tunderruns\tup\tdown\tleft\tright\tcross\tquit\tlink\tnotes\n" > "$R"
sudo mkdir -p "$(dirname "$DROP")"
printf '[Service]\nEnvironment=PSPEXP_LOG_EVENTS=1 PSPEXP_SHOT_EVERY=300 "PSPEXP_SCRIPT=%s"\n' "$SCRIPT" | sudo tee "$DROP" >/dev/null
trap finish EXIT
trap 'exit 130' INT TERM
console_restart
for E in "$@"; do
	case "$E" in
	*::*) cat=${E%%::*}; title=${E#*::}; id=$(echo "$cat-$title" | tr -c 'A-Za-z0-9\n' '_') ;;
	*) cat=Games; id=$(basename "$(dirname "$E")")_$(basename "$E" .entry); title=$(grep -m1 '^title=' "$E" | cut -d= -f2-) ;;
	esac
	grep -q "^$id	" "$R" && continue
	ready
	clear_shots
	: > "$L"   # a fresh log for each game (the console also starts it afresh past 256 KB)
	C "open launcher" >/dev/null; C "category $cat" >/dev/null
	sel=$(C "select $title"); notes=""
	[ "$sel" = ok ] || notes="not in $cat: $sel"
	C "press cross" >/dev/null
	started=0; i=0; while [ $i -lt 60 ]; do [ -n "$(st running)" ] && { started=1; break; }; i=$((i+1)); sleep 1; done
	quit=no; i=0
	if [ $started = 1 ]; then
		while [ $i -lt 70 ]; do [ -z "$(st running)" ] && { quit=yes; break; }; i=$((i+1)); sleep 1; done
		if [ $quit = no ]; then   # what a player would have been stuck with
			P=$(st program_pid); notes="$notes; still running at 70 s, killed"
			for p in $(ps -o pid= --ppid "$P") "$P"; do pkill -KILL -P "$p" 2>/dev/null; done; kill -KILL "$P" 2>/dev/null
			sleep 3
		fi
	fi
	link=no; i=0; while [ $i -lt 25 ]; do [ "$(st connected)" = 1 ] && { link=yes; break; }; i=$((i+1)); sleep 1; done
	G=$OUT/logs/$id.log; cp "$L" "$G"
	fps=$(grep -a ' fps  ' "$G" | grep -v totals | awk '{print $2}' | sort -n | awk '{a[NR]=$1} END{if(NR) printf "%s/%s", a[int((NR+1)/2)], a[NR]; else print "-"}')
	amax=$(grep -a -o 'audio buffered [0-9]*' "$G" | awk '{if($3>m)m=$3} END{print m+0}')
	und=$(grep -a -o 'underruns [0-9]*' "$G" | awk '{if($2>m)m=$2} END{print m+0}')
	up=$(grep -a -c -E 'axis lefty -|axis 1 -|controller.*dpup|hat 1$|key Up down' "$G")
	down=$(grep -a -c -E 'axis lefty [0-9]|axis 1 [0-9]|controller.*dpdown|hat 4$|key Down down' "$G")
	left=$(grep -a -c -E 'axis leftx -|axis 0 -|controller.*dpleft|hat 8$|key Left down' "$G")
	right=$(grep -a -c -E 'axis leftx [0-9]|axis 0 [0-9]|controller.*dpright|hat 2$|key Right down' "$G")
	cross=$(awk '/pad:cross at/{on=1;next} /script:/{on=0} on && /event: /{n++} END{print n+0}' "$G")
	grep -a -q 'leaving' "$G" && notes="$notes; ended by the 2 s way out"
	grep -a -q 'since asked to quit: ended\|after leaving: ended' "$G" && notes="$notes; ended by the layer's watchdog"
	err=$(grep -a -i -E 'error|fail|cannot|not found|segmentation|abort' "$G" | grep -v 'errors 0' | head -2 | tr '\t\n' '  ' | cut -c1-160)
	[ -n "$err" ] && notes="$notes; $err"
	if [ $link = yes ]; then   # the latest screenshot but one (the last may be the way out)
		n=$(C "fs ls ms0:/PSP/GAME/PSPExp" | grep -o 'pspexp-shot-[0-9]*' | grep -o '[0-9]*$' | sort -n | tail -2 | head -1)
		[ -n "$n" ] && C "fs get ms0:/PSP/GAME/PSPExp/pspexp-shot-$n.bmp $OUT/shots/$id.bmp" >/dev/null
		clear_shots
	fi
	printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$id" "$title" $started "$fps" "$amax" "$und" "$up" "$down" "$left" "$right" "$cross" $quit $link "$notes" >> "$R"
	echo "$(date +%T) $id started=$started quit=$quit link=$link fps=$fps"
	[ $link = yes ] || { echo "the PSP is gone: stopping (switch it on and run again: finished games are kept)"; exit 3; }
done
echo done
