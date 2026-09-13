#!/bin/sh
# spreadtest.sh — S127 Z2: `mode = spread` places each record on K nodes.
#
# THE CLAIM, and the only one this makes: a write reaches its K HOLDERS
# and nobody else.  That is the whole reason the mode exists.  Under
# eager every node applies every write in the fleet, so per-node apply
# equals the fleet's total write rate and one node's apply path caps the
# fleet however many nodes are added.  Under a copy factor the passive
# work is (K-1)/P per node and FALLS as the fleet grows.  If the push
# still went to every peer, the mode would be eager with extra config.
#
# HOW IT IS ASSERTED, and why not with `get`: a get on a non-holder
# PULLS from a holder and keeps the copy, so reading the keys back is
# the one thing that destroys the property under test.  Per-collection
# `entries` from /stats is pull-free, so the fleet-wide SUM of entries
# is the observable: K*N under spread, P*N under eager.
#
# EAGER IS RUN AS THE CONTROL, in the same harness with the same keys.
# Without it a broken push that dropped records would also read as
# "fewer copies than eager" and pass.
#
# Usage: test/spreadtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcspread.XXXXXX)
P1= P2= P3= P4= P5=
trap 'for v in "$P1" "$P2" "$P3" "$P4" "$P5"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

NKEYS=120
MC=${MC:-239.255.77.44}
MP=${MP:-17144}

node() { # node <n> <cli-port> <http-port> <cluster-block>
	rm -rf "$D/s$1"                # no carry-over between arms
	mkdir -p "$D/s$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = info
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = spread-client-secret
cluster = spread-cluster-secret
[listen]
tcp = 127.0.0.1:$2
http = 127.0.0.1:$3
plaintext = loopback
[cluster]
multicast = $MC:$MP
advertise = 127.0.7.$1
pull_timeout_ms = 200
$4
collections = b
[collection b]
buckets_log2 = 12
pull = 1
EOF
}
start() { # start <n> <var>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$2=\$!"
	i=0
	while [ $i -lt 120 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "  node $1 did not start:"; tail -4 "$D/n$1.log"; return 1
}
stopall() {
	for v in "$P1" "$P2" "$P3" "$P4" "$P5"; do
		[ -n "$v" ] && kill -9 "$v" 2>/dev/null
	done
	P1= P2= P3= P4= P5=
	sleep 0.5
}
cli() { printf '%s\n' "$2" | timeout 8 python3 -c '
import json, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=6)
f = s.makefile("rwb")
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    r = json.loads(line); r["id"] = 1; r["jsonrpc"] = "2.0"
    f.write(json.dumps(r).encode() + b"\n"); f.flush()
    d = json.loads(f.readline())
    print(json.dumps(d.get("result", d.get("error"))))' "$1"; }

entries() { # entries <http-port>  -> entries in collection b, or empty
	timeout 6 python3 -c '
import json, sys, urllib.request
try:
    d = json.load(urllib.request.urlopen("http://127.0.0.1:%s/stats" % sys.argv[1], timeout=4))
except Exception:
    sys.exit(0)
for c in d.get("collections", []):
    if c.get("name") == "b":
        print(c.get("entries", 0)); break' "$1" 2>/dev/null
}

cstat() { # cstat <http-port> <cluster field>
	timeout 6 python3 -c '
import json, sys, urllib.request
try:
    d = json.load(urllib.request.urlopen("http://127.0.0.1:%s/stats" % sys.argv[1], timeout=4))
except Exception:
    print(0); sys.exit(0)
print(d.get("cluster", {}).get(sys.argv[2], 0))' "$1" "$2" 2>/dev/null || echo 0
}
served_sum() { # fleet-wide pull requests RECEIVED
	a=$(cstat 18401 pull_served); b=$(cstat 18402 pull_served)
	c=$(cstat 18403 pull_served); d=$(cstat 18404 pull_served)
	echo $(( ${a:-0} + ${b:-0} + ${c:-0} + ${d:-0} ))
}

ready4() { # every node ready and seeing 3 peers
	i=0
	while [ $i -lt 150 ]; do
		a=$(cli 17401 '{"method":"stats"}' | python3 -c \
			'import json,sys; d=json.load(sys.stdin); print(d["cluster"]["peers_up"], d["state"])' 2>/dev/null)
		[ "$a" = "3 ready" ] && return 0
		sleep 0.2; i=$((i+1))
	done
	return 1
}

# writes NKEYS through node 1 and returns the fleet-wide sum of entries
run_arm() { # run_arm <cluster-block> <label>
	node 1 17401 18401 "$1"; node 2 17402 18402 "$1"
	node 3 17403 18403 "$1"; node 4 17404 18404 "$1"
	start 1 1 || return 1
	start 2 2 || return 1
	start 3 3 || return 1
	start 4 4 || return 1
	ready4 || { echo "  $2: fleet never formed"; return 1; }
	i=0
	while [ $i -lt $NKEYS ]; do
		printf '{"method":"set","params":{"col":"b","key":"sk%03d","value":"v%03d","ttl":600}}\n' $i $i
		i=$((i+1))
	done | timeout 30 python3 -c '
import json, socket, sys
s = socket.create_connection(("127.0.0.1", 17401), timeout=10)
f = s.makefile("rwb")
n = 0
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    r = json.loads(line); r["id"] = 1; r["jsonrpc"] = "2.0"
    f.write(json.dumps(r).encode() + b"\n"); f.flush()
    f.readline(); n += 1
print("wrote", n, file=sys.stderr)' 2>/dev/null
	# WAIT FOR THE COUNT TO SETTLE, do not sleep a fixed time.  The
	# gather groups flush on a timer and the repair sweep runs on its
	# own period, so a fixed wait samples a moving number: the same arm
	# read 240 once and 283 a moment earlier, and neither was wrong -
	# the measurement was just taken mid-convergence.
	prev=-1; stable=0; w=0
	while [ $w -lt 60 ]; do
		E1=$(entries 18401); E2=$(entries 18402)
		E3=$(entries 18403); E4=$(entries 18404)
		SUM=$(( ${E1:-0} + ${E2:-0} + ${E3:-0} + ${E4:-0} ))
		if [ "$SUM" = "$prev" ] && [ "$SUM" -gt 0 ]; then
			stable=$((stable+1))
			[ $stable -ge 5 ] && break
		else
			stable=0
		fi
		echo "    [$2 t=${w}s] $E1/$E2/$E3/$E4 sum=$SUM"
		prev=$SUM
		sleep 2; w=$((w+1))
	done
	[ $stable -ge 5 ] || echo "  $2: WARNING count never settled (last $SUM)"
	echo "  $2: per-node entries ${E1:-?}/${E2:-?}/${E3:-?}/${E4:-?}  sum=$SUM"
	return 0                       # the CALLER stops the fleet: the
}                                      # read-back below needs it alive

# ---- arm A: the CONTROL.  eager must put every key on every node -----
run_arm "mode = eager" "eager(control)" || { echo "eager arm failed"; exit 1; }
EAGER=$SUM
WANT=$(( NKEYS * 4 ))
[ "$EAGER" -ge $(( WANT * 90 / 100 )) ] && ok "eager holds ~4 copies of each key (sum $EAGER, want ~$WANT)" \
	|| bad "eager sum $EAGER, expected ~$WANT - the CONTROL is broken, so the spread arm proves nothing"
stopall

# ---- arm B: spread K=2 -----------------------------------------------
run_arm "mode = spread
replicas = 2" "spread K=2" || { echo "spread arm failed"; exit 1; }
K2=$SUM
# THE NON-WRITER NODES are where the property is asserted, and the
# arithmetic is worth writing down.  With K=2 of P=4 and every write
# entering at node 1: node 1 is a holder for about half the keys, so of
# the 2N copies the fleet owes, about N/2 land on node 1 and about
# 3N/2 land on nodes 2-4.  For N=120 that is ~180 across the three.
OTHERS=$(( ${E2:-0} + ${E3:-0} + ${E4:-0} ))
WANT_OTHERS=$(( NKEYS * 3 / 2 ))
LO=$(( WANT_OTHERS * 75 / 100 )); HI=$(( WANT_OTHERS * 125 / 100 ))
[ "$OTHERS" -ge "$LO" ] && [ "$OTHERS" -le "$HI" ] \
	&& ok "the non-writing nodes hold their K=2 share ($OTHERS, want ~$WANT_OTHERS)" \
	|| bad "non-writing nodes hold $OTHERS, expected $LO..$HI"

# no single non-writer may hold the whole keyspace, and none may hold
# nothing: either would be placement collapsing rather than spreading
for e in "${E2:-0}" "${E3:-0}" "${E4:-0}"; do
	[ "$e" -gt $(( NKEYS / 5 )) ] && [ "$e" -lt $(( NKEYS * 9 / 10 )) ] \
		&& ok "a non-writer holds a share, not all and not none ($e of $NKEYS)" \
		|| bad "a non-writer holds $e of $NKEYS - placement collapsed"
done

# EXACT-K RETENTION, decided 2026-09-13.  A node that is not in the
# top-K set may ACCEPT and FORWARD a write - any-member write is about
# admission, not placement - but must not keep it.  Every client here
# writes through node 1, so if non-holders retained, node 1 would hold
# all N; it must instead hold only its placement share, ~N*K/P.
WANT_W=$(( NKEYS * 2 / 4 ))
WLO=$(( WANT_W * 60 / 100 )); WHI=$(( WANT_W * 140 / 100 ))
[ "${E1:-0}" -ge "$WLO" ] && [ "${E1:-0}" -le "$WHI" ] \
	&& ok "the writer keeps only its share, not everything (${E1} of $NKEYS, want ~$WANT_W)" \
	|| bad "the writer holds ${E1:-?} of $NKEYS, expected $WLO..$WHI - non-holders are retaining"

# EXACTLY K*N, now that the settled reclaim pass drops surplus copies.
#
# This was a lower bound with a TODO(S127-reclaim) while the converged
# total was 240 or 283 depending on how membership happened to
# converge.  Surplus was never merely wasted memory: a non-holder is
# never sent subsequent writes (pc_repl_push targets the set) and a
# local hit is served without consulting anyone (op_get_miss runs only
# on a miss), so a surplus copy could serve STALE data until its TTL.
#
# Reclaim is a LOCAL, settled-only policy, not a receive-time refusal:
# a receiver whose view has not converged refuses copies it should
# hold, which was measured at 212 where 240 was owed.
WANT_T=$(( NKEYS * 2 ))
[ "$K2" = "$WANT_T" ] \
	&& ok "the fleet holds EXACTLY K*N copies (sum $K2)" \
	|| bad "fleet holds $K2, owes exactly $WANT_T ($([ "$K2" -gt "$WANT_T" ] && echo "surplus $(( K2 - WANT_T )) not reclaimed" || echo "UNDER-replicated"))"

# The differential is the assertion that cannot be faked by a push that
# simply drops records: eager and spread ran the same keys through the
# same harness, and only the copy count differs.
[ "$K2" -lt "$EAGER" ] && ok "spread stores strictly fewer copies than eager ($K2 < $EAGER)" \
	|| bad "spread stored $K2 and eager $EAGER - the mode changed nothing"

# ---- DROPPING MUST NOT LOSE DATA -------------------------------------
# Node 1 accepted every write and kept only ~half.  Every key must still
# be readable THROUGH IT: the ones it holds locally, the ones it has to
# pull from a holder.  Counting copies proves the bound; this proves the
# bound was achieved by placing records rather than by losing them.
# It runs LAST because a get on a non-holder pulls and keeps a copy,
# which would corrupt the entry counts above.
SERVED0=$(served_sum)
MISS=$(i=0; while [ $i -lt $NKEYS ]; do
	printf '{"method":"get","params":{"col":"b","key":"sk%03d"}}\n' $i
	i=$((i+1))
done | timeout 40 python3 -c '
import json, socket, sys
s = socket.create_connection(("127.0.0.1", 17401), timeout=10)
f = s.makefile("rwb")
miss = 0
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    r = json.loads(line); r["id"] = 1; r["jsonrpc"] = "2.0"
    f.write(json.dumps(r).encode() + b"\n"); f.flush()
    d = json.loads(f.readline()).get("result", {})
    if not isinstance(d, dict) or not d.get("found"):
        miss += 1
print(miss)' 2>/dev/null)
[ "${MISS:-999}" = "0" ] \
	&& ok "all $NKEYS keys still readable through the node that dropped half" \
	|| bad "${MISS:-?} of $NKEYS keys are GONE - dropping lost data"

# ---- READ AMPLIFICATION: a miss asks ONE holder, not everybody -------
# pull_served counts every PULL_REQ a node RECEIVES, found or not.  A
# broadcast miss therefore lands on all P-1 peers and a unicast miss on
# exactly one, so the fleet-wide delta over a known number of misses is
# the amplification factor - measured, not inferred from the code path.
SERVED1=$(served_sum)
DELTA=$(( SERVED1 - SERVED0 ))
LOCALMISS=$(( NKEYS - ${E1:-0} ))
echo "  reads: $NKEYS through node 1, ~$LOCALMISS of them local misses; pull_served delta $DELTA"
# broadcast on four nodes would be ~3x the misses; unicast is ~1x.
[ "$DELTA" -le $(( LOCALMISS * 2 )) ] \
	&& ok "a spread miss asks ONE holder (delta $DELTA for ~$LOCALMISS misses, broadcast would be ~$(( LOCALMISS * 3 )))" \
	|| bad "delta $DELTA for ~$LOCALMISS misses - the miss is still broadcasting"

# ---- EXACT-K MUST SURVIVE READS --------------------------------------
# The read-back above pulled ~53 records THROUGH a node that is not
# their holder.  If the pull path keeps them, the same orphan the write
# path was taught to avoid comes back through the read door: node 1
# would climb from its placement share to the whole keyspace, and the
# fleet from K*N to something unbounded by K.
A1=$(entries 18401); A2=$(entries 18402)
A3=$(entries 18403); A4=$(entries 18404)
ASUM=$(( ${A1:-0} + ${A2:-0} + ${A3:-0} + ${A4:-0} ))
echo "  after reading all $NKEYS through node 1: ${A1}/${A2}/${A3}/${A4} sum=$ASUM (was $K2)"
[ "${A1:-0}" -le $(( ${E1:-0} + NKEYS / 10 )) ] \
	&& ok "reading through a non-holder did not turn it into one (${A1} vs ${E1} before)" \
	|| bad "node 1 climbed ${E1} -> ${A1} by READING - the pull path retains on non-holders"

# ---- THE ROUTING CONTRACT THE CLIENT READS ---------------------------
# libperfd only routes for a mode it RECOGNISES and needs the copy
# factor to know how many holders a key has.  Both travel in the stats
# `routing` block, so if this is wrong every client silently stops
# routing and pays a pull per read - correct, but the performance floor
# S127 warned about.  (End-to-end client routing needs the Noise
# handshake, which this harness cannot do with plaintext = loopback;
# that arm belongs with Z6's fleet work.)
RJ=$(cli 17401 '{"method":"members"}' | timeout 6 python3 -c '
import json, sys
d = json.load(sys.stdin)
r = d.get("routing", {}) if isinstance(d, dict) else {}
print("%s %s %s" % (r.get("mode"), r.get("replicas"), r.get("algo")))' 2>/dev/null)
set -- $RJ
[ "$1" = "spread" ] && ok "routing.mode is published as spread" \
	|| bad "routing.mode is [$1] - an up-to-date client will not route"
[ "$2" = "2" ] && ok "routing.replicas is published as K=2" \
	|| bad "routing.replicas is [$2] - a client cannot know how many holders"

# ---- THE GRACE PERIOD: a blip must not re-replicate the keyspace -----
# Kill one node and watch the SURVIVORS.  Without a grace, the dead
# node leaves the top-K set the moment it misses a heartbeat, a new
# holder is promoted for every key it held, and the repair sweep copies
# them there - a rebuild storm paid for a node that is coming back.
# With the grace its slots are held, so the survivors' total must NOT
# climb.  The trade is explicit: during the grace those keys have K-1
# reachable copies instead of K.
SURV0=$(( ${A1:-0} + ${A2:-0} + ${A3:-0} ))
echo "  killing node 4 (held ${A4}); survivors hold $SURV0 (repairs so far: $(cstat 18401 spread_repaired)/$(cstat 18402 spread_repaired)/$(cstat 18403 spread_repaired))"
kill -9 "$P4" 2>/dev/null; P4=
w=0
while [ $w -lt 8 ]; do
	sleep 2; w=$((w+1))
done
G1=$(entries 18401); G2=$(entries 18402); G3=$(entries 18403)
SURV1=$(( ${G1:-0} + ${G2:-0} + ${G3:-0} ))
echo "  16s after the kill: ${G1}/${G2}/${G3} = $SURV1 (was $SURV0), repairs $(cstat 18401 spread_repaired)/$(cstat 18402 spread_repaired)/$(cstat 18403 spread_repaired)"
# a storm would move node 4's share onto the survivors
[ "$SURV1" -le $(( SURV0 + NKEYS / 10 )) ] \
	&& ok "a blip did not re-replicate: survivors $SURV0 -> $SURV1 (a storm would add ~${A4})" \
	|| bad "survivors climbed $SURV0 -> $SURV1 - the grace is not holding the dead node's slots"

# The counter, asserted directly rather than inferred from totals.  This
# is what locks the footgun: peer_gone() zeroes ->node, and a
# fingerprint that includes the node id therefore "changes" while the
# grace is still holding the slot - arming a repair inside the window
# the grace exists to create.  The fingerprint must hash what PLACEMENT
# hashes (advertise address + cluster port), so it moves only when the
# set moves.  Totals alone could not tell this apart from noise.
REP=$(( $(cstat 18401 spread_repaired) + $(cstat 18402 spread_repaired) + $(cstat 18403 spread_repaired) ))
[ "$REP" = "0" ] \
	&& ok "no repair was armed inside the grace (the set fingerprint held)" \
	|| bad "$REP repair(s) armed inside the grace - the fingerprint is moving on something that is not the set"

# ---- PAST THE GRACE: the set legitimately changes ---------------------
# The grace holds a quiet node's slots for 30s past the liveness window.
# Once it expires the departure is real, node 4's slots must go to the
# survivors, and the fleet must return to K*N over three nodes.  This is
# MEASURED rather than assumed: it tells us whether the existing repair
# sweep already re-places the affected keys or whether Z4 must.
w=0
while [ $w -lt 14 ]; do
	sleep 4; w=$((w+1))
	H1=$(entries 18401); H2=$(entries 18402); H3=$(entries 18403)
	SURV2=$(( ${H1:-0} + ${H2:-0} + ${H3:-0} ))
	echo "    [post-grace t=$((w*4))s] ${H1}/${H2}/${H3} = $SURV2"
	[ "$SURV2" -ge $(( NKEYS * 2 * 90 / 100 )) ] && break
done
WANT3=$(( NKEYS * 2 ))
echo "  after the grace: ${H1}/${H2}/${H3} = $SURV2 (want ~$WANT3 = K*N on three nodes)"
[ "$SURV2" -ge $(( WANT3 * 85 / 100 )) ] \
	&& ok "a real departure re-placed the keys: survivors $SURV0 -> $SURV2" \
	|| bad "survivors stuck at $SURV2, want ~$WANT3 - a departed node's keys were NOT re-placed"

# ---- THE NODE RETURNS: surplus must be RECLAIMED ---------------------
# This is the case the whole reclaim pass exists for, and it is the
# realistic one - a node is away and comes back, which is why its id is
# reserved for a week.  While it was gone its keys were re-placed onto
# survivors.  When it returns it is a holder again, those survivors are
# NOT, and their copies are surplus: outside the set, so the repair
# sweep neither maintains nor reclaims them, and a local hit would
# serve them without ever being updated again.
#
# Nothing before this arm exercised reclaim at all: a clean convergence
# produces no surplus, so the exact-K*N assertion above can pass without
# the pass ever running.  This is the arm that makes it mean something.
echo "  restarting node 4"
start 4 4 || bad "node 4 did not come back"
w=0
while [ $w -lt 30 ]; do
	sleep 4; w=$((w+1))
	R1=$(entries 18401); R2=$(entries 18402)
	R3=$(entries 18403); R4=$(entries 18404)
	RSUM=$(( ${R1:-0} + ${R2:-0} + ${R3:-0} + ${R4:-0} ))
	RECL=$(( $(cstat 18401 spread_reclaimed) + $(cstat 18402 spread_reclaimed) \
		+ $(cstat 18403 spread_reclaimed) + $(cstat 18404 spread_reclaimed) ))
	echo "    [return t=$((w*4))s] ${R1}/${R2}/${R3}/${R4} = $RSUM reclaimed=$RECL"
	[ "$RSUM" = "$(( NKEYS * 2 ))" ] && [ "$RECL" -gt 0 ] && break
done
echo "  after the return: ${R1}/${R2}/${R3}/${R4} = $RSUM, reclaimed $RECL"
[ "$RECL" -gt 0 ] \
	&& ok "surplus copies were reclaimed after the node returned ($RECL)" \
	|| bad "nothing was reclaimed - the settled pass never ran, so exact K*N above proves nothing"
[ "$RSUM" = "$(( NKEYS * 2 ))" ] \
	&& ok "the fleet is back to EXACTLY K*N with the node returned ($RSUM)" \
	|| bad "fleet holds $RSUM, owes exactly $(( NKEYS * 2 ))"

# ---- K=3 on the same fixture -----------------------------------------
# S127 asks for K=2 and K=3.  The copy count is the property that must
# track K: the same keys through the same harness must cost 3N copies
# where K=2 cost 2N, or `replicas` is not doing what its name says.
stopall
run_arm "mode = spread
replicas = 3" "spread K=3" || { echo "K=3 arm failed"; exit 1; }
# the settle wait inside run_arm stops when the COUNT stops moving, and
# a surplus is perfectly stable - so wait for reclaim to have run too
WANT_K3=$(( NKEYS * 3 ))
w=0
while [ $w -lt 30 ]; do
	Q1=$(entries 18401); Q2=$(entries 18402)
	Q3=$(entries 18403); Q4=$(entries 18404)
	SUM=$(( ${Q1:-0} + ${Q2:-0} + ${Q3:-0} + ${Q4:-0} ))
	[ "$SUM" = "$WANT_K3" ] && break
	sleep 4; w=$((w+1))
done
echo "  K=3 after reclaim: ${Q1}/${Q2}/${Q3}/${Q4} = $SUM"
K3=$SUM
[ "$K3" = "$WANT_K3" ] \
	&& ok "K=3 holds exactly 3N copies (sum $K3)" \
	|| bad "K=3 holds $K3, owes exactly $WANT_K3"
# and the copy count must actually TRACK K, not merely be large
[ "$K3" -gt "$K2" ] \
	&& ok "K=3 stores more than K=2 ($K3 > $K2), so replicas is load-bearing" \
	|| bad "K=3 stored $K3 and K=2 stored $K2 - replicas changes nothing"
stopall

# ---- FIVE NODES, K=2 and K=3 -----------------------------------------
# S127 asks for three- and five-node fleets at both copy factors.  Five
# is not four plus one for a reason worth testing: with P=5 and K=2 the
# writer is a holder for only 2/5 of the keyspace, so most writes are
# accepted by a node that must forward and drop - the exact-K retention
# path runs on the majority of writes rather than on half of them.
five() { # five <K>
	stopall
	block="mode = spread
replicas = $1"
	# NOT `i`: start() uses i as its own wait counter and POSIX sh has
	# no locals, so looping `start $i $i` clobbers the index mid-loop.
	# The symptom was three nodes never restarting while their STALE
	# logs still said "ready", and a fleet reporting 0 peers.
	nn=1
	while [ $nn -le 5 ]; do
		node $nn $((17400 + nn)) $((18400 + nn)) "$block"
		nn=$((nn + 1))
	done
	nn=1
	while [ $nn -le 5 ]; do
		start $nn $nn || { bad "five/K=$1: node $nn did not start"; return 1; }
		nn=$((nn + 1))
	done
	# every node ready and seeing four peers
	j=0
	while [ $j -lt 200 ]; do
		r=$(cli 17401 '{"method":"stats"}' | python3 -c \
			'import json,sys; d=json.load(sys.stdin); print(d["cluster"]["peers_up"], d["state"])' 2>/dev/null)
		[ "$r" = "4 ready" ] && break
		sleep 0.2; j=$((j + 1))
	done
	if [ "$r" != "4 ready" ]; then
		bad "five/K=$1: fleet never formed ($r)"
		for L in 1 2 3 4 5; do
			echo "    --- node $L ---"
			grep -iE "error|warn|refus|bind|multicast|joined|ready" "$D/n$L.log" 2>/dev/null | tail -4 | sed 's/^/      /'
		done
		return 1
	fi
	i=0
	while [ $i -lt $NKEYS ]; do
		printf '{"method":"set","params":{"col":"b","key":"fk%03d","value":"v%03d","ttl":600}}\n' $i $i
		i=$((i + 1))
	done | timeout 40 python3 -c '
import json, socket, sys
s = socket.create_connection(("127.0.0.1", 17401), timeout=10)
f = s.makefile("rwb")
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    r = json.loads(line); r["id"] = 1; r["jsonrpc"] = "2.0"
    f.write(json.dumps(r).encode() + b"\n"); f.flush(); f.readline()' 2>/dev/null
	# wait for the copy count to reach K*N and hold - reclaim has to run
	want=$(( NKEYS * $1 ))
	j=0
	while [ $j -lt 40 ]; do
		t=0
		for pnum in 18401 18402 18403 18404 18405; do
			e=$(entries $pnum); t=$(( t + ${e:-0} ))
		done
		[ "$t" = "$want" ] && break
		sleep 3; j=$((j + 1))
	done
	echo "  five nodes K=$1: sum=$t (owed $want)"
	[ "$t" = "$want" ] \
		&& ok "five nodes at K=$1 hold exactly K*N ($t)" \
		|| bad "five nodes at K=$1 hold $t, owe exactly $want"
	stopall
}
five 2
five 3

echo "spreadtest: $pass passed, $fail failed"
[ $fail -eq 0 ] || exit 1
exit 0
