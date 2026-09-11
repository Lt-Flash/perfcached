#!/bin/sh
# splitkeeptest.sh - S131: the table keeps up with a write burst.
#
# The splitter's budget was a flat 128 splits per maintenance tick, once a
# second, whatever the deficit.  A burst therefore outran it and it never
# caught up at any useful speed: 600,000 records left a table at load
# factor 102 needing about nineteen minutes of ticking to reach its target
# of 4, and everything that would not fit six to a bucket went to the
# overflow leg - which is one chain per hash bucket under a single lock,
# and which NEVER drains back into the table.  The budget is now bounded
# by TIME rather than by a count, so the table grows with the arrivals.
#
# The deficit has to be large to tell the two apart: at 41,000 records the
# old budget reaches load factor 9.4 against the new one's 5.5, which is
# not a gate.  At 300,000 it is 100 against 4.
#
# Fail-first, measured on the pre-S131 binary: load factor ~100, leg ~280k.
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcsk.XXXXXX)
P=""
trap '[ -n "$P" ] && grep -qa -- "$D" /proc/$P/cmdline 2>/dev/null && kill -9 "$P"; rm -rf "$D"' EXIT TERM INT
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

R=$(python3 - "$N" <<'PYEOF'
import json, socket, sys, time
N = int(sys.argv[1])
P = 17993
def col():
    s = socket.create_connection(("127.0.0.1", P), timeout=60); f = s.makefile("rwb")
    f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": "stats"}) + "\n").encode()); f.flush()
    c = json.loads(f.readline())["result"]["collections"][0]; s.close(); return c
s = socket.create_connection(("127.0.0.1", P), timeout=600); f = s.makefile("rwb"); infl = 0
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
# the property: load factor within a generous multiple of the target of 4
LF10=$(( ENT * 10 / BKT ))
[ "$LF10" -le 160 ] \
	&& ok "the table kept up: load factor $((LF10/10)).$((LF10%10)), target 4" \
	|| bad "S131: load factor $((LF10/10)).$((LF10%10)) after a ${N}-record burst - the splitter did not keep up"
# and the leg, which is the consequence an operator feels: it never
# drains back into the table, so what lands there stays until it expires
[ "$LEG" -lt $(( ENT / 2 )) ] \
	&& ok "most of the table is IN the table, not the leg ($LEG of $ENT in the leg)" \
	|| bad "S131: $LEG of $ENT records are in the overflow leg"
# the maintenance thread is still answering while all this happens: the
# walk's own cost is published, and a walk that never completed is 0
W=$(python3 - <<'PYEOF'
import json, socket
s = socket.create_connection(("127.0.0.1", 17993), timeout=30); f = s.makefile("rwb")
f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": "stats"}) + "\n").encode()); f.flush()
print(json.loads(f.readline())["result"]["collections"][0]["held_walk_us"])
PYEOF
)
[ "$W" -gt 0 ] \
	&& ok "the maintenance thread completed a walk through the burst (${W}us)" \
	|| bad "no held walk completed - the maintenance thread never got round"

echo "splitkeeptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
