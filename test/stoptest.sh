#!/bin/sh
# stoptest.sh - S222: a clean stop leaves a snapshot, so a node that was
# stopped on purpose comes back COMPLETE on its own.
#
# An eager replica never logs the copies it receives - its WAL holds only
# what that node WROTE.  Before S222 a clean stop took no snapshot, so
# after the whole fleet was stopped every node came back off its WAL with
# its own third and waited on the repair sweep for the rest: none of them
# could have served the keyspace alone, and a node restarted by itself
# answered "not found" for two thirds of it.  The snapshot walk stores the
# copies too, and a clean stop now takes one after every thread has
# joined.
#
# Asserted (three eager nodes, WAL on, `save = off` so the ONLY snapshot
# is the one the stop takes; 100 keys authored per node):
#   1. SIGTERM the fleet: every node logs its shutdown snapshot; node 3,
#      restarted ALONE, serves all 300 keys - two thirds of them copies
#      it could only have had from that snapshot;
#   2. bring 1 and 2 back: all 300 on all three, nothing reconcile-dropped;
#   3. kill -9 the fleet instead: no snapshot is taken, and node 3 alone
#      has its own 100 - crash behaviour is exactly what it was;
#   4. THE FLEET STOP: `fleetstop` without privilege is refused; with it
#      (enable, then fleetstop, on one connection) node 1 reports both
#      peers stopped, all three exit cleanly with a snapshot each, node 1
#      last, and node 2 restarted alone serves all 300;
#   5. FAIL-FIRST, a step of the run: scenario 1 on a build with the stop
#      snapshot short-circuited must leave the lone node with 100.
#   6. S233, the split RV-5's fault run left behind: a member whose own
#      claimed term is ABOVE the map it follows (node 3 joins with a
#      persisted term of 50) still obeys its master's fleet stop - it was
#      judged against the claim and ignored as "a fleet that has since
#      moved on".  And a peer that leaves WITHOUT a goodbye (kill -9 just
#      before the stop) is reported "lost", not "stopped".  Both run on a
#      build with the two fixes reverted too, where both must fail.
#   7. S233, perfcli: a stop that outwaits the client's read timeout (a
#      peer that ignores it - the reverted build as node 3 - holds it the
#      whole timeout_ms, 40 s here).  Piped, perfcli's 30 s timeout
#      expires first; it redials and must NOT send the stop again (it
#      did: the fault run's second "asking").  One-shot `perfcli
#      fleetstop 40000` waits long enough to get the answer, naming the
#      peer still up.  A perfcli
#      with both changes reverted, built here, must fail both.
# Usage: test/stoptest.sh [./perfcached] [./perfcli]
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
D=$(mktemp -d /var/tmp/pcst.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
MC=239.255.77.89
MP=17189
PER=100
TOT=$((PER * 3))

echo "--- building the variant without the stop snapshot, for the fail-first"
mkdir -p "$D/t"
tar -cf - --exclude=./.git --exclude='*.o' --exclude='*.a' . 2>/dev/null |
	(cd "$D/t" && tar -xf -) || { echo "cannot copy the tree"; exit 1; }
sed -i 's/if (pc_rdb_save_on_stop() != 0)/if (0 \&\& pc_rdb_save_on_stop() != 0)/' "$D/t/src/daemon.c"
grep -q "if (0 && pc_rdb_save_on_stop() != 0)" "$D/t/src/daemon.c" ||
	{ echo "the sed missed - the stop snapshot is still there"; exit 1; }
make -C "$D/t" -j"$(nproc 2>/dev/null || echo 4)" perfcached > "$D/build.log" 2>&1 || {
	echo "the variant did not build:"; tail -5 "$D/build.log"; exit 1; }
cp "$D/t/perfcached" "$D/perfcached.nostop"
# S233's variant: the stop gate on the node's own claim again, and
# "stopped" for anything that left the live set
sed -i 's/ours = C.map_valid ? C.map.term : pc_term_current();/ours = pc_term_current();/' "$D/t/src/cluster.c"
sed -i 's/} else if (bye_since(targets\[i\], bye_mark)) {/} else if (1 || bye_since(targets[i], bye_mark)) {/' "$D/t/src/cluster.c"
grep -q "ours = pc_term_current();" "$D/t/src/cluster.c" && grep -q "} else if (1 || bye_since" "$D/t/src/cluster.c" ||
	{ echo "the S233 sed missed"; exit 1; }
sed -i 's/if (0 \&\& pc_rdb_save_on_stop() != 0)/if (pc_rdb_save_on_stop() != 0)/' "$D/t/src/daemon.c"
make -C "$D/t" -j"$(nproc 2>/dev/null || echo 4)" perfcached > "$D/build2.log" 2>&1 || {
	echo "the S233 variant did not build:"; tail -5 "$D/build2.log"; exit 1; }
cp "$D/t/perfcached" "$D/perfcached.s233old"
# and perfcli before S233: resends anything after a redial, 30 s for all
sed -i 's/if (unsafe_twice(method)) {/if (0 \&\& unsafe_twice(method)) {/' "$D/t/cli/perfcli.c"
sed -i 's/o.io_timeout_ms = 125000;/o.io_timeout_ms = 30000;/' "$D/t/cli/perfcli.c"
grep -q "if (0 && unsafe_twice(method)) {" "$D/t/cli/perfcli.c" && grep -q "o.io_timeout_ms = 30000;$" "$D/t/cli/perfcli.c" ||
	{ echo "the perfcli sed missed"; exit 1; }
make -C "$D/t" perfcli > "$D/build3.log" 2>&1 || {
	echo "the perfcli variant did not build:"; tail -5 "$D/build3.log"; exit 1; }
cp "$D/t/perfcli" "$D/perfcli.s233old"

conf() { # conf <n>
	mkdir -p "$D/wal$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = st-client-secret
cluster = st-cluster-secret
enable = st-enable-secret
[listen]
tcp = 127.0.0.1:1734$1
plaintext = loopback
[cluster]
multicast = $MC:$MP
advertise = 127.0.13.$1
pull_timeout_ms = 400
mode = eager
collections = c
[collection c]
buckets_log2 = 12
[wal]
dir = $D/wal$1
probe = no
fsync = always
segment_mb = 8
segments = 4
save = off
EOF
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> <binary>
	"$2" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0
	while [ $i -lt 150 ]; do
		[ "$(grep -c "perfcached ready" "$D/n$1.log" 2>/dev/null)" -gt "$(cat "$D/n$1.readies" 2>/dev/null || echo 0)" ] && {
			grep -c "perfcached ready" "$D/n$1.log" > "$D/n$1.readies"; return 0; }
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"
	return 1
}
stop_clean() { # stop_clean <n>: SIGTERM and wait for the clean-shutdown line
	pid=$(cat "$D/n$1.pid" 2>/dev/null) || return 0
	# grep -c prints 0 AND exits 1 on no match, so "|| echo 0" would say
	# it twice - default the empty case instead
	before=$(grep -c "clean shutdown" "$D/n$1.log" 2>/dev/null); before=${before:-0}
	kill -TERM "$pid" 2>/dev/null
	i=0
	while [ $i -lt 300 ]; do
		kill -0 "$pid" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	rm -f "$D/n$1.pid"
	[ "$(grep -c "clean shutdown" "$D/n$1.log")" -gt "$before" ]
}
stop_kill() { # stop_kill <n>
	kill -9 "$(cat "$D/n$1.pid" 2>/dev/null)" 2>/dev/null; rm -f "$D/n$1.pid"
}
call() { printf '%s\n' "$2" | timeout 15 python3 -c '
import json, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10)
r = json.loads(sys.stdin.read()); r["id"] = 1; r["jsonrpc"] = "2.0"
s.sendall(json.dumps(r).encode() + b"\n")
print(s.makefile("rb").readline().decode().strip())' "$1"; }
cl() { call "$1" '{"method":"stats"}' | python3 -c \
	'import json,sys
try: print((json.load(sys.stdin)["result"]["cluster"]).get(sys.argv[1], "?"))
except Exception: print("?")' "$2" 2>/dev/null || echo "?"; }
entries() { call "$1" '{"method":"collections"}' | python3 -c \
	'import json,sys
try: print((json.load(sys.stdin)["result"]["collections"] or [{}])[0].get("entries",-1))
except Exception: print(-1)' 2>/dev/null || echo -1; }
fill() { timeout 60 python3 -c '
import json, socket, sys
port, pre, n = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])
f = socket.create_connection(("127.0.0.1", port), timeout=20).makefile("rwb")
for i in range(n):
    f.write((json.dumps({"jsonrpc":"2.0","id":i,"method":"set",
        "params":{"col":"c","key":"%s%03d" % (pre,i),"value":"v%s%03d" % (pre,i)}})+"\n").encode())
f.flush()
for _ in range(n): f.readline()' "$1" "$2" "$3"; }
# how many of the 3*PER keys a node answers for LOCALLY: a node with no
# peers cannot forward, so this is what it holds
present() { timeout 60 python3 -c '
import json, socket, sys
port, n = int(sys.argv[1]), int(sys.argv[2])
f = socket.create_connection(("127.0.0.1", port), timeout=20).makefile("rwb")
keys = ["%s%03d" % (p, i) for p in ("a","b","c") for i in range(n)]
for j, k in enumerate(keys):
    f.write((json.dumps({"jsonrpc":"2.0","id":j,"method":"get",
        "params":{"col":"c","key":k}})+"\n").encode())
f.flush()
good = 0
for _ in range(len(keys)):
    m = json.loads(f.readline()); r = m.get("result") or {}
    if r.get("value") == "v" + keys[m.get("id", 0)]: good += 1
print(good)' "$1" "$2"; }
converged() { # converged <count> <seconds> <ports...>
	want=$1; secs=$2; shift 2
	i=0
	while [ $i -lt $((secs * 2)) ]; do
		all=1
		for p in "$@"; do [ "$(entries "$p")" = "$want" ] || all=0; done
		[ $all = 1 ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}
load_fleet() { # load_fleet <binary>
	rm -rf "$D"/wal1 "$D"/wal2 "$D"/wal3 "$D"/n1.log "$D"/n2.log "$D"/n3.log "$D"/*.readies
	conf 1; conf 2; conf 3
	start 1 "$1" && start 2 "$1" && start 3 "$1" || return 1
	sleep 4
	fill 17341 a $PER; fill 17342 b $PER; fill 17343 c $PER
	converged $TOT 30 17341 17342 17343
}

# ---- 1. a clean fleet stop, and one node back on its own ---------------
echo "--- 1. SIGTERM the fleet, restart node 3 ALONE"
load_fleet "$BIN" || { bad "the fleet did not load"; echo "stoptest: $pass passed, $fail failed"; exit 1; }
ok "loaded: $TOT keys on all three (100 authored by each - the rest are copies no WAL holds)"
S1=0; for n in 1 2 3; do stop_clean $n && S1=$((S1+1)); done
[ $S1 = 3 ] && ok "all three stopped cleanly" || bad "only $S1 of 3 stopped cleanly"
SNAP=0; for n in 1 2 3; do grep -q "taking the shutdown snapshot" "$D/n$n.log" && SNAP=$((SNAP+1)); done
[ $SNAP = 3 ] && ok "and every one took its shutdown snapshot: $(grep -h "rdb: snapshot" "$D/n3.log" | tail -1 | sed 's/.*rdb: //' | cut -c1-60)" \
	|| bad "only $SNAP of 3 logged a shutdown snapshot"
start 3 "$BIN" || bad "node 3 did not restart"
sleep 2
P3=$(present 17343 $PER)
[ "$P3" = $TOT ] && ok "node 3, ALONE, serves all $TOT keys - 200 of them are copies it held for its peers, and only the snapshot could have brought them back" \
	|| bad "node 3 alone serves $P3 of $TOT (without the stop snapshot it would be $PER)"

# ---- 2. the rest come back ---------------------------------------------
echo "--- 2. bring nodes 1 and 2 back"
start 1 "$BIN" || bad "node 1 did not restart"
start 2 "$BIN" || bad "node 2 did not restart"
sleep 20
A=$(present 17341 $PER); B=$(present 17342 $PER); C=$(present 17343 $PER)
R=$(( $(cl 17341 reconciled) + $(cl 17342 reconciled) + $(cl 17343 reconciled) ))
[ "$A" = $TOT ] && [ "$B" = $TOT ] && [ "$C" = $TOT ] && [ $R = 0 ] \
	&& ok "all $TOT on all three, $R reconcile-dropped" \
	|| bad "after the rest returned: $A/$B/$C of $TOT, $R dropped"
for n in 1 2 3; do stop_kill $n; done

# ---- 3. a crash is still a crash ---------------------------------------
echo "--- 3. kill -9 the fleet instead: no snapshot"
load_fleet "$BIN" || bad "the fleet did not load"
for n in 1 2 3; do stop_kill $n; done
sleep 1
grep -q "taking the shutdown snapshot" "$D/n3.log" && bad "a killed node claims a shutdown snapshot" \
	|| ok "no node took a shutdown snapshot - there was no shutdown"
start 3 "$BIN" || bad "node 3 did not restart"
sleep 2
P3=$(present 17343 $PER)
[ "$P3" = $PER ] && ok "node 3 alone has its own $PER - the WAL road, exactly as before" \
	|| bad "after kill -9, node 3 alone serves $P3 (want $PER)"
stop_kill 3

# ---- 4. the fleet stop --------------------------------------------------
echo "--- 4. fleetstop: one privileged request stops the whole fleet"
load_fleet "$BIN" || bad "the fleet did not load"
R=$(call 17341 '{"method":"fleetstop"}')
echo "$R" | grep -q "privileged" && ok "without privilege it is refused: $(echo "$R" | sed 's/.*"message":"//' | cut -c1-60)..." \
	|| bad "an unprivileged fleetstop was not refused: $R"
[ -f "$D/n1.pid" ] && kill -0 "$(cat "$D/n1.pid")" 2>/dev/null && ok "and nothing stopped" || bad "a refused fleetstop stopped node 1"
R=$(timeout 60 python3 -c '
import json, socket
f = socket.create_connection(("127.0.0.1", 17341), timeout=50).makefile("rwb")
for i, (m, p) in enumerate([("enable", {"secret": "st-enable-secret"}), ("fleetstop", {"timeout_ms": 20000})]):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": m, "params": p}) + "\n").encode()); f.flush()
    last = f.readline().decode().strip()
print(last)')
echo "$R" | python3 -c 'import json,sys
r=json.load(sys.stdin).get("result") or {}
sys.exit(0 if r.get("stopping") and r.get("asked")==2 and len(r.get("stopped",[]))==2 and r.get("still_up")==[] else 1)' \
	&& ok "privileged, it answers: $(echo "$R" | sed 's/.*"result"://' | cut -c1-70)" \
	|| bad "the fleetstop reply was not two peers stopped: $R"
i=0
while [ $i -lt 100 ]; do
	up=0; for n in 1 2 3; do [ -f "$D/n$n.pid" ] && kill -0 "$(cat "$D/n$n.pid")" 2>/dev/null && up=$((up+1)); done
	[ $up = 0 ] && break
	sleep 0.2; i=$((i+1))
done
[ $up = 0 ] && ok "all three processes have exited" || bad "$up node(s) still running after the fleet stop"
C=0; S=0
for n in 1 2 3; do
	grep -q "clean shutdown" "$D/n$n.log" && C=$((C+1))
	grep -q "taking the shutdown snapshot" "$D/n$n.log" && S=$((S+1))
	rm -f "$D/n$n.pid"
done
[ $C = 3 ] && [ $S = 3 ] && ok "each one stopped cleanly and took its snapshot" || bad "clean stops $C/3, snapshots $S/3"
grep -q "FLEET STOP requested by node" "$D/n2.log" && grep -q "FLEET STOP requested by node" "$D/n3.log" \
	&& ok "nodes 2 and 3 logged who asked" || bad "a peer did not log the request"
L1=$(grep -h "clean shutdown" "$D/n1.log" | tail -1 | cut -c8-15)
L2=$(grep -h "clean shutdown" "$D/n2.log" | tail -1 | cut -c8-15)
L3=$(grep -h "clean shutdown" "$D/n3.log" | tail -1 | cut -c8-15)
[ "$L1" \> "$L2" -o "$L1" = "$L2" ] && [ "$L1" \> "$L3" -o "$L1" = "$L3" ] \
	&& ok "and node 1 went last ($L2, $L3, then $L1)" || bad "node 1 did not go last ($L1 vs $L2, $L3)"
start 2 "$BIN" || bad "node 2 did not restart"
sleep 2
P2=$(present 17342 $PER)
[ "$P2" = $TOT ] && ok "node 2, restarted alone, serves all $TOT" || bad "node 2 alone serves $P2 of $TOT"
stop_kill 2

# ---- 5. fail-first -----------------------------------------------------
echo "--- 5. fail-first: scenario 1 on a build without the stop snapshot"
load_fleet "$D/perfcached.nostop" || bad "the fleet did not load"
for n in 1 2 3; do stop_clean $n; done
start 3 "$D/perfcached.nostop" || bad "node 3 did not restart"
sleep 2
P3=$(present 17343 $PER)
[ "$P3" = $PER ] && ok "without it, node 3 alone serves only its own $PER of $TOT - which is what scenario 1 fixed" \
	|| bad "without the stop snapshot node 3 alone serves $P3 - scenario 1 proves nothing"
stop_kill 3

# ---- 6. S233 ------------------------------------------------------------
mapterm() { call "$1" '{"method":"stats"}' | python3 -c \
	'import json,sys
try: print(json.load(sys.stdin)["result"]["cluster"]["map"]["term"])
except Exception: print("?")' 2>/dev/null || echo "?"; }
priv_stop() { timeout 60 python3 -c '
import json, socket
f = socket.create_connection(("127.0.0.1", 17341), timeout=50).makefile("rwb")
for i, (m, p) in enumerate([("enable", {"secret": "st-enable-secret"}), ("fleetstop", {"timeout_ms": 15000})]):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": m, "params": p}) + "\n").encode()); f.flush()
    last = f.readline().decode().strip()
print(last)'; }
reply() { echo "$1" | python3 -c 'import json,sys
r=json.load(sys.stdin).get("result") or {}
print("asked=%s stopped=%d still_up=%d lost=%s" % (r.get("asked"), len(r.get("stopped",[])), len(r.get("still_up",[])), len(r.get("lost",[])) if "lost" in r else "-"))' 2>/dev/null; }
# running = alive and not a zombie: this arm runs inside $(...), so a
# daemon that exited stays unreaped while the subshell waits, and kill -0
# answers for a zombie
alive() { p=$(cat "$D/n$1.pid" 2>/dev/null) || return 1
	s=$(awk '{print $3}' "/proc/$p/stat" 2>/dev/null) || return 1
	[ -n "$s" ] && [ "$s" != Z ]; }
all_down() { i=0
	while [ $i -lt 150 ]; do
		up=0; for n in 1 2 3; do alive $n && up=$((up+1)); done
		[ $up = 0 ] && break
		sleep 0.2; i=$((i+1))
	done; }
split_fleet() { # split_fleet <binary>: nodes 1+2, then 3 with term 50
	for n in 1 2 3; do stop_kill $n; done
	rm -rf "$D"/wal1 "$D"/wal2 "$D"/wal3 "$D"/n1.log "$D"/n2.log "$D"/n3.log "$D"/*.readies
	conf 1; conf 2; conf 3
	start 1 "$1" && start 2 "$1" || return 1
	sleep 3
	# check = FNV-1a over the term's four LE bytes (clterm.c's format)
	printf 'perfcached-node-term 1\nterm 50\ncheck 8d700e0ae6ddc407\n' > "$D/wal3/node-term"
	start 3 "$1" || return 1
	sleep 4
}
s233() { # s233 <binary> <label>: prints "<split ok> <stop ok> <lost ok>"
	split_fleet "$1" || { echo "0 0 0"; return; }
	T3=$(cl 17343 term); M3=$(mapterm 17343)
	sp=0; [ "$T3" != "?" ] && [ "$M3" != "?" ] && [ "$T3" -ge 50 ] && [ "$M3" -lt 50 ] && sp=1
	echo "   $2: node 3 claims term $T3 and follows a map at term $M3" >&2
	R=$(priv_stop); echo "   $2: fleetstop -> $(reply "$R")" >&2
	all_down
	st=0; [ "$up" = 0 ] && ! grep -q "IGNORING a fleet stop" "$D/n3.log" && st=1
	grep -h "IGNORING a fleet stop" "$D/n3.log" | head -1 | sed "s/^/   $2: node 3: /" >&2
	echo "   $2: $up process(es) still running after the stop" >&2
	for n in 1 2 3; do stop_kill $n; done
	# lost: a fleet that loads, node 3 killed -9 just before the stop
	load_fleet "$1" >/dev/null 2>&1
	stop_kill 3
	R=$(priv_stop); echo "   $2: node 3 killed -9, then fleetstop -> $(reply "$R")" >&2
	lo=0; echo "$R" | python3 -c 'import json,sys
r=json.load(sys.stdin).get("result") or {}
sys.exit(0 if r.get("asked")==2 and len(r.get("stopped",[]))==1 and len(r.get("lost",[]))==1 else 1)' && lo=1
	all_down
	for n in 1 2 3; do stop_kill $n; done
	echo "$sp $st $lo"
}
echo "--- 6. S233: a claimed term above the followed map; a stop that is not a goodbye"
set -- $(s233 "$BIN" "this build")
[ "$1" = 1 ] && ok "node 3 joined claiming a term above the map it follows (the split F3 left)" \
	|| bad "the split was not set up - the arm tests nothing"
[ "$2" = 1 ] && ok "its master's fleet stop stopped it anyway - all three exited, nothing ignored" \
	|| bad "the fleet stop left a node running or was ignored"
[ "$3" = 1 ] && ok "a peer killed -9 before the stop is reported lost, not stopped" \
	|| bad "a peer that never said goodbye was not reported lost"
set -- $(s233 "$D/perfcached.s233old" "fixes reverted")
[ "$1" = 1 ] && [ "$2" = 0 ] && ok "fail-first: with the gate on the claim again, node 3 ignored the stop and kept running" \
	|| bad "fail-first: the reverted gate did not leave node 3 running (split $1, stop $2)"
[ "$3" = 0 ] && ok "fail-first: with 'stopped' = 'left the live set', the killed peer was reported stopped" \
	|| bad "fail-first: the reverted reply still reported the killed peer lost"

# ---- 7. S233, perfcli ------------------------------------------------------
stuck_fleet() { # nodes 1+2 on this build, node 3 on the reverted one
	for n in 1 2 3; do stop_kill $n; done
	rm -rf "$D"/wal1 "$D"/wal2 "$D"/wal3 "$D"/n1.log "$D"/n2.log "$D"/n3.log "$D"/*.readies
	conf 1; conf 2; conf 3
	start 1 "$BIN" && start 2 "$BIN" || return 1
	sleep 3
	printf 'perfcached-node-term 1\nterm 50\ncheck 8d700e0ae6ddc407\n' > "$D/wal3/node-term"
	start 3 "$D/perfcached.s233old" || return 1
	sleep 4
}
asks() { grep -c "FLEET STOP - asking" "$D/n1.log" 2>/dev/null; }
cli7() { # cli7 <perfcli> <label>: prints "<pipe ok> <oneshot ok>"
	stuck_fleet || { echo "0 0"; return; }
	printf '{"method":"fleetstop","params":{"timeout_ms":40000}}\n' |
		timeout 100 "$1" -q -h 127.0.0.1 -p 17341 -E st-enable-secret > "$D/cli7p.out" 2>&1
	sleep 12                           # the 40 s wait ends, node 1 goes
	a=$(asks); echo "   $2, piped: node 1 was asked to stop $a time(s); perfcli: $(grep -m1 "not sending it again\|reconnected" "$D/cli7p.out" | cut -c1-90)" >&2
	pp=0; [ "$a" = 1 ] && grep -q "not sending it again" "$D/cli7p.out" && pp=1
	stuck_fleet || { echo "$pp 0"; return; }
	# 40 s: past the 30 s read timeout perfcli used for everything
	timeout 150 "$1" -q -h 127.0.0.1 -p 17341 -E st-enable-secret fleetstop 40000 > "$D/cli7o.out" 2>&1
	rc=$?; a=$(asks)
	echo "   $2, one-shot: exit $rc, asked $a time(s): $(grep -m1 'still_up\|reconnected' "$D/cli7o.out" | cut -c1-90)" >&2
	oo=0; [ $rc = 0 ] && [ "$a" = 1 ] && grep -q '"still_up":\[[0-9]' "$D/cli7o.out" && oo=1
	for n in 1 2 3; do stop_kill $n; done
	echo "$pp $oo"
}
echo "--- 7. S233: perfcli and a stop that outwaits its read timeout"
set -- $(cli7 "$CLI" "this perfcli")
[ "$1" = 1 ] && ok "piped: the read timed out, perfcli redialled and did NOT send the stop again" \
	|| bad "piped: perfcli sent the fleet stop again after its redial, or said nothing"
[ "$2" = 1 ] && ok "one-shot: it waited for the answer, which names the peer still up" \
	|| bad "one-shot fleetstop did not get its answer"
set -- $(cli7 "$D/perfcli.s233old" "perfcli before S233")
[ "$1" = 0 ] && ok "fail-first: the old perfcli sent the stop twice, piped" \
	|| bad "fail-first: the old perfcli did not resend - the piped arm proves nothing"
[ "$2" = 0 ] && ok "fail-first: and one-shot it timed out before the answer" \
	|| bad "fail-first: the old perfcli got the one-shot answer - the timeout arm proves nothing"

echo "stoptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
