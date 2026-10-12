#!/bin/sh
# reconcileresizetest.sh - S275: a resize in the middle of the post-restart
# reconcile does not make the pass skip keys.
#
# The reconcile walked its table with pcache_ht_scan, whose cursor is a
# bucket index into the table it was given.  Since S69 a resize builds a
# new table and SWAPS: the next tick handed the old cursor to the new
# table.  After a SHRINK the cursor pointed past the new table's end, the
# walk read "complete", and every key not yet asked about was kept -
# including keys deleted on the fleet while this node was down.  The walk
# now goes through pc_store_scan, whose cursor carries the table's
# generation and restarts across a swap (a key may be asked twice, never
# not asked).
#
# Three eager nodes with a WAL, tombstone retention OFF (so only the
# reconcile can remove a key deleted during the outage), 3000 keys:
#   1. node 3 stops once a restart of it would be witnessed (S274)
#   2. 1500 keys are deleted through node 1 while it is down
#   3. node 3 comes back (2^14 buckets) and, once its reconcile has begun
#      asking, is shrunk to 2^12 - the resize lands MID-pass (2^12: room
#      for every record in the buckets; a smaller table puts some in the
#      overflow leg, whose drain a walk can miss - S281, filed apart)
#   4. when the pass is over, node 3 holds none of the 1500 deleted keys
#      and all 1500 kept ones, and node 1 none of the deleted
# The standalone edition refuses [cluster]: SKIPPED, loudly.
# Two defects, both measured here (DESIGN 12hc): the walk's raw cursor
# (after the shrink the pass read "complete" having asked 137 of 1500),
# and the resize copy restamping each record's write tick - which is what
# marks a recovered record as held until the reconcile rules (S245) - so
# the sweep pushed deleted keys back to every peer (236 of 1500, on the
# whole fleet).  Node 1 is read too: a key back on a node that never
# restarted is the second defect.
# Fail-first: the build before S275 fails 4 on both counts.
# Usage: test/reconcileresizetest.sh [./perfcached] [./perfcli]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrr.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; [ -n "${KEEP:-}" ] && cp -a "$D" "$KEEP"; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
if "$BIN" -V 2>/dev/null | grep -q standalone; then
	echo "  SKIP the standalone edition refuses a [cluster] section"
	echo "reconcileresizetest: 0 passed, 0 failed, 1 skipped"; exit 0
fi
for p in 18991 18992 18993 18994 18995 18996; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "reconcileresizetest: port $p busy" >&2; exit 1; }
done

conf() { # conf <n> <buckets_log2>
	mkdir -p "$D/s$1/wal"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
allow_create = yes
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = rr-client-secret
cluster = rr-cluster-secret
enable = rr-enable
[listen]
tcp = 127.0.0.1:1899$1
http = 127.0.0.1:1899$(($1 + 3))
plaintext = loopback
[cluster]
multicast = 239.255.78.16:18999
advertise = 127.0.31.$1
mode = eager
collections = 0
tombstone_retain_s = 0
[collection 0]
buckets_log2 = $2
autoscale = off
[wal]
dir = $D/s$1/wal
probe = no
fsync = everysec
segment_mb = 8
segments = 8
save = off
C
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> <nth ready line>
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 200 ]; do
		[ "$(grep -c "perfcached ready" "$D/n$1.log")" -ge "$2" ] && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; return 1
}
st() { curl -s -m 3 "http://127.0.0.1:1899$(($1 + 3))/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin); c = r["cluster"]; t = [x for x in r["collections"] if x["name"] == "0"][0]
print(eval(sys.argv[1]))' "$2" 2>/dev/null; }
ready3() {
	i=0; while [ $i -lt 300 ]; do
		[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ] && \
			[ "$(st 1 'c["peers_up"]')$(st 2 'c["peers_up"]')$(st 3 'c["peers_up"]')" = 222 ] && return 0
		sleep 0.2; i=$((i+1))
	done
	return 1
}
drive() { # drive <node> <op> <from> <to>: set/del k<from>..k<to-1>; prints how many landed
	python3 - "$@" <<'PY'
import json, pcnative, socket, sys
n, op, a, b = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
s = socket.create_connection(("127.0.0.1", 18990 + n), 30); f = pcnative.wrap(s); good = 0
for x in range(a, b, 200):
    rq = []
    for i in range(x, min(b, x + 200)):
        p = {"col": "0", "key": "k%04d" % i}
        if op == "set": p["value"] = "v%04d" % i
        rq.append(json.dumps({"jsonrpc": "2.0", "id": i, "method": op, "params": p}))
    f.write(("\n".join(rq) + "\n").encode()); f.flush()
    for _ in rq:
        r = json.loads(f.readline()).get("result") or {}
        good += 1 if (r.get("stored") or r.get("deleted") or r.get("exists")) else 0
print(good)
PY
}
held() { # held <node> <from> <to>: how many of k<from>..k<to-1> the node's own table has
	python3 - "$@" <<'PY'
import json, pcnative, socket, sys
n, a, b = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
s = socket.create_connection(("127.0.0.1", 18990 + n), 30); f = pcnative.wrap(s); good = 0
rq = [json.dumps({"jsonrpc": "2.0", "id": i, "method": "exists", "params": {"col": "0", "key": "k%04d" % i}}) for i in range(a, b)]
f.write(("\n".join(rq) + "\n").encode()); f.flush()
for _ in rq:
    good += 1 if (json.loads(f.readline()).get("result") or {}).get("exists") else 0
print(good)
PY
}

conf 1 10; conf 2 10; conf 3 14
for n in 1 2 3; do start $n 1 || exit 1; done
ready3 || { echo "reconcileresizetest: the fleet did not form"; exit 1; }
N=$(drive 1 set 0 3000)
sleep 3                                    # everysec, and the eager push
H3=$(held 3 0 3000)
[ "$N" = 3000 ] && [ "$H3" = 3000 ] || { echo "reconcileresizetest: setup: stored $N, node 3 holds $H3 of 3000"; exit 1; }
# 1. only once a restart of node 3 would be witnessed - else the pass is
#    skipped and nothing is tested (S274)
i=0; while [ $i -lt 90 ] && [ "$(st 3 'c["restart"]["safe"]')" != True ]; do sleep 1; i=$((i+1)); done
[ "$(st 3 'c["restart"]["safe"]')" = True ] && ok "1. a restart of node 3 would be witnessed (after ${i} s)" \
	|| { bad "1. node 3 never said a restart would be witnessed"; echo "reconcileresizetest: $pass passed, $fail failed"; exit 1; }
kill "$(cat "$D/n3.pid")"; i=0; while kill -0 "$(cat "$D/n3.pid")" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done
# 2
DEL=$(drive 1 del 0 1500)
[ "$DEL" = 1500 ] && ok "2. 1500 keys deleted through node 1 while node 3 was down" || bad "2. deleted $DEL of 1500"
sleep 2
# 3
start 3 2 || exit 1
i=0; while [ $i -lt 300 ] && [ "$(st 3 'c.get("reconcile_probed", 0)')" = 0 ]; do sleep 0.05; i=$((i+1)); done
P0=$(st 3 'c.get("reconcile_probed", 0)'); B0=$(st 3 't["buckets"]')
R=$(printf '%s\n%s\n' '{"jsonrpc":"2.0","id":1,"method":"enable","params":{"secret":"rr-enable"}}' \
	'{"jsonrpc":"2.0","id":2,"method":"resize","params":{"col":"0","buckets_log2":12}}' | \
	python3 -c 'import pcnative,socket,sys; s=socket.create_connection(("127.0.0.1",18993),10); f=pcnative.wrap(s); f.write(sys.stdin.buffer.read()); f.flush(); f.readline(); print(f.readline().decode().strip())')
i=0; while [ $i -lt 100 ] && [ "$(st 3 't["buckets"]')" != 4096 ]; do sleep 0.1; i=$((i+1)); done
P1=$(st 3 'c.get("reconcile_probed", 0)')
[ "$(st 3 't["buckets"]')" = 4096 ] && [ "$B0" = 16384 ] && [ "${P0:-0}" -gt 0 ] 2>/dev/null \
	&& ok "3. node 3 shrank $B0 -> 4096 buckets mid-pass (probed $P0 when asked, $P1 when it landed)" \
	|| bad "3. the shrink did not land mid-pass: buckets $B0 -> $(st 3 't["buckets"]'), probed $P0 / $P1 ($R)"
# 4: the pass ends, then its answers resolve
i=0; while [ $i -lt 120 ] && [ "$(st 3 'c.get("reconcile_ms", 0)')" = 0 ]; do sleep 1; i=$((i+1)); done
sleep 5
GONE=$(held 3 0 1500); KEPT=$(held 3 1500 3000); GONE1=$(held 1 0 1500)
echo "  info node 1 (never restarted) holds $GONE1 of the 1500 deleted keys"
[ "$GONE" = 0 ] && [ "$KEPT" = 1500 ] && [ "$GONE1" = 0 ] \
	&& ok "4. after the pass (${i} s, $(st 3 'c.get("reconcile_probed")') probed): node 3 holds 0 of the 1500 deleted keys and all 1500 kept" \
	|| bad "4. after the pass: node 3 still holds $GONE of the 1500 deleted keys, $KEPT of 1500 kept (probed $(st 3 'c.get("reconcile_probed")'), reconciled $(st 3 'c.get("reconciled")'), deferred $(st 3 'c.get("reconcile_deferred")'), digest-skipped $(st 3 'c.get("reconcile_dig_skipped")'), skipped $(st 3 'c.get("reconcile_skipped")'), witness $(st 3 'c.get("reconcile_witness")'); 20 s later still $(sleep 20; held 3 0 1500))"
echo "reconcileresizetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
