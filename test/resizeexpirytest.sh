#!/bin/sh
# resizeexpirytest.sh - a record that expires while a resize drains the old table is reaped by the live
# table's sweep, which notes the expiry for the cluster: the drain must not remove the live copy first.
# Usage: test/resizeexpirytest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrx.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
if "$BIN" -V 2>/dev/null | grep -q standalone; then
	echo "  SKIP the standalone edition refuses a [cluster] section"
	echo "resizeexpirytest: 0 passed, 0 failed, 1 skipped"; exit "${PC_SKIP_FAILS:-0}"
fi
for p in 21311 21312 21321 21322; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "resizeexpirytest: port $p busy" >&2; exit 1; }
done

for n in 1 2; do
	cat > "$D/n$n.conf" <<C
[daemon]
workers = 2
log_level = notice
allow_create = yes
[memory]
arena_mb = 64
[secrets]
client = rx-client
cluster = rx-cluster
enable = rx-enable
[listen]
tcp = 127.0.0.1:2131$n
http = 127.0.0.1:2132$n
plaintext = loopback
[cluster]
multicast = 239.255.81.31:21331
advertise = 127.0.33.$n
mode = eager
collections = c
[collection c]
buckets_log2 = 16
autoscale = off
C
	chmod 600 "$D/n$n.conf"
	"$BIN" -f "$D/n$n.conf" > "$D/n$n.log" 2>&1 &
	echo $! > "$D/n$n.pid"
done
st() { curl -s -m 3 "http://127.0.0.1:2132$1/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin); c = [x for x in r["collections"] if x["name"] == "c"][0]
t = r["cluster"].get("tombstones_retained", {})
print(eval(sys.argv[1]))' "$2" 2>/dev/null; }
i=0; while [ $i -lt 300 ]; do
	[ "$(st 1 'r["state"]')$(st 2 'r["state"]')" = readyready ] && \
		[ "$(st 1 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))')" = 1 ] && break
	sleep 0.2; i=$((i+1))
done
[ $i -lt 300 ] || { echo "resizeexpirytest: the pair did not form"; exit 1; }

# 2,000 keys at TTLs of 2 to 6 s, then a shrink at once: two pre-swap passes and two drain passes a
# second apart, so some of them lapse while the drain still walks the old table
N0=$(st 1 't.get("noted_expiries")')
R=$(python3 - <<'PY'
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 21311), timeout=30); f = pcnative.wrap(s)
rq = [json.dumps({"jsonrpc": "2.0", "id": i, "method": "set",
	"params": {"col": "c", "key": "e%05d" % i, "value": "v%05d" % i, "ttl": 2 + i % 5}}) for i in range(2000)]
rq.append(json.dumps({"jsonrpc": "2.0", "id": 2000, "method": "enable", "params": {"secret": "rx-enable"}}))
rq.append(json.dumps({"jsonrpc": "2.0", "id": 2001, "method": "resize", "params": {"col": "c", "buckets_log2": 12}}))
f.write(("\n".join(rq) + "\n").encode()); f.flush()
ok, rs = 0, None
while rs is None:
    m = json.loads(f.readline())
    if m.get("id") == 2001:
        rs = m
    elif isinstance(m.get("id"), int) and m["id"] < 2000 and (m.get("result") or {}).get("stored"):
        ok += 1
print(ok, json.dumps(rs.get("result") or rs.get("error")))
PY
)
case "$R" in
	"2000 "*resizing*) ok "2,000 keys stored at TTLs of 2-6 s and a shrink to 2^12 accepted on node 1" ;;
	*) bad "setup: $R"; echo "resizeexpirytest: $pass passed, $fail failed"; exit 1 ;;
esac
i=0; while [ $i -lt 100 ] && [ "$(st 1 'c["buckets"]')" != 4096 ]; do sleep 0.1; i=$((i+1)); done
sleep 10                                   # every key lapsed and swept, the drain long done
N1=$(st 1 't.get("noted_expiries")')
L=$(python3 - <<'PY'
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 21311), timeout=30); f = pcnative.wrap(s)
f.write(("\n".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "get", "params": {"col": "c", "key": "e%05d" % i}})
	for i in range(2000)) + "\n").encode()); f.flush()
print(sum(1 for _ in range(2000) if (next(m for m in iter(lambda: json.loads(f.readline()), None) if "id" in m).get("result") or {}).get("value")))
PY
)
[ "$(st 1 'c["buckets"]')" = 4096 ] && [ "$L" = 0 ] \
	&& ok "the shrink landed (4096 buckets) and none of the 2,000 reads back" \
	|| bad "the shrink: buckets $(st 1 'c["buckets"]'), $L of 2,000 still read back"
D1=$(( ${N1:-0} - ${N0:-0} ))
[ "$D1" = 2000 ] \
	&& ok "node 1 noted all 2,000 expiries for the cluster ($N0 -> $N1), those that lapsed mid-drain too" \
	|| bad "node 1 noted $D1 of the 2,000 expiries ($N0 -> $N1): the drain removed the live copy of the rest before its sweep reaped it, so no expiry was retained for them (resize lines: $(grep -c 'resiz' "$D/n1.log"))"
echo "resizeexpirytest: $pass passed, $fail failed"
[ $fail -eq 0 ]
