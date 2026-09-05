#!/bin/sh
# eagertest.sh — eager store mode: the distributor as a background
# push-to-all synchronizer.  Asserted:
#  - records written through ONE node appear on EVERY node WITHOUT any
#    reads (the whole point: low-TTL keyspaces never converge via
#    pull-on-miss, eager pushes them);
#  - converged reads are LOCAL (a read burst adds zero pulls);
#  - TTLs travel with the copies; updates re-propagate (wtick moves);
#  - a node that died and rejoined resyncs IN FULL from zeroed marks;
#  - deletes still ride the tombstone plane fleet-wide.
# Usage: test/eagertest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pceg.XXXXXX)
P1= P2= P3=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; \
     [ -n "$P3" ] && kill -9 $P3 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

node() { # node <n> <cliport>  (advertises 127.0.1.1<n>)
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = eg-client-secret
cluster = eg-cluster-secret
[listen]
tcp = 127.0.0.1:$2
plaintext = loopback
[cluster]
multicast = 239.255.77.47:17147
advertise = 127.0.1.1$1
pull_timeout_ms = 300
[collection eg]
buckets_log2 = 12
pull = 1
mode = eager
EOF
}
start() { # start <id>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$1=\$!"
	i=0
	while [ $i -lt 60 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; cat "$D/n$1.log"; exit 1
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
entc() { call $1 '{"method":"stats","params":{"col":"eg"}}' \
	| python3 -c 'import json,sys; print(json.load(sys.stdin)["collections"][0]["entries"])'; }
cstat() { call $1 '{"method":"stats"}' \
	| python3 -c "import json,sys; print(json.load(sys.stdin)[\"cluster\"][\"$2\"])"; }

node 1 17071
node 2 17072
node 3 17073
start 1
start 2
start 3
sleep 3.5

# write 200 keys via node1 - NOBODY reads them
for i in $(seq 0 199); do
	printf '{"method":"set","params":{"col":"eg","key":"gk%03d","value":"val-%03d","ttl":500}}\n' $i $i
done | python3 "$CLI" 17071 > /dev/null

# convergence without reads: the sweep runs every ~10s
i=0
while [ $i -lt 5 ]; do
	E2=$(entc 17072); E3=$(entc 17073)
	[ "$E2" -eq 200 ] && [ "$E3" -eq 200 ] && break
	sleep 6; i=$((i+1))
done
E2=$(entc 17072); E3=$(entc 17073)
echo "eager census after writes only: 200 / $E2 / $E3"
[ "$E2" -eq 200 ] && [ "$E3" -eq 200 ] && ok \
	|| bad "eager push did not converge ($E2/$E3)"
RO=$(cstat 17071 repl_out 2>/dev/null || echo 0)
[ "${RO:-0}" -ge 200 ] 2>/dev/null && ok || bad "repl_out counter: $RO"

# converged reads are LOCAL: a 50-read burst adds zero pulls
PS0=$(cstat 17072 pull_sent)
for i in $(seq 0 49); do
	printf '{"method":"get","params":{"col":"eg","key":"gk%03d"}}\n' $i
done | python3 "$CLI" 17072 > "$D/reads.out"
PS1=$(cstat 17072 pull_sent)
[ "$PS1" -eq "$PS0" ] && ok || bad "converged reads still pulled ($PS0 -> $PS1)"
grep -q '"val-007"' "$D/reads.out" && ok || bad "wrong replicated value"

# TTLs travel with the copies
T=$(call 17073 '{"method":"ttl","params":{"col":"eg","key":"gk005"}}' \
	| python3 -c 'import json,sys; print(json.load(sys.stdin)["ttl"])')
[ "$T" -gt 400 ] && [ "$T" -le 500 ] && ok || bad "replicated ttl: $T"

# updates re-propagate (the wtick moves past the mark).  gk010 rewrites
# the value in place (same cell - the reused-cell wtick re-stamp);
# gk011 re-sets the SAME value with a new TTL (the versionless bump
# path must re-stamp too, or a TTL refresh never travels)
call 17072 '{"method":"set","params":{"col":"eg","key":"gk010","value":"updated","ttl":500}}' > /dev/null
call 17072 '{"method":"set","params":{"col":"eg","key":"gk011","value":"val-011","ttl":900}}' > /dev/null
i=0
while [ $i -lt 5 ]; do
	R=$(call 17073 '{"method":"get","params":{"col":"eg","key":"gk010"}}')
	echo "$R" | grep -q '"updated"' && break
	sleep 6; i=$((i+1))
done
echo "$R" | grep -q '"updated"' && ok || bad "update did not propagate: $R"
i=0
while [ $i -lt 4 ]; do
	T=$(call 17073 '{"method":"ttl","params":{"col":"eg","key":"gk011"}}' \
		| python3 -c 'import json,sys; print(json.load(sys.stdin)["ttl"])')
	[ "$T" -gt 500 ] && break
	sleep 4; i=$((i+1))
done
[ "$T" -gt 500 ] && ok || bad "ttl refresh did not propagate: $T"

# a rejoining node resyncs IN FULL (marks zeroed on the new session)
kill -9 $P3 2>/dev/null; wait $P3 2>/dev/null; P3=
sleep 7   # past the purge window: the peers forget it
for i in $(seq 200 279); do
	printf '{"method":"set","params":{"col":"eg","key":"gk%03d","value":"late-%03d","ttl":500}}\n' $i $i
done | python3 "$CLI" 17071 > /dev/null
start 3
i=0
while [ $i -lt 8 ]; do
	E3=$(entc 17073)
	[ "$E3" -eq 280 ] && break
	sleep 6; i=$((i+1))
done
E3=$(entc 17073)
echo "rejoined census: $E3 / 280"
[ "$E3" -eq 280 ] && ok || bad "rejoin resync incomplete ($E3/280)"
R=$(call 17073 '{"method":"get","params":{"col":"eg","key":"gk777"}}' 2>/dev/null)
R=$(call 17073 '{"method":"get","params":{"col":"eg","key":"gk250"}}')
echo "$R" | grep -q '"late-250"' && ok || bad "late key on rejoined node: $R"
R=$(call 17073 '{"method":"get","params":{"col":"eg","key":"gk003"}}')
echo "$R" | grep -q '"val-003"' && ok || bad "old key on rejoined node: $R"

# NO ECHO: replicas are passive and never re-propagate.  node3
# authored nothing since its rejoin - if pushed copies bounced, its
# repl_out would grow every sweep
sleep 12
R3=$(cstat 17073 repl_out)
[ "$R3" -eq 0 ] && ok || bad "replica echo: node3 repl_out=$R3"
RO1=$(cstat 17071 repl_out)
sleep 12
RO2=$(cstat 17071 repl_out)
[ "$RO2" -eq "$RO1" ] && ok || bad "author keeps re-pushing ($RO1 -> $RO2)"

# BUDGET: a keyspace larger than one tick's byte budget must still
# converge.  Before the resumable cursor the sweep restarted at bucket 0
# every tick, so it replicated exactly one budget's worth (4MiB) and
# re-sent that same head forever - measured, with the tail never
# arriving.  200 x 30KB is comfortably past the budget.
for i in $(seq 0 199); do
	printf '{"method":"set","params":{"col":"eg","key":"big%03d","value":"' $i
	awk 'BEGIN{while(i++<30000) printf "x"}'
	printf '","ttl":600}}\n'
done | python3 "$CLI" 17071 > /dev/null
i=0
while [ $i -lt 20 ]; do
	E2=$(entc 17072)
	[ "$E2" -ge 480 ] && break
	sleep 6; i=$((i+1))
done
E2=$(entc 17072); E3=$(entc 17073)
echo "over-budget census: $E2 / $E3 (want >= 480 each)"
[ "$E2" -ge 480 ] && [ "$E3" -ge 480 ] && ok \
	|| bad "over-budget keyspace did not converge ($E2/$E3 of 480)"
# and once converged the sweep goes QUIET - it must not re-push forever
RQ1=$(cstat 17071 repl_out)
sleep 14
RQ2=$(cstat 17071 repl_out)
[ "$RQ2" -eq "$RQ1" ] && ok \
	|| bad "sweep still re-pushing after convergence ($RQ1 -> $RQ2)"

# ---- a node that comes back EMPTY must be refilled by its peers -------
# The steady sweep deliberately sends only records this node AUTHORED: a
# copy it merely received is PASSIVE, and re-sending those would echo the
# keyspace around the fleet for ever.  The consequence was that a node
# restarted empty got NOTHING from peers holding only copies - measured
# before the fix at 120 seconds flat at 0 entries while both peers held
# 20000.  The fix arms a ONE-SHOT backfill when a peer's incarnation
# changes, from a single designated sender.
WANT=$(entc 17072)
kill -9 $P1 2>/dev/null; P1=
sleep 8                        # past the purge window, so the fleet notices
start 1
[ "$(entc 17071)" -eq 0 ] && ok || bad "node1 did not come back empty"
i=0
while [ $i -lt 45 ]; do
	E1=$(entc 17071)
	[ "$E1" -ge "$WANT" ] && break
	sleep 2; i=$((i+1))
done
E1=$(entc 17071)
echo "refill after empty restart: $E1 of $WANT"
[ "$E1" -ge "$WANT" ] && ok \
	|| bad "peers never refilled the restarted node ($E1 of $WANT)"
# ...and having refilled it, the fleet must go QUIET again: a backfill
# that keeps firing IS the echo the PASSIVE rule exists to prevent
RB1=$(cstat 17072 repl_out)
sleep 14
RB2=$(cstat 17072 repl_out)
[ "$RB2" -eq "$RB1" ] && ok \
	|| bad "backfill kept re-pushing after it completed ($RB1 -> $RB2)"

# deletes still tombstone fleet-wide
call 17072 '{"method":"del","params":{"col":"eg","key":"gk004"}}' > /dev/null
sleep 1
R=$(call 17071 '{"method":"get","params":{"col":"eg","key":"gk004"}}')
echo "$R" | grep -q '"found": false' && ok || bad "tombstone missed node1: $R"

kill -TERM $P1 $P2 $P3 2>/dev/null; wait 2>/dev/null; P1= P2= P3=
echo "eagertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
