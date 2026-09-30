#!/bin/sh
# restartsharetest.sh - RV-4e: a shard, spread or proxy node restarting
# from a SNAPSHOT plus a WAL TAIL, and holding only its share.
#
# RV-4's map (DESIGN 12gb): snapshots were exercised only in eager and on
# single nodes; masterlosstest restarts shard and spread nodes from a WAL
# alone, and proxy had no restart suite.  Per mode, three nodes with a
# WAL (save = off: the ONLY snapshot is the one asked for):
#   1. 600 keys a000-a599 through node 1, placed by the mode;
#   2. a member (not the master) takes a snapshot (`save`), and then a
#      TAIL lands after it: a000-a099 overwritten, a100-a149 deleted,
#      b000-b199 new - the WAL made durable on every node (`sync`);
#   3. kill -9 the member, restart it: its log must show BOTH halves -
#      records from the snapshot and records applied from the WAL;
#   4. within 150 s: every key reads the same through all three nodes
#      (a000-a099 the new value, a100-a149 absent, a150-a599 and b the
#      value written), the fleet holds exactly K copies of each of the
#      750 live keys (shard 1, spread 2, proxy 1), and the member holds
#      exactly the share it held before the kill.
# Usage: test/restartsharetest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=
trap '[ -n "$D" ] && { kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"; }' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 18701 18702 18703 18711 18712 18713; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "restartsharetest: port $p busy" >&2; exit 1; }
done

conf() { # conf <n> <cluster lines> <collection lines>
	mkdir -p "$D/s$1/wal"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = rs-client-secret
cluster = rs-cluster-secret
[listen]
tcp = 127.0.0.1:1870$1
http = 127.0.0.1:1871$1
plaintext = loopback
[cluster]
multicast = 239.255.77.96:18698
advertise = 127.0.16.$1
pull_timeout_ms = 400
$2
[collection c]
buckets_log2 = 12
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
st() { curl -s "http://127.0.0.1:1871$1/stats" | python3 -c '
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
	python3 - "1870$1" "$2" "${3:-{\}}" <<'PY'
import json, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=30)
s.sendall((json.dumps({"jsonrpc": "2.0", "id": 1, "method": sys.argv[2], "params": json.loads(sys.argv[3])}) + "\n").encode())
print(s.makefile("rb").readline().decode().strip())
PY
}
# op <n> <set|del> <prefix> <from> <to> <tag>: prints how many succeeded
op() { python3 - "1870$1" "$2" "$3" "$4" "$5" "$6" <<'PY'
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
wrong() { python3 - "1870$1" <<'PY'
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
# which map each node routes by, and what it logged about it, when reads
# stay wrong (rc51 GitHub check-asan, shard: node 1 answered none of node
# 2's 267 keys for 150 s after node 2's restart, nodes 2 and 3 all 800)
map_dump() {
	for n in 1 2 3; do
		echo "      node $n: $(st $n '"role %s, map %s, peers up %s, sync %s, state %s" % (r["cluster"].get("role"), json.dumps(r["cluster"].get("map")), sum(1 for p in r["cluster"]["peers"] if p.get("up")), json.dumps(r["cluster"].get("sync")), r.get("state"))')"
		grep -hiE "map|reshard|owner|term|slot|standby|sync" "$D"/n$n.log.* 2>/dev/null | tail -8 | sed 's/^/        /'
	done
}

run() { # run <label> <K> <cluster lines> <collection lines>
	D=$(mktemp -d /var/tmp/pcrs.XXXXXX)
	echo "--- $1"
	for n in 1 2 3; do conf $n "$3" "$4"; done
	for n in 1 2 3; do start $n || { bad "$1: node $n did not start"; return; }; done
	ready3 || { bad "$1: the fleet did not form"; return; }
	W=$(op 1 set a 0 600 v1)
	sleep 3
	# the victim: a member holding keys - the master is RV-4b's case - or,
	# where placement put everything on one node (proxy fills the
	# ingress first), that node whatever its role
	V=3; [ "$(st 3 'r["cluster"]["role"]')" = master ] && V=2
	[ "$(ent $V)" = 0 ] && for n in 1 2 3; do [ "$(ent $n)" -gt 0 ] && V=$n; done
	# 2: the snapshot, then the tail after it
	R=$(call $V save)
	i=0; while [ $i -lt 100 ] && ! grep -q "rdb: snapshot" "$D"/n$V.log.0; do sleep 0.1; i=$((i+1)); done
	T=$(( $(op 1 set a 0 100 v2) + $(op 1 del a 100 150 x) + $(op 1 set b 0 200 v1) ))
	sleep 2
	for n in 1 2 3; do call $n sync >/dev/null; done
	E1=$(ent 1); E2=$(ent 2); E3=$(ent 3); EV=$(ent $V)
	[ "$W" = 600 ] && [ "$T" = 350 ] && grep -q "rdb: snapshot" "$D"/n$V.log.0 \
		&& ok "$1: 600 keys placed, node $V (member) snapshotted, then a 350-op tail; entries $E1/$E2/$E3" \
		|| bad "$1: stored $W of 600, tail $T of 350, snapshot: $(grep -c 'rdb: snapshot' "$D"/n$V.log.0) ($R)"
	# 3
	kill -9 "$(cat "$D/n$V.pid")"
	i=0; while kill -0 "$(cat "$D/n$V.pid")" 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
	start $V || { bad "$1: node $V did not come back"; return; }
	RL=$(grep -m1 "recover: " "$D"/n$V.log.1 | sed 's/.*recover: //')
	RN=$(echo "$RL" | sed -n 's/.*rdb loaded (\([0-9]*\) records.*/\1/p'); WA=$(echo "$RL" | sed -n 's/.*wal replay \([0-9]*\) applied.*/\1/p')
	[ "${RN:-0}" -gt 0 ] 2>/dev/null && [ "${WA:-0}" -gt 0 ] 2>/dev/null \
		&& ok "$1: node $V restored from BOTH: $RN records from the snapshot, $WA applied from the WAL tail" \
		|| bad "$1: node $V's recovery did not use both halves: $RL"
	# 4
	ready3 || bad "$1: the fleet did not re-form"
	i=0; while [ $i -lt 150 ]; do           # counts first: the reads are 2,400 gets
		S=$(( $(ent 1) + $(ent 2) + $(ent 3) ))
		[ "$S" = $((750 * $2)) ] && [ "$(ent $V)" = "$EV" ] && \
			[ "$(wrong 1 2>/dev/null)$(wrong 2 2>/dev/null)$(wrong 3 2>/dev/null)" = 000 ] && break
		sleep 3; i=$((i+3))
	done
	X1=$(wrong 1); X2=$(wrong 2); X3=$(wrong 3)
	[ "$X1$X2$X3" = 000 ] \
		&& ok "$1: all 800 names read as expected through every node (a000-a099 new, a100-a149 gone) after ${i} s" \
		|| { bad "$1: names answering wrong through nodes 1/2/3: $X1/$X2/$X3 of 800"; map_dump; }
	S=$(( $(ent 1) + $(ent 2) + $(ent 3) ))
	[ "$S" = $((750 * $2)) ] && [ "$(ent $V)" = "$EV" ] \
		&& ok "$1: exactly $2 cop$( [ $2 = 1 ] && echo y || echo ies) of each of 750 keys ($S held), and node $V holds its share again ($EV)" \
		|| bad "$1: the fleet holds $S (want $((750 * $2))); node $V holds $(ent $V), held $EV before the kill"
	kill -9 $(cat "$D"/*.pid) 2>/dev/null
	rm -rf "$D"; D=
}

run "shard" 1 "mode = shard
collections = c" ""
run "spread K=2" 2 "mode = spread
replicas = 2
collections = c" ""
run "proxy" 1 "" "mode = proxy"
echo "restartsharetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
