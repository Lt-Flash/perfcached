#!/bin/sh
# promotepulltest.sh - a member still pulling its bootstrap that is elected master stays RECOVERING
# until the pull settles: READY means complete, and mastership does not change what it holds.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcpromopull.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
if "$BIN" -V 2>/dev/null | grep -q standalone; then
	echo "  SKIP the standalone edition refuses a [cluster] section"
	echo "promotepulltest: 0 passed, 0 failed, 1 skipped"; exit "${PC_SKIP_FAILS:-0}"
fi
for p in 28651 28652 28661 28662; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "promotepulltest: port $p busy" >&2; exit 1; }
done

conf() {
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = pp-client-secret
cluster = pp-cluster-secret
[listen]
tcp = 127.0.0.1:2865$1
http = 127.0.0.1:2866$1
plaintext = loopback
[cluster]
multicast = 239.255.97.34:28696
advertise = 127.0.233.$1
[collection c]
buckets_log2 = 10
mode = eager
C
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> [VAR=value ...]
	n=$1; shift
	: > "$D/n$n.log"
	env "$@" "$BIN" -f "$D/n$n.conf" > "$D/n$n.log" 2>&1 &
	echo $! > "$D/n$n.pid"
	i=0; while [ $i -lt 300 ]; do grep -q "perfcached ready" "$D/n$n.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "node $n did not start: $(tail -2 "$D/n$n.log" | tr '\n' ' ')"; return 1
}
st() { python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:2866%s/stats" % sys.argv[1],timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"; }
state() { st $1 'r["state"]'; }
role() { st $1 'r["cluster"]["role"]'; }
wait_for() { # wait_for <s> <condition>
	k=0; while [ $k -lt $(($1 * 2)) ]; do eval "$2" && return 0; sleep 0.5; k=$((k+1)); done; return 1
}
fill() { python3 - <<'PY'
import json, pcnative, socket
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 28651), timeout=30))
for i in range(200):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": "set", "params": {"col": "c", "key": "k%04d" % i, "value": "v%04d" % i}}) + "\n").encode())
f.flush()
print(sum(1 for i in range(200) if (json.loads(f.readline()).get("result") or {}).get("stored")))
PY
}

conf 1; conf 2
start 1 || exit 1
wait_for 30 '[ "$(role 1)" = master ] && [ "$(state 1)" = ready ]' || bad "node 1 did not found the fleet: $(role 1)/$(state 1)"
F=$(fill); [ "$F" = 200 ] && ok "node 1 founded and holds 200 records" || bad "node 1 stored $F of 200"
# node 2 joins holding nothing; its pull is picked, then held 40 s on the bulk thread
start 2 PERFCACHED_TEST_BOOT_DELAY_MS=40000 || exit 1
wait_for 30 '[ "$(role 2)" = member ] && [ "$(state 2)" = recovering ]' \
	&& ok "node 2 joined holding nothing and is RECOVERING, its bootstrap pull held" \
	|| bad "node 2 is $(role 2)/$(state 2), not a member pulling its bootstrap"
# an admitted member elects only 8 s after it went ONLINE
sleep 10
[ "$(state 2)" = recovering ] || bad "node 2 left RECOVERING before the election: $(state 2) - the arm tests nothing"
kill "$(cat "$D/n1.pid")"                    # a goodbye: node 2 elects at once
wait_for 20 '[ "$(role 2)" = master ]' && ok "node 1 stopped; node 2 took mastership" || bad "node 2 did not take mastership: $(role 2)"
S=$(state 2)
[ "$S" = recovering ] && ok "node 2, master now, is still RECOVERING - its pull has not settled" \
	|| bad "node 2 became master and reports $S while its bootstrap pull is still held - READY with $(st 2 '[c for c in r["collections"] if c["name"] == "c"][0]["entries"]') of 200 records"
# the held pull wakes, finds node 1 gone, and the bootstrap gives up: ready, relying on the push
wait_for 90 '[ "$(state 2)" = ready ]' && ok "once the pull settled node 2 is ready ($(grep -c 'relying on the push' "$D/n2.log") give-up line(s))" \
	|| bad "node 2 is still $(state 2) 90 s after node 1 stopped - mastership left the bootstrap undriven"
echo "promotepulltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
