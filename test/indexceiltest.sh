#!/bin/sh
# indexceiltest.sh - S128: a collection's index is carved UNDER the arena
# ceiling, and one range means one range.
#
# The arena's ceiling is enforced on the chunk carve - that is what makes
# `arena full - write dropped` reach a client - but the REGION carve, the
# index structures a table is built from, fell through to shm_malloc with
# no test at all.  So records were bounded and the tables holding them
# were not: one `create {buckets_log2: 24}` on a 2 GB node left the arena
# holding 2.6 GB, 21% past a bound that is supposed to be hard.
#
# A node with a 64 MB arena is asked for indexes it cannot afford, at
# `create`, at `resize` and in its own config file at startup, and must
# refuse each one while HOLDING NO MORE THAN ITS CEILING.  Each refusal is
# paired with a size that fits, so a blanket refusal cannot pass.
#
# Fail-first on the pre-S128 build: the 2^20 create SUCCEEDS and held goes
# to ~200 MB against a 64 MB max, the 2^20 resize likewise, the 2^22
# startup collection starts, and `buckets_log2 = 1` is accepted by the
# config while the verbs refuse it.
set -u
BIN=${1:-./perfcached}
SEC=idx-client-secret
D=$(mktemp -d /var/tmp/pcix.XXXXXX)
P=""
trap '[ -n "$P" ] && grep -qa -- "$D" /proc/$P/cmdline 2>/dev/null && kill -9 "$P"; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

MB=1048576
MAXMB=64

conf() { # conf <file> <extra collection block>
	cat > "$1" <<CONF
[daemon]
workers = 2
log_level = notice
allow_create = yes
[memory]
arena_mb = $MAXMB
[secrets]
client = $SEC
cluster = idx-cluster-secret
[listen]
plaintext = loopback
tcp = 127.0.63.1:17981
[collection c]
buckets_log2 = 12
$2
CONF
}

conf "$D/n1.conf" ""
"$BIN" -f "$D/n1.conf" >> "$D/n1.log" 2>&1 &
P=$!
i=0
while [ $i -lt 200 ]; do
	grep -q "perfcached ready" "$D/n1.log" 2>/dev/null && break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/n1.log" || { echo "node did not start"; cat "$D/n1.log"; exit 1; }

# drive <op> [args] - create/resize print "OK" or "ERR <message>";
# held/max print the memory figure; set/get exercise a live collection
drive() {
	python3 - "$@" <<'PYEOF'
import json, socket, sys
s = socket.create_connection(("127.0.63.1", 17981), timeout=20)
f = s.makefile("rwb"); rid = [0]
def call(m, **p):
    rid[0] += 1
    r = {"jsonrpc": "2.0", "id": rid[0], "method": m}
    if p: r["params"] = p
    f.write((json.dumps(r) + "\n").encode()); f.flush()
    return json.loads(f.readline())
def say(r):
    if "error" in r:
        print("ERR " + str(r["error"].get("message", r["error"])))
    else:
        print("OK")
op = sys.argv[1]
if op == "create":
    say(call("create", col=sys.argv[2], buckets_log2=int(sys.argv[3])))
elif op == "resize":
    say(call("resize", col=sys.argv[2], buckets_log2=int(sys.argv[3])))
elif op == "set":
    say(call("set", col=sys.argv[2], key=sys.argv[3], value=sys.argv[4], ttl=60))
elif op == "get":
    r = call("get", col=sys.argv[2], key=sys.argv[3])
    print(r.get("result", {}).get("value", "MISSING") if "error" not in r else "ERR")
elif op == "cols":
    r = call("stats"); r = r.get("result", r)
    print(" ".join("%s:%d" % (c.get("name"), c.get("buckets", 0))
                   for c in r.get("collections", [])))
else:
    r = call("stats"); r = r.get("result", r).get("memory", {})
    print(r.get(sys.argv[2], "MISSING"))
PYEOF
}

# every held reading in this suite must be under the ceiling: that is the
# property, not a step towards it
MAX=$(drive mem arena_max)
[ "$MAX" = "$(( MAXMB * MB ))" ] \
	&& ok "the ceiling is the configured arena_mb ($MAX bytes)" \
	|| bad "arena_max is $MAX, wanted $(( MAXMB * MB ))"
under() { # under <label>
	H=$(drive mem arena_held)
	[ "$H" != MISSING ] && [ "$H" -le "$MAX" ] \
		&& ok "held is under the ceiling $1 ($H <= $MAX)" \
		|| bad "held $H is PAST the ceiling $MAX $1"
}
under "at start"
H0=$(drive mem arena_held)

# 1. a create whose index does not fit is refused, and says what it would
#    have taken.  2^20 is 192.8 MB of index against a 64 MB ceiling.
R=$(drive create toobig 20)
case "$R" in
ERR*needs*index*holds*ceiling*)
	ok "the oversized create is refused, naming the figures: $R" ;;
ERR*)	bad "refused with the wrong message: $R" ;;
*)	bad "the 2^20 create SUCCEEDED on a $MAXMB MB arena: $R" ;;
esac
H1=$(drive mem arena_held)
[ "$H1" = "$H0" ] && ok "the refused create carved nothing (held still $H1)" \
	|| bad "the refused create moved held $H0 -> $H1"
under "after the refused create"

# 2. ... and a size that fits still works, so the refusal is not blanket
R=$(drive create fits 16)
[ "$R" = OK ] && ok "a 2^16 create (12.8 MB of index) is accepted" \
	|| bad "the affordable create was refused: $R"
H2=$(drive mem arena_held)
[ "$H2" -gt "$H1" ] && ok "it carved its index (held $H1 -> $H2)" \
	|| bad "held did not move for a table that was created ($H1 -> $H2)"
under "after the accepted create"

# 3. a resize is the sharper case - the OLD table is held while the NEW
#    one is built, so the ceiling has to cover both at once
R=$(drive resize c 20)
case "$R" in
ERR*needs*index*holds*ceiling*) ok "the oversized resize is refused: $R" ;;
ERR*)	bad "the resize refused with the wrong message: $R" ;;
*)	bad "the 2^20 resize SUCCEEDED on a $MAXMB MB arena: $R" ;;
esac
under "after the refused resize"
echo "$(drive cols)" | grep -q "c:4096" \
	&& ok "the collection is untouched by the refused resize (still 2^12)" \
	|| bad "the refused resize changed the table: $(drive cols)"
[ "$(drive set c k1 v1)" = OK ] && [ "$(drive get c k1)" = v1 ] \
	&& ok "the collection still stores and returns after the refusal" \
	|| bad "the collection is broken after a refused resize"

# 4. ... and a resize that fits still runs
R=$(drive resize c 14)
[ "$R" = OK ] && ok "a 2^14 resize (3.8 MB of index) is accepted" \
	|| bad "the affordable resize was refused: $R"
under "after the accepted resize"

# 5. the range is 4..24 at the verb, both ends
rngbad=0
for b in 0 1 3 25 31; do
	R=$(drive create r$b $b)
	case "$R" in
	ERR*4..24*) : ;;
	*) bad "buckets_log2 $b was not refused as out of range: $R"; rngbad=1 ;;
	esac
done
[ $rngbad -eq 0 ] && ok "buckets_log2 0, 1, 3, 25 and 31 are all refused as out of range"
[ "$(drive create edge4 4)" = OK ] && ok "buckets_log2 4 is accepted (the floor)" \
	|| bad "buckets_log2 4 was refused"

kill "$P" 2>/dev/null; wait "$P" 2>/dev/null; P=""

# 6. the same range in the config file.  This parsed 1..24 while the verbs
#    took 4..24: the same value legal in one place and refused in the other.
cfgbad=0
for b in 1 3 25; do
	conf "$D/bad$b.conf" ""
	sed -i "s/^buckets_log2 = 12/buckets_log2 = $b/" "$D/bad$b.conf"
	if "$BIN" -C -f "$D/bad$b.conf" >"$D/c$b.out" 2>&1; then
		bad "the config accepted buckets_log2 = $b where the verbs refuse it"
		cfgbad=1
	fi
done
[ $cfgbad -eq 0 ] && ok "the config refuses buckets_log2 1, 3 and 25, as the verbs do"
conf "$D/ok4.conf" ""
sed -i "s/^buckets_log2 = 12/buckets_log2 = 4/" "$D/ok4.conf"
"$BIN" -C -f "$D/ok4.conf" >/dev/null 2>&1 \
	&& ok "the config accepts buckets_log2 = 4, as the verbs do" \
	|| bad "the config refused buckets_log2 = 4"

# 7. a collection the config asks for and the arena cannot hold: the node
#    refuses to START rather than silently exceeding what it was sized for
conf "$D/huge.conf" "[collection big]
buckets_log2 = 22"
# the pre-S128 daemon STARTS here and runs forever, so this is bounded and
# "still alive at the bound" is the failure, not a slow refusal
timeout 20 "$BIN" -f "$D/huge.conf" > "$D/huge.log" 2>&1
rc=$?
if [ $rc -eq 124 ]; then
	bad "the node STARTED with a configured index it cannot hold (held $(grep -c . "$D/huge.log") log lines)"
elif [ $rc -ne 0 ]; then
	ok "a 2^22 collection (768.8 MB of index) on a $MAXMB MB arena refuses to start (rc=$rc)"
else
	bad "the node exited 0 with a configured index it cannot hold"
fi
grep -q "does not fit under the arena's ceiling" "$D/huge.log" \
	&& ok "and the log says why: $(grep -m1 "does not fit under" "$D/huge.log" | sed 's/.*ERROR: *//')" \
	|| { bad "the startup refusal did not name the ceiling"; sed 's/^/    /' "$D/huge.log"; }

echo "indexceiltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
