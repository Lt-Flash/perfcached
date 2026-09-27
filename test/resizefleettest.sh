#!/bin/sh
# resizefleettest.sh - RV-4g: an online resize reaching a shard, spread or
# proxy fleet.
#
# RV-4's map (DESIGN 12gb): a resize reached the fleet only in store and
# eager (colresizetest, which exists because a resize was once broadcast
# as a DROP).  Per mode, three nodes, collection c at 2^12 with
# autoscale = off (S239 - so nothing but the operator changes its size):
#   1. 600 keys through node 1;
#   2. `resize c 14` through node 1 (privileged), and while the tables
#      move: a000-a099 overwritten through node 2, a100-a149 deleted
#      through node 3, b000-b199 new through node 1;
#   3. every node's table reaches 16,384 buckets, the fleet holds exactly
#      K copies of the 750 live keys, and every name reads as expected
#      through every node;
#      (S249: the deletes through node 3 must be ACKNOWLEDGED - under
#      proxy node 3 holds nothing and must find the holder);
#   4. `resize c 10` through node 2 - below what 750 keys want, so the
#      overflow leg takes the rest - and the same holds at 1,024 buckets.
# Usage: test/resizefleettest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=
trap '[ -n "$D" ] && { kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"; }' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 18821 18822 18823 18831 18832 18833; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "resizefleettest: port $p busy" >&2; exit 1; }
done

conf() { # conf <n> <cluster lines> <collection lines>
	mkdir -p "$D/s$1/wal"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
allow_create = yes
[memory]
arena_mb = 64
[secrets]
client = rz-client-secret
cluster = rz-cluster-secret
enable = rz-enable-secret
[listen]
tcp = 127.0.0.1:1882$1
http = 127.0.0.1:1883$1
plaintext = loopback
[cluster]
multicast = 239.255.78.2:18845
advertise = 127.0.20.$1
pull_timeout_ms = 400
$2
[collection c]
buckets_log2 = 12
autoscale = off
$3
[wal]
dir = $D/s$1/wal
probe = no
fsync = everysec
segment_mb = 8
segments = 4
save = off
C
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> - each start logs to its own file, n<n>.log.<k>
	k=$(ls "$D"/n$1.log.* 2>/dev/null | wc -l); L="$D/n$1.log.$k"
	"$BIN" -f "$D/n$1.conf" > "$L" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$L" && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start: $(tail -2 "$L" | tr '\n' ' ')"
	return 1
}
st() { curl -s "http://127.0.0.1:1883$1/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin); c = [x for x in r["collections"] if x["name"] == "c"][0]
print(eval(sys.argv[1]))' "$2" 2>/dev/null; }
up() { st $1 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))'; }
ready3() {
	i=0; while [ $i -lt 120 ]; do
		[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ] && \
			[ "$(up 1)$(up 2)$(up 3)" = 222 ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}
call() { # call <n> <method> [params json]: prints the reply
	python3 - "1882$1" "$2" "${3:-{\}}" <<'PY'
import json, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=30)
s.sendall((json.dumps({"jsonrpc": "2.0", "id": 1, "method": sys.argv[2], "params": json.loads(sys.argv[3])}) + "\n").encode())
print(s.makefile("rb").readline().decode().strip())
PY
}
# op <n> <set|del> <prefix> <from> <to> <tag>: prints how many succeeded
op() { python3 - "1882$1" "$2" "$3" "$4" "$5" "$6" <<'PY'
import json, socket, sys
port, verb, pre, a, b, tag = int(sys.argv[1]), sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
s = socket.create_connection(("127.0.0.1", port), timeout=60); f = s.makefile("rb"); ok = 0
for x in range(a, b, 100):
    rq = []
    for i in range(x, min(b, x + 100)):
        p = {"col": "c", "key": "%s%03d" % (pre, i)}
        if verb == "set":
            p["value"] = "%s-%s%03d" % (tag, pre, i)
        rq.append(json.dumps({"jsonrpc": "2.0", "id": i, "method": verb, "params": p}))
    s.sendall(("\n".join(rq) + "\n").encode())
    for _ in rq:
        r = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if "id" in _m).get("result") or {}
        ok += 1 if (r.get("stored") or r.get("deleted") or r.get("removed")) else 0
print(ok)
PY
}
# wrong <n>: how many of the 800 names answer other than expected through node n
wrong() { python3 - "1882$1" <<'PY'
import json, socket, sys
want = {}
for i in range(600):
    want["a%03d" % i] = None if 100 <= i < 150 else ("v2-a%03d" % i if i < 100 else "v1-a%03d" % i)
for i in range(200):
    want["b%03d" % i] = "v1-b%03d" % i
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=60); f = s.makefile("rb")
keys = list(want); bad = 0
for x in range(0, len(keys), 200):
    rq = [json.dumps({"jsonrpc": "2.0", "id": j, "method": "get", "params": {"col": "c", "key": keys[j]}}) for j in range(x, min(len(keys), x + 200))]
    s.sendall(("\n".join(rq) + "\n").encode())
    for _ in rq:
        m = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if "id" in _m); v = (m.get("result") or {}).get("value")
        bad += 0 if v == want[keys[m["id"]]] else 1
        if v != want[keys[m["id"]]] and bad <= 3:
            sys.stderr.write("      %s: want %s got %s\n" % (keys[m["id"]], want[keys[m["id"]]], v))
print(bad)
PY
}
ent() { st $1 'c["entries"]'; }


bk() { st $1 'c["buckets"]'; }
resize() { # resize <n> <log2>: privileged, one connection
	python3 - "1882$1" "$2" <<'PY'
import json, socket, sys
f = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=30).makefile("rwb")
for i, (m, p) in enumerate([("enable", {"secret": "rz-enable-secret"}), ("resize", {"col": "c", "buckets_log2": int(sys.argv[2])})]):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": m, "params": p}) + "\n").encode()); f.flush()
    last = f.readline().decode().strip()
print(last)
PY
}
settle() { # settle <label> <K> <buckets> <live>: every node at <buckets>, exact copies, all names right
	i=0; while [ $i -lt 120 ]; do
		[ "$(bk 1)$(bk 2)$(bk 3)" = "$3$3$3" ] && [ $(( $(ent 1) + $(ent 2) + $(ent 3) )) = $(( $4 * $2 )) ] && \
			[ "$(wrong 1 2>/dev/null)$(wrong 2 2>/dev/null)$(wrong 3 2>/dev/null)" = 000 ] && break
		sleep 3; i=$((i+3))
	done
	B="$(bk 1)/$(bk 2)/$(bk 3)"; S=$(( $(ent 1) + $(ent 2) + $(ent 3) ))
	X1=$(wrong 1); X2=$(wrong 2); X3=$(wrong 3)
	[ "$B" = "$3/$3/$3" ] \
		&& ok "$1: every node's table is at $3 buckets (after ${i} s) - the resize reached the fleet as a resize" \
		|| bad "$1: buckets $B (want $3 on all three)"
	[ "$S" = $(( $4 * $2 )) ] && [ "$X1$X2$X3" = 000 ] \
		&& ok "$1: exactly $2 cop$( [ $2 = 1 ] && echo y || echo ies) of each of $4 keys ($S held), every name right through every node" \
		|| bad "$1: $S held (want $(( $4 * $2 ))), names wrong through nodes 1/2/3: $X1/$X2/$X3"
}

run() { # run <label> <K> <cluster lines> <collection lines>
	# a run that returned early left its fleet up: take it down first
	[ -n "$D" ] && { kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"; }
	D=$(mktemp -d /var/tmp/pcrz.XXXXXX)
	echo "--- $1"
	for n in 1 2 3; do conf $n "$3" "$4"; done
	for n in 1 2 3; do start $n || { bad "$1: node $n did not start"; return; }; done
	ready3 || { bad "$1: the fleet did not form"; return; }
	W=$(op 1 set a 0 600 v1)
	[ "$W" = 600 ] || bad "$1: stored $W of 600"
	R=$(resize 1 14)
	case "$R" in *resiz*) ok "$1: resize c 14 accepted through node 1 ($R)";; *) bad "$1: resize refused: $R"; return;; esac
	# writes while the tables move: the tail the suite reads back
	T=$(( $(op 2 set a 0 100 v2) + $(op 3 del a 100 150 x) + $(op 1 set b 0 200 v1) ))
	# S249: the deletes go through node 3, which never saw these keys -
	# under proxy it holds none, and a delete it cannot locate must still
	# reach the holder and say so
	[ "$T" = 350 ] && ok "$1: all 350 ops during the resize acknowledged - the 50 deletes through node 3 (not the writer) included" \
		|| bad "$1: $T of 350 ops during the resize acknowledged (S249: a delete through a node that does not know the holder)"
	settle "$1 up to 2^14" $2 16384 750
	R=$(resize 2 10)
	case "$R" in *resiz*) ok "$1: resize c 10 accepted through node 2 - below what 750 keys want, autoscale off: the leg takes the rest";; *) bad "$1: resize down refused: $R"; return;; esac
	settle "$1 down to 2^10" $2 1024 750
	kill -9 $(cat "$D"/*.pid) 2>/dev/null
	rm -rf "$D"; D=
}

run "shard" 1 "mode = shard
collections = c" ""
run "spread K=2" 2 "mode = spread
replicas = 2
collections = c" ""
run "proxy" 1 "" "mode = proxy"
echo "resizefleettest: $pass passed, $fail failed"
[ $fail -eq 0 ]
