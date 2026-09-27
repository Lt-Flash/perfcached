#!/bin/sh
# outagetest.sh - S223: a whole-fleet outage must not make the fleet
# delete its own data.
#
# THE DEFECT, measured on 222 on 2026-09-22 before this existed: three
# eager nodes, 900 keys with each node authoring 300, killed together
# with no snapshot.  Restarted together, 600 of 900 keys were GONE.
# Restarted one by one it was worse and read as correct behaviour all
# the way down: node 1 came back alone and recovered its 300; node 2
# joined with its own durable 300 and DELETED 256 of them within 5
# seconds, all 300 within 15.
#
# Every step of that was working as designed.  An eager replica does not
# persist the copies it receives, so each node's WAL holds only what it
# AUTHORED and a whole-fleet outage leaves every node with a different
# third.  Each then runs B4's reconcile - "does anyone still have this?"
# - and each peer answers no, truthfully, because it never persisted
# that copy and has not yet been re-sent it.  The reconcile probes at 64
# keys a second from about a second after READY; the repair sweep that
# would re-send them runs every 10 s.  The reconcile wins the race and
# the delete is TOMBSTONED, so it survives every later restart.
#
# THE FIX (S223): the pass needs a WITNESS - a live peer that booted
# before this node died, and so saw anything deleted meanwhile.  Every
# peer's boot time comes from the uptime in its ALIVE; this node's death
# is the newest write in its storage directory or its alive stamp.  A
# fleet that came back together has no witness, and the pass is skipped.
# (The filed design - a purpose on the probe, "not authoritative"
# answers, re-share before asking - was built and measured wrong three
# ways, the last being that re-sharing first RESURRECTS a deleted key.)
#
# Asserted, over the three scenarios of the filing (scaled to 300 keys,
# 100 per node - the mechanism does not need volume):
#   1. killed together, restarted together, NO snapshot: every key still
#      there, on every node, and nothing reconcile-dropped;
#   2. killed together, restarted ONE BY ONE with a gap wide enough for
#      the measured loss (it happened inside 5 s): same;
#   3. the same outage WITH `save = 5 1`: same - the configuration that
#      already worked must keep working;
#   4. a STAGGERED failure: node 1 dies and reboots while 2 and 3 are
#      still up, and they die ten seconds later.  Node 1 booted before
#      they died but had not been re-sent their records: the 30 s grace
#      must refuse it as a witness.  Node 1's OWN pass, witnessed by 2 and
#      3 while they were up and complete, must still run.
# In each, every node must report reconcile_skipped - the pass was
# armed, evaluated, and deliberately not run.  Without that, "nothing
# was dropped" could be a pass that never got as far as deciding.
#
# FAIL-FIRST, and it is a step of this run: the same scenarios against a
# build whose pass never asks for a witness - one sed, and it is the
# whole of S223 - which must LOSE keys.  Without it a green run here
# says only that the fleet was healthy.
# Usage: test/outagetest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcog.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

MC=239.255.77.83
MP=17183
PER=100                                 # keys each node authors
TOT=$((PER * 3))

# ---- the pre-S223 build, for the fail-first -------------------------
echo "--- building the pre-S223 variant for the fail-first"
mkdir -p "$D/t"
tar -cf - --exclude=./.git --exclude='*.o' --exclude='*.a' . 2>/dev/null |
	(cd "$D/t" && tar -xf -) || { echo "cannot copy the tree"; exit 1; }
# ONE line: with the grace hugely negative, every peer this node has heard
# from counts as a witness, so the pass always runs - which is what the
# daemon did before S223.  The constant rather than the call site, so the
# variant still compiles cleanly (nothing goes unused under -Werror) and
# the per-tick and drop-time re-checks are disabled by the same edit.
sed -i 's/^#define WITNESS_GRACE_MS   (3LL \* REPL_SWEEP_BEATS \* 1000)$/#define WITNESS_GRACE_MS   (-(1LL << 60))/' \
	"$D/t/src/cluster.c"
grep -q "define WITNESS_GRACE_MS   (-(1LL << 60))" "$D/t/src/cluster.c" ||
	{ echo "the sed missed - the grace is still in force"; exit 1; }
make -C "$D/t" -j"$(nproc 2>/dev/null || echo 4)" perfcached > "$D/build.log" 2>&1 || {
	echo "the pre-S223 variant did not build:"; tail -5 "$D/build.log"; exit 1; }
cp "$D/t/perfcached" "$D/perfcached.pre"

# ---- harness --------------------------------------------------------
conf() { # conf <n> <save-line>
	mkdir -p "$D/wal$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = og-client-secret
cluster = og-cluster-secret
[listen]
tcp = 127.0.0.1:1735$1
plaintext = loopback
[cluster]
multicast = $MC:$MP
advertise = 127.0.11.$1
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
save = $2
EOF
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> <binary>
	"$2" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0
	while [ $i -lt 150 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"
	return 1
}
killall3() {
	for n in 1 2 3; do
		[ -f "$D/n$n.pid" ] && kill -9 "$(cat "$D/n$n.pid")" 2>/dev/null
		rm -f "$D/n$n.pid"
	done
	sleep 1
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
fill() { # fill <port> <prefix> <count>
	timeout 60 python3 -c '
import json, socket, sys
port, pre, n = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])
f = socket.create_connection(("127.0.0.1", port), timeout=20).makefile("rwb")
for i in range(n):
    f.write((json.dumps({"jsonrpc":"2.0","id":i,"method":"set",
        "params":{"col":"c","key":"%s%03d" % (pre,i),"value":"v%s%03d" % (pre,i)}})+"\n").encode())
f.flush()
bad = 0
for _ in range(n):
    if not (json.loads(f.readline()).get("result") or {}).get("stored"): bad += 1
print(bad)' "$1" "$2" "$3"; }
present() { # present <port> <count-per-prefix>: how many of the 3*PER keys are there
	timeout 60 python3 -c '
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
    m = json.loads(f.readline())
    r = m.get("result") or {}
    if r.get("value") == "v" + keys[m.get("id", 0)]: good += 1
print(good)' "$1" "$2"; }
converged() { # converged <count> <seconds>: all three hold <count>
	i=0
	while [ $i -lt $(($2 * 2)) ]; do
		[ "$(entries 17351)" = "$1" ] && [ "$(entries 17352)" = "$1" ] &&
			[ "$(entries 17353)" = "$1" ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}
load_fleet() { # load_fleet <binary> <save>
	rm -rf "$D"/wal1 "$D"/wal2 "$D"/wal3 "$D"/n1.log "$D"/n2.log "$D"/n3.log
	conf 1 "$1"; conf 2 "$1"; conf 3 "$1"
	start 1 "$2" && start 2 "$2" && start 3 "$2" || return 1
	sleep 4
	fill 17351 a $PER > /dev/null
	fill 17352 b $PER > /dev/null
	fill 17353 c $PER > /dev/null
	converged $TOT 30
}
# settle: the reconcile probes at 64/s and the sweep ticks every 10 s, so
# give both room and then read the fleet
settle() { sleep 30; }

report() { # report <label>
	P1=$(present 17351 $PER); P2=$(present 17352 $PER); P3=$(present 17353 $PER)
	R1=$(cl 17351 reconciled); R2=$(cl 17352 reconciled); R3=$(cl 17353 reconciled)
	Q1=$(cl 17351 reconcile_probed); Q2=$(cl 17352 reconcile_probed)
	Q3=$(cl 17353 reconcile_probed)
	K1=$(cl 17351 reconcile_skipped); K2=$(cl 17352 reconcile_skipped)
	K3=$(cl 17353 reconcile_skipped)
	echo "    $1: keys $P1/$P2/$P3 of $TOT; dropped $R1/$R2/$R3; probed $Q1/$Q2/$Q3; skipped $K1/$K2/$K3"
}
lost() { # lost: 1 if any node is missing a key
	[ "$P1" = "$TOT" ] && [ "$P2" = "$TOT" ] && [ "$P3" = "$TOT" ] && return 1
	return 0
}
skipped_everywhere() {
	[ "$K1" = True ] && [ "$K2" = True ] && [ "$K3" = True ]
}

# ---- scenario runners ------------------------------------------------
sc_together() { # <binary> <save>
	load_fleet "$2" "$1" || { echo "    fleet did not load"; return 1; }
	killall3
	start 1 "$1" && start 2 "$1" && start 3 "$1" || return 1
	settle
}
sc_onebyone() { # <binary> <save>
	load_fleet "$2" "$1" || { echo "    fleet did not load"; return 1; }
	killall3
	start 1 "$1" || return 1
	sleep 12                          # the measured loss happened in 5
	start 2 "$1" || return 1
	sleep 12
	start 3 "$1" || return 1
	settle
}

sc_staggered() { # <binary>: node 1 dies and reboots while 2 and 3 are
	# still up; 2 and 3 die ten seconds later and come back one by one
	load_fleet off "$1" || { echo "    fleet did not load"; return 1; }
	sleep 46                          # 2 and 3 are up past the grace:
	                                  # they can witness for node 1.
	# 46, not 35: node 1's death estimate is its last `alive` stamp,
	# touched every 10 beats, so it can read up to 10 s early and the
	# grace a survivor must clear is up to 40 s.  35 passed on a quiet
	# box and failed rc33's check-asan on GitHub (witness 0) - the same
	# trap sweepclocktest and tombstonetest fell into on rc30.
	kill -9 "$(cat "$D/n1.pid")" 2>/dev/null; rm -f "$D/n1.pid"; sleep 1
	start 1 "$1" || return 1          # node 1 back among complete peers
	sleep 10                          # up 10 s: LESS than the grace
	for n in 2 3; do
		kill -9 "$(cat "$D/n$n.pid")" 2>/dev/null; rm -f "$D/n$n.pid"
	done
	sleep 1
	start 2 "$1" || return 1
	sleep 5
	start 3 "$1" || return 1
	settle
}

# ---- 1-3. the three scenarios, on the build under test --------------
echo "--- 1. killed together, restarted together, save = off"
sc_together "$BIN" off; report "together"
lost && bad "keys lost: $P1/$P2/$P3 of $TOT (dropped $R1/$R2/$R3)" \
	|| ok "every one of the $TOT keys is on all three nodes; $R1/$R2/$R3 reconcile-dropped"
skipped_everywhere && ok "and every node decided it: reconcile_skipped on all three, $Q1/$Q2/$Q3 probes - no peer was up while it was down, so none was asked" \
	|| bad "not every node skipped the pass (skipped $K1/$K2/$K3, probed $Q1/$Q2/$Q3)"
killall3

echo "--- 2. killed together, restarted one by one, save = off"
sc_onebyone "$BIN" off; report "one-by-one"
lost && bad "keys lost: $P1/$P2/$P3 of $TOT (dropped $R1/$R2/$R3)" \
	|| ok "node 2 joined node 1 with its own durable third and KEPT it - $TOT on all three"
skipped_everywhere && ok "and every node skipped the pass - each earlier node booted AFTER the later ones died, so none of them was a witness" \
	|| bad "not every node skipped the pass (skipped $K1/$K2/$K3, probed $Q1/$Q2/$Q3)"
killall3

echo "--- 3. the same outage with save = 5 1 (the case that already worked)"
sc_together "$BIN" "5 1"; report "snapshots"
lost && bad "keys lost WITH snapshots: $P1/$P2/$P3 of $TOT" \
	|| ok "snapshots still recover everything - the fix did not break the working configuration"
killall3

echo "--- 4. a STAGGERED failure: node 1 dies and reboots, then 2 and 3 go"
sc_staggered "$BIN"; report "staggered"
W1=$(cl 17351 reconcile_witness)
lost && bad "keys lost: $P1/$P2/$P3 of $TOT (dropped $R1/$R2/$R3)" \
	|| ok "all $TOT on all three - node 1 booted before 2 and 3 died, but had not been re-sent their records, and was not taken for a witness"
[ "$K2" = True ] && [ "$K3" = True ] \
	&& ok "nodes 2 and 3 skipped their passes (node 1 had been up 10 s, under the 30 s grace)" \
	|| bad "nodes 2 and 3 did not both skip (skipped $K1/$K2/$K3) - the early riser was taken for a witness"
case "$W1" in ''|0|'?') bad "node 1 found no witness - 2 and 3 were up 46 s before it died (witness $W1)";;
	*) ok "and node 1 DID run its pass, witnessed by node $W1 - which was up throughout, and complete";; esac
killall3

# ---- 5. the fail-first: the same runs without S223 -------------------
echo "--- 5. fail-first: the same two scenarios on a build with S223 compiled out"
sc_together "$D/perfcached.pre" off; report "pre-S223 together"
lost && ok "it loses keys: $P1/$P2/$P3 of $TOT, $R1/$R2/$R3 reconcile-dropped - which is the defect, and the proof that the three runs above were the fix acting" \
	|| bad "the pre-S223 build lost nothing either ($P1/$P2/$P3): this suite cannot fail, so it proves nothing"
killall3
sc_onebyone "$D/perfcached.pre" off; report "pre-S223 one-by-one"
lost && ok "and one by one it loses them too: $P1/$P2/$P3 of $TOT, $R1/$R2/$R3 dropped" \
	|| bad "the pre-S223 build kept everything one-by-one ($P1/$P2/$P3)"
killall3

echo "outagetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
