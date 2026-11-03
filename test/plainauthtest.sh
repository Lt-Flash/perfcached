#!/bin/sh
# plainauthtest.sh - S468 A: the native door authenticates with the Noise
# handshake and, by default ([listen] encrypt = no), carries the requests in
# the clear: libperfd asks in msg1, the node grants it in msg2, the client's
# first record (the switch) proves it holds the session keys.
#   1. with the right secret (perfcli blocking, natbench async): requests
#      flow, no record is opened past the switch, the connections are counted
#      plaintext, and the start says the door is readable on the network;
#   2. a wrong secret is refused as before, and said;
#   3. a client that does not ask (NB_ENCRYPT) is served encrypted there;
#   4. a listener with encrypt=yes grants nothing: the same clients are
#      served encrypted (perfcli and natbench alike);
#   5. enable over the plaintext connection is refused - the privilege
#      secret never crosses in the clear - and served over --encrypt.
# usage: test/plainauthtest.sh [./perfcached] [./perfcli] [./natbench]
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
NB=${3:-./natbench}
PORT=${PORT:-17877}
PORT2=${PORT2:-17878}
HPORT=${HPORT:-17879}
D=$(mktemp -d /var/tmp/pcplain.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL $1"; }
REV=$("$BIN" -V 2>/dev/null | sed -n 's/.*(\(.*\)).*/\1/p')
[ -z "$REV" ] || [ "$REV" = unknown ] && { echo "plainauthtest: build cannot name itself"; exit 2; }
for pt in $PORT $PORT2 $HPORT; do
	ss -ltn 2>/dev/null | grep -q ":$pt[[:space:]]" && { echo "plainauthtest: port $pt already bound" >&2; exit 1; }
done

cat > "$D/n.conf" <<CFG
[daemon]
workers = 2
[memory]
arena_mb = 128
[secrets]
client = pa-client-secret
enable = pa-enable-secret
[listen]
tcp = 127.0.0.1:$PORT
tcp = 127.0.0.1:$PORT2 encrypt=yes
http = 127.0.0.1:$HPORT
plaintext = never
[collection c]
buckets_log2 = 14
CFG
chmod 640 "$D/n.conf"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
P=$!
k=0
while [ $k -lt 100 ]; do
	grep -q "perfcached ready" "$D/n.log" && break
	sleep 0.1; k=$((k+1))
done
grep -q "perfcached ready" "$D/n.log" || { echo "plainauthtest: the node did not start"; tail -3 "$D/n.log"; exit 1; }

# conns refused records requests: the door totals widen once a second.  Over
# HTTP - a native connection to ask would count in the figures it reads
door() {
	sleep 1.5
	curl -s -m 5 "http://127.0.0.1:$HPORT/stats" |
	python3 -c "import json,sys
try:
    n=json.load(sys.stdin)['native']; p=n['plain']; b=n['binary']
    print(p['conns'], p['refused'], b.get('records', -1), b['requests'])
except Exception: print('-1 -1 -1 -1')"
}
nb() { # nb <port> [env] - 8 connections, 2 s of 32 B SETs; prints ops/s
	env ${2:-} "$NB" 127.0.0.1 "$1" pa-client-secret c 8 2 32 2 10000 32 0 1 50 0 0 > "$D/nb.out" 2>&1
	grep -m1 -oE '[0-9]+ ops/s' "$D/nb.out" | cut -d' ' -f1
}

echo "=== plainauthtest ($REV) ==="

# 1. plaintext after the handshake, served
R=$("$CLI" -h 127.0.0.1 -p "$PORT" -a pa-client-secret -q -j '{"method":"ping"}' 2>&1)
echo "$R" | grep -q pong && ok "1. perfcli with the client secret is served" \
	|| bad "1. perfcli with the client secret: $R"
set -- $(door); C0=$1; RC0=$3
OPS=$(nb "$PORT")
set -- $(door)
echo "  plain:    natbench ${OPS:-?} ops/s; plain conns $C0 -> $1, records $RC0 -> $3, requests $4"
[ "${OPS:-0}" -gt 1000 ] 2>/dev/null && ok "1. natbench (async) served in the clear" \
	|| bad "1. natbench in the clear: ${OPS:-no} ops/s ($(tail -2 "$D/nb.out" | tr '\n' ' '))"
[ "$1" -ge $((C0 + 8)) ] 2>/dev/null && [ "$3" = "$RC0" ] \
	&& ok "1. its connections counted plaintext, no record past the switch" \
	|| bad "1. plain conns $C0 -> $1 (want +8), records $RC0 -> $3 (want no change)"
grep -q "native door 127.0.0.1:$PORT: authenticated, not encrypted" "$D/n.log" \
	&& ! grep -q "native door 127.0.0.1:$PORT2:" "$D/n.log" \
	&& ok "1. the start says the default door is readable on the network, not the encrypt=yes one" \
	|| bad "1. start notices: $(grep -c 'native door' "$D/n.log") ($(grep 'native door' "$D/n.log" | head -2 | tr '\n' ' '))"

# 2. a wrong secret
set -- $(door); F0=$2
R=$("$CLI" -h 127.0.0.1 -p "$PORT" -a not-the-secret -q -j '{"method":"ping"}' 2>&1)
set -- $(door)
! echo "$R" | grep -q pong && [ "$2" = "$F0" ] \
	&& grep -q "handshake: authentication failed" "$D/n.log" \
	&& ok "2. a wrong secret is refused at the handshake, and said" \
	|| bad "2. a wrong secret: '$R', switch refusals $F0 -> $2, log $(grep -c 'authentication failed' "$D/n.log")"

# 3. a client that does not ask, on the default door
set -- $(door); C0=$1; RC0=$3
OPS=$(nb "$PORT" NB_ENCRYPT=1)
set -- $(door)
[ "${OPS:-0}" -gt 1000 ] 2>/dev/null && [ "$3" -gt "$RC0" ] 2>/dev/null && [ "$1" = "$C0" ] \
	&& ok "3. NB_ENCRYPT: served encrypted on the default door (${OPS} ops/s, records $RC0 -> $3)" \
	|| bad "3. NB_ENCRYPT: ${OPS:-no} ops/s, records $RC0 -> $3, plain conns $C0 -> $1"

# 4. encrypt=yes grants nothing
set -- $(door); C0=$1; RC0=$3
R=$("$CLI" -h 127.0.0.1 -p "$PORT2" -a pa-client-secret -q -j '{"method":"ping"}' 2>&1)
OPS=$(nb "$PORT2")
set -- $(door)
echo "$R" | grep -q pong && ok "4. encrypt=yes: perfcli (blocking) served encrypted" \
	|| bad "4. encrypt=yes: perfcli $R"
[ "${OPS:-0}" -gt 1000 ] 2>/dev/null && [ "$3" -gt "$RC0" ] 2>/dev/null && [ "$1" = "$C0" ] \
	&& ok "4. encrypt=yes: natbench (async) served encrypted (${OPS} ops/s, records $RC0 -> $3, no plain conn)" \
	|| bad "4. encrypt=yes: natbench ${OPS:-no} ops/s, records $RC0 -> $3, plain conns $C0 -> $1"

# 5. enable needs Noise
R=$("$CLI" -h 127.0.0.1 -p "$PORT" -a pa-client-secret -q \
	-j '{"method":"enable","params":{"secret":"pa-enable-secret"}}' 2>&1)
echo "$R" | grep -q "enable needs an encrypted connection" \
	&& ok "5. enable over the plaintext connection - refused, said why" || bad "5. enable over plaintext: $R"
R=$("$CLI" -h 127.0.0.1 -p "$PORT" -a pa-client-secret --encrypt -q \
	-j '{"method":"enable","params":{"secret":"pa-enable-secret"}}' 2>&1)
echo "$R" | grep -q '"privileged": *true' && ok "5. enable over --encrypt - privileged" \
	|| bad "5. enable over --encrypt: $R"
kill $P 2>/dev/null; wait $P 2>/dev/null; P=

echo "plainauthtest: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
