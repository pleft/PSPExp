#!/bin/sh
# The PSP runtime's slots (docs/Package_Manager.md §6.1) end to end in PPSSPP:
# the real boot loader and runtime, the console side through pspexp-ms. The
# emulator's memory stick is a folder here, so every boot's outcome is read
# from active / active.new / fallback directly. Takes a few minutes.
#   tests/psp/slots-ppsspp.sh
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PPSSPP="${PPSSPP:-/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL}"
M="${PPSSPP_MEMSTICK:-$HOME/.config/ppsspp/PSP}/GAME/PSPExp"
MS="$ROOT/build/host/pspexp-ms --tcp 127.0.0.1:3838"
W=$(mktemp -d); trap 'killall PPSSPPSDL 2>/dev/null; rm -rf "$W"' EXIT
pass=0; fail=0
check() { if eval "$1"; then pass=$((pass + 1)); echo "  PASS $2"; else fail=$((fail + 1)); echo "  FAIL $2 ($1)"; fi; }
active() { tr '\n' ' ' < "$M/active" 2>/dev/null; }
stop() { killall PPSSPPSDL 2>/dev/null; i=0; while pgrep -x PPSSPPSDL >/dev/null && [ $i -lt 20 ]; do sleep 0.5; i=$((i+1)); done; sleep 1; }
boot() {   # start the emulator, wait until the runtime listens (or gives up)
	stop; : > "$M/pspexp-rt.log"
	("$PPSSPP" "$M/EBOOT.PBP" > "$ROOT/build/ppsspp.log" 2>&1 &)
	i=0; until grep -q listening "$M/pspexp-rt.log" 2>/dev/null || [ $i -ge 90 ]; do sleep 1; i=$((i+1)); done
	grep -q listening "$M/pspexp-rt.log" || { echo "  (the runtime did not come up: $(tail -1 "$M/pspexp-rt.log" 2>/dev/null))"; return 1; }
}
connect() { $MS ls ms0:/ >/dev/null 2>&1; }   # one connection: the runtime confirms its slot

# a fresh slotted install (slot a) and a second, newer runtime set to install
stop; rm -rf "$M"; "$ROOT/scripts/packages/runtime-slot" --install "$M" >/dev/null
sleep 1; "$ROOT/scripts/packages/runtime-slot" "$W/set" >/dev/null
NEWV=$(sed -n 's/^version=//p' "$W/set/VERSION")

echo "boot and confirm"
boot; check 'grep -q "slot=a" "$M/active" && grep -q "tries=1" "$M/active"' "the loader boots slot a and counts the attempt"
connect; check 'grep -q "tries=0" "$M/active"' "a connection sets the attempts back to 0"

echo "an update, then the new slot"
out=$($MS runtime-update "$W/set" 2>&1); r=$?
[ $r -eq 0 ] || echo "$out" | sed 's/^/    /'
check '[ $r -eq 0 ] && grep -q "slot=b" "$M/active.new" && grep -q "version=$NEWV" "$M/slot-b/VERSION"' "the update fills slot b and writes active.new (the running runtime is untouched)"
check 'grep -q "slot=a" "$M/active"' "active still names slot a until the next start"
boot; check '[ ! -f "$M/active.new" ] && grep -q "slot=b" "$M/active"' "the next start boots slot b"
connect; check 'grep -q "tries=0" "$M/active"' "slot b confirmed"

echo "interrupted updates: each leaves a complete, verified slot booting (§10.3)"
for p in invalidated copy-partial copied version-partial version switch-partial; do
	# back to: running a, b holding an older valid set (the stale VERSION case)
	stop; printf 'slot=a\ntries=0\n' > "$M/active"; rm -f "$M/active.new" "$M/fallback"
	boot; connect
	PSPEXP_SLOT_CRASH=$p $MS runtime-update "$W/set" >/dev/null 2>&1
	boot
	check 'grep -q "slot=a" "$M/active" && [ ! -f "$M/fallback" ] && [ ! -f "$M/active.new" ]' "stopped after $p: the old slot a boots, nothing pending"
done
echo "stopped right after the switch: the new slot boots, the old kept for fallback"
stop; printf 'slot=a\ntries=0\n' > "$M/active"; rm -f "$M/active.new" "$M/fallback"
boot; connect; $MS runtime-update "$W/set" >/dev/null 2>&1
boot; check 'grep -q "slot=b" "$M/active" && [ -f "$M/slot-a/VERSION" ]' "the new slot b boots; slot a stays whole"

echo "a slot that does not connect: fallback after 3 attempts"
stop; printf 'slot=b\ntries=0\n' > "$M/active"; rm -f "$M/fallback"
boot; boot; boot   # three starts, no connection: tries 1, 2, 3
check 'grep -q "tries=3" "$M/active"' "three starts without a connection count 3 attempts"
boot; check 'grep -q "slot=a" "$M/active" && grep -q "from=b to=a" "$M/fallback"' "the fourth start boots slot a and records the fallback"

echo "a stale VERSION over a damaged file is never booted"
stop; printf 'x' | dd of="$M/slot-b/pspexp_runtime.prx" bs=1 seek=4000 conv=notrunc 2>/dev/null
printf 'slot=b\ntries=0\n' > "$M/active"; rm -f "$M/fallback"
boot; check 'grep -q "slot=a" "$M/active" && grep -q "reason=not valid" "$M/fallback"' "slot b does not check out: slot a boots instead"
stop; printf 'slot=a\ntries=0\n' > "$M/active"; printf 'slot=b\ntries=0\n' > "$M/active.new"; rm -f "$M/fallback"
boot; check 'grep -q "slot=a" "$M/active" && [ ! -f "$M/active.new" ]' "an active.new naming that damaged slot is refused and removed"

echo "maintenance mode: another protocol version still installs a runtime"
stop; rm -rf "$M/slot-b" "$M/active.new" "$M/fallback"; printf 'slot=a\ntries=0\n' > "$M/active"
boot
out=$(PSPEXP_TEST_PROTOCOL=99 $MS runtime-update "$W/set" 2>&1); r=$?
check '[ $r -eq 0 ] && grep -q "slot=b" "$M/active.new" && grep -q maintenance "$M/pspexp-rt.log"' "a host speaking another version connects for maintenance and fills slot b"

echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
