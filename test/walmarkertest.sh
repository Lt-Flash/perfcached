#!/bin/sh
# walmarkertest.sh - a start that restores nothing from its snapshot (every record expired) and whose
# log was lost resumes the WAL sequence above the snapshot's marker: the writes after it must replay
# after a second unclean stop, not be skipped as older than the snapshot.  Usage: [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcwmk.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
ss -ltn 2>/dev/null | grep -q ":16495[[:space:]]" && { echo "walmarkertest: port 16495 busy" >&2; exit 1; }
mkdir -p "$D/wal"
cat > "$D/n.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = wmk-client
[listen]
tcp = 127.0.0.1:16495
plaintext = loopback
[wal]
dir = $D/wal
segment_mb = 8
fsync = everysec
save = 900 1
[collection c]
buckets_log2 = 10
EOF
chmod 600 "$D/n.conf"

start() { # start <log>
	: > "$D/$1"
	"$BIN" -f "$D/n.conf" > "$D/$1" 2>&1 & PID=$!
	i=0; while [ $i -lt 150 ]; do
		grep -q "perfcached ready" "$D/$1" && return 0
		kill -0 $PID 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "  the node did not start: $(tail -3 "$D/$1" | tr '\n' ' ')"; return 1
}
crash() { kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=; }
# rpc <json lines on stdin>: one result per line
rpc() { python3 -c '
import json, pcnative, socket, sys
s = socket.create_connection(("127.0.0.1", 16495), timeout=60); f = pcnative.wrap(s); n = 0
for line in sys.stdin:
    if not line.strip(): continue
    n += 1; q = json.loads(line); q["id"] = n; q["jsonrpc"] = "2.0"
    f.write((json.dumps(q) + "\n").encode()); f.flush()
    r = next(m for m in iter(lambda: json.loads(f.readline()), None) if "id" in m)
    print(json.dumps(r.get("result", r.get("error"))))'
}
puts() { # puts <prefix> <count> [ttl]
	python3 -c 'import json,sys
for i in range(int(sys.argv[2])):
    p = {"col": "c", "key": "%s%04d" % (sys.argv[1], i), "value": "v%d" % i}
    if len(sys.argv) > 3: p["ttl"] = int(sys.argv[3])
    print(json.dumps({"method": "set", "params": p}))' "$@" | rpc | grep -c stored
}
found() { # found <prefix> <count>
	python3 -c 'import json,sys
for i in range(int(sys.argv[2])):
    print(json.dumps({"method": "get", "params": {"col": "c", "key": "%s%04d" % (sys.argv[1], i)}}))' "$@" |
		rpc | grep -c '"value"'
}

# 1. 400 records with a 2 s TTL, a snapshot (its marker above 400), the log synced
start a.log || { echo "walmarkertest: $pass passed, $((fail+1)) failed"; exit 1; }
[ "$(puts t 400 2)" = 400 ] && ok "400 short-lived records written" || bad "the TTL writes were not all stored"
echo '{"method":"save"}' | rpc > /dev/null
i=0; while [ $i -lt 100 ] && ! [ -s "$D/wal/dump.rdb" ]; do sleep 0.1; i=$((i+1)); done
sleep 0.5
[ -s "$D/wal/dump.rdb" ] && ok "the snapshot is written" || bad "no dump.rdb after save"
crash

# 2. the log lost (power loss before it reached the disk), the snapshot kept; its records expire
rm -f "$D"/wal/wal-*.seg "$D/wal/CONTROL"
sleep 3
start b.log || { echo "walmarkertest: $pass passed, $((fail+1)) failed"; exit 1; }
grep -qE "recover: rdb loaded \(0 records, 400 expired" "$D/b.log" \
	&& ok "the start loaded the snapshot and restored nothing: its 400 records had expired" \
	|| bad "the premise: $(grep -m1 'recover: rdb' "$D/b.log" | cut -c1-120)"
[ "$(puts n 50)" = 50 ] && ok "50 new writes stored" || bad "the new writes were not all stored"
R=$(echo '{"method":"sync"}' | rpc)
case "$R" in *'"synced": true'*) ok "the log is synced";; *) bad "sync: $R";; esac
crash

# 3. a second unclean stop: the 50 writes are in the log and must come back
start c.log || { echo "walmarkertest: $pass passed, $((fail+1)) failed"; exit 1; }
g=$(found n 50)
[ "$g" = 50 ] && ok "after the second unclean stop all 50 writes are back" \
	|| bad "after the second unclean stop $g of 50 writes are back - the WAL numbered them below the snapshot's marker"
b=$(grep -o '[0-9]* below marker' "$D/c.log" | head -1)
[ "${b%% *}" = 0 ] && ok "the replay skipped none as below the marker" || bad "the replay says: $b"
crash
e=$(cat "$D"/*.log | grep -E ' (ERROR|CRIT)' | grep -cv 'an fsync has been stuck')
[ "$e" = 0 ] && ok "no ERROR/CRIT" || { bad "$e ERROR/CRIT lines"; cat "$D"/*.log | grep -E ' (ERROR|CRIT)' | head -5; }
echo "walmarkertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
