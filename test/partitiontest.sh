#!/bin/sh
# partitiontest.sh — S42a: prove the partition primitive actually
# partitions, and that healing actually heals.
#
# This asserts the TOOL, not the cluster's behaviour under partition -
# that is S42b onwards.  It exists because every later fault-injection
# test is worthless if `cut` silently does nothing: a suite that cut
# nothing and then observed "no split brain" would be the most
# convincing wrong answer available.
#
# Assertions, in order:
#   1. a healthy fleet sees all three members      (the fail-first
#      baseline: run before any rule exists)
#   2. cutting 1<->2 is visible in the netns rules (the mechanism)
#   3. the two cut nodes stop seeing each other    (the effect)
#   4. healing restores the rules AND the membership
#
# Usage: test/partitiontest.sh [runtime]
set -u

RT=${1:-}
[ -n "$RT" ] || { command -v nerdctl >/dev/null 2>&1 && RT=nerdctl || RT=docker; }
TOP=$(cd "$(dirname "$0")/.." && pwd)
. "$TOP/test/lib/partition.sh"

pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

# heal on ANY exit - a leaked DROP rule outlives this script and
# poisons every later run against the same fleet
trap 'part_heal_all >/dev/null 2>&1' EXIT INT TERM

part_init "$RT" || { echo "partitiontest: SKIPPED - $RT/nsenter/iptables unusable"; exit 0; }

for n in 1 2 3; do
	$RT inspect pcnode$n >/dev/null 2>&1 || {
		echo "partitiontest: SKIPPED - pcnode$n is not running."
		echo "  bring the fleet up first: bench/containers-up.sh"
		exit 0; }
done

# peers_up as this node sees it right now
peers() { # peers <n>
	$RT exec pcnode$1 perfcli -q -j '{"method":"stats"}' 2>/dev/null |
		python3 -c 'import json,sys
try:
    c = json.load(sys.stdin).get("cluster") or {}
    print(c.get("peers_up", -1))
except Exception:
    print(-1)' 2>/dev/null || echo -1
}

# wait until <n> reports <want> peers, or give up after ~<secs>
wait_peers() { # wait_peers <n> <want> <secs>
	wp_i=0
	while [ $wp_i -lt $(( $3 * 5 )) ]; do
		[ "$(peers "$1")" = "$2" ] && return 0
		sleep 0.2; wp_i=$((wp_i + 1))
	done
	return 1
}

echo "=== partitiontest ($RT) ==="

# ---- 1. baseline: a healthy fleet, before any rule exists ------------
B1=$(peers 1); B2=$(peers 2)
if [ "$B1" = 2 ] && [ "$B2" = 2 ]; then
	ok "healthy fleet: node1 and node2 each see 2 peers"
else
	bad "fleet is not healthy to begin with (n1=$B1 n2=$B2) - nothing
	     below would mean anything"
	echo "partitiontest: $pass passed, $fail failed"; exit 1
fi
R0=$(part_rules pcnode1)
[ "$R0" = 0 ] && ok "no DROP rules before the cut" \
	|| bad "node1 already had $R0 DROP rule(s) - a previous run leaked"

# ---- 2. the mechanism -----------------------------------------------
part_cut pcnode1 pcnode2 || bad "part_cut returned an error"
R1=$(part_rules pcnode1); R2=$(part_rules pcnode2)
{ [ "$R1" -ge 1 ] && [ "$R2" -ge 1 ]; } \
	&& ok "the cut installed a DROP at BOTH ends (n1=$R1 n2=$R2)" \
	|| bad "the cut is one-sided or absent (n1=$R1 n2=$R2)"

# ---- 3. the effect: they must lose each other ------------------------
# MASTER_DEAD_MS/PEER_UP_MS govern how long this takes; 30s is generous
if wait_peers 1 1 30; then
	ok "node1 lost sight of node2 ($(peers 1) peer left)"
else
	bad "node1 still reports $(peers 1) peers 30s after the cut - the
	     rule is in place but traffic is still getting through"
fi

# ---- 4. healing restores both the rules and the membership ----------
part_heal pcnode1 pcnode2
H1=$(part_rules pcnode1); H2=$(part_rules pcnode2)
{ [ "$H1" = 0 ] && [ "$H2" = 0 ]; } \
	&& ok "heal removed the rules at both ends" \
	|| bad "rules survived the heal (n1=$H1 n2=$H2)"
if wait_peers 1 2 60; then
	ok "membership reconverged after healing ($(peers 1) peers)"
else
	bad "node1 still reports $(peers 1) peers 60s after healing"
fi

echo "partitiontest: $pass passed, $fail failed"
[ $fail -eq 0 ]
