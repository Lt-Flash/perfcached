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
# S238 changed the store-mode answer.  The survivors now REMEMBER the
# deletes (tombstone_retain_s) and replay them to a peer that comes back
# from a silence, so the returning node's WAL copies are deleted by
# version instead of served.  B and C pin the tombstone_ms claim itself,
# so they run with that memory OFF (tombstone_retain_s = 0), as before;
#   D  no eager, returns LONG after the window, retention ON (default) ->
#                no key comes back, and the survivor still says absent
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
# daemons die by the pids this script started - each checked against its
# command line, so a reused pid never hits a stranger - and the name-bound
# pattern stays as a fallback; a pattern alone let a renamed daemon outlive
# its suite (2026-09-09)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill "$p"; done; pkill -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on

# ---- fixture --------------------------------------------------------
mk() { # mk <node> <eager 0|1> <tombstone_ms> [tombstone_retain_s] [tombstone_retain_max]
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
tombstone_retain_s = ${4:-900}
${5:+tombstone_retain_max = $5}
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
	PIDS="$PIDS $!"
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
run_arm() { # run_arm <label> <eager> <tombstone_ms> <wait_before_return_s> [retain_s]
	ra_label=$1; ra_eager=$2; ra_tomb=$3; ra_wait=$4; ra_ret=${5:-900}
	echo "--- arm $ra_label: eager=$ra_eager tombstone_ms=$ra_tomb, node1 returns ${ra_wait}s after the delete"
	stop_all
	for i in 1 2 3; do mk $i "$ra_eager" "$ra_tomb" "$ra_ret"; done
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

	# S223: node1's reconcile will only run with a WITNESS - a peer up at
	# least 30 s (three sweep periods) before node1 went down, because only
	# that peer saw the deletes.  This arm started all three together a few
	# seconds ago, so wait until the survivors are old enough - 46 s:
	# node1's death is estimated from its alive stamp, touched every ten
	# beats, so it reads up to 10 s early and the effective grace is up
	# to 40 s; killed
	# earlier, node1 rightly skips the pass (a fleet that has only just
	# formed has no witness) and the deletes come back BY DESIGN - the
	# documented cost of the rule, not what this arm is testing.
	i=0
	while [ $i -lt 60 ]; do
		up=$(call 2 '{"method":"stats"}' | python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("process") or {}).get("uptime_s", 0))
except Exception: print(0)' 2>/dev/null)
		[ "${up:-0}" -ge 46 ] 2>/dev/null && break
		sleep 1; i=$((i + 1))
	done
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
run_arm B 0 2000 15 0 || { echo "tombstonetest: arm B did not run"; exit 1; }
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
run_arm C 0 30000 0 0 || { echo "tombstonetest: arm C did not run"; exit 1; }
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

# ---- arm D: no eager, well past the window, retention ON (S238) -------
run_arm D 0 2000 15 900 || { echo "tombstonetest: arm D did not run"; exit 1; }
if [ "$A_BACK" = 0 ] && [ "$A_PEER" = 0 ]; then
	ok "D: store mode with the deletes REMEMBERED - none of the $N came back on
       the returning node, and node2 still says absent (B, the same arm
       with the memory off, brought all $N back)"
else
	bad "D: with retained deletes $A_BACK of $N came back on the returning
	     node and node2 answers for $A_PEER - the replay did not reach it"
fi

# ---- arm E: the table's limits reach the alerts (AR15) ----------------
# A cap of 1,000 and 2,000 deletes: the cap drops the oldest before
# tombstone_retain_s, so cluster.tombstones (warning) - with the count, the
# cap and the window.  A member away longer than the 5-s window comes back:
# a survivor raises cluster.peer_absent, an event (one-off).  0.6.4 only
# logged both: neither reaches the alerts there.
echo "--- arm E: tombstone_retain_s 5, tombstone_retain_max 1000, 2000 deletes; node3 away 8 s"
stop_all
export PERFCACHED_ALERT_FAST=60
for i in 1 2 3; do mk $i 0 2000 5 1000; done
for i in 1 2 3; do start $i E-a || { echo "tombstonetest: arm E did not start"; exit 1; }; done
sleep 5
al() { # al <node> <python expr over a = stats' alerts block>
	call "$1" '{"method":"stats"}' | python3 -c 'import json,sys
try:
    a = json.load(sys.stdin)["alerts"]; print(eval(sys.argv[1]))
except Exception as e: print("err %s" % e)' "$2" 2>/dev/null
}
j=1
while [ $j -le 2000 ]; do
	echo "{\"method\":\"set\",\"params\":{\"col\":\"c\",\"key\":\"te-$j\",\"value\":\"v\"}}"
	j=$((j + 1))
done | cli 1 >/dev/null
j=1
while [ $j -le 2000 ]; do
	echo "{\"method\":\"del\",\"params\":{\"col\":\"c\",\"key\":\"te-$j\"}}"
	j=$((j + 1))
done | cli 1 >/dev/null
q='"cluster.tombstones" in [x["key"] for x in a["active"]]'
i=0
while [ $i -lt 30 ] && [ "$(al 1 "$q")" != True ]; do sleep 1; i=$((i + 1)); done
if [ "$(al 1 "$q")" = True ]; then
	ok "E: 2000 deletes past a cap of 1000 - cluster.tombstones raised: $(al 1 'next(x["text"] for x in a["active"] if x["key"] == "cluster.tombstones")')"
	# S443: and it says they were deletes - none of them expiries
	case "$(al 1 'next(x["text"] for x in a["active"] if x["key"] == "cluster.tombstones")')" in
	*" deletes, 0 expiries;"*) ok "E: the text says they were deletes, no expiries (S443)" ;;
	*) bad "E: the text does not split deletes from expiries: $(al 1 'next(x["text"] for x in a["active"] if x["key"] == "cluster.tombstones")')" ;;
	esac
else
	bad "E: no cluster.tombstones after 2000 deletes past a cap of 1000 - active: $(al 1 '[x["key"] for x in a["active"]]'); dropped_early $(stat_of 1 tombstones_retained)"
fi
kill -9 "$(pidof_node 3)" 2>/dev/null
sleep 8
start 3 E-b || { echo "tombstonetest: node3 did not come back"; exit 1; }
q='[x["text"] for x in a["recent"] if x["key"] == "cluster.peer_absent"][:1]'
i=0
while [ $i -lt 30 ] && [ "$(al 1 "$q")" = "[]" ]; do sleep 1; i=$((i + 1)); done
r=$(al 1 "$q")
case "$r" in
*"back after"*"tombstone_retain_s 5 s"*) ok "E: node3 back after 8 s, past the 5-s window - cluster.peer_absent on node1: $r" ;;
*) bad "E: no cluster.peer_absent on node1 after node3's 8-s absence: $r" ;;
esac

# ---- arm E2: the same cap filled by EXPIRIES (S443) ----------------------
# An eager collection notes every key its sweep reaps as a delete at that
# version (S392), so a TTL workload fills the table on its own: 2,000 keys
# with a 1-s TTL past a cap of 1,000 (retained 30 s, so the drops are early
# whatever the sweep's pace).  The alert and /stats must say EXPIRIES -
# 245-247 read 12.4 million "deletes dropped" that were a TTL benchmark.
# FAIL-FIRST: the build before S443 says "deletes dropped" and has no
# expiries figures.
echo "--- arm E2: eager, tombstone_retain_s 30, tombstone_retain_max 1000, 2000 keys with ttl 1"
stop_all
for i in 1 2 3; do mk $i 1 2000 30 1000; done
for i in 1 2 3; do start $i E2 || { echo "tombstonetest: arm E2 did not start"; exit 1; }; done
sleep 5
j=1
while [ $j -le 2000 ]; do
	echo "{\"method\":\"set\",\"params\":{\"col\":\"c\",\"key\":\"tx-$j\",\"value\":\"v\",\"ttl\":1}}"
	j=$((j + 1))
done | cli 1 >/dev/null
q='next((x["text"] for x in a["active"] if x["key"] == "cluster.tombstones"), "")'
i=0
while [ $i -lt 40 ] && [ -z "$(al 1 "$q")" ]; do sleep 1; i=$((i + 1)); done
r=$(al 1 "$q")
case "$r" in
*" 0 deletes, "*" expiries;"*) ok "E2: 2000 expiries past a cap of 1000 - cluster.tombstones says expiries, 0 deletes: $r" ;;
*) bad "E2: cluster.tombstones after 2000 expiries does not say they were expiries: '$r'" ;;
esac
tr=$(call 1 '{"method":"stats"}' | python3 -c 'import json,sys
try:
    t = json.load(sys.stdin)["cluster"]["tombstones_retained"]
    print("%s %s %s %s" % (t.get("dropped_early"), t.get("dropped_early_expiries"), t.get("expiries"), t.get("win_dropped_early")))
except Exception as e: print("err %s" % e)' 2>/dev/null)
set -- $tr
# at least the cap's overflow of node1's OWN 2,000 expiries; a replay to a
# peer back from a silence carries no kind on the wire, so what node1 got
# that way counts as deletes - the drops may hold some (GitLab 111796:
# 1,334 dropped, 1,157 expiries)
if [ "${2:-x}" != None ] && [ "${2:-0}" -ge 1000 ] 2>/dev/null && [ "$2" -le "${1:-0}" ] 2>/dev/null && [ "${3:-None}" != None ] && [ "${4:-None}" != None ]; then
	ok "E2: /stats: $1 dropped early, $2 of them expiries; $3 expiries kept; a 5-minute window ($4)"
else
	bad "E2: /stats tombstones_retained dropped_early/dropped_early_expiries/expiries/win_dropped_early: $tr"
fi

# ---- arm F: the settings the fleet must agree on (AR11) ------------------
# Node3 fsyncs less ([wal] fsync = no) than nodes 1 and 2 (always).  The
# keepalive carries each member's settings; "always" is the majority, so
# every node raises config.wal.fsync - node1 naming node3, node3 saying it
# is the one.  0.6.4 carries no settings and raises nothing.
echo "--- arm F: node3 on [wal] fsync = no beside two on always"
stop_all
for i in 1 2 3; do mk $i 0 2000; done
sed -i 's/^fsync = always$/fsync = no/' "$D/n3.conf"
for i in 1 2 3; do start $i F-a || { echo "tombstonetest: arm F did not start"; exit 1; }; done
q='[x["text"] for x in a["active"] if x["key"] == "config.wal.fsync"][:1]'
i=0
while [ $i -lt 30 ] && { [ "$(al 1 "$q")" = "[]" ] || [ "$(al 3 "$q")" = "[]" ]; }; do
	sleep 1; i=$((i + 1))
done
r1=$(al 1 "$q"); r3=$(al 3 "$q")
case "$r1" in
*"[wal] fsync differs on node "*"(127.0.49.3)"*"this node and 1 other(s) hold always"*)
	ok "F: three members, one fsyncing less - node1 names it: $r1" ;;
*) bad "F: node1's config.wal.fsync: $r1" ;;
esac
case "$r3" in
*"[wal] fsync differs here (no) from 2 of the fleet's 3 members"*)
	ok "F: and node3 says it is the one that differs: $r3" ;;
*) bad "F: node3's config.wal.fsync: $r3" ;;
esac
r2=$(al 2 '[x["key"] for x in a["active"] if x["key"].startswith("config.") and x["key"] != "config.wal.fsync"]')
[ "$r2" = "[]" ] && ok "F: no other setting is said to differ" || bad "F: other config alerts on node2: $r2"
# UP1: the same keepalive carries each member's build - the version its binary says, the wire
# epochs it speaks - in /members on every node; as facts they are never compared (r2 above)
V=$("$BIN" -V | head -1 | awk '{print $2}')
mv=$(call 1 '{"method":"members"}' | python3 -c 'import json,sys
r = json.load(sys.stdin); m = r.get("members", []) if isinstance(r, dict) else r
print(sorted(set((x.get("version"), tuple(x.get("wire_epochs") or [])) for x in m)), len(m))' 2>/dev/null)
[ "$mv" = "[('$V', (2, 2))] 3" ] && ok "F: UP1 - all three members report version $V, wire epochs 2..2" \
	|| bad "F: UP1 - /members versions $mv, want [('$V', (2, 2))] 3"
unset PERFCACHED_ALERT_FAST

echo "tombstonetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
