#!/bin/sh
# eagercatchuptest.sh - T600: a returning eager node whose peers hold more records than it ever will
# (PROD: 7k the clients never read) still settles - once its pulls' answers stop finding anything,
# its misses are final again instead of each parking on a pull for the rest of its life.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 17031 17032 17033 17034 17035 17036; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "eagercatchuptest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcec.XXXXXX)
trap 'for f in "$D"/n*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM

conf() {
	mkdir -p "$D/s$1/wal"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = ec-client-secret
cluster = ec-cluster-secret
[listen]
tcp = 127.0.0.1:1703$1
http = 127.0.0.1:1703$(($1 + 3))
plaintext = loopback
[cluster]
multicast = 239.255.79.46:17030
advertise = 127.0.45.$1
pull_timeout_ms = 400
mode = eager
collections = c
[collection c]
buckets_log2 = 12
[wal]
dir = $D/s$1/wal
probe = no
fsync = everysec
segment_mb = 16
segments = 4
save = off
C
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> [env]: node 1 runs with the bias - a peer ahead of node 3 for good
	L="$D/n$1.log"
	env ${2:-PC_NOTHING=1} "$BIN" -f "$D/n$1.conf" > "$L" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 300 ]; do
		grep -q "perfcached ready" "$L" && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "  node $1 did not start: $(tail -2 "$L" | tr '\n' ' ')"; return 1
}
st() { # st <n> <python expr over r>
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (17033 + int(sys.argv[1])),timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
formed() {
	i=0; while [ $i -lt 120 ]; do
		[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ] && \
			[ "$(st 1 'r["cluster"]["peers_up"]')$(st 2 'r["cluster"]["peers_up"]')$(st 3 'r["cluster"]["peers_up"]')" = 222 ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}
# ops <n> <method> <prefix> <count>: set stores <prefix><i>; get reads them, printing how many found
ops() { python3 - "1703$1" "$2" "$3" "$4" <<'PY'
import json, pcnative, socket, sys
port, meth, pre, n = int(sys.argv[1]), sys.argv[2], sys.argv[3], int(sys.argv[4])
s = socket.create_connection(("127.0.0.1", port), timeout=120); f = pcnative.wrap(s); got = 0
for x in range(0, n, 200):
    rq = []
    for i in range(x, min(n, x + 200)):
        p = {"col": "c", "key": "%s%05d" % (pre, i)}
        if meth == "set":
            p["value"] = "v%05d" % i
        rq.append(json.dumps({"jsonrpc": "2.0", "id": i, "method": meth, "params": p}))
    f.write(("\n".join(rq) + "\n").encode()); f.flush()
    for _ in rq:
        r = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if "id" in _m).get("result") or {}
        got += 1 if (r.get("stored") or r.get("value")) else 0
print(got)
PY
}
fin() { st 3 'r["cluster"].get("eager_miss_final")'; }

for n in 1 2 3; do conf $n; done
start 1 PERFCACHED_TEST_ENTRIES_BIAS=500 || exit 1
start 2 || exit 1
start 3 || exit 1
formed || { bad "fleet did not form"; exit 1; }
grep -q "PERFCACHED_TEST_ENTRIES_BIAS=500" "$D/n1.log" && ok "node 1 claims 500 records it does not hold (the test flag)" \
	|| bad "node 1 never said it runs with the bias"
[ "$(ops 1 set k 200)" = 200 ] && ok "200 keys written on node 1" || bad "the writes were not all stored"
i=0; while [ "$(st 3 '[c for c in r["collections"] if c["name"]=="c"][0]["entries"]')" != 200 ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i+1)); done
[ "$(ops 3 get k 200)" = 200 ] && ok "node 3 holds all 200" || bad "node 3 reads $(ops 3 get k 200) of 200"

# past the 30 s settle window the counts still say node 3 is behind node 1
t0=$(( $(date +%s) - 1 ))   # node 3 settled from its start, just before the fleet formed
sleep 35
[ "$(fin)" = False ] && ok "1. behind node 1 by count after the settle window: node 3's misses still ask (eager_miss_final false)" \
	|| bad "1. eager_miss_final $(fin) - want false while node 1 claims more and no pull has answered"
# inside its first 120 s a burst of empty answers decides nothing: the sweeps' re-sends land first
[ "$(ops 3 get early 1200)" = 0 ] && sleep 3 && [ "$(fin)" = False ] \
	&& ok "1. 1,200 empty pulls inside the first 120 s leave it asking" || bad "1. eager_miss_final $(fin) after an early burst - want false before 120 s"

# after 120 s: misses on keys nobody holds, every peer answers 'not here'
while [ $(( $(date +%s) - t0 )) -lt 125 ]; do sleep 1; done
p0=$(st 3 'r["cluster"]["pull_sent"]')
# a pull's answers count once, the fleet's verdict: 1,200 misses fill a window of 1,000
[ "$(ops 3 get absent 1200)" = 0 ] && ok "2. 1,200 reads of absent keys on node 3 answer absent" || bad "2. an absent key read as found"
i=0; while [ "$(fin)" != True ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i+1)); done
p1=$(st 3 'r["cluster"]["pull_sent"]')
[ "$(fin)" = True ] && ok "2. its pulls found nothing it lacks: node 3's misses are final ($((p1 - p0)) pulls sent for them)" \
	|| bad "2. eager_miss_final $(fin) after $((p1 - p0)) pulls that all came back empty (want true once 1,000 answers are in)"
grep -q "found nothing this node lacks; an eager miss here is final" "$D/n3.log" && ok "2. node 3 logs why: $(grep -o 'caught up - holding [0-9]* record(s) to a peer.s [0-9]*' "$D/n3.log" | head -1)" \
	|| bad "2. no caught-up line on node 3: $(grep 'caught up' "$D/n3.log" | tail -1)"
[ "$(ops 3 get absent2 300)" = 0 ] && p2=$(st 3 'r["cluster"]["pull_sent"]') && [ "$((p2 - p1))" -lt 30 ] \
	&& ok "3. 300 more misses on node 3 send $((p2 - p1)) pulls" || bad "3. 300 more misses sent $((p2 - ${p1:-0})) pulls - still asking"
[ "$(ops 3 get k 200)" = 200 ] && ok "3. and it still reads all 200 keys" || bad "3. node 3 lost keys"
e=$(cat "$D"/n*.log | grep -E ' (ERROR|CRIT)' | grep -cv 'an fsync has been stuck')
[ "$e" = 0 ] && ok "no ERROR/CRIT" || { bad "$e ERROR/CRIT lines"; cat "$D"/n*.log | grep -E ' (ERROR|CRIT)' | sort | uniq -c | head -5; }
for n in 1 2 3; do p=$(cat "$D/n$n.pid"); kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; rm -f "$D/n$n.pid"; done
echo "eagercatchuptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
