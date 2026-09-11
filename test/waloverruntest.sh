#!/bin/sh
# waloverruntest.sh - a segment overrun FAILS the node.
#
# A ring drop and a segment overrun are the same event: an acknowledged
# write that will not be there after a restart.  The drop has FAILED the
# node since the shed went in; the overrun used to overwrite the records,
# count them, and leave the struggling node in service - which is the
# worst of both, since healthy peers then wait on a node that is
# destroying data.  This drives a real overrun and asserts the node
# leaves service.
# Usage: test/waloverruntest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
SEC=ovr-client-secret
D=$(mktemp -d /var/tmp/pcovr.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

ss -ltn 2>/dev/null | grep -q ":17994[[:space:]]" && {
	echo "waloverruntest: port 17994 already bound" >&2; exit 1; }

mkdir -p "$D/wal"
cat > "$D/c.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = ovr-cluster-secret
[listen]
tcp = 127.0.54.4:17994
# loopback plaintext: perfcli's authenticated path is exclusive with it,
# so this test speaks JSON-RPC on the socket rather than through the CLI
plaintext = loopback
[cluster]
multicast = 239.255.77.184:17284
advertise = 127.0.54.4
mode = store
collections = th
[wal]
dir = $D/wal
probe = no
fsync = everysec
# The smallest ring the config allows, and no automatic snapshots: the
# marker never advances, so nothing is ever free and a few MB of writes
# must wrap it.  This is the fault being reproduced, not a suggestion.
segment_mb = 1
segments = 4
save = off
# A ring big enough that it CANNOT drop: this test is about segments
# filling faster than snapshots free them, and a ring drop already
# FAILS the node by a different route.  The assertion below on
# dropped == 0 is what keeps the two apart - without it this test
# passes on a binary that never learned to shed an overrun at all.
ring_kb = 16384
[collection th]
buckets_log2 = 14
EOF

echo "=== waloverruntest ==="
"$BIN" -f "$D/c.conf" > "$D/log" 2>&1 &
PID=$!
i=0; while [ $i -lt 200 ]; do grep -q "perfcached ready" "$D/log" && break
	sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/log" || { echo "daemon did not start"
	tail -5 "$D/log"; exit 1; }

python3 - > "$D/out" 2>&1 <<'DRIVER'
import json, socket, time
HOST, PORT = "127.0.54.4", 17994

def call(f, m, **p):
    r = {"jsonrpc": "2.0", "id": 1, "method": m}
    if p:
        r["params"] = p
    f.write((json.dumps(r) + "\n").encode())
    f.flush()
    return json.loads(f.readline())

def state(f):
    r = call(f, "stats")
    return r.get("result", r).get("state", "ERR")

s = socket.create_connection((HOST, PORT), timeout=60)
f = s.makefile("rwb")

# a clustered node is STARTING until the map settles
for _ in range(200):
    if state(f) == "ready":
        break
    time.sleep(0.25)
print("STATE_BEFORE", state(f))

# ~6 MB into a 4 MB ring that can never free a segment
n = 8000
for i in range(n):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i + 1, "method": "set",
        "params": {"col": "th", "key": "k%06d" % i,
                   "value": "v" * 800}}) + "\n").encode())
    if (i + 1) % 500 == 0:
        f.flush()
        for _ in range(500):
            f.readline()
f.flush()
for _ in range(n % 500):
    f.readline()
print("DROVE", n)

for _ in range(80):
    if state(f) == "failed":
        break
    time.sleep(0.25)
print("STATE_AFTER", state(f))
print("DROPPED", call(f, "stats").get("result", {}).get("wal", {}).get("dropped", "ERR"))
print("WRITE_AFTER", json.dumps(call(f, "set", col="th", key="after", value="x")))
print("READ_AFTER", json.dumps(call(f, "get", col="th", key="k000001")))
DRIVER
# the driver's lines are the evidence; the two JSON bodies are long and
# only wanted when something fails
grep -vE "^(WRITE|READ)_AFTER " "$D/out"

grep -q "^STATE_BEFORE ready" "$D/out" && ok "starts ready" \
	|| bad "never became ready: $(grep STATE_BEFORE "$D/out")"

grep -q "wal: FULL" "$D/log" && ok "the overrun happened (wal: FULL)" \
	|| bad "no overrun to observe - the ring never wrapped"

grep -q "^DROPPED 0" "$D/out" \
	&& ok "the ring never dropped - this is the OVERRUN path" \
	|| bad "$(grep DROPPED "$D/out") - ring drops would FAIL the node by \
the other route, so this run proves nothing about the overrun"

grep -q "^STATE_AFTER failed" "$D/out" \
	&& ok "the node left service (state=failed)" \
	|| bad "$(grep STATE_AFTER "$D/out") after destroying un-snapshotted records"

case "$(grep '^WRITE_AFTER ' "$D/out")" in
*error*)	ok "writes are refused after the shed" ;;
*)		bad "write ACCEPTED after the shed: $(grep '^WRITE_AFTER ' "$D/out")" ;;
esac

case "$(grep '^READ_AFTER ' "$D/out")" in
*error*)	bad "reads refused too - a failed node must still answer reads" ;;
*)		ok "reads still answered" ;;
esac

kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
echo "waloverruntest: $pass passed, $fail failed"
[ $fail -eq 0 ]
