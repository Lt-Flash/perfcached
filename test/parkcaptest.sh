#!/bin/sh
# parkcaptest.sh - a parked reply that does not fit the output cap closes the connection; it never leaves
# its frame's header staged for the next frames to be read as its payload (PERFCACHED_TEST_OUTQ lowers the cap).
set -u
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for n in 1 2; do
	ss -ltn 2>/dev/null | grep -q "127.0.207.$n:17906[[:space:]]" && { echo "parkcaptest: 127.0.207.$n busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcpcap.XXXXXX)
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
client = pcap-client-secret
cluster = pcap-cluster-secret
[listen]
tcp = 127.0.207.$n:17906
http = 127.0.207.$n:17798
plaintext = loopback
[cluster]
multicast = 239.255.82.7:17900
advertise = 127.0.207.$n
mode = proxy
collections = sessions
[collection sessions]
buckets_log2 = 12
C
	chmod 600 "$D/n$n.conf"
	: > "$D/n$n.log"
	if [ $n = 2 ]; then
		PERFCACHED_TEST_OUTQ=16384 "$BIN" -f "$D/n$n.conf" >> "$D/n$n.log" 2>&1 &
	else
		"$BIN" -f "$D/n$n.conf" >> "$D/n$n.log" 2>&1 &
	fi
	echo $! > "$D/n$n.pid"
done
for n in 1 2; do
	i=0; while [ $i -lt 300 ] && ! grep -q "perfcached ready" "$D/n$n.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/n$n.log" || { echo "parkcaptest: node $n did not start: $(tail -2 "$D/n$n.log")"; exit 1; }
done
i=0; while [ $i -lt 120 ]; do
	good=1
	for n in 1 2; do
		c=$(curl -s "http://127.0.207.$n:17798/members" | python3 -c 'import json,sys; print(sum(1 for m in json.load(sys.stdin).get("members",[]) if m.get("gone_s",-1) < 0 and m.get("state") == "ready"))' 2>/dev/null)
		[ "$c" = 2 ] || good=0
	done
	[ $good = 1 ] && break
	sleep 0.5; i=$((i+1))
done
[ $good = 1 ] || { bad "the proxy fleet did not form"; echo "parkcaptest: $pass passed, $fail failed"; exit 1; }
sleep 2

# a 40 KB value on node 1; node 2 (output cap 16 KB) serves a get of it by pulling, so the
# reply is staged on completion: header first, then a payload past the cap
R=$(python3 - "$T" <<'PY'
import json, socket, sys
sys.path.insert(0, sys.argv[1])
import pcnative
def conn(n):
    s = socket.create_connection(("127.0.207.%d" % n, 17906), timeout=5)
    return s, pcnative.wrap(s)
def call(f, rid, method, **p):
    f.write(json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": p}).encode() + b"\n")
    f.flush()
s1, f1 = conn(1)
call(f1, 1, "set", col="sessions", key="big", value="x" * 40000)
f1.readline()
s2, f2 = conn(2)
call(f2, 1, "set", col="sessions", key="small", value="s")
f2.readline()
# the get that parks, then one node 2 answers at once; frames match by id, so the small
# one may come first - the big one must then be the connection's end, not a lone header
call(f2, 2, "get", col="sessions", key="big")
call(f2, 3, "get", col="sessions", key="small")
out = "both answered"
for _ in range(2):
    try:
        line = f2.readline()
    except Exception as e:
        out = "stuck:" + type(e).__name__
        break
    if not line:
        out = "eof"
        break
    if json.loads(line).get("id") == 2:
        out = "big answered: " + line[:60].decode("latin-1", "replace")
        break
print(out)
PY
)
case "$R" in
eof) ok "a parked reply past the output cap closes the connection";;
*) bad "a parked reply past the output cap: '$R' - the connection kept, its frame's header staged without the payload";;
esac
grep -q "slow consumer" "$D/n2.log" && ok "and node 2 says why (slow consumer)" ||
	bad "node 2 logged no slow consumer: $(grep disconnected "$D/n2.log" | tail -1)"
curl -s "http://127.0.207.2:17798/members" > /dev/null && ok "node 2 still serves" || bad "node 2 died"

echo "parkcaptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
