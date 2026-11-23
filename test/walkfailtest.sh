#!/bin/sh
# walkfailtest.sh - a repair sweep whose walk could not run (no memory for its buffers) is no clean
# cycle: the peer's mark stays, and the records it missed go once the walks run again.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
SHIM=$(cd "$(dirname "${2:-./netcutshim.so}")" && pwd)/$(basename "${2:-./netcutshim.so}")
D=$(mktemp -d /var/tmp/pcwalkfail.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
if "$BIN" -V 2>/dev/null | grep -q standalone; then
	echo "  SKIP the standalone edition refuses a [cluster] section"
	echo "walkfailtest: 0 passed, 0 failed, 1 skipped"; exit "${PC_SKIP_FAILS:-0}"
fi
for p in 28631 28632 28641 28642; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "walkfailtest: port $p busy" >&2; exit 1; }
done
[ -f "$SHIM" ] || { echo "walkfailtest: no shim at $SHIM" >&2; exit 1; }
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
PRELOAD="${SANRT:+$SANRT }$SHIM"
NOMEM_MS=50000

conf() {
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = wf-client-secret
cluster = wf-cluster-secret
[listen]
tcp = 127.0.0.1:2863$1
http = 127.0.0.1:2864$1
plaintext = loopback
[cluster]
multicast = 239.255.97.33:28695
advertise = 127.0.232.$1
sweep_digests = no
[collection c]
buckets_log2 = 12
autoscale = off
mode = eager
C
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> [VAR=value ...]
	n=$1; shift
	: > "$D/cut$n"
	env "$@" LD_PRELOAD="$PRELOAD${PC_NOSYNC_LIB:+ $PC_NOSYNC_LIB}" PC_NETCUT_CTL="$D/cut$n" PC_NETCUT_LOG="$D/cutlog$n" \
		"$BIN" -f "$D/n$n.conf" > "$D/n$n.log" 2>&1 &
	echo $! > "$D/n$n.pid"
	i=0; while [ $i -lt 300 ]; do grep -q "perfcached ready" "$D/n$n.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "node $n did not start: $(tail -2 "$D/n$n.log" | tr '\n' ' ')"; return 1
}
st() { python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:2864%s/stats" % sys.argv[1],timeout=5).read())
x=r["cluster"]; c=[y for y in r["collections"] if y["name"] == "c"][0]
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"; }
ent() { st $1 'c["entries"]'; }
fill() { python3 - "$1" "$2" <<'PY'
import json, pcnative, socket, sys
a, b = int(sys.argv[1]), int(sys.argv[2])
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 28631), timeout=30))
for i in range(a, b):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": "set", "params": {"col": "c", "key": "k%05d" % i, "value": "v%05d" % i}}) + "\n").encode())
f.flush()
print(sum(1 for i in range(a, b) if (json.loads(f.readline()).get("result") or {}).get("stored")))
PY
}

conf 1; conf 2
T0=$(date +%s)
start 1 PERFCACHED_TEST_WALK_NOMEM_MS=$NOMEM_MS || exit 1
start 2 || exit 1
i=0; while [ $i -lt 60 ]; do
	[ "$(st 1 'r["state"]')$(st 2 'r["state"]')" = readyready ] && [ "$(st 1 'x["peers_up"]')$(st 2 'x["peers_up"]')" = 11 ] && break
	sleep 0.5; i=$((i+1))
done
[ "$(st 1 'r["state"]')$(st 2 'r["state"]')" = readyready ] && ok "the pair formed" || bad "the pair did not form: $(st 1 'r["state"]')/$(st 2 'r["state"]')"
grep -q "PERFCACHED_TEST_WALK_NOMEM_MS" "$D/n1.log" && ok "node 1 announces the test flag" || bad "node 1 does not announce PERFCACHED_TEST_WALK_NOMEM_MS"
F=$(fill 0 100); [ "$F" = 100 ] || bad "stored $F of the first 100"
# the push to node 2 runs direct first: while node 2 is not yet usable, pushes wait and go once it
# is, and a cut lifted before then lets them all through
i=0; while [ $i -lt 60 ] && [ "$(ent 2)" != 100 ]; do sleep 0.5; i=$((i+1)); done
[ "$(ent 2)" = 100 ] && ok "node 1's pushes reach node 2 (100 of 100)" || bad "node 2 holds $(ent 2) of node 1's first 100 writes after 30 s"
# node 2 cut off for 300 writes: only the sweep can bring them, and node 1's walks fail meanwhile;
# the cut stands a second past the writes, so no push in flight goes after it
echo "cut 127.0.232.1 rst" > "$D/cut2"; echo "cut 127.0.232.2 rst" > "$D/cut1"
sleep 0.5
F=$(fill 100 400)
sleep 1
: > "$D/cut2"; : > "$D/cut1"
sleep 1
E=$(ent 2)
[ "$F" = 300 ] && [ "$E" -lt 400 ] 2>/dev/null && ok "300 writes on node 1 while node 2 was cut off: node 2 holds $E of 400" \
	|| bad "the cut did not hold: stored $F, node 2 holds $E of 400 - the arm tests nothing"
LEFT=$(( T0 + NOMEM_MS / 1000 - $(date +%s) ))
[ "$LEFT" -gt 10 ] && ok "node 1's sweep walks fail for ${LEFT} s more - more than one sweep round" \
	|| bad "only ${LEFT} s of failing walks left - the setup was too slow for the arm"
# the walks run again at T0 + NOMEM_MS; two sweep rounds later every record must be on node 2
END=$(( T0 + NOMEM_MS / 1000 + 35 ))
while [ "$(date +%s)" -lt "$END" ] && [ "$(ent 2)" != 400 ]; do sleep 1; done
E=$(ent 2)
[ "$E" = 400 ] && ok "node 2 holds all 400 $(( $(date +%s) - T0 - NOMEM_MS / 1000 )) s after node 1's walks run again" \
	|| bad "node 2 holds $E of 400 35 s after node 1's walks run again - a sweep whose walk failed was taken for a clean cycle and the mark passed the records it never sent"
echo "walkfailtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
