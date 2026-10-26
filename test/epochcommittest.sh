#!/bin/sh
# epochcommittest.sh - DESIGN 12nu (UP12, 0.7.1): a build speaks a RANGE of wire
# epochs, a fleet speaks ONE committed epoch, and the commit is an explicit step
# the master takes once every live member can speak the new epoch.
#   - three eager members; nodes 2 and 3 claim epochs 2..3 (the test flag
#     PERFCACHED_TEST_EPOCH_MAX=3), node 1 speaks 2..2: the fleet speaks 2,
#     /stats cluster.epoch says so on each, /members shows every range;
#   - cluster_epoch_commit 3 on the master is refused naming node 1; on a
#     non-master it is refused as "not the master";
#   - node 1 restarted with the flag: `perfcli upgrade commit epoch=3` is
#     accepted; within a few beats every node speaks 3 (the master's keepalive
#     carries it), the commit is counted and logged;
#   - a node without the flag (speaks 2..2) now cannot join: refused by name,
#     epoch_refused counts it, membership does not move;
#   - the master restarted: the committed epoch survives (persisted, and the
#     new master had adopted it);
#   - `upgrade commit epoch=2` (down, every member speaks 2) is accepted.
# Usage: test/epochcommittest.sh [./perfcached] [./perfcli]
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
case "$CLI" in /*) ;; *) CLI=$(pwd)/$CLI;; esac
D=$(mktemp -d /var/tmp/pcepc.XXXXXX)
P1ID= P2ID= P3ID= P4ID=
trap 'for p in $P1ID $P2ID $P3ID $P4ID; do kill -9 $p 2>/dev/null; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 18811 18812 18813 18814 18821 18822 18823 18824; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "epochcommittest: port $p already bound" >&2; exit 1; }
done
node() { # node <n> [epoch max] - start member n; a max > 2 is the test flag
	mkdir -p "$D/state$1"
	[ -f "$D/n$1.conf" ] || cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state$1
[memory]
arena_mb = 64
[secrets]
client = epc-client-secret
cluster = epc-cluster-secret
enable = epc-enable
[listen]
tcp = 127.0.86.$1:1881$1
http = 127.0.86.$1:1882$1
plaintext = loopback
[cluster]
multicast = 239.255.78.26:18894
advertise = 127.0.86.$1
mode = eager
collections = 0
[collection 0]
buckets_log2 = 10
C
	chmod 600 "$D/n$1.conf"
	n0=$(grep -c "perfcached ready" "$D/n$1.log" 2>/dev/null || echo 0)
	if [ -n "${2:-}" ]; then PERFCACHED_TEST_EPOCH_MAX=$2 "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	else "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & fi
	eval "P${1}ID=\$!"
	i=0; while [ $i -lt 150 ] && [ "$(grep -c "perfcached ready" "$D/n$1.log")" -le "$n0" ]; do sleep 0.1; i=$((i+1)); done
	[ "$(grep -c "perfcached ready" "$D/n$1.log")" -gt "$n0" ] || { echo "node $1 did not start"; tail -5 "$D/n$1.log"; exit 1; }
}
st() { # st <n> <python expr over s (the node's /stats)>
	python3 - "$1" "$2" <<'PY' 2>&1
import json, sys, urllib.request
s = json.load(urllib.request.urlopen("http://127.0.86.%s:1882%s/stats" % (sys.argv[1], sys.argv[1]), timeout=5))
print(eval(sys.argv[2]))
PY
}
ep() { st "$1" "s['cluster']['epoch']['$2']"; }
commit() { # commit <n> <epoch> - the verb on node n (privileged); prints the result or the error
	python3 - "$1" "$2" <<'PY' 2>&1
import json, socket, sys
import pcnative
f = pcnative.wrap(socket.create_connection(("127.0.86.%s" % sys.argv[1], 18810 + int(sys.argv[1])), 10))
pcnative.call(f, "enable", secret="epc-enable")
r = pcnative.call(f, "cluster_epoch_commit", epoch=int(sys.argv[2]))
print(json.dumps(r.get("result", r.get("error", {}).get("message")), sort_keys=True))
PY
}
master() { for n in 1 2 3; do [ "$(st $n "s['cluster']['role']")" = master ] && { echo $n; return; }; done; echo 0; }
waitfor() { i=0; while [ $i -lt $(( $1 * 5 )) ] && ! eval "$2"; do sleep 0.2; i=$((i+1)); done; eval "$2"; }
T=$(cd "$(dirname "$0")" && pwd); PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH

node 1; node 2 3; node 3 3
waitfor 20 '[ "$(st 1 "s[\"cluster\"][\"peers_up\"]")" = 2 ] && [ "$(st 1 "s[\"state\"]")" = ready ] && [ "$(st 2 "s[\"state\"]")" = ready ] && [ "$(st 3 "s[\"state\"]")" = ready ]' && ok "three members up and ready" || bad "members: peers_up $(st 1 "s['cluster']['peers_up']")"
sleep 2
M=$(master); [ "$M" != 0 ] && ok "the master is node $M" || bad "no master"
# ---- 1. the ranges and the committed epoch ---------------------------------------------------
[ "$(ep 1 committed) $(ep 2 committed) $(ep 3 committed)" = "2 2 2" ] && [ "$(ep 1 max) $(ep 2 max) $(ep 3 max)" = "2 3 3" ] && [ "$(ep 1 min)" = 2 ] &&
	ok "1. the fleet speaks wire epoch 2; nodes 2 and 3 speak 2..3 (the test flag), node 1 speaks 2..2" || bad "1. committed $(ep 1 committed)/$(ep 2 committed)/$(ep 3 committed), max $(ep 1 max)/$(ep 2 max)/$(ep 3 max)"
r=$(st 1 "sorted(tuple(m['wire_epochs']) for m in s['cluster']['members'] if m.get('wire_epochs'))" 2>&1)
case "$r" in *"(2, 2)"*"(2, 3)"*) ok "1. /members lists every member's range: $r" ;; *) r=$(python3 -c 'import json,urllib.request; m=json.load(urllib.request.urlopen("http://127.0.86.1:18821/members",timeout=5)); print(sorted(tuple(x["wire_epochs"]) for x in (m.get("members") or m) if isinstance(x,dict) and x.get("wire_epochs")))' 2>&1); case "$r" in *"(2, 2)"*"(2, 3)"*) ok "1. /members lists every member's range: $r" ;; *) bad "1. members' wire_epochs: $r" ;; esac ;; esac
grep -q "PERFCACHED_TEST_EPOCH_MAX=3" "$D/n3.log" && ok "1. the test flag is announced in the log (a build that claims an epoch it does not have)" || bad "1. no flag warning in n3.log"
# ---- 2. a commit refused: node 1 cannot speak 3; a non-master cannot commit ------------------
r=$(commit $M 3)
echo "$r" | grep -q "node .* speaks wire epochs 2..2, not 3" && ok "2. commit 3 on the master is refused naming the node that speaks 2..2: $r" || bad "2. commit 3 on the master: $r"
NM=$(( M % 3 + 1 ))
r=$(commit $NM 3)
echo "$r" | grep -q "not the master" && ok "2. commit on node $NM (not the master) is refused: $r" || bad "2. commit on a non-master: $r"
[ "$(ep $M committed)" = 2 ] && [ "$(ep $M commits)" = 0 ] && ok "2. nothing committed (fleet still 2, commits 0)" || bad "2. committed $(ep $M committed), commits $(ep $M commits)"
# ---- 3. node 1 upgraded (restarted with the flag): the commit goes through -----------------------
kill $P1ID 2>/dev/null; wait $P1ID 2>/dev/null; P1ID=
node 1 3
waitfor 20 '[ "$(st 1 "s[\"state\"]")" = ready ] && [ "$(st 3 "s[\"cluster\"][\"peers_up\"]")" = 2 ]' || bad "3. node 1 did not rejoin"
sleep 2
M=$(master)
r=$("$CLI" -q -h 127.0.86.$M -p 1881$M -E epc-enable upgrade commit epoch=3 2>&1)
echo "$r" | grep -q "the fleet speaks wire epoch 3 now" && ok "3. perfcli upgrade commit epoch=3 on the master: $r" || bad "3. perfcli commit: $r"
waitfor 10 '[ "$(ep 1 committed) $(ep 2 committed) $(ep 3 committed)" = "3 3 3" ]' && ok "3. every node speaks 3 within a few beats (the master's keepalive carries the commit)" || bad "3. committed $(ep 1 committed)/$(ep 2 committed)/$(ep 3 committed)"
[ "$(ep $M commits)" = 1 ] && grep -q "wire epoch 2 -> 3 committed here" "$D/n$M.log" && ok "3. the master counted and logged the commit" || bad "3. commits $(ep $M commits); log: $(grep -h 'committed' "$D/n$M.log" | tail -1 | cut -c1-120)"
for n in 1 2 3; do [ $n = $M ] || { grep -q "wire epoch 2 -> 3 committed by node" "$D/n$n.log" && ok "3. node $n adopted the commit from the keepalive (log)" || bad "3. node $n: $(grep -h 'committed' "$D/n$n.log" | tail -1 | cut -c1-120)"; break; }; done
[ "$(st $M "s['cluster']['wire_epoch']")" = 3 ] && ok "3. cluster.wire_epoch reads the one spoken (3)" || bad "3. wire_epoch $(st $M "s['cluster']['wire_epoch']")"
# ---- 4. a build that speaks 2..2 cannot join a fleet committed to 3 ------------------------------
R0=$(st $M "s['cluster']['epoch_refused']")
node 4
sleep 4
[ "$(st 1 "s['cluster']['peers_up']")" = 2 ] && [ "$(st 4 "s['cluster']['peers_up']")" = 0 ] && grep -q "REFUSING" "$D/n4.log" "$D/n$M.log" &&
	ok "4. node 4 (speaks 2..2) is refused: it joins nobody, membership stays at 3 (REFUSING logged)" || bad "4. peers_up 1: $(st 1 "s['cluster']['peers_up']"), 4: $(st 4 "s['cluster']['peers_up']"); refusals: $(grep -h -c REFUSING "$D"/n*.log | tr '\n' ' ')"
[ "$(st $M "s['cluster']['epoch_refused']")" -gt "$R0" ] && ok "4. epoch_refused counts it on the master ($R0 -> $(st $M "s['cluster']['epoch_refused']"))" || bad "4. epoch_refused $R0 -> $(st $M "s['cluster']['epoch_refused']")"
# the fleet SPEAKS 3: node 4 refuses the master's keepalive by both numbers before it could even adopt
grep -q -E "REFUSING master keepalive .* speaks cluster wire epoch 3 and we speak 2..2|committed the fleet to wire epoch 3, which this build does not speak" "$D/n4.log" && ok "4. node 4 says why it cannot stay: the fleet speaks 3, it speaks 2..2" || bad "4. n4.log: $(grep -h -E 'epoch|REFUS' "$D/n4.log" | tail -2 | cut -c1-140 | tr '\n' '|')"
kill -9 $P4ID 2>/dev/null; P4ID=
# ---- 5. the master restarted: the committed epoch survives ------------------------------------------
eval "mp=\$P${M}ID"; kill $mp 2>/dev/null; wait $mp 2>/dev/null; eval "P${M}ID="
sleep 3
NEWM=$(for n in 1 2 3; do [ $n = $M ] && continue; [ "$(st $n "s['cluster']['role']")" = master ] && { echo $n; break; }; done)
[ -n "$NEWM" ] && [ "$(ep $NEWM committed)" = 3 ] && ok "5. the master is gone: node $NEWM leads and the fleet still speaks 3" || bad "5. new master '${NEWM:-none}', committed $( [ -n "$NEWM" ] && ep $NEWM committed)"
node $M 3
waitfor 20 '[ "$(st '$M' "s[\"state\"]")" = ready ]' || bad "5. node $M did not come back"
sleep 2
[ "$(ep $M committed)" = 3 ] && [ -f "$D/state$M/node-epoch" ] && [ "$(cat "$D/state$M/node-epoch")" = 3 ] && ok "5. node $M is back speaking 3 (node-epoch persisted it)" || bad "5. node $M committed $(ep $M committed), file: $(cat "$D/state$M/node-epoch" 2>&1)"
# ---- 6. a commit down to 2 while every member speaks 2..3 -------------------------------------------
M=$(master)
r=$(commit $M 2)
echo "$r" | grep -q '"committed": 2' && waitfor 10 '[ "$(ep 1 committed) $(ep 2 committed) $(ep 3 committed)" = "2 2 2" ]' && ok "6. commit 2 (every member speaks it) is accepted and the fleet speaks 2 again" || bad "6. commit 2: $r; committed $(ep 1 committed)/$(ep 2 committed)/$(ep 3 committed)"
r=$(commit $M 2)
echo "$r" | grep -q "already" && ok "6. committing the epoch spoken is refused as a no-op: $r" || bad "6. repeat commit: $r"
grep -q -E 'CRIT' "$D"/n[123].log && bad "a node logged CRIT: $(grep -h -m1 CRIT "$D"/n[123].log | cut -c1-120)" || ok "no CRIT in the members' logs"
echo "epochcommittest: $pass passed, $fail failed"
[ $fail -eq 0 ]
