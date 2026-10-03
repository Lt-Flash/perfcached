#!/bin/sh
# probetest.sh - S122: the existence probe must find a record wherever the
# table keeps it.  2,000 records pipelined into 256 buckets of six slots
# put hundreds of them in the overflow leg, and the leg stays real after
# the maintenance thread has grown the table (it stops at load factor
# four, where a few percent of the records still overflow); the leg is
# MEASURED before and after the checks (the table's own overflow count), so
# the assertions can never pass on an empty leg.  RESP EXISTS and TYPE
# (both on the probe) must answer for every record, and the native door's
# ttl verb (which reads the same path but keeps the metadata) shows get and ttl
# see them all.  Fail-first: before the fix EXISTS answered 0 for 539 of
# the 2,000 present keys.
BIN=${1:-./perfcached}
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pcpr.XXXXXX)
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
arena_mb = 32
[secrets]
client = pr-client-secret
cluster = pr-cluster-secret
[listen]
tcp = 127.0.0.1:17681
resp = 127.0.0.1:17682
plaintext = loopback
[collection 0]
buckets_log2 = 8
CONF
"$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
PIDS="$PIDS $!"
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" 2>/dev/null && break; sleep 0.1; i=$((i+1)); done
R=$(python3 - <<'PYEOF'
import json, pcnative, socket, time
s = socket.create_connection(("127.0.0.1", 17681), timeout=20); f = pcnative.wrap(s); rid = [0]
def call(m, **p):
    rid[0] += 1; f.write((json.dumps({"jsonrpc": "2.0", "id": rid[0], "method": m, "params": p}) + "\n").encode()); f.flush()
    return json.loads(f.readline())
# the fill, pipelined: one write, then the replies
reqs = []
for i in range(2000):
    rid[0] += 1
    reqs.append(json.dumps({"jsonrpc": "2.0", "id": rid[0], "method": "set", "params": {"col": "0", "key": "pk%d" % i, "value": "v%d%s" % (i, "x" * (i % 50)), "ttl": 300}}))
f.write(("\n".join(reqs) + "\n").encode()); f.flush()
stored = sum(1 for _ in reqs if json.loads(f.readline()).get("result", {}).get("stored"))
def buckets():
    return [c for c in call("stats")["result"]["collections"] if c["name"] == "0"][0]["buckets"]
def leg():
    # the table's OWN count of records in the overflow leg (/stats
    # collections[].overflow).  It was counted off the dump's tail: every
    # record returned once the cursor named a chain in the leg, from the
    # last bucket on - S281 walks the leg FIRST, so from the last bucket
    # the walk simply ends and that count read 0 (rc62's GitHub red).
    c = [c for c in call("stats")["result"]["collections"] if c["name"] == "0"][0]
    return c["overflow"], c["buckets"]
# the table must stand still first: the splitter is still moving records
# while it grows, and a leg measured mid-split is a different table's leg
b0, still = buckets(), 0
for _ in range(120):
    time.sleep(0.5)
    b1 = buckets()
    still = still + 1 if b1 == b0 else 0
    b0 = b1
    if still >= 4:
        break
leg_before, nb_before = leg()
ttl_absent = sum(1 for i in range(2000) if call("ttl", col="0", key="pk%d" % i)["result"]["ttl"] == -2)
get_miss = sum(1 for i in range(2000) if not call("get", col="0", key="pk%d" % i)["result"].get("found"))
r = socket.create_connection(("127.0.0.1", 17682), timeout=20); rf = r.makefile("rwb")
def resp(*args):
    rf.write(("*%d\r\n" % len(args) + "".join("$%d\r\n%s\r\n" % (len(a), a) for a in args)).encode()); rf.flush()
    return rf.readline().decode().strip()
exists_zero = sum(1 for i in range(2000) if resp("EXISTS", "pk%d" % i) != ":1")
type_none = sum(1 for i in range(2000) if resp("TYPE", "pk%d" % i) != "+string")
one = resp("EXISTS", "pk-not-there")
leg_after, nb_after = leg()
print("stored=%d leg_before=%d leg_after=%d buckets=%d/%d get_miss=%d ttl_absent=%d exists_zero=%d type_none=%d absent_key=%s" % (stored, leg_before, leg_after, nb_before, nb_after, get_miss, ttl_absent, exists_zero, type_none, one))
PYEOF
)
echo "  ..   $R"
LB=$(echo "$R" | sed -n 's/.*leg_before=\([0-9-]*\).*/\1/p'); LA=$(echo "$R" | sed -n 's/.*leg_after=\([0-9-]*\).*/\1/p')
case "$R" in "stored=2000 "*) ok "2,000 records stored, pipelined";; *) bad "fill: $R";; esac
[ "${LB:-0}" -ge 50 ] && [ "${LA:-0}" -ge 1 ] && ok "the overflow leg is real: at least $LB records before the checks, $LA after" || bad "the leg is not there to test against: $R"
case "$R" in *"get_miss=0 ttl_absent=0"*) ok "get and ttl find every record (the leg is read on the value path)";; *) bad "get/ttl: $R";; esac
case "$R" in *"exists_zero=0 "*) ok "RESP EXISTS answers 1 for every present key, leg included";; *) bad "RESP EXISTS answers 0 for present keys: $R";; esac
case "$R" in *"type_none=0 "*) ok "RESP TYPE answers string for every present key";; *) bad "RESP TYPE: $R";; esac
case "$R" in *"absent_key=:0") ok "an absent key still answers 0 (the instrument can say no)";; *) bad "absent key: $R";; esac
echo "probetest: $pass passed, $fail failed"
[ $fail = 0 ]
