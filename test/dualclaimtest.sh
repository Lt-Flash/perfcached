#!/bin/sh
# dualclaimtest.sh — two nodes that claim the SAME term.
#
# clustersim reports 1296/2000 schedules ending with two masters when a
# master dies in the same window as a split - because clterm.h's rule 3
# is a STRICT inequality (`theirs > mine`), so two nodes promoting from
# the same highest seen term hold the same term and neither can act on
# the other.
#
# But clustersim models rule 3 ALONE.  The daemon also carries the
# member-count/address cure in handle_master_alive, which is a total
# order and may already resolve this.  So before anything is "fixed",
# stage it against the daemon and find out.  A fix for a bug the product
# does not have is worse than no fix.
#
# THE FAULT: kill the master AND cut the two survivors from each other
# in the same instant.  Each survivor is then alone, sees no master, and
# promotes from the same term the dead master held - so both claim the
# same next one.  Heal them to each other and see what happens.
#
# Usage: test/dualclaimtest.sh [runtime]
set -u

RT=${1:-}
[ -n "$RT" ] || { command -v nerdctl >/dev/null 2>&1 && RT=nerdctl || RT=docker; }
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

command -v "$RT" >/dev/null 2>&1 || { echo "dualclaimtest: SKIPPED - no $RT"; exit 0; }
for n in 1 2 3; do
	$RT inspect pcnode$n >/dev/null 2>&1 || {
		echo "dualclaimtest: SKIPPED - pcnode$n not running"; exit 0; }
done

D=$(dirname "$0")
. "$D/lib/partition.sh"
part_init "$RT" || { echo "dualclaimtest: SKIPPED - no fault injection"; exit 0; }
trap 'part_heal_all >/dev/null 2>&1; for n in 1 2 3; do
      $RT start pcnode$n >/dev/null 2>&1; done' EXIT INT TERM

cl() { $RT exec pcnode$1 perfcli -q -j '{"method":"stats"}' 2>/dev/null; }
role()  { cl "$1" | python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("cluster") or {}).get("role","?"))
except Exception: print("down")' 2>/dev/null || echo down; }
myid()  { cl "$1" | python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("cluster") or {}).get("node",-1))
except Exception: print(-1)' 2>/dev/null || echo -1; }
mterm() { cl "$1" | python3 -c 'import json,sys
try: print(((json.load(sys.stdin).get("cluster") or {}).get("map") or {}).get("term",-1))
except Exception: print(-1)' 2>/dev/null || echo -1; }

echo "=== dualclaimtest ($RT) ==="

MN=0
for n in 1 2 3; do [ "$(role $n)" = master ] && MN=$n; done
[ "$MN" != 0 ] && ok "one master before the test (pcnode$MN, term $(mterm $MN))" \
	|| { bad "no master to begin with"; echo "dualclaimtest: $pass passed, $fail failed"; exit 1; }

# the two survivors
A=0; B=0
for n in 1 2 3; do
	[ "$n" = "$MN" ] && continue
	[ "$A" = 0 ] && A=$n || B=$n
done
echo "       survivors will be pcnode$A and pcnode$B"

# CUT FIRST, then kill: if the master dies first the survivors can still
# see each other, elect ONE master between them, and the collision never
# happens.  The cut has to be in place before they start looking.
part_cut pcnode$A pcnode$B
$RT kill pcnode$MN >/dev/null 2>&1
echo "       master killed, survivors cut from each other"

# each alone, each with no master: both must promote
TA=-1; TB=-1
i=0
while [ $i -lt 45 ]; do
	[ "$(role $A)" = master ] && [ "$(role $B)" = master ] && break
	sleep 1; i=$((i + 1))
done
RA=$(role $A); RB=$(role $B)
TA=$(mterm $A); TB=$(mterm $B)
echo "       after ${i}s: pcnode$A is $RA at term $TA (node $(myid $A)),"
echo "       pcnode$B is $RB at term $TB (node $(myid $B))"
if [ "$RA" = master ] && [ "$RB" = master ]; then
	ok "both survivors promoted independently"
else
	bad "only one side promoted ($RA / $RB) - the collision was not
	     staged, so nothing below is a test of it"
	echo "dualclaimtest: $pass passed, $fail failed"; exit 1
fi
if [ "$TA" = "$TB" ]; then
	ok "they claimed the SAME term ($TA) - this is the collision"
else
	bad "the terms differ ($TA vs $TB) - rule 3 alone resolves that, so
	     this run is not exercising the equal-term case"
fi

# ---- heal them to each other ---------------------------------------
part_heal_all
echo "       healed - two masters at the same term now see each other"
i=0
while [ $i -lt 45 ]; do
	M=0
	for n in $A $B; do [ "$(role $n)" = master ] && M=$((M+1)); done
	[ "$M" = 1 ] && break
	sleep 1; i=$((i + 1))
done
M=0
for n in $A $B; do [ "$(role $n)" = master ] && M=$((M+1)); done
if [ "$M" = 1 ]; then
	ok "resolved to ONE master after ${i}s (pcnode$A=$(role $A) pcnode$B=$(role $B))"
else
	bad "$M masters after ${i}s at the same term: pcnode$A=$(role $A)
	     term $(mterm $A), pcnode$B=$(role $B) term $(mterm $B).  Rule 3
	     is a strict inequality and nothing else broke the tie"
fi

echo "dualclaimtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
