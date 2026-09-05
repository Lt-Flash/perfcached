#!/bin/sh
# tombstonetest.sh — S42e: a node returning with a WAL full of keys the
# fleet deleted while it was down, tested AT THE TOMBSTONE BOUNDARY.
#
# WHAT WAS ALREADY COVERED, AND WHAT WAS NOT.  rejointest's obligation 3
# already shows a deleted key not coming back - but it runs with
# `eager = 1` and brings the node back about two seconds after the
# delete, so BOTH possible protections are in play at once and the test
# cannot say which one held.  `tombstone_ms` is a configured number
# (100..60000, default 2000) and nothing had ever been run against its
# boundary.
#
# WHAT THE CODE SAYS, and this suite exists to pin it:
#
#   - a tombstone is a LIVE BROADCAST plus a negative-cache entry on the
#     nodes that RECEIVE it (cluster.c pc_tombstone_send / pc_neg_set).
#     A node that is DOWN for the delete never receives one, and its WAL
#     still holds the SET.  So the window does nothing whatsoever for
#     the returning node's own copy.
#   - what actually removes it is the B4 reconcile, which is EAGER-ONLY
#     by design: "in plain store mode peers legitimately lack keys they
#     never pulled, and absent-means-deleted would destroy live data".
#
# Which yields the claim this suite tests, and which is easy to get
# backwards when reading the config:
#
#     tombstone_ms bounds how long the fleet HIDES a resurrection.
#     It does not decide whether one happens.
#
# THREE ARMS, so no arm can pass by luck:
#   A  eager,    returns LONG after the window -> keys stay deleted, and
#                the reconcile counters must move (it worked, not luck)
#   B  no eager, returns LONG after the window -> keys come back AND the
#                resurrection SPREADS to a survivor that had deleted them
#   C  no eager, returns WELL INSIDE the window -> keys come back just
#                the same; the survivor merely still says absent, which
#                is the masking, not a fix
# B is A's control: it proves the WAL really carries the keys, so A's
# pass cannot be an empty replay.
#
# usage: test/tombstonetest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
N=${N:-200}
SEC=tb-client-secret
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
note(){ echo "       $1"; }

command -v ss >/dev/null 2>&1 && for pf in 17961 17962 17963; do
	ss -ltn 2>/dev/null | grep -q ":$pf[[:space:]]" && {
		echo "tombstonetest: port $pf already bound" >&2; exit 1; }
done

D=$(mktemp -d /var/tmp/pctb.XXXXXX)
# INSTANCE-SCOPED cleanup: a prefix-glob pkill would murder a concurrent
# twin (and once killed a live CI job).  Match this fixture's own dir.
trap 'pkill -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT INT TERM

# ---- fixture --------------------------------------------------------
mk() { # mk <node> <eager 0|1> <tombstone_ms>
	_md=store; [ "$2" = 1 ] && _md=eager
	mkdir -p "$D/w$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = tb-cluster-secret
[listen]
tcp = 127.0.49.$1:1796$1
[cluster]
multicast = 239.255.77.154:17263
advertise = 127.0.49.$1
pull_timeout_ms = 400
tombstone_ms = $3
mode = $_md
collections = c
[wal]
dir = $D/w$1
segment_mb = 8
probe = no
fsync = always
save = off
[collection c]
buckets_log2 = 12
EOF
}
start() { # start <node> <tag>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.$2.log" 2>&1 &
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.$2.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i + 1))
	done
	echo "node $1 did not start"; tail -3 "$D/n$1.$2.log"; return 1
}
pidof_node() { pgrep -f "[p]erfcached -f $D/n$1.conf" | head -1; }
cli() { # cli <node>  (JSON lines on stdin)
	timeout 30 ./perfcli -h 127.0.49.$1 -p 1796$1 -a $SEC -q 2>/dev/null
}
call() { printf '%s\n' "$2" | cli "$1" | head -1; }
found() { # found <node> <key> -> yes|no
	call "$1" "{\"method\":\"get\",\"params\":{\"col\":\"c\",\"key\":\"$2\"}}" |
		python3 -c 'import json,sys
try: print("yes" if json.load(sys.stdin).get("found") else "no")
except Exception: print("err")' 2>/dev/null || echo err
}
stat_of() { # stat_of <node> <field>
	call "$1" '{"method":"stats"}' | python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("cluster") or {}).get("'"$2"'", -1))
except Exception: print(-1)' 2>/dev/null || echo -1
}
# how many of the N keys node <n> still answers "found" for
alive_on() { # alive_on <node>
	j=1
	while [ $j -le "$N" ]; do
		echo "{\"method\":\"get\",\"params\":{\"col\":\"c\",\"key\":\"tk-$j\"}}"
		j=$((j + 1))
	done | cli "$1" | python3 -c 'import json,sys
n=0
for line in sys.stdin:
    line=line.strip()
    if not line: continue
    try:
        if json.loads(line).get("found"): n+=1
    except Exception: pass
print(n)' 2>/dev/null || echo -1
}

stop_all() {
	pkill -f "[p]erfcached -f $D" 2>/dev/null
	i=0
	while [ $i -lt 100 ] && pgrep -f "[p]erfcached -f $D" >/dev/null 2>&1; do
		sleep 0.1; i=$((i + 1))
	done
	rm -rf "$D"/w1 "$D"/w2 "$D"/w3
}

# ---- one arm --------------------------------------------------------
# Returns via the globals A_BACK (keys alive on the returned node) and
# A_PEER (keys alive on a survivor that deleted them).
run_arm() { # run_arm <label> <eager> <tombstone_ms> <wait_before_return_s>
	ra_label=$1; ra_eager=$2; ra_tomb=$3; ra_wait=$4
	echo "--- arm $ra_label: eager=$ra_eager tombstone_ms=$ra_tomb, node1 returns ${ra_wait}s after the delete"
	stop_all
	for i in 1 2 3; do mk $i "$ra_eager" "$ra_tomb"; done
	for i in 1 2 3; do start $i "$ra_label-a" || return 1; done
	sleep 5

	# fill through node1, then make sure a SURVIVOR has them too, so the
	# delete below is a delete of data the fleet really holds
	j=1
	while [ $j -le "$N" ]; do
		echo "{\"method\":\"set\",\"params\":{\"col\":\"c\",\"key\":\"tk-$j\",\"value\":\"v-$j\"}}"
		j=$((j + 1))
	done | cli 1 >/dev/null
	i=0
	while [ $i -lt 40 ]; do
		[ "$(alive_on 2)" = "$N" ] && break
		sleep 1; i=$((i + 1))
	done
	ra_pre2=$(alive_on 2)
	note "before the kill: node2 answers for $ra_pre2/$N keys"
	[ "$ra_pre2" = "$N" ] || { bad "arm $ra_label: the fleet never held all
	     $N keys ($ra_pre2) - the delete would not be a real delete"
	     return 1; }

	kill -9 "$(pidof_node 1)" 2>/dev/null
	sleep 2
	# delete every key from a SURVIVOR while node1 is down
	j=1
	while [ $j -le "$N" ]; do
		echo "{\"method\":\"del\",\"params\":{\"col\":\"c\",\"key\":\"tk-$j\"}}"
		j=$((j + 1))
	done | cli 2 >/dev/null
	sleep 1
	ra_del2=$(alive_on 2)
	[ "$ra_del2" = 0 ] \
		&& ok "arm $ra_label: all $N keys deleted on the survivors" \
		|| bad "arm $ra_label: $ra_del2 key(s) survived the delete on node2"

	# THE BOUNDARY: how long we wait before the node comes back
	sleep "$ra_wait"
	start 1 "$ra_label-b" || return 1
	sleep 20                       # reconcile budget is 64 probes/tick

	A_BACK=$(alive_on 1)
	A_PEER=$(alive_on 2)
	A_PROBED=$(stat_of 1 reconcile_probed)
	A_RECON=$(stat_of 1 reconciled)
	note "after the return: node1 answers for $A_BACK/$N, node2 for $A_PEER/$N"
	note "node1 reconcile: probed=$A_PROBED reconciled=$A_RECON"
}

echo "=== tombstonetest ($N keys) ==="

# ---- arm A: eager, well past the window -----------------------------
run_arm A 1 2000 15 || { echo "tombstonetest: arm A did not run"; exit 1; }
if [ "$A_BACK" = 0 ]; then
	ok "A: eager - $N deleted keys stayed deleted, $((15))s past a 2s window"
else
	bad "A: eager - $A_BACK of $N deleted keys came back on the returning
	     node despite the reconcile"
fi
# and prove WHY: the reconcile must actually have run, or A passed
# because the WAL was empty rather than because anything worked
if [ "${A_PROBED:-0}" -gt 0 ]; then
	ok "A: the reconcile really ran (probed $A_PROBED key(s))"
else
	bad "A: reconcile_probed is $A_PROBED - nothing was reconciled, so the
	     keys were never in the WAL and this arm proves nothing"
fi

# ---- arm B: no eager, well past the window --------------------------
run_arm B 0 2000 15 || { echo "tombstonetest: arm B did not run"; exit 1; }
if [ "$A_BACK" = "$N" ]; then
	ok "B: no eager - all $N keys came back, as 6.4 says they must
       (this is also A's control: the WAL really does carry them)"
else
	bad "B: no eager - only $A_BACK of $N keys came back.  Either the WAL
	     is not replaying, in which case arm A proved nothing, or plain
	     store mode has quietly acquired a reconcile it must not have"
fi
if [ "$A_PEER" -gt 0 ]; then
	ok "B: the resurrection SPREAD - node2 deleted them and now answers
       for $A_PEER/$N again, the window having expired"
else
	bad "B: node2 still answers for 0 keys - the resurrection did not
	     spread, so the pull path is not doing what 6.4 describes"
fi

# ---- arm C: no eager, well INSIDE the window ------------------------
run_arm C 0 30000 0 || { echo "tombstonetest: arm C did not run"; exit 1; }
if [ "$A_BACK" = "$N" ]; then
	ok "C: inside a 30s window the keys came back ANYWAY - tombstone_ms
       does not protect a node that was down for the delete"
else
	bad "C: only $A_BACK of $N came back inside the window - if the window
	     really does protect the returning node's own copy, the comment
	     in cluster.c and this suite's premise are both wrong"
fi
if [ "$A_PEER" = 0 ]; then
	ok "C: and node2 still says absent - the window MASKS the split, it
       does not heal it (node1 serves $A_BACK, node2 serves $A_PEER)"
else
	bad "C: node2 answers for $A_PEER key(s) inside its own negative-cache
	     window - the tombstone is not suppressing the pull it should"
fi

echo "tombstonetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
