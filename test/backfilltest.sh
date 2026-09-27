#!/bin/sh
# backfilltest.sh — S81: a node that restarts empty must be backfilled
# even when the designated sender changes hands mid-way.
#
# The production shape (rolling restart, 2026-09-04): the empty node
# rejoins while the lowest-id survivor is still live, so a HIGHER
# survivor is not the sender and runs one ORDINARY sweep cycle to it -
# no copies sent - and advances its mark.  The lowest survivor dies
# before its own tick.  The higher one inherits the role and walks from
# the advanced mark, which discards every passive copy on wtick <=
# since, walks "clean", clears the flag and logs "backfilled".  The
# empty node then holds the fleet's NEW writes and none of its old
# records, for ever - and reads still succeed through pull-on-miss, so
# nothing alerts.
#
# Reproduced with SIGSTOP on the sender (it stays "live" for PEER_UP_MS
# and cannot tick).  ORDER MATTERS: the empty node must rejoin and be
# given its id by the still-live master BEFORE the sender is frozen -
# the lowest id is the master, and freezing it first stalls the join
# until a new master is elected, by which time the sender is already
# dead to everyone and the role never moves.  The case is then DETECTED
# rather than assumed: a
# marker the higher node AUTHORED arriving on the empty node proves its
# ordinary cycle ran; the passive record missing beside it proves that
# cycle was not a backfill.  Only then is the role transfer awaited.
# An attempt where both arrive together (the higher node's tick landed
# after the sender died) is ambiguous and is retried.
#
# S83 (2026-09-07): the empty node now PULLS its bootstrap from the lowest
# ready peer holding records, and the push walk is the fallback (exercised
# by boottest).  The two scenarios below keep their shape - the sender
# frozen mid-way, two restarts inside one sweep - but assert the property
# the pull gives: a node that reports ready holds the keyspace, says from
# whom it got it, and serves it locally.
# Usage: test/backfilltest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcbf.XXXXXX)
P1= P2= P3=
trap 'for p in $P1 $P2 $P3; do kill -CONT $p 2>/dev/null; kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

node() { # node <n> <cliport>  (advertises 127.0.1.2<n>)
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = bf-client-secret
cluster = bf-cluster-secret
[listen]
tcp = 127.0.0.1:$2
plaintext = loopback
[cluster]
multicast = 239.255.77.48:17148
advertise = 127.0.1.2$1
pull_timeout_ms = 300
[collection eg]
buckets_log2 = 12
pull = 1
mode = eager
EOF
}
start() { # start <n>  (truncates the log: readiness must be THIS run's)
	: > "$D/n$1.log"
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$1=\$!"
	i=0
	while [ $i -lt 60 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; tail -5 "$D/n$1.log"; exit 1
}
CLI="$D/cli.py"
cat > "$CLI" <<'EOF'
import json, socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=8)
f = s.makefile("rwb"); rid = 0
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    rid += 1
    req = json.loads(line); req["id"] = rid; req["jsonrpc"] = "2.0"
    f.write(json.dumps(req).encode()+b"\n"); f.flush()
    r = json.loads(f.readline())
    print(json.dumps(r.get("result", r.get("error"))))
EOF
call() { echo "$2" | python3 "$CLI" "$1"; }
entc() { { call $1 '{"method":"stats","params":{"col":"eg"}}' \
	| python3 -c 'import json,sys; print(json.load(sys.stdin)["collections"][0]["entries"])'; } 2>/dev/null || echo "?"; }
cstat() { { call $1 '{"method":"stats"}' \
	| python3 -c "import json,sys; print(json.load(sys.stdin)[\"cluster\"][\"$2\"])"; } 2>/dev/null || echo "?"; }
pidof_node() { eval "echo \$P$1"; }

node 1 17081; node 2 17082; node 3 17083
start 1; start 2; start 3
# S83: a joiner is RECOVERING until it knows there is nothing to pull, and
# a write into that window is refused; wait for each node's own ready line
i=0
while [ $i -lt 200 ]; do
	grep -q "node state .* -> ready" "$D/n1.log" && grep -q "node state .* -> ready" "$D/n2.log" \
		&& grep -q "node state .* -> ready" "$D/n3.log" && break
	sleep 0.1; i=$((i+1))
done
[ $i -lt 200 ] || { echo "a node never reported ready: $(grep -h 'node state' "$D"/n[123].log | tail -3 | tr '\n' '|')"; exit 1; }
sleep 1
T0=$(date +%s)

# E = node 3 restarts empty.  Of the survivors, the LOWER id is the
# designated sender (L); the other (H) is where the bug lives.
ID1=$(cstat 17081 node); ID2=$(cstat 17082 node)
case "$ID1$ID2" in *\?*) echo "no node ids ($ID1/$ID2)"; exit 1 ;; esac
if [ "$ID1" -lt "$ID2" ]; then LN=1; LP=17081; HN=2; HP=17082
else LN=2; LP=17082; HN=1; HP=17081; fi
EP=17083
echo "survivors: L=node$LN(id $ID1/$ID2 lower) H=node$HN; E=node3 restarts empty"

# H AUTHORS the marker; E authors the record, so L and H hold it PASSIVE
call $HP '{"method":"set","params":{"col":"eg","key":"marker-by-H","value":"authored-by-H","ttl":900}}' >/dev/null
call $EP '{"method":"set","params":{"col":"eg","key":"orphan","value":"authored-by-E","ttl":900}}' >/dev/null
i=0
while [ $i -lt 40 ]; do
	[ "$(entc $LP)" = 2 ] && [ "$(entc $HP)" = 2 ] && break
	sleep 1; i=$((i+1))
done
[ "$(entc $LP)" = 2 ] && [ "$(entc $HP)" = 2 ] && ok \
	|| { bad "setup: copies did not reach both survivors (L=$(entc $LP) H=$(entc $HP))"; exit 1; }

# Everyone joined this cluster EMPTY, so every node holds a fresh stamp
# for every peer from the cold start.  Within the sender holdoff (S82,
# 30 s) H would skip L and self-designate for E from its first tick -
# there would be no ordinary cycle to observe.  Let the cold-start
# stamps age out first; this is the property under test, not a wait.
while [ $(( $(date +%s) - T0 )) -lt 36 ]; do sleep 1; done

# Restart E and let the LIVE master hand it an id; then freeze the
# lowest holder L.  S83: E pulls from L if its pull beat the freeze, and
# from H after the pull to a frozen L times out (5 s); either way E must
# end complete, say from whom, and serve the record locally.  A frozen
# peer is the shape of a sender dying mid-transfer.
kill -9 "$(pidof_node 3)" 2>/dev/null; wait "$(pidof_node 3)" 2>/dev/null
P3=; start 3
i=0; eid=0
while [ $i -lt 40 ]; do
	eid=$(cstat $EP node); [ "$eid" != "?" ] && [ "$eid" -gt 0 ] && break
	sleep 0.5; i=$((i+1))
done
[ "$eid" != "?" ] && [ "$eid" -gt 0 ] && ok || bad "E never got an id"
kill -STOP "$(pidof_node $LN)"
i=0
while [ $i -lt 45 ]; do
	[ "$(entc $EP)" = 2 ] && break
	sleep 1; i=$((i+1))
done
kill -CONT "$(pidof_node $LN)" 2>/dev/null
if [ "$(entc $EP)" = 2 ]; then
	ok
	BL=$(grep "bootstrapped from node" "$D/n3.log" | tail -1 | sed 's/.*cluster: //')
	echo "  E after ${i}s: ${BL:-no bootstrap line}" | cut -c1-140
	[ -n "$BL" ] && ok || bad "S81: E holds the keyspace but never said it bootstrapped"
	# and it is LOCAL: a read on E pulls nothing
	PS0=$(cstat $EP pull_sent)
	R=$(call $EP '{"method":"get","params":{"col":"eg","key":"orphan"}}')
	PS1=$(cstat $EP pull_sent)
	echo "$R" | grep -q '"authored-by-E"' && ok || bad "backfilled value wrong: $R"
	[ "$PS1" = "$PS0" ] && ok || bad "backfilled record was served by a pull ($PS0 -> $PS1), not held locally"
else
	bad "S81: the passive record never reached the restarted node with its lowest holder frozen (E holds $(entc $EP) of 2)"
	grep -hE "bootstrap|backfill" "$D/n3.log" | tail -2 | sed 's/^/  E said: /'
fi


# ---- phase 2 (S82): two restarts within one sweep of each other --------
# Restart A empty, then B empty right behind it.  Under the push, B's
# designated sender was the lowest live id - if A drew it, A sent B its
# nothing and B stayed short for ever.  Under the pull (S83) a node that
# is still recovering is never a candidate: B pulls from a READY holder,
# and A, once complete, is one.  Both must end complete, from a node
# that held the data, served locally.
if [ $fail -eq 0 ]; then
	kill -9 "$(pidof_node 3)" 2>/dev/null; wait "$(pidof_node 3)" 2>/dev/null
	P3=; start 3
	i=0; a=0
	while [ $i -lt 40 ]; do
		a=$(cstat 17083 node); [ "$a" != "?" ] && [ "$a" -gt 0 ] && break
		sleep 0.5; i=$((i+1))
	done
	[ "$a" != "?" ] && [ "$a" -gt 0 ] && ok || bad "S82: A got no id"
	s1=$(cstat 17081 node); s2=$(cstat 17082 node)
	# B = the survivor with the higher id restarts; C = the other stays
	if [ "$s1" -gt "$s2" ]; then BN=1; BP=17081; CN=2; CP=17082; else BN=2; BP=17082; CN=1; CP=17081; fi
	kill -9 "$(pidof_node $BN)" 2>/dev/null; wait "$(pidof_node $BN)" 2>/dev/null
	eval "P$BN="; start $BN
	echo "  phase 2: A=node3 (id $a) and B=node$BN restarted empty back to back; C=node$CN (id $(cstat $CP node)) holds the data"
	i=0
	while [ $i -lt 50 ]; do
		[ "$(entc $BP)" = 2 ] && [ "$(entc 17083)" = 2 ] && break
		sleep 1; i=$((i+1))
	done
	if [ "$(entc $BP)" = 2 ] && [ "$(entc 17083)" = 2 ]; then
		ok
		for n in 3 $BN; do
			BL=$(grep "bootstrapped from node" "$D/n$n.log" | tail -1 | sed 's/.*cluster: //')
			echo "  node$n after ${i}s: ${BL:-no bootstrap line}" | cut -c1-140
			[ -n "$BL" ] && ok || bad "S82: node$n holds the keyspace but never said it bootstrapped"
		done
		PS0=$(cstat $BP pull_sent)
		R=$(call $BP '{"method":"get","params":{"col":"eg","key":"orphan"}}')
		PS1=$(cstat $BP pull_sent)
		echo "$R" | grep -q '"authored-by-E"' && ok || bad "S82: B's copy is wrong: $R"
		[ "$PS1" = "$PS0" ] && ok || bad "S82: B served the record by a pull ($PS0 -> $PS1), not locally"
	else
		bad "S82: a back-to-back restart ended short (B holds $(entc $BP) of 2, A holds $(entc 17083))"
		grep -hE "bootstrap|backfill" "$D/n3.log" "$D/n$BN.log" | tail -4 | sed 's/^/  said: /'
	fi
else
	echo "phase 2 skipped: phase 1 failed"
fi

echo "backfilltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
