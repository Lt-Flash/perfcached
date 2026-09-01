#!/bin/sh
# clmapwiretest.sh — the cluster map on a live fleet (wiring step C).
#
# The map is published by the master and adopted by everyone else, and
# NOTHING CONSUMES IT YET - shard ownership still runs through the
# cluster's own HRW.  That is the point of this test: prove the plumbing
# carries a map correctly while nothing depends on it, so the day
# placement switches over, only one thing is new.
#
#  1. a master claims a term, persists it, and publishes a map
#  2. every other node adopts it, at the same epoch
#  3. the map names every live node and the right master
#  4. nothing is refused - a refused map means the fleet disagrees about
#     its own format, which is worse than not having one
#  5. the term SURVIVES A RESTART.  A term a node forgets is a term it
#     can reissue with different content behind it.
#  6. re-publication carries the epoch forward, so a node that missed a
#     datagram catches up without anyone tracking who has what
# Usage: test/clmapwiretest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
SEC=cm-client-secret
D=$(mktemp -d /var/tmp/pccm.XXXXXX)
trap 'pkill -9 -f "/var/tmp/[p]ccm" 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

for pf in 17961 17962 17963; do
	ss -ltn 2>/dev/null | grep -q ":$pf[[:space:]]" && {
		echo "clmapwiretest: port $pf already bound" >&2; exit 1; }
done

mk() {
	mkdir -p "$D/s$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = cm-cluster-secret
[listen]
tcp = 127.0.55.$1:1796$1
[wal]
dir = $D/s$1
segment_mb = 8
probe = no
save = off
[cluster]
multicast = 239.255.77.191:17291
advertise = 127.0.55.$1
mode = store
collections = c
[collection c]
buckets_log2 = 12
EOF
}
start() {
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}
cli() { n=$1; shift; ./perfcli -h 127.0.55.$n -p 1796$n -a $SEC "$@" 2>/dev/null; }
# $2 = dotted path under cluster, e.g. map.term
q() {
	cli $1 -j '{"method":"stats"}' | python3 -c '
import json,sys
p = sys.argv[1].split(".")
try:
    o = json.load(sys.stdin)["cluster"]
    for k in p: o = o[k]
    print(o)
except Exception: print("ERR")' "$2"
}
# role and map.received from ONE stats call: the check below needs both
# per node per poll, and two calls would double the load on a fleet the
# test is already timing.
rr() {                                 # rr <n> -> "<role> <received>"
	# bounded: an unresponsive node must cost ~3s, not the client
	# library's full patience, or the await above cannot keep its
	# wall-clock promise
	timeout 3 ./perfcli -h 127.0.55.$1 -p 1796$1 -a $SEC \
		-j '{"method":"stats"}' 2>/dev/null | python3 -c '
import json,sys
try:
    c = json.load(sys.stdin)["cluster"]
    print(c.get("role", "?"), c.get("map", {}).get("received", -1))
except Exception: print("ERR -1")'
}

await_map() {
	i=0
	while [ $i -lt 80 ]; do
		[ "$(q $1 map.valid)" = "True" ] && return 0
		sleep 0.25; i=$((i+1))
	done
	return 1
}

# Poll a condition until it holds, up to <secs>.  A fixed sleep followed
# by ONE check is a coin flip against a republication tick, and both
# flakes this file used to produce under load were exactly that: they
# passed alone and failed when the machine was busy enough to shift the
# tick relative to the sleep.
await() {                              # await <secs> <fn>
	# Deadline by WALL CLOCK, not by iteration count.  The old
	# `secs * 4` iterations assumed every predicate call was instant;
	# a predicate that blocks (a stats query to a frozen daemon waits
	# out the whole client timeout) stretched "await 25" to ten-plus
	# minutes of wall clock before it could report its own failure -
	# found by SIGSTOPping a peer and watching the harness time out
	# before the check did.
	aw_end=$(( $(date +%s) + $1 )); shift
	# ALWAYS check once, whatever the budget: a zero budget that never
	# calls the predicate leaves the reporting variables unset, and
	# under set -u that aborts the whole file mid-run - losing every
	# assertion after it, which is worse than the flake this replaced.
	while :; do
		"$@" && return 0
		[ "$(date +%s)" -ge "$aw_end" ] && return 1
		sleep 0.25
	done
}

# Sample all three epochs TOGETHER.  The old code read the master's
# epoch, then queried each node in turn, so a republication between
# those reads made them differ legitimately - and the failure message
# re-queried afterwards, printing three identical values as "diverged".
E1="unread"; E2="unread"; E3="unread"    # so the FAIL line can print
epochs_agree() {
	E1=$(q 1 map.term).$(q 1 map.seq)
	E2=$(q 2 map.term).$(q 2 map.seq)
	E3=$(q 3 map.term).$(q 3 map.seq)
	[ "$E1" = "$E2" ] && [ "$E2" = "$E3" ]
}

mk 1; mk 2; mk 3
start 1 && start 2 && start 3 || { echo "fleet did not start"; exit 1; }
sleep 7

# ---- 1. a term was claimed and a map published -------------------------
MT=0
for n in 1 2 3; do
	[ "$(q $n role)" = "master" ] && MASTER=$n
done
[ -n "${MASTER:-}" ] && ok "the fleet elected a master (node index $MASTER)" \
	|| { bad "no master"; echo "clmapwiretest: $pass passed, $((fail+1)) failed"; exit 1; }

T=$(q $MASTER term)
case "$T" in ''|ERR|0) bad "the master claimed no term (got '$T') - every map it issues would be unorderable against the next master's" ;;
	*) ok "the master claimed term $T" ;;
esac
P=$(q $MASTER map.published)
[ "${P:-0}" -ge 1 ] 2>/dev/null && ok "the master published a map ($P)" \
	|| bad "the master published no map (published=$P)"

# ---- 2 + 3. everyone adopts it, at the same epoch ----------------------
for n in 1 2 3; do
	await_map $n || bad "node $n never adopted a map"
done
if await 20 epochs_agree; then
	ok "every node holds the same epoch ($E1)"
else
	bad "epochs diverged: $E1 $E2 $E3"
fi

N=$(q $MASTER map.nodes)
[ "${N:-0}" = "3" ] && ok "the map names all 3 nodes" \
	|| bad "the map names $N node(s), expected 3"

MID=$(q $MASTER node)
[ "$(q 1 map.master)" = "$MID" ] && ok "the map names the right master ($MID)" \
	|| bad "map master is $(q 1 map.master), the master says $MID"

# ---- 3b. ONLY the master publishes -------------------------------------
# Two publishers means two opinions at one epoch, which is the ambiguity
# the term exists to remove.  Stated as observable behaviour rather than
# as a guard: the guards inside clmap_build/clmap_publish are unreachable
# today - the property is enforced by WHERE the function is called from,
# so a future call site added outside the master branch would slip past
# them and be caught only here.
NONM=0
for n in 1 2 3; do
	[ "$n" = "$MASTER" ] && continue
	P2=$(q $n map.published)
	[ "${P2:-0}" = "0" ] || NONM=$((NONM + 1))
done
[ "$NONM" = "0" ] && ok "no non-master published a map" \
	|| bad "$NONM non-master node(s) published a map - two publishers put
         two different maps at one epoch"

# ---- 4. nothing refused ------------------------------------------------
R=0
for n in 1 2 3; do R=$((R + $(q $n map.refused))); done
[ "$R" = "0" ] && ok "no map was refused" \
	|| bad "$R map(s) refused - the fleet disagrees about its own format"

# ---- 5. the term survives a restart ------------------------------------
BEFORE=$(q 2 term)
pkill -9 -f "$D/n2.conf"; sleep 1
: > "$D/n2.log"
start 2 || bad "node 2 did not restart"
sleep 3
AFTER=$(q 2 term)
[ "$AFTER" != "ERR" ] && [ "${AFTER:-0}" -ge "${BEFORE:-0}" ] 2>/dev/null \
	&& ok "the term survived a restart ($BEFORE -> $AFTER)" \
	|| bad "the term went backwards on restart ($BEFORE -> $AFTER) - a
         forgotten term can be reissued with different content"
grep -q "mastership term" "$D/n2.log" \
	&& ok "the restarted node logged the term it loaded" \
	|| bad "the restarted node said nothing about its term"

# ---- 6. re-publication moves the epoch forward -------------------------
S1=$(q $MASTER map.seq)
S2="unread"; RX="unread"                 # same reason as E1/E2/E3 above
seq_advanced() {
	S2=$(q $MASTER map.seq)
	[ "${S2:-0}" -gt "${S1:-0}" ] 2>/dev/null
}
# was: sleep 6 then check once.  Whether a ~5s republication tick lands
# inside a 6s window depends on where in its period the fleet started,
# and on how loaded the machine is - so it passed alone and failed under
# the full suite.  Poll instead: this also returns as soon as it is
# true, so the common case is FASTER than the old fixed sleep.
if await 25 seq_advanced; then
	ok "re-publication advanced the epoch ($S1 -> $S2)"
else
	bad "the epoch did not advance ($S1 -> $S2) - a node that missed
         a datagram would never catch up"
fi

# "Peers keep receiving maps" - so ask the PEERS.
#
# This polled a hardcoded `q 1 map.received` and failed with a hard 0
# whenever the election landed on node index 1, which under load it
# does: a master PUBLISHES rather than adopts, so its map.received is
# structurally zero and no amount of waiting could satisfy the check.
# Confirmed from the logs of a failing and a passing run - master index
# 1 versus 3 - and then reproduced deterministically by letting node 1
# found the cluster.
#
# Roles are re-derived on EVERY poll rather than captured once.
# Mastership is not fixed: node ids are proposed from a persisted
# identity hash, the founder takes id 1 regardless, and a switchover
# hands mastership to a node with quite a different id.  Anything this
# check remembers about who the master was can be stale by the time it
# next looks.
#
# The weakest peer decides, not the first one: a single node that stops
# adopting is exactly the failure worth catching, and averaging or
# first-match would hide it.
RXLOW="unread"; RXN="?"
rx_enough() {
	rx_low=""; rx_node=""
	for n in 1 2 3; do
		set -- $(rr $n)
		[ "$1" = "master" ] && continue
		if [ "$1" = "ERR" ]; then
			rx_low=""                  # unreadable: not a verdict
			break
		fi
		v=$2
		case "$v" in ''|*[!0-9]*) v=0 ;; esac
		if [ -z "$rx_low" ] || [ "$v" -lt "$rx_low" ]; then
			rx_low=$v; rx_node=$n
		fi
	done
	RXLOW=${rx_low:-unread}; RXN=${rx_node:-?}
	[ -n "$rx_low" ] && [ "$rx_low" -ge 2 ] 2>/dev/null
}
if await 25 rx_enough; then
	ok "peers keep receiving maps (lowest $RXLOW, node index $RXN)"
elif [ "$RXLOW" = "unread" ]; then
	bad "a peer never answered a stats query - cannot tell whether maps
         are still arriving"
else
	bad "peer at node index $RXN received only $RXLOW map(s)"
fi

echo "clmapwiretest: $pass passed, $fail failed"
[ $fail -eq 0 ]
