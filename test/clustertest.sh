#!/bin/sh
# clustertest.sh — M4 verification: a two-node store-mode cluster on
# AUTOMATIC membership (no node ids, no peer lists - only the group):
#  - formation: two cold-started nodes elect exactly ONE master, get
#    DISTINCT auto-assigned node ids, and see each other up;
#  - pull-on-miss: a key set on A is served to a client of B (source
#    cluster), then B holds it locally (a second get is a local hit);
#  - negative cache: a genuine miss on both nodes is asked ONCE, then
#    the negative cache answers without new pull traffic;
#  - tombstone: a delete on A removes it from B and plants the tombstone
#    so a racing pull cannot resurrect it;
#  - remaining TTL survives the pull;
#  - quarantine (S19): a datagram sealed with the WRONG cluster secret
#    is counted as bad_auth and dropped - the node keeps serving, never
#    self-terminates;
#  - failover: killing the MASTER promotes the survivor within ~5s and
#    it keeps serving - the automatic-membership core.
# Usage: test/clustertest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pccl.XXXXXX)
PA= PB=
trap '[ -n "$PA" ] && kill -9 $PA 2>/dev/null; \
      [ -n "$PB" ] && kill -9 $PB 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

node() { # node <n> <cli-port>  (advertises 127.0.0.2<n>)
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = shared-client-secret
cluster = shared-cluster-secret
[listen]
tcp = 127.0.0.1:$2
plaintext = loopback
[cluster]
multicast = 239.255.77.31:17131
advertise = 127.0.0.2$1
pull_timeout_ms = 200
negative_ms = 500
tombstone_ms = 2000
[collection th]
buckets_log2 = 12
pull = 1
EOF
}
start() { # start <id> <var>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$2=$!"
	i=0
	while [ $i -lt 60 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; cat "$D/n$1.log"; exit 1
}

node 1 17001
node 2 17002
start 1 A
start 2 B

CLI="$D/cli.py"
cat > "$CLI" <<'EOF'
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
    print(json.dumps(json.loads(f.readline())["result"]))
EOF
call() { echo "$2" | python3 "$CLI" "$1"; }
cfield() { call $1 '{"method":"stats"}' \
	| python3 -c "import json,sys; print(json.load(sys.stdin)[\"cluster\"][\"$2\"])"; }

# ---- formation: one master, distinct auto ids, mutual visibility -------
sleep 3.5   # join window + election + first keepalives
RA=$(cfield 17001 role); RB=$(cfield 17002 role)
NA=$(cfield 17001 node); NB=$(cfield 17002 node)
echo "formation: A id=$NA role=$RA, B id=$NB role=$RB"
case "$RA/$RB" in
	master/member|member/master) ok ;;
	*) bad "no single master (A=$RA B=$RB)" ;;
esac
[ "$NA" -ge 1 ] && [ "$NB" -ge 1 ] && [ "$NA" != "$NB" ] && ok \
	|| bad "node ids not distinct/assigned (A=$NA B=$NB)"
UP=$(cfield 17001 peers_up)
[ "$UP" = 1 ] && ok || bad "A does not see B up (peers_up=$UP)"

# pull-on-miss: set on A, get on B -> served from cluster
call 17001 '{"method":"set","params":{"col":"th","key":"shared","value":"fromA","ttl":600}}' >/dev/null
R=$(call 17002 '{"method":"get","params":{"col":"th","key":"shared"}}')
echo "B get: $R"
echo "$R" | grep -q '"source": "cluster"' && ok || bad "no cluster pull: $R"
echo "$R" | grep -q '"value": "fromA"' && ok || bad "wrong pulled value"
T=$(echo "$R" | python3 -c 'import json,sys; print(json.load(sys.stdin)["ttl"])')
[ "$T" -gt 500 ] && [ "$T" -le 600 ] && ok || bad "ttl not preserved ($T)"

# now B holds it locally: a second get is a plain local hit (no source)
R=$(call 17002 '{"method":"get","params":{"col":"th","key":"shared"}}')
echo "$R" | grep -q '"source"' && bad "still pulling: $R" || ok

# negative cache: a key nobody has, asked twice, pulls only ONCE
call 17002 '{"method":"get","params":{"col":"th","key":"ghost"}}' >/dev/null
S1=$(cfield 17002 pull_sent)
call 17002 '{"method":"get","params":{"col":"th","key":"ghost"}}' >/dev/null
S2=$(cfield 17002 pull_sent)
[ "$S2" = "$S1" ] && ok || bad "negative cache did not suppress (pull_sent $S1->$S2)"
NEG=$(cfield 17002 neg_hits)
[ "$NEG" -ge 1 ] && ok || bad "no neg_hits recorded"

# tombstone: delete on A -> gone on B, and a re-pull cannot resurrect
call 17001 '{"method":"del","params":{"col":"th","key":"shared"}}' >/dev/null
sleep 0.5
R=$(call 17002 '{"method":"get","params":{"col":"th","key":"shared"}}')
echo "B after A-delete: $R"
echo "$R" | grep -q '"found": false' && ok || bad "tombstone did not remove: $R"

# both nodes still serving, no crash
call 17001 '{"method":"ping"}' | grep -q pong && ok || bad "A died"
call 17002 '{"method":"ping"}' | grep -q pong && ok || bad "B died"

# quarantine: a wrong-secret datagram to A's unicast port must be dropped
python3 - <<'EOF'
import socket, os
# a random 200-byte blob with the right magic but garbage AEAD
pkt = bytes([0xA1, 1]) + os.urandom(198)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for _ in range(20):
    s.sendto(pkt, ("127.0.0.21", 17131))
EOF
sleep 0.5
BA=$(cfield 17001 bad_auth)
[ "$BA" -ge 20 ] && ok || bad "bad_auth not counted ($BA)"
call 17001 '{"method":"ping"}' | grep -q pong && ok || bad "A self-terminated on bad auth"
grep -q "Shutting down\|self-term" "$D/n1.log" && bad "A logged a self-terminate" || ok

# ---- failover: kill the master, the survivor promotes itself -----------
if [ "$RA" = master ]; then
	kill -9 $PA; PA=; SURV=17002; SLOG="$D/n2.log"
else
	kill -9 $PB; PB=; SURV=17001; SLOG="$D/n1.log"
fi
i=0
while [ $i -lt 80 ]; do
	R=$(cfield $SURV role 2>/dev/null || echo joining)
	[ "$R" = master ] && break
	sleep 0.1; i=$((i+1))
done
echo "failover: survivor role=$R after $((i / 10)).$((i % 10))s"
[ "$R" = master ] && ok || bad "survivor never promoted (role=$R)"
call $SURV '{"method":"set","params":{"col":"th","key":"post-failover","value":"alive"}}' >/dev/null
R=$(call $SURV '{"method":"get","params":{"col":"th","key":"post-failover"}}')
echo "$R" | grep -q '"value": "alive"' && ok || bad "survivor not serving: $R"

kill -TERM $PA $PB 2>/dev/null; wait 2>/dev/null; PA= PB=
echo "clustertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
