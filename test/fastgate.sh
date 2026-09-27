#!/bin/sh
# fastgate.sh — S141: the ninety-second fleet gate.
#
# The two worst defects of 2026-09-13 were both in cluster.c's
# ORCHESTRATION - a buffer sized from the wrong number, and a field
# tested one line before the parse that sets it.  Neither was visible to
# gcc 14 with -Werror, to -fanalyzer, or to any of the 21 satellite
# suites under ASan: the codecs were correct, and the defects were call
# sites.  A running fleet is the only instrument that sees them, and the
# fleet gate cost twenty minutes and ran by hand - so it ran before a
# TAG and not before a CHANGE, which is backwards for a class of bug
# nothing cheaper finds.
#
# This is the cheap half.  Three nodes on loopback, no containers, each
# PLANE exercised once - form, admit, pull, forward, migrate, promote -
# and nothing else.  It is not a replacement for the tag gate:
# splitbraintest, stepdowntest and dualclaimtest need real partitions
# and stay where they are.  The claim is only that the first ninety
# seconds of fleet behaviour catches the ordering class.
#
# Usage: test/fastgate.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcfast.XXXXXX)
P1= P2= P3= P4=
trap 'for v in "$P1" "$P2" "$P3" "$P4"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
T0=$(date +%s)

node() { # node <n> <cli-port> <http-port>
	mkdir -p "$D/s$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = info
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = fast-client-secret
cluster = fast-cluster-secret
[listen]
tcp = 127.0.0.1:$2
http = 127.0.0.1:$3
plaintext = loopback
[cluster]
multicast = 239.255.77.41:17141
advertise = 127.0.0.4$1
pull_timeout_ms = 200
negative_ms = 500
tombstone_ms = 2000
[collection b]
buckets_log2 = 12
pull = 1
# eager, because the bulk BOOTSTRAP pull is gated on any_eager(): a node
# joining a non-eager fleet just takes push backfill and never opens a
# bulk channel, which is how the first version of this gate passed with
# the bkind bug reintroduced.
mode = eager
EOF
}
start() { # start <n> <var>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$2=\$!"
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "  node $1 did not start:"; tail -3 "$D/n$1.log"; return 1
}
# The client is clustertest.sh's, verbatim: line-oriented JSON-RPC over
# the TCP client port.  My first attempt POSTed to an HTTP /rpc endpoint
# that does not exist, and every data check returned empty - copy the
# working example rather than infer the interface.
CLI="$D/cli.py"
cat > "$CLI" <<'PYEOF'
import json, socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=5)
f = s.makefile("rwb"); rid = 0
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    rid += 1
    req = json.loads(line); req["id"] = rid; req["jsonrpc"] = "2.0"
    f.write(json.dumps(req).encode()+b"\n"); f.flush()
    r = json.loads(f.readline())
    # An error reply must not kill the client: with r["result"] a single
    # error raised KeyError, and every later request on this pipe was
    # lost while the callers' 2>/dev/null made it look like an empty
    # field.  Print the error and keep going, as the other suites do.
    if "result" not in r:
        sys.stderr.write("rpc error: %s\n" % json.dumps(r.get("error", r)))
    print(json.dumps(r.get("result", r.get("error"))))
PYEOF
call()  { echo "$2" | python3 "$CLI" "$1" 2>/dev/null; }
# rc from the COMMAND, never a pipeline: `rc=$?` after `cmd | tail -1`
# reads tail's status, and this project's own gate script reported
# "0 failing" that way on 2026-09-13.
top() { # top <port> <top-level key>  -- state lives here, not in cluster
	call "$1" '{"method":"stats"}' | python3 -c "
import json,sys
try: print(json.load(sys.stdin).get('$2',''))
except Exception: print('')" 2>/dev/null
}
field() { # field <port> <cluster key>
	call "$1" '{"method":"stats"}' | python3 -c "
import json,sys
try: print(json.load(sys.stdin)['cluster'].get('$2',''))
except Exception: print('')" 2>/dev/null
}
value() { # value <port> <key>  -> the stored value, or empty
	call "$1" "{\"method\":\"get\",\"params\":{\"col\":\"b\",\"key\":\"$2\"}}" \
	| python3 -c "
import json,sys
try:
    d=json.load(sys.stdin); print(d.get('value','') if isinstance(d,dict) else '')
except Exception: print('')" 2>/dev/null
}
setk() { # setk <port> <key> <val>
	call "$1" "{\"method\":\"set\",\"params\":{\"col\":\"b\",\"key\":\"$2\",\"value\":\"$3\"}}" >/dev/null 2>&1
}

wait_val() { # wait_val <port> <key> <want> [tenths]  -> rc 0 when it lands
	_n=${4:-50}; _i=0
	while [ $_i -lt $_n ]; do
		[ "$(value "$1" "$2")" = "$3" ] && return 0
		sleep 0.2; _i=$((_i+1))
	done
	return 1
}

node 1 17241 18241; node 2 17242 18242; node 3 17243 18243
C1=17241; C2=17242; C3=17243; C4=17244
start 1 1 || exit 1
start 2 2 || exit 1
start 3 3 || exit 1

# ---- 1. FORM: three nodes find each other and elect one master ----
i=0
while [ $i -lt 100 ]; do
	up=$(field $C1 peers_up)
	[ "${up:-0}" = "2" ] && break
	sleep 0.2; i=$((i+1))
done
[ "${up:-0}" = "2" ] && ok "formed: node 1 sees 2 peers" \
	|| bad "did not form in 20s (peers_up=${up:-none})"
# peers_up reaching 2 does NOT mean the election has settled and
# propagated - reading master right after it gave 0 / 0 / 778.  Wait for
# the condition rather than assume a sleep covers it.
i=0
while [ $i -lt 100 ]; do
	m1=$(field $C1 master); m2=$(field $C2 master); m3=$(field $C3 master)
	[ -n "$m1" ] && [ "$m1" != "0" ] && [ "$m1" = "$m2" ] && [ "$m2" = "$m3" ] && break
	sleep 0.2; i=$((i+1))
done
[ -n "$m1" ] && [ "$m1" != "0" ] && [ "$m1" = "$m2" ] && [ "$m2" = "$m3" ] \
	&& ok "one master, and all three name it ($m1)" \
	|| bad "masters disagree after 20s: $m1 / $m2 / $m3"

# ---- 2. ADMIT: each node got a distinct id and an identity ----
n1=$(field $C1 node)
n2=$(field $C2 node)
n3=$(field $C3 node)
[ -n "$n1" ] && [ "$n1" != "$n2" ] && [ "$n2" != "$n3" ] && [ "$n1" != "$n3" ] \
	&& ok "three distinct node ids ($n1 $n2 $n3)" \
	|| bad "node ids collided: $n1 $n2 $n3"
u1=$(field $C1 identity_uuid)
u2=$(field $C2 identity_uuid)
[ -n "$u1" ] && [ "$u1" != "$u2" ] && ok "distinct identities on the wire" \
	|| bad "identities missing or equal: [$u1] [$u2]"

# Every node in an EAGER fleet starts empty and enters RECOVERING, which
# refuses writes.  The first set was being rejected and k1 never existed
# anywhere - the check reported "node 2 never saw k1", which reads like a
# replication failure and was not one.  Wait for READY first.
i=0
while [ $i -lt 100 ]; do
	s1=$(top $C1 state); s2=$(top $C2 state); s3=$(top $C3 state)
	[ "$s1" = "ready" ] && [ "$s2" = "ready" ] && [ "$s3" = "ready" ] && break
	sleep 0.2; i=$((i+1))
done
[ "$s1" = "ready" ] && ok "all three report ready to serve" \
	|| bad "not ready in 20s: $s1 / $s2 / $s3"

# ---- 3. WRITE + PULL: a miss on one node fetches from the holder ----
setk $C1 k1 v1
wait_val $C2 k1 v1 && ok "pull: node 2 fetched a record node 1 holds" \
	|| bad "pull: node 2 never saw k1 [$(value $C2 k1)]"

# ---- 4. FORWARD: a write through a non-holder reaches the holder ----
setk $C3 k2 v2
wait_val $C1 k2 v2 && ok "forward: a write via node 3 is readable at node 1" \
	|| bad "forward: node 1 never saw k2 [$(value $C1 k2)]"

# ---- 5. MIGRATE: a batch of records spreads to every node ----
i=0
while [ $i -lt 60 ]; do
	setk $C1 "m$i" x
	i=$((i+1))
done
wait_val $C2 m59 x && ok "migrate: a 60-record batch is readable across the fleet" \
	|| bad "migrate: node 2 cannot read m59 [$(value $C2 m59)]"

# ---- 6. BOOTSTRAP: a NEW node pulls the keyspace over the bulk plane ----
#
# This check exists because the gate did not earn its keep without it.
# Reintroducing the bkind ordering bug - the one that made every
# bootstrap pull return zero records - left the first five checks GREEN,
# because they all drive the datagram write path and never open a bulk
# channel.  A fourth node joining an already-populated fleet is what
# reaches bulk_serve_boot, and it is the difference between a gate that
# catches this class and one that only looks like it does.
node 4 17244 18244
start 4 4 || exit 1
i=0
while [ $i -lt 150 ]; do
	grep -q "bootstrapped from node" "$D/n4.log" 2>/dev/null && break
	sleep 0.2; i=$((i+1))
done
if grep -q "bootstrapped from node" "$D/n4.log" 2>/dev/null; then
	ok "bootstrap: node 4 pulled the keyspace over the bulk plane"
else
	bad "no bulk bootstrap in 30s: $(grep -oE 'bootstrap[^\"]*' "$D/n4.log" 2>/dev/null | tail -1)"
fi
wait_val $C4 m59 x && ok "and can serve a record it pulled" \
	|| bad "node 4 cannot read m59 after bootstrap [$(value $C4 m59)]"

# ---- 7. PROMOTE: kill the master, a survivor takes over ----
case "$m1" in
"$n1") victim=$P1; vport=$C2 ;;
"$n2") victim=$P2; vport=$C1 ;;
*)     victim=$P3; vport=$C1 ;;
esac
kill -9 "$victim" 2>/dev/null
i=0
while [ $i -lt 150 ]; do
	nm=$(field $vport master)
	[ -n "$nm" ] && [ "$nm" != "$m1" ] && break
	sleep 0.2; i=$((i+1))
done
[ -n "$nm" ] && [ "$nm" != "$m1" ] \
	&& ok "promoted: master $m1 died, $nm took over" \
	|| bad "no promotion in 30s (still $nm)"
wait_val $vport k1 v1 && ok "the survivor still serves reads" \
	|| bad "survivor cannot serve k1 [$(value $vport k1)]"

T1=$(date +%s)
echo "fastgate: $pass passed, $fail failed  ($((T1 - T0))s)"
[ $fail -eq 0 ] || exit 1
exit 0
