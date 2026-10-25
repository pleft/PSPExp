#!/bin/sh
# The virtual disc's board side (architecture §12c): pspexp-plugin-host serving a PSP game's sectors
# to the PSPExp plugin, here a stand-in over TCP (-T). A disc image and a CSO of it are made,
# then reads of every kind are checked byte for byte against the image.
set -e
BUILD="$1"
W=$(mktemp -d)
trap 'kill $PID 2>/dev/null; rm -rf "$W"' EXIT
PORT=$((20000 + $$ % 20000))
export PSPEXP_DISC_FILE="$W/disc" PSPEXP_CAPTURE_SOCKET="$W/capture.sock" TMPDIR="$W"
python3 - "$W" <<'PY'
import os, sys, struct, zlib, random
w = sys.argv[1]
random.seed(7)
iso = bytearray(random.getrandbits(8) for _ in range(2048 * 300))
iso[16 * 2048:16 * 2048 + 6] = b'\x01CD001'
open(w + '/game.iso', 'wb').write(iso)
blocks = len(iso) // 2048
idx, data, at = [], bytearray(), 24 + 4 * (blocks + 1)
for i in range(blocks):
    blk = bytes(iso[i * 2048:(i + 1) * 2048])
    c = zlib.compressobj(9, zlib.DEFLATED, -15); z = c.compress(blk) + c.flush()
    idx.append(at + len(data) | (0x80000000 if len(z) >= 2048 else 0))
    data += blk if len(z) >= 2048 else z
idx.append(at + len(data))
h = b'CISO' + struct.pack('<IQIBBH', 24, len(iso), 2048, 1, 0, 0)
open(w + '/game.cso', 'wb').write(h + b''.join(struct.pack('<I', x) for x in idx) + data)
PY
"$BUILD/pspexp-plugin-host" -k -q -T "$PORT" > "$W/recv.log" 2>&1 &
PID=$!
python3 - "$W" "$PORT" <<'PY'
import socket, struct, sys, time
w, port = sys.argv[1], int(sys.argv[2])
iso = open(w + '/game.iso', 'rb').read()
for _ in range(50):
    try: s = socket.create_connection(('127.0.0.1', port)); break
    except OSError: time.sleep(0.1)
def exact(n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise SystemExit('the receiver closed the link')
        b += c
    return b
seq = 0
def ask(off, n):
    global seq
    seq += 1
    s.sendall(b'PXDQ' + struct.pack('<IQI', seq, off, n) + bytes(12))
    while True:   # (other commands first: the mode, CMD_KEEP, as a plugin connects)
        h = exact(12)
        assert h[:4] == b'PXCC', h
        body = exact(struct.unpack('<I', h[8:12])[0])
        if struct.unpack('<I', h[4:8])[0] == 15: break
    rseq, status, size = struct.unpack('<IIQ', body[:16])
    assert rseq == seq, (rseq, seq)
    return status, size, body[16:]
fails = 0
def check(name, cond):
    global fails
    if not cond: fails += 1; print('FAIL', name)
for kind in ('iso', 'cso'):
    open(w + '/disc', 'w').write(w + '/game.' + kind + '\n')
    st, size, d = ask(0, 0)
    check(kind + ' size', st == 0 and size == len(iso) and d == b'')
    for off, n in ((0, 2048), (16 * 2048, 4096), (5000, 3000), (2047, 2), (len(iso) - 100, 100), (123 * 2048 + 17, 65536)):
        st, size, d = ask(off, n)
        check('%s read %d+%d' % (kind, off, n), st == 0 and d == iso[off:off + n])
    st, size, d = ask(len(iso) - 10, 4096)
    check(kind + ' past the end: cut', st == 0 and d == iso[-10:])
    st, size, d = ask(len(iso) + 5, 10)
    check(kind + ' beyond: nothing', st == 0 and d == b'')
    st, size, d = ask(0, 1 << 20)
    check(kind + ' at most 64 KB', st == 0 and d == iso[:65536])
open(w + '/disc', 'w').write(w + '/missing.iso\n')
st, size, d = ask(0, 2048)
check('a missing disc: status 1', st == 1 and d == b'')
print('disc serve: %d checks, %d failures' % (2 * 10 + 1, fails))
sys.exit(1 if fails else 0)
PY
