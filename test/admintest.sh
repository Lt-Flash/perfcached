#!/bin/sh
# admintest.sh — the admin verbs: save (existing), sync (WAL barrier),
# load (additive snapshot import - the live value always wins).
# Usage: test/admintest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcadm.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT TERM INT
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
import json, socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=8)
f = s.makefile("rwb"); rid = 0
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

# load: k1 comes back from the snapshot, k2 keeps the LIVE value
R=$(call 17059 '{"method":"load"}')
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

echo "admintest: $pass passed, $fail failed"
[ $fail -eq 0 ]
