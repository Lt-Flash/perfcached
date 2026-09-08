#!/bin/sh
# ceilingtest.sh - S114: the spell at the ceiling ENDS once the arena can
# carve again, and `held` says what it is made of.  One node with a 16 MB
# reservation under a 64 MB ceiling (the elastic shape pressuretest uses:
# a fixed reservation cannot give back by construction) is driven past
# the ceiling with short-lived 60 KB values (writes refused, at_ceiling
# true), the records expire and the give-back runs,
# and the flag must clear within a few ticks - it used to clear only on a
# successful carve, which a drained table never needs.  The structural
# figure (regions: bucket directories, hint tables, counters) must be
# there, a whole number of slots, and unchanged by a burst that grew no
# table; the warm-free figure must fall when the give-back punches.
# Fail-first: a daemon without arena_regions answers MISSING, and one
# that clears the spell only on a carve keeps at_ceiling true here.
set -u
BIN=${1:-./perfcached}
SEC=ceil-client-secret
D=$(mktemp -d /var/tmp/pcce.XXXXXX)
trap 'pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
cat > "$D/n1.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 16
arena_cap_mb = 64
reclaim_keep = 1
reclaim_quiet_s = 1
reclaim_cooloff_s = 1
shrink_step_mb = 16
[secrets]
client = $SEC
cluster = ceil-cluster-secret
[listen]
plaintext = loopback
tcp = 127.0.62.1:17971
[collection c]
buckets_log2 = 12
CONF
"$BIN" -f "$D/n1.conf" >> "$D/n1.log" 2>&1 &
i=0
while [ $i -lt 200 ]; do
	grep -q "perfcached ready" "$D/n1.log" 2>/dev/null && break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/n1.log" || { echo "node did not start"; cat "$D/n1.log"; exit 1; }
# drive <op> [args] - ops: fill <n> <ttl> (60 KB values; prints stored/full),
# mem <field> (stats.memory.<field>, dotted path allowed; MISSING when absent),
# buckets (sum of collections[].buckets)
drive() {
	python3 - "$@" <<'PYEOF'
import json, socket, sys
s = socket.create_connection(("127.0.62.1", 17971), timeout=10)
f = s.makefile("rwb"); rid = [0]
def call(m, **p):
    rid[0] += 1
    r = {"jsonrpc": "2.0", "id": rid[0], "method": m}
    if p: r["params"] = p
    f.write((json.dumps(r) + "\n").encode()); f.flush()
    return json.loads(f.readline())
op = sys.argv[1]
if op == "fill":
    n, ttl = int(sys.argv[2]), int(sys.argv[3]); stored = full = 0
    v = "x" * 60000
    for i in range(n):
        r = call("set", col="c", key="ck%04d" % i, value=v, ttl=ttl)
        if "error" in r:
            if "full" in json.dumps(r["error"]): full += 1
        else: stored += 1
    print("stored=%d full=%d" % (stored, full))
elif op == "buckets":
    r = call("stats"); r = r.get("result", r)
    print(sum(c.get("buckets", 0) for c in r.get("collections", [])))
else:
    r = call("stats"); r = r.get("result", r).get("memory", {})
    for k in sys.argv[2].split("."):
        r = r.get(k) if isinstance(r, dict) else None
    print("MISSING" if r is None else r)
PYEOF
}
await_mem() { # await_mem <field> <want> <secs>
	i=0
	while [ $i -lt $(( $3 * 10 )) ]; do
		[ "$(drive mem $1)" = "$2" ] && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}

R0=$(drive mem arena_regions); W0=$(drive mem arena_warm_free); B0=$(drive buckets)
[ "$R0" != MISSING ] && [ "$R0" -gt 0 ] && [ $(( R0 % 262144 )) -eq 0 ] \
	&& ok "structure is reported: arena_regions $R0 bytes, a whole number of 256 KB slots" \
	|| bad "arena_regions absent or not slot-sized: $R0"
[ "$W0" != MISSING ] && ok "warm-free is reported ($W0 bytes at start)" || bad "arena_warm_free absent"

# 1. past the ceiling: 60 KB values with a 2 s life, until refused
R=$(drive fill 1300 2)
FULL=$(echo "$R" | sed 's/.*full=//')
[ "$FULL" -gt 0 ] && ok "the burst was refused at the ceiling ($R)" || bad "never refused: $R"
[ "$(drive mem at_ceiling)" = True ] && ok "at_ceiling is true during the spell" || bad "at_ceiling not true at the ceiling"
N0=$(drive mem nomem)
[ "$N0" -gt 0 ] && ok "nomem counts the refusals ($N0)" || bad "nomem 0 after refusals"
H1=$(drive mem arena_held)

# 2. the records expire, the give-back runs, the spell must end
await_mem at_ceiling False 25 && ok "at_ceiling cleared once a carve would succeed again (after $(drive mem arena_held) bytes held)" \
	|| bad "at_ceiling still true 25 s after the burst expired (held $(drive mem arena_held), max $(drive mem arena_max))"
N1=$(drive mem nomem)
[ "$N1" = "$N0" ] && ok "no new refusals while it cleared (nomem $N1)" || bad "nomem moved $N0 -> $N1"
H2=$(drive mem arena_held)
[ "$H2" -lt "$H1" ] && ok "held fell after the drain ($H1 -> $H2)" || bad "held did not fall ($H1 -> $H2)"

# 3. structure is unchanged by a burst that grew no table; warm-free fell
R1=$(drive mem arena_regions); B1=$(drive buckets)
[ "$B1" = "$B0" ] && ok "no table grew (buckets $B1)" || echo "  note: buckets grew $B0 -> $B1"
[ "$B1" = "$B0" ] && { [ "$R1" = "$R0" ] && ok "arena_regions unchanged by the burst ($R1): structure, not data" \
	|| bad "arena_regions moved $R0 -> $R1 with no table growth"; }
W1=$(drive mem arena_warm_free)
[ "$W1" -le "$W0" ] || [ "$W1" -lt "$H2" ] && ok "warm-free after the give-back: $W1 bytes (held $H2, regions $R1)" \
	|| bad "warm-free $W1 exceeds held $H2"
echo "ceilingtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
