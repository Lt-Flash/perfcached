#!/bin/sh
# expirerejointest.sh - RV-4d: an eager fleet's mass expiry while a node
# is down, and that node coming back.
#
# RV-4's map (DESIGN 12gb): mass expiry was never exercised in a cluster.
# Expiry is not a delete - no tombstone travels - so the only things that
# stop a returning node from bringing expired keys back are the absolute
# (wall-clock) expiry its snapshot and WAL carry, and B4's reconcile for
# what it replays that the fleet no longer holds.
#
# Three eager nodes with a WAL; half the keys written through node 1,
# half through node 3 (so node 3's WAL AUTHORS some of each kind):
#   s  3,000 keys, ttl 12 s        - expire while node 3 is down
#   i  1,000 keys, immortal        - must all survive
#   x    500 keys, ttl 12 s, then re-set ttl 600 while node 3 is down:
#                                    node 3's copy is older and dies first
#   t    500 keys, immortal, then re-set ttl 6 while node 3 is down:
#                                    node 3's copy is older and IMMORTAL
# Node 3 goes down, the re-sets land, everything short expires, node 3
# comes back, its reconcile finishes and 25 s of repair cycles follow,
# with nothing read.  Then, through every node: s 0, t 0, x 500 at the NEW value,
# i 1,000.  Twice: a clean stop (node 3 restores from its shutdown
# snapshot, replicas included) and kill -9 (its WAL only - its own
# records).  The t keys node 3 authored are the case only reconcile
# stands in front of: replayed immortal, gone everywhere else.
# A third run reads the t keys through node 1 while node 3 reconciles:
# none may be answered.  Before S303 the miss pulled and node 3 had to
# hold the pull back; since S303 node 1's eager miss is final and asks
# nobody - S245's pull guard is no longer reached from here.
#
# S245, found here: expiry leaves no tombstone, so a peer accepts ANY
# copy of an expired key.  Node 3's sweep pushed its recovered t copies
# before its reconcile had asked about them - the peers took them, then
# answered the probe "held" - and they came back immortal on every node
# (376 of 500 from the snapshot, 158 of 500 from the WAL, with a
# witness; all 500 without one).  Now a recovered record is neither
# swept nor served to a pull until reconcile has ruled on it.
# FAIL-FIRST: the build before S245 fails every "through node" check.
# Not covered, by design (S223): a fleet with no witness skips
# reconcile, and then nothing stands between a replayed key and the
# fleet - the arms wait out the witness grace first.
# Usage: test/expirerejointest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
BASE=/var/tmp
D=
trap '[ -n "$D" ] && { kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"; }' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 18681 18682 18683 18691 18692 18693; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "expirerejointest: port $p busy" >&2; exit 1; }
done

conf() { # conf <n>
	mkdir -p "$D/wal$1"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = xr-client-secret
cluster = xr-cluster-secret
[listen]
tcp = 127.0.0.1:1868$1
http = 127.0.0.1:1869$1
plaintext = loopback
[cluster]
multicast = 239.255.77.95:18695
advertise = 127.0.15.$1
pull_timeout_ms = 400
mode = eager
collections = c
[collection c]
buckets_log2 = 13
[wal]
dir = $D/wal$1
probe = no
fsync = always
segment_mb = 8
segments = 4
save = off
C
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> - each start logs to its own file, n<n>.log.<k>
	k=$(ls "$D"/n$1.log.* 2>/dev/null | wc -l); L="$D/n$1.log.$k"
	"$BIN" -f "$D/n$1.conf" > "$L" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$L" && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start: $(tail -2 "$L" | tr '\n' ' ')"
	return 1
}
st() { curl -s "http://127.0.0.1:1869$1/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin); c = [x for x in r["collections"] if x["name"] == "c"][0]
print(eval(sys.argv[1]))' "$2" 2>/dev/null; }
ready3() {
	i=0; while [ $i -lt 120 ]; do
		[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ] && \
			[ "$(st 3 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))')" = 2 ] && [ "$(st 1 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))')" = 2 ] && [ "$(st 2 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))')" = 2 ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}
# put <n> <prefix> <from> <to> <tag> <ttl>: prints how many stored
put() { python3 - "1868$1" "$2" "$3" "$4" "$5" "$6" <<'PY'
import json, pcnative, socket, sys
port, pre, a, b, tag, ttl = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5], int(sys.argv[6])
s = socket.create_connection(("127.0.0.1", port), timeout=60); f = pcnative.wrap(s); ok = 0
for x in range(a, b, 200):
    rq = []
    for i in range(x, min(b, x + 200)):
        p = {"col": "c", "key": "%s%05d" % (pre, i), "value": "%s-%s%05d" % (tag, pre, i)}
        if ttl:
            p["ttl"] = ttl
        rq.append(json.dumps({"jsonrpc": "2.0", "id": i, "method": "set", "params": p}))
    f.write(("\n".join(rq) + "\n").encode()); f.flush()
    for _ in rq:
        ok += 1 if (next(_m for _m in iter(lambda: json.loads(f.readline()), None) if "id" in _m).get("result") or {}).get("stored") else 0
print(ok)
PY
}
# reads <n> <prefix> <from> <to> [tag]: how many answer (with <tag>- when given)
reads() { python3 - "1868$1" "$2" "$3" "$4" "${5:-}" <<'PY'
import json, pcnative, socket, sys
port, pre, a, b, tag = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
s = socket.create_connection(("127.0.0.1", port), timeout=60); f = pcnative.wrap(s); hit = 0
for x in range(a, b, 200):
    rq = [json.dumps({"jsonrpc": "2.0", "id": i, "method": "get", "params": {"col": "c", "key": "%s%05d" % (pre, i)}})
          for i in range(x, min(b, x + 200))]
    f.write(("\n".join(rq) + "\n").encode()); f.flush()
    for i in range(x, min(b, x + 200)):
        v = (next(_m for _m in iter(lambda: json.loads(f.readline()), None) if "id" in _m).get("result") or {}).get("value")
        hit += 1 if v and (not tag or v.startswith(tag + "-")) else 0
print(hit)
PY
}

arm() { # arm <term|kill> [pull]
	D=$(mktemp -d $BASE/pcxr.XXXXXX)
	echo "--- arm: node 3 stopped by $1${2:+, then the t keys READ through node 1 while it reconciles}"
	for n in 1 2 3; do conf $n; done
	for n in 1 2 3; do start $n || { bad "$1: node $n did not start"; return; }; done
	ready3 || { bad "$1: the fleet did not form"; return; }
	# S223: reconcile trusts only a peer up WITNESS_GRACE (30 s) before
	# this node went down - a younger fleet skips it by design
	sleep 32
	W=$(( $(put 1 s 0 1500 v1 12) + $(put 3 s 1500 3000 v1 12) + $(put 1 i 0 500 v1 0) + $(put 3 i 500 1000 v1 0) \
		+ $(put 1 x 0 250 v1 12) + $(put 3 x 250 500 v1 12) + $(put 1 t 0 250 v1 0) + $(put 3 t 250 500 v1 0) ))
	sleep 2
	H=$(( $(reads 3 s 0 3000) + $(reads 3 i 0 1000) + $(reads 3 x 0 500) + $(reads 3 t 0 500) ))
	[ "$W" = 5000 ] && [ "$H" = 5000 ] && ok "$1: 5,000 written (half through node 3), node 3 holds all 5,000" \
		|| bad "$1: written $W, node 3 holds $H of 5,000"
	if [ "$1" = term ]; then
		kill "$(cat "$D/n3.pid")"
	else
		kill -9 "$(cat "$D/n3.pid")"
	fi
	i=0; while kill -0 "$(cat "$D/n3.pid")" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done
	R=$(( $(put 1 x 0 500 v2 600) + $(put 1 t 0 500 v2 6) ))
	sleep 15
	S1=$(reads 1 s 0 3000); T1=$(reads 1 t 0 500); X1=$(reads 1 x 0 500 v2); I1=$(reads 1 i 0 1000)
	[ "$R" = 1000 ] && [ "$S1$T1" = 00 ] && [ "$X1" = 500 ] && [ "$I1" = 1000 ] \
		&& ok "$1: with node 3 down - x re-set to 600 s and t to 6 s; 15 s on node 1 holds s 0, t 0, x 500 new, i 1,000" \
		|| bad "$1: while down: re-set $R of 1,000; node 1 s $S1 t $T1 x $X1 i $I1 (want 0 0 500 1000)"
	start 3 || { bad "$1: node 3 did not come back"; return; }
	ready3 || bad "$1: the fleet did not re-form"
	if [ -n "${2:-}" ]; then
		# node 3 holds every t key, immortal, until its reconcile has
		# asked about it.  Before S303 a miss on node 1 pulled and node
		# 3 had to hold the pull back (S245); since S303 a READY eager
		# node's miss is final and asks nobody.  Either way none of the
		# 500 may be answered.
		PS0=$(st 1 'r["cluster"].get("pull_sent", 0)')
		P1=$(reads 1 t 0 500); PH=$(st 3 'r["cluster"].get("pull_held_recovered", 0)')
		PS1=$(st 1 'r["cluster"].get("pull_sent", 0)')
		[ "$P1" = 0 ] && { [ "${PH:-0}" -gt 0 ] || [ "$PS1" = "$PS0" ]; } 2>/dev/null \
			&& ok "$1: t read through node 1 during node 3's reconcile: 0 of 500 answered (node 1 pulls $PS0 -> $PS1, $PH held back by node 3)" \
			|| bad "$1: t read through node 1 during node 3's reconcile answered $P1 of 500 (node 1 pulls $PS0 -> $PS1, node 3 held back $PH)"
	fi
	# NOTHING else is read from here to the checks: reconcile asks at 64 keys
	# a tick, and what matters is the sweep AFTER it - held records are
	# released then, and a leak would land on the peers
	i=0; while [ $i -lt 240 ]; do
		grep -q "reconcile pass finished asking" "$D/n3.log.1" && [ "$(st 3 'r["cluster"]["reconcile_pending"]')" = 0 ] && break
		grep -q "reconcile SKIPPED" "$D/n3.log.1" && break
		sleep 0.5; i=$((i+1))
	done
	grep -q "reconcile pass finished asking" "$D/n3.log.1" \
		&& ok "$1: node 3 reconciled against a witness: $(grep -m1 'reconcile pass finished' "$D/n3.log.1" | sed 's/.*reconcile pass finished asking - //' | cut -c1-40)" \
		|| bad "$1: node 3's reconcile did not finish in 120 s: $(grep -m1 -E 'reconcile (SKIPPED|-)' "$D/n3.log.1" | cut -c1-120)"
	sleep 25                                   # two repair cycles after it
	E="$(st 1 'c["entries"]')/$(st 2 'c["entries"]')/$(st 3 'c["entries"]')"
	HR=$(st 3 'r["cluster"].get("repl_held_recovered", 0)')
	RC="probed $(st 3 'r["cluster"].get("reconcile_probed")'), dropped $(st 3 'r["cluster"].get("reconciled")'); the sweep held back $HR, pulls unanswered $(st 3 'r["cluster"].get("pull_held_recovered")')"
	echo "   reconcile done + 25 s: entries $E (1,500 live); node 3: $RC"
	# a leak's shape, printed only when the counts are off: which node
	# holds which t key, by LOCAL ttl (the ttl verb never pulls)
	[ "$E" = 1500/1500/1500 ] || python3 - <<'DPY'
import json, pcnative, socket
socks = {n: socket.create_connection(("127.0.0.1", 18680 + n), timeout=20) for n in (1, 2, 3)}
fs = {n: pcnative.wrap(socks[n]) for n in socks}
def ttl(n, key):
    fs[n].write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ttl", "params": {"col": "c", "key": key}}) + "\n").encode()); fs[n].flush()
    return (json.loads(fs[n].readline()).get("result") or {}).get("ttl")
shown = 0
for i in range(500):
    t = {n: ttl(n, "t%05d" % i) for n in (1, 2, 3)}
    if any(v != -2 for v in t.values()) and shown < 8:
        print("   diag t%05d local ttl: n1 %s n2 %s n3 %s" % (i, t[1], t[2], t[3])); shown += 1
DPY
	for n in 3 1 2; do
		S=$(reads $n s 0 3000); T=$(reads $n t 0 500); X=$(reads $n x 0 500 v2); XO=$(reads $n x 0 500 v1); I=$(reads $n i 0 1000)
		[ "$S$T" = 00 ] && [ "$X" = 500 ] && [ "$XO" = 0 ] && [ "$I" = 1000 ] \
			&& ok "$1: through node $n - s 0 of 3,000, t 0 of 500, x 500 at the new value, i 1,000" \
			|| bad "$1: through node $n - s $S (0), t $T (0), x new $X old $XO (500/0), i $I (1000)"
	done
	kill -9 $(cat "$D"/*.pid) 2>/dev/null
	cat "$D"/n3.log.1 > "$BASE/pcxr.$1.n3.log" 2>/dev/null
	rm -rf "$D"; D=
}

arm term
arm kill
arm term pull
echo "expirerejointest: $pass passed, $fail failed"
[ $fail -eq 0 ]
