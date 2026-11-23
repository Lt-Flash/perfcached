#!/bin/sh
# respparkmaptest.sh - a RESP command that parks on a proxy fleet completes on the collection its SELECT
# index maps to (resp_collections = 0:sessions), not on a collection named after the index.
set -u
BIN=${1:-./perfcached}
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for n in 1 2; do
	ss -ltn 2>/dev/null | grep -q "127.0.203.$n:17787[[:space:]]" && { echo "respparkmaptest: 127.0.203.$n busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcrpm.XXXXXX)
trap 'for f in "$D"/n*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
for n in 1 2; do
	mkdir -p "$D/s$n"
	cat > "$D/n$n.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$n
[memory]
arena_mb = 64
[secrets]
client = rpm-client-secret
cluster = rpm-cluster-secret
[listen]
tcp = 127.0.203.$n:17896
resp = 127.0.203.$n:17787
http = 127.0.203.$n:17788
plaintext = loopback
resp_collections = 0:sessions
[cluster]
multicast = 239.255.82.3:17890
advertise = 127.0.203.$n
mode = proxy
collections = sessions
[collection sessions]
buckets_log2 = 12
C
	chmod 600 "$D/n$n.conf"
	: > "$D/n$n.log"
	"$BIN" -f "$D/n$n.conf" >> "$D/n$n.log" 2>&1 &
	echo $! > "$D/n$n.pid"
done
for n in 1 2; do
	i=0; while [ $i -lt 300 ] && ! grep -q "perfcached ready" "$D/n$n.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/n$n.log" || { echo "respparkmaptest: node $n did not start: $(tail -2 "$D/n$n.log")"; exit 1; }
done
i=0; while [ $i -lt 120 ]; do
	good=1
	for n in 1 2; do
		c=$(curl -s "http://127.0.203.$n:17788/members" | python3 -c 'import json,sys; print(sum(1 for m in json.load(sys.stdin).get("members",[]) if m.get("gone_s",-1) < 0 and m.get("state") == "ready"))' 2>/dev/null)
		[ "$c" = 2 ] || good=0
	done
	[ $good = 1 ] && break
	sleep 0.5; i=$((i+1))
done
[ $good = 1 ] || { bad "the proxy fleet did not form"; echo "respparkmaptest: $pass passed, $fail failed"; exit 1; }
sleep 2
# r <n> <args...>: one RESP command on node n, flattened
r() { n=$1; shift; python3 - "127.0.203.$n" "$@" <<'PY'
import socket, sys
s = socket.create_connection((sys.argv[1], 17787), timeout=10)
a = sys.argv[2:]
s.sendall(("*%d\r\n" % len(a) + "".join("$%d\r\n%s\r\n" % (len(x.encode()), x) for x in a)).encode())
f = s.makefile("rb")
l = f.readline().decode().rstrip("\r\n")
t, v = l[0], l[1:]
print(("nil" if v == "-1" else f.read(int(v) + 2)[:-2].decode()) if t == "$" else v if t in ":+" else "-" + v)
PY
}
eq() { [ "$2" = "$3" ] && ok "$1: $2" || bad "$1: got '$2', want '$3'"; }

# a new key on a proxy node asks the fleet first (the command parks), then stores on
# the mapped collection when every peer says absent
eq "SET of a new key through SELECT 0 (parks, then stores in sessions)" "$(r 1 SET pm:new v1)" "OK"
eq "GET it back on the same node" "$(r 1 GET pm:new)" "v1"
eq "GET it through the other node" "$(r 2 GET pm:new)" "v1"
eq "INCR of a new key parks and counts in sessions" "$(r 1 INCR pm:cnt)" "1"
eq "and again" "$(r 1 INCR pm:cnt)" "2"

echo "respparkmaptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
