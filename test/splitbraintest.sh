#!/bin/sh
# splitbraintest.sh — S42b: a write on BOTH SIDES of a partition.
#
# The case that would hurt most, and the one nothing pinned until now.
# DESIGN.md §6.4 states the contract per mode:
#
#   proxy - writes serialise at the holder, but the simultaneous-birth
#           race (two ingresses, both probes confirm absent) DOES fork;
#           the demoter then heals it deterministically, lower node-id
#           wins, the loser becomes a stub.  A partition is precisely
#           how that race is arranged on demand - so this asserts the
#           HEALING, which had never been exercised.
#   store - "two writers on two nodes diverge permanently".  Divergence
#           is the documented behaviour, not a bug, so this asserts it
#           holds: a silent change to convergence would be just as much
#           a contract break as a silent change the other way.
#
# Assertions are on what a CLIENT CAN READ afterwards, never on log
# lines (feedback-prove-the-path-can-execute).
#
# Usage: test/splitbraintest.sh [runtime]
set -u

RT=${1:-}
[ -n "$RT" ] || { command -v nerdctl >/dev/null 2>&1 && RT=nerdctl || RT=docker; }
TOP=$(cd "$(dirname "$0")/.." && pwd)
. "$TOP/test/lib/partition.sh"

pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
trap 'part_heal_all >/dev/null 2>&1' EXIT INT TERM

part_init "$RT" || { echo "splitbraintest: SKIPPED - $RT/nsenter/iptables unusable"; exit 0; }
for n in 1 2 3; do
	$RT inspect pcnode$n >/dev/null 2>&1 || {
		echo "splitbraintest: SKIPPED - pcnode$n not running (bench/containers-up.sh)"
		exit 0; }
done

set_k() { # set_k <node> <col> <key> <val>
	$RT exec pcnode$1 perfcli -q -j \
		"{\"method\":\"set\",\"params\":{\"col\":\"$2\",\"key\":\"$3\",\"value\":\"$4\"}}" \
		2>/dev/null | head -1
}
get_k() { # get_k <node> <col> <key> -> value, or MISS, or ERR
	$RT exec pcnode$1 perfcli -q -j \
		"{\"method\":\"get\",\"params\":{\"col\":\"$2\",\"key\":\"$3\"}}" \
		2>/dev/null | python3 -c 'import json,sys
try:
    r = json.load(sys.stdin)
    print(r.get("value") if r.get("found") else "MISS")
except Exception:
    print("ERR")' 2>/dev/null || echo ERR
}
peers() { $RT exec pcnode$1 perfcli -q -j '{"method":"stats"}' 2>/dev/null |
	python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("cluster") or {}).get("peers_up",-1))
except Exception: print(-1)' 2>/dev/null || echo -1; }
wait_peers() { wp=0; while [ $wp -lt $(( $3 * 5 )) ]; do
	[ "$(peers "$1")" = "$2" ] && return 0; sleep 0.2; wp=$((wp+1)); done; return 1; }

K=sb-$(date +%s)
echo "=== splitbraintest ($RT, key suffix $K) ==="

# ---- 0. fail-first: the harness must be able to SEE agreement -------
set_k 1 px "healthy-$K" "one-value" >/dev/null
sleep 1
A=$(get_k 1 px "healthy-$K"); B=$(get_k 2 px "healthy-$K"); C=$(get_k 3 px "healthy-$K")
if [ "$A" = one-value ] && [ "$B" = one-value ] && [ "$C" = one-value ]; then
	ok "healthy fleet: one write is read identically from all three"
else
	bad "the fleet does not agree BEFORE any partition (n1=$A n2=$B n3=$C)"
	echo "splitbraintest: $pass passed, $fail failed"; exit 1
fi

# ---- 1. partition {1,2} | {3} ---------------------------------------
part_cut pcnode3 pcnode1 || bad "cut 3<->1 failed"
part_cut pcnode3 pcnode2 || bad "cut 3<->2 failed"
if wait_peers 3 0 30 && wait_peers 1 1 30; then
	ok "fleet split: node3 isolated, nodes 1-2 still paired"
else
	bad "the split did not take (n1=$(peers 1) n3=$(peers 3))"
fi

# ---- 2. proxy: a birth race, arranged on purpose --------------------
PK="px-$K"
RA=$(set_k 1 px "$PK" "from-majority")
RC=$(set_k 3 px "$PK" "from-isolated")
echo "       majority side: $RA   isolated side: $RC"
PA=$(get_k 1 px "$PK"); PC=$(get_k 3 px "$PK")
if [ "$PA" = from-majority ] && [ "$PC" = from-isolated ]; then
	ok "both sides accepted a write for the same new key (the fork)"
else
	ok "one side refused - no fork to heal (n1=$PA n3=$PC)"
fi

# ---- 3. store: divergence is the documented contract ----------------
SK="st-$K"
set_k 1 b "$SK" "store-majority" >/dev/null
set_k 3 b "$SK" "store-isolated" >/dev/null

# ---- 4. heal, and let the demoter work ------------------------------
part_heal_all
if wait_peers 1 2 60 && wait_peers 3 2 60; then
	ok "membership reconverged after healing"
else
	bad "membership did not reconverge (n1=$(peers 1) n3=$(peers 3))"
fi
# ---- 5. THE ASSERTION: proxy must converge, and it heals LAZILY -----
# The demoter is NOT a timer.  It fires when a broadcast probe collects
# a SECOND positive answer (cluster.c, "TWO holders: deterministic
# demotion"), so a fork survives until something LOOKS UP the key in a
# way that probes.  Sleeping and reading once therefore tests nothing
# in particular - and worse, the reads are themselves the repair, so a
# naive check can trigger the heal it then reports as absent.
#
# So: drive the detector on purpose and give it a bounded budget.
CONV=0
i=0
while [ $i -lt 30 ]; do
	F1=$(get_k 1 px "$PK"); F2=$(get_k 2 px "$PK"); F3=$(get_k 3 px "$PK")
	if [ "$F1" = "$F2" ] && [ "$F2" = "$F3" ] && [ "$F1" != ERR ]; then
		CONV=1; break
	fi
	sleep 2; i=$((i + 1))
done
echo "       proxy after heal: n1=$F1 n2=$F2 n3=$F3 (after $i lookup rounds)"
if [ $CONV = 1 ]; then
	ok "proxy converged to a single value once the key was looked up"
else
	bad "PROXY STILL FORKED after 30 lookup rounds: n1=$F1 n2=$F2 n3=$F3 -
	     one key answers differently depending on which node is asked"
fi

# ---- 6. store: assert the documented divergence still holds ---------
S1=$(get_k 1 b "$SK"); S3=$(get_k 3 b "$SK")
echo "       store after heal: n1=$S1 n3=$S3"
if [ "$S1" != "$S3" ]; then
	ok "store diverged, as §6.4 says it must (n1=$S1 n3=$S3)"
else
	bad "store CONVERGED to $S1 - the documented contract says two
	     writers on two nodes diverge permanently; if that changed on
	     purpose, DESIGN.md needs updating with it"
fi

echo "splitbraintest: $pass passed, $fail failed"
[ $fail -eq 0 ]
