#!/bin/sh
# pressuretest.sh — S47: memory pressure must be VISIBLE before it bites.
#
# arena_mb is a real bound (b1e71f1): a full node refuses writes.  This
# suite proves the operator can see the cliff coming and the recovery
# happening:
#   1. the stats memory block carries the pressure surface: tier,
#      headroom_pct, nomem (writes refused arena-full), and a reclaim
#      block (retired / pages_freed / released_bytes / cold_bytes /
#      flushes / giveback)
#   2. filling to refusal raises nomem and drives headroom_pct to ~0,
#      and the refusal is the honest "cache full" error
#   3. drain + a quiet window gives memory back (released_bytes grows,
#      headroom recovers) - reclaim driven by a workload, not a unit.
#      NOTE the arena_cap_mb > arena_mb config: the huge-page
#      RESERVATION is mlocked, and mlock/MADV_DONTNEED are mutually
#      exclusive (EINVAL), so a node that never grows past its
#      reservation can never give anything back.  What comes back is
#      the malloc'd page overflow above the reservation - so the
#      workload has to create some.
#   4. reclaim_floor_mb holds a floor under give-back (the HG-v3
#      hsize_min adoption): the floored node keeps >= floor held while
#      the unfloored one shrinks below it - the control matters, a
#      node that cannot shrink AT ALL would satisfy any floor
#   5. a floor wider than the arena cap is refused at config time
# Usage: test/pressuretest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
SEC=press-client-secret
D=$(mktemp -d /var/tmp/pcpr.XXXXXX)
trap 'pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

mk() { # mk <n> <extra-memory-lines>
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 16
arena_cap_mb = 64
reclaim_keep = 1
reclaim_quiet_s = 1
reclaim_cooloff_s = 1
$2
[secrets]
client = $SEC
cluster = press-cluster-secret
[listen]
plaintext = loopback
tcp = 127.0.61.$1:1796$1
[collection p]
buckets_log2 = 12
EOF
}
start() {
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}

# drive <node> <op> ... - one connection, many ops, python speaks the
# text dialect.  Ops: fill <n> <ttl> (60KB values, prints stored/full),
# drain <n> (dels), mem <field> (prints stats.memory.<field>, dotted
# path allowed, MISSING when absent)
drive() {
	n=$1; shift
	python3 - "$n" "$@" <<'PYEOF'
import json, socket, sys
node = sys.argv[1]
s = socket.create_connection(("127.0.61." + node, int("1796" + node)),
	timeout=10)
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
op = sys.argv[2]
if op == "fill":
    n, ttl = int(sys.argv[3]), int(sys.argv[4])
    stored = full = 0
    v = "x" * 60000
    for i in range(n):
        p = {"col":"p","key":"pk%04d" % i,"value":v}
        if ttl: p["ttl"] = ttl
        r = call("set", **p)
        if "error" in r:
            if r["error"].get("message") == "cache full":
                full += 1
            else:
                print("ERR:" + r["error"].get("message","?")); sys.exit(0)
        else:
            stored += 1
    print("stored=%d full=%d" % (stored, full))
elif op == "drain":
    n = int(sys.argv[3])
    for i in range(n):
        call("del", col="p", key="pk%04d" % i)
    print("drained")
elif op == "mem":
    r = call("stats")
    o = r.get("result", {}).get("memory", {})
    for k in sys.argv[3].split("."):
        o = o.get(k) if isinstance(o, dict) else None
        if o is None:
            print("MISSING"); sys.exit(0)
    print(o)
PYEOF
}

mk 1 ""
mk 2 "reclaim_floor_mb = 32"
start 1 || { echo "node 1 did not start:"; tail -3 "$D/n1.log"; exit 1; }
N2UP=1
start 2 || { N2UP=0
	bad "floor node did not start (reclaim_floor_mb rejected?)"; }

# ---- 1. the pressure surface exists -----------------------------------
T=$(drive 1 mem tier)
case "$T" in ""|MISSING|DRIVEFAIL|none) bad "tier not reported ($T)";;
	*huge*|*4K*|*4k*) ok "tier is reported ($T)";;
	*) bad "tier unrecognised ($T)";; esac
H=$(drive 1 mem headroom_pct)
case "$H" in MISSING) bad "headroom_pct missing";;
	*) [ "$H" -ge 0 ] 2>/dev/null && [ "$H" -le 100 ] \
		&& ok "headroom_pct in range ($H)" \
		|| bad "headroom_pct out of range ($H)";; esac
N0=$(drive 1 mem nomem)
[ "$N0" = "0" ] && ok "nomem starts at zero" \
	|| bad "nomem missing or nonzero at start ($N0)"
R=$(drive 1 mem reclaim.released_bytes)
case "$R" in ""|MISSING|DRIVEFAIL) bad "reclaim block missing ($R)";;
	*) ok "reclaim block present (released_bytes=$R)";; esac

# ---- 2. filling to the bound is refused, counted, and visible ---------
F=$(drive 1 fill 900 0)
case "$F" in
*full=0*|ERR:*|pyfail*) bad "the arena never refused ($F)";;
*full=*) ok "the bound refused honestly ($F)";;
*) bad "fill did not run ($F)";; esac
N1=$(drive 1 mem nomem)
[ "${N1:-0}" -gt 0 ] 2>/dev/null && ok "nomem counted the refusals ($N1)" \
	|| bad "nomem did not move ($N1)"
H1=$(drive 1 mem headroom_pct)
[ "${H1:-100}" -le 5 ] 2>/dev/null && ok "headroom collapsed at the bound ($H1)" \
	|| bad "headroom_pct still high at refusal ($H1)"

# ---- 3. drain + quiet gives memory back -------------------------------
drive 1 drain 900 >/dev/null
sleep 6
R1=$(drive 1 mem reclaim.released_bytes)
[ "${R1:-0}" -gt 0 ] 2>/dev/null && ok "give-back happened (released=$R1)" \
	|| bad "nothing released after drain+quiet ($R1)"
H2=$(drive 1 mem headroom_pct)
[ "${H2:-0}" -gt "${H1:-0}" ] 2>/dev/null \
	&& ok "headroom recovered ($H1 -> $H2)" \
	|| bad "headroom did not recover ($H1 -> $H2)"

# ---- 4. the shrink floor holds ----------------------------------------
FLOOR=$((32 * 1048576))
if [ "$N2UP" = 1 ]; then
	drive 2 fill 900 0 >/dev/null
	drive 2 drain 900 >/dev/null
	sleep 6
	HELD2=$(drive 2 mem arena_held)
	[ "${HELD2:-0}" -ge "$FLOOR" ] 2>/dev/null \
		&& ok "floored node held >= 32MB ($HELD2)" \
		|| bad "the floor did not hold ($HELD2 < $FLOOR)"
else
	bad "floor hold unproven - the floored node never ran"
fi
HELD1=$(drive 1 mem arena_held)
case "$HELD1" in ''|*[!0-9]*) bad "unfloored held unreadable ($HELD1)";;
*) [ "$HELD1" -lt "$FLOOR" ] \
	&& ok "unfloored node shrank below the floor ($HELD1)" \
	|| bad "unfloored node never shrank ($HELD1) - the floor check
	         above proves nothing";; esac

# ---- 5. the pinned reservation is an OPERATOR-VISIBLE fact ------------
# mlock and MADV_DONTNEED cannot both apply to the reservation, so a
# refused punch is expected here - and must be reported, not swallowed.
GB=$(drive 1 mem reclaim.giveback_off)
case "$GB" in true|false|True|False) ok "giveback_off is reported ($GB)";;
	*) bad "giveback_off missing ($GB)";; esac

# ---- 6. a floor wider than the cap is refused -------------------------
mk 3 "reclaim_floor_mb = 128"    # above the 64 MB arena_cap_mb
"$BIN" -f "$D/n3.conf" -C >/dev/null 2>&1 \
	&& bad "floor > arena cap was accepted" \
	|| ok "floor > arena cap refused at config time"

echo "pressuretest: $pass passed, $fail failed"
[ $fail -eq 0 ]
