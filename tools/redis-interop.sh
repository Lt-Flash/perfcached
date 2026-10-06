#!/bin/sh
# redis-interop.sh - S331 read-through against a REAL Redis (DESIGN 12ix).
#
# The suites use test/fakeupstream.py; this runs the same paths against
# the redis:8 image in a THROWAWAY local container (podman, or docker),
# removed on exit - never point it at a shared Redis.  Not in CI (no image
# pulls there); run it before a release that touches read-through:
#
#   tools/redis-interop.sh [./perfcached] [image]
#
# It connects as a read-only ACL user (the GUIDE's line, verbatim), reads
# a string, a hash, a RedisJSON document and a list through, promotes an
# INCR and a DEL, changes a key in Redis and waits for the invalidation,
# runs the drain sweep (updrain) and reads the swept keys after FLUSHALL
# on the Redis side, then proves the one-way rule two ways: MONITOR shows
# perfcached sending only the handshake and reads, and ACL LOG shows
# nothing refused.
set -u
BIN=$(readlink -f "${1:-./perfcached}")
PYTHONPATH=$(cd "$(dirname "$0")/../test" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
IMG=${2:-docker.io/library/redis:8}
ENG=
for e in podman docker; do command -v $e >/dev/null 2>&1 && { ENG=$e; break; }; done
[ -n "$ENG" ] || { echo "redis-interop: SKIPPED - neither podman nor docker"; exit 0; }
RP=18780; RESP=18781; HTTP=18782; TCP=18783
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
D=$(mktemp -d /var/tmp/pcinterop.XXXXXX)
PID= MPID=
cleanup() { [ -n "$PID" ] && kill $PID 2>/dev/null; [ -n "$MPID" ] && kill $MPID 2>/dev/null; $ENG rm -f s331-redis >/dev/null 2>&1; rm -rf "$D"; }
trap cleanup EXIT INT TERM
$ENG rm -f s331-redis >/dev/null 2>&1
$ENG run -d --rm --name s331-redis -p 127.0.0.1:$RP:6379 "$IMG" \
	redis-server --save "" --appendonly no > /dev/null || { echo "cannot start redis:8"; exit 1; }
k=0; until $ENG exec s331-redis redis-cli PING 2>/dev/null | grep -q PONG; do sleep 0.2; k=$((k+1)); [ $k -gt 50 ] && exit 1; done
R() { $ENG exec s331-redis redis-cli "$@"; }
echo "redis: $(R INFO server | grep -m1 redis_version | tr -d '\r')"
# the GUIDE's read-only user, exactly as written there
R ACL SETUSER reader on '>rpw-s331' '~*' +@read +hello +auth +select +ping '+client|id' '+client|tracking' >/dev/null
R SET tok:a alpha EX 120 >/dev/null
R HSET tok:h f1 v1 f2 v2 >/dev/null
R JSON.SET tok:j '$' '{"x":1,"y":[true,null]}' >/dev/null
R RPUSH tok:l a b >/dev/null
R SET tok:cnt 41 >/dev/null
R SET tok:d dval >/dev/null
R SET tok:t1 t1a >/dev/null
R SET other:z nope >/dev/null
R SET tok:s1 s1v EX 300 >/dev/null
R HSET tok:sh a x >/dev/null
R JSON.SET tok:sj '$' '{"k":2}' >/dev/null
R SADD tok:ss m >/dev/null
$ENG exec s331-redis redis-cli MONITOR > "$D/monitor.log" 2>&1 &
MPID=$!
sleep 0.3

cat > "$D/rc.py" <<'PY'
import socket, sys
def enc(cmd):
    parts = cmd.split(" ")
    return b"*%d\r\n" % len(parts) + b"".join(b"$%d\r\n%s\r\n" % (len(p), p.encode()) for p in parts)
def rd(f):
    l = f.readline()
    t, rest = l[:1], l[1:-2]
    if t in (b"+", b"-"): return (t + rest).decode()
    if t == b":": return int(rest)
    if t == b"$":
        n = int(rest)
        return None if n < 0 else f.read(n + 2)[:-2].decode("latin-1")
    if t == b"*":
        n = int(rest)
        return None if n < 0 else [rd(f) for _ in range(n)]
    return "?" + l.decode("latin-1")
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10)
s.sendall(b"".join(enc(c) for c in sys.argv[2:]))
f = s.makefile("rb")
for _ in sys.argv[2:]:
    print(repr(rd(f)))
PY
mkdir -p "$D/state/wal"
cat > "$D/n.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = interop-client-secret
upstream = rpw-s331
enable = interop-enable-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
plaintext = loopback
[wal]
dir = $D/state/wal
probe = no
fsync = everysec
segment_mb = 8
[collection 0]
buckets_log2 = 10
upstream = redis://reader@127.0.0.1:$RP/0
upstream_prefixes = tok:*
upstream_timeout_ms = 2000
C
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
PID=$!
k=0; until grep -q "perfcached ready" "$D/n.log"; do sleep 0.1; k=$((k+1)); [ $k -gt 100 ] && { cat "$D/n.log"; exit 1; }; done
stat() { python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats",timeout=5).read())
print([c for c in r["collections"] if c["name"]=="0"][0]["upstream"][sys.argv[1]])' "$1" 2>/dev/null; }
k=0; while [ "$(stat tracking)" != True ] && [ $k -lt 50 ]; do sleep 0.1; k=$((k+1)); done
[ "$(stat tracking)" = True ] && ok "tracking on against redis:8, as the read-only ACL user" \
	|| bad "tracking: $(stat tracking); $(grep -iE 'tracking|upstream|AUTH' "$D/n.log" | tail -2)"
rc() { python3 "$D/rc.py" $RESP "$@" 2>&1; }
[ "$(rc 'GET tok:a')" = "'alpha'" ] && ok "a string" || bad "GET tok:a: $(rc 'GET tok:a')"
T=$(rc 'TTL tok:a'); [ "$T" -ge 110 ] 2>/dev/null && [ "$T" -le 121 ] && ok "with redis's TTL ($T s)" || bad "TTL tok:a: $T"
[ "$(rc 'HGETALL tok:h')" = "['f1', 'v1', 'f2', 'v2']" ] && ok "a hash, in redis's field order" || bad "HGETALL tok:h: $(rc 'HGETALL tok:h')"
J=$(rc 'JSON.GET tok:j')
python3 -c 'import json,sys,ast; sys.exit(0 if json.loads(ast.literal_eval(sys.argv[1]))=={"x":1,"y":[True,None]} else 1)' "$J" 2>/dev/null \
	&& ok "a RedisJSON document ($J)" || bad "JSON.GET tok:j: $J"
case "$(rc 'GET tok:l')" in *WRONGTYPE*) ok "a list answers WRONGTYPE" ;; *) bad "GET tok:l: $(rc 'GET tok:l')" ;; esac
[ "$(rc 'GET other:z')" = None ] && ok "outside the allow-list: nil" || bad "GET other:z: $(rc 'GET other:z')"
X=$(rc 'INCR tok:cnt' 'GET tok:cnt' | tr '\n' ' ')
[ "$X" = "42 '42' " ] && ok "INCR of redis's 41 promotes and answers 42" || bad "INCR tok:cnt: $X"
X=$(rc 'DEL tok:d' 'GET tok:d' | tr '\n' ' ')
[ "$X" = "1 None " ] && ok "DEL of a redis-only key: 1, then gone" || bad "DEL tok:d: $X"
[ "$(R GET tok:d | tr -d '\r')" = dval ] && [ "$(R GET tok:cnt | tr -d '\r')" = 41 ] \
	&& ok "redis itself unchanged (tok:d still dval, tok:cnt still 41)" || bad "redis changed: tok:d=$(R GET tok:d) tok:cnt=$(R GET tok:cnt)"
I0=$(stat invalidations)
A=$(rc 'GET tok:t1')
R SET tok:t1 t1b >/dev/null
k=0; B=""; while [ $k -lt 30 ]; do B=$(rc 'GET tok:t1'); [ "$B" = "'t1b'" ] && break; sleep 0.1; k=$((k+1)); done
[ "$A" = "'t1a'" ] && [ "$B" = "'t1b'" ] && ok "redis changes tok:t1: the invalidation arrives, the next read has t1b ($((k * 100)) ms)" \
	|| bad "invalidation: $A then $B (invalidations $I0 -> $(stat invalidations))"
# the drain sweep: SCAN as the read-only user, the fleet's keys kept
dr() { python3 -c 'import json,socket,sys,pcnative
f=pcnative.wrap(socket.create_connection(("127.0.0.1",int(sys.argv[1])),10))
pcnative.call(f,"enable",secret="interop-enable-secret")
r=pcnative.call(f,"updrain",col="0")
print(json.dumps(r.get("result", r.get("error"))))' $TCP 2>&1; }
dstat() { python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats",timeout=5).read())
print([c for c in r["collections"] if c["name"]=="0"][0]["upstream"]["drain"][sys.argv[1]])' "$1" 2>/dev/null; }
X=$(dr)
k=0; while [ "$(dstat state)" = running ] && [ $k -lt 100 ]; do sleep 0.2; k=$((k+1)); done
ST="$(dstat state) read=$(dstat read) taken=$(dstat taken_over) fleet_had=$(dstat fleet_had) wrongtype=$(dstat wrongtype) errors=$(dstat errors)"
case "$X" in *started*) [ "$(dstat state)" = done ] && [ "$(dstat taken_over)" -ge 7 ] && [ "$(dstat fleet_had)" -ge 2 ] \
	&& [ "$(dstat wrongtype)" = 2 ] && [ "$(dstat errors)" = 0 ] && ok "updrain against redis:8 is done ($ST)" \
	|| bad "the sweep: $ST" ;;
*) bad "updrain: $X" ;; esac
R FLUSHALL >/dev/null
sleep 0.3
X=$(rc 'GET tok:s1' 'HGETALL tok:sh' 'JSON.GET tok:sj' 'GET tok:t1' 'GET tok:cnt' 'GET tok:d' | tr '\n' ' ')
T=$(rc 'TTL tok:s1')
[ "$X" = "'s1v' ['a', 'x'] '{\"k\":2}' 't1b' '42' None " ] && [ "$T" -ge 280 ] 2>/dev/null && [ "$T" -le 301 ] \
	&& ok "redis flushed: the swept keys answer from the collection (tok:s1 TTL $T s), the fleet's kept" \
	|| bad "after the sweep and FLUSHALL: $X (TTL $T)"
sleep 0.5
kill $MPID 2>/dev/null; MPID=
# perfcached's connections: every command the monitor saw that the seeding
# (redis-cli inside the container, 127.0.0.1) did not send
python3 - "$D/monitor.log" <<'P' > "$D/mon.out"
import re, sys
reads = {"HELLO","AUTH","SELECT","PING","TYPE","PTTL","GET","HGETALL","JSON.GET","CLIENT","SCAN"}
seen, bad = {}, []
for l in open(sys.argv[1], errors="replace"):
    m = re.match(r'^\S+ \[\d+ ([^\]]+)\] "([^"]+)"', l)
    if not m or m.group(1).startswith("127.0.0.1:"):
        continue
    c = m.group(2).upper()
    seen[c] = seen.get(c, 0) + 1
    if c not in reads:
        bad.append(l.strip()[:120])
print(" ".join("%s=%d" % kv for kv in sorted(seen.items())))
print("BAD " + " | ".join(bad[:5]) if bad else "NOBAD")
P
SEEN=$(sed -n 1p "$D/mon.out"); B2=$(sed -n 2p "$D/mon.out")
[ -n "$SEEN" ] && [ "$B2" = NOBAD ] && ok "MONITOR: perfcached sent only reads and the handshake ($SEEN)" \
	|| bad "MONITOR: $SEEN / $B2"
AL=$(R ACL LOG | grep -c . )
[ "$AL" = 0 ] && ok "ACL LOG empty: the read-only user was never refused anything" || bad "ACL LOG has $AL line(s): $(R ACL LOG | head -12 | tr '\n' ' ')"
grep -qE " (ERROR|CRIT)" "$D/n.log" && bad "ERROR/CRIT in the node log: $(grep -m1 -E ' (ERROR|CRIT)' "$D/n.log" | cut -c1-120)" || ok "no ERROR or CRIT in the node log"
echo "interop: $pass passed, $fail failed"
rm -rf "$D"
[ $fail -eq 0 ]
