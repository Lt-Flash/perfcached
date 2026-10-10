#!/bin/sh
# expirereaptest.sh - S333: a key that EXPIRED and was REAPED, then written
# again on another thread, replays with the value it was serving.
#
# The standalone build's version clock is per thread (S252): a write
# floors its version on the key's record under the bucket lock, and a
# DELETE publishes its version to the shared clock, because afterwards
# there is no record to floor on.  The expiry sweep removed records too,
# and published nothing: an EXPIRE's touch stamped by a busy thread,
# lapsed and reaped, then a SET of the key on a quieter thread stamped
# BELOW it - and the WAL replay, by version (S240), put the touch's past
# expiry back over the SET.  An acknowledged write gone after a restart:
# walordertest's expirer round on Alpine, v0.5.6 GitHub, 192 of 20,000.
#
# Made certain, not timed: ONE connection (one worker) writes 2,000 junk
# keys before each key's SET + EXPIRE 1, so only its thread's clock
# climbs; the sweep reaps them; then each key is written once from a NEW
# connection - about half land on the other worker, whose clock is near
# zero.  sync, kill -9, restart from the WAL alone, read.  Before S333
# about half the keys come back absent (none lost: 2^-40); the clustered
# edition's clock is one shared counter, so it passes either way.
# Usage: test/expirereaptest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
BASE=/var/tmp; [ -d /dev/shm ] && [ -w /dev/shm ] && BASE=/dev/shm
D=$(mktemp -d $BASE/pcexr.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=18541
ss -ltn 2>/dev/null | grep -qE ":$PORT[[:space:]]" && { echo "expirereaptest: port $PORT already bound" >&2; exit 1; }
mkdir -p "$D/wal"
cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = exr-client
[listen]
tcp = 127.0.0.1:$PORT
plaintext = loopback
[collection c]
# sized for the 80,000 junk keys, and no autoscale: a resize copy would
# drop the lapsed keys itself (they do not travel) and count them on the
# outgoing table - the sweep has to be what reaps them here
buckets_log2 = 15
autoscale = off
[wal]
dir = $D/wal
probe = no
fsync = everysec
segment_mb = 8
segments = 4
save = off
CONF
chmod 600 "$D/n.conf"
start() {
	: > "$D/n.log"
	"$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
	PID=$!
	i=0; while [ $i -lt 300 ]; do grep -q "perfcached ready" "$D/n.log" && return 0; kill -0 $PID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
	echo "did not start: $(tail -3 "$D/n.log" | tr '\n' ' ')"; return 1
}
DRV="$D/drv.py"
cat > "$DRV" <<'PY'
# drv.py <port> touch | expired | rewrite | read <file>
import json, pcnative, socket, sys
port, op = int(sys.argv[1]), sys.argv[2]
K, JUNK = 40, 2000
def conn():
    return pcnative.wrap(socket.create_connection(("127.0.0.1", port), timeout=60))
def batch(f, reqs):
    f.write("".join(json.dumps(dict(r, jsonrpc="2.0", id=n)) + "\n" for n, r in enumerate(reqs)).encode())
    f.flush()
    return [json.loads(f.readline()) for _ in reqs]
def call(f, m, **p):
    return batch(f, [{"method": m, "params": p}])[0]
if op == "touch":
    # one connection, one worker: its clock climbs by JUNK before each
    # key's SET and EXPIRE, the other worker's not at all
    f = conn()
    for i in range(K):
        batch(f, [{"method": "set", "params": {"col": "c", "key": "junk%02d-%04d" % (i, j), "value": "j"}}
                  for j in range(JUNK)])
        r = batch(f, [{"method": "set", "params": {"col": "c", "key": "k%02d" % i, "value": "v1"}},
                      {"method": "expire", "params": {"col": "c", "key": "k%02d" % i, "ttl": 1}}])
        assert (r[1].get("result") or {}), r
    print("TOUCHED=%d" % K)
elif op == "expired":
    f = conn()
    st = call(f, "stats")["result"]
    print([c for c in st["collections"] if c["name"] == "c"][0].get("expired", 0))
elif op == "rewrite":
    # each key once, each from a NEW connection: the kernel spreads them
    # over both workers
    for i in range(K):
        f = conn()
        call(f, "set", col="c", key="k%02d" % i, value="v2")
    f = conn()
    print("SYNC=%s" % json.dumps(call(f, "sync").get("result")))
elif op == "read":
    f = conn()
    out = {}
    for i in range(K):
        out["k%02d" % i] = (call(f, "get", col="c", key="k%02d" % i).get("result") or {}).get("value")
    json.dump(out, open(sys.argv[3], "w"))
    print("PRESENT=%d" % sum(1 for v in out.values() if v))
PY
start || { echo "expirereaptest: the node did not start"; exit 1; }
python3 "$DRV" $PORT touch > /dev/null
# EXPIRE 1 lapses within two seconds; the sweep runs once a second
k=0; X=0; while [ $k -lt 40 ]; do X=$(python3 "$DRV" $PORT expired); [ "${X:-0}" -ge 40 ] && break; sleep 0.25; k=$((k+1)); done
[ "${X:-0}" -ge 40 ] && ok "40 keys expired and were reaped by the sweep ($X)" \
	|| bad "the sweep reaped $X of 40 - the case under test never happened"
python3 "$DRV" $PORT rewrite > /dev/null
python3 "$DRV" $PORT read "$D/before.json" > "$D/b.out"
grep -q "PRESENT=40" "$D/b.out" && ok "each written again from a new connection: all 40 served v2" \
	|| bad "served before the kill: $(cat "$D/b.out")"
kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
start || { bad "the node did not restart"; echo "expirereaptest: $pass passed, $fail failed"; exit 1; }
python3 "$DRV" $PORT read "$D/after.json" > "$D/a.out"
L=$(python3 -c '
import json, sys
a = json.load(open(sys.argv[1])); b = json.load(open(sys.argv[2]))
print(sum(1 for k in a if a[k] != b.get(k)))' "$D/before.json" "$D/after.json")
if [ "$L" = 0 ]; then
	ok "after kill -9 and a replay from the WAL every key has the value it served"
else
	bad "$L of 40 keys came back without the value they served ($(cat "$D/a.out")) - $(grep -h 'recover:' "$D/n.log" | tail -1 | sed 's/.*wal replay/wal replay/' | cut -c1-110)"
fi
kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
echo "expirereaptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
