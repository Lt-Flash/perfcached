#!/bin/sh
# hugetlbtest.sh — S94: a punched-out hugetlb page is re-committed only
# once it is secured; an empty pool refuses the re-commit, it never
# faults blind (that fault is a SIGBUS - the daemon used to DIE here).
#
# Needs a real hugetlb pool: /sys/kernel/mm/hugepages/hugepages-2048kB/
# free_hugepages readable and holding enough pages for a small arena
# plus a few, and the daemon must actually land on tier 1.  Anything
# else SKIPS, loudly - a container with no pool proves nothing either
# way and must not read as green.
#
#  1. a tier-1 node fills its reservation, drains, and punches groups
#     out (cold_bytes > 0; the pool's free count rises)
#  2. the pool is drained FROM OUTSIDE (a holder mmaps every free page)
#  3. the node is written to again: it must stay ALIVE, serve the writes
#     from the 4K overflow or refuse them at the ceiling, and count the
#     refused re-commits in stats.memory.pool_empty
#  4. the holder lets go; the node serves again and is still alive
# Before the fix step 3 killed the process (SIGBUS on the re-fault).
# Usage: test/hugetlbtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
SEC=htlb-client-secret
SYS=/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages
D=$(mktemp -d /var/tmp/pchl.XXXXXX)
HOLDER=
trap '[ -n "$HOLDER" ] && kill -9 $HOLDER 2>/dev/null; pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
skip() { echo "hugetlbtest: SKIP - $1"; echo "hugetlbtest: 0 passed, 0 failed (skipped)"; exit 0; }

FREE=$(cat "$SYS" 2>/dev/null) || skip "no hugetlb pool here ($SYS unreadable)"
case "$FREE" in ''|*[!0-9]*) skip "unreadable free count '$FREE'";; esac
[ "$FREE" -ge 8 ] || skip "only $FREE free huge pages; the arena needs 4 and the test a margin"

cat > "$D/n1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 8
arena_cap_mb = 64
reclaim_keep = 0
reclaim_quiet_s = 1
reclaim_cooloff_s = 1
[secrets]
client = $SEC
cluster = htlb-cluster-secret
[listen]
plaintext = loopback
tcp = 127.0.62.1:17981
[collection h]
buckets_log2 = 12
EOF
"$BIN" -f "$D/n1.conf" >> "$D/n1.log" 2>&1 &
PID=$!
i=0; while [ $i -lt 200 ]; do grep -q "perfcached ready" "$D/n1.log" 2>/dev/null && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n1.log" || { echo "node did not start:"; tail -3 "$D/n1.log"; exit 1; }

drive() { # drive <op> ...  (fill <n> 30KB values | drain <n> | mem <dotted>)
	python3 - "$@" <<'PYEOF'
import json, socket, sys
try:
    s = socket.create_connection(("127.0.62.1", 17981), timeout=10)
except Exception:
    print("DRIVEFAIL"); sys.exit(0)
f = s.makefile("rwb"); rid = [0]
def call(m, **p):
    rid[0] += 1
    r = {"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"] = p
    try:
        f.write((json.dumps(r) + "\n").encode()); f.flush()
        return json.loads(f.readline())
    except Exception:
        print("DRIVEFAIL"); sys.exit(0)
op = sys.argv[1]
if op == "fill":
    stored = full = 0; v = "x" * 30000
    for i in range(int(sys.argv[2])):
        r = call("set", col="h", key="hk%04d" % i, value=v)
        if "error" in r:
            if r["error"].get("message") == "cache full": full += 1
            else: print("ERR:" + r["error"].get("message", "?")); sys.exit(0)
        else: stored += 1
    print("stored=%d full=%d" % (stored, full))
elif op == "drain":
    for i in range(int(sys.argv[2])): call("del", col="h", key="hk%04d" % i)
    print("drained")
elif op == "mem":
    o = call("stats").get("result", {}).get("memory", {})
    for k in sys.argv[2].split("."):
        o = o.get(k) if isinstance(o, dict) else None
        if o is None: print("MISSING"); sys.exit(0)
    print(o)
PYEOF
}

T=$(drive mem tier)
case "$T" in *HUGETLB*) ok "the node is on tier 1 ($T)";;
	*) skip "the daemon did not get tier 1 (tier='$T') - a pool exists but this process cannot use it";; esac

# ---- 1. fill, drain, punch --------------------------------------------
F=$(drive fill 400)
case "$F" in stored=*) ok "filled ($F)";; *) bad "fill failed ($F)";; esac
F0=$(cat "$SYS")
drive drain 400 >/dev/null
sleep 5
CB=$(drive mem reclaim.cold_bytes)
[ "${CB:-0}" -gt 0 ] 2>/dev/null && ok "groups were punched out (cold_bytes=$CB)" \
	|| bad "nothing was punched (cold_bytes=$CB) - the rest of this suite proves nothing"
F1=$(cat "$SYS")
[ "$F1" -gt "$F0" ] && ok "the pool got its pages back ($F0 -> $F1 free)" \
	|| bad "the pool did not grow ($F0 -> $F1)"

# ---- 2. drain the pool from OUTSIDE -----------------------------------
python3 - "$SYS" > "$D/holder.log" 2>&1 <<'PYEOF' &
import mmap, sys, time
sysf = sys.argv[1]
MAP_HUGETLB = 0x40000; MAP_POPULATE = 0x8000
held = []
while True:
    n = int(open(sysf).read())
    if n <= 0: break
    try:
        m = mmap.mmap(-1, 2 << 20, flags=mmap.MAP_PRIVATE|mmap.MAP_ANONYMOUS|MAP_HUGETLB|MAP_POPULATE)
        m[0] = 1; held.append(m)
    except Exception as e:
        print("hold failed at %d: %s" % (len(held), e)); break
print("holding %d pages" % len(held), flush=True)
time.sleep(3600)
PYEOF
HOLDER=$!
i=0; while [ $i -lt 100 ]; do [ "$(cat "$SYS")" = "0" ] && break; sleep 0.1; i=$((i+1)); done
[ "$(cat "$SYS")" = "0" ] && ok "the pool is empty from outside ($(cat "$D/holder.log" | tail -1))" \
	|| bad "could not empty the pool ($(cat "$SYS") free; $(tail -1 "$D/holder.log"))"

# ---- 3. write again: alive, served or refused, never faulted -----------
F=$(drive fill 400)
kill -0 $PID 2>/dev/null && ok "the daemon is ALIVE after re-committing against an empty pool" \
	|| bad "the daemon DIED (this is the SIGBUS S94 exists for): $(tail -2 "$D/n1.log")"
case "$F" in stored=*) ok "the writes were served or honestly refused ($F)";;
	*) bad "the writes were neither served nor refused ($F)";; esac
PE=$(drive mem pool_empty)
[ "${PE:-0}" -gt 0 ] 2>/dev/null && ok "refused re-commits are counted (pool_empty=$PE)" \
	|| bad "pool_empty did not move ($PE) - either nothing was cold or the refusal is not counted"

# ---- 4. the holder lets go: the node serves again ----------------------
kill -9 $HOLDER 2>/dev/null; wait $HOLDER 2>/dev/null; HOLDER=
sleep 1
drive drain 400 >/dev/null
F=$(drive fill 100)
case "$F" in stored=*) ok "served again once the pool came back ($F)";; *) bad "not serving after the pool returned ($F)";; esac
kill -0 $PID 2>/dev/null && ok "still alive at the end" || bad "died after the pool returned"

echo "hugetlbtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
