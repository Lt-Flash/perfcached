#!/bin/sh
# reshardtailtest.sh - a shard join moves every key the joiner now owns: values too big for one batch
# are held for the next tick, never dropped, and a table the walk cannot finish in one tick moves its tail.
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrst.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
if "$BIN" -V 2>/dev/null | grep -q standalone; then
	echo "  SKIP the standalone edition refuses a [cluster] section"
	echo "reshardtailtest: 0 passed, 0 failed, 1 skipped"; exit "${PC_SKIP_FAILS:-0}"
fi
for p in 28611 28612 28613 28614 28621 28622 28623 28624; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "reshardtailtest: port $p busy" >&2; exit 1; }
done

conf() { # conf <n> <multicast port>: RESP 2861<n>, HTTP 2862<n>, advertises 127.0.231.<n>
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 128
[secrets]
client = rst-client-secret
cluster = rst-cluster-secret
[listen]
resp = 127.0.0.1:2861$1
http = 127.0.0.1:2862$1
plaintext = loopback
[cluster]
multicast = 239.255.97.31:$2
advertise = 127.0.231.$1
[collection 0]
buckets_log2 = 12
mode = shard
C
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> [VAR=value ...]
	n=$1; shift
	env "$@" "$BIN" -f "$D/n$n.conf" > "$D/n$n.log" 2>&1 &
	echo $! > "$D/n$n.pid"
	i=0
	while [ $i -lt 300 ]; do
		grep -q "perfcached ready" "$D/n$n.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $n did not start: $(tail -3 "$D/n$n.log" | tr '\n' '|')"; return 1
}

# arm <first node> <keys> <value bytes> <wait s>: the first node alone writes the keys, the second
# joins; prints "<entries first> <entries second> <ready>" once the census stood still 15 s
arm() {
	python3 - "$@" <<'PY'
import json, socket, sys, time, urllib.request
a, nkeys, vlen, wait = (int(x) for x in sys.argv[1:5])
b = a + 1
def stats(n):
    try:
        return json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (28620 + n), timeout=10).read())
    except Exception:
        return None
def entries(n):
    s = stats(n)
    return s["collections"][0]["entries"] if s else -1
end = time.time() + 30
while time.time() < end and (stats(a) or {}).get("state") != "ready":
    time.sleep(0.2)
s = socket.create_connection(("127.0.0.1", 28610 + a), 8); s.settimeout(60)
f = s.makefile("rwb")
def cmd(*xs):
    out = b"*%d\r\n" % len(xs)
    for x in xs:
        if isinstance(x, str): x = x.encode()
        out += b"$%d\r\n%s\r\n" % (len(x), x)
    s.sendall(out)
    return f.readline()
bad = 0
for i in range(nkeys):
    v = (b"%d-" % i) * (vlen // 2 + 1)
    if not cmd("SET", "rk%05d" % i, v[:vlen]).startswith(b"+OK"):
        bad += 1
print("WROTE %d %d" % (nkeys - bad, entries(a)), flush=True)
open(sys.argv[5], "w").close()           # the shell starts the joiner now
end = time.time() + wait
last, since = None, time.time()
while time.time() < end:
    cur = (entries(a), entries(b))
    if cur != last:
        last, since = cur, time.time()
    elif cur[1] > 0 and time.time() - since >= 15:
        break
    time.sleep(1)
sb = stats(b)
print("CENSUS %d %d %s" % (entries(a), entries(b), sb["state"] if sb else "?"))
PY
}
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
# run_arm <first node> <keys> <value bytes> <wait s> [VAR=value ...] - sets WROTE and CENSUS
run_arm() {
	a=$1 k=$2 v=$3 w=$4; shift 4
	conf $a $((28690 + a)); conf $((a + 1)) $((28690 + a))
	start $a "$@" || return 1
	rm -f "$D/go$a"
	arm $a $k $v $w "$D/go$a" > "$D/arm$a.out" 2>&1 &
	ap=$!
	i=0; while [ $i -lt 600 ] && [ ! -f "$D/go$a" ]; do sleep 0.1; i=$((i+1)); done
	start $((a + 1)) || return 1
	wait $ap
	WROTE=$(sed -n 's/^WROTE //p' "$D/arm$a.out")
	CENSUS=$(sed -n 's/^CENSUS //p' "$D/arm$a.out")
	[ -n "$CENSUS" ] || { echo "   the driver said: $(tail -3 "$D/arm$a.out" | tr '\n' '|')"; return 1; }
}

echo "--- A: 60 values of 255,000 bytes - a 4 MiB batch holds 16, the rest wait for the next tick"
if run_arm 1 60 255000 120; then
	set -- $CENSUS
	[ "${WROTE%% *}" = 60 ] && ok "node 1 alone took all 60 writes" || bad "node 1 took ${WROTE%% *} of 60 writes"
	[ "$2" -gt 16 ] && ok "node 2 joined and holds $2 of the keys ($3) - more than one batch moved" \
		|| bad "node 2 holds $2 keys ($3) - the arm needs more than one batch's worth owed to it"
	[ $(($1 + $2)) = 60 ] && ok "nothing lost in the move: node 1 $1 + node 2 $2 = 60" \
		|| bad "node 1 $1 + node 2 $2 = $(($1 + $2)) of 60 - a record the batch had no room for was removed and never sent"
else
	bad "arm A did not run"
fi

echo "--- B: 2000 keys, each tick's walk stopped after 500 records (PERFCACHED_TEST_MIG_SCAN_RECS)"
if run_arm 3 2000 100 150 PERFCACHED_TEST_MIG_SCAN_RECS=500; then
	set -- $CENSUS
	grep -q "PERFCACHED_TEST_MIG_SCAN_RECS" "$D/n3.log" && ok "node 3 announces the test flag" || bad "node 3 does not announce PERFCACHED_TEST_MIG_SCAN_RECS"
	[ $(($1 + $2)) = 2000 ] && ok "nothing lost: node 3 $1 + node 4 $2 = 2000" || bad "node 3 $1 + node 4 $2 = $(($1 + $2)) of 2000"
	[ "$2" -ge 800 ] && ok "node 4 holds $2 keys - its share moved, the walk's tail too" \
		|| bad "node 4 holds $2 keys of its ~1000 - each tick's walk began again at bucket 0 and stopped at the same horizon, so the tail never moved"
else
	bad "arm B did not run"
fi

echo "reshardtailtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
