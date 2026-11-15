#!/bin/sh
# rejoinwitnesstest.sh - an eager node restarting into a fleet with a survivor (a peer up through its
# absence) copies that state whole (12ol); with every survivor gone mid-copy, or one restarted since it
# was picked, it starts again from its own files.  Usage: test/rejoinwitnesstest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrj.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
if "$BIN" -V 2>/dev/null | grep -q standalone; then
	echo "  SKIP the standalone edition refuses a [cluster] section"
	echo "rejoinwitnesstest: 0 passed, 0 failed, 1 skipped"; exit "${PC_SKIP_FAILS:-0}"
fi
for p in 27491 27492 27493 27591 27592 27593; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "rejoinwitnesstest: port $p busy" >&2; exit 1; }
done

conf() {
	mkdir -p "$D/s$1/wal"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = rj-client-secret
cluster = rj-cluster-secret
[listen]
tcp = 127.0.0.1:2749$1
http = 127.0.0.1:2759$1
plaintext = loopback
[cluster]
discovery = unicast
port = 27390
seeds = 127.0.191.1, 127.0.191.2, 127.0.191.3
advertise = 127.0.191.$1
mode = eager
collections = 0
[collection 0]
buckets_log2 = 10
[wal]
dir = $D/s$1/wal
probe = no
fsync = everysec
segment_mb = 8
segments = 4
save = 3600 1
C
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> <how many "ready" lines the log holds then> [VAR=value ...]
	n=$1 want=${2:-1}; shift; [ $# -gt 0 ] && shift
	env "$@" "$BIN" -f "$D/n$n.conf" >> "$D/n$n.log" 2>&1 &
	echo $! > "$D/n$n.pid"
	i=0; while [ $i -lt 150 ]; do
		[ "$(grep -c "perfcached ready" "$D/n$n.log")" -ge "$want" ] && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $n did not start"; return 1
}
stop1() { kill "$(cat "$D/n$1.pid")" 2>/dev/null; i=0; while kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done; }
st() { curl -s -m 3 "http://127.0.0.1:2759$1/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin); c = r["cluster"]
print(eval(sys.argv[1]))' "$2" 2>/dev/null; }
ready3() {
	i=0; while [ $i -lt 200 ]; do
		[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ] && \
			[ "$(st 1 'c["peers_up"]')$(st 2 'c["peers_up"]')$(st 3 'c["peers_up"]')" = 222 ] && return 0
		sleep 0.2; i=$((i+1))
	done
	return 1
}
readyn() { # readyn <n>
	i=0; while [ $i -lt 200 ] && [ "$(st "$1" 'r["state"]')" != ready ]; do sleep 0.2; i=$((i+1)); done
	[ "$(st "$1" 'r["state"]')" = ready ]
}
put() { # put <port> <prefix> <count> <value>
	python3 - "$@" <<'PY'
import json, pcnative, socket, sys
port, pre, n, v = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), sys.argv[4]
f = pcnative.wrap(socket.create_connection(("127.0.0.1", port), 10))
for i in range(n):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": "set", "params": {"col": "0", "key": "%s%04d" % (pre, i), "value": v}}) + "\n").encode()); f.flush()
    f.readline()
PY
}
del() { # del <port> <from> <to>: k<from>..k<to-1>
	python3 - "$@" <<'PY'
import pcnative, socket, sys
port, a, b = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
f = pcnative.wrap(socket.create_connection(("127.0.0.1", port), 10))
for i in range(a, b):
    pcnative.call(f, "del", col="0", key="k%04d" % i)
PY
}
held() { # held <port>: "<w at b> <w at c> <k present of 200> <k0000-k0049 present> <k0050-k0099 present>"
	python3 - "$1" <<'PY'
import pcnative, socket, sys
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10))
def val(k):
    return (pcnative.call(f, "get", col="0", key=k).get("result") or {}).get("value")
w = [val("w%04d" % i) for i in range(3000)]
k = [val("k%04d" % i) is not None for i in range(200)]
print(w.count("b"), w.count("c"), sum(k), sum(k[:50]), sum(k[50:100]))
PY
}
after() { # after <n> <nth copy begun>: the log from that line on
	awk -v want="$2" '/cluster: rejoin - node [0-9]+ has been up/ { seen++ } seen >= want' "$D/n$1.log"
}
# the runner's storage stalling is no finding of this suite: shown, not counted
errs() {
	grep -h 'an fsync has been stuck' "$D"/n*.log 2>/dev/null | head -n 1 | sed "s/^/  ..   not counted, the runner's storage stalled: /"
	grep -hE ' (ERROR|CRIT)' "$D"/n*.log | grep -cv 'an fsync has been stuck'
}

for n in 1 2 3; do conf $n; done
for n in 1 2 3; do start $n || exit 1; done
ready3 || { echo "rejoinwitnesstest: the fleet did not form"; exit 1; }
put 27491 k 200 v
# both peers up long enough before node 1's coming outage to be survivors for it
i=0; while [ $i -lt 90 ] && [ "$(st 1 'c["restart"]["witnesses"]')" != 2 ]; do sleep 1; i=$((i+1)); done
[ "$(st 1 'c["restart"]["witnesses"]')" = 2 ] || { echo "rejoinwitnesstest: node 1 never counted 2 witnesses"; exit 1; }
put 27491 w 3000 a
sleep 2.5                                  # the WAL's everysec
stop1 1
# while node 1 is down: every w key rewritten, k0000-k0049 deleted
put 27492 w 3000 b
del 27492 0 50

echo "--- 1. node 1 restarts with survivors up: it copies their state whole"
L0=$(($(wc -l < "$D/n1.log") + 1))
start 1 2 || exit 1
i=0; while [ $i -lt 30 ] && ! grep -q "cluster: rejoin - node [0-9]* has been up" "$D/n1.log"; do sleep 0.5; i=$((i+1)); done
L=$(grep "cluster: rejoin - node [0-9]* has been up" "$D/n1.log")
[ "$(echo "$L" | grep -c .)" = 1 ] && echo "$L" | grep -q "rejoin - node [0-9]* has been up since before this node went down" \
	&& ok "1a. node 1 copies a survivor's state: $(echo "$L" | sed 's/.*cluster: //' | cut -c1-150)" \
	|| bad "1a. rejoin lines $(echo "$L" | grep -c .) (want 1, from a survivor): $(grep -E "rejoin -|reconcile|resync -" "$D/n1.log" | tail -2 | cut -c1-160 | tr '\n' ' ')"
i=0; while [ $i -lt 120 ] && [ "$(st 1 'r["state"]')$(st 1 'c.get("resyncing")')" != readyFalse ]; do sleep 0.5; i=$((i+1)); done
RJ=$(st 1 'c.get("rejoins")'); RS=$(st 1 'c.get("resyncs")'); WI=$(st 1 'c.get("reconcile_witness")')
[ "$RJ" = 1 ] && [ "$RS" = 0 ] && [ "$(st 1 'r["state"]')" = ready ] && [ "$(st 1 'c.get("reconcile_open")')" = False ] \
	&& [ "${WI:-0}" = 0 ] && ! grep -q "cluster: reconcile - node" "$D/n1.log" \
	&& ok "1b. node 1 is ready: rejoins 1, resyncs 0, and no record was ruled on one by one" \
	|| bad "1b. node 1: state $(st 1 'r["state"]'), reconcile_open $(st 1 'c.get("reconcile_open")'), rejoins ${RJ:-?}, resyncs ${RS:-?}, reconcile_witness ${WI:-?} (want ready, False, 1, 0, 0)"
H=$(held 27491)
[ "$H" = "3000 0 150 0 50" ] \
	&& ok "1c. node 1 holds the fleet's state: every w key at the rewritten value, the 50 keys deleted while it was down gone, the 150 others there" \
	|| bad "1c. node 1 holds (w at b, w at c, k present, k0000-49, k0050-99) = $H (want 3000 0 150 0 50)"
i=0; while [ $i -lt 30 ] && ! after 1 1 | grep -q "rdb: snapshot"; do sleep 1; i=$((i+1)); done
after 1 1 | grep -q "rdb: snapshot" \
	&& ok "1d. a snapshot followed the copy, so the files from before it go" \
	|| bad "1d. no snapshot after the copy: $(grep "rdb: " "$D/n1.log" | tail -2 | cut -c1-140 | tr '\n' ' ')"
# the copy is decided once the master has the links proved (ONLINE), and nothing is served before it
S1=$(sed -n "$L0,\$p" "$D/n1.log")
ON=$(echo "$S1" | grep -n "cluster: ONLINE - every link" | head -n 1 | cut -d: -f1)
CP=$(echo "$S1" | grep -n "cluster: rejoin - node [0-9]* has been up" | head -n 1 | cut -d: -f1)
[ -n "$ON" ] && [ -n "$CP" ] && [ "$ON" -lt "$CP" ] && ! echo "$S1" | grep -q "node state ready -> recovering" \
	&& ok "1e. node 1 went ONLINE (every link proved) before its copy began, and was not ready in between" \
	|| bad "1e. ONLINE at line ${ON:-none}, the copy at ${CP:-none} (want ONLINE first, no ready -> recovering): $(echo "$S1" | grep -E "admitted as|ONLINE|node state|rejoin - node" | cut -c1-120 | tr '\n' '|')"

echo "--- 2. every survivor gone mid-copy: node 1 starts again from its own files"
stop1 1
put 27492 w 3000 c
del 27492 50 100
start 1 3 PERFCACHED_TEST_BOOT_DELAY_MS=6000 || exit 1
P1=$(cat "$D/n1.pid")
i=0; while [ $i -lt 60 ] && [ "$(grep -c "cluster: rejoin - node [0-9]* has been up" "$D/n1.log")" -lt 2 ]; do sleep 0.25; i=$((i+1)); done
kill -9 "$(cat "$D/n2.pid")" "$(cat "$D/n3.pid")" 2>/dev/null
[ "$(grep -c "cluster: rejoin - node [0-9]* has been up" "$D/n1.log")" = 2 ] \
	&& ok "2a. node 1 began a second copy, and both survivors were killed before its pull" \
	|| bad "2a. node 1 never began the second copy: $(grep -E "rejoin -|reconcile|resync -" "$D/n1.log" | tail -2 | cut -c1-160 | tr '\n' ' ')"
i=0; while [ $i -lt 120 ] && kill -0 "$P1" 2>/dev/null; do sleep 0.5; i=$((i+1)); done
# a node still up after 60 s fails the step; `wait` on it would block for ever
if kill -0 "$P1" 2>/dev/null; then kill -9 "$P1" 2>/dev/null; wait "$P1" 2>/dev/null; RC="none: still running after 60 s"; else wait "$P1" 2>/dev/null; RC=$?; fi
rm -f "$D/n1.pid" "$D/n2.pid" "$D/n3.pid"
[ "$RC" = 75 ] && after 1 2 | grep -q "cluster: rejoin - no peer that was up through this node's absence is left" \
	&& after 1 2 | grep -q "stopping WITHOUT the shutdown snapshot" \
	&& ok "2b. node 1 stopped without a snapshot and exited 75, for its supervisor to restart it" \
	|| bad "2b. node 1 exited ${RC} (want 75): $(after 1 2 | grep -E "rejoin -|bootstrap|stopping|stopped" | tail -3 | cut -c1-160 | tr '\n' ' ')"
after 1 2 | grep -q "rdb: snapshot" \
	&& bad "2c. a snapshot ran during the failed copy: $(after 1 2 | grep "rdb: snapshot" | head -1 | cut -c1-140)" \
	|| ok "2c. no snapshot ran from the copy's start to the exit, so the files are as they were"
start 1 4 || exit 1
readyn 1
H=$(held 27491)
# k0050-99's deletes may reach it before the kill (the tombstone catch-up), kept in its WAL: either is right
set -- $H
[ "${1:-}" = 3000 ] && [ "${2:-}" = 0 ] && [ "${4:-}" = 0 ] && [ $((${3:-0} - ${5:-0})) = 100 ] \
	&& ok "2d. restarted alone, node 1 holds what its files held before the copy: w at b, not the fleet's c, k0100-199 all there; ${5} of k0050-99, deleted meanwhile, too (the rest replayed to it as deletes)" \
	|| bad "2d. node 1 holds (w at b, w at c, k present, k0000-49, k0050-99) = $H (want 3000 0 100+N 0 N)"

echo "--- 3. a survivor restarted mid-copy serves what it replayed: node 1 refuses it and starts from its files"
start 2 2 && start 3 2 || exit 1               # back with node 1 up, which booted after they died: they keep their files
readyn 2; readyn 3
i=0; while [ $i -lt 90 ] && [ "$(st 1 'c["restart"]["witnesses"]')" != 2 ]; do sleep 1; i=$((i+1)); done
[ "$(st 1 'c["restart"]["witnesses"]')" = 2 ] || { echo "rejoinwitnesstest: node 1 never counted 2 witnesses again"; exit 1; }
# deletes node 1 missed in 2 may be a hole in node 2's ended run: floored 30 s on, with a copy of node
# 2's state (12ol) - done, no hole left, before the writes; step 3's own copy is counted from N0
i=0; while [ $i -lt 120 ] && [ "$(st 1 'r["state"]')$(st 1 'c.get("resyncing")')$(st 1 'sum(m["confirmed"] < m["hi"] for m in c["acks"]["marks"])')" != readyFalse0 ]; do sleep 0.5; i=$((i+1)); done
put 27491 z 100 z                              # node 1's own: in its WAL, only copies on 2 and 3
sleep 2.5
N0=$(grep -c "cluster: rejoin - node [0-9]* has been up" "$D/n1.log"); W=$((N0 + 1))
stop1 1
start 1 5 PERFCACHED_TEST_BOOT_DELAY_MS=10000 || exit 1
P1=$(cat "$D/n1.pid")
i=0; while [ $i -lt 60 ] && [ "$(grep -c "cluster: rejoin - node [0-9]* has been up" "$D/n1.log")" -lt "$W" ]; do sleep 0.25; i=$((i+1)); done
kill -9 "$(cat "$D/n2.pid")" "$(cat "$D/n3.pid")" 2>/dev/null
rm -f "$D/n3.pid"
start 2 3 || exit 1                            # its new self answers the delayed pull with its own files
i=0; while [ $i -lt 120 ] && kill -0 "$P1" 2>/dev/null; do sleep 0.5; i=$((i+1)); done
if kill -0 "$P1" 2>/dev/null; then kill -9 "$P1" 2>/dev/null; wait "$P1" 2>/dev/null; RC="none: still running after 60 s"; else wait "$P1" 2>/dev/null; RC=$?; fi
rm -f "$D/n1.pid"
[ "$RC" = 75 ] && after 1 $W | grep -q "cluster: rejoin - node [0-9]* restarted after it was picked" \
	&& ! after 1 $W | grep -q "is still the survivor that served the copy" \
	&& ok "3a. node 1 took the restarted survivor's copy for what it was and exited 75: $(after 1 $W | grep "restarted after it was picked" | sed 's/.*cluster: //' | cut -c1-120)" \
	|| bad "3a. node 1 exited ${RC} (want 75, refusing the copy; copy $W): $(after 1 $N0 | grep -E "rejoin -|bootstrapped|stopping|floored" | cut -c1-160 | tr '\n' ' ')"
start 1 6 || exit 1
readyn 1
Z=$(python3 - <<'PY2'
import pcnative, socket
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 27491), 10))
print(sum(1 for i in range(100) if (pcnative.call(f, "get", col="0", key="z%04d" % i).get("result") or {}).get("value") == "z"))
PY2
)
[ "$Z" = 100 ] \
	&& ok "3b. restarted, node 1 holds its own 100 z keys from its files - none lost to the restarted node's copy" \
	|| bad "3b. node 1 holds ${Z:-?} of its 100 z keys (want 100: the restarted node never held them on disk)"

E=$(errs)
[ "$(echo "$E" | tail -n 1)" = 0 ] && { echo "$E" | sed '$d'; ok "4. no ERROR/CRIT line on any node"; } \
	|| bad "4. $(echo "$E" | tail -n 1) ERROR/CRIT line(s): $(grep -hE ' (ERROR|CRIT)' "$D"/n*.log | grep -v 'an fsync has been stuck' | head -2 | cut -c1-160)"

echo "rejoinwitnesstest: $pass passed, $fail failed"
[ $fail -eq 0 ]
