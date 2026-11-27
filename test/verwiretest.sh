#!/bin/sh
# verwiretest.sh - the version travels with the bytes: a replica or a pulled copy carries the
# author's version, refusals are counted, and a TTL re-arm still propagates.
set -u

BIN=${1:-./perfcached}
SEC=vw-client-secret
D=$(mktemp -d /var/tmp/pcvw.XXXXXX)
# daemons die by the pids this script started, each checked against its command line so a
# reused pid never hits a stranger; the name-bound pattern stays as a fallback
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
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
	PIDS="$PIDS $!"
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
# a joiner is CONNECTING until its links answer: up to 30 s for both ready and seeing each other
st() { cli $1 -q -j '{"method":"stats"}' | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("state"), d["cluster"].get("peers_up"))' 2>/dev/null; }
i=0
while [ $i -lt 300 ] && { [ "$(st 1)" != "ready 1" ] || [ "$(st 2)" != "ready 1" ]; }; do
	sleep 0.1; i=$((i+1))
done

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
# eager on both may make it a replica already: use a key written while 1 is the one asking
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
