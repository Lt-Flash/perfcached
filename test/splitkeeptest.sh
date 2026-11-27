#!/bin/sh
# splitkeeptest.sh - the splitter's time-bounded budget keeps the table up with a write burst: after
# 300,000 records it converges on its target load factor within 90 s (a flat budget needs ~19 min).
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcsk.XXXXXX)
P=""
trap '[ -n "$P" ] && grep -qa -- "$D" /proc/$P/cmdline 2>/dev/null && kill -9 "$P"; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
N=300000
cat > "$D/n.conf" <<CONF
[daemon]
workers = 4
log_level = notice
[memory]
arena_mb = 512
[secrets]
client = sk-client-secret
cluster = sk-cluster-secret
[listen]
tcp = 127.0.0.1:17993
plaintext = loopback
[collection c]
buckets_log2 = 12
CONF
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
P=$!
i=0
while [ $i -lt 200 ]; do
	grep -q "perfcached ready" "$D/n.log" 2>/dev/null && break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/n.log" || { echo "node did not start"; cat "$D/n.log"; exit 1; }

col() { # col <key>: stats.collections[0].<key>
	python3 - "$1" <<'PYEOF'
import json, pcnative, socket, sys
s = socket.create_connection(("127.0.0.1", 17993), timeout=30); f = pcnative.wrap(s)
f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": "stats"}) + "\n").encode()); f.flush()
c = json.loads(f.readline())["result"]["collections"][0]
print(c.get(sys.argv[1], 0))
PYEOF
}

R=$(python3 - "$N" <<'PYEOF'
import json, pcnative, socket, sys, time
N = int(sys.argv[1])
P = 17993
def col():
    s = socket.create_connection(("127.0.0.1", P), timeout=60); f = pcnative.wrap(s)
    f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": "stats"}) + "\n").encode()); f.flush()
    c = json.loads(f.readline())["result"]["collections"][0]; s.close(); return c
s = socket.create_connection(("127.0.0.1", P), timeout=600); f = pcnative.wrap(s); infl = 0
t0 = time.time()
for i in range(N):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": "set", "params":
        {"col": "c", "key": "k%08d" % i, "value": "y" * 100, "ttl": 7200}}) + "\n").encode())
    infl += 1
    if infl == 4000:
        f.flush()
        for _ in range(4000): f.readline()
        infl = 0
f.flush()
for _ in range(infl): f.readline()
el = time.time() - t0
s.close()
c = col()
print("%d %d %d %.1f %.0f" % (c["entries"], c["buckets"], c["overflow"], el, N / el if el else 0))
PYEOF
)
set -- $R
ENT=$1; BKT=$2; LEG=$3; EL=$4; RATE=$5
echo "  ..   $ENT entries in $BKT buckets, leg $LEG, fill ${EL}s (${RATE} set/s)"
[ "$ENT" -ge $((N - N / 100)) ] \
	&& ok "the burst was stored ($ENT of $N)" \
	|| bad "only $ENT of $N stored"
# THE property: the table converges on its target load factor once the burst is over; the time it
# took is reported, so a regression that merely slows it is visible
WAITED=-1
i=0
while [ $i -lt 90 ]; do
	B=$(col buckets); E=$(col entries)
	[ "$B" -gt 0 ] && [ $(( E * 10 / B )) -le 60 ] && { WAITED=$i; break; }
	sleep 1; i=$((i+1))
done
BKT=$(col buckets); ENT=$(col entries); LEG=$(col overflow)
LF10=$(( ENT * 10 / BKT ))
[ "$WAITED" -ge 0 ] \
	&& ok "the table caught up in ${WAITED}s: load factor $((LF10/10)).$((LF10%10)) in $BKT buckets, target 4" \
	|| bad "S131: load factor $((LF10/10)).$((LF10%10)) in $BKT buckets after 90s - the splitter never caught up"
# the leg is REPORTED, not asserted: how much lands there depends on how fast this machine's client
# outruns the tick, and it never drains afterwards
echo "  ..   $LEG of $ENT records went to the overflow leg during the burst"
# the maintenance thread is still answering while all this happens: the
# walk's own cost is published, and a walk that never completed is 0
W=$(python3 - <<'PYEOF'
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 17993), timeout=30); f = pcnative.wrap(s)
f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": "stats"}) + "\n").encode()); f.flush()
print(json.loads(f.readline())["result"]["collections"][0]["held_walk_us"])
PYEOF
)
[ "$W" -gt 0 ] \
	&& ok "the maintenance thread completed a walk through the burst (${W}us)" \
	|| bad "no held walk completed - the maintenance thread never got round"

echo "splitkeeptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
