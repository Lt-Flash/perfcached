#!/bin/sh
# proxytest.sh — M5 verification: proxy mode, the capacity plane.
#  - placement: an ingress node meaningfully fuller than its peers sends
#    new keys AWAY (power-of-two on free-MB + the self band); sum of
#    local entries across the fleet == keys written, ingress holds ~none;
#  - sticky forwarding: updates and counters route to the single holder
#    (counters come out EXACT - the conflict-model proof);
#  - reads: a non-holder serves from the cluster WITHOUT storing, and
#    the locator turns the second read into a unicast (loc_hits);
#  - delete via non-holder forwards; the key dies fleet-wide;
#  - birth race: 30 same-new-key racing writes through two nodes end
#    with EXACTLY ONE holder each (the keys verb is local-only - the
#    per-node holder census is exact);
#  - the rebalancer: a late-joining empty node RECEIVES migrated keys
#    within two 10s ticks, totals conserved, values intact - leveling
#    is PROPORTIONAL (utilization), so the donor keeps its fair share
#    (sm registers first so the small records migrate in tick one);
#  - sm runs on 16 buckets so ~3/4 of its 400 records live in OVERFLOW:
#    the donor scan MUST survive the overflow leg (the ovf_lock
#    self-deadlock regression - the peer thread froze mid-tick).
# Usage: test/proxytest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcpx.XXXXXX)
P1= P2= P3=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; \
     [ -n "$P3" ] && kill -9 $P3 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

node() { # node <n> <cliport> <arena>  (advertises 127.0.0.4<n>)
	id=$1; cli=$2; arena=$3
	cat > "$D/n$id.conf" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = $arena
[secrets]
client = px-client-secret
cluster = px-cluster-secret
[listen]
tcp = 127.0.0.1:$cli
plaintext = loopback
[cluster]
multicast = 239.255.77.41:17141
advertise = 127.0.0.4$id
pull_timeout_ms = 300
negative_ms = 300
tombstone_ms = 2000
[collection sm]
buckets_log2 = 4
mode = proxy
[collection px]
buckets_log2 = 12
mode = proxy
[collection st]
buckets_log2 = 10
EOF
}
start() { # start <id> <pidvar>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$2=\$!"
	i=0
	while [ $i -lt 60 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; cat "$D/n$1.log"; exit 1
}

CLI="$D/cli.py"
cat > "$CLI" <<'EOF'
import json, socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=6)
f = s.makefile("rwb"); rid = 0
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    rid += 1
    req = json.loads(line); req["id"] = rid; req["jsonrpc"] = "2.0"
    f.write(json.dumps(req).encode()+b"\n"); f.flush()
    r = json.loads(f.readline())
    print(json.dumps(r.get("result", r.get("error"))))
EOF
call() { echo "$2" | python3 "$CLI" "$1"; }
jget() { python3 -c "import json,sys; d=json.load(sys.stdin); print($2)" ; }
entries() { call $1 '{"method":"stats","params":{"col":"px"}}' \
	| python3 -c 'import json,sys; print(json.load(sys.stdin)["collections"][0]["entries"])'; }
entc() { call $1 "{\"method\":\"stats\",\"params\":{\"col\":\"$2\"}}" \
	| python3 -c 'import json,sys; print(json.load(sys.stdin)["collections"][0]["entries"])'; }
cstat() { call $1 '{"method":"stats"}' \
	| python3 -c "import json,sys; print(json.load(sys.stdin)[\"cluster\"][\"$2\"])"; }

# A is SMALL (32MB) so its placements go remote; C joins late with 256MB
node 1 17011 32
node 2 17012 64
node 3 17013 256
start 1 1
start 2 2
sleep 3.5   # join window + election + first keepalives

# ---- a value ABOVE the datagram ceiling, written FIRST and aged a
# second: coldest-first must pick it in the joiner tick -> bulk TCP ------
python3 - <<'EOF'
import json, socket
s = socket.create_connection(("127.0.0.1", 17012), timeout=6)
f = s.makefile("rwb")
f.write(json.dumps({"jsonrpc":"2.0","id":1,"method":"set","params":
    {"col":"px","key":"bigkey","value":"B"*60000}}).encode()+b"\n")
f.flush()
r = json.loads(f.readline())
assert r["result"].get("stored"), r
EOF
[ $? -eq 0 ] && ok || bad "big value stored"
sleep 1.1

# ---- placement: 120 keys via the small ingress land on B --------------
python3 - "$CLI" <<'EOF'
import json, socket, sys
s = socket.create_connection(("127.0.0.1", 17011), timeout=6)
f = s.makefile("rwb")
for i in range(120):
    f.write(json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":
        {"col":"px","key":"pk%03d"%i,"value":"V"*40000}}).encode()+b"\n")
f.flush()
okc = 0
for _ in range(120):
    r = json.loads(f.readline())
    if r.get("result", {}).get("stored"): okc += 1
assert okc == 120, okc
EOF
[ $? -eq 0 ] && ok || bad "placement writes"
# The burst above is the REGRESSION for the receive-buffer flake: a
# forward the kernel drops is a refused write (forwards carry no
# retry), and at the 208KB rmem_max default this leg lost 4-6 of the
# 120 - UDP RcvbufErrors matched the refusals one for one.  That check
# is probabilistic, so pin the cause deterministically too: the daemon
# either GOT the buffer it asked for, or said out loud that it did not.
RB=$(cstat 17011 rcvbuf)
[ "${RB:-0}" -gt 0 ] 2>/dev/null && ok || bad "rcvbuf not reported: $RB"
if [ "${RB:-0}" -ge 8388608 ]; then
	ok
elif grep -q "receive buffer is" "$D/n1.log"; then
	ok        # clamped, but WARNED - honest under-delivery
else
	bad "receive buffer clamped to $RB with no warning logged"
fi
# 400 SMALL records too (the gather-fodder for M_MIGRATE_MANY)
python3 - <<'EOF'
import json, socket
s = socket.create_connection(("127.0.0.1", 17011), timeout=6)
f = s.makefile("rwb")
for i in range(400):
    f.write(json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":
        {"col":"sm","key":"sk%03d"%i,"value":"s"*100}}).encode()+b"\n")
f.flush()
okc = 0
for _ in range(400):
    r = json.loads(f.readline())
    if r.get("result", {}).get("stored"): okc += 1
assert okc == 400, okc
EOF
[ $? -eq 0 ] && ok || bad "small-record writes"
E1=$(entries 17011); E2=$(entries 17012)
echo "entries after ingress-A: A=$E1 B=$E2"
[ $((E1 + E2)) -eq 121 ] && ok || bad "total entries $((E1+E2)) != 121 (120 + bigkey)"
[ "$E2" -gt 100 ] && ok || bad "placement stayed on the full ingress (B=$E2)"

# ---- reads via non-holder: served, not stored, locator kicks in --------
R=$(call 17011 '{"method":"get","params":{"col":"px","key":"pk005"}}')
echo "$R" | grep -q '"source": "cluster"' && ok || bad "proxy read: $R"
EA=$(entries 17011)
[ "$EA" -eq "$E1" ] && ok || bad "non-holder STORED the value (A $E1->$EA)"
L0=$(cstat 17011 loc_hits)
call 17011 '{"method":"get","params":{"col":"px","key":"pk005"}}' >/dev/null
L1=$(cstat 17011 loc_hits)
[ "$L1" -gt "$L0" ] && ok || bad "locator unused on repeat read"

# ---- sticky forward + exact counters -----------------------------------
R=$(call 17011 '{"method":"set","params":{"col":"px","key":"pk005","value":"updated"}}')
echo "$R" | grep -q '"stored": true' && ok || bad "forwarded update: $R"
R=$(call 17012 '{"method":"get","params":{"col":"px","key":"pk005"}}')
echo "$R" | grep -q '"value": "updated"' && ok || bad "update invisible: $R"
# counters: 10 adds via A + 10 via B, all forwarded to ONE holder
for i in 1 2 3 4 5; do
	call 17011 '{"method":"add","params":{"col":"px","key":"ctr","by":1}}' >/dev/null
	call 17012 '{"method":"add","params":{"col":"px","key":"ctr","by":1}}' >/dev/null
done
R=$(call 17013 2>/dev/null || true)
V=$(call 17011 '{"method":"add","params":{"col":"px","key":"ctr","by":0}}' \
	| python3 -c 'import json,sys; print(json.load(sys.stdin)["value"])')
[ "$V" = 10 ] && ok || bad "counter not serialized (got $V, want 10)"

# ---- delete via non-holder ---------------------------------------------
# unprimed (no locator): the tombstone broadcast removes it fleet-wide,
# the reply honestly reflects local absence - the documented contract
R=$(call 17011 '{"method":"del","params":{"col":"px","key":"pk007"}}')
echo "$R" | grep -q '"deleted": false' && ok || bad "unprimed del reply: $R"
sleep 0.4
R=$(call 17012 '{"method":"get","params":{"col":"px","key":"pk007"}}')
echo "$R" | grep -q '"found": false' && ok || bad "key survived delete: $R"
# primed (locator known from a read): the delete FORWARDS to the holder
call 17011 '{"method":"get","params":{"col":"px","key":"pk009"}}' >/dev/null
R=$(call 17011 '{"method":"del","params":{"col":"px","key":"pk009"}}')
echo "$R" | grep -q '"deleted": true' && ok || bad "forwarded delete: $R"
R=$(call 17012 '{"method":"get","params":{"col":"px","key":"pk009"}}')
echo "$R" | grep -q '"found": false' && ok || bad "pk009 survived: $R"

# ---- forwarded JSON ops: the RMW lands at the holder --------------------
call 17011 '{"method":"jset","params":{"col":"px","key":"jd1","val":{"n":1,"l":[]},"mkpath":true}}' >/dev/null
sleep 0.2
R=$(call 17011 '{"method":"jincr","params":{"col":"px","key":"jd1","path":"$.n","by":4}}')
echo "$R" | grep -q '"value": 5' && ok || bad "fwd jincr: $R"
R=$(call 17012 '{"method":"jincr","params":{"col":"px","key":"jd1","path":"$.n"}}')
echo "$R" | grep -q '"value": 6' && ok || bad "fwd jincr other node: $R"
R=$(call 17011 '{"method":"jarrappend","params":{"col":"px","key":"jd1","path":"$.l","val":"e1"}}')
echo "$R" | grep -q '"count": 1' && ok || bad "fwd append: $R"
R=$(call 17012 '{"method":"jget","params":{"col":"px","key":"jd1","path":"$.l[0]"}}')
echo "$R" | grep -q '"e1"' && ok || bad "fwd jget: $R"
R=$(call 17011 '{"method":"jdel","params":{"col":"px","key":"jd1","path":"$.l"}}')
echo "$R" | grep -q '"deleted": true' && ok || bad "fwd jdel: $R"

# ---- birth race: 30 same-new-key races -> exactly one holder each ------
python3 - <<'EOF'
import json, socket, threading
def burst(port, keys, tag):
    s = socket.create_connection(("127.0.0.1", port), timeout=6)
    f = s.makefile("rwb")
    for i, k in enumerate(keys):
        f.write(json.dumps({"jsonrpc":"2.0","id":i,"method":"set","params":
            {"col":"px","key":k,"value":tag}}).encode()+b"\n")
    f.flush()
    for _ in keys: f.readline()
keys = ["race%02d" % i for i in range(30)]
t1 = threading.Thread(target=burst, args=(17011, keys, "viaA"))
t2 = threading.Thread(target=burst, args=(17012, keys, "viaB"))
t1.start(); t2.start(); t1.join(); t2.join()
EOF
sleep 1.5   # demotes settle; reads flush any duals
# force probes so any dual-holder is detected and demoted
for i in 0 1 2; do
	call 17013 2>/dev/null >/dev/null || true
done
python3 - "$CLI" <<'EOF'
import json, socket, subprocess, sys
def local_holders(port, key):
    s = socket.create_connection(("127.0.0.1", port), timeout=6)
    f = s.makefile("rwb")
    f.write(json.dumps({"jsonrpc":"2.0","id":1,"method":"keys","params":
        {"col":"px","match":key}}).encode()+b"\n")
    f.flush()
    return len(json.loads(f.readline())["result"]["keys"])
dual = 0
for i in range(30):
    k = "race%02d" % i
    n = local_holders(17011, k) + local_holders(17012, k)
    if n != 1: dual += 1
assert dual == 0, "%d keys with holder-count != 1" % dual
print("birth race: 30/30 keys have exactly one holder")
EOF
[ $? -eq 0 ] && ok || bad "birth race left duals"

# ---- the late joiner: C starts empty, JOINS AUTOMATICALLY, migration
# fills it (no config anywhere names it - the automatic-membership demo)
start 3 3
sleep 3.5
B0=$(entries 17012); C0=$(entries 17013)
echo "before rebalance: B=$B0 C=$C0"
# Condition-wait, not a fixed sleep: natively the first 10s tick moves
# the records and the loop exits in 1-2 ticks, faster than the old
# `sleep 22`; under sanitizer slowdown the SAME semantics simply take
# longer, and a fixed window read healthy-but-slow as broken.  The
# ceiling is the failure detector.  Totals gate the exit too: mid-tick
# a stubbed record is at neither node, so sample only a quiet moment.
i=0; A1=0; B1=0; C1=0; TOT=0
while [ $i -lt 45 ]; do
	A1=$(entries 17011); B1=$(entries 17012); C1=$(entries 17013)
	TOT=$(( ${A1:-0} + ${B1:-0} + ${C1:-0} ))
	[ "${C1:-0}" -gt 0 ] && [ "$TOT" -eq 151 ] && break
	sleep 2; i=$((i+1))
done
MI=$(cstat 17013 migrated_in 2>/dev/null || echo 0)
echo "after rebalance (${i}x2s): B=$B1 C=$C1 migrated_in(C)=$MI"
[ "${C1:-0}" -gt 0 ] && ok || bad "late joiner received nothing"
[ "$TOT" -eq $((120 - 2 + 1 + 30 + 1 + 1)) ] && ok \
	|| bad "totals not conserved ($TOT != 151, jd1+bigkey included)"
# a migrated key still reads correctly through any node
R=$(call 17011 '{"method":"get","params":{"col":"px","key":"pk042"}}')
echo "$R" | grep -q '"found": true' && ok || bad "migrated key lost: $R"

# the over-ceiling value survived (bulk TCP if it moved, in place if
# not) and SOME donor pushed bulk records when it did move
R=$(call 17013 '{"method":"get","params":{"col":"px","key":"bigkey"}}')
BL=$(echo "$R" | python3 -c 'import json,sys; r=json.load(sys.stdin); print(len(r.get("value","")) if r.get("found") else -1)')
[ "$BL" = 60000 ] && ok || bad "big value lost/corrupt (len=$BL)"
BO=$(cstat 17012 bulk_out)
BI=$(cstat 17013 bulk_in)
echo "bulk: donor bulk_out=$BO joiner bulk_in=$BI"
[ $((BO + BI)) -ge 1 ] && ok || bad "bulk channel unused (out=$BO in=$BI)"

# ---- M_MIGRATE_MANY: the 400 small records moved GATHERED ---------------
# sm registers first so these move in tick one natively; the same
# condition-wait discipline as above covers a slowed world.
i=0; SA=0; SB=0; SC=0
while [ $i -lt 30 ]; do
	SA=$(entc 17011 sm); SB=$(entc 17012 sm); SC=$(entc 17013 sm)
	[ $(( ${SA:-0} + ${SB:-0} + ${SC:-0} )) -eq 400 ] && \
		[ "${SC:-0}" -ge 300 ] && break
	sleep 2; i=$((i+1))
done
SA=${SA:-0}; SB=${SB:-0}; SC=${SC:-0}
MO=$(cstat 17012 migrated_out); MD=$(cstat 17012 migrate_dgrams)
ML=$(cstat 17012 migrate_lost)
echo "small records: A=$SA B=$SB C=$SC; donor migrated_out=$MO in $MD datagram(s), lost=$ML"
[ $((SA + SB + SC)) -eq 400 ] && ok || bad "small records not conserved ($((SA+SB+SC)) != 400)"
[ "$SC" -ge 300 ] && ok || bad "small records not migrated (C=$SC)"
[ "$ML" = 0 ] && ok || bad "migration lost records ($ML)"
# the gather proof: >=449 records left B in far fewer datagrams - the
# 400 small ones fit ~1 group (~450/56KB) vs 40KB records at 1/each
[ "$MD" -le $((MO - 350)) ] && ok \
	|| bad "no gathering: $MO records took $MD datagrams"
R=$(call 17011 '{"method":"get","params":{"col":"sm","key":"sk123"}}')
echo "$R" | grep -q '"found": true' && ok || bad "small migrated key lost: $R"

# ---- probe-before-place: a fresh-locator ingress must not fork ----------
# Regression for the measured locator-eviction duplication: a RE-WRITE
# through a node that no longer knew the holder re-entered placement,
# which kept the key local ~1/3 of the time - a duplicate holder (the
# container matrix measured ~220.7k entries for 200k keys).  A restart
# is the deterministic eviction: node B comes back with an empty
# locator AND empty negative cache, so every re-write must
# probe-then-forward, never fork.
S0=$(( $(entries 17011) + $(entries 17012) + $(entries 17013) ))
E2_0=$(entries 17012)
PKCOUNT=0
for pp in 17011 17012 17013; do
	CNT=$(call $pp '{"method":"keys","params":{"col":"px","match":"pk*","limit":1000}}' 		| python3 -c 'import json,sys; print(len(json.load(sys.stdin)["keys"]))')
	PKCOUNT=$((PKCOUNT + CNT))
done
K2=$(call 17012 '{"method":"keys","params":{"col":"px","match":"pk*","limit":1000}}' | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["keys"]))')
kill -9 $P2 2>/dev/null; wait $P2 2>/dev/null || true
start 2 2
i=0
while [ $i -lt 60 ]; do
	UP=$(cstat 17012 peers_up 2>/dev/null || echo 0)
	[ "$UP" = 2 ] && break
	sleep 0.2; i=$((i+1))
done
[ "$UP" = 2 ] && ok || bad "restarted node did not rejoin"
PS0=$(cstat 17012 pull_sent)
for i in $(seq 0 119); do
	printf '{"method":"set","params":{"col":"px","key":"pk%03d","value":"rw-%03d"}}\n' $i $i
done | python3 "$CLI" 17012 > "$D/rewr.out"
RW_OK=$(grep -c '"stored": true' "$D/rewr.out" || true)
S1=$(( $(entries 17011) + $(entries 17012) + $(entries 17013) ))
PS1=$(cstat 17012 pull_sent)
# the kill wiped node2's in-memory shard (proxy node loss loses its
# keys, by design).  Expected: survivors' entries plus a re-create for
# every key that DIED with node2 (K2) or was deleted earlier
# (120 - PKCOUNT).  Any fork lands ABOVE this exact number.
WANT=$((S0 - E2_0 + K2 + 120 - PKCOUNT))
echo "probe-before-place: sum $S0 -> $S1 (want $WANT; node2 held $E2_0 incl $K2 pk; $PKCOUNT of 120 existed); acked=$RW_OK; probes=$((PS1 - PS0))"
[ "$S1" -eq "$WANT" ] && ok || bad "re-writes through a fresh locator FORKED ($S0 -> $S1, want $WANT)"
[ "$RW_OK" -ge 118 ] && ok || bad "rewrites refused ($RW_OK/120)"
[ $((PS1 - PS0)) -ge 100 ] && ok || bad "probe path did not execute ($((PS1 - PS0)) probes)"
R=$(call 17013 '{"method":"get","params":{"col":"px","key":"pk007"}}')
echo "$R" | grep -q 'rw-007' && ok || bad "rewritten value not readable fleet-wide: $R"
# a BLIND create probes once (fast all-negative), then placement decides
R=$(call 17012 '{"method":"set","params":{"col":"px","key":"brandnew","value":"nv"}}')
echo "$R" | grep -q '"stored": true' && ok || bad "blind create refused: $R"
S2=$(( $(entries 17011) + $(entries 17012) + $(entries 17013) ))
[ "$S2" -eq $((S1 + 1)) ] && ok || bad "blind create census wrong ($S1 -> $S2)"
# the get-miss -> set pattern pays NO probe: the get already stamped the
# negative cache, so the set places immediately
call 17012 '{"method":"get","params":{"col":"px","key":"gated"}}' >/dev/null
PS3=$(cstat 17012 pull_sent)
R=$(call 17012 '{"method":"set","params":{"col":"px","key":"gated","value":"gv"}}')
echo "$R" | grep -q '"stored": true' && ok || bad "neg-gated create refused: $R"
PS4=$(cstat 17012 pull_sent)
[ "$PS4" -eq "$PS3" ] && ok || bad "neg-gated set still probed ($PS3 -> $PS4)"

# everyone alive
for p in 17011 17012 17013; do
	call $p '{"method":"ping"}' | grep -q pong && ok || bad "node on $p died"
done

kill -TERM $P1 $P2 $P3 2>/dev/null; wait 2>/dev/null; P1= P2= P3=
echo "proxytest: $pass passed, $fail failed"
[ $fail -eq 0 ]
