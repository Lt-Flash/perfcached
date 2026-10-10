#!/bin/sh
# restartwitnesstest.sh - S274: a node says whether a restart of it NOW
# would be witnessed, and the reconcile then does what it said.
#
# S223's rule: a restarted eager node reconciles its replayed records only
# if some live peer was up since before it went down (by WITNESS_GRACE_MS);
# with none, the pass is SKIPPED - nothing dropped, but a key deleted
# during the outage survives.  rc54's roll restarted 246 inside that grace.
# Nothing told the operator; the roll script waits 50 s by hand.  Now
# /stats cluster.restart = {safe, witnesses, safe_in_ms} is the node's own
# answer, computed from the same predicate with its error bars.
#
# Three eager nodes with a WAL, 200 keys:
#   1. just formed: no peer has been up long enough - every node says
#      NOT safe, with a wait of about the grace (> 30 s, < 45 s)
#   2. restart node 1 then: its reconcile is SKIPPED - the prediction held
#   3. once the wait has passed, nodes 2 and 3 say safe, 2 witnesses each
#   4. restart node 2 then: its reconcile finds a witness - the prediction
#      held the other way
#   5. node 2, just restarted, is a witness to nobody yet: node 3 counts
#      one witness (node 1 is old enough by then), not two
# The standalone edition refuses [cluster]: SKIPPED, loudly.
# Fail-first: a build before S274 has no cluster.restart - 1, 3, 5 fail.
# Usage: test/restartwitnesstest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrw.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
if "$BIN" -V 2>/dev/null | grep -q standalone; then
	echo "  SKIP the standalone edition refuses a [cluster] section"
	echo "restartwitnesstest: 0 passed, 0 failed, 1 skipped"; exit 0
fi
for p in 18971 18972 18973 18981 18982 18983; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "restartwitnesstest: port $p busy" >&2; exit 1; }
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
client = rw-client-secret
cluster = rw-cluster-secret
[listen]
tcp = 127.0.0.1:1897$1
http = 127.0.0.1:1898$1
plaintext = loopback
[cluster]
multicast = 239.255.78.15:18975
advertise = 127.0.30.$1
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
save = off
C
	chmod 600 "$D/n$1.conf"
}
start() {
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 150 ]; do
		[ "$(grep -c "perfcached ready" "$D/n$1.log")" -ge "${2:-1}" ] && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; return 1
}
stop1() { kill "$(cat "$D/n$1.pid")" 2>/dev/null; i=0; while kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done; }
st() { curl -s -m 3 "http://127.0.0.1:1898$1/stats" | python3 -c '
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
settled() { # settled <n>: the node's reconcile verdict is in (skipped or a witness)
	i=0; while [ $i -lt 150 ]; do
		[ "$(st $1 'c.get("reconcile_skipped")')" = True ] && return 0
		[ "$(st $1 'c.get("reconcile_witness", 0) != 0')" = True ] && return 0
		sleep 0.2; i=$((i+1))
	done
	return 1
}

for n in 1 2 3; do conf $n; done
for n in 1 2 3; do start $n || exit 1; done
ready3 || { echo "restartwitnesstest: the fleet did not form"; exit 1; }
python3 - <<'PY'
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 18971), 10); f = pcnative.wrap(s)
for i in range(200):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": "set", "params": {"col": "0", "key": "k%03d" % i, "value": "v"}}) + "\n").encode()); f.flush()
    f.readline()
PY
sleep 2.5                                  # the WAL's everysec, on every node

# 1: the wait is the whole margin - the S223 grace (30 s) + one alive-
# stamp period (10 s) + uptime's truncation (1 s) = 41 s from the peer's
# boot - so wait + the youngest peer's uptime is 41 s, give or take the
# second uptime_s truncates.  (Without the margin it is 30: a node would
# say "safe" while its own death estimate could still miss the witness.)
R1=$(st 1 'json.dumps(c.get("restart"))'); W1=$(st 1 'c["restart"]["safe_in_ms"]')
U23=$(for n in 2 3; do st $n 'r["process"]["uptime_s"]'; done | sort -n | head -1)
T=$(( ${W1:-0} + ${U23:-0} * 1000 ))
[ "$(st 1 'c["restart"]["safe"]')$(st 2 'c["restart"]["safe"]')$(st 3 'c["restart"]["safe"]')" = FalseFalseFalse ] \
	&& [ "$T" -ge 39500 ] && [ "$T" -le 42500 ] \
	&& ok "1. just formed: every node says a restart is NOT witnessed yet, for the whole margin ($R1; + peers up ${U23} s = ${T} ms)" \
	|| bad "1. just formed: node 1 says $R1, peers up ${U23} s: wait + uptime = ${T} ms (want 41 s +- 1.5, safe false on all three)"
# 2
stop1 1; start 1 2 || exit 1; ready3
settled 1 && [ "$(st 1 'c.get("reconcile_skipped")')" = True ] \
	&& ok "2. restarted inside the grace: node 1's reconcile was SKIPPED, as it said" \
	|| bad "2. node 1 restarted inside the grace: skipped=$(st 1 'c.get("reconcile_skipped")') witness=$(st 1 'c.get("reconcile_witness")')"
# 3: wait out node 2's own verdict
i=0; while [ $i -lt 60 ] && [ "$(st 2 'c["restart"]["safe"]')" != True ]; do sleep 1; i=$((i+1)); done
[ "$(st 2 'c["restart"]["safe"]')" = True ] && [ "$(st 3 'c["restart"]["safe"]')" = True ] \
	&& [ "$(st 3 'c["restart"]["witnesses"]')" -ge 1 ] 2>/dev/null \
	&& ok "3. after ${i} s more: nodes 2 and 3 say a restart is witnessed (node 2: $(st 2 'json.dumps(c["restart"])'))" \
	|| bad "3. nodes 2/3 never said safe: $(st 2 'json.dumps(c.get("restart"))') / $(st 3 'json.dumps(c.get("restart"))')"
# 4
stop1 2; start 2 2 || exit 1; ready3
settled 2 && [ "$(st 2 'c.get("reconcile_witness", 0) != 0')" = True ] && [ "$(st 2 'c.get("reconcile_skipped")')" = False ] \
	&& ok "4. restarted once it said safe: node 2's reconcile found witness node $(st 2 'c["reconcile_witness"]'), as it said" \
	|| bad "4. node 2 restarted when safe: skipped=$(st 2 'c.get("reconcile_skipped")') witness=$(st 2 'c.get("reconcile_witness")')"
# 5: node 2 is young again - node 3 counts at most node 1
W3=$(st 3 'c["restart"]["witnesses"]')
[ "${W3:-9}" -le 1 ] 2>/dev/null \
	&& ok "5. node 2, just restarted, is no witness yet: node 3 counts $W3 witness(es), not 2" \
	|| bad "5. node 3 counts $W3 witnesses right after node 2's restart (want <= 1)"
# ---- ST8: a reconcile in progress makes its witnesses unsafe to restart -----
# 2026-10-10, production: a rolling restart went on while the first node's
# reconcile was still asking; both its witnesses restarted, and its pass
# waited for good - every peer now booted after its outage.  A node with a
# reconcile open stamps every frame (CLWIRE_F_RECONCILING); a peer then says
# restart.safe false with waits_for naming it.  A pass left with no witness
# says so (cluster.reconcile_no_witness_s, one WARNING after 30 s) and is
# released by restarting the node itself once that is witnessed.
nid() { st $1 'c["node"]'; }
wait_safe() { i=0; while [ $i -lt ${2:-90} ] && [ "$(st $1 'c["restart"]["safe"]')" != True ]; do sleep 1; i=$((i+1)); done; }
wait_safe 1; wait_safe 3
# both peers old enough to witness node 1's coming outage (node 2 restarted
# in case 4): node 1 counts 2 witnesses
i=0; while [ $i -lt 60 ] && [ "$(st 1 'c["restart"]["witnesses"]')" != 2 ]; do sleep 1; i=$((i+1)); done
python3 - <<'PY'
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 18971), 10); f = pcnative.wrap(s)
for i in range(3000):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": "set", "params": {"col": "0", "key": "w%04d" % i, "value": "a"}}) + "\n").encode()); f.flush()
    f.readline()
PY
sleep 2.5
N1=$(nid 1)
stop1 1
python3 - <<'PY'
import json, pcnative, socket
s = socket.create_connection(("127.0.0.1", 18972), 10); f = pcnative.wrap(s)
for i in range(3000):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": "set", "params": {"col": "0", "key": "w%04d" % i, "value": "b"}}) + "\n").encode()); f.flush()
    f.readline()
PY
start 1 3 || exit 1
ready3
# 6 (narrowed in 0.5.7.1): node 1's pass has TWO live witnesses - either may
# restart, one at a time; once one has, the other is its LAST witness and
# must not.  A peer names its only live witness in its keepalive.
i=0; while [ $i -lt 40 ] && [ "$(st 1 'c.get("reconcile_witnesses_live")')" != 2 ]; do sleep 0.25; i=$((i+1)); done
W2=$(st 2 'json.dumps(c["restart"])'); W3=$(st 3 'json.dumps(c["restart"])')
[ "$(st 1 'c.get("reconcile_witnesses_live")')" = 2 ] \
	&& [ "$(st 2 'c["restart"]["waits_for"]')$(st 3 'c["restart"]["waits_for"]')" = "[][]" ] \
	&& ok "6a. node 1 reconciles with 2 live witnesses: neither node 2 nor node 3 is its last, both may restart ($W2)" \
	|| bad "6a. node 1 witnesses_live $(st 1 'c.get("reconcile_witnesses_live")'): node 2 $W2, node 3 $W3 (want 2, waits_for [] on both)"
stop1 2; start 2 3 || exit 1
ready3
i=0; while [ $i -lt 40 ] && [ "$(st 3 'json.dumps(c["restart"].get("waits_for"))')" != "[$N1]" ]; do sleep 0.25; i=$((i+1)); done
W2=$(st 2 'json.dumps(c["restart"])'); W3=$(st 3 'json.dumps(c["restart"])')
[ "$(st 1 'c.get("reconcile_witnesses_live")')" = 1 ] && [ "$(st 3 'c["restart"]["safe"]')" = False ] \
	&& [ "$(st 3 'c["restart"]["waits_for"]')" = "[$N1]" ] && [ "$(st 2 'c["restart"]["waits_for"]')" = "[]" ] \
	&& ok "6b. node 2 restarted: node 3 is node 1's LAST witness - NOT safe, waits for node $N1 ($W3); node 2 waits for nobody" \
	|| bad "6b. after node 2's restart: node 1 witnesses_live $(st 1 'c.get("reconcile_witnesses_live")'), node 3 $W3, node 2 $W2 (want 1; node 3 safe false waits_for [$N1]; node 2 [])"
# 6c: the members view carries it - node 3 lists node 1 reconciling with
# node 3 as its sole witness (what the status page's chips read)
N3=$(nid 3)
MV=$(curl -s -m 3 "http://127.0.0.1:18983/members" | python3 -c '
import json, sys
m = [x for x in json.load(sys.stdin)["members"] if x["node"] == int(sys.argv[1])]
print("%s %s" % (m[0].get("reconciling"), m[0].get("sole_witness")) if m else "none")' "$N1" 2>/dev/null)
[ "$MV" = "True $N3" ] && ok "6c. /members on node 3: node $N1 reconciling, its sole witness node $N3" \
	|| bad "6c. /members on node 3 says node $N1: $MV (want True $N3)"
# 7: restart node 3 anyway - node 1's pass is left with no witness
stop1 3; start 3 2 || exit 1
ready3
i=0; while [ $i -lt 50 ] && ! grep -q "reconcile has had no witness" "$D/n1.log"; do sleep 1; i=$((i+1)); done
NW=$(st 1 'c.get("reconcile_no_witness_s")')
[ "${NW:-0}" -ge 30 ] 2>/dev/null && [ "$(grep -c "reconcile has had no witness" "$D/n1.log")" = 1 ] \
	&& grep -q "Restart THIS node" "$D/n1.log" \
	&& ok "7. its last witness restarted mid-pass: node 1 says it has had no witness for ${NW} s, and the WARNING once, naming the remedy" \
	|| bad "7. stuck pass: reconcile_no_witness_s ${NW:-?}, WARNING lines $(grep -c "reconcile has had no witness" "$D/n1.log")"
# 8: restart node 1 once that is witnessed - its pass finishes, the others are safe again
wait_safe 1 90
n0=$(grep -c "reconcile pass finished asking" "$D/n1.log")
stop1 1; start 1 4 || exit 1
ready3
i=0; while [ $i -lt 120 ] && [ "$(grep -c "reconcile pass finished asking" "$D/n1.log")" -le "$n0" ]; do sleep 1; i=$((i+1)); done
sleep 3
[ "$(grep -c "reconcile pass finished asking" "$D/n1.log")" -gt "$n0" ] && [ "$(st 1 'c.get("reconcile_no_witness_s")')" = 0 ] \
	&& [ "$(st 2 'c["restart"]["waits_for"]')$(st 3 'c["restart"]["waits_for"]')" = "[][]" ] \
	&& ok "8. node 1 restarted when witnessed: its pass finished, and nodes 2 and 3 wait for nobody" \
	|| bad "8. after node 1's restart: finished $(grep -c "reconcile pass finished asking" "$D/n1.log") (was $n0), no_witness_s $(st 1 'c.get("reconcile_no_witness_s")'), waits_for $(st 2 'c["restart"]["waits_for"]') $(st 3 'c["restart"]["waits_for"]')"
echo "restartwitnesstest: $pass passed, $fail failed"
[ $fail -eq 0 ]
