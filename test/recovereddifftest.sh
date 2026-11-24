#!/bin/sh
# recovereddifftest.sh - an eager node restarted from its WAL gets a diff from its peers: copies it lacks
# or holds older, which no live peer authors, reach it without a read and without a cold restart.
# Usage: test/recovereddifftest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 16981 16982 16983 16984 16985 16986; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "recovereddifftest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcrd.XXXXXX)
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
client = rd-client-secret
cluster = rd-cluster-secret
[listen]
tcp = 127.0.0.1:1698$1
http = 127.0.0.1:1698$(($1 + 3))
plaintext = loopback
[cluster]
multicast = 239.255.79.63:16980
advertise = 127.0.63.$1
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
start() { # start <n>
	L="$D/n$1.log"
	: > "$L"
	"$BIN" -f "$D/n$1.conf" > "$L" 2>&1 &
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
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (16983 + int(sys.argv[1])),timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
ent() { st "$1" '[c for c in r["collections"] if c["name"]=="c"][0]["entries"]'; }
waitfor() { w=0; while [ $w -lt $(($1 * 4)) ]; do eval "$2" && return 0; sleep 0.25; w=$((w+1)); done; return 1; }
# ops <n> <method> <prefix> <count> [value]: set stores <prefix><i>; get prints how many read [value]
ops() { python3 - "1698$1" "$2" "$3" "$4" "${5:-v}" <<'PY'
import json, pcnative, socket, sys
port, meth, pre, n, val = int(sys.argv[1]), sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
s = socket.create_connection(("127.0.0.1", port), timeout=120); f = pcnative.wrap(s); got = 0
for x in range(0, n, 200):
    rq = []
    for i in range(x, min(n, x + 200)):
        p = {"col": "c", "key": "%s%05d" % (pre, i)}
        if meth == "set":
            p["value"] = val
        rq.append(json.dumps({"jsonrpc": "2.0", "id": i, "method": meth, "params": p}))
    f.write(("\n".join(rq) + "\n").encode()); f.flush()
    for _ in rq:
        r = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if "id" in _m).get("result") or {}
        got += 1 if (r.get("stored") or r.get("value") == val) else 0
print(got)
PY
}
stop() { # stop <n> <signal>
	p=$(cat "$D/n$1.pid"); kill "-$2" "$p" 2>/dev/null; wait "$p" 2>/dev/null; rm -f "$D/n$1.pid"
}

for n in 1 2 3; do conf $n; done
start 1 && start 2 && start 3 || exit 1
waitfor 30 '[ "$(st 1 "r[\"cluster\"][\"peers_up\"]")$(st 2 "r[\"cluster\"][\"peers_up\"]")$(st 3 "r[\"cluster\"][\"peers_up\"]")" = 222 ]' \
	|| { bad "the fleet did not form"; exit 1; }
[ "$(ops 1 set k 200)" = 200 ] && [ "$(ops 1 set o 50 old)" = 50 ] && ok "250 keys written on node 1" || bad "the first writes were not all stored"
waitfor 30 '[ "$(ent 3)" = 250 ]' && ok "node 3 holds all 250" || bad "node 3 holds $(ent 3) of 250"
sleep 2                                   # node 3's WAL synced (fsync = everysec)

# node 3 down; node 2 writes 300 new keys and overwrites 50; node 2 then leaves for good, so on node 1
# all of these are copies whose author is gone - no live peer's sweep sends a copy
stop 3 9
[ "$(ops 2 set n 300)" = 300 ] && [ "$(ops 2 set o 50 new)" = 50 ] && ok "while node 3 was down: 300 new keys and 50 overwrites through node 2" \
	|| bad "node 2's writes were not all stored"
waitfor 30 '[ "$(ent 1)" = 550 ]' && ok "node 1 holds 550" || bad "node 1 holds $(ent 1) of 550"
stop 2 TERM

start 3 || exit 1
grep -q "node .* restarted holding .* recovered" "$D/n1.log" && ok "node 1 saw node 3 restart RECOVERED (from its WAL)" \
	|| bad "node 1 never saw a recovered restart: $(grep 'node .* restarted\|started cold' "$D/n1.log" | tail -1)"
# counted before any read: a read of a key it lacks would pull it
waitfor 60 '[ "$(ent 3)" = 550 ]'
[ "$(ent 3)" = 550 ] && ok "node 3 holds all 550 without a read - the 300 it lacked came by the diff" \
	|| bad "node 3 holds $(ent 3) of node 1's $(ent 1) after 60 s: the copies it lacked never came"
r=$(ops 3 get o 50 new)
[ "$r" = 50 ] && ok "node 3 reads the 50 new values (a hit answers locally)" || bad "node 3 reads $r of the 50 new values - the rest its older copies"
# a round a sweep turn (10 s): ask, walk, ask again and find every slot equal
waitfor 60 'grep -q "diff sync with node [0-9]* done" "$D/n1.log"'
grep -q "diff sync with node [0-9]* done" "$D/n1.log" && ok "node 1 logs the diff's end: $(grep -o 'diff sync with node [0-9]* done[^)]*)' "$D/n1.log" | head -1)" \
	|| bad "no 'diff sync ... done' on node 1: $(grep 'diff sync' "$D/n1.log" | tail -1)"
e=$(cat "$D"/n*.log | grep -E ' (ERROR|CRIT)' | grep -cv 'an fsync has been stuck')
[ "$e" = 0 ] && ok "no ERROR/CRIT" || { bad "$e ERROR/CRIT lines"; cat "$D"/n*.log | grep -E ' (ERROR|CRIT)' | sort | uniq -c | head -5; }
for n in 1 3; do stop $n TERM; done
echo "recovereddifftest: $pass passed, $fail failed"
[ $fail -eq 0 ]
