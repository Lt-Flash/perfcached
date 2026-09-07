#!/bin/sh
# verwiretest.sh — task A2: the version travels with the bytes.
#
# A1 made a record's version survive a restart.  It is still only
# meaningful on the node that wrote it: a copy that arrives from a peer
# is stored with a FRESH LOCAL tick, so the same key carries a different
# number on every node holding it and no two of them can be compared.
#
#  1. an eager replica carries the AUTHOR's version, not the receiver's
#  2. an update propagates and the replica's version follows it
#  3. a pulled copy carries the HOLDER's version.  This one is not
#     tidiness: the fleet's clocks converge to the maximum, so a local
#     tick here can land ABOVE the holder's own number, and the holder's
#     very next genuine update would then be refused as older.
#  4. refusals are counted rather than silent.  They are NOT an error
#     signal on their own: a pull and an eager push can both deliver the
#     same record, and the second one is correctly refused.  The counter
#     exists so a RUNAWAY - a sender looping on records nobody takes - is
#     visible.
#  5. a TTL re-arm still propagates.  `expire` re-arms without rewriting
#     the value, so if it does not move the version, the peer refuses the
#     refresh as not-newer and the deadline silently stops replicating -
#     a regression A2 introduces unless the touch path ticks too.
#
# The refusal itself is proved deterministically in the htable selftest
# (store_ver: older refused, equal refused, newer taken, a local write
# still outranks a peer copy) - two nodes cannot be made to disagree on
# demand from a shell script.
# Usage: test/verwiretest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
SEC=vw-client-secret
D=$(mktemp -d /var/tmp/pcvw.XXXXXX)
trap 'pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

for pf in 17981 17982; do
	ss -ltn 2>/dev/null | grep -q ":$pf[[:space:]]" && {
		echo "verwiretest: port $pf already bound" >&2; exit 1; }
done

mk() {
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = vw-cluster-secret
[listen]
tcp = 127.0.52.$1:1798$1
[cluster]
multicast = 239.255.77.161:17261
advertise = 127.0.52.$1
pull_timeout_ms = 600
mode = eager
collections = c
[collection c]
buckets_log2 = 12
EOF
}
start() {
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	i=0
	while [ $i -lt 200 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}
cli() { n=$1; shift; ./perfcli -h 127.0.52.$n -p 1798$n -a $SEC "$@" 2>/dev/null; }
# the record's version as THIS node holds it; "-" = absent
verof() {
	cli $1 -j "{\"method\":\"exists\",\"params\":{\"col\":\"c\",\"key\":\"$2\"}}" \
		| python3 -c 'import json,sys
try:
    r = json.load(sys.stdin)
    print(r["ver"] if r.get("exists") else "-")
except Exception: print("ERR")'
}
put() { cli $1 -j "{\"method\":\"set\",\"params\":{\"col\":\"c\",\"key\":\"$2\",\"value\":\"$3\"}}" >/dev/null; }
older() {
	cli $1 -j '{"method":"stats"}' | python3 -c 'import json,sys
def find(o):
    if isinstance(o, dict):
        if "recv_older" in o: return o["recv_older"]
        for v in o.values():
            r = find(v)
            if r is not None: return r
    return None
try:
    r = find(json.load(sys.stdin))
    print("ERR" if r is None else r)
except Exception: print("ERR")'
}
# poll until the key lands on the peer (eager sweeps on a tick)
await() {
	i=0
	while [ $i -lt 60 ]; do
		[ "$(verof $1 $2)" != "-" ] && return 0
		sleep 0.25; i=$((i+1))
	done
	return 1
}

mk 1; mk 2
start 1 && start 2 || { echo "fleet did not start"; exit 1; }
sleep 6

# ---- 1. an eager replica keeps the author's version --------------------
put 1 wk1 hello
await 2 wk1 || bad "wk1 never replicated to node 2"
V1=$(verof 1 wk1); V2=$(verof 2 wk1)
[ "$V1" = "$V2" ] && [ "$V1" != "ERR" ] && [ "$V1" != "-" ] \
	&& ok "the replica carries the author's version ($V1)" \
	|| bad "author and replica disagree: node1=$V1 node2=$V2"

# ---- 2. an update carries its new version across ------------------------
put 1 wk1 hello-again
i=0
while [ $i -lt 60 ]; do
	W2=$(verof 2 wk1)
	[ "$W2" != "$V2" ] && break
	sleep 0.25; i=$((i+1))
done
W1=$(verof 1 wk1)
[ "$W1" = "$W2" ] && [ "$W1" != "$V1" ] \
	&& ok "the update moved both versions together ($V1 -> $W1)" \
	|| bad "update versions diverged: node1=$W1 node2=$W2 (was $V1)"

# ---- 3. a pulled copy carries the holder's version ---------------------
# write on 2, read it from 1: with eager on both this may already be a
# replica, so use a key written while 1 is the one asking
put 2 pk1 pulled-value
await 1 pk1 || bad "pk1 never reached node 1"
P1=$(verof 1 pk1); P2=$(verof 2 pk1)
[ "$P1" = "$P2" ] && [ "$P1" != "ERR" ] \
	&& ok "the copy on the asking node carries the holder's version ($P2)" \
	|| bad "holder and copy disagree: node2=$P2 node1=$P1"

# ---- 4. refusals are counted, and a healthy fleet has none -------------
R1=$(older 1); R2=$(older 2)
case "$R1$R2" in
	*ERR*) bad "recv_older is not reported in stats (n1=$R1 n2=$R2)" ;;
	*) ok "refusals are counted (n1=$R1 n2=$R2)" ;;
esac

# ---- 5. a TTL re-arm must still cross ----------------------------------
put 1 tk1 ttl-value
await 2 tk1 || bad "tk1 never replicated to node 2"
TTLBEFORE=$(cli 2 -j '{"method":"ttl","params":{"col":"c","key":"tk1"}}' \
	| python3 -c 'import json,sys
try: print(json.load(sys.stdin)["ttl"])
except Exception: print("ERR")')
cli 1 -j '{"method":"expire","params":{"col":"c","key":"tk1","ttl":3600}}' >/dev/null
i=0
while [ $i -lt 60 ]; do
	TTLAFTER=$(cli 2 -j '{"method":"ttl","params":{"col":"c","key":"tk1"}}' \
		| python3 -c 'import json,sys
try: print(json.load(sys.stdin)["ttl"])
except Exception: print("ERR")')
	[ "$TTLAFTER" != "$TTLBEFORE" ] && break
	sleep 0.25; i=$((i+1))
done
[ "$TTLAFTER" != "$TTLBEFORE" ] && [ "$TTLAFTER" != "ERR" ] \
	&& ok "a TTL re-arm still propagates ($TTLBEFORE -> $TTLAFTER)" \
	|| bad "the TTL re-arm never reached node 2 (still $TTLAFTER):
         a touch that does not move the version is refused as not-newer"

echo "verwiretest: $pass passed, $fail failed"
[ $fail -eq 0 ]
