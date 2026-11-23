#!/bin/sh
# expirerejointest.sh - mass expiry while a node is down: on its return no expired key comes back
# on any node, since a recovered record is neither swept nor served until reconcile rules on it.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
# the one-by-one reconcile a restart or a floor ran before 12ol is this suite's subject; the default
# now copies a survivor's state whole (rejoinwitnesstest)
export PERFCACHED_TEST_RECONCILE=1
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
# rtn <n>: what node n retains of deletes and expiries, and its arena's sparse chunks and moves
rtn() { curl -s "http://127.0.0.1:1869$1/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin); t = r["cluster"].get("tombstones_retained", {}); f = {}
def walk(x):
    for k, v in (x.items() if isinstance(x, dict) else []):
        if k in ("arena_sparse_chunks", "arena_evacuated"): f[k] = v
        walk(v)
walk(r)
print("node %s: %s; %s" % (r["cluster"].get("node"), {k: t.get(k) for k in ("entries", "expiries", "noted_expiries", "forgotten_expiries", "dropped_early_expiries", "replayed")}, f))' 2>/dev/null; }
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

down() { # an arm that gave up early left its nodes up, holding the next arm's ports
	[ -n "$D" ] || return 0
	P=$(cat "$D"/*.pid 2>/dev/null); kill -9 $P 2>/dev/null
	for p in $P; do wait "$p" 2>/dev/null; done
	rm -rf "$D"; D=
}
arm() { # arm <term|kill> [pull]
	down
	D=$(mktemp -d $BASE/pcxr.XXXXXX)
	echo "--- arm: node 3 stopped by $1${2:+, then the t keys READ through node 1 while it reconciles}"
	for n in 1 2 3; do conf $n; done
	for n in 1 2 3; do start $n || { bad "$1: node $n did not start"; return; }; done
	ready3 || { bad "$1: the fleet did not form"; return; }
	# reconcile trusts only a peer up WITNESS_GRACE (30 s) before this node went down, and a witness
	# vouches only for a window past its own grace (the stop snapshot less 10 s) - so 45, not 32
	sleep 45
	W=$(( $(put 1 s 0 1500 v1 12) + $(put 3 s 1500 3000 v1 12) + $(put 1 i 0 500 v1 0) + $(put 3 i 500 1000 v1 0) \
		+ $(put 1 x 0 250 v1 12) + $(put 3 x 250 500 v1 12) + $(put 1 t 0 250 v1 0) + $(put 3 t 250 500 v1 0) ))
	sleep 2
	H=$(( $(reads 3 s 0 3000) + $(reads 3 i 0 1000) + $(reads 3 x 0 500) + $(reads 3 t 0 500) ))
	[ "$W" = 5000 ] && [ "$H" = 5000 ] && ok "$1: 5,000 written (half through node 3), node 3 holds all 5,000" \
		|| bad "$1: written $W, node 3 holds $H of 5,000"
	if [ "$1" = term ]; then
		kill "$(cat "$D/n3.pid")"
	else
		# an ack is at the WAL's ring (src/wal.h) and kill -9 takes what a ring holds - a stalled
		# fsync holds the pump: node 3's log must hold its writes first, or nothing is replayed
		i=0; while [ "$(st 3 'r["wal"]["staged"]')" != 0 ] && [ $i -lt 120 ]; do sleep 0.5; i=$((i+1)); done
		SG=$(st 3 'r["wal"]["staged"]')
		[ "$SG" = 0 ] || bad "$1: node 3's WAL still stages $SG record(s) after 60 s - a kill -9 now replays without them"
		kill -9 "$(cat "$D/n3.pid")"
	fi
	i=0; while kill -0 "$(cat "$D/n3.pid")" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done
	R=$(( $(put 1 x 0 500 v2 600) + $(put 1 t 0 500 v2 6) ))
	sleep 15
	S1=$(reads 1 s 0 3000); T1=$(reads 1 t 0 500); X1=$(reads 1 x 0 500 v2); I1=$(reads 1 i 0 1000)
	[ "$R" = 1000 ] && [ "$S1$T1" = 00 ] && [ "$X1" = 500 ] && [ "$I1" = 1000 ] \
		&& ok "$1: with node 3 down - x re-set to 600 s and t to 6 s; 15 s on node 1 holds s 0, t 0, x 500 new, i 1,000" \
		|| bad "$1: while down: re-set $R of 1,000; node 1 s $S1 t $T1 x $X1 i $I1 (want 0 0 500 1000)"
	RT0_1=$(rtn 1); RT0_2=$(rtn 2)
	start 3 || { bad "$1: node 3 did not come back"; return; }
	ready3 || bad "$1: the fleet did not re-form"
	if [ -n "${2:-}" ]; then
		# node 3 holds every t key, immortal, until its reconcile has asked about it; a READY eager
		# node's miss is final, so none of the 500 may be answered through node 1
		PS0=$(st 1 'r["cluster"].get("pull_sent", 0)')
		P1=$(reads 1 t 0 500); PH=$(st 3 'r["cluster"].get("pull_held_recovered", 0)')
		PS1=$(st 1 'r["cluster"].get("pull_sent", 0)')
		# after a clean stop the fast path, or the budgeted walk, can end node 3's pass before these
		# reads: nothing left to hold back, none answered, node 3 serving none
		O3B=$(st 3 'r["cluster"].get("reconcile_open")')    # BEFORE node 3's reads
		P3=$(reads 3 t 0 500); F3=$(st 3 'r["cluster"].get("reconcile_fast")')
		O3=$(st 3 'r["cluster"].get("reconcile_open")')
		# ... or node 1 pulled while the pass was open; what must hold is none answered through node 1,
		# and node 3's own reads count only when its pass was shut before them
		[ "$P1" = 0 ] && { [ "$O3B" = True ] || [ "$P3" = 0 ]; } \
			&& ok "$1: t read through node 1 during node 3's reconcile: 0 of 500 answered (node 1 pulls $PS0 -> $PS1, $PH held back by node 3; node 3 fast path $F3, pass open $O3, serves $P3)" \
			|| bad "$1: t read through node 1 during node 3's reconcile answered $P1 of 500 (node 1 pulls $PS0 -> $PS1, node 3 held back $PH, fast path $F3, pass open $O3B before node 3's reads and $O3 after, serves $P3)"
	fi
	# NOTHING else is read until the checks: the sweep after the pass releases held records and a
	# leak would land on the peers; the pass ends by the fast path (clean stop) or the walk (kill -9)
	FIN="reconcile pass finished asking|reconcile finished by the fast path"
	i=0; while [ $i -lt 240 ]; do
		grep -q -E "$FIN" "$D/n3.log.1" && [ "$(st 3 'r["cluster"]["reconcile_pending"]')" = 0 ] && break
		grep -q "reconcile SKIPPED" "$D/n3.log.1" && break
		sleep 0.5; i=$((i+1))
	done
	grep -q -E "$FIN" "$D/n3.log.1" \
		&& ok "$1: node 3 reconciled against a witness: $(grep -m1 -E "$FIN" "$D/n3.log.1" | sed -E 's/.*cluster: reconcile (pass )?finished//' | cut -c1-60)" \
		|| bad "$1: node 3's reconcile did not finish in 120 s: $(grep -m1 -E 'reconcile (SKIPPED|-)' "$D/n3.log.1" | cut -c1-120)"
	FP=$(st 3 'r["cluster"].get("reconcile_fast")')
	if [ "$1" = term ]; then
		[ "$FP" = True ] \
			&& ok "$1: S392 - the fast path ended it: the witness replayed $(st 3 'r["cluster"].get("reconcile_fast_deletes")') delete(s) and expiries since node 3's stop snapshot, nothing walked (probed $(st 3 'r["cluster"].get("reconcile_probed")'))" \
			|| bad "$1: S392 - a clean stop with a witness up should end by the fast path: reconcile_fast $FP, why '$(st 3 'r["cluster"].get("reconcile_fast_why")')'"
	else
		[ "$FP" = False ] && st 3 'r["cluster"].get("reconcile_fast_why")' | grep -q "no snapshot" \
			&& ok "$1: S392 - no snapshot after kill -9 with save = off, so it walked" \
			|| bad "$1: S392 - kill -9 without a snapshot must walk: reconcile_fast $FP, why '$(st 3 'r["cluster"].get("reconcile_fast_why")')'"
	fi
	sleep 25                                   # two repair cycles after it
	E="$(st 1 'c["entries"]')/$(st 2 'c["entries"]')/$(st 3 'c["entries"]')"
	HR=$(st 3 'r["cluster"].get("repl_held_recovered", 0)')
	RC="probed $(st 3 'r["cluster"].get("reconcile_probed")'), dropped $(st 3 'r["cluster"].get("reconciled")'); the sweep held back $HR, pulls unanswered $(st 3 'r["cluster"].get("pull_held_recovered")')"
	echo "   reconcile done + 25 s: entries $E (1,500 live); node 3: $RC"
	echo "   retained before node 3's return - $RT0_1 | $RT0_2"
	echo "   retained now - $(rtn 1) | $(rtn 2)"
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
	cat "$D"/n3.log.1 > "$BASE/pcxr.$1.n3.log" 2>/dev/null
	down
}

arm term
arm kill
arm term pull
echo "expirerejointest: $pass passed, $fail failed"
[ $fail -eq 0 ]
