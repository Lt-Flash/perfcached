#!/bin/sh
# eagertest.sh - eager mode pushes every write to every node without reads; converged reads are
# local, TTLs and updates travel, a rejoined node resyncs in full, deletes tombstone fleet-wide.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pceg.XXXXXX)
P1= P2= P3=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; \
     [ -n "$P3" ] && kill -9 $P3 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
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
	# the child truncates the log only once it runs: a restart's old ready line must be gone first
	rm -f "$D/n$1.log"
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
import json, pcnative, socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=8)
f = pcnative.wrap(s); rid = 0
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    rid += 1
    req = json.loads(line); req["id"] = rid; req["jsonrpc"] = "2.0"
    f.write(json.dumps(req).encode()+b"\n"); f.flush()
    r = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if not (isinstance(_m, dict) and "notify" in _m))
    print(json.dumps(r.get("result", r.get("error"))))
EOF
call() { echo "$2" | python3 "$CLI" "$1"; }
entc() { call $1 '{"method":"stats","params":{"col":"eg"}}' \
	| python3 -c 'import json,sys; print(json.load(sys.stdin)["collections"][0]["entries"])'; }
cstat() { call $1 '{"method":"stats"}' \
	| python3 -c "import json,sys; print(json.load(sys.stdin)[\"cluster\"][\"$2\"])"; }
# the node's OWN start kind, from its members entry
selfstart() { call $1 '{"method":"members"}' | python3 -c '
import json,sys
d = json.load(sys.stdin)
for v in d.values():
    if isinstance(v, list):
        for m in v:
            if isinstance(m, dict) and m.get("self"):
                print(m.get("start", "?")); sys.exit(0)
print("no-self-entry")'; }

node 1 17071
node 2 17072
node 3 17073
start 1
start 2
start 3
# a joiner is RECOVERING until it knows there is nothing to pull, and a write into that window
# is refused; wait for each node's own ready line
i=0
while [ $i -lt 200 ]; do
	grep -q "node state .* -> ready" "$D/n1.log" && grep -q "node state .* -> ready" "$D/n2.log" \
		&& grep -q "node state .* -> ready" "$D/n3.log" && break
	sleep 0.1; i=$((i+1))
done
[ $i -lt 200 ] || { echo "a node never reported ready: $(grep -h 'node state' "$D"/n[123].log | tail -3 | tr '\n' '|')"; exit 1; }
sleep 1

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
# the write-path push coalesces - 200 writes down one connection are a handful of datagrams
# per peer, not 200
RP=$(cstat 17071 repl_pushed); RG=$(cstat 17071 repl_groups)
[ "$RG" -gt 0 ] && [ $((RG * 4)) -le "$RP" ] && ok \
	|| bad "S105: the push did not coalesce (records $RP in $RG datagram(s))"
echo "  push coalescing: $RP records in $RG datagrams"
[ "$E2" -eq 200 ] && [ "$E3" -eq 200 ] && ok \
	|| bad "eager push did not converge ($E2/$E3)"
# delivery is the write-path push - 200 keys to 2 peers; the sweep is repair and may not have run
PU=$(cstat 17071 repl_pushed 2>/dev/null || echo 0)
[ "${PU:-0}" -ge 400 ] 2>/dev/null && ok || bad "repl_pushed at convergence: $PU (want >= 400)"

# a write replicates on the write, whatever its TTL: a 30 s and a 5 s key reach node2 and node3
# within a second or two, not on the next 10 s sweep
E2b=$(entc 17072); E3b=$(entc 17073)
call 17071 '{"method":"set","params":{"col":"eg","key":"short30","value":"lives-30s","ttl":30}}' > /dev/null
call 17071 '{"method":"set","params":{"col":"eg","key":"short5","value":"lives-5s","ttl":5}}' > /dev/null
sleep 1.5
E2c=$(entc 17072); E3c=$(entc 17073)
echo "short-TTL push after 1.5s: node2 +$((E2c-E2b)) node3 +$((E3c-E3b)) (want +2 each)"
[ "$E2c" -ge $((E2b+2)) ] && [ "$E3c" -ge $((E3b+2)) ] && ok \
	|| bad "S73: short-TTL writes did not replicate on the write (+$((E2c-E2b))/+$((E3c-E3b)))"
# held LOCALLY (no pull), with the TTL travelling
PS0=$(cstat 17073 pull_sent)
R=$(call 17073 '{"method":"get","params":{"col":"eg","key":"short30"}}')
PS1=$(cstat 17073 pull_sent)
echo "$R" | grep -q '"lives-30s"' && [ "$PS1" = "$PS0" ] && ok \
	|| bad "S73: the 30 s copy is not held locally on node3 (pulls $PS0->$PS1): $R"
T=$(call 17073 '{"method":"ttl","params":{"col":"eg","key":"short30"}}' \
	| python3 -c 'import json,sys; print(json.load(sys.stdin)["ttl"])' 2>/dev/null || echo 0)
[ "$T" -gt 20 ] && [ "$T" -le 30 ] && ok || bad "S73: replicated ttl of the 30 s key: $T"
PU=$(cstat 17071 repl_pushed 2>/dev/null || echo 0)
[ "${PU:-0}" -ge 2 ] 2>/dev/null && ok || bad "S73: repl_pushed counter: $PU"
# a TTL touch travels as well
call 17071 '{"method":"expire","params":{"col":"eg","key":"short30","ttl":600}}' > /dev/null
sleep 1
T=$(call 17072 '{"method":"ttl","params":{"col":"eg","key":"short30"}}' \
	| python3 -c 'import json,sys; print(json.load(sys.stdin)["ttl"])' 2>/dev/null || echo 0)
[ "$T" -gt 500 ] && ok || bad "S73: a touch did not travel (node2 ttl $T)"
# leave the keyspace as the exact counts below expect it
call 17071 '{"method":"del","params":{"col":"eg","key":"short30"}}' > /dev/null
call 17071 '{"method":"del","params":{"col":"eg","key":"short5"}}' > /dev/null
sleep 1

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

# updates re-propagate: gk010 rewrites in place (the reused-cell wtick re-stamp); gk011 re-sets
# the same value with a new TTL (the versionless bump path must re-stamp too)
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

# NO ECHO: replicas are passive; node3 authored nothing since its rejoin, so its repl_out must
# not grow every sweep
sleep 12
R3=$(cstat 17073 repl_out)
[ "$R3" -eq 0 ] && ok || bad "replica echo: node3 repl_out=$R3"
RO1=$(cstat 17071 repl_out)
sleep 12
RO2=$(cstat 17071 repl_out)
[ "$RO2" -eq "$RO1" ] && ok || bad "author keeps re-pushing ($RO1 -> $RO2)"

# BUDGET: a keyspace past one tick's byte budget (4MiB) must still converge through the resumable
# cursor; 200 x 30KB is past it
for i in $(seq 0 199); do
	printf '{"method":"set","params":{"col":"eg","key":"big%03d","value":"' $i
	awk 'BEGIN{while(i++<30000) printf "x"}'
	printf '","ttl":600}}\n'
done | python3 "$CLI" 17071 > /dev/null
i=0
while [ $i -lt 20 ]; do
	E2=$(entc 17072); E3=$(entc 17073)
	[ "$E2" -ge 480 ] && [ "$E3" -ge 480 ] && break
	sleep 6; i=$((i+1))
done
E2=$(entc 17072); E3=$(entc 17073)
echo "over-budget census: $E2 / $E3 (want >= 480 each)"
[ "$E2" -ge 480 ] && [ "$E3" -ge 480 ] && ok \
	|| bad "over-budget keyspace did not converge ($E2/$E3 of 480)"
# once converged the sweep goes QUIET; let the repair sweep's first pass finish (repl_out stops
# moving across one sweep period) before the quiet window opens
RQ0=$(cstat 17071 repl_out); i=0
while [ $i -lt 6 ]; do
	sleep 11; RQn=$(cstat 17071 repl_out)
	[ "$RQn" -eq "$RQ0" ] && break
	RQ0=$RQn; i=$((i+1))
done
RQ1=$(cstat 17071 repl_out)
sleep 14
RQ2=$(cstat 17071 repl_out)
[ "$RQ2" -eq "$RQ1" ] && ok \
	|| bad "sweep still re-pushing after convergence ($RQ1 -> $RQ2)"

# ---- a node that comes back EMPTY must be refilled by its peers -------
# the sweep sends only authored records, so a peer's new incarnation arms a one-shot backfill
WANT=$(entc 17072)
COLD0=$(grep -c "started cold" "$D/n2.log")
# only survivor log lines written after the restart may satisfy the waits below: node 2's log
# already carries a backfill line from node 3's rejoin
POS2=$(wc -l < "$D/n2.log"); POS3=$(wc -l < "$D/n3.log")
kill -9 $P1 2>/dev/null; P1=
sleep 8                        # past the purge window, so the fleet notices
start 1
# a start with nothing of its own reads "cold" in its members entry for 30 s; no entries == 0
# check here - the backfill completes inside the first sample, the start kind is the property
ST1=$(selfstart 17071)
if [ -z "$ST1" ]; then             # nothing answered right after its ready line: what listened, and for how long not
	echo "  ..   listening on 17071: $(ss -ltn 2>/dev/null | grep -c ':17071[[:space:]]') socket(s); refused for $(python3 -c '
import socket, time
t0 = time.time()
while time.time() - t0 < 5:
    try:
        socket.create_connection(("127.0.0.1", 17071), 0.5).close(); break
    except OSError:
        time.sleep(0.05)
print("%.0f ms more" % ((time.time() - t0) * 1000))')"
	echo "  ..   port 17071 sockets by state: $(ss -tan 2>/dev/null | awk '$4 ~ /:17071$/ {print $1}' | sort | uniq -c | tr '\n' ' ')"
	grep -E "ERROR|WARN|listener|perfcached ready" "$D/n1.log" | head -8 | cut -c1-200 | sed 's/^/  ..   n1: /'
fi
[ "$ST1" = "cold" ] && ok || bad "node1 did not come back cold (start=$ST1)"
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
# and the reason it worked: node2 classified the restart from the ALIVE's
# start byte, not from a record count a sweep could have bumped first
[ "$(grep -c "started cold" "$D/n2.log")" -gt "$COLD0" ] && ok \
	|| bad "node2 did not see the restart as a cold start: $(grep -E 'NOT backfilling|cold' "$D/n2.log" | tail -1)"
# ...and having refilled it the fleet goes QUIET again: the window opens on the restarted node's
# own completion line, and after one sweep period every node's counter must hold
ID1=$(cstat 17071 node)
after() { tail -n +"$(($2 + 1))" "$D/n$1.log"; }   # after <n> <pos>: lines since the restart
i=0
while [ $i -lt 60 ]; do
	grep -q "bootstrapped from node" "$D/n1.log" && break
	sleep 1; i=$((i+1))
done
BL=$(grep "bootstrapped from node" "$D/n1.log" | tail -1)
[ -n "$BL" ] && ok \
	|| bad "node $ID1 never reported its own bootstrap complete in ${i}s: $(grep -E 'bootstrap|cold|cluster:' "$D/n1.log" | tail -2 | tr '\n' '|')"
echo "  $(echo "$BL" | sed 's/.*cluster: //' | cut -c1-110) (after ${i}s)"
{ after 2 "$POS2"; after 3 "$POS3"; } | grep -q "backfilled node $ID1 after its restart" \
	&& bad "a survivor walked a push backfill for a node that pulled its own" || ok
sleep 11                       # one sweep period: anything in flight lands
RA1=$(cstat 17071 repl_out); RB1=$(cstat 17072 repl_out); RC1=$(cstat 17073 repl_out)
sleep 14
RA2=$(cstat 17071 repl_out); RB2=$(cstat 17072 repl_out); RC2=$(cstat 17073 repl_out)
if [ "$RA2" -eq "$RA1" ] && [ "$RB2" -eq "$RB1" ] && [ "$RC2" -eq "$RC1" ]; then ok; else
	bad "a node kept pushing after the backfill completed (n1 $RA1->$RA2, n2 $RB1->$RB2, n3 $RC1->$RC2)"
	echo "  --- ids: n1=$ID1 n2=$(cstat 17072 node) n3=$(cstat 17073 node); senders:$SENDERS"
	for n in 2 3; do echo "  --- node$n cluster lines since the restart:"; eval "after $n \"\$POS$n\"" | grep -E "cluster:" | tail -16 | sed 's/^/    /'; done
fi

# deletes still tombstone fleet-wide
call 17072 '{"method":"del","params":{"col":"eg","key":"gk004"}}' > /dev/null
sleep 1
R=$(call 17071 '{"method":"get","params":{"col":"eg","key":"gk004"}}')
echo "$R" | grep -q '"found": false' && ok || bad "tombstone missed node1: $R"

kill -TERM $P1 $P2 $P3 2>/dev/null; wait 2>/dev/null; P1= P2= P3=
echo "eagertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
