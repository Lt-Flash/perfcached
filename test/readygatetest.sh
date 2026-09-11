#!/bin/sh
# readygatetest.sh — C7: a node that is not READY refuses DATA verbs and
# still answers OBSERVABILITY ones.
#
# WHY THE GATE EXISTS.  A node replaying its WAL can still hold a key
# that was DELETED while it was down (that is B4's resurrection bug, and
# reconcile is what settles it).  Until it settles, serving does not
# answer LATE - it answers WRONGLY, which is the one failure a cache
# must not let a client mistake for a fact.
#
# THE WINDOW.  A clustered node that recovered data enters RECOVERING at
# daemon.c:699 and leaves it when it founds, JOIN_WAIT_MS (2 s) later.
# Workers are spawned before that, so the window is reachable by a
# client.  The test polls through it.
#
# IF THE WINDOW IS MISSED the test says so and FAILS rather than
# passing: "no refusal seen" is indistinguishable from "gate absent"
# unless we also prove we were actually inside the window, so the run
# asserts it observed state=recovering too.
#
# usage: test/readygatetest.sh [./perfcached] [./perfcli]
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
D=${READYGATE_DIR:-/var/tmp/readygate}
PORT=${PORT:-17841}
MC=${MC:-239.77.41.9:17842}
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); }
bad() { FAIL=$((FAIL+1)); echo "  FAIL $1"; }

rm -rf "$D"; mkdir -p "$D/wal"
REV=$("$BIN" -V 2>/dev/null | sed -n 's/.*(\(.*\)).*/\1/p')
{ [ -z "$REV" ] || [ "$REV" = unknown ]; } && \
	{ echo "readygatetest: build cannot name itself"; exit 2; }

cat > "$D/n1.conf" <<CFG
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 128
[secrets]
client = rg-secret
cluster = rg-cluster
[listen]
tcp = 127.0.0.1:$PORT
plaintext = loopback
[cluster]
multicast = $MC
[wal]
dir = $D/wal
segment_mb = 8
probe = no
fsync = no
save = off
[collection c]
buckets_log2 = 12
CFG

# stderr is NOT discarded: perfcli writes an error RESPONSE there, and a
# refusal is exactly what this test is looking for.  Swallowing it made
# the gate's reply read as an empty string and the whole run vacuous.
j() { "$CLI" -h 127.0.0.1 -p "$PORT" -j "$1" 2>&1; }
start() {
	"$BIN" -f "$D/n1.conf" > "$2" 2>&1 &
	P=$!
	k=0
	while [ $k -lt 300 ]; do
		grep -q "perfcached ready" "$2" 2>/dev/null && return 0
		sleep 0.1; k=$((k + 1))
	done
	return 1
}
stop() { [ -n "${P:-}" ] && kill -9 $P 2>/dev/null; P=; wait 2>/dev/null; }

echo "=== readygatetest ($REV) ==="

# --- populate a WAL so the restart has something to recover ------------
start x "$D/fill.log" || { echo "  FAIL first start"; exit 1; }
i=0
while [ $i -lt 200 ]; do
	j "{\"method\":\"set\",\"params\":{\"col\":\"c\",\"key\":\"k$i\",\"value\":\"v\"}}" \
		>/dev/null
	i=$((i + 1))
done
GOT=$(j '{"method":"get","params":{"col":"c","key":"k7"}}')
case "$GOT" in *'"v"'*) ok;; *) bad "fill did not take (get k7: $GOT)";; esac
stop

# --- restart: poll the RECOVERING window ------------------------------
"$BIN" -f "$D/n1.conf" > "$D/rec.log" 2>&1 &
P=$!
SAW_REC=0; SAW_REFUSE=0; SAW_STATS=0; SAW_MEMBERS=0
k=0
while [ $k -lt 120 ]; do
	ST=$(j '{"method":"members"}')
	case "$ST" in
	*'"state":"recovering"'*) SAW_REC=1;;
	esac
	case "$ST" in
	*'"members"'*) SAW_MEMBERS=1;;
	esac
	G=$(j '{"method":"get","params":{"col":"c","key":"k7"}}')
	case "$G" in
	*"not READY"*) SAW_REFUSE=1;;
	esac
	S=$(j '{"method":"stats"}')
	case "$S" in
	*'"version"'*) SAW_STATS=1;;
	esac
	[ "$SAW_REC" = 1 ] && [ "$SAW_REFUSE" = 1 ] && break
	sleep 0.05; k=$((k + 1))
done

# the window must have been OBSERVED, or a missing refusal proves nothing
if [ "$SAW_REC" = 1 ]; then ok
else bad "never observed state=recovering - window missed, the refusal
       result below proves nothing either way"; fi
if [ "$SAW_REFUSE" = 1 ]; then ok
else bad "a data verb was SERVED while the node was not READY"; fi
# observability must survive the gate: this is when you need it most
if [ "$SAW_STATS" = 1 ]; then ok; else bad "stats was gated too"; fi
if [ "$SAW_MEMBERS" = 1 ]; then ok; else bad "members was gated too"; fi

# --- and it must let go once READY ------------------------------------
k=0
while [ $k -lt 200 ]; do
	G=$(j '{"method":"get","params":{"col":"c","key":"k7"}}')
	case "$G" in *'"v"'*) break;; esac
	sleep 0.1; k=$((k + 1))
done
case "${G:-}" in
*'"v"'*) ok;;
*) bad "still refusing after the node should be READY ($G)";;
esac
stop

echo "readygatetest: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ] || exit 1
