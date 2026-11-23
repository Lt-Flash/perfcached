#!/bin/sh
# clustertest.sh - a two-node store cluster on automatic membership: one master, distinct ids,
# pull-on-miss, negative cache, tombstones, TTL, wrong-secret quarantine and master failover.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pccl.XXXXXX)
PA= PB=
trap '[ -n "$PA" ] && kill -9 $PA 2>/dev/null; \
      [ -n "$PB" ] && kill -9 $PB 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
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
	: > "$D/n$1.log"
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
import json, pcnative, socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=5)
f = pcnative.wrap(s); rid = 0
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    rid += 1
    req = json.loads(line); req["id"] = rid; req["jsonrpc"] = "2.0"
    f.write(json.dumps(req).encode()+b"\n"); f.flush()
    print(json.dumps(next(_m for _m in iter(lambda: json.loads(f.readline()), None) if not (isinstance(_m, dict) and "notify" in _m))["result"]))
EOF
call() { echo "$2" | python3 "$CLI" "$1"; }
cfield() { call $1 '{"method":"stats"}' \
	| python3 -c "import json,sys; print(json.load(sys.stdin)[\"cluster\"][\"$2\"])"; }

# ---- formation: one master, distinct auto ids, mutual visibility -------
# a joiner is CONNECTING until its links answer: wait for both ready and seeing each other
st() { call $1 '{"method":"stats"}' | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("state"), d["cluster"].get("peers_up"))' 2>/dev/null; }
i=0
while [ $i -lt 300 ] && { [ "$(st 17001)" != "ready 1" ] || [ "$(st 17002)" != "ready 1" ]; }; do
	sleep 0.1; i=$((i+1))
done
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

call 17001 '{"method":"ping"}' | grep -q pong && ok || bad "A died"
call 17002 '{"method":"ping"}' | grep -q pong && ok || bad "B died"

# quarantine: a wrong-secret datagram to A's unicast port must be dropped.  The forgery carries
# this build's version: any OTHER version counts as wire_ver_refused, not as bad auth.
CLVER=$(sed -n 's/^#define PC_CL_VER[[:space:]]*\([0-9][0-9]*\).*/\1/p' src/cluster.h)
[ -n "$CLVER" ] || { bad "PC_CL_VER not found in src/cluster.h"; exit 1; }
CLVER=$CLVER python3 - <<'EOF'
import socket, os
# a random 200-byte blob with the right magic and version but garbage AEAD
ver = int(os.environ["CLVER"])
pkt = bytes([0xA1, ver]) + os.urandom(198)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for _ in range(20):
    s.sendto(pkt, ("127.0.0.21", 17131))
# and ten with a version this build does not speak: wire_ver_refused, not bad_auth
pkt = bytes([0xA1, (ver + 1) & 0xff]) + os.urandom(198)
for _ in range(10):
    s.sendto(pkt, ("127.0.0.21", 17131))
EOF
sleep 0.5
BA=$(cfield 17001 bad_auth)
[ "$BA" -ge 20 ] && ok || bad "bad_auth not counted ($BA)"
WV=$(cfield 17001 wire_ver_refused)
[ "$WV" -ge 10 ] && ok || bad "wire_ver_refused not counted ($WV)"
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
