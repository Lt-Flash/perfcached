#!/bin/sh
# boottest.sh — S83: a node that joins an eager fleet holding nothing PULLS
# its bootstrap from one ready peer and reports ready only when the stream
# has ended.  "Ready" means complete - which no push from a sender could
# ever say, because only the sender knew when it was done (S81, S82).
#
# Leg 1, the pull: 2000 records on a 3-node fleet; node 3 restarts empty.
#  - it joins RECOVERING, not ready, and says it is pulling;
#  - its own log says "bootstrapped from node L": every record streamed
#    and stored, BEFORE the line that turns it ready;
#  - the count read right after "ready" is complete - the property;
#  - the peer that served it counts the same records out;
#  - nobody pushed: the survivors' sweep counters stay flat, and the
#    flags they armed on its cold heartbeat drop as stale;
#  - it reports itself established at once - no holdoff to wait out.
# Leg 2, the fallback: the survivors' bulk TCP ports are taken by
# listeners that accept and hang up - what a build before the pull, or a
# refusal, looks like to the joiner.  The pull fails on every candidate,
# the node says so, reports ready anyway, and the PUSH refills it: the
# old path is the fallback, and still has to work.
# usage: test/boottest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcboot.XXXXXX)
P1= P2= P3= P4= P5= P6= L4= L5=
trap 'for p in $P1 $P2 $P3 $P4 $P5 $P6 $L4 $L5; do [ -n "$p" ] && kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

node() { # node <n> <cliport> <mcast-group> <mcast-port>  (advertises 127.0.1.4<n>)
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = bt-client-secret
cluster = bt-cluster-secret
[listen]
tcp = 127.0.0.1:$2
plaintext = loopback
[cluster]
multicast = $3:$4
advertise = 127.0.1.4$1
pull_timeout_ms = 300
[collection eg]
buckets_log2 = 12
pull = 1
mode = eager
EOF
}
start() { # start <n>
	: > "$D/n$1.log"
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$1=\$!"
	i=0
	while [ $i -lt 80 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		eval "kill -0 \$P$1" 2>/dev/null || return 1
		sleep 0.1; i=$((i+1))
	done
	return 1
}
CLI="$D/cli.py"
cat > "$CLI" <<'PYEOF'
import json, socket, sys
port = int(sys.argv[1]); op = sys.argv[2]
try:
    s = socket.create_connection(("127.0.0.1", port), timeout=8)
except Exception:
    print("?"); sys.exit(0)
f = s.makefile("rwb"); rid = [0]
def call(m, **p):
    rid[0] += 1
    r = {"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"] = p
    f.write((json.dumps(r) + "\n").encode()); f.flush()
    return json.loads(f.readline())
if op == "entries":
    print(call("stats", col="eg")["result"]["collections"][0]["entries"])
elif op == "stat":
    print(call("stats")["result"]["cluster"].get(sys.argv[3], "?"))
elif op == "selfstart":
    for m in call("members")["result"]["members"]:
        if m.get("self"): print(m.get("start", "?")); break
    else: print("?")
elif op == "portof":
    want = int(sys.argv[3])
    for m in call("members")["result"]["members"]:
        if m.get("node") == want: print(m.get("port", "?")); break
    else: print("?")
elif op == "fill":
    v = "v" * 200
    for i in range(int(sys.argv[3])):
        call("set", col="eg", key="bk%05d" % i, value=v, ttl=900)
    print("filled")
PYEOF
q() { python3 "$CLI" "$@" 2>/dev/null; }   # q <port> entries|stat <f>|selfstart|portof <id>|fill N
await_entries() { # await_entries <port> <n> <secs>
	i=0
	while [ $i -lt "$3" ]; do
		[ "$(q "$1" entries)" = "$2" ] && return 0
		sleep 1; i=$((i+1))
	done
	return 1
}
await_line() { # await_line <file> <pattern> <secs>
	i=0
	while [ $i -lt "$3" ]; do
		grep -q "$2" "$1" && return 0
		sleep 1; i=$((i+1))
	done
	return 1
}
await_ready() { # await_ready <n>...: every named node's log says it is ready to serve
	for n in "$@"; do
		await_line "$D/n$n.log" "node state .* -> ready" 20 || return 1
	done
	return 0
}

# ---- leg 1: the pull ------------------------------------------------------
node 1 17101 239.255.77.52 17152
node 2 17102 239.255.77.52 17152
node 3 17103 239.255.77.52 17152
start 1 || { echo "node 1 did not start"; tail -3 "$D/n1.log"; exit 1; }
start 2 || { echo "node 2 did not start"; tail -3 "$D/n2.log"; exit 1; }
start 3 || { echo "node 3 did not start"; tail -3 "$D/n3.log"; exit 1; }
# S83: a joiner is RECOVERING until it knows there is nothing to pull; a
# write into that window is refused, as it should be
await_ready 1 2 3 && ok "every node reported ready to serve" || bad "a node never reported ready: $(grep -h 'node state' "$D"/n[123].log | tail -3 | tr '\n' '|')"
N=2000
q 17101 fill $N >/dev/null
await_entries 17102 $N 40 && await_entries 17103 $N 40 \
	&& ok "$N records on every node (the write-path push)" \
	|| { bad "the fleet never converged on $N (n2=$(q 17102 entries) n3=$(q 17103 entries))"; echo "boottest: $pass passed, $fail failed"; exit 1; }
sleep 12                             # one sweep: the fleet goes quiet
R1=$(q 17101 stat repl_out); R2=$(q 17102 stat repl_out)
ID3=$(q 17103 stat node)
kill -9 $P3 2>/dev/null; P3=
sleep 8                              # past the purge window
T0=$(date +%s)
start 3 || { bad "node 3 did not restart: $(tail -2 "$D/n3.log" | tr '\n' ' ')"; }
await_line "$D/n3.log" "holding nothing - pulling a bootstrap" 15 \
	&& ok "it joined saying it would pull before reporting ready" \
	|| bad "no 'pulling a bootstrap' line on the join: $(grep 'joined as' "$D/n3.log" | tail -1)"
grep -q "node state starting -> recovering" "$D/n3.log" \
	&& ok "it entered RECOVERING, not ready" || bad "it did not enter recovering: $(grep 'node state' "$D/n3.log" | tr '\n' ' ')"
await_line "$D/n3.log" "bootstrapped from node" 30 \
	&& ok "it bootstrapped itself (after $(( $(date +%s) - T0 ))s)" \
	|| bad "no 'bootstrapped from node' line in 30s: $(grep -E 'bootstrap|cluster:' "$D/n3.log" | tail -3 | tr '\n' ' ')"
BL=$(grep "bootstrapped from node" "$D/n3.log" | tail -1)
SRC=$(echo "$BL" | sed -n 's/.*bootstrapped from node \([0-9]*\) .*/\1/p')
STREAMED=$(echo "$BL" | sed -n 's/.* - \([0-9]*\) record(s) streamed.*/\1/p')
STORED=$(echo "$BL" | sed -n 's/.*streamed, \([0-9]*\) stored.*/\1/p')
echo "  $BL" | cut -c1-150
[ "$STREAMED" = "$N" ] && [ "$STORED" = "$N" ] && ok "every record streamed and stored ($STREAMED/$STORED of $N)" \
	|| bad "the pull was partial: streamed $STREAMED stored $STORED of $N"
LB=$(grep -n "bootstrapped from node" "$D/n3.log" | tail -1 | cut -d: -f1)
LR=$(grep -n "node state recovering -> ready" "$D/n3.log" | tail -1 | cut -d: -f1)
[ -n "$LB" ] && [ -n "$LR" ] && [ "$LB" -lt "$LR" ] && ok "the completion was logged BEFORE ready (lines $LB < $LR)" \
	|| bad "ready did not follow the completion (bootstrapped at line '$LB', ready at line '$LR')"
E3=$(q 17103 entries)
[ "$E3" = "$N" ] && ok "read right after ready: complete ($E3) - ready means complete" \
	|| bad "ready but incomplete: $E3 of $N"
[ "$(q 17103 stat boot_in)" = "$N" ] && ok "its boot_in counts the pull" || bad "boot_in on the joiner: $(q 17103 stat boot_in)"
SP=$(q 17103 portof "$SRC")
[ "$(q "$SP" stat boot_out)" = "$N" ] && ok "node $SRC (port $SP) counts the same $N out" \
	|| bad "boot_out on the sender node $SRC (port $SP): $(q "$SP" stat boot_out)"
[ "$(q 17103 selfstart)" = "established" ] && ok "it reports itself established at once - no holdoff" \
	|| bad "start kind after the pull: $(q 17103 selfstart) (wanted established)"
# the survivors armed a push on its cold heartbeat and must now drop it
await_line "$D/n1.log" "node $ID3 is established - a backfill armed for it here" 15 && D1=y || D1=n
await_line "$D/n2.log" "node $ID3 is established - a backfill armed for it here" 5 && D2=y || D2=n
[ "$D1" = y ] || [ "$D2" = y ] && ok "the survivors dropped their armed push as stale (n1=$D1 n2=$D2)" \
	|| bad "no survivor dropped its stale flag for node $ID3"
sleep 12                             # one sweep period
R1b=$(q 17101 stat repl_out); R2b=$(q 17102 stat repl_out)
[ "$R1b" = "$R1" ] && [ "$R2b" = "$R2" ] && ok "nobody pushed: sweep counters flat (n1 $R1, n2 $R2)" \
	|| bad "a survivor pushed at the pulled node (n1 $R1->$R1b, n2 $R2->$R2b)"
grep -q "backfilled node $ID3 after its restart" "$D/n1.log" "$D/n2.log" 2>/dev/null \
	&& bad "a survivor walked a backfill for a node that pulled its own" || ok "no survivor walked a backfill"
for p in $P1 $P2 $P3; do kill -9 $p 2>/dev/null; done; P1= P2= P3=
sleep 1

# ---- leg 2: the fallback ---------------------------------------------------
# Listeners on the survivors' bulk TCP ports (the cluster port, on the
# advertised address) that accept and hang up: the daemons warn that the
# bulk listener is unavailable and cannot serve a pull; the joiner's
# pull sees the peer close before the first record - what an older build
# does on the magic count - and falls back to the push.
cat > "$D/hangup.py" <<'PYEOF'
import socket, sys
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((sys.argv[1], int(sys.argv[2]))); s.listen(8)
while True:
    c, _ = s.accept(); c.close()
PYEOF
python3 "$D/hangup.py" 127.0.1.44 17153 & L4=$!
python3 "$D/hangup.py" 127.0.1.45 17153 & L5=$!
sleep 0.5
node 4 17104 239.255.77.53 17153
node 5 17105 239.255.77.53 17153
node 6 17106 239.255.77.53 17153
start 4 || { echo "node 4 did not start"; tail -3 "$D/n4.log"; exit 1; }
start 5 || { echo "node 5 did not start"; tail -3 "$D/n5.log"; exit 1; }
start 6 || { echo "node 6 did not start"; tail -3 "$D/n6.log"; exit 1; }
grep -q "bulk TCP listener unavailable" "$D/n4.log" && grep -q "bulk TCP listener unavailable" "$D/n5.log" \
	&& ok "the survivors' bulk listeners are taken (they cannot serve a pull)" \
	|| bad "the hang-up listeners did not take the bulk ports: $(grep -h 'bulk TCP' "$D/n4.log" "$D/n5.log" | head -2 | tr '\n' ' ')"
await_ready 4 5 6 || bad "a node of the second fleet never reported ready"
M=300
q 17104 fill $M >/dev/null
await_entries 17105 $M 40 && await_entries 17106 $M 40 \
	&& ok "$M records on the second fleet" \
	|| { bad "the second fleet never converged (n5=$(q 17105 entries) n6=$(q 17106 entries))"; echo "boottest: $pass passed, $fail failed"; exit 1; }
ID6=$(q 17106 stat node)
kill -9 $P6 2>/dev/null; P6=
sleep 8
T0=$(date +%s)
start 6 || bad "node 6 did not restart: $(tail -2 "$D/n6.log" | tr '\n' ' ')"
await_line "$D/n6.log" "no peer served a bootstrap" 30 \
	&& ok "every candidate failed and it said so (after $(( $(date +%s) - T0 ))s)" \
	|| bad "no 'no peer served a bootstrap' line: $(grep -E 'bootstrap|pull' "$D/n6.log" | tail -2 | tr '\n' ' ')"
grep -q "relying on the push backfill" "$D/n6.log" && ok "it named the fallback" || bad "the fallback was not named"
grep -q "node state recovering -> ready" "$D/n6.log" && ok "it reported ready anyway" || bad "it never turned ready: $(grep 'node state' "$D/n6.log" | tr '\n' ' ')"
[ "$(q 17106 stat boot_failed)" -ge 2 ] && ok "boot_failed counts the candidates tried ($(q 17106 stat boot_failed))" \
	|| bad "boot_failed on node 6: $(q 17106 stat boot_failed)"
await_entries 17106 $M 60 && ok "the PUSH refilled it ($(q 17106 entries) of $M, after $(( $(date +%s) - T0 ))s)" \
	|| bad "the push fallback never refilled node 6 ($(q 17106 entries) of $M)"
await_line "$D/n4.log" "backfilled node $ID6 after its restart" 40 && W4=y || W4=n
grep -q "backfilled node $ID6 after its restart" "$D/n5.log" && W5=y || W5=n
[ "$W4" = y ] || [ "$W5" = y ] && ok "a survivor walked the push backfill (n4=$W4 n5=$W5)" \
	|| bad "no survivor logged the push backfill for node $ID6"

echo "boottest: $pass passed, $fail failed"
[ $fail -eq 0 ]
