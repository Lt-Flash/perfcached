#!/bin/sh
# reshardtest.sh — S42d: a hand-over whose receiver never receives, while
# ownership changes underneath.
#
# THE INVARIANT UNDER TEST (doc/DESIGN.md, "fork-impossibility"):
#
#   stub-first, never reinstate - under the bucket lock the value is
#   swapped for a forwarding stub, THEN sent; on transfer failure the
#   key is simply gone.  A miss beats a fork, because a write may
#   already have landed at the receiver.
#
# So the failure this hunts is not lost data - losing the key is the
# DESIGNED outcome of a failed transfer, and the test asserts nothing
# against it.  The failure is the same key existing in two places at
# once, which is what a send-then-delete, or a delete-with-reinstate-on-
# error, would produce the moment a write reached the receiver first.
#
# THE FAULT.  A partition is the wrong instrument here: it cuts both
# ways, so the donor stops hearing the receiver, drops it from the
# membership within PEER_UP_MS, and never tries to hand anything over.
# What is needed is one-directional loss - part_mute - so the donor goes
# on believing the receiver is alive and keeps handing keys to a peer
# that will never receive them.  That also puts the two ends in genuine
# disagreement about who is alive, which is the "ownership changes
# underneath" half of the scenario.
#
# THE SHAPE.
#   1. shrink the fleet to {1,2} and fill the shard collection
#   2. bring node3 back - ownership recomputes, keys are owed to it
#   3. MUTE node1 -> node3: node1's hand-overs vanish; node2's do not
#   4. rewrite every key through node2, so the new owner takes a NEWER
#      value while node1 is still trying to hand over the old one -
#      exactly the "a write may already have landed" case
#   5. unmute, settle, and take a LOCAL inventory of all three nodes
#      (scan is a local table walk - no forwarding, so it shows what a
#      node really holds rather than what the cluster would answer)
#
# Usage: test/reshardtest.sh [runtime] [nkeys]
set -u

RT=${1:-}
[ -n "$RT" ] || { command -v nerdctl >/dev/null 2>&1 && RT=nerdctl || RT=docker; }
N=${2:-400}
COL=sh
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

command -v "$RT" >/dev/null 2>&1 || { echo "reshardtest: SKIPPED - no $RT"; exit 0; }
for n in 1 2 3; do
	$RT inspect pcnode$n >/dev/null 2>&1 || {
		echo "reshardtest: SKIPPED - pcnode$n not running (bench/containers-up.sh)"
		exit 0; }
done

D=$(dirname "$0")
. "$D/lib/partition.sh"
part_init "$RT" || { echo "reshardtest: SKIPPED - no fault injection"; exit 0; }

cli()  { $RT exec -i pcnode$1 perfcli -q 2>/dev/null; }        # stdin batch
cli1() { $RT exec pcnode$1 perfcli -q -j "$2" 2>/dev/null | head -1; }

# The collection has to be SHARD mode or none of this means anything:
# store and proxy never recompute ownership, so nothing is ever handed
# over and every assertion below would pass on an idle fleet.
MODE=$(cli1 1 "{\"method\":\"stats\",\"params\":{\"col\":\"$COL\"}}" |
	python3 -c 'import json,sys
try:
    c = (json.load(sys.stdin).get("collections") or [{}])[0]
    print(c.get("mode", "unreported"))
except Exception: print("none")' 2>/dev/null || echo none)
[ "$MODE" = shard ] || {
	echo "reshardtest: SKIPPED - collection '$COL' is '$MODE', not shard"
	echo "             (re-run bench/containers-up.sh to get it)"
	exit 0; }

peers() { cli1 "$1" '{"method":"stats"}' | python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("cluster") or {}).get("peers_up",-1))
except Exception: print(-1)' 2>/dev/null || echo -1; }
mlost() { cli1 "$1" '{"method":"stats"}' | python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("cluster") or {}).get("migrate_lost",-1))
except Exception: print(-1)' 2>/dev/null || echo -1; }

# LOCAL inventory of one node: "<key> <value>" per line, straight from
# that node's own table.
#
# THE CURSOR MUST BE DRAINED.  scan's `count` is MAX BUCKETS, not max
# items (pcache_ht_scan_ex: "from bucket *cursor it visits up to
# @max_buckets buckets... then sets *cursor to the bucket to resume
# from - or 0 once the walk is complete").  A single call over a
# 65536-bucket collection therefore reports whatever happens to live in
# the first slice and NOTHING else - which is a fork detector that
# cannot see a fork, the exact false green this suite exists to avoid.
# The loop below is bounded so a bug in the cursor cannot hang the run.
inv() { # inv <node>
	iv_cur=0
	iv_round=0
	while [ $iv_round -lt 64 ]; do
		$RT exec pcnode$1 perfcli -q -j \
		    "{\"method\":\"scan\",\"params\":{\"col\":\"$COL\",\"cursor\":$iv_cur,\"count\":16384,\"values\":true}}" \
		    2>/dev/null > /tmp/reshard-scan.$$
		iv_cur=$(python3 -c '
import json,sys
try: print(json.load(open("/tmp/reshard-scan.'"$$"'")).get("cursor", 0))
except Exception: print(0)' 2>/dev/null || echo 0)
		python3 -c '
import json,sys
try: r = json.load(open("/tmp/reshard-scan.'"$$"'"))
except Exception: sys.exit(0)
for it in r.get("items") or []:
    k = it.get("k"); v = it.get("v")
    if k is not None and v is not None:
        print(k, v)
' 2>/dev/null
		[ "${iv_cur:-0}" = 0 ] && break
		iv_round=$((iv_round + 1))
	done
	rm -f /tmp/reshard-scan.$$
}

# whatever happens: heal the plane and leave every node running
trap 'part_heal_all >/dev/null 2>&1; $RT start pcnode3 >/dev/null 2>&1' \
	EXIT INT TERM

R=rs$(date +%s)
echo "=== reshardtest ($RT, $N keys, collection $COL) ==="

# ---- 0. baseline ----------------------------------------------------
B1=$(peers 1); B2=$(peers 2); B3=$(peers 3)
{ [ "$B1" = 2 ] && [ "$B2" = 2 ] && [ "$B3" = 2 ]; } \
	&& ok "healthy 3-node fleet before the test" \
	|| { bad "fleet unhealthy before the test (n1=$B1 n2=$B2 n3=$B3)"
	     echo "reshardtest: $pass passed, $fail failed"; exit 1; }

# ---- 1. shrink to {1,2} ---------------------------------------------
# Long enough for BOTH survivors to purge node3 (PEER_UP_MS 10000), or
# the fill below would be placed under three-member ownership and the
# rejoin would move nothing.
echo "  stopping pcnode3 and waiting for the fleet to shrink"
$RT kill pcnode3 >/dev/null 2>&1
i=0
while [ $i -lt 25 ]; do
	P1=$(peers 1); P2=$(peers 2)
	[ "$P1" = 1 ] && [ "$P2" = 1 ] && break
	sleep 1; i=$((i + 1))
done
[ "$P1" = 1 ] && [ "$P2" = 1 ] \
	&& ok "fleet shrank to 2 members in ${i}s" \
	|| bad "fleet did not shrink (n1=$P1 n2=$P2) - the fill would be
	     placed under the wrong ownership and nothing would move"

# ---- 2. fill under two-member ownership -----------------------------
# One perfcli session, N lines: a per-key exec would cost minutes and
# the write burst has to be short next to the fault window.
j=1
while [ $j -le "$N" ]; do
	echo "set $COL $R-$j v1-$j"
	j=$((j + 1))
done | cli 1 > /tmp/reshard-fill.$$ 2>&1
# `grep -c` exits 1 when the count is ZERO, so `|| echo 0` would append
# a second line and make this "0\n0" - which then fails "= 0" and kills
# the next arithmetic.  `|| true` keeps grep's own printed count.
FERR=$(/bin/grep -c '"error"' /tmp/reshard-fill.$$ 2>/dev/null || true)
FERR=${FERR:-0}
[ "$FERR" = 0 ] \
	&& ok "wrote $N keys under 2-member ownership" \
	|| bad "$FERR of $N writes failed during the fill"
# scoped to THIS run: the collection keeps earlier runs' keys, and an
# unscoped count made the printed line look like data loss that was not
PRE1=$(inv 1 | /bin/grep -c "^$R-" || true); PRE2=$(inv 2 | /bin/grep -c "^$R-" || true)
echo "  before the rejoin, of this run's $N keys: node1 holds ${PRE1:-0}, node2 holds ${PRE2:-0}"

# ---- 3. grow back, then mute the hand-over --------------------------
L0=$(mlost 1)
$RT start pcnode3 >/dev/null 2>&1
i=0
while [ $i -lt 30 ]; do
	[ "$(peers 1)" = 2 ] && break
	sleep 1; i=$((i + 1))
done
[ "$(peers 1)" = 2 ] \
	&& ok "node1 sees node3 again after ${i}s - ownership now includes it" \
	|| bad "node1 never saw node3 return - no hand-over would be owed"
part_mute pcnode1 pcnode3 || bad "could not mute node1 -> node3"
MR=$(part_rules pcnode3)
[ "${MR:-0}" -ge 1 ] \
	&& ok "the mute is really in place (node3 dropping on $MR rule(s))" \
	|| bad "no DROP rule on node3 - the fault was never applied"

# ---- 4. a newer write reaches the receiver, through node2 -----------
# node2 is unmuted in both directions, so each key lands on whichever
# node OWNS it now - including node3, the very peer node1 is trying and
# failing to hand the old copy to.
sleep 2
j=1
while [ $j -le "$N" ]; do
	echo "set $COL $R-$j v2-$j"
	j=$((j + 1))
done | cli 2 > /tmp/reshard-rewrite.$$ 2>&1
WERR=$(/bin/grep -c '"error"' /tmp/reshard-rewrite.$$ 2>/dev/null || true)
WERR=${WERR:-0}
echo "  rewrote through node2: $((N - WERR)) accepted, $WERR refused"

# let node1 keep trying (and keep failing) across several ticks
sleep 12
L1=$(mlost 1)
part_unmute pcnode1 pcnode3
PART_MUTES=""
sleep 12

LOST=$((L1 - L0))

# ---- 5. the inventory: what each node ACTUALLY holds ------------------
inv 1 > /tmp/reshard-n1.$$; inv 2 > /tmp/reshard-n2.$$; inv 3 > /tmp/reshard-n3.$$
echo "  after, of this run's keys: node1 $(/bin/grep -c "^$R-" /tmp/reshard-n1.$$ || true), node2 $(/bin/grep -c "^$R-" /tmp/reshard-n2.$$ || true), node3 $(/bin/grep -c "^$R-" /tmp/reshard-n3.$$ || true)"

python3 - "$R" "$N" /tmp/reshard-n1.$$ /tmp/reshard-n2.$$ /tmp/reshard-n3.$$ \
	> /tmp/reshard-verdict.$$ <<'PY'
import sys
run, n = sys.argv[1], int(sys.argv[2])
held = {}
for node, path in enumerate(sys.argv[3:], start=1):
    for line in open(path):
        k, _, v = line.rstrip("\n").partition(" ")
        if k.startswith(run + "-"):
            held.setdefault(k, []).append((node, v))
dup  = {k: w for k, w in held.items() if len(w) > 1}
fork = {k: w for k, w in dup.items() if len({v for _, v in w}) > 1}
stale = [k for k, w in held.items() if len(w) == 1 and w[0][1].startswith("v1-")]
print("DUP", len(dup))
print("FORK", len(fork))
print("STALE", len(stale))
print("SURVIVORS", len(held))
print("MISSING", n - len(held))
for k, w in list(fork.items())[:5]:
    print("FORKED", k, " ".join("node%d=%s" % (a, b) for a, b in w))
for k, w in list(dup.items())[:5]:
    print("DUPED", k, " ".join("node%d=%s" % (a, b) for a, b in w))
PY
V() { /bin/grep "^$1 " /tmp/reshard-verdict.$$ | awk '{print $2}'; }
DUP=$(V DUP); FORK=$(V FORK); STALE=$(V STALE)
SURV=$(V SURVIVORS); MISS=$(V MISSING)
echo "  survivors=$SURV missing=$MISS duplicated=$DUP forked=$FORK stale-v1=$STALE"

# ---- 6. the fault must have been REAL ---------------------------------
# Two independent signals, either of which proves hand-overs failed:
# node1's own migrate_lost counter, and keys that are simply gone (with
# a clean fill and no TTLs, a key can only vanish by being stubbed for a
# hand-over that never arrived).  Both at zero means the mute did not
# bite - the fleet was never disturbed and every assertion below would
# pass on an idle cluster.
if [ "$LOST" -gt 0 ] || [ "${MISS:-0}" -gt 0 ]; then
	ok "the hand-over really failed: migrate_lost +$LOST, $MISS key(s) gone"
	[ "${MISS:-0}" = 0 ] && [ "$LOST" -gt 0 ] && echo "       (nothing went
       missing because the concurrent write through node2 had already
       put a NEWER value on the new owner - which is exactly the case
       the invariant is written for: the discarded copy was stale)"
else
	bad "NOTHING failed: migrate_lost did not move ($L0 -> $L1) and no
	     key went missing - node1 never handed anything to node3, so
	     the fork-impossibility assertion below is vacuous"
fi

# THE assertion.  Not "no key was lost" - losing them is the design -
# but "no key is in two places", which is what stub-first buys.
if [ "$DUP" = 0 ]; then
	ok "no key exists on two nodes: the hand-over never forked"
else
	bad "$DUP key(s) exist on more than one node ($FORK with DIFFERENT
	     values) - a failed hand-over reinstated a copy the new owner
	     had already taken over"
	/bin/grep -E "^(FORKED|DUPED) " /tmp/reshard-verdict.$$ |
		sed 's/^/       /'
fi

# ---- 7. a lost key must read as an honest MISS ------------------------
# The other half of "a miss beats a fork": what the fleet lost, it says
# it lost - it does not serve a stale value in its place.
BADREAD=0; CHECKED=0
j=1
while [ $j -le "$N" ] && [ $CHECKED -lt 40 ]; do
	K=$R-$j
	if ! /bin/grep -q "^$K " /tmp/reshard-n1.$$ /tmp/reshard-n2.$$ \
	        /tmp/reshard-n3.$$ 2>/dev/null; then
		CHECKED=$((CHECKED + 1))
		G=$(cli1 2 "{\"method\":\"get\",\"params\":{\"col\":\"$COL\",\"key\":\"$K\"}}" |
			python3 -c 'import json,sys
try:
    r = json.load(sys.stdin); print(r.get("value") if r.get("found") else "MISS")
except Exception: print("ERR")' 2>/dev/null || echo ERR)
		[ "$G" = MISS ] || { BADREAD=$((BADREAD + 1))
			echo "       $K read back as '$G'"; }
	fi
	j=$((j + 1))
done
if [ $CHECKED -eq 0 ]; then
	ok "nothing was lost - every key survived the failed hand-over"
elif [ $BADREAD -eq 0 ]; then
	ok "all $CHECKED sampled lost key(s) read as an honest MISS"
else
	bad "$BADREAD of $CHECKED lost keys did NOT read as a miss - the
	     fleet is serving something for a key no node holds"
fi

# ---- 8. the collection still works ------------------------------------
i=0
while [ $i -lt 30 ]; do
	P1=$(peers 1); P2=$(peers 2); P3=$(peers 3)
	{ [ "$P1" = 2 ] && [ "$P2" = 2 ] && [ "$P3" = 2 ]; } && break
	sleep 1; i=$((i + 1))
done
cli1 1 "{\"method\":\"set\",\"params\":{\"col\":\"$COL\",\"key\":\"$R-after\",\"value\":\"served\"}}" >/dev/null
sleep 1
A=$(cli1 3 "{\"method\":\"get\",\"params\":{\"col\":\"$COL\",\"key\":\"$R-after\"}}" |
	python3 -c 'import json,sys
try:
    r = json.load(sys.stdin); print(r.get("value") if r.get("found") else "MISS")
except Exception: print("ERR")' 2>/dev/null || echo ERR)
[ "$A" = served ] \
	&& ok "the shard collection serves again after the fault (n1=$P1 n2=$P2 n3=$P3)" \
	|| bad "the shard collection does not serve after the fault (got $A)"

rm -f /tmp/reshard-fill.$$ /tmp/reshard-rewrite.$$ /tmp/reshard-verdict.$$ \
      /tmp/reshard-n1.$$ /tmp/reshard-n2.$$ /tmp/reshard-n3.$$
echo "reshardtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
