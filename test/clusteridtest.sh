#!/bin/sh
# clusteridtest.sh - S302: a cluster id, so two clusters never merge.
#
# Before S302 two fleets that shared a multicast group, a secret and a
# config were indistinguishable and folded into one, data and all.  Now
# the founder mints an id, members adopt it, every datagram carries it in
# its authenticated header, and an established id refuses every other.
#
#   1  cold start: three nodes with no id start together - ONE cluster,
#      one id (a UUID v7), established after the 30 s provisional
#      window (two founders of one cold start settle into one; they must
#      not split)
#   2  never merge: fleet B forms on its own group, outlives its window,
#      stops, and restarts on fleet A's group - same secret, same config.
#      They stay two; both count foreign datagrams.  (0.4.0: they fold.)
#   3  pinned ids: two fleets pinned to different ids on one group stay
#      two from the first second
#   4  a pin that disagrees with the state directory refuses the start
#   5  a FORCED cold-start race (two founders that cannot hear each other
#      until both have founded) ends as one cluster; then a member cut
#      from that master promotes behind the cut and the master steps
#      down to it - and nobody mints a new id.  (Shard mode with declared
#      collections, as standbypassovertest - where this was caught.)  The first S302 build
#      dropped a provisional id on every join request, so the master
#      re-founded under a new id its own members refused as foreign.
# Usage: test/clusteridtest.sh [./perfcached] [./netcutshim.so]
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./netcutshim.so}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
SANRT=$(ldd "$BIN" 2>/dev/null | awk "/libasan|libclang_rt\\.asan/ { print \$3; exit }")
D=$(mktemp -d /var/tmp/pccid.XXXXXX)
PIDS=""
cleanup() { for p in $PIDS; do kill -9 $p 2>/dev/null; done; rm -rf "$D"; }
trap cleanup EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

# each group its own port: a wildcard-bound socket on Linux hears every
# group joined on its port on the host (IP_MULTICAST_ALL), so two groups
# sharing a port on one host are not two networks
conf() { # conf <name> <n> <group> [pin]
	cat > "$D/$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/state-$1
[memory]
arena_mb = 16
[secrets]
client = cid-client-secret
cluster = cid-cluster-secret
[listen]
tcp = 127.0.0.1:$((18100 + 2 * $2))
http = 127.0.0.1:$((18101 + 2 * $2))
plaintext = loopback
[cluster]
multicast = $3:$(( 18199 - ${3##*.} + 95 ))
advertise = 127.0.5.$2
${4:+cluster_id = $4}
[collection 0]
buckets_log2 = 10
mode = eager
EOF
	chmod 600 "$D/$1.conf"
}
start() { # start <name> -> sets P
	"$BIN" -f "$D/$1.conf" >> "$D/$1.log" 2>&1 &
	P=$!; PIDS="$PIDS $P"
}
stat() { # stat <n> <cluster field>
	python3 -c 'import json,sys,urllib.request
d=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (18101 + 2 * int(sys.argv[1])),timeout=5).read())
print(d["cluster"].get(sys.argv[2], ""))' "$1" "$2" 2>/dev/null || echo "?"
}
members() { # members <n> -> live members this node sees, itself included
	python3 -c 'import json,sys,urllib.request
d=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/members" % (18101 + 2 * int(sys.argv[1])),timeout=5).read())
print(sum(1 for m in d.get("members",[]) if m.get("gone_s",-1) < 0))' "$1" 2>/dev/null || echo -1
}
nstate() { # nstate <n> -> the node's state (starting, ready, ...)
	python3 -c 'import json,sys,urllib.request
d=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (18101 + 2 * int(sys.argv[1])),timeout=5).read())
print(d.get("state", ""))' "$1" 2>/dev/null || echo "?"
}
wait_for() { # wait_for <secs> <shell condition>
	k=0
	while [ $k -lt $(($1 * 2)) ]; do
		eval "$2" && return 0
		sleep 0.5; k=$((k+1))
	done
	return 1
}

echo "--- 1: a cold start of three nodes with no id"
for n in 1 2 3; do conf c$n $n 239.255.77.95; done
for n in 1 2 3; do start c$n; eval "PC$n=$P"; done
wait_for 40 '[ "$(members 1)" = 3 ] && [ "$(members 2)" = 3 ] && [ "$(members 3)" = 3 ]' \
	&& ok "three nodes, one fleet" || bad "no single fleet ($(members 1) $(members 2) $(members 3))"
I1=$(stat 1 cluster_id); I2=$(stat 2 cluster_id); I3=$(stat 3 cluster_id)
[ -n "$I1" ] && [ "$I1" = "$I2" ] && [ "$I2" = "$I3" ] && ok "one cluster id on all three ($I1)" \
	|| bad "the ids differ or are missing ($I1 / $I2 / $I3)"
# a v7 UUID: version 7, RFC variant, and its first 48 bits are the
# founding time in Unix ms - within a minute of now
python3 -c 'import sys,time,uuid
u=uuid.UUID(sys.argv[1]); t=int(sys.argv[1].replace("-","")[:12],16)
sys.exit(0 if u.version==7 and u.variant==uuid.RFC_4122 and abs(t-time.time()*1000)<60000 else 1)' "$I1" 2>/dev/null \
	&& ok "the id is a UUID v7 stamped with its founding time" || bad "not a v7 UUID of now ($I1)"
wait_for 45 '[ "$(stat 1 cluster_id_state)" = established ] && [ "$(stat 3 cluster_id_state)" = established ]' \
	&& ok "the id is established after its provisional window" \
	|| bad "the id never became established ($(stat 1 cluster_id_state))"
[ -s "$D/state-c1/cluster-id" ] && grep -q "$I1" "$D/state-c1/cluster-id" \
	&& ok "and persisted in the state directory" || bad "no cluster-id file holding $I1"

echo "--- 2: a fleet founded elsewhere moves onto this group"
conf b4 4 239.255.77.96; conf b5 5 239.255.77.96
start b4; PB4=$P; start b5; PB5=$P
wait_for 30 '[ "$(members 4)" = 2 ] && [ "$(members 5)" = 2 ]' || bad "fleet B did not form on its own group"
IB=$(stat 4 cluster_id)
wait_for 45 '[ "$(stat 4 cluster_id_state)" = established ] && [ "$(stat 5 cluster_id_state)" = established ]' \
	|| bad "fleet B's id never became established"
kill $PB4 $PB5; wait $PB4 $PB5 2>/dev/null
conf b4 4 239.255.77.95; conf b5 5 239.255.77.95       # fleet A's group now
start b4; PB4=$P; start b5; PB5=$P
sleep 15
[ "$(members 1)" = 3 ] && [ "$(members 4)" = 2 ] && ok "they stay two fleets (3 and 2), never one of 5" \
	|| bad "the fleets merged or broke ($(members 1) / $(members 4))"
[ "$(stat 4 cluster_id)" = "$IB" ] && [ "$IB" != "$I1" ] && ok "each keeps its own id" \
	|| bad "an id changed ($(stat 4 cluster_id) vs $IB, A $I1)"
[ "$(stat 1 foreign_seen)" -gt 0 ] && [ "$(stat 4 foreign_seen)" -gt 0 ] && ok "both count the other's datagrams as foreign" \
	|| bad "foreign_seen not counted ($(stat 1 foreign_seen) / $(stat 4 foreign_seen))"
grep -q "it belongs to cluster $IB" "$D"/c*.log && ok "fleet A logs which cluster it is ignoring" \
	|| bad "no 'belongs to cluster' line on fleet A"
for p in $PC1 $PC2 $PC3 $PB4 $PB5; do kill $p 2>/dev/null; wait $p 2>/dev/null; done

echo "--- 3: two fleets pinned to different ids on one group"
conf p6 6 239.255.77.97 alpha; conf p7 7 239.255.77.97 alpha; conf p8 8 239.255.77.97 beta
start p6; P6=$P; start p7; P7=$P; start p8; P8=$P
wait_for 30 '[ "$(members 6)" = 2 ] && [ "$(members 7)" = 2 ]' && sleep 4
[ "$(members 6)" = 2 ] && [ "$(members 8)" = 1 ] && ok "alpha is two, beta is one - never three" \
	|| bad "pinned fleets mixed ($(members 6) / $(members 8))"
[ "$(stat 8 cluster_id_state)" = established ] && ok "a pinned id is established from the start" \
	|| bad "pinned id state is $(stat 8 cluster_id_state)"
for p in $P6 $P7 $P8; do kill $p 2>/dev/null; wait $p 2>/dev/null; done

echo "--- 4: a pin that disagrees with the state directory"
conf p6 6 239.255.77.97 gamma
"$BIN" -f "$D/p6.conf" > "$D/p6b.log" 2>&1 & P=$!; PIDS="$PIDS $P"
k=0; while [ $k -lt 100 ] && kill -0 $P 2>/dev/null; do sleep 0.1; k=$((k+1)); done
if kill -0 $P 2>/dev/null; then bad "it started"; kill -9 $P
else wait $P; ok "the start is refused (rc $?)"; fi
grep -q "belongs to another cluster" "$D/p6b.log" && ok "and says the directory belongs to another cluster" \
	|| bad "no 'belongs to another cluster' message"
python3 -c 'import sys,uuid
u=uuid.UUID(open(sys.argv[1]).read().split()[-1]); sys.exit(0 if u.version==8 and u.variant==uuid.RFC_4122 else 1)' \
	"$D/state-p6/cluster-id" 2>/dev/null && ok "a pinned name is held as a UUID v8" || bad "the pinned name is not a v8 UUID"

echo "--- 5: a forced cold-start race, then a step-down inside the cluster"
if [ ! -f "$SHIM" ]; then
	bad "no netcut shim at $SHIM - part 5 not run"
else
	# shard mode with declared collections, as standbypassovertest runs
	# (where this was caught)
	for n in 9 10 11; do
		conf r$n $n 239.255.77.98
		sed -i -e 's/^\[collection 0\]$/[collection c]/' -e '/^mode = eager$/d' \
			-e 's/^\[cluster\]$/[cluster]\nmode = shard\ncollections = c/' "$D/r$n.conf"
	done
	echo "cut 127.0.5.11" > "$D/cut9"; echo "cut 127.0.5.9" > "$D/cut11"; : > "$D/cut10"
	for n in 9 10 11; do
		LD_PRELOAD="${SANRT:+$SANRT }$SHIM" PC_NETCUT_CTL="$D/cut$n" PC_NETCUT_LOG="$D/cutlog$n" \
			"$BIN" -f "$D/r$n.conf" >> "$D/r$n.log" 2>&1 &
		P=$!; PIDS="$PIDS $P"; eval "PR$n=$P"
	done
	wait_for 20 'grep -q "founded the cluster" "$D/r9.log" && grep -q "founded the cluster" "$D/r11.log"' \
		&& ok "the race is forced: nodes 9 and 11 both founded" || bad "the race was not forced"
	: > "$D/cut9"; : > "$D/cut11"
	wait_for 30 '[ "$(members 9)" = 3 ] && [ "$(members 10)" = 3 ] && [ "$(members 11)" = 3 ] && [ "$(stat 9 cluster_id)" = "$(stat 11 cluster_id)" ]' \
		&& ok "it ends as one cluster with one id ($(stat 9 cluster_id))" \
		|| bad "the race did not settle ($(members 9) $(members 10) $(members 11); $(stat 9 cluster_id) / $(stat 11 cluster_id))"
	# all ready before the cut, so the promotion behind it is the ordinary one
	wait_for 30 '[ "$(nstate 9)$(nstate 10)$(nstate 11)" = readyreadyready ]' \
		|| bad "the three nodes never all reported ready ($(nstate 9) $(nstate 10) $(nstate 11))"
	M=; for n in 9 10 11; do [ "$(stat $n role)" = master ] && M=$n; done
	# cut the member with the HIGHEST address: a member whose master goes
	# silent promotes only if no live peer it still sees has a higher
	# address (membership_tick), so this one promotes behind the cut and
	# the master then steps down to its higher term without being able
	# to reach it
	B=; for n in 9 10 11; do [ "$n" != "$M" ] && B=$n; done
	F0=$(cat "$D"/r*.log | grep -c "founded cluster")
	S0=$(grep -c "stepping down - node [0-9]* holds term" "$D/r$M.log")
	# it loses the master AND the other member: since S36 e3 a member
	# that loses only its master asks the others first, and holds while
	# one still hears it - cut from everyone, nobody vouches, it promotes
	O=; for n in 9 10 11; do [ "$n" != "$M" ] && [ "$n" != "$B" ] && O=$n; done
	echo "cut 127.0.5.$M 127.0.5.$O" > "$D/cut$B"
	sleep 15
	: > "$D/cut$B"
	wait_for 30 '[ "$(members 9)" = 3 ] && [ "$(members 10)" = 3 ] && [ "$(members 11)" = 3 ] && [ "$(stat 9 cluster_id)" = "$(stat 10 cluster_id)" ] && [ "$(stat 10 cluster_id)" = "$(stat 11 cluster_id)" ]' \
		&& ok "healed: one cluster of three, one id" \
		|| bad "not one cluster after the heal ($(members 9) $(members 10) $(members 11))"
	F1=$(cat "$D"/r*.log | grep -c "founded cluster")
	[ "$(grep -c "stepping down - node [0-9]* holds term" "$D/r$M.log")" -gt "$S0" ] \
		&& ok "the step-down happened: the master yielded to the cut member's higher term" \
		|| bad "the step-down was NOT exercised (the cut member never promoted) - the next check proves nothing"
	[ -n "$M" ] && [ "$F1" = "$F0" ] && ok "no node minted a new id across the step-down (master $M, cut member $B)" \
		|| { bad "a node minted a new id across the step-down ($F0 -> $F1 'founded cluster' lines, master ${M:-?})"; grep -h "founded cluster\|stepping down\|yielding" "$D"/r*.log | tail -6; }
	for n in 9 10 11; do eval "kill \$PR$n" 2>/dev/null; done
fi

echo "clusteridtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
