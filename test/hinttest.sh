#!/bin/sh
# hinttest.sh - S295: proxy holder hints (test/hinttest.c holds the legs).
# A three-node proxy fleet, each node on its own loopback address so a
# client's standbys reach the address a member advertises.
# Fail-first: a daemon before S295 sends no hint - every read keeps going
# to node 1 and pays the pull (hint hits 0).
# Usage: test/hinttest.sh [./perfcached] [./hinttest]
set -u
BIN=${1:-./perfcached}
DRV=${2:-./hinttest}
PORT=17774
for n in 1 2 3; do
	ss -ltn 2>/dev/null | grep -q "127.0.43.$n:$PORT[[:space:]]" && { echo "hinttest: 127.0.43.$n:$PORT busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pchint.XXXXXX)
trap 'for f in "$D"/n*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; [ -n "${KEEP:-}" ] && cp -r "$D" "$KEEP"; rm -rf "$D"' EXIT INT TERM
for n in 1 2 3; do
	mkdir -p "$D/s$n/wal"
	cat > "$D/n$n.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$n
[memory]
arena_mb = 64
[secrets]
client = hint-client-secret
cluster = hint-cluster-secret
[listen]
tcp = 127.0.43.$n:$PORT
http = 127.0.43.$n:17775
plaintext = loopback
[cluster]
multicast = 239.255.80.43:17776
advertise = 127.0.43.$n
mode = proxy
collections = 0
[collection 0]
buckets_log2 = 12
[wal]
dir = $D/s$n/wal
probe = no
fsync = everysec
segment_mb = 8
segments = 4
C
	chmod 600 "$D/n$n.conf"
done
for n in 1 2 3; do
	: > "$D/n$n.log"
	"$BIN" -f "$D/n$n.conf" >> "$D/n$n.log" 2>&1 &
	echo $! > "$D/n$n.pid"
done
for n in 1 2 3; do
	i=0; while [ $i -lt 300 ] && ! grep -q "perfcached ready" "$D/n$n.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/n$n.log" || { echo "hinttest: node $n did not start: $(tail -2 "$D/n$n.log")"; exit 1; }
done
i=0; while [ $i -lt 120 ]; do
	ok=1
	for n in 1 2 3; do
		c=$(curl -s "http://127.0.43.$n:17775/members" | python3 -c 'import json,sys; print(sum(1 for m in json.load(sys.stdin).get("members",[]) if m.get("gone_s",-1) < 0 and m.get("state") == "ready"))' 2>/dev/null)
		[ "$c" = 3 ] || ok=0
	done
	[ $ok = 1 ] && break
	sleep 0.5; i=$((i+1))
done
[ $ok = 1 ] || { echo "hinttest: the proxy fleet did not form"; echo "hinttest: 0 passed, 1 failed"; exit 1; }
sleep 2
"$DRV" 127.0.43.1 127.0.43.2 127.0.43.3 $PORT "$(cat "$D/n3.pid")"
rc=$?
rm -f "$D/n3.pid"                      # the driver killed it
e=$(cat "$D"/n1.log "$D"/n2.log | grep -cE ' (ERROR|CRIT)')
[ "$e" = 0 ] || { echo "  FAIL $e ERROR/CRIT on nodes 1-2: $(grep -hE ' (ERROR|CRIT)' "$D"/n1.log "$D"/n2.log | head -2)"; rc=1; }
exit $rc
