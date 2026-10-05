#!/bin/sh
# walordertest.sh - S240: after a crash, every key holds the value it was
# SERVING, whatever order its records reached the WAL.
#
# A write takes its WAL sequence after its table op commits, outside the
# key's lock, and each worker has its own ring: two workers writing ONE
# key can log them in the opposite order to their versions (wal_late
# counts every out-of-order record, but cannot tell a same-key one from
# the harmless cross-ring interleaving).  Replay applied records in LOG
# order, blind to the version each carries, so an inverted pair restored
# the OLDER value - an acknowledged later write lost.
#
# Each round turns every key into a race: C connections (spread over the
# workers) write the same K keys in the same order, released together,
# so each key gets C near-simultaneous writes.  Every other round one of
# the connections DELETES every key instead, so deletes race the sets and
# a key's served state may be absent.  Then `sync`, read every
# key as served, kill -9, restart from the WAL alone (no snapshot: save =
# off), and read again.  A key whose replayed value differs from the one
# served before the kill is an inversion replayed.  A round in which the
# ring dropped anything is discarded (a drop loses records for another
# reason).  FAIL-FIRST: the build before S240 replays in log order.
# The expirer round (3, 7) is where 0.4.6's CI found an EXPIRE's WAL
# record carrying a racing SET's version, so the SET was refused on
# replay as older than what it met (5 of 20,000 on a loaded runner; the
# window is a few hundred ns and is not hit on an idle box - run the
# round repeatedly under load: test/walordertest.sh ./perfcached 3 3).
# Usage: test/walordertest.sh [./perfcached] [rounds] [first-round]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
ROUNDS=${2:-8}
BASE=/var/tmp; [ -d /dev/shm ] && [ -w /dev/shm ] && BASE=/dev/shm
D=$(mktemp -d $BASE/pcwo.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=18531
ss -ltn 2>/dev/null | grep -qE ":$PORT[[:space:]]" && { echo "walordertest: port $PORT already bound" >&2; exit 1; }
mkdir -p "$D/wal"
cat > "$D/n.conf" <<CONF
[daemon]
workers = 4
log_level = notice
[memory]
arena_mb = 256
[secrets]
client = wo-client
[listen]
tcp = 127.0.0.1:$PORT
plaintext = loopback
[collection c]
buckets_log2 = 15
[wal]
dir = $D/wal
probe = no
fsync = everysec
segment_mb = 16
segments = 8
ring_kb = 16384
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
# drv.py <port> <op> [round]: race | read
import json, pcnative, socket, sys, time
from multiprocessing import Barrier, Process
port, op = int(sys.argv[1]), sys.argv[2]
K, C = 20000, 8
# a round's KIND, by its number: 1 sets; 2 sets + a deleter; 3 sets + an
# EXPIRER (EXPIRE k 1 - lapsed by the time the node restarts: a TOUCH
# record whose expiry is in the past); 0 sets + a deleter, with LONG keys
# (600 bytes: past the replay's 512-byte tombstone key buffer)
def kind(rnd):
    return int(rnd) % 4
def key(i, rnd):
    return "k%05d" % i + ("x" * 600 if kind(rnd) == 0 else "")
def conn():
    s = socket.create_connection(("127.0.0.1", port), timeout=60); return s, pcnative.wrap(s)
def call(s, f, m, **p):
    f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": m, "params": p}) + "\n").encode()); f.flush()
    return json.loads(f.readline())
def writer(c, rnd, bar, mix):
    s, f = conn()
    call(s, f, "ping")                       # connected before the release
    bar.wait()
    # a mixed round: connection 0 DELETES (or, kind 3, EXPIREs in 1 s)
    # every key while the others set it, so each key's served state may be
    # absent - and must replay so
    dele = mix and c == 0
    def req(i):
        if dele and kind(rnd) == 3:
            return {"jsonrpc": "2.0", "id": i, "method": "expire",
                "params": {"col": "c", "key": key(i, rnd), "ttl": 1}}
        if dele:
            return {"jsonrpc": "2.0", "id": i, "method": "del",
                "params": {"col": "c", "key": key(i, rnd)}}
        return {"jsonrpc": "2.0", "id": i, "method": "set",
            "params": {"col": "c", "key": key(i, rnd), "value": "r%s:c%d:%d" % (rnd, c, i)}}
    for b in range(0, K, 64):
        f.write("".join(json.dumps(req(i)) + "\n"
            for i in range(b, min(K, b + 64))).encode()); f.flush()
        for _ in range(b, min(K, b + 64)):
            f.readline()
if op == "race":
    rnd = sys.argv[3]
    bar = Barrier(C)
    mix = kind(rnd) != 1
    ps = [Process(target=writer, args=(c, rnd, bar, mix)) for c in range(C)]
    [p.start() for p in ps]; [p.join() for p in ps]
    s, f = conn()
    print("SYNC=%s" % json.dumps(call(s, f, "sync").get("result")))
    w = call(s, f, "stats")["result"]["wal"]
    print("DROPPED=%s LATE=%s APPENDED=%s" % (w.get("dropped"), w.get("late"), w.get("appended")))
elif op == "read":
    rnd = sys.argv[4]
    s, f = conn()
    out = {}
    for b in range(0, K, 500):
        f.write("".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "get",
            "params": {"col": "c", "key": key(i, rnd)}}) + "\n" for i in range(b, min(K, b + 500))).encode()); f.flush()
        for _ in range(b, min(K, b + 500)):
            r = json.loads(f.readline()); out[r["id"]] = (r.get("result") or {}).get("value")
    json.dump(out, open(sys.argv[3], "w"))
    print("READ=%d" % sum(1 for v in out.values() if v))
PY
start || { echo "walordertest: the node did not start"; exit 1; }
tot_keys=0 tot_inv=0 used=0 late=0
r=${3:-1}
while [ $r -le $ROUNDS ]; do
	R=$(python3 "$DRV" $PORT race $r)
	dr=$(echo "$R" | sed -n 's/.*DROPPED=\([0-9]*\).*/\1/p'); lt=$(echo "$R" | sed -n 's/.*LATE=\([0-9]*\).*/\1/p')
	# kind 3: let the EXPIRE 1 lapse before the served state is read, so
	# the replay meets touch records whose expiry is already past
	[ $((r % 4)) = 3 ] && sleep 2
	python3 "$DRV" $PORT read "$D/before.json" $r >/dev/null
	kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
	start || { bad "round $r: the node did not restart"; break; }
	python3 "$DRV" $PORT read "$D/after.json" $r >/dev/null
	INV=$(python3 -c '
import json, sys
a = json.load(open(sys.argv[1])); b = json.load(open(sys.argv[2]))
print(sum(1 for k in a if a[k] != b.get(k)))' "$D/before.json" "$D/after.json")
	# what kind of wrong, when there is one: served vs replayed, per key -
	# absent->present is a delete undone, present->absent a write lost,
	# present->other an older value winning - and the node's own replay
	# summary.  rc49 GitHub check: 1 of 20,000 long keys, not reproduced
	# on 222 in 440,000; the next one has to say which it was.
	[ "${INV:-0}" != 0 ] && python3 -c '
import json, sys
a = json.load(open(sys.argv[1])); b = json.load(open(sys.argv[2]))
bad = [k for k in a if a[k] != b.get(k)]
for k in bad[:5]:
    print("     key %s: served %r, replayed %r" % (k, a[k], b.get(k)))' "$D/before.json" "$D/after.json" && \
		grep -h "recover:" "$D/n.log" | tail -2 | sed "s/^/     /"
	if [ "${dr:-1}" != 0 ]; then
		echo "   round $r: DISCARDED - the ring dropped $dr record(s)"
	else
		case $((r % 4)) in 1) kind=sets;; 2) kind="sets + a deleter";;
			3) kind="sets + an expirer (EXPIRE 1, lapsed before the restart)";;
			0) kind="sets + a deleter, 600-byte keys";; esac
		echo "   round $r ($kind): 20000 raced keys, $INV replayed in a state other than the one served (wal late so far $lt)"
		tot_keys=$((tot_keys + 20000)); tot_inv=$((tot_inv + INV)); used=$((used + 1)); late=$lt
	fi
	r=$((r + 1))
done
[ $used -gt 0 ] && ok "$used round(s) valid: $tot_keys raced keys, each written by 8 connections at once (wal late $late)" \
	|| bad "no valid round - the ring dropped in every one"
[ $used -gt 0 ] && [ $tot_inv = 0 ] \
	&& ok "every key came back from the WAL with the value it was serving before kill -9" \
	|| bad "$tot_inv of $tot_keys keys came back with an OLDER value than they served - an inverted pair replayed in log order"
echo "walordertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
