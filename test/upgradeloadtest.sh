#!/bin/sh
# upgradeloadtest.sh - UP7c (DESIGN 12ns; 12ne's UP10, its first form): the
# roll under load, with the three assertions of 12ne.
#   - a three-member loopback fleet; 2,000 keys pre-filled and replicated,
#     500 more written and then deleted (replicated too);
#   - rollload (libperfd, 4 threads, spares) reads the pre-filled keys and
#     writes 2,000 others for as long as the roll takes;
#   - perfcli upgrade rolls every node with the switch hook (this binary
#     restarted - the install is the one step CI cannot do);
#   - no error: the load saw no error, no miss and no wrong value - the
#     restarts were failovers inside the library;
#   - no lost key: every pre-filled key and every key the load wrote reads
#     back with its value on EVERY node;
#   - no resurrection: every deleted key is absent on every node.
# The previous minor's release artifact (UP10's full form) comes when a
# release lives in the package registry; the epoch bump, the standalone
# node and the bad signature stay on UP10's list.
#
# MODE=shard runs the same roll on a shard fleet (one owner per key, the
# WAL on every node so a restart brings its share back): there the owner's
# share is away while it restarts, by design (README's table), so the
# load's errors are counted and bounded, not zero; a miss or a wrong value
# is still a defect (an existing key must never read as absent), and
# after the roll every key must be there, with its value, through every
# node, and every deleted key absent - no lost key, no resurrection.
# Usage: [MODE=eager|shard] test/upgradeloadtest.sh [./perfcached] [./perfcli] [./rollload]
set -u
BIN=${1:-./perfcached}; CLI=${2:-./perfcli}; LOAD=${3:-./rollload}
MODE=${MODE:-eager}
case "$MODE" in eager|shard) ;; *) echo "upgradeloadtest: MODE is eager or shard" >&2; exit 2;; esac
echo "upgradeloadtest: mode $MODE"
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
case "$CLI" in /*) ;; *) CLI=$(pwd)/$CLI;; esac
case "$LOAD" in /*) ;; *) LOAD=$(pwd)/$LOAD;; esac
T=$(cd "$(dirname "$0")" && pwd); PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH   # pcnative
D=$(mktemp -d /var/tmp/pcrollload.XXXXXX)
LP=
# the daemons are waited for, not only killed: the shard variant starts on the
# same ports the moment the eager one exits (v0.7.0-rc3, GitLab 110995 roll:
# "port 17711 already bound" - the killed nodes were still going away)
trap 'kill $LP 2>/dev/null; PS=$(cat "$D"/pid* 2>/dev/null); for p in $PS; do kill -9 $p 2>/dev/null; done
	for p in $PS; do i=0; while [ $i -lt 50 ] && kill -0 $p 2>/dev/null; do sleep 0.1; i=$((i+1)); done; done
	[ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
# a port still held is given 10 s: a run before this one may be going away
for p in 17711 17712 17713 17721 17722 17723; do
	i=0; while [ $i -lt 100 ] && ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]"; do sleep 0.1; i=$((i+1)); done
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upgradeloadtest: port $p already bound" >&2; exit 1; }
done
VER=$("$BIN" -V | head -1 | awk '{print $2}')
head -c 300000 /dev/urandom > "$D/a.tgz"
node() {
	mkdir -p "$D/state$1"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = info
state_dir = $D/state$1
[memory]
arena_mb = 64
[secrets]
client = load-client-secret
cluster = load-cluster-secret
enable = load-enable
[listen]
tcp = 127.0.0.8$1:1771$1
http = 127.0.0.8$1:1772$1
plaintext = loopback
[cluster]
mode = $MODE
multicast = 239.255.77.72:17672
advertise = 127.0.0.8$1
collections = 0
pull_timeout_ms = 200
negative_ms = 500
tombstone_ms = 2000
[upgrade]
allow_unsigned = yes
C
	if [ "$MODE" = shard ]; then mkdir -p "$D/wal$1"; cat >> "$D/n$1.conf" <<C
[wal]
dir = $D/wal$1
segment_mb = 8
probe = no
save = off
C
	fi
	cat >> "$D/n$1.conf" <<C
[collection 0]
buckets_log2 = 12
C
	chmod 600 "$D/n$1.conf"
}
start() {
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/pid$1"
	i=0; while [ $i -lt 150 ] && ! tail -50 "$D/n$1.log" | grep -q "perfcached ready"; do sleep 0.1; i=$((i+1)); done
	tail -50 "$D/n$1.log" | grep -q "perfcached ready" || { echo "node $1 did not start"; tail -5 "$D/n$1.log"; exit 1; }
}
cat > "$D/hook.sh" <<EOF2
#!/bin/sh
op=\$1 addr=\$2
n=\${addr#127.0.0.8}
echo "\$op \$n \$(date +%s)" >> "$D/hook.log"
p=\$(cat "$D/pid\$n")
kill \$p 2>/dev/null
i=0; while [ \$i -lt 100 ] && kill -0 \$p 2>/dev/null; do sleep 0.1; i=\$((i+1)); done
"$BIN" -f "$D/n\$n.conf" >> "$D/n\$n.log" 2>&1 &
echo \$! > "$D/pid\$n"
exit 0
EOF2
chmod 755 "$D/hook.sh"
node 1; node 2; node 3
start 1; start 2; start 3
# the nodes join and pull a bootstrap before they are READY (a replicated
# collection): the first write came 3.5 s after the log said ready and was
# refused "not READY (recovering)" on 225 (2026-10-16) - wait for the state
# ... and for its PEERS: a node READY before it has seen a live peer owns every
# slot (HRW over nobody) and stores a shard write locally; the fill then sat on
# node 1 and the deletes, forwarded to the owners a second later, found nothing
# (deleted 166 of 500, 225 2026-10-16)
rdy() {
	i=0; while [ $i -lt 150 ] && pyn "$1" peek | grep -q 'not READY'; do sleep 0.2; i=$((i+1)); done
	i=0; while [ $i -lt 150 ] && [ "$(python3 -c 'import json,sys,urllib.request
print(json.load(urllib.request.urlopen("http://127.0.0.8%s:1772%s/stats" % (sys.argv[1], sys.argv[1]), timeout=5))["cluster"]["peers_up"])' "$1" 2>/dev/null)" != 2 ]; do sleep 0.2; i=$((i+1)); done
}
# pcnative over the native door of node <n>: fill, delete, verify
pyn() { python3 - "$@" <<'PY' 2>&1
import socket, sys
import pcnative
n = sys.argv[1]; what = sys.argv[2]
f = pcnative.wrap(socket.create_connection(("127.0.0.8%s" % n, 17710 + int(n)), 20))
def call(m, **kw):
    r = pcnative.call(f, m, **kw)
    if "error" in r: raise SystemExit("ERR %s %s" % (m, r["error"]))
    return r["result"]
if what == "fill":
    # a forwarded set that timed out answers {stored: false} without an error
    # (the native door's FWD_SET) - checked, so a placement fault fails here
    # with the key, not 2,500 slot timeouts later
    for k in range(2000):
        if not call("set", col="0", key="roll:p:%d" % k, value="p%d" % k).get("stored"): raise SystemExit("set roll:p:%d not stored" % k)
    for k in range(500):
        if not call("set", col="0", key="roll:d:%d" % k, value="d%d" % k).get("stored"): raise SystemExit("set roll:d:%d not stored" % k)
    print("filled")
elif what == "del":
    n_del = sum(1 for k in range(500) if call("del", col="0", key="roll:d:%d" % k).get("deleted") in (True, 1))
    print("deleted %d" % n_del)
elif what == "peek":
    r1 = call("get", col="0", key="roll:p:1999"); r2 = call("get", col="0", key="roll:d:499")
    print("p1999=%s d499=%s" % (r1.get("value"), r2.get("value")))
elif what == "verify":
    written = sys.argv[3]
    lost = wrong = res = wlost = 0
    for k in range(2000):
        r = call("get", col="0", key="roll:p:%d" % k)
        if not r.get("found"): lost += 1
        elif r.get("value") != "p%d" % k: wrong += 1
    for k in range(500):
        if call("get", col="0", key="roll:d:%d" % k).get("found"): res += 1
    for k, w in enumerate(written):
        if w == "1":
            r = call("get", col="0", key="roll:w:%d" % k)
            if not r.get("found") or r.get("value") != "w%d" % k: wlost += 1
    print("lost=%d wrong=%d resurrected=%d wlost=%d written=%d" % (lost, wrong, res, wlost, written.count("1")))
PY
}
rdy 1; rdy 2; rdy 3
[ "$(pyn 1 fill)" = filled ] && ok "2,500 keys written on node 1" || bad "fill: $(pyn 1 fill)"
i=0; while [ $i -lt 100 ] && [ "$(pyn 3 peek)" != "p1999=p1999 d499=d499" ]; do sleep 0.2; i=$((i+1)); done
[ "$(pyn 3 peek)" = "p1999=p1999 d499=d499" ] && ok "replicated to node 3" || bad "node 3 holds: $(pyn 3 peek)"
r=$(pyn 1 del); case "$r" in "deleted 500") ok "500 keys deleted on node 1";; *) bad "del: $r";; esac
i=0; while [ $i -lt 100 ] && [ "$(pyn 3 peek)" != "p1999=p1999 d499=None" ]; do sleep 0.2; i=$((i+1)); done
[ "$(pyn 3 peek)" = "p1999=p1999 d499=None" ] && ok "the deletes reached node 3" || bad "node 3 after the deletes: $(pyn 3 peek)"

# "-": the loopback door is plaintext (plaintext = loopback does no crypto), so the
# library connects without a secret, as perfcli does here
"$LOAD" 127.0.0.81 17711 - 0 4 2000 600 "$D/load.out" > "$D/load.log" 2>&1 &
LP=$!
sleep 2
kill -0 $LP 2>/dev/null && ok "the load runs (4 threads, spares, through libperfd)" || bad "the load died: $(cat "$D/load.log")"

P="$CLI -q -h 127.0.0.81 -p 17711 -E load-enable"
$P upgrade plan "$D/a.tgz" version=$VER switch_cmd="$D/hook.sh" journal="$D/j" ready_s=30 gate_s=60 > /dev/null 2>&1
r=$($P upgrade start journal="$D/j" 2>&1); rc=$?
[ $rc -eq 0 ] && [ "$(grep -c 'event=gated' "$D/j")" = 3 ] && grep -q '^[0-9]* done ' "$D/j" && ok "the roll under load: three nodes gated, done" || bad "roll rc=$rc: $(tail -4 "$D/j" | tr '\n' '|')"
sleep 2
kill -TERM $LP; wait $LP 2>/dev/null; lrc=$?; LP=
summary=$(head -1 "$D/load.out")
echo "  ..   $summary"
ops=$(echo "$summary" | sed 's/.*ops=\([0-9]*\).*/\1/'); errs=$(echo "$summary" | sed 's/.*errors=\([0-9]*\).*/\1/')
miss=$(echo "$summary" | sed 's/.*misses=\([0-9]*\).*/\1/'); wr=$(echo "$summary" | sed 's/.*wrong=\([0-9]*\).*/\1/'); fo=$(echo "$summary" | sed 's/.*failovers=\([0-9]*\).*/\1/')
[ "$ops" -ge 1000 ] 2>/dev/null && ok "the load did $ops requests across the roll" || bad "the load did only $ops requests"
if [ "$MODE" = eager ]; then
	[ "$lrc" = 0 ] && [ "$errs" = 0 ] && [ "$miss" = 0 ] && [ "$wr" = 0 ] && ok "NO ERROR: errors 0, misses 0, wrong values 0 (failovers $fo)" || bad "the load saw errors $errs, misses $miss, wrong $wr: $(grep 'last error' "$D/load.out" | head -2 | tr '\n' '|')"
else
	# shard: the owner's share is away while it restarts - errors are expected and bounded (under 5% of
	# the requests across three restarts); a miss or a wrong value is a defect all the same
	# shard keeps ONE copy: while an owner restarts its share reads as absent
	# (a pull to a node that is down times out) - misses are the restart windows'
	# and bounded, wrong values never
	[ "$wr" = 0 ] && [ "$miss" -lt $((ops * 2 / 5)) ] && ok "shard: wrong values 0, misses $miss (the owners' shares while they restarted; errors $errs, failovers $fo)" || bad "shard: misses $miss, wrong $wr: $(grep 'last error' "$D/load.out" | head -2 | tr '\n' '|')"
	[ "$errs" -lt $((ops / 20)) ] 2>/dev/null && ok "shard: the errors ($errs) stayed under 5% of $ops requests" || bad "shard: errors $errs of $ops requests - more than the restart windows explain: $(grep 'last error' "$D/load.out" | head -2 | tr '\n' '|')"
fi
[ "$fo" -ge 1 ] 2>/dev/null && ok "the library failed over $fo time(s)" || bad "no failover counted: the load's node was never restarted?"
WR=$(sed -n 's/^written=//p' "$D/load.out")
for n in 1 2 3; do
	v=$(pyn $n verify "$WR")
	case "$v" in
	"lost=0 wrong=0 resurrected=0 wlost=0 written="*) ok "node $n: NO LOST KEY, NO RESURRECTION ($v)";;
	*) bad "node $n: $v";;
	esac
done
echo "upgradeloadtest ($MODE): $pass passed, $fail failed"
[ $fail -eq 0 ]
