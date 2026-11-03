#!/bin/sh
# sealbatchtest.sh - S468 B: libperfd seals once per flush, not once per
# request.
#
# An async handle staged each request as a sealed record of its own, so a
# pipeline of 32 SETs was 32 seals in the client and 32 opens in the
# server - libsodium took 39.7% of the server's CPU on encrypted 32 B SETs
# (OM-4, 207).  Now the plaintext waits in the handle and is sealed when the
# buffer is pushed.  The server counts the records it opens
# (/stats native.binary.records) beside the requests they carried
# (native.binary.requests): one per request before, far fewer now.
#   1. an encrypted natbench pipeline (depth 32) for 3 s: requests flowed,
#      the counter moved (it counts), and at least 4 requests rode a record
#      on average.
# usage: test/sealbatchtest.sh [./perfcached] [./natbench]
set -u
BIN=${1:-./perfcached}
NB=${2:-./natbench}
PORT=${PORT:-17875}
HPORT=${HPORT:-17876}
D=$(mktemp -d /var/tmp/pcsealb.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL $1"; }
REV=$("$BIN" -V 2>/dev/null | sed -n 's/.*(\(.*\)).*/\1/p')
[ -z "$REV" ] || [ "$REV" = unknown ] && { echo "sealbatchtest: build cannot name itself"; exit 2; }
for pt in $PORT $HPORT; do
	ss -ltn 2>/dev/null | grep -q ":$pt[[:space:]]" && { echo "sealbatchtest: port $pt already bound" >&2; exit 1; }
done

cat > "$D/n.conf" <<CFG
[daemon]
workers = 2
[memory]
arena_mb = 128
[secrets]
client = sealbatch-client-secret
[listen]
tcp = 127.0.0.1:$PORT
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
grep -q "perfcached ready" "$D/n.log" || { echo "sealbatchtest: the node did not start"; tail -3 "$D/n.log"; exit 1; }

echo "=== sealbatchtest ($REV) ==="
NB_ENCRYPT=1 "$NB" 127.0.0.1 "$PORT" sealbatch-client-secret c 8 2 32 3 10000 32 0 1 50 0 0 > "$D/nb.out" 2>&1
sleep 2                                # the door totals widen once a second
set -- $(curl -s -m 5 "http://127.0.0.1:$HPORT/stats" | python3 -c "import json,sys
try:
    b=json.load(sys.stdin)['native']['binary']; print(b['requests'], b.get('records', -1))
except Exception: print('-1 -1')")
echo "  natbench: $(grep -m1 -oE '[0-9]+ ops/s' "$D/nb.out"); server: requests=$1 records=$2"
[ "$1" -gt 1000 ] 2>/dev/null && ok "1. requests flowed ($1)" || bad "1. requests $1 - the pipeline never ran"
[ "$2" -gt 0 ] 2>/dev/null && ok "1. the record counter counts ($2)" || bad "1. records $2 - the counter never moved, so the ratio proves nothing"
[ "$2" -gt 0 ] 2>/dev/null && [ $(( $1 / $2 )) -ge 4 ] \
	&& ok "1. $(( $1 / $2 )) requests a record on average - sealed per flush" \
	|| bad "1. $1 requests in $2 records - a seal per request (want 4 or more a record)"
kill $P 2>/dev/null; wait $P 2>/dev/null; P=

echo "sealbatchtest: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
