#!/bin/sh
# stepdowntest.sh — clterm.h rule 3, against the DAEMON: a master that
# observes a higher term steps down.
#
# clustersim proves the rule composes; this proves the daemon runs it.
# They are different claims and both are needed - the simulator calls
# pc_term_must_stepdown directly, so it would stay green no matter what
# cluster.c did with the answer.
#
# The fault: isolate the master.  The majority side loses sight of it
# after MASTER_DEAD_MS (8000) and promotes, claiming a term above
# everything it has seen.  Heal, and the old master - still master, still
# sending MASTER_ALIVE - receives a map at the higher term.
#
# WHAT IS ASSERTED, and why "one master" is NOT enough.  Run against the
# unwired build this test passed 5/5 - because cluster.c already had a
# SECOND cure in handle_master_alive that ranks by member count and then
# by address.  It does converge the fleet to one master.  It just picks
# the wrong one: measured, the stale isolated master (10.99.0.13) won on
# address over the node that had legitimately promoted, and the fleet
# went BACKWARDS to the lower term.
#
# So the discriminating assertion is not "one master" but "the master
# holding the HIGHER TERM is the one that survived".  That is the whole
# content of rule 3, and it is the only assertion here that can tell a
# wired build from an unwired one.
#
# Usage: test/stepdowntest.sh [runtime]
set -u

RT=${1:-}
[ -n "$RT" ] || { command -v nerdctl >/dev/null 2>&1 && RT=nerdctl || RT=docker; }
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

command -v "$RT" >/dev/null 2>&1 || { echo "stepdowntest: SKIPPED - no $RT"; exit 0; }
for n in 1 2 3; do
	$RT inspect pcnode$n >/dev/null 2>&1 || {
		echo "stepdowntest: SKIPPED - pcnode$n not running (bench/containers-up.sh)"
		exit 0; }
done

D=$(dirname "$0")
. "$D/lib/partition.sh"
part_init "$RT" || { echo "stepdowntest: SKIPPED - no fault injection"; exit 0; }
trap 'part_heal_all >/dev/null 2>&1' EXIT INT TERM

cl() { $RT exec pcnode$1 perfcli -q -j '{"method":"stats"}' 2>/dev/null; }
role()   { cl "$1" | python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("cluster") or {}).get("role","?"))
except Exception: print("err")' 2>/dev/null || echo err; }
master() { cl "$1" | python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("cluster") or {}).get("master",-1))
except Exception: print(-1)' 2>/dev/null || echo -1; }
myid()   { cl "$1" | python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("cluster") or {}).get("node",-1))
except Exception: print(-1)' 2>/dev/null || echo -1; }
mterm()  { cl "$1" | python3 -c 'import json,sys
try: print(((json.load(sys.stdin).get("cluster") or {}).get("map") or {}).get("term",-1))
except Exception: print(-1)' 2>/dev/null || echo -1; }
nmasters() {
	c=0
	for n in 1 2 3; do [ "$(role $n)" = master ] && c=$((c+1)); done
	echo $c
}

echo "=== stepdowntest ($RT) ==="

# ---- baseline: exactly one master ----------------------------------
M=$(nmasters)
[ "$M" = 1 ] && ok "one master before the test" \
	|| { bad "the fleet starts with $M masters - nothing below is meaningful"
	     echo "stepdowntest: $pass passed, $fail failed"; exit 1; }
for n in 1 2 3; do
	[ "$(role $n)" = master ] && MN=$n
done
echo "       master is pcnode$MN (node $(myid $MN))"

# ---- isolate the master --------------------------------------------
for n in 1 2 3; do
	[ "$n" = "$MN" ] || part_cut pcnode$MN pcnode$n
done

# the majority side needs MASTER_DEAD_MS (8s) plus an election
echo "       waiting for the majority side to promote"
NEW=0
i=0
while [ $i -lt 40 ]; do
	for n in 1 2 3; do
		[ "$n" = "$MN" ] && continue
		[ "$(role $n)" = master ] && NEW=$n
	done
	[ "$NEW" != 0 ] && break
	sleep 1; i=$((i + 1))
done
if [ "$NEW" != 0 ]; then
	ok "the majority side promoted pcnode$NEW after ${i}s"
else
	bad "no new master appeared in ${i}s - the isolated-master fault did
	     not produce a competing term, so the step-down is untested"
	echo "stepdowntest: $pass passed, $fail failed"; exit 1
fi
# and the isolated one is still master: two, for now, which is correct
[ "$(role $MN)" = master ] \
	&& ok "the isolated master is still master while cut off (two, as expected)" \
	|| echo "       note: the isolated node left mastership on its own"

# THE TERMS ON EACH SIDE - this is what the verdict turns on.
# A node reports role=master the moment it is elected, which is BEFORE
# it publishes its first map, so map.term still reads the old epoch for
# a beat.  Sampling immediately caught that beat and reported the
# promoted node at term 1 while the daemon log said it had claimed 2.
# Wait for the publication rather than racing it.
OLDT=$(mterm $MN)
i=0
while [ $i -lt 20 ]; do
	NEWT=$(mterm $NEW)
	[ "$NEWT" -gt "$OLDT" ] 2>/dev/null && break
	sleep 1; i=$((i + 1))
done
OLDID=$(myid $MN); NEWID=$(myid $NEW)
echo "       terms while split: isolated pcnode$MN (node $OLDID) at $OLDT,"
echo "       promoted pcnode$NEW (node $NEWID) at $NEWT"
if [ "$NEWT" -gt "$OLDT" ] 2>/dev/null; then
	ok "the promotion really did claim a HIGHER term ($NEWT > $OLDT)"
else
	bad "the promoted node is at term $NEWT and the isolated one at
	     $OLDT - without a higher term there is nothing for rule 3 to
	     act on and the rest of this test is vacuous"
fi

# ---- heal: rule 3 must fire ----------------------------------------
part_heal_all
echo "       healed - the old master should now see a higher term"
i=0
while [ $i -lt 45 ]; do
	[ "$(nmasters)" = 1 ] && break
	sleep 1; i=$((i + 1))
done
M=$(nmasters)
if [ "$M" = 1 ]; then
	ok "exactly one master ${i}s after the heal - the step-down fired"
else
	bad "$M masters ${i}s after the heal: n1=$(role 1) n2=$(role 2)
	     n3=$(role 3).  A master that saw a higher term did not step
	     down, so split brain outlives the partition that caused it"
fi

# ---- and the fleet AGREES on which ---------------------------------
A=$(master 1); B=$(master 2); Cc=$(master 3)
if [ "$A" = "$B" ] && [ "$B" = "$Cc" ] && [ "$A" != -1 ]; then
	ok "all three nodes name the same master (node $A)"
else
	bad "the fleet disagrees about who the master is: n1=$A n2=$B n3=$Cc"
fi

# ---- THE assertion: the HIGHER TERM won -----------------------------
# Everything above is satisfied by the member-count/address cure alone,
# which converges the fleet while seating whichever node has the higher
# address.  Only this distinguishes a wired rule 3 from an unwired one.
FT=$(mterm 1)
if [ "$FT" -ge "$NEWT" ] 2>/dev/null; then
	ok "the fleet settled at term $FT - at or above the promoted term $NEWT"
else
	bad "the fleet settled at term $FT, BELOW the promoted term $NEWT -
	     the term went backwards, so a stale master outranked a
	     legitimate promotion (this is the member-count cure winning
	     where rule 3 should have)"
fi
if [ "$A" = "$NEWID" ]; then
	ok "the node that claimed the higher term is the surviving master"
else
	bad "node $A is master, but node $NEWID is the one that claimed the
	     higher term ($NEWT).  A master that saw a higher term kept its
	     role - rule 3 did not fire"
fi

echo "stepdowntest: $pass passed, $fail failed"
[ $fail -eq 0 ]
