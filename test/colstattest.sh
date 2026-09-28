#!/bin/sh
# colstattest.sh - S237: a collection's write-stream counters count every
# write, from every worker.
#
# stored_bytes / stored_n / size_hist (S67, the page's "are we storing
# what we think we are") were plain `+=` in the collection's registry
# entry - so writes on different workers lost counts to each other, and
# every SET dirtied the lines every request's collection lookup reads
# (23% of a saturated SET at 12 workers, DESIGN 12ga).  They are now
# per-thread stripes, summed on read.
#
# Eight connections spread over four workers write 30,000 values each,
# in three sizes, at once; the counters must say exactly 240,000 writes,
# exactly the bytes sent, a histogram that sums to the count - and a
# reset must zero all of it.  FAIL-FIRST: the build before S237 loses
# counts under the same storm.
# Usage: test/colstattest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
BASE=/var/tmp; [ -d /dev/shm ] && [ -w /dev/shm ] && BASE=/dev/shm
D=$(mktemp -d $BASE/pccs.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=18511
if ss -ltn 2>/dev/null | grep -qE ":$PORT[[:space:]]"; then
	echo "colstattest: port $PORT already bound" >&2; exit 1
fi
cat > "$D/n.conf" <<CONF
[daemon]
workers = 4
log_level = notice
[memory]
arena_mb = 256
[secrets]
client = cs-client
[listen]
tcp = 127.0.0.1:$PORT
plaintext = loopback
[collection c]
buckets_log2 = 16
CONF
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
PID=$!
i=0; while [ $i -lt 300 ]; do grep -q "perfcached ready" "$D/n.log" && break; sleep 0.1; i=$((i+1)); done
R=$(python3 - "$PORT" <<'PY'
import json, socket, sys
from multiprocessing import Process
port = int(sys.argv[1])
CONNS, PER = 8, 30000
SIZES = (10, 300, 5000)                  # three size_hist classes
def conn():
    s = socket.create_connection(("127.0.0.1", port), timeout=60)
    return s, s.makefile("rb")
def call(s, f, m, **p):
    s.sendall((json.dumps({"jsonrpc": "2.0", "id": 1, "method": m, "params": p}) + "\n").encode())
    return json.loads(f.readline())
def writer(k):
    s, f = conn()
    for b in range(0, PER, 100):
        s.sendall("".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "set",
            "params": {"col": "c", "key": "w%d:%d" % (k, i % 2000),
                       "value": "v" * SIZES[i % 3]}}) + "\n" for i in range(b, b + 100)).encode())
        for _ in range(100):
            f.readline()
ps = [Process(target=writer, args=(k,)) for k in range(CONNS)]
[p.start() for p in ps]; [p.join() for p in ps]
want_n = CONNS * PER
want_b = sum(len("w%d:%d" % (k, i % 2000)) + SIZES[i % 3]
             for k in range(CONNS) for i in range(PER))
s, f = conn()
def col():
    st = call(s, f, "stats")["result"]
    cs = st.get("collections") or []
    return next((c for c in cs if c.get("name") == "c"), {})
c = col()
print("WANT_N=%d WANT_B=%d" % (want_n, want_b))
print("N=%s B=%s H=%s" % (c.get("stored_n"), c.get("stored_bytes"), sum(c.get("size_hist") or [-1])))
print("CLASSES=%d" % sum(1 for x in (c.get("size_hist") or []) if x))
call(s, f, "reset_stats")
c = col()
print("AFTER_N=%s AFTER_B=%s AFTER_H=%s" % (c.get("stored_n"), c.get("stored_bytes"), sum(c.get("size_hist") or [-1])))
PY
)
v() { echo "$R" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
[ "$(v N)" = "$(v WANT_N)" ] && [ "$(v B)" = "$(v WANT_B)" ] \
	&& ok "8 connections x 30,000 writes on 4 workers: stored_n $(v N) and stored_bytes $(v B), exactly what was sent" \
	|| bad "counts lost under concurrent writes: stored_n $(v N) of $(v WANT_N), stored_bytes $(v B) of $(v WANT_B)"
[ "$(v H)" = "$(v N)" ] && [ "$(v CLASSES)" = 3 ] \
	&& ok "the size histogram sums to the count, in the 3 classes written" \
	|| bad "size_hist sums to $(v H) against stored_n $(v N), in $(v CLASSES) classes"
[ "$(v AFTER_N)" = 0 ] && [ "$(v AFTER_B)" = 0 ] && [ "$(v AFTER_H)" = 0 ] \
	&& ok "reset_stats zeroes all of it" \
	|| bad "after reset_stats: stored_n $(v AFTER_N), stored_bytes $(v AFTER_B), histogram $(v AFTER_H)"
echo "colstattest: $pass passed, $fail failed"
[ $fail -eq 0 ]
