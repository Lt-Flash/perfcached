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
# rc21 mapping probe_op 4 to SET, so a forwarded DELETE became a write).
#
# Three variant builds come off the tree under test, each differing from
# it in exactly one place:
#   ver - PC_VERSION only.  A different release, same contract.
#   ep  - PC_CL_EPOCH only.  Same release, another contract.
#   old - S221 removed: the field taken off both writers, the two lengths
#         shortened, and the three door checks short-circuited.  This is
#         what the fleet is running right now, and what it emits during
#         the upgrade that introduces the check.
#
# Asserted:
#  1. the builds differ where intended and nowhere else - each reports
#     its own version, and stats.cluster.wire_epoch reads 1, 1, 2, 1;
#  2. SAME EPOCH, DIFFERENT VERSION: they join, they replicate, and they
#     survive a reshard - a third node lifts the map term and every key
#     written before it still reads back afterwards;
#  3. DIFFERENT EPOCH: the join is refused at BOTH doors, each message
#     names both numbers, epoch_refused counts it, the refused node never
#     enters anyone's membership, and neither node is harmed - the fleet
#     keeps its keys and takes new writes, and the refused node still
#     serves its own clients;
#  4. the GRANDFATHER rule: a pre-S221 build carries no epoch, reads as
#     0, and is accepted while we are at epoch 1 - with the NOTICE that
#     says the next epoch will not accept it.
# Fail-first, and it is part of the run (step 5): the SAME epoch-2
# datagram that step 3 refuses is ACCEPTED by the old build, which has
# no check - so the refusal in step 3 is the check acting, not two
# daemons failing to find each other.
# Usage: test/epochtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
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
# ANCHORED on the exact text: if the source moves, the build still
# succeeds and step 1 catches the variant that did not take.
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
build ver
cp src/version.h "$D/t/src/version.h"
sed -i 's/^#define PC_CL_EPOCH 1$/#define PC_CL_EPOCH 2/' "$D/t/src/cluster.h"
build ep
cp src/cluster.h "$D/t/src/cluster.h"
sed -i '/pc_w8(&c, (unsigned)in->epoch);/d; s/buf\[len - 2\]/buf[len - 1]/' \
	"$D/t/src/clmemb.c"
# and the READING side: a build that predates the field does not check it
# either, which is the whole of step 5
sed -i 's/if (!epoch_ok(/if (0 \&\& !epoch_ok(/' "$D/t/src/cluster.c"
sed -i 's/^#define CLMEMB_ALIVE_LEN     (92 + 56 \* CLMEMB_COLS_MAX + 12)/#define CLMEMB_ALIVE_LEN     (92 + 56 * CLMEMB_COLS_MAX + 11)/;
	s/^#define CLMEMB_JOINREQ_LEN   43$/#define CLMEMB_JOINREQ_LEN   42/' \
	"$D/t/src/clmemb.h"
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
import json, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=8)
r = json.loads(sys.stdin.read()); r["id"] = 1; r["jsonrpc"] = "2.0"
s.sendall(json.dumps(r).encode() + b"\n")
print(s.makefile("rb").readline().decode().strip())' "$1"; }
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
import json, socket, sys
n = int(sys.argv[2])
f = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=20).makefile("rwb")
for i in range(n):
    f.write((json.dumps({"jsonrpc":"2.0","id":i,"method":"set",
        "params":{"col":"c","key":"k%04d" % i,"value":"v%04d" % i}})+"\n").encode())
f.flush()
bad = 0
for _ in range(n):
    r = json.loads(f.readline())
    if not (r.get("result") or {}).get("stored"): bad += 1
print(bad)' "$1" "$2"; }
readback() { timeout 60 python3 -c '
import json, socket, sys
n = int(sys.argv[2])
f = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=20).makefile("rwb")
for i in range(n):
    f.write((json.dumps({"jsonrpc":"2.0","id":i,"method":"get",
        "params":{"col":"c","key":"k%04d" % i}})+"\n").encode())
f.flush()
good = 0
for _ in range(n):
    m = json.loads(f.readline())
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
[ "$E_STOCK" = 1 ] && [ "$E_ver" = 1 ] && [ "$E_ep" = 2 ] && [ "$E_old" = 1 ] \
	&& ok "wire_epoch reads 1/1/2/1 across stock, ver, ep, old - the epoch sed took, and only on the ep build" \
	|| bad "wire_epoch: stock $E_STOCK, ver $E_ver, ep $E_ep, old $E_old (want 1/1/2/1)"
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
echo "$M1" | grep -q "epoch 2 and we speak 1" \
	&& ok "A: $(echo "$M1" | cut -c1-78)..." \
	|| bad "A's message does not name both numbers: ${M1:-none}"
echo "$M4" | grep -q "epoch 1 and we speak 2" \
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

# ---- 4. the grandfather, and 5. the fail-first ----------------------
echo "--- the grandfather: a pre-S221 build carries no epoch"
rm -f "$D/n1.log" "$D/n4.log"
start 1 "$BIN"              || bad "A did not restart"
start 2 "$D/perfcached.old" || bad "OLD did not start"
if await 17381 peers_up 1 20 && await 17382 peers_up 1 20; then
	ok "a build that predates the field joined a build that has it"
else
	bad "the grandfather was refused (A $(cl 17381 peers_up), OLD $(cl 17382 peers_up))"
fi
grep -q "REFUSING" "$D/n1.log" && bad "A refused the pre-S221 build" || ok "A refused nothing"
i=0
while [ $i -lt 40 ]; do
	grep -q "speaks no wire epoch" "$D/n1.log" && break
	sleep 0.5; i=$((i+1))
done
grep -q "The NEXT epoch will refuse it" "$D/n1.log" \
	&& ok "A said so, and said what happens next: $(grep -m1 -o "The NEXT epoch will refuse it[^.]*." "$D/n1.log")" \
	|| bad "A accepted it silently - an operator would never learn the fleet is mid-upgrade"
echo "--- fail-first: the same epoch-2 node, offered to a build with no check"
start 4 "$D/perfcached.ep" || bad "X did not restart"
if await 17382 peers_up 2 25; then
	ok "the pre-S221 build TOOK the epoch-2 node into its membership - which is the bug S221 closes, and the proof that step 3's refusal was the check acting"
else
	bad "the pre-S221 build did not take it either (OLD peers_up $(cl 17382 peers_up)) - step 3 proves nothing"
fi
[ "$(cl 17381 peers_up)" = 1 ] \
	&& ok "while A, beside it on the same wire, still refused it" \
	|| bad "A's membership moved to $(cl 17381 peers_up)"

echo "epochtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
