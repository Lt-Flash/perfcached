#!/bin/sh
# admintest.sh - the admin verbs: save, sync (WAL barrier), load (additive snapshot import, the live
# value always wins), probe.  Usage: test/admintest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcadm.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

mkdir -p "$D/wal"
cat > "$D/w.conf" <<EOF
[daemon]
workers = 2
log_level = warn
[memory]
arena_mb = 64
[secrets]
client = adm-c
cluster = adm-k
enable = adm-e
[listen]
tcp = 127.0.0.1:17059
plaintext = loopback
[wal]
dir = $D/wal
segment_mb = 8
fsync = everysec
save = 900 1
[collection c]
buckets_log2 = 10
EOF
sed -e 's/17059/17060/' -e '/^\[wal\]/,/^save/d' "$D/w.conf" > "$D/n.conf"
"$BIN" -f "$D/w.conf" > "$D/w.log" 2>&1 &
P1=$!
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
P2=$!
i=0
while [ $i -lt 100 ]; do
	grep -q ready "$D/w.log" && grep -q ready "$D/n.log" && break
	sleep 0.1; i=$((i+1))
done

CLI="$D/cli.py"
cat > "$CLI" <<'EOF'
import json, pcnative, socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=8)
f = pcnative.wrap(s); rid = 0
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    rid += 1
    req = json.loads(line); req["id"] = rid; req["jsonrpc"] = "2.0"
    f.write(json.dumps(req).encode()+b"\n"); f.flush()
    r = json.loads(f.readline())
    print(json.dumps(r.get("result", r.get("error"))))
EOF
call() { echo "$2" | python3 "$CLI" "$1"; }

# seed + sync: the barrier must land synced_seq on last_seq
call 17059 '{"method":"set","params":{"col":"c","key":"k1","value":"v1"}}' >/dev/null
call 17059 '{"method":"set","params":{"col":"c","key":"k2","value":"v2"}}' >/dev/null
call 17059 '{"method":"set","params":{"col":"c","key":"k3","value":"v3"}}' >/dev/null
R=$(call 17059 '{"method":"sync"}')
echo "$R" | grep -q '"synced": true' && ok || bad "sync: $R"
R=$(call 17059 '{"method":"stats"}')
EQ=$(echo "$R" | python3 -c 'import json,sys; w=json.load(sys.stdin)["wal"]; print(1 if w["synced_seq"] == w["last_seq"] and w["last_seq"] >= 3 else 0)')
[ "$EQ" = 1 ] && ok || bad "synced_seq != last_seq after sync"

# snapshot, then diverge the live store from it
R=$(call 17059 '{"method":"save"}')
echo "$R" | grep -q '"started": true' && ok || bad "save: $R"
i=0
while [ $i -lt 100 ]; do
	[ -s "$D/wal/dump.rdb" ] && break
	sleep 0.1; i=$((i+1))
done
sleep 0.3
call 17059 '{"method":"del","params":{"col":"c","key":"k1"}}' >/dev/null
call 17059 '{"method":"set","params":{"col":"c","key":"k2","value":"v2-live"}}' >/dev/null

# load imports into every collection: a connection that has not enabled is refused, as for
# create, drop and rename
R=$(call 17059 '{"method":"load"}')
echo "$R" | grep -q 'privileged' && ok || bad "load without enable was not refused: $R"
# load: k1 comes back from the snapshot, k2 keeps the LIVE value
R=$(printf '%s\n' '{"method":"enable","params":{"secret":"adm-e"}}' '{"method":"load"}' |
	python3 "$CLI" 17059 | tail -1)
echo "load: $R"
echo "$R" | grep -q '"loaded": 1' && ok || bad "load count: $R"
echo "$R" | grep -q '"skipped_existing": 2' && ok || bad "skip count: $R"
R=$(call 17059 '{"method":"get","params":{"col":"c","key":"k1"}}')
echo "$R" | grep -q '"value": "v1"' && ok || bad "k1 not restored: $R"
R=$(call 17059 '{"method":"get","params":{"col":"c","key":"k2"}}')
echo "$R" | grep -q '"value": "v2-live"' && ok || bad "live value lost: $R"

# ---- probe: re-measure iops/throughput on demand ----------------------
R=$(call 17059 '{"method":"probe"}')
echo "$R" | grep -q '"sync_iops"' && ok || bad "probe returned no iops: $R"
echo "$R" | grep -q '"cached": false' && ok || bad "probe not fresh: $R"
echo "$R" | grep -q '"qd": 1' && ok || bad "probe lacks qd: $R"
echo "$R" | grep -q '"recommend"' && ok || bad "probe lacks recommendation: $R"
IOPS=$(echo "$R" | python3 -c 'import json,sys; print(json.load(sys.stdin)["sync_iops"])')
[ "$IOPS" -gt 0 ] && ok || bad "probe iops is zero"
R=$(call 17059 '{"method":"stats"}')
echo "$R" | grep -q '"cached": false' && ok || bad "stats still cached after reprobe: $R"
R=$(call 17059 '{"method":"probe","params":{"secs":99}}')
echo "$R" | grep -qi "capped" && ok || bad "secs cap not enforced: $R"
R=$(call 17059 '{"method":"probe"}')
echo "$R" | grep -q '"wal_total_mb"' && ok || bad "probe lacks size recommendation: $R"
echo "$R" | grep -q '"basis"' && ok || bad "size recommendation lacks its basis: $R"

# without persistence all three verbs refuse loudly
R=$(call 17060 '{"method":"sync"}')
echo "$R" | grep -q 'not configured' && ok || bad "sync no-wal: $R"
R=$(call 17060 '{"method":"load"}')
echo "$R" | grep -q 'not configured' && ok || bad "load no-wal: $R"
R=$(call 17060 '{"method":"probe"}')
echo "$R" | grep -q 'not configured' && ok || bad "probe no-wal: $R"

kill -TERM $P1 $P2 2>/dev/null; wait 2>/dev/null; P1= P2=

# two probes at once: the second may not zero the first's result between its run and its
# reply (PERFCACHED_TEST_PROBE_GAP_MS holds that gap open for 8 s; the second starts at 5 s)
mkdir -p "$D/wal3"
sed -e 's/17059/17061/' -e "s|$D/wal|$D/wal3|" "$D/w.conf" > "$D/p.conf"
PERFCACHED_TEST_PROBE_GAP_MS=8000 "$BIN" -f "$D/p.conf" > "$D/p.log" 2>&1 &
P1=$!
i=0
while [ $i -lt 100 ]; do
	grep -q ready "$D/p.log" && break
	sleep 0.1; i=$((i+1))
done
R=$(python3 - <<'EOF'
import json, pcnative, socket, threading, time
def conn():
    s = socket.create_connection(("127.0.0.1", 17061), timeout=60)
    return s, pcnative.wrap(s)
def call(f, method, params):
    f.write(json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                        "params": params}).encode() + b"\n"); f.flush()
    line = f.readline()
    return json.loads(line) if line else {"eof": True}
def worker(f):
    return call(f, "conninfo", {}).get("result", {}).get("worker")
def probe(f, secs, out):
    try:
        out.append(call(f, "probe", {"secs": secs}))
    except Exception as e:
        out.append({"exc": str(e)})
# the two probes on different workers: one worker serves its connections one at a time
keep = [conn()]
wa = worker(keep[0][1])
fb = None
for _ in range(64):
    keep.append(conn())
    if worker(keep[-1][1]) != wa:
        fb = keep[-1][1]
        break
a, b = [], []
ta = threading.Thread(target=probe, args=(keep[0][1], 0, a)); ta.start()
time.sleep(5)
tb = threading.Thread(target=probe, args=(fb, 12, b)) if fb else None
if tb: tb.start()
ta.join()
if tb: tb.join()
else: b.append({"exc": "no second worker reached"})
c = []
probe(keep[0][1], 0, c)
print(json.dumps({"a": a[0], "b": b[0], "c": c[0]}))
EOF
)
echo "$R" | python3 -c 'import json,sys; r=json.load(sys.stdin); sys.exit(0 if "seq_mb_s" in r["a"].get("result", {}) else 1)' \
	&& ok || bad "concurrent probe: the first probe got no result: $R"
echo "$R" | python3 -c 'import json,sys; r=json.load(sys.stdin); sys.exit(0 if "running" in r["b"].get("error", {}).get("message", "") else 1)' \
	&& ok || bad "concurrent probe: the second is not refused while one runs: $R"
kill -0 $P1 2>/dev/null && ok || bad "concurrent probe: the daemon died"
echo "$R" | python3 -c 'import json,sys; r=json.load(sys.stdin); sys.exit(0 if "sync_iops" in r["c"].get("result", {}) else 1)' \
	&& ok || bad "concurrent probe: a probe after the pair: $R"
kill -TERM $P1 2>/dev/null; wait 2>/dev/null; P1=

echo "admintest: $pass passed, $fail failed"
[ $fail -eq 0 ]
