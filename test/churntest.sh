#!/bin/sh
# churntest.sh - membership churn faster than the liveness windows: membership converges to the truth
# afterwards, the fleet serves, an unchurned node keeps its data.  Usage: test/churntest.sh [runtime] [cycles]
set -u

RT=${1:-${RUNTIME:-}}       # the argument, else $RUNTIME (CI), else nerdctl or docker
[ -n "$RT" ] || { command -v nerdctl >/dev/null 2>&1 && RT=nerdctl || RT=docker; }
CYCLES=${2:-6}
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

command -v "$RT" >/dev/null 2>&1 || { echo "churntest: SKIPPED - no $RT"; exit "${PC_SKIP_FAILS:-0}"; }
for n in 1 2 3; do
	$RT inspect pcnode$n >/dev/null 2>&1 || {
		echo "churntest: SKIPPED - pcnode$n not running (bench/containers-up.sh)"
		exit "${PC_SKIP_FAILS:-0}"; }
done
# whatever happens, leave the churned node running: a stopped one breaks every later suite
trap '$RT start pcnode3 >/dev/null 2>&1' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on

set_k() { $RT exec pcnode$1 perfcli -q -j \
	"{\"method\":\"set\",\"params\":{\"col\":\"$2\",\"key\":\"$3\",\"value\":\"$4\"}}" \
	2>/dev/null | head -1; }
get_k() { $RT exec pcnode$1 perfcli -q -j \
	"{\"method\":\"get\",\"params\":{\"col\":\"$2\",\"key\":\"$3\"}}" 2>/dev/null |
	python3 -c 'import json,sys
try:
    r = json.load(sys.stdin); print(r.get("value") if r.get("found") else "MISS")
except Exception: print("ERR")' 2>/dev/null || echo ERR; }
peers() { $RT exec pcnode$1 perfcli -q -j '{"method":"stats"}' 2>/dev/null |
	python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("cluster") or {}).get("peers_up",-1))
except Exception: print(-1)' 2>/dev/null || echo -1; }

K=ch-$(date +%s)
echo "=== churntest ($RT, $CYCLES cycles) ==="

# ---- baseline -------------------------------------------------------
B1=$(peers 1); B2=$(peers 2)
{ [ "$B1" = 2 ] && [ "$B2" = 2 ]; } \
	&& ok "healthy fleet before churn (n1=$B1 n2=$B2)" \
	|| { bad "fleet unhealthy before churn (n1=$B1 n2=$B2)"
	     echo "churntest: $pass passed, $fail failed"; exit 1; }

# data on a node that will NOT be churned
set_k 1 b "survivor-$K" "still-here" >/dev/null

# ---- churn: real leave/join cycles.  A death is noticed after ~7 s (PEER_PURGE_MS plus a beat)
# and a rejoin takes ~4 s, so down 9 s / up 6 s is a genuine leave and rejoin.
DOWN=${DOWN:-9}
UP=${UP:-6}
echo "  churning pcnode3: down ${DOWN}s / up ${UP}s, $CYCLES cycles"
MINSEEN=2
c=0
while [ $c -lt "$CYCLES" ]; do
	$RT kill pcnode3 >/dev/null 2>&1
	d=0
	while [ $d -lt "$DOWN" ]; do
		V=$(peers 1)
		case "$V" in ''|*[!0-9-]*) ;; *)
			[ "$V" -lt "$MINSEEN" ] && MINSEEN=$V ;; esac
		sleep 1; d=$((d + 1))
	done
	$RT start pcnode3 >/dev/null 2>&1
	sleep "$UP"
	c=$((c + 1))
done
echo "  churn done (lowest peers_up seen from node1: $MINSEEN)"

# the churn must have been noticed, or nothing below means anything
if [ "$MINSEEN" -lt 2 ]; then
	ok "the churn was real: node1 saw the fleet drop to $MINSEEN peer(s)"
else
	bad "membership NEVER dropped during churn (min=$MINSEEN) - the
	     outages were shorter than the detection window, so every
	     assertion below would pass without the fleet being disturbed"
fi

# ---- 1. membership must converge to the truth -----------------------
CONV=0
i=0
while [ $i -lt 60 ]; do
	P1=$(peers 1); P2=$(peers 2); P3=$(peers 3)
	if [ "$P1" = 2 ] && [ "$P2" = 2 ] && [ "$P3" = 2 ]; then CONV=1; break; fi
	sleep 1; i=$((i + 1))
done
if [ $CONV = 1 ]; then
	ok "membership converged to 3 members after churn (${i}s)"
else
	bad "membership did NOT converge after ${i}s: n1=$P1 n2=$P2 n3=$P3 -
	     a node is stuck believing in a peer that is not there, or
	     cannot see one that is"
fi

# ---- 2. the fleet still serves --------------------------------------
W=$(set_k 2 b "after-$K" "post-churn")
sleep 1
R=$(get_k 2 b "after-$K")
[ "$R" = post-churn ] && ok "writes and reads still work after churn" \
	|| bad "the fleet does not serve after churn (set=$W get=$R)"

# ---- 3. an un-churned node kept its own data ------------------------
S=$(get_k 1 b "survivor-$K")
[ "$S" = still-here ] && ok "a node that was never killed kept its data" \
	|| bad "node1 lost a key it was never asked to give up (got $S)"

echo "churntest: $pass passed, $fail failed"
[ $fail -eq 0 ]
