#!/bin/sh
# legwalktest.sh - S121: the cooperative walk budgets the overflow leg.  A
# table of 4,096 buckets takes 30,000 records pipelined (load factor 7,
# well past the four the maintenance thread grows at, so the leg holds
# thousands of records for the seconds this takes); `scan` with values in
# chunks of 64 buckets must never return more than 64 buckets' worth of
# records plus one chain, must offer every record, and must cross the leg
# over several chunks (cursors carrying the leg bit); `dump` on the same
# table the same.  Fail-first: the walk before S121 handed the whole leg
# out in the final chunk - thousands of records against a 384-record budget.
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pclw.XXXXXX)
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT TERM INT
cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = lw-client-secret
cluster = lw-cluster-secret
[listen]
tcp = 127.0.0.1:17701
plaintext = loopback
[collection c]
buckets_log2 = 12
CONF
"$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
PIDS="$PIDS $!"
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" 2>/dev/null && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { echo "node did not start"; cat "$D/n.log"; exit 1; }
R=$(python3 - <<'PYEOF'
import json, socket, time
N = 30000; COUNT = 64; SLOTS = 6
s = socket.create_connection(("127.0.0.1", 17701), timeout=60); f = s.makefile("rwb"); rid = [0]
def call(m, **p):
    rid[0] += 1; f.write((json.dumps({"jsonrpc": "2.0", "id": rid[0], "method": m, "params": p}) + "\n").encode()); f.flush()
    return json.loads(f.readline())
reqs = []
for i in range(N):
    rid[0] += 1
    reqs.append(json.dumps({"jsonrpc": "2.0", "id": rid[0], "method": "set", "params": {"col": "c", "key": "lk%d" % i, "value": "v%d" % i, "ttl": 600}}))
f.write(("\n".join(reqs) + "\n").encode()); f.flush()
stored = sum(1 for _ in reqs if json.loads(f.readline()).get("result", {}).get("stored"))
def walk(verb):
    cursor = 0; seen = {}; chunks = 0; biggest = 0; leg_chunks = 0; dups = 0; t0 = time.time()
    while True:
        if verb == "scan":
            r = call("scan", col="c", cursor=cursor, count=COUNT, values=True)["result"]; recs = r["items"]
        else:
            r = call("dump", col="c", cursor=cursor, count=COUNT)["result"]; recs = r["records"]
        chunks += 1; biggest = max(biggest, len(recs))
        if cursor & 0x80000000: leg_chunks += 1
        for rec in recs:
            k = rec["k"]
            if k in seen: dups += 1
            seen[k] = 1
        cursor = r["cursor"]
        if not r["more"]: break
    return "%s: unique=%d chunks=%d biggest=%d leg_chunks=%d dups=%d secs=%.1f" % (verb, len(seen), chunks, biggest, leg_chunks, dups, time.time() - t0)
buckets = [c for c in call("stats")["result"]["collections"] if c["name"] == "c"][0]["buckets"]
print("fill: stored=%d buckets=%d" % (stored, buckets))
print(walk("scan"))
print(walk("dump"))
PYEOF
)
echo "$R" | sed 's/^/  ..   /'
g() { echo "$R" | grep "^$1: " | sed "s/^$1: //"; }
case "$(g fill)" in "stored=30000 buckets="[0-9]*) ok "30,000 records stored into a table of $(echo "$(g fill)" | sed 's/.*buckets=//') buckets";; *) bad "fill: $(g fill)";; esac
for verb in scan dump; do
	L=$(g $verb); U=$(echo "$L" | sed -n 's/.*unique=\([0-9]*\).*/\1/p'); B=$(echo "$L" | sed -n 's/.*biggest=\([0-9]*\).*/\1/p'); LC=$(echo "$L" | sed -n 's/.*leg_chunks=\([0-9]*\).*/\1/p'); DU=$(echo "$L" | sed -n 's/.*dups=\([0-9]*\).*/\1/p')
	[ "$U" = 30000 ] && ok "$verb offered every record ($U)" || bad "$verb unique: $L"
	# 64 buckets of 6 slots = 384 records' worth; the leg may run over by one chain (a few dozen at most here)
	[ "${B:-9999}" -le 500 ] && ok "$verb: no chunk larger than the budget plus a chain (biggest $B of 384 + a chain)" || bad "$verb: a chunk of $B records - the leg came out whole: $L"
	[ "${LC:-0}" -ge 2 ] && ok "$verb crossed the leg over $LC chunks with leg cursors" || bad "$verb leg chunks: $L"
	[ "${DU:-1}" = 0 ] && ok "$verb: no record twice while the table stood still" || bad "$verb dups: $L"
done
echo "legwalktest: $pass passed, $fail failed"
[ $fail = 0 ]
