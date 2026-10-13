#!/bin/sh
# refusedlogtest.sh - S256: a full arena refuses writes without logging each.
#
# carve_chunk() logged "no more memory for a chunk" for EVERY allocation a
# full arena refused - every refused write - with the arena lock held.  On
# 223 at 12 workers that serialised the node at 47k refused writes/s, 10 us
# each (stored writes: 4M/s at 2 us), 38% of the CPU in write() and the
# futex queue behind the log.  The ceiling is already reported by
# slot_take(), rate-limited; the carve's line now is too.
#   - a 16 MB arena filled with 1 KB values until writes are refused
#   - 20,000 more writes of NEW keys on one connection: every one answered
#     `cache full`, and the node logs at most 20 chunk-refusal lines for them
#   - and it still serves a read afterwards
# Fail-first: rc50 logs one line per refused write (20,000).
# Usage: test/refusedlogtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrefusedlog.XXXXXX); P1=
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
[memory]
arena_mb = 16
[secrets]
client = refusedlog-client-secret
[listen]
resp = 127.0.0.1:17589
plaintext = loopback
[collection 0]
buckets_log2 = 12
autoscale = off
CONF
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" -D > "$D/n.log" 2>&1 & P1=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break; sleep 0.1; i=$((i+1)); done
if ! grep -q "perfcached ready" "$D/n.log"; then
	bad "the daemon did not start: $(tail -1 "$D/n.log")"
	echo "refusedlogtest: $pass passed, $fail failed"; exit 1
fi

timeout 120 python3 - > "$D/res" 2> "$D/py.err" <<'PY'
import socket
s = socket.create_connection(("127.0.0.1", 17589), timeout=30); f = s.makefile("rb")
V = b"v" * 1024
def sets(lo, n):
    s.sendall(b"".join(b"*3\r\n$3\r\nSET\r\n$10\r\nk%09d\r\n$1024\r\n%s\r\n" % (i, V) for i in range(lo, lo + n)))
    return [f.readline() for _ in range(n)]
i = 0; stored = 0
while True:                                  # fill until the first refusal
    r = sets(i, 500); i += 500
    stored += sum(1 for l in r if l == b"+OK\r\n")
    if any(l.startswith(b"-") for l in r) or i > 200000:
        break
print("FILL", stored, i)
refused = 0
for b in range(0, 20000, 500):
    refused += sum(1 for l in sets(10**8 + b, 500) if b"cache full" in l)
print("REFUSED", refused)
s.sendall(b"*2\r\n$3\r\nGET\r\n$10\r\nk000000001\r\n")
print("READ", f.readline().strip().decode(), len(f.readline()))
PY
FILL=$(sed -n 's/^FILL //p' "$D/res"); REF=$(sed -n 's/^REFUSED //p' "$D/res"); RD=$(sed -n 's/^READ //p' "$D/res")
[ -n "$FILL" ] && [ "${FILL%% *}" -gt 1000 ] && ok "the 16 MB arena took ${FILL%% *} 1 KB values, then refused" || bad "the fill: $FILL"
[ "$REF" = 20000 ] && ok "20,000 more writes of new keys, every one answered cache full" || bad "refused $REF of 20,000"
L=$(grep -c "no more memory for a chunk" "$D/n.log")
[ "$L" -le 20 ] && ok "the node logged $L chunk-refusal line(s) for them (at most 20)" \
	|| bad "the node logged $L chunk-refusal lines - one per refused write, with the arena lock held"
[ "$RD" = '$1024 1026' ] && ok "and it still serves a read" || bad "read afterwards: $RD"
[ -s "$D/py.err" ] && { bad "the python driver raised:"; tail -3 "$D/py.err"; }
kill "$P1" 2>/dev/null; wait "$P1" 2>/dev/null; P1=
echo "refusedlogtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
