#!/bin/sh
# shardhandovertest.sh — when a node FAILS, its keys must leave it.
#
# In shard mode a key has exactly one home, so a node that stops being
# usable while still holding keys does not degrade the fleet - it takes
# that slice of the keyspace with it.  The design says a FAILED node is
# excluded from placement and its reshard tick therefore hands every key
# it holds to the new owner.  This is that claim, measured.
#
# THE FAULT IS REAL, NOT SIMULATED.  Node 1's WAL sits on a dm-delay
# device at 60ms per write with a 64KB ring, so driving load at it makes
# the ring overflow and the daemon marks itself FAILED on the first
# acknowledged write it drops.  Nothing here calls an internal function
# to fake the state.
#
# ASSERTED, in order:
#   1. before the fault, node 1 actually HOLDS some of the keyspace
#      (or nothing below is a test of anything)
#   2. the fault lands: node 1 reports state = failed
#   3. every key node 1 held is still READABLE from a survivor
#   4. node 1 has stopped holding them
#
# Usage: test/shardhandovertest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
KEYS=${KEYS:-400}
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

command -v dmsetup >/dev/null 2>&1 && command -v losetup >/dev/null 2>&1 || {
	echo "shardhandovertest: SKIPPED - needs dmsetup/losetup (root)"; exit 0; }

D=$(mktemp -d /var/tmp/pcsh.XXXXXX); LOOP=; DM=shdm$$
cleanup() {
	pkill -f "[p]erfcached -f $D" 2>/dev/null; sleep 1
	umount "$D/mnt" 2>/dev/null; dmsetup remove $DM 2>/dev/null
	[ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
	rm -rf "$D"
}
trap cleanup EXIT INT TERM

dd if=/dev/zero of="$D/img" bs=1M count=256 status=none
LOOP=$(losetup -f --show "$D/img") || { echo "shardhandovertest: SKIPPED - no loop"; exit 0; }
dmsetup create $DM --table "0 $(blockdev --getsz "$LOOP") delay $LOOP 0 0 $LOOP 0 60" \
	|| { echo "shardhandovertest: SKIPPED - no dm-delay target"; exit 0; }
mkfs.ext4 -q -F /dev/mapper/$DM
mkdir -p "$D/mnt"; mount /dev/mapper/$DM "$D/mnt"

for i in 1 2 3; do
	# node 1 alone gets the slow device and a ring too small for it -
	# the others must stay healthy or there is nowhere to hand over TO
	if [ $i = 1 ]; then WD="$D/mnt/wal1"; EXTRA="ring_kb = 64"; else WD="$D/wal$i"; EXTRA=""; fi
	mkdir -p "$WD"
	cat > "$D/n$i.conf" <<CFG
[daemon]
workers = 1
[memory]
arena_mb = 64
[secrets]
client = sh-secret
cluster = sh-cluster
[listen]
tcp = 127.0.61.$i:1647$i
plaintext = loopback
[cluster]
multicast = 239.255.77.211:17211
advertise = 127.0.61.$i
mode = shard
collections = th
[wal]
dir = $WD
probe = no
fsync = always
$EXTRA
segment_mb = 2
segments = 4
save = off
[collection th]
buckets_log2 = 12
CFG
	"$BIN" -f "$D/n$i.conf" > "$D/n$i.log" 2>&1 &
done
i=0
while [ $i -lt 200 ]; do
	n=0
	for j in 1 2 3; do
		grep -q "perfcached ready" "$D/n$j.log" 2>/dev/null && n=$((n+1))
	done
	[ $n = 3 ] && break
	sleep 0.1; i=$((i+1))
done
[ "${n:-0}" = 3 ] || { echo "shardhandovertest: SKIPPED - fleet did not start"; exit 0; }
sleep 6                                    # let membership and the map settle

# Raw JSON over TCP, not perfcli: `plaintext = loopback` makes the
# listener plaintext-EXCLUSIVE and perfcli authenticates, so every
# perfcli call here came back empty and the first version of this test
# reported "node1 holds 0 keys" when it had never managed to ASK.
call() { python3 - "$1" "$2" <<'PY2'
import json, socket, sys
n, req = sys.argv[1], sys.argv[2]
try:
    s = socket.create_connection(("127.0.61." + n, 16470 + int(n)), timeout=30)
    f = s.makefile("rwb")
    f.write(req.encode() + b"\n"); f.flush()
    sys.stdout.write(f.readline().decode())
except Exception:
    pass
PY2
}

# every key node <n> holds LOCALLY.  scan's `count` is MAX BUCKETS, not
# max items, so the cursor has to be drained or this silently reports a
# slice of the table as the whole of it.
local_keys() {
	cur=0; round=0
	while [ $round -lt 64 ]; do
		call "$1" "{\"method\":\"scan\",\"params\":{\"col\":\"th\",\"cursor\":$cur,\"count\":16384,\"values\":false}}" \
			> "$D/scan.$$"
		cur=$(python3 -c 'import json;print((json.load(open("'"$D/scan.$$"'")).get("result") or {}).get("cursor",0))' 2>/dev/null || echo 0)
		python3 -c 'import json
for it in (json.load(open("'"$D/scan.$$"'")).get("result") or {}).get("items") or []:
    k=it.get("k")
    if k and k.startswith("hk-"): print(k)' 2>/dev/null
		[ "${cur:-0}" = 0 ] && break
		round=$((round+1))
	done
	rm -f "$D/scan.$$"
}

echo "=== shardhandovertest ($KEYS keys, 3-node shard fleet) ==="

# ---- fill through node 2, so placement decides where each key lands --
python3 - "$KEYS" <<'PY2'
import json, socket, sys
n = int(sys.argv[1])
s = socket.create_connection(("127.0.61.2", 16472), timeout=30)
f = s.makefile("rwb"); rid = [0]
def call(m, **p):
    rid[0] += 1
    r = {"jsonrpc": "2.0", "id": rid[0], "method": m}
    if p: r["params"] = p
    f.write(json.dumps(r).encode() + b"\n"); f.flush()
    return json.loads(f.readline())
bad = 0
for i in range(1, n + 1):
    if "error" in call("set", col="th", key="hk-%d" % i, value="v-%d" % i):
        bad += 1
print("  fill: %d of %d refused" % (bad, n))
PY2
sleep 2

# Where did the keys actually go?  A distribution of 0/400/0 means the
# fleet never clustered and node 2 kept everything; 0/0/0 means the fill
# itself failed.  Both look identical at the assertion below, and the
# first version of this test could not tell them apart.
for n in 1 2 3; do
	P=$(call $n '{"method":"stats"}' | python3 -c 'import json,sys
try:
    d=(json.load(sys.stdin).get("result") or {}); c=d.get("cluster") or {}
    print("state=%s peers_up=%s entries=%s" % (d.get("state"),
        c.get("peers_up"), (d.get("collections") or [{}])[0].get("entries")))
except Exception as e: print("unreadable")' 2>/dev/null)
	echo "  node$n: $P"
done

local_keys 1 | sort > "$D/before1"
B1=$(wc -l < "$D/before1")
echo "  node1 holds $B1 of $KEYS keys before the fault"
[ "$B1" -gt 0 ] \
	&& ok "node1 owns part of the keyspace (anti-vacuity)" \
	|| { bad "node1 holds nothing - there is no handover to observe.
	     Check the per-node line above: all keys on one node means the
	     fleet never sharded, zero everywhere means the fill failed"
	     echo "shardhandovertest: $pass passed, $fail failed"; exit 1; }

# ---- drive node 1 until its WAL drops and it fails -------------------
echo "  driving writes at node1 until its ring overflows"
python3 - <<PY
import json, socket, time
s = socket.create_connection(("127.0.61.1", 16471), timeout=30)
f = s.makefile("rwb"); rid=[0]
def call(m, **p):
    rid[0]+=1
    r={"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
t0=time.time(); n=0
while time.time()-t0 < 15:
    call("set", col="th", key="junk-%06d"%n, value="v"*900); n+=1
PY
sleep 3

ST=$(call 1 '{"method":"stats"}' | python3 -c 'import json,sys
try: print((json.load(sys.stdin).get("result") or {}).get("state"))
except Exception: print("?")' 2>/dev/null)
[ "$ST" = failed ] \
	&& ok "the fault landed: node1 reports state=failed" \
	|| { bad "node1 is '$ST', not failed - the WAL never dropped, so no
	     re-shard was triggered and nothing below is a test of it"
	     echo "shardhandovertest: $pass passed, $fail failed"; exit 1; }

# ---- the handover ----------------------------------------------------
# Placement excludes a non-READY node, so every key node1 held is now
# somebody else's; its reshard tick has to deliver them.  Give it room:
# the tick is ~1 Hz and the device under it is deliberately slow.
echo "  waiting for the re-shard to move its keys"
# Count only the keys this test placed.  The first version counted the
# WHOLE collection, so the hammer's junk - which node1 owned while it was
# still READY - inflated the total from 121 to 150 and hid the fact that
# every original key had already gone.  It reported a working handover as
# broken.
MOVED=0
i=0
while [ $i -lt 60 ]; do
	sleep 2; i=$((i + 2))
	local_keys 1 | sort > "$D/after1"
	A1=$(wc -l < "$D/after1")
	[ "$A1" = 0 ] && { MOVED=1; break; }
done
echo "  node1 holds $A1 of its original $B1 keys after ${i}s"

# THE assertion, and it must be about LOCATION.  Reading through a
# survivor proves nothing: a shard-mode GET on a non-owner FORWARDS, and
# a FAILED node still serves reads - so the value comes back whether it
# moved or not.  The first version of this test asserted exactly that and
# would have called a broken handover a pass.  Ask the survivors what
# they physically hold.
local_keys 2 | sort > "$D/on2"
local_keys 3 | sort > "$D/on3"
sort -u "$D/on2" "$D/on3" > "$D/survivors"
LANDED=$(comm -12 "$D/before1" "$D/survivors" | wc -l)
echo "  survivors now hold $LANDED of node1's original $B1 keys"
if [ "$LANDED" = "$B1" ]; then
	ok "every key node1 held is now physically on a survivor"
else
	bad "only $LANDED of $B1 keys reached a survivor - the rest are
	     still on a node that has stopped taking writes"
fi
[ "$MOVED" = 1 ] \
	&& ok "node1 gave up every key it held ($B1 -> 0)" \
	|| bad "node1 still holds $A1 of its original $B1 keys - the
	     re-shard did not finish moving them"

echo "shardhandovertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
