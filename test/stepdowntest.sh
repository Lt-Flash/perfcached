#!/bin/sh
# stepdowntest.sh - a daemon master that observes a higher term steps down: after a partition heals
# the master holding the higher term survives, not merely one master.  Usage: test/stepdowntest.sh [runtime]
set -u

RT=${1:-${RUNTIME:-}}       # the argument, else $RUNTIME (CI), else nerdctl or docker
[ -n "$RT" ] || { command -v nerdctl >/dev/null 2>&1 && RT=nerdctl || RT=docker; }
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

command -v "$RT" >/dev/null 2>&1 || { echo "stepdowntest: SKIPPED - no $RT"; exit "${PC_SKIP_FAILS:-0}"; }
for n in 1 2 3; do
	$RT inspect pcnode$n >/dev/null 2>&1 || {
		echo "stepdowntest: SKIPPED - pcnode$n not running (bench/containers-up.sh)"
		exit "${PC_SKIP_FAILS:-0}"; }
done

D=$(dirname "$0")
. "$D/lib/partition.sh"
part_init "$RT" || { echo "stepdowntest: SKIPPED - no fault injection"; exit "${PC_SKIP_FAILS:-0}"; }
trap 'part_heal_all >/dev/null 2>&1' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on

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
# the node's OWN term (pc_term_current): the highest it has seen, which its
# next promotion claims above
cterm()  { cl "$1" | python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("cluster") or {}).get("term",-1))
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

# the terms on each side: a node is master before it publishes its first map, so wait for the
# publication rather than read the old term
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
# the node that stepped down holds the term it yielded to, read at once before a map from the
# winner can bring it
ST=$(cterm $MN)
if [ "$M" = 1 ]; then
	ok "exactly one master ${i}s after the heal - the step-down fired"
	if [ "$ST" -ge "$NEWT" ] 2>/dev/null; then
		ok "the node that stepped down holds term $ST - at or above the one it yielded to ($NEWT)"
	else
		bad "the node that stepped down holds term $ST, below the $NEWT it
	     yielded to - its next promotion would claim a term the fleet
	     has already used"
	fi
else
	bad "$M masters ${i}s after the heal: n1=$(role 1) n2=$(role 2)
	     n3=$(role 3).  A master that saw a higher term did not step
	     down, so split brain outlives the partition that caused it"
fi

# ---- and the fleet agrees on which, within a few beats: for up to a beat (1 Hz) a member may still
# follow the loser's last beacon
i=0
while :; do
	A=$(master 1); B=$(master 2); Cc=$(master 3)
	{ [ "$A" = "$B" ] && [ "$B" = "$Cc" ] && [ "$A" != -1 ]; } && break
	[ $i -ge 5 ] && break
	sleep 1; i=$((i + 1))
done
if [ "$A" = "$B" ] && [ "$B" = "$Cc" ] && [ "$A" != -1 ]; then
	ok "all three nodes name the same master (node $A) ${i}s after the step-down"
else
	bad "the fleet disagrees about who the master is ${i}s after the step-down: n1=$A n2=$B n3=$Cc"
fi

# ---- the assertion: the higher term won.  The member-count/address cure alone converges the fleet
# too, on whichever node has the higher address.
# node 1 stepped down and is admitted again: the winner's map reaches it once it is back, up to 15 s
i=0; while [ $i -lt 15 ] && ! [ "$(mterm 1)" -ge "$NEWT" ] 2>/dev/null; do sleep 1; i=$((i + 1)); done
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
