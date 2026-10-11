#!/bin/sh
# epochtest.sh - S221: the cluster wire epoch, tested with TWO BUILDS.
#
# Every other cluster suite runs one binary against itself, so nothing
# has ever asked the question this one asks: what happens when the
# daemons on the wire are not the same program?  The answer must depend
# on ONE number and nothing else.  PC_CL_EPOCH names the contract the
# cluster datagrams belong to; PC_VERSION names the release.  A fleet
# may mix releases all day, and must never mix epochs - the epoch is
# bumped only when a frame means something an older build would
# MIS-EXECUTE rather than ignore (the audit that set the baseline found
# rc21 mapping probe_op 4 to SET, so a forwarded DELETE became a write;
# S317 bumped it to 2 when the ALIVE frame lost its JSON-dialect client
# count and every field behind it moved four bytes).
#
# Three variant builds come off the tree under test, each differing from
# it in exactly one place:
#   ver - PC_VERSION only.  A different release, same contract.
#   ep  - PC_CL_EPOCH only.  Same release, another contract.
#   old - S221 removed: the epoch byte (and S314's features byte behind
#         it, which a stock reader would otherwise take FOR the epoch)
#         off the three writers, the three lengths shortened, the ALIVE
#         reason byte back at the end, and the three door checks
#         short-circuited.  A build that sends no epoch and checks none.
#
# Asserted:
#  1. the builds differ where intended and nowhere else - each reports
#     its own version, and stats.cluster.wire_epoch reads 2, 2, 3, 2
#     (the old build still NAMES epoch 2; it just never sends it);
#  2. SAME EPOCH, DIFFERENT VERSION: they join, they replicate, and they
#     survive a reshard - a third node lifts the map term and every key
#     written before it still reads back afterwards;
#  3. DIFFERENT EPOCH: the join is refused at BOTH doors, each message
#     names both numbers, epoch_refused counts it, the refused node never
#     enters anyone's membership, and neither node is harmed - the fleet
#     keeps its keys and takes new writes, and the refused node still
#     serves its own clients;
#  4. NO EPOCH: a pre-S221 build carries none, reads as 0, and is refused
#     like any other - the grandfather rule that admitted one at epoch 1
#     expired with the bump to 2, as it said it would.  The message names
#     0 and 2, no grandfather notice is logged, and it never enters A's
#     membership - while, checking nothing, it counts A from A's own
#     keepalives: the refusal is one-sided by design.
# Fail-first, and it is part of the run (step 5): the SAME epoch-3 node
# that step 3 refuses is TAKEN by the old build, which has no check - so
# the refusals in steps 3 and 4 are the check acting, not two daemons
# failing to find each other.
# Usage: test/epochtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pcep.XXXXXX)
PIDS=
trap 'for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

MC=239.255.77.79
MP=17179
N=200

# ---- the three variant builds ---------------------------------------
# One copy of the tree, three seds, three links.  The sed patterns are
# ANCHORED on the exact text, and every one is followed by a grep for
# what it should have produced: if the source moves, the suite stops
# here saying which sed missed, rather than building a variant that is
# the stock daemon under another name (the old build's seds were silent
# no-ops from S314 until S317, and step 1 cannot see that).
echo "--- building the variants"
mkdir -p "$D/t"
tar -cf - --exclude=./.git --exclude='*.o' --exclude='*.a' . 2>/dev/null |
	(cd "$D/t" && tar -xf -) || { echo "cannot copy the tree"; exit 1; }
build() { # build <name>
	make -C "$D/t" -j"$(nproc 2>/dev/null || echo 4)" perfcached \
		> "$D/build.$1.log" 2>&1 || {
		echo "the $1 variant did not build:"; tail -5 "$D/build.$1.log"
		echo "epochtest: $pass passed, $((fail+1)) failed"; exit 1; }
	cp "$D/t/perfcached" "$D/perfcached.$1"
}
sed -i 's/^#define PC_VERSION "\(.*\)"$/#define PC_VERSION "\1+epochtest"/' \
	"$D/t/src/version.h"
grep -q '+epochtest"' "$D/t/src/version.h" ||
	{ echo "the version sed missed"; exit 1; }
build ver
cp src/version.h "$D/t/src/version.h"
sed -i 's/^#define PC_CL_EPOCH 2$/#define PC_CL_EPOCH 3/' "$D/t/src/cluster.h"
grep -q '^#define PC_CL_EPOCH 3$' "$D/t/src/cluster.h" ||
	{ echo "the epoch sed missed"; exit 1; }
build ep
cp src/cluster.h "$D/t/src/cluster.h"
# the writers: the epoch byte goes off ALIVE, JOIN_REQ and MASTER_ALIVE,
# and so does everything appended BEHIND it on ALIVE - S314's features
# byte and 0.5.7.1's sole_witness (2) - which a pre-S221 build cannot
# have, and which a stock reader would otherwise take for the epoch.  The
# reason byte is then the last byte of ALIVE again, so the patch by
# offset-from-the-end and the shortest-frame guard move with it.
sed -i '/pc_w8(&c, (unsigned)in->epoch);/d; /pc_w8(&c, (unsigned)in->features);/d;
	/pc_w16(&c, (uint16_t)in->sole_witness);/d;
	s/buf\[len - 5\]/buf[len - 1]/; s/if (len < 88 + 15)/if (len < 88 + 11)/' \
	"$D/t/src/clmemb.c"
grep -qE 'in->(epoch|features|sole_witness)\);' "$D/t/src/clmemb.c" &&
	{ echo "the clmemb.c sed missed - a writer still sends the field"; exit 1; }
grep -q 'buf\[len - 1\]' "$D/t/src/clmemb.c" && grep -q 'len < 88 + 11' "$D/t/src/clmemb.c" ||
	{ echo "the clmemb.c sed missed - the reason byte did not move"; exit 1; }
# and the READING side: a build that predates the field does not check it
# either, at any of the three doors - which is the whole of step 5
sed -i 's/if (!epoch_ok(/if (0 \&\& !epoch_ok(/' "$D/t/src/cluster.c"
[ "$(grep -c 'if (0 && !epoch_ok(' "$D/t/src/cluster.c")" = 3 ] ||
	{ echo "the cluster.c sed missed - $(grep -c 'if (0 && !epoch_ok(' "$D/t/src/cluster.c") of 3 door checks short-circuited"; exit 1; }
sed -i 's/^#define CLMEMB_ALIVE_LEN     (88 + 56 \* CLMEMB_COLS_MAX + 15)/#define CLMEMB_ALIVE_LEN     (88 + 56 * CLMEMB_COLS_MAX + 11)/;
	s/^\(#define CLMEMB_JOINREQ_LEN *\)43$/\142/; s/^\(#define CLMEMB_MALIVE_LEN *\)40$/\139/' \
	"$D/t/src/clmemb.h"
grep -q 'CLMEMB_COLS_MAX + 11)' "$D/t/src/clmemb.h" && grep -q 'CLMEMB_JOINREQ_LEN *42$' "$D/t/src/clmemb.h" &&
	grep -q 'CLMEMB_MALIVE_LEN *39$' "$D/t/src/clmemb.h" ||
	{ echo "the clmemb.h sed missed - a length did not shorten"; exit 1; }
build old

# ---- the fleet harness ----------------------------------------------
conf() { # conf <n>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = ep-client-secret
cluster = ep-cluster-secret
[listen]
tcp = 127.0.0.1:1738$1
plaintext = loopback
[cluster]
multicast = $MC:$MP
advertise = 127.0.9.$1
pull_timeout_ms = 300
mode = shard
collections = c
[collection c]
buckets_log2 = 12
EOF
	chmod 600 "$D/n$1.conf"
}
start() { # start <n> <binary>; the pid goes in PIDS and in $D/n<n>.pid
	"$2" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"; PIDS="$PIDS $!"
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"
	return 1
}
stop() { # stop <n>
	[ -f "$D/n$1.pid" ] || return 0
	kill -9 "$(cat "$D/n$1.pid")" 2>/dev/null
	rm -f "$D/n$1.pid"; sleep 0.3
}
call() { printf '%s\n' "$2" | timeout 10 python3 -c '
import json, socket, sys, pcnative
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=8)
r = json.loads(sys.stdin.read()); r["id"] = 1; r["jsonrpc"] = "2.0"
f = pcnative.wrap(s)
f.write(json.dumps(r).encode() + b"\n"); f.flush()
print(f.readline().decode().strip())' "$1"; }
cl() { # cl <port> <key>: one field of stats.cluster, "?" when absent
	call "$1" '{"method":"stats"}' | python3 -c \
	'import json,sys
try: print((json.load(sys.stdin)["result"]["cluster"]).get(sys.argv[1], "?"))
except Exception: print("?")' "$2" 2>/dev/null || echo "?"; }
term() { call "$1" '{"method":"stats"}' | python3 -c \
	'import json,sys
try: print(((json.load(sys.stdin)["result"]["cluster"]).get("map") or {}).get("term",-1))
except Exception: print(-1)' 2>/dev/null || echo -1; }
ver() { call "$1" '{"method":"stats"}' | python3 -c \
	'import json,sys
try: print(json.load(sys.stdin)["result"].get("version","?"))
except Exception: print("?")' 2>/dev/null || echo "?"; }
await() { # await <port> <field> <value> <seconds>
	i=0
	while [ $i -lt $(($4 * 4)) ]; do
		[ "$(cl "$1" "$2")" = "$3" ] && return 0
		sleep 0.25; i=$((i+1))
	done
	return 1
}
# THE RESPONSES ARE NOT IN ORDER.  A shard node answers the keys it owns
# immediately and the ones it forwards when the owner replies, so a driver
# that pairs the Nth reply with the Nth request scores about 2% and looks
# like data loss.  Both drivers key on the id.
fill() { timeout 60 python3 -c '
import json, socket, sys, pcnative
n = int(sys.argv[2])
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=20))
for i in range(n):
    f.write((json.dumps({"jsonrpc":"2.0","id":i,"method":"set",
        "params":{"col":"c","key":"k%04d" % i,"value":"v%04d" % i}})+"\n").encode())
f.flush()
bad = 0
for _ in range(n):
    r = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if not (isinstance(_m, dict) and "notify" in _m))
    if not (r.get("result") or {}).get("stored"): bad += 1
print(bad)' "$1" "$2"; }
readback() { timeout 60 python3 -c '
import json, socket, sys, pcnative
n = int(sys.argv[2])
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=20))
for i in range(n):
    f.write((json.dumps({"jsonrpc":"2.0","id":i,"method":"get",
        "params":{"col":"c","key":"k%04d" % i}})+"\n").encode())
f.flush()
good = 0
for _ in range(n):
    m = next(_m for _m in iter(lambda: json.loads(f.readline()), None) if not (isinstance(_m, dict) and "notify" in _m))
    r = m.get("result") or {}
    if r.get("value") == "v%04d" % m.get("id", -1): good += 1
print(good)' "$1" "$2"; }

# ---- 1. the builds differ where intended, and nowhere else ----------
echo "--- the variants report themselves"
conf 1; conf 2; conf 3; conf 4
start 1 "$BIN" || { bad "the tree's own build did not start"; \
	echo "epochtest: $pass passed, $fail failed"; exit 1; }
V_STOCK=$(ver 17381); E_STOCK=$(cl 17381 wire_epoch)
stop 1
for v in ver ep old; do
	start 1 "$D/perfcached.$v" || bad "the $v variant did not start"
	eval "V_$v=\$(ver 17381)"; eval "E_$v=\$(cl 17381 wire_epoch)"
	stop 1
done
[ "$E_STOCK" = 2 ] && [ "$E_ver" = 2 ] && [ "$E_ep" = 3 ] && [ "$E_old" = 2 ] \
	&& ok "wire_epoch reads 2/2/3/2 across stock, ver, ep, old - the epoch sed took, and only on the ep build (old names 2 and sends nothing)" \
	|| bad "wire_epoch: stock $E_STOCK, ver $E_ver, ep $E_ep, old $E_old (want 2/2/3/2)"
[ "$V_ver" != "$V_STOCK" ] && [ "$V_ep" = "$V_STOCK" ] && [ "$V_old" = "$V_STOCK" ] \
	&& ok "and the version differs on the ver build alone ($V_STOCK vs $V_ver) - the two numbers move independently" \
	|| bad "versions: stock $V_STOCK, ver $V_ver, ep $V_ep, old $V_old"

# ---- 2. same epoch, different release: join, replicate, reshard -----
echo "--- same epoch, different PC_VERSION: two releases in one cluster"
start 1 "$BIN"      || bad "A did not start"
start 2 "$D/perfcached.ver" || bad "B did not start"
if await 17381 peers_up 1 20 && await 17382 peers_up 1 20; then
	ok "$V_STOCK and $V_ver federated - the release number is not a barrier"
else
	bad "no federation: A peers_up $(cl 17381 peers_up), B peers_up $(cl 17382 peers_up)"
fi
grep -q "REFUSING" "$D/n1.log" "$D/n2.log" && bad "one of them refused the other" \
	|| ok "neither refused the other"
[ "$(fill 17381 $N)" = 0 ] && ok "$N keys written through A (shard mode: A owns some, forwards the rest to B)" \
	|| bad "the fill did not store cleanly"
G=$(readback 17382 $N)
[ "$G" = $N ] && ok "all $N read back through B, the other build" \
	|| bad "B returned $G of $N"
T0=$(term 17381)
echo "--- and a reshard across the two builds"
start 3 "$BIN" || bad "C did not start"
if await 17381 peers_up 2 25 && await 17382 peers_up 2 25; then
	ok "C joined: three nodes, two builds"
else
	bad "C did not join (A $(cl 17381 peers_up), B $(cl 17382 peers_up))"
fi
i=0; T1=$T0
while [ $i -lt 60 ]; do
	T1=$(term 17381); [ "$T1" -gt "$T0" ] 2>/dev/null && break
	sleep 0.5; i=$((i+1))
done
[ "$T1" -gt "$T0" ] 2>/dev/null && ok "the map term lifted $T0 -> $T1: the slots moved" \
	|| bad "the map term never lifted (still $T1)"
sleep 3
G=$(readback 17382 $N)
[ "$G" = $N ] && ok "all $N still read back through B after the reshard - keys crossed a build boundary and survived" \
	|| bad "after the reshard B returned $G of $N"

# ---- 3. different epoch: refused at both doors, nobody harmed -------
echo "--- different epoch: the refusal"
R_BEFORE=$(cl 17381 epoch_refused)
start 4 "$D/perfcached.ep" || bad "X did not start"
i=0
while [ $i -lt 40 ]; do
	grep -q "REFUSING" "$D/n1.log" && grep -q "REFUSING" "$D/n4.log" && break
	sleep 0.5; i=$((i+1))
done
M1=$(grep -m1 -o "REFUSING.*" "$D/n1.log")
M4=$(grep -m1 -o "REFUSING.*" "$D/n4.log")
echo "$M1" | grep -q "epoch 3 and we speak 2" \
	&& ok "A: $(echo "$M1" | cut -c1-78)..." \
	|| bad "A's message does not name both numbers: ${M1:-none}"
echo "$M4" | grep -q "epoch 2 and we speak 3" \
	&& ok "X: $(echo "$M4" | cut -c1-78)..." \
	|| bad "X's message does not name both numbers: ${M4:-none}"
R_AFTER=$(cl 17381 epoch_refused); RX=$(cl 17384 epoch_refused)
[ "$R_AFTER" -gt "$R_BEFORE" ] 2>/dev/null && [ "$RX" -gt 0 ] 2>/dev/null \
	&& ok "epoch_refused counts it at both ends (A $R_BEFORE -> $R_AFTER, X $RX)" \
	|| bad "epoch_refused: A $R_BEFORE -> $R_AFTER, X $RX"
sleep 3
[ "$(cl 17381 peers_up)" = 2 ] && [ "$(cl 17384 peers_up)" = 0 ] \
	&& ok "X entered nobody's membership and admitted nobody: A still has its 2 peers, X has 0" \
	|| bad "membership moved: A $(cl 17381 peers_up), X $(cl 17384 peers_up)"
G=$(readback 17382 $N)
R=$(call 17381 '{"method":"set","params":{"col":"c","key":"after","value":"yes"}}')
echo "$R" | grep -q '"stored":[ ]*true' && [ "$G" = $N ] \
	&& ok "and the fleet is unharmed: all $N keys still read, and it takes a new write during the refusals" \
	|| bad "the fleet was harmed: $G of $N read, new write said $R"
R=$(call 17384 '{"method":"set","params":{"col":"c","key":"own","value":"yes"}}')
echo "$R" | grep -q '"stored":[ ]*true' \
	&& ok "and X is unharmed too - refused from the cluster, still serving its own clients" \
	|| bad "X stopped serving: $R"
stop 4; stop 3; stop 2; stop 1

# ---- 4. no epoch on the wire, and 5. the fail-first ------------------
echo "--- no epoch: a pre-S221 build is refused (the grandfather rule expired with epoch 2)"
rm -f "$D/n1.log" "$D/n2.log" "$D/n4.log"
start 1 "$BIN"              || bad "A did not restart"
R_BEFORE=$(cl 17381 epoch_refused)
start 2 "$D/perfcached.old" || bad "OLD did not start"
i=0
while [ $i -lt 40 ]; do
	grep -q "REFUSING" "$D/n1.log" && break
	sleep 0.5; i=$((i+1))
done
M1=$(grep -m1 -o "REFUSING.*" "$D/n1.log")
echo "$M1" | grep -q "speaks cluster wire epoch 0 and we speak 2" \
	&& ok "A: $(echo "$M1" | cut -c1-78)..." \
	|| bad "A did not refuse the epoch-less build by both numbers: ${M1:-none}"
grep -q "speaks no wire epoch\|NEXT epoch will refuse" "$D/n1.log" \
	&& bad "A still carries the grandfather notice - the rule did not expire with the bump" \
	|| ok "and no grandfather notice: an absent epoch is 0, refused like any other"
# the old build checks nothing, so ITS view is one-sided: it counts A
# from A's keepalives while A refuses it at every door.  Asserted here so
# that step 5 can tell "took the epoch-3 node" from "never heard anyone".
if await 17382 peers_up 1 20; then
	ok "the old build, checking nothing, counted A as a peer from A's own keepalives"
else
	bad "the old build never heard A (OLD peers_up $(cl 17382 peers_up)) - step 5 could not tell a refusal from silence"
fi
sleep 3
R_AFTER=$(cl 17381 epoch_refused)
[ "$(cl 17381 peers_up)" = 0 ] && [ "$R_AFTER" -gt "$R_BEFORE" ] 2>/dev/null \
	&& ok "while A admitted nothing: 0 peers after the join window, epoch_refused $R_BEFORE -> $R_AFTER" \
	|| bad "A took the epoch-less build, or did not count the refusal: peers_up $(cl 17381 peers_up), epoch_refused $R_BEFORE -> $R_AFTER"
echo "--- fail-first: the same epoch-3 node, offered to a build with no check"
start 4 "$D/perfcached.ep" || bad "X did not restart"
if await 17382 peers_up 2 25; then
	ok "the old build TOOK the epoch-3 node into its peer table beside A (peers_up 2) - which is the bug S221 closes, and the proof that steps 3 and 4 refused on the check, not on silence"
else
	bad "the old build did not take it either (OLD peers_up $(cl 17382 peers_up)) - steps 3 and 4 prove nothing"
fi
# A's side is the claim: it refused the old build (no epoch) and X (epoch
# 3) alike, so it sits alone.  X's table is not asserted - X asked to join
# and the no-check master ASSIGNED it (the assignment is not one of the
# three epoch doors), so X holds whatever member list that master handed
# it while refusing their keepalives; that is the old build's bug at work,
# not X's.
[ "$(cl 17381 peers_up)" = 0 ] \
	&& ok "while A, beside it on the same wire, still refused everything: 0 peers (X holds $(cl 17384 peers_up), handed to it by the build with no check)" \
	|| bad "A's membership moved to $(cl 17381 peers_up)"

echo "epochtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
