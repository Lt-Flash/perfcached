#!/bin/sh
# tombdeadtest.sh - S473: a hole in the deletes of an origin's run that is over.
#
# Only the origin, in that run, may floor a member's mark (S463).  Restart it
# with the member still short - AR13 on 0.7.4.4-rc6: 245 floored up to 72 of 246's
# 1,492 when 246 restarted - and nothing can move the mark: the peers keep no
# copies past their time rule, so cluster.acks_stuck stays up for good, and a key
# whose delete was in the hole stays on the member.  Three eager unicast nodes;
# node 1 the master (the highest address), node 2 under netcutshim, node 3 never
# restarts; the peers re-send no copies (a test flag, for that rule's 10 s tick):
#   1. node 2, cut from node 3 for a second, deletes 200 of 300 keys; node 1
#      takes them
#   2. healed, one more delete tells node 3 of the hole, and node 2 is killed
#      before node 3 asks; restarted, it is a new run
#   3. the mark reaches the head, floored by node 3 itself (gap_dead)
#   4. node 3 holds none of the 201 deleted keys and all 99 that live, with no
#      resync; node 1 holds none: nothing came back to it
#   5. no cluster.acks_stuck is left on any node
# GitLab 112775: past that rule was a race, node 1 resent all 200 in 2 of 3 runs;
# 112785: with the flag on gap asks alone, its 10 s catch-up closed the hole.
# Fail-first (GitLab 112769, both builds): the mark 0 of 201 after 120 s, 13 asks, 0
# resent, node 3 holding 200 of the 201 deleted keys, acks_stuck on nodes 1 and 2.
# Usage: test/tombdeadtest.sh [./perfcached] [./netcutshim.so]
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./netcutshim.so}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
export PERFCACHED_TEST_GAP_NO_COPIES=1
SANRT=$(ldd "$BIN" 2>/dev/null | awk "/libasan|libclang_rt\\.asan/ { print \$3; exit }")
D=$(mktemp -d /var/tmp/pctdd.XXXXXX)
P1= P2= P3=
trap 'for p in $P1 $P2 $P3; do kill -9 $p 2>/dev/null; done; [ -n "${KEEP:-}" ] && cp -a "$D" "$KEEP"; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
done_() { [ $fail -gt 0 ] && grep -hE "stuck|floored|run of|resync -|no longer holds|reconcile" "$D"/n3.log | grep -v "reconcile dropped" | tail -10 | cut -c1-170 | sed 's/^/    /'; echo "tombdeadtest: $pass passed, $fail failed"; [ $fail -eq 0 ]; exit; }
[ -f "$SHIM" ] || { echo "tombdeadtest: no netcut shim at $SHIM" >&2; exit 1; }
for p in 19581 19582 19583 19590; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "tombdeadtest: port $p already bound" >&2; exit 1; }
done
: > "$D/cut2"
ad() { [ "$1" = 1 ] && echo 9 || echo "$1"; }
for i in 1 2 3; do
	mkdir -p "$D/w$i" "$D/s$i"
	cat > "$D/n$i.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$i
[memory]
arena_mb = 64
[secrets]
client = td-client-secret
cluster = td-cluster-secret
[listen]
tcp = 127.0.161.$(ad $i):1958$i
plaintext = loopback
[cluster]
discovery = unicast
port = 19590
seeds = 127.0.161.9
advertise = 127.0.161.$(ad $i)
mode = eager
collections = c
[wal]
dir = $D/w$i
segment_mb = 8
probe = no
save = off
[collection c]
buckets_log2 = 12
C
	chmod 600 "$D/n$i.conf"
done
start() { # start <i>: node 2 under the shim; the log is kept across its restart
	if [ "$1" = 2 ]; then
		LD_PRELOAD="${SANRT:+$SANRT }$SHIM" PC_NETCUT_CTL="$D/cut2" PC_NETCUT_LOG="$D/cutlog2" \
			"$BIN" -f "$D/n2.conf" >> "$D/n2.log" 2>&1 &
	else
		"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	fi
	eval "P$1=\$!"
}
ready() { # ready <i> <how many ready lines>
	k=0; while [ $k -lt 300 ] && [ "$(grep -c "perfcached ready" "$D/n$1.log" 2>/dev/null)" -lt "$2" ]; do sleep 0.1; k=$((k+1)); done
	[ "$(grep -c "perfcached ready" "$D/n$1.log")" -ge "$2" ]
}
st() { # st <i> <python expr over r = /stats, c = its cluster>
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.161.%d" % (9 if i == 1 else i), 19580 + i), 10))
r = pcnative.call(f, "stats")["result"]; c = r["cluster"]
print(eval(sys.argv[2]))
PY
}
ops() { # ops <node> <set|del> <from> <to>: prints how many succeeded
	python3 - "$@" <<'PY'
import socket, sys, pcnative
n, op, a, b = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
f = pcnative.wrap(socket.create_connection(("127.0.161.%d" % (9 if n == 1 else n), 19580 + n), 10))
ok = 0
for i in range(a, b):
    k = "r:%d" % i
    r = pcnative.call(f, "set", col="c", key=k, value="v") if op == "set" else pcnative.call(f, "del", col="c", key=k)
    ok += "error" not in r
print(ok)
PY
}
held() { # held <node> <from> <to>: how many it holds
	python3 - "$@" <<'PY' 2>/dev/null
import socket, sys, pcnative
n, a, b = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
f = pcnative.wrap(socket.create_connection(("127.0.161.%d" % (9 if n == 1 else n), 19580 + n), 10))
print(sum(1 for i in range(a, b) if (pcnative.call(f, "get", col="c", key="r:%d" % i).get("result") or {}).get("found")))
PY
}
stuck() { st "$1" '[a["key"] for a in (r.get("alerts") or {}).get("active", []) if a["key"] == "cluster.acks_stuck"]'; }
start 1; ready 1 1 || { bad "node 1 did not start: $(tail -2 "$D/n1.log" | tr '\n' ' ')"; done_; }
T0=$(date +%s)
sleep 1
start 2; start 3
ready 2 1 && ready 3 1 || { bad "the members did not start"; done_; }
k=0; while [ $k -lt 150 ] && { [ "$(st 3 'c.get("peers_up")')$(st 2 'c.get("peers_up")')" != 22 ] || [ "$(st 3 'c["acks"]["fleet"]["confirming"]')" != True ]; }; do sleep 0.2; k=$((k+1)); done
[ "$(st 3 'c.get("peers_up")')" = 2 ] && [ "$(st 1 'c.get("role")')" = master ] \
	|| { bad "the fleet did not form with node 1 master: node 1 $(st 1 'c.get("role")'), node 3 peers $(st 3 'c.get("peers_up")')"; done_; }
# the pass's drops need a witness: a peer up 30 s before node 2's restart (S223)
w=$((33 - $(date +%s) + T0)); [ $w -gt 0 ] && sleep $w
k=0; while [ $k -lt 75 ]; do [ "$(ops 1 set 0 1)" = 1 ] && break; sleep 0.2; k=$((k+1)); done
S=$(ops 1 set 0 300)
k=0; while [ $k -lt 100 ] && [ "$(held 3 0 300)" != 300 ]; do sleep 0.2; k=$((k+1)); done
[ "$S" = 300 ] && [ "$(held 3 0 300)" = 300 ] || { bad "setup: node 1 took $S of 300, node 3 holds $(held 3 0 300)"; done_; }
# 1
echo "cut 127.0.161.3" > "$D/cut2"
sleep 0.3
DEL=$(ops 2 del 0 200)
[ "$DEL" = 200 ] || bad "1. node 2 took $DEL of the 200 deletes"
sleep 1
H3=$(held 3 0 200)
[ "$H3" = 200 ] && ok "1. node 2 deleted 200 of 300, cut from node 3, which still holds all 200" \
	|| { bad "1. node 3 holds $H3 of the 200: the cut let them through, or a read pulled"; done_; }
# 2: killed before node 3's first ask (GAP_WAIT_MS 500)
: > "$D/cut2"
sleep 0.2
python3 - "$P2" <<'PY'
import os, signal, socket, sys, time, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.161.2", 19582), 10))
pcnative.call(f, "del", col="c", key="r:200")
time.sleep(0.15)
os.kill(int(sys.argv[1]), signal.SIGKILL)
PY
wait "$P2" 2>/dev/null
start 2; ready 2 2 || { bad "2. node 2 did not restart: $(tail -2 "$D/n2.log" | tr '\n' ' ')"; done_; }
# 3
HOLE=
k=0; while [ $k -lt 50 ]; do
	HOLE=$(st 3 '[(m["origin"], m["incarn"], m["confirmed"], m["hi"]) for m in c["acks"]["marks"] if m["confirmed"] < m["hi"]]')
	case "$HOLE" in "[("*) break;; esac
	sleep 0.2; k=$((k+1))
done
case "$HOLE" in "[("*)
	ok "2. node 2 restarted with node 3 short of its run: $HOLE";;
	*) bad "2. node 3 has no hole to test ($HOLE): the kill came too late, gap_floored by node 2 $(st 2 'c["acks"]["gap_floored"]')"; done_;;
esac
k=0; L=1
while [ $k -lt 240 ]; do
	L=$(st 3 'len([m for m in c["acks"]["marks"] if m["confirmed"] < m["hi"]])')
	[ "$L" = 0 ] && break
	sleep 0.5; k=$((k+1))
done
[ "$L" = 0 ] && [ "$(st 3 'c["acks"].get("gap_dead", 0)')" -ge 1 ] 2>/dev/null \
	&& ok "3. node 3 floored the run that was over itself, after $((k / 2)) s (gap_dead $(st 3 'c["acks"].get("gap_dead")'))" \
	|| bad "3. node 3's mark still short after $((k / 2)) s: $(st 3 '[(m["confirmed"], m["hi"]) for m in c["acks"]["marks"] if m["confirmed"] < m["hi"]]'), gap_dead $(st 3 'c["acks"].get("gap_dead")'), asked $(st 3 'c["acks"]["gap_asked"]'), node 1 resent $(st 1 'c["acks"]["gap_resent"]')"
# 4: the pass opens at a beat, so what says it ran is its drops
k=0; while [ $k -lt 120 ] && [ "$(st 3 'c.get("reconciled", 0)')" -lt 200 ] 2>/dev/null; do sleep 0.5; k=$((k+1)); done
k=0; while [ $k -lt 240 ] && { [ "$(st 3 'c.get("reconcile_open")')" != False ] || [ "$(st 3 'c.get("reconcile_pending")')" != 0 ]; }; do sleep 0.5; k=$((k+1)); done
G3=$(held 3 0 201); K3=$(held 3 201 300); G1=$(held 1 0 201)
[ "$G3" = 0 ] && [ "$K3" = 99 ] && [ "$G1" = 0 ] \
	&& ok "4. node 3 holds none of the 201 deleted keys and all 99 that live; node 1 holds none" \
	|| bad "4. node 3 holds $G3 of the 201 deleted and $K3 of the 99 live, node 1 $G1 of the deleted; probed $(st 3 'c.get("reconcile_probed")'), dropped $(st 3 'c.get("reconciled")'), witness $(st 3 'c.get("reconcile_witness")'), skipped $(st 3 'c.get("reconcile_skipped")')"
[ "$(st 3 'c.get("resyncs")')" = 0 ] && [ "$(st 3 'r.get("state")')" = ready ] \
	&& ok "4. no resync: node 3 stayed ready" \
	|| bad "4. node 3: resyncs $(st 3 'c.get("resyncs")'), state $(st 3 'r.get("state")')"
# 5
k=0; while [ $k -lt 200 ]; do
	[ "$(stuck 1)$(stuck 2)$(stuck 3)" = "[][][]" ] && break
	sleep 0.5; k=$((k+1))
done
[ "$(stuck 1)$(stuck 2)$(stuck 3)" = "[][][]" ] && ok "5. no cluster.acks_stuck on any node" \
	|| bad "5. cluster.acks_stuck stands: node 1 $(st 1 '(r.get("alerts") or {}).get("active")' | cut -c1-200), node 2 $(stuck 2), node 3 $(stuck 3)"
done_
