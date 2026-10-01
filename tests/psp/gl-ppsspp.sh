#!/bin/sh
# The SDL layer's OpenGL against saved pictures, in PPSSPP (the real PSP
# runtime and GE executor): each case runs examples/gl-demo from a set frame
# under a freshly booted emulator (only the first client's dumps are
# faithful) and compares the PSP's frame with tests/gl-ref/<case>.png. The
# demo's frames are the same on every run, so any change is a change in the
# layer, the protocol or the runtime. macOS (sips); a minute or two.
#   tests/psp/gl-ppsspp.sh                 all cases
#   tests/psp/gl-ppsspp.sh cube multi      only these
#   tests/psp/gl-ppsspp.sh --record [...]  take the pictures as the new references
# Needs scripts/build.sh first (build/host/gl-demo, dist/PSPExp). Pictures of
# the run: build/gl-regress/<case>.png, and <case>-diff.png where it differs.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
M="${PPSSPP_MEMSTICK:-$HOME/.config/ppsspp/PSP}/GAME/PSPExp"
REF="$ROOT/tests/gl-ref"
OUT="$ROOT/build/gl-regress"
record=0
[ "${1:-}" = "--record" ] && { record=1; shift; }
mkdir -p "$OUT" "$REF"
trap 'killall PPSSPPSDL 2>/dev/null' EXIT

# name | environment | first frame of the animation
CASES='demo-0||0
demo-60||60
demo-200||200
shader|GLDEMO_SHADER=1|60
one|GLDEMO_ONE=45|0
cube|GLDEMO_CUBE=1|60
sphere|GLDEMO_SPHERE=1|60
multi|GLDEMO_MULTI=1|0'

stop() { killall PPSSPPSDL 2>/dev/null; i=0; while pgrep -x PPSSPPSDL >/dev/null && [ $i -lt 20 ]; do sleep 0.5; i=$((i+1)); done; }
shot() {   # $1 name, $2 environment, $3 first frame -> $OUT/$1.png
	stop
	rm -f "$M"/pspexp-shot-*.bmp "$M/pspexp-rt.log"
	"$ROOT/scripts/dev/run-ppsspp.sh" >/dev/null
	i=0; while ! grep -q "listening" "$M/pspexp-rt.log" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i+1)); done
	env $2 SDL_AUDIODRIVER=dummy PSPEXP_SHOT_EVERY=2 PSPEXP_WINDOW=hide \
		perl -e 'alarm 60; exec @ARGV' "$ROOT/scripts/dev/pspexp-run" --tcp:127.0.0.1:3838 "$ROOT/build/host/gl-demo" 3 "$3" \
		> "$OUT/$1.log" 2>&1
	i=0; while ! grep -q "screenshot written" "$M/pspexp-rt.log" 2>/dev/null && [ $i -lt 40 ]; do sleep 0.3; i=$((i+1)); done
	rm -f "$OUT/$1.png"
	sips -s format png "$M/pspexp-shot-0.bmp" --out "$OUT/$1.png" >/dev/null 2>&1
	[ -s "$OUT/$1.png" ]
}

# the two pictures as BMP (sips), compared here: pixels whose channels differ by more than 8
compare() {   # $1 picture, $2 reference, $3 diff picture to write
	sips -s format bmp "$1" --out "$OUT/.a.bmp" >/dev/null 2>&1 && sips -s format bmp "$2" --out "$OUT/.b.bmp" >/dev/null 2>&1 || { echo "unreadable"; return 1; }
	python3 - "$OUT/.a.bmp" "$OUT/.b.bmp" "$3" <<'EOF'
import struct, sys
def bmp(p):
    d = open(p, 'rb').read()
    off, w, h, bpp = struct.unpack_from('<I', d, 10)[0], *struct.unpack_from('<iiH', d, 18)[:2], struct.unpack_from('<H', d, 28)[0]
    row = (w * bpp // 8 + 3) & ~3
    return w, abs(h), bpp // 8, row, d[off:], h > 0   # (h > 0: rows bottom-up)
wa, ha, ba, ra, a, up = bmp(sys.argv[1]); wb, hb, bb, rb, b, upb = bmp(sys.argv[2])
if up != upb: print("row orders differ"); sys.exit(1)
if (wa, ha) != (wb, hb): print("size %dx%d, reference %dx%d" % (wa, ha, wb, hb)); sys.exit(1)
bad, box = 0, None
mask = bytearray(wa * ha)
for y in range(ha):
    for x in range(wa):
        pa, pb = a[y * ra + x * ba: y * ra + x * ba + 3], b[y * rb + x * bb: y * rb + x * bb + 3]
        if max(abs(pa[k] - pb[k]) for k in range(3)) > 8:
            bad += 1; mask[y * wa + x] = 1
            box = (min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y)) if box else (x, y, x, y)
if not bad: sys.exit(0)
# the differing pixels white on black, as a BMP the caller turns into a PNG
rowo = (wa * 3 + 3) & ~3
px = bytearray(rowo * ha)
for y in range(ha):
    for x in range(wa):
        if mask[y * wa + x]: px[y * rowo + x * 3: y * rowo + x * 3 + 3] = b'\xff\xff\xff'
hdr = b'BM' + struct.pack('<IHHI', 54 + len(px), 0, 0, 54) + struct.pack('<IiiHHIIiiII', 40, wa, ha if up else -ha, 1, 24, 0, len(px), 2835, 2835, 0, 0)
open(sys.argv[3] + '.bmp', 'wb').write(hdr + px)
if up: box = (box[0], ha - 1 - box[3], box[2], ha - 1 - box[1])
print("%d pixels differ, within x %d..%d, y %d..%d" % (bad, box[0], box[2], box[1], box[3])); sys.exit(1)
EOF
}

echo "$CASES" | while IFS='|' read -r name envs first; do
	[ $# -gt 0 ] && { case " $* " in *" $name "*) ;; *) continue ;; esac; }
	if ! shot "$name" "$envs" "$first"; then echo "  FAIL $name: no picture (see $OUT/$name.log)"; continue; fi
	if [ $record = 1 ]; then cp "$OUT/$name.png" "$REF/$name.png"; echo "  recorded $name"; continue; fi
	if [ ! -f "$REF/$name.png" ]; then echo "  FAIL $name: no reference (--record)"; continue; fi
	rm -f "$OUT/$name-diff.png"
	if why=$(compare "$OUT/$name.png" "$REF/$name.png" "$OUT/$name-diff"); then echo "  PASS $name"
	else
		[ -f "$OUT/$name-diff.bmp" ] && sips -s format png "$OUT/$name-diff.bmp" --out "$OUT/$name-diff.png" >/dev/null 2>&1 && rm -f "$OUT/$name-diff.bmp"
		echo "  FAIL $name: $why"
	fi
done | tee "$OUT/report.txt"
rm -f "$OUT/.a.bmp" "$OUT/.b.bmp"
grep -q "FAIL" "$OUT/report.txt" && exit 1
exit 0
