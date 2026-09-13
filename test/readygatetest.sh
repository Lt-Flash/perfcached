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
RPORT=${RPORT:-17843}
MC=${MC:-239.77.41.9:17842}
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); }
bad() { FAIL=$((FAIL+1)); echo "  FAIL $1"; }

rm -rf "$D"; mkdir -p "$D/wal"
REV=$("$BIN" -V 2>/dev/null | sed -n 's/.*(\(.*\)).*/\1/p')
{ [ -z "$REV" ] || [ "$REV" = unknown ]; } && \
	{ echo "readygatetest: build cannot name itself"; exit 2; }

# resp_collections maps RESP db 0 onto the collection this test fills.
# Without it DBSIZE answers "no such collection" both inside the
# RECOVERING window and after READY, so the DBSIZE probe below cannot
# tell a gated node from a serving one and proves nothing.
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
resp = 127.0.0.1:$RPORT
plaintext = loopback
resp_collections = 0:c
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
# S138: what a RESP client is TOLD while the node is not READY.  The gate
# sits in front of command dispatch, so any data command reaches it, and
# the first word is the point: clients dispatch on the error CODE, and
# -LOADING is the one a real Redis sends while it reads its dataset.
resp1() { # resp1 <inline command> - the first line of the RESP reply
	python3 -c 'import socket, sys
try:
    s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
    s.sendall((sys.argv[2] + "\r\n").encode())
    print(s.recv(256).decode("utf-8", "replace").split("\r\n")[0])
    s.close()
except OSError as e:
    print("no-conn %s" % e)' "$RPORT" "$1" 2>/dev/null
}
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
SAW_RESP_LOADING=0; SAW_RESP_ERR=""
SAW_DB_LOADING=0; DB_SEEN=""
SAW_FL_LOADING=0; FL_SEEN=""
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
	R=$(resp1 "GET k7")
	case "$R" in
	"-LOADING "*)    SAW_RESP_LOADING=1;;
	"-ERR LOADING"*) SAW_RESP_ERR="$R";;
	esac
	# S140: DBSIZE and FLUSHDB resolve no collection of their own, but
	# that does not make them connection-plane commands - one reports
	# the keyspace and the other would destroy it, so both belong
	# BEHIND this gate exactly like GET.  Probed here, adjacent to the
	# GET probe, so all three samples fall in the same window.
	DB=$(resp1 "DBSIZE")
	case "$DB" in
	"-LOADING "*) SAW_DB_LOADING=1;;
	*)            [ -n "$DB" ] && DB_SEEN="$DB";;
	esac
	FL=$(resp1 "FLUSHDB")
	case "$FL" in
	"-LOADING "*) SAW_FL_LOADING=1;;
	*)            [ -n "$FL" ] && FL_SEEN="$FL";;
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
# S138: a RESP client dispatches on the error CODE, so the condition has
# to BE the code.  Buried inside a generic -ERR it is just a failure.
if [ "$SAW_RESP_LOADING" = 1 ]; then ok
elif [ -n "$SAW_RESP_ERR" ]; then
	bad "RESP refused with '$SAW_RESP_ERR' - the code must be LOADING, not ERR"
else bad "no RESP refusal seen in the window (last probe: ${R:-none})"; fi
if [ "$SAW_DB_LOADING" = 1 ]; then ok
else bad "DBSIZE was SERVED while the node was not READY (${DB_SEEN:-no answer})"; fi
if [ "$SAW_FL_LOADING" = 1 ]; then ok
else bad "FLUSHDB was ACCEPTED while the node was not READY (${FL_SEEN:-no answer})"; fi

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
# the probe must be able to answer WITHOUT the code, or seeing it above
# proves nothing: a serving node answers the miss, or `no such collection`
R=$(resp1 "GET k7")
case "$R" in
"-LOADING "*) bad "RESP still says LOADING after the node is READY ($R)";;
*) ok;;
esac
# the same two controls: each probe must be able to answer WITHOUT the
# code, or seeing -LOADING above proves nothing about the gate.
DB=$(resp1 "DBSIZE")
case "$DB" in
:*) ok;;
*)  bad "DBSIZE does not answer a count once READY ($DB) - the window
       result above proves nothing either way";;
esac
FL=$(resp1 "FLUSHDB")
case "$FL" in
"-ERR FLUSHDB"*) ok;;
*) bad "FLUSHDB does not reach its handler once READY ($FL) - the window
       result above proves nothing either way";;
esac
stop

echo "readygatetest: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ] || exit 1
