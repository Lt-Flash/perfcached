#!/bin/sh
# reconcilefasttest.sh - S392: the reconcile's fast path.  A node back from
# a restart asks a witness for every delete - and every expiry - planted
# since its snapshot began, instead of asking the fleet about each key it
# replayed; when the witness vouches for the whole window and the whole
# replay arrives, the pass is over.
#
# WHY IT CAN BE ENOUGH.  An eager replica persists no copy and no delete
# it receives: after a restart it holds its last snapshot plus its own
# writes since.  So what can come back that the fleet no longer holds is
# (a) a key deleted after that snapshot began - while the node was up but
# had not persisted the delete, or while it was down - and (b) a key the
# fleet re-wrote with a shorter TTL that then expired, whose older copy
# the snapshot holds.  Each live peer planted (a) in its retained table
# when the delete was made (S238) and, since S392, (b) when its sweep
# reaped the record.  Both are versioned, so the replay drops only what
# they superseded.
#
# Three eager nodes, `save = 900 1` (no snapshot but the one asked for),
# every node up past the 30 s witness grace.  Through node 2: 300 tk keys,
# 100 immortal tx keys; node 1 SNAPSHOTS; then, node 1 still up, tk 1-100
# are deleted and tx 1-100 re-set with a 3 s TTL; node 1 is killed -9;
# tk 101-200 are deleted while it is down and the tx keys expire on the
# peers; node 1 comes back from its snapshot.
#   A  defaults: the fast path ends the pass (reconcile_fast true, nothing
#      probed, a few seconds): node 1 holds tk 201-300 only, no tx - and
#      after a repair cycle node 2 holds the same (nothing spread back)
#   B  tombstone_retain_s = 5: the window is older than the witnesses'
#      table - it says so, and the WALK runs and drops the same keys
#   C  tombstone_retain_max = 1000 and 1,200 more deletes after the
#      snapshot: the cap drops some inside the window - it says so, and
#      the walk runs
# FAIL-FIRST: before S392 arm A has no reconcile_fast (the stat reads
# absent) and walks; without the expiry planting the tx copies come back
# immortal through the fast path - arm A's tx count is that check.
# Usage: test/reconcilefasttest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
SEC=rf-client-secret
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
note(){ echo "       $1"; }

command -v ss >/dev/null 2>&1 && for pf in 17421 17422 17423; do
	ss -ltn 2>/dev/null | grep -q ":$pf[[:space:]]" && {
		echo "reconcilefasttest: port $pf already bound" >&2; exit 1; }
done

D=$(mktemp -d /var/tmp/pcrf.XXXXXX)
# daemons die by the pids this script started, each checked against its
# command line, so a reused pid never hits a stranger
PIDS=
kill_ours() {
	for p in $PIDS; do
		grep -q "$D/n" "/proc/$p/cmdline" 2>/dev/null && kill -9 "$p" 2>/dev/null
	done
}
trap 'kill_ours; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM

# ---- fixture --------------------------------------------------------
mk() { # mk <node> <retain_s> <retain_max>
	mkdir -p "$D/w$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = rf-cluster-secret
[listen]
tcp = 127.0.72.$1:1742$1
[cluster]
multicast = 239.255.79.2:17429
advertise = 127.0.72.$1
pull_timeout_ms = 400
tombstone_retain_s = $2
tombstone_retain_max = $3
mode = eager
collections = c
[wal]
dir = $D/w$1
segment_mb = 8
probe = no
fsync = always
save = 900 1
[collection c]
buckets_log2 = 12
EOF
}
start() { # start <node> <tag>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.$2.log" 2>&1 &
	PIDS="$PIDS $!"
	echo $! > "$D/n$1.pid"
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.$2.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i + 1))
	done
	echo "node $1 did not start"; tail -3 "$D/n$1.$2.log"; return 1
}
cli() { # cli <node>  (JSON lines on stdin)
	timeout 30 ./perfcli -h 127.0.72.$1 -p 1742$1 -a $SEC -q 2>/dev/null
}
call() { printf '%s\n' "$2" | cli "$1" | head -1; }
stat_of() { # stat_of <node> <python expression over r = the stats reply>
	call "$1" '{"method":"stats"}' | python3 -c 'import json,sys
try:
    r = json.load(sys.stdin); c = r.get("cluster") or {}
    print(eval(sys.argv[1]))
except Exception: print(-1)' "$2" 2>/dev/null || echo -1
}
gen() { # gen <set|del> <prefix> <from> <to> [ttl]
	python3 - "$@" <<'PY'
import json, sys
op, pre, a, b = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
ttl = int(sys.argv[5]) if len(sys.argv) > 5 else 0
for i in range(a, b + 1):
    p = {"col": "c", "key": "%s-%d" % (pre, i)}
    if op == "set":
        p["value"] = "v-%d" % i
        if ttl:
            p["ttl"] = ttl
    print(json.dumps({"method": op, "params": p}))
PY
}
alive() { # alive <node> <prefix> <from> <to> -> how many are found
	python3 - "$2" "$3" "$4" <<'PY' | cli "$1" | python3 -c 'import json,sys
n=0
for line in sys.stdin:
    line=line.strip()
    if not line: continue
    try:
        if json.loads(line).get("found"): n+=1
    except Exception: pass
print(n)' 2>/dev/null || echo -1
import json, sys
pre, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
for i in range(a, b + 1):
    print(json.dumps({"method": "get", "params": {"col": "c", "key": "%s-%d" % (pre, i)}}))
PY
}
stop_all() {
	kill_ours
	i=0
	while [ $i -lt 100 ]; do
		live=0
		for p in $PIDS; do kill -0 "$p" 2>/dev/null && live=1; done
		[ $live = 0 ] && break
		sleep 0.1; i=$((i + 1))
	done
	PIDS=
	rm -rf "$D"/w1 "$D"/w2 "$D"/w3
}

# ---- one arm --------------------------------------------------------
# Leaves FAST, WHY, PROBED, RMS, TK1, TX1, FK1 (node 1 after its pass)
# and TK2, TX2 (node 2 a repair cycle later).
run_arm() { # run_arm <label> <retain_s> <retain_max> <fillers>
	ra=$1; FILL=$4
	echo "--- arm $ra: tombstone_retain_s $2, tombstone_retain_max $3, $FILL extra deletes after the snapshot"
	stop_all
	for i in 1 2 3; do mk $i "$2" "$3"; done
	for i in 1 2 3; do start $i "$ra-a" || return 1; done
	# the witnesses must be up past the grace before node 1 dies, and
	# (S392) before the window opens: its snapshot, less a 10 s margin
	i=0
	while [ $i -lt 90 ]; do
		[ "$(stat_of 2 '(r.get("process") or {}).get("uptime_s", 0)')" -ge 46 ] 2>/dev/null && break
		sleep 1; i=$((i + 1))
	done
	{ gen set tk 1 300; gen set tx 1 100; [ "$FILL" -gt 0 ] && gen set fk 1 "$FILL"; } | cli 2 >/dev/null
	i=0
	while [ $i -lt 40 ]; do
		H1=$(( $(alive 1 tk 1 300) + $(alive 1 tx 1 100) ))
		[ "$H1" = 400 ] && break
		sleep 1; i=$((i + 1))
	done
	[ "$H1" = 400 ] || { bad "arm $ra: node 1 holds $H1 of the 400 before its snapshot"; return 1; }
	s0=$(stat_of 1 'r["rdb"]["saves"]')
	call 1 '{"method":"save"}' >/dev/null
	i=0
	while [ $i -lt 50 ] && [ "$(stat_of 1 'r["rdb"]["saves"]')" = "$s0" ]; do
		sleep 0.2; i=$((i + 1))
	done
	[ "$(stat_of 1 'r["rdb"]["saves"]')" != "$s0" ] || { bad "arm $ra: node 1's snapshot did not complete"; return 1; }
	# node 1 is up: it applies these, and persists none of them
	{ gen del tk 1 100; gen set tx 1 100 3; [ "$FILL" -gt 0 ] && gen del fk 1 "$FILL"; } | cli 2 >/dev/null
	sleep 1
	kill -9 "$(cat "$D/n1.pid")" 2>/dev/null
	sleep 1
	gen del tk 101 200 | cli 2 >/dev/null
	sleep 5                                # the tx copies expire on the peers
	XP=$(alive 2 tx 1 100)
	[ "$XP" = 0 ] || note "arm $ra: node 2 still answers $XP tx keys before node 1 returns"
	start 1 "$ra-b" || return 1
	i=0
	while [ $i -lt 180 ]; do
		grep -q -E "reconcile pass finished asking|reconcile finished by the fast path|reconcile SKIPPED" "$D/n1.$ra-b.log" &&
			[ "$(stat_of 1 'c.get("reconcile_open")')" = False ] &&
			[ "$(stat_of 1 'c.get("reconcile_pending")')" = 0 ] && break
		sleep 0.5; i=$((i + 1))
	done
	FAST=$(stat_of 1 'c.get("reconcile_fast")')
	WHY=$(stat_of 1 'c.get("reconcile_fast_why")')
	PROBED=$(stat_of 1 'c.get("reconcile_probed")')
	RMS=$(stat_of 1 'c.get("reconcile_ms")')
	TK1=$(alive 1 tk 1 300); TX1=$(alive 1 tx 1 100)
	FK1=0; [ "$FILL" -gt 0 ] && FK1=$(alive 1 fk 1 "$FILL")
	note "node 1 after its pass: tk $TK1/300, tx $TX1/100, fk $FK1/$FILL; fast $FAST ($WHY), probed $PROBED, $RMS ms"
	note "$(grep -m1 -E 'reconcile (finished by the fast path|pass finished asking)' "$D/n1.$ra-b.log" | sed 's/.*cluster: //' | cut -c1-150)"
	sleep 12                               # a repair cycle after the hold
	TK2=$(alive 2 tk 1 300); TX2=$(alive 2 tx 1 100)
}

check_dropped() { # the same keys gone, whichever way the pass ran
	[ "$TK1" = 100 ] && [ "$TX1" = 0 ] && [ "$FK1" = 0 ] \
		&& ok "$1: node 1 holds tk 201-300 only - the deletes after its snapshot and during its outage are gone, and no tx came back" \
		|| bad "$1: node 1 holds tk $TK1 (want 100), tx $TX1 (want 0), fk $FK1 (want 0)"
	[ "$TK2" = 100 ] && [ "$TX2" = 0 ] \
		&& ok "$1: and a repair cycle later node 2 holds the same - nothing spread back from node 1" \
		|| bad "$1: a repair cycle later node 2 holds tk $TK2 (want 100), tx $TX2 (want 0)"
}

echo "=== reconcilefasttest ==="

run_arm A 900 250000 0 || { echo "reconcilefasttest: arm A did not run"; exit 1; }
[ "$FAST" = True ] && [ "$PROBED" = 0 ] && [ "${RMS:-99999}" -lt 15000 ] 2>/dev/null \
	&& ok "A: the fast path ended the pass in $RMS ms - nothing probed" \
	|| bad "A: reconcile_fast $FAST ($WHY), probed $PROBED, $RMS ms - the fast path did not end the pass"
check_dropped A

run_arm B 5 250000 0 || { echo "reconcilefasttest: arm B did not run"; exit 1; }
[ "$FAST" = False ] && echo "$WHY" | grep -q "reach back" && [ "${PROBED:-0}" -gt 0 ] 2>/dev/null \
	&& ok "B: retain_s 5 is shorter than the window: the witness said so ($WHY) and node 1 walked ($PROBED probed)" \
	|| bad "B: reconcile_fast $FAST, why '$WHY', probed $PROBED - want the walk, for a window older than the table"
check_dropped B

run_arm C 900 1000 1200 || { echo "reconcilefasttest: arm C did not run"; exit 1; }
[ "$FAST" = False ] && echo "$WHY" | grep -q "cap" && [ "${PROBED:-0}" -gt 0 ] 2>/dev/null \
	&& ok "C: the cap dropped deletes inside the window: the witness said so ($WHY) and node 1 walked ($PROBED probed)" \
	|| bad "C: reconcile_fast $FAST, why '$WHY', probed $PROBED - want the walk, the cap having dropped inside the window"
check_dropped C

echo "reconcilefasttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
