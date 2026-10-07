#!/bin/sh
# proxylocatortest.sh - S321: a proxy node's locator can be stale, and a
# miss on the node it names is not the fleet's answer.
#
# Proxy keeps one copy per key; a node that read a key through a peer
# remembers the holder (the locator) and asks it directly next time.  A
# key deleted on its holder and written again through another node left
# that entry pointing at a node without it, and 0.4.6 answered the miss
# there as final: GET said absent, DEL and EXPIRE said "nothing there",
# for a key the fleet held (DESIGN 12im).  Now the steered miss forgets
# the entry and asks the fleet once.  A three-node proxy fleet, each
# node on its own loopback address, the RESP door:
#   1  GET, 2  DEL, 3  EXPIRE through node 1 after the key moved from
#      node 2 to node 3 - right answers, and the effect lands on node 3
#   4  a key absent everywhere still reads absent through a stale entry
# Each move waits out the 2 s tombstone a delete leaves (a separate,
# designed behaviour: a delete wins for tombstone_ms).
# Fail-first: 0.4.6 answers nil, 0 and 0 to legs 1-3.
# Usage: test/proxylocatortest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for n in 1 2 3; do
	ss -ltn 2>/dev/null | grep -q "127.0.46.$n:17777[[:space:]]" && { echo "proxylocatortest: 127.0.46.$n busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcploc.XXXXXX)
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
client = ploc-client-secret
cluster = ploc-cluster-secret
[listen]
tcp = 127.0.46.$n:17886
resp = 127.0.46.$n:17777
http = 127.0.46.$n:17778
plaintext = loopback
[cluster]
multicast = 239.255.80.46:17880
advertise = 127.0.46.$n
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
	: > "$D/n$n.log"
	"$BIN" -f "$D/n$n.conf" >> "$D/n$n.log" 2>&1 &
	echo $! > "$D/n$n.pid"
done
for n in 1 2 3; do
	i=0; while [ $i -lt 300 ] && ! grep -q "perfcached ready" "$D/n$n.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/n$n.log" || { echo "proxylocatortest: node $n did not start: $(tail -2 "$D/n$n.log")"; exit 1; }
done
i=0; while [ $i -lt 120 ]; do
	good=1
	for n in 1 2 3; do
		c=$(curl -s "http://127.0.46.$n:17778/members" | python3 -c 'import json,sys; print(sum(1 for m in json.load(sys.stdin).get("members",[]) if m.get("gone_s",-1) < 0 and m.get("state") == "ready"))' 2>/dev/null)
		[ "$c" = 3 ] || good=0
	done
	[ $good = 1 ] && break
	sleep 0.5; i=$((i+1))
done
[ $good = 1 ] || { bad "the proxy fleet did not form"; echo "proxylocatortest: $pass passed, $fail failed"; exit 1; }
sleep 2
# r <n> <args...>: one RESP command on node n, flattened
r() { n=$1; shift; python3 - "127.0.46.$n" "$@" <<'PY'
import socket, sys
s = socket.create_connection((sys.argv[1], 17777), timeout=10)
a = sys.argv[2:]
s.sendall(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x.encode()), x) for x in a)).encode())
f = s.makefile("rb")
l = f.readline().decode().rstrip("\r\n")
t, v = l[0], l[1:]
print(("nil" if v == "-1" else f.read(int(v) + 2)[:-2].decode()) if t == "$" else v if t in ":+" else "-" + v)
PY
}
eq() { [ "$2" = "$3" ] && ok "$1: $2" || bad "$1: got '$2', want '$3'"; }
# stale <key>: written through node 2, read through node 1 (node 1's
# locator now names node 2), then moved to node 3
stale() {
	r 2 SET "$1" first > /dev/null
	[ "$(r 1 GET "$1")" = first ] || bad "$1: the first read through node 1"
	r 2 DEL "$1" > /dev/null
	r 3 SET "$1" moved > /dev/null
}
stale k1; stale k2; stale k3
r 2 SET k4 gone > /dev/null; r 1 GET k4 > /dev/null; r 2 DEL k4 > /dev/null
sleep 3                                # past the deletes' 2 s tombstones
eq "1. GET through node 1, its locator stale" "$(r 1 GET k1)" moved
eq "2. DEL through node 1, its locator stale" "$(r 1 DEL k2)" 1
eq "   and the key is gone from node 3" "$(r 3 EXISTS k2)" 0
eq "3. EXPIRE through node 1, its locator stale" "$(r 1 EXPIRE k3 100)" 1
t=$(r 3 TTL k3)
[ "$t" -ge 95 ] 2>/dev/null && [ "$t" -le 100 ] && ok "   and node 3's copy took the TTL ($t)" || bad "   node 3's TTL after the EXPIRE: $t"
eq "4. a key absent everywhere, through a stale entry" "$(r 1 GET k4)" nil
e=$(cat "$D"/n*.log | grep -cE ' (ERROR|CRIT)')
[ "$e" = 0 ] && ok "no ERROR/CRIT" || bad "$e ERROR/CRIT: $(grep -hE ' (ERROR|CRIT)' "$D"/n*.log | head -2)"
echo "proxylocatortest: $pass passed, $fail failed"
[ $fail -eq 0 ]
