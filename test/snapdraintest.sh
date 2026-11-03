#!/bin/sh
# snapdraintest.sh - S453: a snapshot taken while a resize swaps and drains
# holds every record.
#
# THE BUG.  The snapshot takes its WAL marker when its walk starts and walks
# the collection's live table only.  A resize that swaps during that walk
# leaves the walk on the old table, and the drain's end empties it - every
# record the walk had not reached yet is in neither the snapshot nor (below
# the marker) the WAL replay: a restart loses it, acknowledged.  A snapshot
# that starts while a swapped resize still drains misses what the drain has
# not carried.  walordertest's round 8 lost one key this way (1 of 160,000).
#
# A fresh node per arm, 30,000 records of ~330 bytes, the snapshot writer
# held to 1 MB/s (a walk of ~10 s); a resize starts 0.3 s into the walk:
#   1. grow 2^13 -> 2^15, then kill -9 and restart: all 30,000 back, each right;
#   2. shrink 2^15 -> 2^13 the same way.
# Each arm also reads the premise: the walk still ran when the resize was
# asked for.  Fail-first (GitLab 112549, without the fix): the snapshots held
# 8,398 and 7,302 records, and 21,603 and 22,698 were gone after the restart.
# Usage: test/snapdraintest.sh [./perfcached] [./perfcli]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
for b in "$BIN" "$CLI"; do
	[ -x "$b" ] || { echo "snapdraintest: $b not built - SKIPPED, and a skip is not a pass"; exit 0; }
done
D=$(mktemp -d /var/tmp/pcsnapdr.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=18611
N=30000
ss -ltn 2>/dev/null | grep -qE ":$PORT[[:space:]]" && { echo "snapdraintest: port $PORT already bound" >&2; exit 1; }

conf() { # conf <buckets_log2>
	cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
log_level = notice
allow_create = yes
state_dir = $D/s
grow_at_pct = 100
[memory]
arena_mb = 128
[secrets]
client = sd-client
enable = sd-enable
[listen]
tcp = 127.0.0.1:$PORT
plaintext = loopback
[collection c]
buckets_log2 = $1
autoscale = off
[wal]
dir = $D/wal
probe = no
fsync = everysec
save = off
rdb_mb_s = 1
CONF
	chmod 600 "$D/n.conf"
}
start() {
	"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
	PID=$!
	i=0
	while [ $i -lt 300 ]; do grep -q "perfcached ready" "$D/n.log" && return 0; sleep 0.1; i=$((i+1)); done
	return 1
}
drive() { timeout 120 python3 - "$PORT" "$N" "$@" <<'PY'
import json, pcnative, socket, sys
port, n, op = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
s = socket.create_connection(("127.0.0.1", port), timeout=60)
f = pcnative.wrap(s)
def val(i): return "v%05d-" % i + "x" * 320
def batch(reqs):
    for b in range(0, len(reqs), 256):
        part = reqs[b:b + 256]
        f.write("".join(json.dumps(r) + "\n" for r in part).encode()); f.flush()
        for _ in part:
            yield json.loads(f.readline())
def one(m, **p):
    return next(batch([{"jsonrpc": "2.0", "id": 1, "method": m, "params": p}]))
if op == "fill":
    bad = sum(1 for r in batch([{"jsonrpc": "2.0", "id": i, "method": "set",
        "params": {"col": "c", "key": "k%05d" % i, "value": val(i)}} for i in range(n)]) if "error" in r)
    print("stored %d" % (n - bad))
elif op == "check":
    miss = wrong = 0; first = ""
    for r in batch([{"jsonrpc": "2.0", "id": i, "method": "get",
            "params": {"col": "c", "key": "k%05d" % i}} for i in range(n)]):
        i = r.get("id"); v = (r.get("result") or {}).get("value")
        if v is None: miss += 1
        elif v != val(i): wrong += 1
        if (v is None or v != val(i)) and not first: first = "k%05d -> %r" % (i, (v or "")[:12])
    print("missing %d wrong %d%s" % (miss, wrong, (" first " + first) if first else ""))
elif op == "sync":
    print(json.dumps(one("sync", timeout_ms=10000).get("result")))
elif op == "save":
    print(json.dumps(one("save").get("result")))
PY
}
after() { tail -n +$((BASE + 1)) "$D/n.log"; }   # the log since the save
waitlog() { # waitlog <ERE> <seconds>
	i=0; while [ $i -lt $(($2 * 10)) ]; do after | grep -qE "$1" && return 0; sleep 0.1; i=$((i+1)); done; return 1
}

arm() { # arm <name> <from log2> <to log2>
	[ -n "$PID" ] && { kill -9 $PID; wait $PID 2>/dev/null; PID=; }
	rm -rf "$D/s" "$D/wal"; mkdir -p "$D/s" "$D/wal"
	conf "$2"
	start || { bad "$1: the node did not start: $(tail -2 "$D/n.log")"; return; }
	R=$(drive fill)
	[ "$R" = "stored $N" ] || { bad "$1: fill: $R"; return; }
	drive sync > /dev/null
	BASE=$(wc -l < "$D/n.log")
	S=$(drive save)
	sleep 0.3
	"$CLI" -q -h 127.0.0.1 -p $PORT -E sd-enable resize c "$3" > "$D/rs.out" 2>&1
	grep -q '"resizing"' "$D/rs.out" || { bad "$1: resize to 2^$3 refused: $(head -1 "$D/rs.out")"; return; }
	waitlog "rdb: snapshot [0-9]+ records" 90 || { bad "$1: no snapshot within 90 s (save: $S)"; return; }
	waitlog "resize complete at 2\\^$3 " 90 || { bad "$1: the resize to 2^$3 did not complete"; return; }
	snap=$(after | grep -oE "rdb: snapshot [0-9]+ records, [0-9]+ bytes, [0-9]+ ms" | tail -1)
	recs=$(echo "$snap" | awk '{print $3}'); ms=$(echo "$snap" | awk '{print $7}')
	# the premise: the walk still ran when the resize was asked for, 0.3 s in
	[ "${ms:-0}" -ge 1000 ] 2>/dev/null \
		|| bad "$1: the snapshot walked ${ms:-?} ms - over before the resize was asked for ($snap)"
	[ "$recs" = "$N" ] && ok "$1: the snapshot holds all $N records ($snap)" \
		|| bad "$1: the snapshot holds $recs of $N records ($snap)"
	drive sync > /dev/null
	kill -9 $PID; wait $PID 2>/dev/null; PID=
	start || { bad "$1: the node did not restart: $(tail -2 "$D/n.log")"; return; }
	R=$(drive check)
	[ "$R" = "missing 0 wrong 0" ] && ok "$1: after kill -9 and a restart all $N records are back, each right" \
		|| bad "$1: after kill -9 and a restart: $R; $(grep -oE 'rdb loaded[^)]*\)|wal replay [^(]*' "$D/n.log" | tr '\n' ' ')"
}
echo "--- 1. grow 2^13 -> 2^15 inside the snapshot's walk"
arm "1. grow" 13 15
echo "--- 2. shrink 2^15 -> 2^13 inside the snapshot's walk"
arm "2. shrink" 15 13

echo "snapdraintest: $pass passed, $fail failed"
[ $fail -eq 0 ]
