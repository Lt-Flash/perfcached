#!/bin/sh
# reconciledigtest.sh - S261: the post-restart reconcile asks only about
# the slots that differ from the witness.
#
# A node back from an outage probes every key it replayed, 64 a second,
# to find the ones the fleet deleted while it was down: 2,000 keys took
# 31 s, 1M would take 4.3 h, and every deleted key is readable on this
# node until its probe comes round.  Now the node first compares S260's
# slot digests with the witness and probes only the keys in slots that
# differ.  Three eager nodes, 2,000 keys authored by node 3 (its WAL
# replays only what it authored), everyone up past the 30 s witness
# grace, node 3 killed -9, 40 of its keys deleted through node 1, node 3
# restarted from its WAL:
#   1. the 40 are gone from node 3, the other 1,960 are all there;
#   2. it probed at most a tenth of the 2,000 and skipped the rest on
#      matching digests, and the pass finished inside 10 s;
#   3. the same with `sweep_digests = no` on node 3: every key probed
#      ONCE (S263: a probe budget that ran out mid-window made the next
#      tick re-ask the whole window - 2,752 probes for 2,000 keys), the
#      40 gone - the old path is still whole;
#   4. S262: the legacy config - [cluster] mode = eager with no
#      `collections` list - leaves the collection a plain store that is
#      never replicated.  Node 3 must keep all 2,000 of its keys and ask
#      about none: every peer's "absent" is the truth about a copy it was
#      never sent, not evidence of a delete.
# Fail-first: the build before S261 probes all 2,000 in case 1 (31 s);
# the build before S262 DELETES all 2,000 in case 4; the build before
# S263 probes 2,752 in case 3.
# Usage: test/reconciledigtest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=
trap '[ -n "$D" ] && { kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"; }' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 18621 18622 18623 18631 18632 18633; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "reconciledigtest: port $p busy" >&2; exit 1; }
done
N=2000
DEL=40

conf() { # conf <n> <cluster lines>
	mkdir -p "$D/s$1/wal"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = rd-client-secret
cluster = rd-cluster-secret
[listen]
tcp = 127.0.0.1:1862$1
http = 127.0.0.1:1863$1
plaintext = loopback
[cluster]
multicast = 239.255.78.5:18625
advertise = 127.0.23.$1
pull_timeout_ms = 400
mode = eager
$COLS
$2
[collection c]
buckets_log2 = 12
autoscale = off
[wal]
dir = $D/s$1/wal
probe = no
fsync = always
segment_mb = 8
segments = 4
save = off
C
	chmod 600 "$D/n$1.conf"
}
start() {
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 150 ]; do grep -q "perfcached ready" "$D/n$1.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; return 1
}
st() { curl -s "http://127.0.0.1:1863$1/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin); x = r["cluster"]; c = [y for y in r["collections"] if y["name"] == "c"][0]
print(eval(sys.argv[1]))' "$2" 2>/dev/null; }
x3() { st 3 "x.get('$1')"; }   # one cluster counter of node 3
ready3() {
	i=0; while [ $i -lt 120 ]; do
		[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ] && \
			[ "$(st 1 'sum(1 for p in x["peers"] if p.get("up"))')$(st 2 'sum(1 for p in x["peers"] if p.get("up"))')$(st 3 'sum(1 for p in x["peers"] if p.get("up"))')" = 222 ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}
# rpc <port> <method> <from> <to>: set or del k00000.. through one node;
# prints how many succeeded
rpc() { python3 - "$1" "$2" "$3" "$4" <<'PY'
import json, pcnative, socket, sys
port, m, a, b = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
f = pcnative.wrap(socket.create_connection(("127.0.0.1", port), timeout=30))
for i in range(a, b):
    p = {"col": "c", "key": "k%05d" % i}
    if m == "set":
        p["value"] = "v%05d" % i
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": m, "params": p}) + "\n").encode())
f.flush()
ok = 0
for i in range(a, b):
    r = json.loads(f.readline())
    ok += 1 if "result" in r and r["result"] not in (None, False) else 0
print(ok)
PY
}
# present <port>: "<of the deleted still there> <of the rest there>"
present() { python3 - "$1" "$N" "$DEL" <<'PY'
import json, pcnative, socket, sys
port, n, d = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
f = pcnative.wrap(socket.create_connection(("127.0.0.1", port), timeout=30))
for i in range(n):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": "get", "params": {"col": "c", "key": "k%05d" % i}}) + "\n").encode())
f.flush()
dl = kept = 0
for i in range(n):
    r = json.loads(f.readline())
    v = (r.get("result") or {}).get("value")
    if v == "v%05d" % r["id"]:
        if r["id"] < d: dl += 1
        else: kept += 1
print(dl, kept)
PY
}

run() { # run <label> <node 3's cluster lines> <probes: max:N | once:N> <max ms> [legacy]
	D=$(mktemp -d /var/tmp/pcrecdig.XXXXXX)
	echo "--- $1"
	LEGACY=${5:-}
	[ -n "$LEGACY" ] && COLS= || COLS="collections = c"
	conf 1 ""; conf 2 ""; conf 3 "$2"
	for n in 1 2 3; do start $n || { bad "$1: node $n did not start"; return; }; done
	ready3 || { bad "$1: the fleet did not form"; return; }
	F=$(rpc 18623 set 0 $N); [ "$F" = $N ] || bad "$1: stored $F of $N through node 3"
	i=0; while [ $i -lt 60 ]; do
		[ "$(st 1 'c["entries"]')$(st 2 'c["entries"]')" = "$N$N" ] && break
		sleep 0.5; i=$((i+1))
	done
	# the witness must hold node 3's keys, or its "absent" is the truth -
	# and in the legacy case must NOT, or the case proves nothing
	E="$(st 1 'c["entries"]') $(st 2 'c["entries"]')"
	if [ -n "$LEGACY" ]; then
		[ "$E" = "0 0" ] || { bad "$1: nodes 1 and 2 hold $E of node 3's keys - the legacy collection replicated after all"; kill -9 $(cat "$D"/*.pid) 2>/dev/null; rm -rf "$D"; D=; return; }
	else
		[ "$E" = "$N $N" ] || { bad "$1: nodes 1 and 2 hold $E of node 3's $N keys - not replicated"; kill -9 $(cat "$D"/*.pid) 2>/dev/null; rm -rf "$D"; D=; return; }
	fi
	# nodes 1 and 2 up past the 30 s witness grace before node 3 dies
	sleep 34
	kill -9 "$(cat "$D/n3.pid")"; rm -f "$D/n3.pid"; sleep 1
	X=$(rpc 18621 del 0 $DEL)
	[ -n "$LEGACY" ] || [ "$X" = $DEL ] || bad "$1: deleted $X of $DEL through node 1"
	sleep 1
	start 3 || { bad "$1: node 3 did not restart"; return; }
	# node 3's first boot recovered nothing, so this line is the restart's
	i=0; while [ $i -lt 90 ]; do
		grep -q "reconcile pass finished asking" "$D/n3.log" && break
		sleep 1; i=$((i+1))
	done
	[ $i -lt 90 ] || bad "$1: node 3's reconcile pass did not finish in 90 s"
	sleep 2                        # the last answers
	W=$(x3 reconcile_witness); Q=$(x3 reconcile_probed)
	R=$(x3 reconciled); MS=$(x3 reconcile_ms)
	G=$(st 3 "json.dumps(x.get('reconcile_digests'))")
	case "$W" in ''|0|None) bad "$1: node 3 found no witness ($W) - the pass never ran";; esac
	P=$(present 18623)
	if [ -n "$LEGACY" ]; then
		[ "$P" = "$DEL $((N - DEL))" ] \
			&& ok "$1: node 3 kept all $N of its keys (reconciled $R) - nobody was ever sent a copy" \
			|| bad "$1: node 3 holds <of the $DEL> <of the rest> = $P, want $DEL $((N - DEL)) (reconciled $R) - it deleted keys no peer ever held"
	else
		[ "$P" = "0 $((N - DEL))" ] \
			&& ok "$1: node 3 dropped all $DEL keys deleted while it was down and kept the other $((N - DEL)) (reconciled $R)" \
			|| bad "$1: node 3 holds <deleted kept> <others kept> = $P, want 0 $((N - DEL)) (reconciled $R)"
	fi
	case "$3" in
	max:*)	[ "${Q:-99999}" -le "${3#max:}" ] 2>/dev/null \
			&& ok "$1: $Q of $N keys probed (at most ${3#max:}); digests $G" \
			|| bad "$1: $Q of $N keys probed, want at most ${3#max:}; digests $G";;
	# the old path asks about every key still there, and each once.  The
	# low end is N - DEL: the peers' retained tombstones remove most of
	# the deleted keys before the walk reaches them, and a key already
	# gone is not asked about.  2% slack above.
	once:*)	M=${3#once:}
		[ "${Q:-0}" -ge $((M - DEL)) ] 2>/dev/null && [ "$Q" -le $((M + M / 50)) ] \
			&& ok "$1: $Q probes for $N keys - every key asked about once; digests $G" \
			|| bad "$1: $Q probes for $N keys, want $((M - DEL)) to $((M + M / 50)) - each key once; digests $G";;
	esac
	[ "${MS:-99999}" -le "$4" ] 2>/dev/null \
		&& ok "$1: the pass took $MS ms (at most $4)" \
		|| bad "$1: the pass took $MS ms, want at most $4"
	kill -9 $(cat "$D"/*.pid) 2>/dev/null
	rm -rf "$D"; D=
}

run "digests" "" max:$((N / 10)) 10000
run "sweep_digests = no" "sweep_digests = no" once:$N 60000
run "legacy config, collection never replicated" "" max:0 90000 legacy
echo "reconciledigtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
