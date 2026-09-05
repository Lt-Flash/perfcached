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
T0=$(date +%s)
sleep 3.5

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

attempt=0; verdict=""
while [ $attempt -lt 4 ] && [ -z "$verdict" ]; do
	attempt=$((attempt+1))
	# restart E and let the LIVE master hand it an id; only then freeze
	# the sender - it stays "live" to H for PEER_UP_MS and cannot tick,
	# so H's next tick is an ordinary one
	kill -9 "$(pidof_node 3)" 2>/dev/null; wait "$(pidof_node 3)" 2>/dev/null
	P3=; start 3
	i=0; eid=0
	while [ $i -lt 40 ]; do
		eid=$(cstat $EP node); [ "$eid" != "?" ] && [ "$eid" -gt 0 ] && break
		sleep 0.5; i=$((i+1))
	done
	if [ "$eid" = "?" ] || [ "$eid" -le 0 ]; then
		echo "  attempt $attempt: E never got an id"; continue
	fi
	kill -STOP "$(pidof_node $LN)"
	# wait for H's ordinary cycle: the marker H authored lands on E
	i=0; c=0
	while [ $i -lt 60 ]; do
		c=$(entc $EP); [ "$c" != "?" ] && [ "$c" -ge 1 ] && break
		sleep 0.5; i=$((i+1))
	done
	if [ "$c" = "?" ] || [ "$c" -lt 1 ]; then
		echo "  attempt $attempt: nothing reached E in 30s"
		kill -CONT "$(pidof_node $LN)"; sleep 4; continue
	fi
	if [ "$c" -ge 2 ]; then
		# marker and record together: H was already the sender
		echo "  attempt $attempt: ambiguous (a backfill landed before the role moved) - retrying"
		kill -CONT "$(pidof_node $LN)"; sleep 15; continue
	fi
	echo "  attempt $attempt: H's ordinary cycle delivered the marker, not the copy - now the role moves"
	# L goes dead to H; H inherits the sender role and must deliver the copy
	i=0
	while [ $i -lt 45 ]; do
		[ "$(entc $EP)" = 2 ] && break
		sleep 1; i=$((i+1))
	done
	if [ "$(entc $EP)" = 2 ]; then verdict=pass; else verdict=fail; fi
done
kill -CONT "$(pidof_node $LN)" 2>/dev/null

case "$verdict" in
pass)
	ok
	# and it is LOCAL: a read on E pulls nothing
	PS0=$(cstat $EP pull_sent)
	R=$(call $EP '{"method":"get","params":{"col":"eg","key":"orphan"}}')
	PS1=$(cstat $EP pull_sent)
	echo "$R" | grep -q '"authored-by-E"' && ok || bad "backfilled value wrong: $R"
	[ "$PS1" = "$PS0" ] && ok || bad "backfilled record was served by a pull ($PS0 -> $PS1), not held locally"
	grep -q "backfilled node" "$D/n$HN.log" && ok || bad "H never logged the backfill"
	;;
fail)
	bad "S81: the passive record never reached the restarted node after the sender role moved (E holds $(entc $EP) of 2)"
	grep -h "backfilled node" "$D/n$HN.log" | tail -1 | sed 's/^/  H claimed: /'
	;;
*)
	echo "INCONCLUSIVE: the ordinary-cycle window was never observed in $attempt attempts"
	;;
esac


# ---- phase 2 (S82): two restarts within one sweep of each other --------
# Restart A empty, then B empty before anyone has filled A.  B's
# designated sender is the lowest live id: if A drew the lower one, A
# is the sender and holds nothing - it sends B its nothing, walks clean,
# clears its flag, and its own fill lands a moment later as passive
# copies that never go on.  Ids derive from a random identity, so A is
# restarted until its id is below the untouched survivor's, and B goes
# only while A is still empty.  Fixed, the survivor holding the data
# skips A (a peer it is still backfilling) and self-designates for both.
if [ $fail -eq 0 ]; then
	att=0; v2=""
	while [ $att -lt 6 ] && [ -z "$v2" ]; do
		att=$((att+1))
		kill -9 "$(pidof_node 3)" 2>/dev/null; wait "$(pidof_node 3)" 2>/dev/null
		P3=; start 3
		i=0; a=0
		while [ $i -lt 40 ]; do
			a=$(cstat 17083 node); [ "$a" != "?" ] && [ "$a" -gt 0 ] && break
			sleep 0.5; i=$((i+1))
		done
		if [ "$a" = "?" ] || [ "$a" -le 0 ]; then echo "  phase 2 attempt $att: A got no id"; continue; fi
		s1=$(cstat 17081 node); s2=$(cstat 17082 node)
		# C = an untouched survivor whose id is ABOVE A's (so A, not C, is
		# B's lowest-id sender); B = the other
		if [ "$s1" -gt "$a" ] && [ "$s1" -ge "$s2" ]; then CN=1; CP=17081; BN=2; BP=17082
		elif [ "$s2" -gt "$a" ]; then CN=2; CP=17082; BN=1; BP=17081
		else echo "  phase 2 attempt $att: A drew id $a above both survivors ($s1/$s2) - redrawing"; sleep 2; continue; fi
		if [ "$(entc 17083)" != 0 ]; then
			echo "  phase 2 attempt $att: A was filled before B could go - retrying"; sleep 2; continue
		fi
		kill -9 "$(pidof_node $BN)" 2>/dev/null; wait "$(pidof_node $BN)" 2>/dev/null
		eval "P$BN="; start $BN
		echo "  phase 2 attempt $att: A=node3 (id $a, empty) is the lowest-id sender for B=node$BN; C=node$CN (id $(cstat $CP node)) holds the data"
		i=0
		while [ $i -lt 50 ]; do
			[ "$(entc $BP)" = 2 ] && [ "$(entc 17083)" = 2 ] && break
			sleep 1; i=$((i+1))
		done
		if [ "$(entc $BP)" = 2 ]; then v2=pass; else v2=fail; fi
	done
	case "$v2" in
	pass)
		ok
		[ "$(entc 17083)" = 2 ] && ok || bad "S82: A itself ended short ($(entc 17083) of 2)"
		PS0=$(cstat $BP pull_sent)
		R=$(call $BP '{"method":"get","params":{"col":"eg","key":"orphan"}}')
		PS1=$(cstat $BP pull_sent)
		echo "$R" | grep -q '"authored-by-E"' && ok || bad "S82: B's copy is wrong: $R"
		[ "$PS1" = "$PS0" ] && ok || bad "S82: B served the record by a pull ($PS0 -> $PS1), not locally"
		grep -q "backfilled node" "$D/n$CN.log" && ok || bad "S82: C never logged a backfill"
		;;
	fail)
		bad "S82: B was backfilled by a node that was itself still empty (B holds $(entc $BP) of 2, A holds $(entc 17083))"
		;;
	*)
		echo "INCONCLUSIVE (phase 2): the fresh-node-lowest draw never came up in $att attempts"
		;;
	esac
else
	echo "phase 2 skipped: phase 1 failed"
fi

echo "backfilltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
