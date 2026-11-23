#!/bin/sh
# pumpslottest.sh - a sweep's ack pump dispatches datagrams while it waits: when its target says
# goodbye there and a new address takes the freed peer slot, nothing of the run goes to the newcomer.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
INJ=${2:-./clinject}
D=$(mktemp -d /var/tmp/pcpumpslot.XXXXXX)
LOOP=
trap '[ -n "$LOOP" ] && kill $LOOP 2>/dev/null; kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
if "$BIN" -V 2>/dev/null | grep -q standalone; then
	echo "  SKIP the standalone edition refuses a [cluster] section"
	echo "pumpslottest: 0 passed, 0 failed, 1 skipped"; exit "${PC_SKIP_FAILS:-0}"
fi
[ -x "$INJ" ] || { echo "pumpslottest: $INJ is missing (make clinject)"; exit 1; }
for p in 28671 28681; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "pumpslottest: port $p busy" >&2; exit 1; }
done
SEC=ps-cluster-secret
TO=127.0.235.1:28697
F1=127.0.235.9:28698          # the sweep's target, played by clinject as node 77
F2=127.0.235.10:28699         # the newcomer, node 78
ID1=0102030405060708090a0b0c0d0e0f10
ID2=1112131415161718191a1b1c1d1e1f20

cat > "$D/n1.conf" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = ps-client-secret
cluster = $SEC
[listen]
tcp = 127.0.0.1:28671
http = 127.0.0.1:28681
plaintext = loopback
[cluster]
multicast = 239.255.97.35:28697
advertise = 127.0.235.1
sweep_digests = no
[collection c]
buckets_log2 = 12
mode = eager
C
chmod 600 "$D/n1.conf"
st() { python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:28681/stats",timeout=5).read())
x=r["cluster"]
print(eval(sys.argv[1]))' "$1" 2>/dev/null || echo "?"; }

: > "$D/n1.log"
"$BIN" -f "$D/n1.conf" > "$D/n1.log" 2>&1 &
echo $! > "$D/n1.pid"
i=0; while [ $i -lt 60 ] && ! { [ "$(st 'x["role"]')" = master ] && [ "$(st 'r["state"]')" = ready ]; }; do sleep 0.5; i=$((i+1)); done
[ "$(st 'x["role"]')" = master ] || { bad "node 1 did not found: $(tail -2 "$D/n1.log" | tr '\n' '|')"; echo "pumpslottest: $pass passed, $fail failed"; exit 1; }
# 2000 records of 1,000 bytes: about 36 groups, far more than the pump's window of three
F=$(python3 - <<'PY'
import json, pcnative, socket
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 28671), timeout=30))
for i in range(2000):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": "set", "params": {"col": "c", "key": "k%05d" % i, "value": "v" * 1000}}) + "\n").encode())
f.flush()
print(sum(1 for i in range(2000) if (json.loads(f.readline()).get("result") or {}).get("stored")))
PY
)
[ "$F" = 2000 ] && ok "node 1 founded and holds 2000 records of 1,000 bytes" || bad "node 1 stored $F of 2000"
CID=$(st 'x["cluster_id"]')

# node 77 at F1, alive once a second under one identity and incarnation; it never acks
( while :; do "$INJ" -s $SEC -c "$CID" -f $F1 -t $TO -I 7701 alive 77 $ID1 > /dev/null 2>&1; sleep 0.3; done ) &
LOOP=$!
i=0; while [ $i -lt 40 ] && [ "$(st 'x["peers_up"]')" != 1 ]; do sleep 0.5; i=$((i+1)); done
[ "$(st 'x["peers_up"]')" = 1 ] && ok "node 77 at $F1 is a live peer of node 1" || bad "node 1 never took node 77 in: peers_up $(st 'x["peers_up"]')"
G0=$(st 'x["migrate_dgrams"]')
# the next sweep toward node 77 sends its first window and waits for acks that never come
i=0; while [ $i -lt 300 ] && [ "$(st 'x["migrate_dgrams"]')" = "$G0" ]; do sleep 0.1; i=$((i+1)); done
G1=$(st 'x["migrate_dgrams"]')
[ "$G1" != "$G0" ] && ok "a sweep toward node 77 is in its ack pump ($G0 -> $G1 datagrams sent)" || bad "no sweep reached node 77 in 30 s - the arm tests nothing"
kill $LOOP 2>/dev/null; wait $LOOP 2>/dev/null; LOOP=; sleep 0.2   # a clinject of the loop may still hold F1
# inside the pump: node 77 says goodbye (its id little-endian, as pc_g16 reads it), and node 78 at a new address takes the slot it freed
"$INJ" -s $SEC -c "$CID" -f $F1 -t $TO raw 0e4d00 > /dev/null 2>&1
"$INJ" -s $SEC -c "$CID" -f $F2 -t $TO -I 7801 -w 4000 alive 78 $ID2 > "$D/f2.out" 2>&1
N=$(grep -c "^reply 18 " "$D/f2.out")
[ "$N" = 0 ] && ok "node 78 at $F2 got none of the run meant for node 77 ($(grep -c '^reply ' "$D/f2.out") other frame(s))" \
	|| bad "node 78 at $F2 got $N replication batch(es) of the sweep meant for node 77 - the pump kept sending to the slot after it changed hands"
grep -q "node 77 .* said goodbye" "$D/n1.log" && ok "node 1 took node 77's goodbye" || bad "node 1 never logged node 77's goodbye - the arm tests nothing"
echo "pumpslottest: $pass passed, $fail failed"
[ $fail -eq 0 ]
