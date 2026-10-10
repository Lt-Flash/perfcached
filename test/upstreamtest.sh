#!/bin/sh
# upstreamtest.sh - S331: read-through from a RESP upstream (DESIGN 12ix).
#
# One node whose collection 0 names an upstream: test/fakeupstream.py, a
# scripted RESP server standing in for Redis (none on the runners - and it
# FAILS the suite if perfcached ever sends it a write).  Over the RESP door:
#   1  a string, a hash and a JSON document come through on a local miss,
#      with the upstream's TTL; a list answers WRONGTYPE, as Redis would
#   2  only the allow-list falls through; an absent key is remembered
#      (one fetch for two misses inside upstream_negative_ms)
#   3  single-flight: ten clients missing one slow key cause ONE fetch,
#      and every one of them gets the value
#   4  a pipeline whose first command waits on a fetch answers IN ORDER
#   5  a plain SET is local and does not wait; a background ownership
#      check asks the upstream once, and the local value wins
#   6  an upstream that is down answers an error (upstream_on_error=error),
#      and the node comes back when it does
#   7  nothing was ever written upstream; the stats carry the upstream block
#   8  (phase B) a mutation PROMOTES the fetched value first: INCR of an
#      upstream counter, HSET into an upstream hash
#   9  a key this node wrote or deleted never reads through again: DEL of
#      a key only the upstream had, SET-then-DEL, an owned key expiring
#  10  the no-fall entries and promoted records survive a clean restart
#  11  (phase C) the native doors: CMD get/add/del/hcmd and the binary
#      GET/ADD verbs read through and promote the same way; two binary
#      frames in flight, the first waiting on a fetch, each answered
#      under its own id
#  12  (phase D) tracking: an invalidation from the upstream drops the
#      fetched copy (the next read fetches the new value); losing the
#      tracking connection drops every fetched copy; tracking comes back
#  13  the drain sweep (updrain, privileged): every allowed key the upstream
#      holds is taken over unless the fleet has it - then the upstream is
#      switched off and the keys still answer, with their TTLs; the
#      fleet's own versions (written, deleted, promoted) are kept
#  14  one server: a connection that reached another server than the
#      tracking connection (a round-robin proxy) is not tracked - its
#      copy lives upstream_negative_ms, and a change the upstream never
#      announces is read after it; an upstream that will not say which
#      server it is (INFO refused) gets no tracking at all
# FAIL-FIRST: a build without S331 refuses `upstream` in the config.
# Usage: test/upstreamtest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=$(readlink -f "${1:-./perfcached}")
T=$(cd "$(dirname "$0")" && pwd)
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
UP=18760 TCP=18761 RESP=18762 HTTP=18763
for p in $UP $TCP $RESP $HTTP; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "upstreamtest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcup.XXXXXX)
PID= FPID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; [ -n "$FPID" ] && kill -9 $FPID 2>/dev/null; rm -rf "$D"' EXIT INT TERM

mkdir -p "$D/up" "$D/state/wal"
cat > "$D/up/data.json" <<'J'
{"tok:a": {"type": "string", "value": "alpha", "ttl_ms": 60000},
 "tok:h": {"type": "hash", "value": {"f1": "v1", "f2": "v2"}},
 "tok:j": {"type": "json", "value": {"x": 1, "y": [true, null]}},
 "tok:l": {"type": "list", "value": ["a", "b"]},
 "tok:sf": {"type": "string", "value": "sfv"},
 "tok:p": {"type": "string", "value": "pv"},
 "tok:new": {"type": "string", "value": "upstream-old"},
 "tok:down": {"type": "string", "value": "dv"},
 "tok:cnt": {"type": "string", "value": "41"},
 "tok:h2": {"type": "hash", "value": {"a": "1"}},
 "tok:d2": {"type": "string", "value": "dd"},
 "tok:o": {"type": "string", "value": "oo"},
 "tok:e": {"type": "string", "value": "ee"},
 "tok:n1": {"type": "string", "value": "nv1"},
 "tok:n2": {"type": "string", "value": "5"},
 "tok:n3": {"type": "string", "value": "n3v"},
 "tok:nh": {"type": "hash", "value": {"f": "hv"}},
 "tok:b1": {"type": "string", "value": "bv1"},
 "tok:b2": {"type": "string", "value": "10"},
 "tok:bslow": {"type": "string", "value": "slowv"},
 "tok:t1": {"type": "string", "value": "t1a"},
 "tok:t2": {"type": "string", "value": "t2a"},
 "tok:dr1": {"type": "string", "value": "d1", "ttl_ms": 90000},
 "tok:dr2": {"type": "hash", "value": {"a": "x"}},
 "tok:dr3": {"type": "json", "value": {"k": 2}},
 "tok:dr4": {"type": "list", "value": ["l"]},
 "tok:dr5": {"type": "string", "value": "d5"},
 "other:z": {"type": "string", "value": "not-allowed"}}
J
echo '{"password": "up-pass-1", "resp3": true}' > "$D/up/ctl.json"
python3 "$T/fakeupstream.py" $UP "$D/up" > "$D/fake.log" 2>&1 &
FPID=$!
i=0; while [ ! -f "$D/up/ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done

cat > "$D/n.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = up-client-secret
upstream = up-pass-1
enable = up-enable-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
# SELECT 7 is collection 0 too: case 15 reads through under a mapped index
resp_collections = 0, 7:0
http = 127.0.0.1:$HTTP
plaintext = loopback
[wal]
dir = $D/state/wal
probe = no
fsync = everysec
segment_mb = 8
[collection 0]
buckets_log2 = 10
autoscale = floor
upstream = redis://127.0.0.1:$UP
upstream_prefixes = tok:*
upstream_timeout_ms = 2000
upstream_negative_ms = 3000
C
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
PID=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break
	kill -0 $PID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
if ! grep -q "perfcached ready" "$D/n.log"; then
	bad "the node did not start with an upstream: $(grep -m1 -E 'CRIT|ERR|unknown' "$D/n.log" | cut -c1-140)"
	echo "upstreamtest: $pass passed, $fail failed"
	exit 1
fi
grep -q "read-through from 127.0.0.1:$UP" "$D/n.log" && ok "the node starts read-through, and says so" \
	|| bad "no read-through line in the log"
# tracking first: a copy fetched before it is up lives only
# upstream_negative_ms, and case 1 checks the upstream's TTL
stat() { python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats",timeout=5).read())
u=[c for c in r["collections"] if c["name"]=="0"][0]["upstream"]
print(u[sys.argv[1]])' "$1" 2>/dev/null; }
k=0; while [ "$(stat tracking)" != True ] && [ $k -lt 40 ]; do sleep 0.1; k=$((k+1)); done
[ "$(stat tracking)" = True ] && ok "tracking is on (the invalidation connection is up)" \
	|| bad "tracking never came on: $(stat tracking); $(grep -m1 -i tracking "$D/n.log" | cut -c1-120)"
# 0.5.6.1: the companion is sized as its parent - buckets AND autoscale
# (an empty 0.nofall under the default auto shrank to 2^12 after a minute)
NF=$(python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats",timeout=5).read())
c=[c for c in r["collections"] if c["name"]=="0.nofall"]
print(("%s %s" % (c[0].get("autoscale"), c[0].get("autoscale_floor_log2"))) if c else "none")' 2>/dev/null)
[ "$NF" = "floor 10" ] && ok "the companion 0.nofall inherits its parent's autoscale: floor at 2^10" \
	|| bad "0.nofall autoscale: $NF (want: floor 10, as collection 0)"

# rc <cmd> [<cmd> ...]: one connection, the commands PIPELINED in one
# write, one line per reply (RESP2 decoded, printed as Python literals)
cat > "$D/rc.py" <<'P'
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
        if n < 0: return None
        v = f.read(n + 2)[:-2]
        return v.decode("latin-1")
    if t == b"*":
        n = int(rest)
        return None if n < 0 else [rd(f) for _ in range(n)]
    return "?" + l.decode("latin-1")
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10)
s.sendall(b"".join(enc(c) for c in sys.argv[2:]))
f = s.makefile("rb")
for _ in sys.argv[2:]:
    print(repr(rd(f)))
P
rc() { python3 "$D/rc.py" $RESP "$@" 2>&1; }
cnt() { n=$(grep -c "^$1 $2\$" "$D/up/counts" 2>/dev/null); echo "${n:-0}"; }

echo "--- 1: types through the door"
[ "$(rc 'GET tok:a')" = "'alpha'" ] && ok "a string comes through on a miss" || bad "GET tok:a: $(rc 'GET tok:a')"
TT=$(rc 'TTL tok:a')
[ "$TT" -ge 55 ] 2>/dev/null && [ "$TT" -le 61 ] && ok "with the upstream's TTL ($TT s)" || bad "TTL tok:a = $TT (want ~60)"
[ "$(rc 'HGETALL tok:h')" = "['f1', 'v1', 'f2', 'v2']" ] && ok "a hash comes through, fields in order" \
	|| bad "HGETALL tok:h: $(rc 'HGETALL tok:h')"
[ "$(rc 'HGET tok:h f2')" = "'v2'" ] && ok "and answers HGET" || bad "HGET tok:h f2: $(rc 'HGET tok:h f2')"
J=$(rc 'JSON.GET tok:j')
python3 -c 'import json,sys,ast; v=json.loads(ast.literal_eval(sys.argv[1])); sys.exit(0 if v=={"x":1,"y":[True,None]} else 1)' "$J" 2>/dev/null \
	&& ok "a JSON document comes through ($J)" || bad "JSON.GET tok:j: $J"
case "$(rc 'GET tok:l')" in *WRONGTYPE*) ok "a list answers WRONGTYPE, as Redis does" ;;
	*) bad "GET tok:l: $(rc 'GET tok:l')" ;; esac

echo "--- 2: the allow-list and the negative cache"
[ "$(rc 'GET other:z')" = "None" ] && [ "$(cnt TYPE other:z)" = 0 ] \
	&& ok "a key outside upstream_prefixes is not fetched (nil, nothing asked)" \
	|| bad "other:z: $(rc 'GET other:z'), fetched $(cnt TYPE other:z) time(s)"
A1=$(rc 'GET tok:none'); A2=$(rc 'GET tok:none')
[ "$A1" = None ] && [ "$A2" = None ] && [ "$(cnt TYPE tok:none)" = 1 ] \
	&& ok "an absent key is remembered: two misses, one fetch" \
	|| bad "tok:none: $A1/$A2, fetched $(cnt TYPE tok:none) time(s)"

echo "--- 3: single-flight"
# 200 ms on each of the fetch's three commands, inside the 2 s timeout
echo '{"password": "up-pass-1", "resp3": true, "delay_ms": 200, "delay_keys": ["tok:sf", "tok:p"]}' > "$D/up/ctl.json"
sleep 0.2
SFP=""
for n in 1 2 3 4 5 6 7 8 9 10; do rc 'GET tok:sf' > "$D/sf$n" & SFP="$SFP $!"; done
for p in $SFP; do wait $p; done      # the clients only - not the node
G=$(cat "$D"/sf* | sort -u)
[ "$G" = "'sfv'" ] && [ "$(cnt TYPE tok:sf)" = 1 ] && ok "ten clients missing one slow key: one fetch, ten values" \
	|| bad "single-flight: replies [$G], fetched $(cnt TYPE tok:sf) time(s)"

echo "--- 4: a pipeline behind a fetch answers in order"
P=$(rc 'GET tok:p' 'PING' 'GET tok:a' | tr '\n' ' ')
[ "$P" = "'pv' '+PONG' 'alpha' " ] && ok "GET (fetched) / PING / GET answered in order" \
	|| bad "pipeline order: $P"
echo '{"password": "up-pass-1", "resp3": true}' > "$D/up/ctl.json"

echo "--- 5: a plain SET is local"
S=$(rc 'SET tok:new mine' 'GET tok:new' | tr '\n' ' ')
[ "$S" = "'+OK' 'mine' " ] && ok "SET then GET: the local value wins" || bad "tok:new: $S"
k=0; while [ "$(cnt TYPE tok:new)" = 0 ] && [ $k -lt 20 ]; do sleep 0.1; k=$((k+1)); done
[ "$(cnt TYPE tok:new)" = 1 ] && ok "and one background ownership check asked the upstream" \
	|| bad "tok:new ownership check: fetched $(cnt TYPE tok:new) time(s)"

echo "--- 6: the upstream down"
echo '{"password": "up-pass-1", "resp3": true, "down": true}' > "$D/up/ctl.json"
sleep 0.3
case "$(rc 'GET tok:down')" in *upstream*) ok "a miss with the upstream down answers an error naming it" ;;
	*) bad "upstream down: $(rc 'GET tok:down')" ;; esac
echo '{"password": "up-pass-1", "resp3": true}' > "$D/up/ctl.json"
sleep 0.5
R=""; k=0
while [ $k -lt 20 ]; do R=$(rc 'GET tok:down'); [ "$R" = "'dv'" ] && break; sleep 0.25; k=$((k+1)); done
[ "$R" = "'dv'" ] && ok "and reads through again once it is back" || bad "after the upstream came back: $R"

echo "--- 7: nothing written upstream; the stats"
[ ! -s "$D/up/writes" ] && ok "the upstream received no write command" \
	|| bad "WRITES REACHED THE UPSTREAM: $(head -3 "$D/up/writes" | tr '\n' '|')"
ST=$(python3 -c 'import json,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats",timeout=5).read())
u=[c for c in r["collections"] if c["name"]=="0"][0].get("upstream")
print("%d %d %d %d %d" % (u["fetches"], u["fill_string"], u["fill_hash"], u["fill_json"], u["wrongtype"]) if u else "none")' 2>/dev/null)
set -- $ST
[ "${1:-0}" -ge 6 ] 2>/dev/null && [ "${3:-0}" = 1 ] && [ "${4:-0}" = 1 ] && [ "${5:-0}" = 1 ] \
	&& ok "stats: the upstream block counts fetches and fills ($ST)" || bad "stats upstream block: $ST"
grep -qE " (ERROR|CRIT)" "$D/n.log" && bad "ERROR/CRIT in the node log: $(grep -m1 -E ' (ERROR|CRIT)' "$D/n.log" | cut -c1-120)" \
	|| ok "no ERROR or CRIT in the node log"

echo "--- 8: a mutation promotes the fetched value first"
I=$(rc 'INCR tok:cnt' 'GET tok:cnt' | tr '\n' ' ')
[ "$I" = "42 '42' " ] && ok "INCR of an upstream counter (41) answers 42, and it stays 42" \
	|| bad "INCR tok:cnt: $I"
H=$(rc 'HSET tok:h2 b 2' 'HGETALL tok:h2' | tr '\n' ' ')
[ "$H" = "1 ['a', '1', 'b', '2'] " ] && ok "HSET into an upstream hash keeps its fields" \
	|| bad "HSET tok:h2: $H"

echo "--- 9: what this node wrote or deleted never reads through again"
X=$(rc 'DEL tok:d2' 'GET tok:d2' 'EXISTS tok:d2' | tr '\n' ' ')
[ "$X" = "1 None 0 " ] && ok "DEL of a key only the upstream had: 1, then gone" \
	|| bad "DEL tok:d2: $X"
rc 'SET tok:o mine' > /dev/null
k=0; while [ "$(cnt TYPE tok:o)" = 0 ] && [ $k -lt 20 ]; do sleep 0.1; k=$((k+1)); done
sleep 0.3
X=$(rc 'DEL tok:o' 'GET tok:o' | tr '\n' ' ')
[ "$X" = "1 None " ] && ok "SET, then DEL: gone, the upstream's copy does not come back" \
	|| bad "tok:o: $X"
rc 'SET tok:e mine EX 1' > /dev/null
sleep 2.5
X=$(rc 'GET tok:e')
[ "$X" = None ] && ok "an owned key that expired stays gone (the upstream still has one)" \
	|| bad "tok:e after its expiry: $X"
NF=$(python3 -c 'import json,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats",timeout=5).read())
c=[x for x in r["collections"] if x["name"]=="0.nofall"]
u=[x for x in r["collections"] if x["name"]=="0"][0]["upstream"]
print("%s %d %d" % (c[0]["entries"] if c else "none", u["promoted"], u["ownership_found"]))' 2>/dev/null)
set -- $NF
[ "${1:-0}" -ge 4 ] 2>/dev/null && [ "${2:-0}" -ge 2 ] && ok "the companion 0.nofall holds the fleet's keys ($NF: entries, promoted, ownership found)" \
	|| bad "companion/promotion: $NF"

echo "--- 10: a clean restart keeps it"
kill -TERM $PID; wait $PID 2>/dev/null; PID=
B4=$(cnt TYPE tok:d2)
"$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
PID=$!
i=0; while [ $i -lt 100 ]; do [ "$(grep -c "perfcached ready" "$D/n.log")" -ge 2 ] && break; sleep 0.1; i=$((i+1)); done
X=$(rc 'GET tok:cnt' 'GET tok:d2' 'GET tok:o' 'HGETALL tok:h2' | tr '\n' ' ')
[ "$X" = "'42' None None ['a', '1', 'b', '2'] " ] && [ "$(cnt TYPE tok:d2)" = "$B4" ] \
	&& ok "after a restart: promoted records kept, deleted keys still not read through" \
	|| bad "after restart: $X (tok:d2 fetched $B4 -> $(cnt TYPE tok:d2))"
[ ! -s "$D/up/writes" ] && ok "still no write reached the upstream" \
	|| bad "WRITES REACHED THE UPSTREAM: $(head -3 "$D/up/writes" | tr '\n' '|')"

echo "--- 11: the native doors"
cat > "$D/native.py" <<'P'
import json, socket, struct, sys, time, pcnative
port = int(sys.argv[1])
def cmd():
    s = socket.create_connection(("127.0.0.1", port), 10)
    f = pcnative.wrap(s)
    out = []
    for m, p in (("get", {"key": "tok:n1"}), ("add", {"key": "tok:n2", "by": 1}),
                 ("del", {"key": "tok:n3"}), ("get", {"key": "tok:n3"}),
                 ("hcmd", {"key": "tok:nh", "cmd": "HGET", "args": ["f"]})):
        p["col"] = "0"
        r = pcnative.call(f, m, **p)
        out.append(r.get("result", r.get("error")))
    return out
HDR = struct.Struct("<BBBBIQ")
def bframe(rid, verb, key, by=None):
    k = key.encode(); c = b"0"
    body = bytes([verb, len(c)]) + struct.pack("<H", len(k))
    if by is not None:
        body += struct.pack("<qq", by, 0)
    body += c + k
    return HDR.pack(0x9E, 1, 1, 0, len(body), rid) + body
def rdframe(s):
    h = b""
    while len(h) < 16:
        h += s.recv(16 - len(h))
    _, _, _, fl, n, rid = HDR.unpack(h)
    b = b""
    while len(b) < n:
        b += s.recv(n - len(b))
    return rid, fl, b
def bins():
    s = socket.create_connection(("127.0.0.1", port), 10)
    s.sendall(bframe(1, 2, "tok:b1"))
    rid, fl, b = rdframe(s)
    get1 = (rid, b[0], b[5:].decode()) if not fl & 1 else ("err", b.decode())
    s.sendall(bframe(2, 8, "tok:b2", by=5))
    rid, fl, b = rdframe(s)
    add2 = (rid, struct.unpack("<q", b[:8])[0]) if not fl & 1 else ("err", b.decode())
    # two in flight: the slow one first
    s.sendall(bframe(10, 2, "tok:bslow") + bframe(11, 2, "tok:a"))
    got = {}
    for _ in range(2):
        rid, fl, b = rdframe(s)
        got[rid] = b[5:].decode() if b and b[0] == 1 else None
    return [list(get1), list(add2), sorted(got.items())]
print(json.dumps(cmd()))
print(json.dumps(bins()))
P
echo '{"password": "up-pass-1", "resp3": true, "delay_ms": 200, "delay_keys": ["tok:bslow"]}' > "$D/up/ctl.json"
sleep 0.2
NO=$(python3 "$D/native.py" $TCP 2>&1)
echo '{"password": "up-pass-1", "resp3": true}' > "$D/up/ctl.json"
C=$(echo "$NO" | sed -n 1p); B=$(echo "$NO" | sed -n 2p)
python3 -c 'import json,sys
c=json.loads(sys.argv[1])
ok = c[0].get("value")=="nv1" and c[1].get("value") in (6,"6") and c[2].get("deleted") is True and c[3].get("found") is False
sys.exit(0 if ok else 1)' "$C" 2>/dev/null && ok "CMD: get reads through, add promotes (5 -> 6), del of an upstream key, then gone" \
	|| bad "CMD door: $C"
case "$C" in *hv*) ok "CMD: hcmd HGET reads a hash through" ;; *) bad "CMD hcmd: $C" ;; esac
[ "$B" = '[[1, 1, "bv1"], [2, 15], [[10, "slowv"], [11, "alpha"]]]' ] \
	&& ok "binary: GET reads through, ADD promotes (10 -> 15), two frames in flight each answered under its id" \
	|| bad "binary door: $B"
[ ! -s "$D/up/writes" ] && ok "still no write reached the upstream (native doors)" \
	|| bad "WRITES REACHED THE UPSTREAM: $(head -3 "$D/up/writes" | tr '\n' '|')"

echo "--- 12: tracking"
setdata() { python3 -c 'import json,sys
p=sys.argv[1]; d=json.load(open(p)); d[sys.argv[2]]["value"]=sys.argv[3]
json.dump(d, open(p+".t","w")); import os; os.replace(p+".t", p)' "$D/up/data.json" "$1" "$2"; }
k=0; while [ "$(stat tracking)" != True ] && [ $k -lt 40 ]; do sleep 0.1; k=$((k+1)); done
I0=$(stat invalidations)
A=$(rc 'GET tok:t1')
setdata tok:t1 t1b
echo '{"password": "up-pass-1", "resp3": true, "inv_seq": 1, "invalidate": ["tok:t1"]}' > "$D/up/ctl.json"
k=0; R=""; while [ $k -lt 30 ]; do R=$(rc 'GET tok:t1'); [ "$R" = "'t1b'" ] && break; sleep 0.1; k=$((k+1)); done
[ "$A" = "'t1a'" ] && [ "$R" = "'t1b'" ] && [ "$(stat invalidations)" -gt "${I0:-0}" ] \
	&& ok "an invalidation from the upstream: the next read has the new value (t1a -> t1b)" \
	|| bad "invalidation: first $A, then $R, invalidations ${I0} -> $(stat invalidations)"
F0=$(stat flushes)
A=$(rc 'GET tok:t2')
setdata tok:t2 t2b
echo '{"password": "up-pass-1", "resp3": true, "kill_seq": 1}' > "$D/up/ctl.json"
k=0; R=""; while [ $k -lt 40 ]; do R=$(rc 'GET tok:t2'); [ "$R" = "'t2b'" ] && break; sleep 0.1; k=$((k+1)); done
[ "$A" = "'t2a'" ] && [ "$R" = "'t2b'" ] && [ "$(stat flushes)" -gt "${F0:-0}" ] \
	&& ok "the tracking connection lost: every fetched copy dropped, the change read (t2a -> t2b, no invalidation sent)" \
	|| bad "tracking loss: first $A, then $R, flushes ${F0} -> $(stat flushes)"
k=0; while [ "$(stat tracking)" != True ] && [ $k -lt 40 ]; do sleep 0.1; k=$((k+1)); done
[ "$(stat tracking)" = True ] && ok "and tracking comes back" || bad "tracking after the loss: $(stat tracking)"
[ ! -s "$D/up/writes" ] && ok "still no write reached the upstream (tracking)" \
	|| bad "WRITES REACHED THE UPSTREAM: $(head -3 "$D/up/writes" | tr '\n' '|')"

echo "--- 13: the drain sweep"
dr() { python3 -c 'import json,socket,sys,pcnative
f=pcnative.wrap(socket.create_connection(("127.0.0.1",int(sys.argv[1])),10))
if sys.argv[2]=="1":
    pcnative.call(f,"enable",secret="up-enable-secret")
r=pcnative.call(f,"updrain",col="0")
print(json.dumps(r.get("result", r.get("error"))))' $TCP "$1" 2>&1; }
dstat() { python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats",timeout=5).read())
d=[c for c in r["collections"] if c["name"]=="0"][0]["upstream"]["drain"]
print(d[sys.argv[1]])' "$1" 2>/dev/null; }
case "$(dr 0)" in *rivileg*|*enable*) ok "updrain without privilege is refused" ;; *) bad "unprivileged updrain: $(dr 0)" ;; esac
TRK0=$(grep -c "^TRACKING " "$D/up/counts"); SC0=$(grep -c "^SCAN " "$D/up/counts")
R=$(dr 1)
case "$R" in *started*) ok "updrain starts the sweep ($R)" ;; *) bad "updrain: $R" ;; esac
k=0; while [ "$(dstat state)" = running ] && [ $k -lt 100 ]; do sleep 0.2; k=$((k+1)); done
ST="$(dstat state) read=$(dstat read) taken=$(dstat taken_over) fleet_had=$(dstat fleet_had) wrongtype=$(dstat wrongtype) errors=$(dstat errors)"
[ "$(dstat state)" = done ] && [ "$(dstat taken_over)" -ge 4 ] && [ "$(dstat fleet_had)" -ge 3 ] \
	&& [ "$(dstat wrongtype)" -ge 2 ] && [ "$(dstat errors)" = 0 ] && ok "the sweep is done ($ST)" \
	|| bad "the sweep: $ST"
# 0.5.6.1: the sweep's reads go into the collection, not the shadow - its
# connection must not arm tracking, or the upstream remembers every key of
# the family for it and pushes an invalidation for each later write
TRK1=$(grep -c "^TRACKING " "$D/up/counts"); SC1=$(grep -c "^SCAN " "$D/up/counts")
[ "$SC1" -gt "$SC0" ] && [ "$TRK1" = "$TRK0" ] \
	&& ok "the sweep's connection read with SCAN and never asked for tracking ($((SC1 - SC0)) SCANs, 0 TRACKING)" \
	|| bad "the sweep: $((SC1 - SC0)) SCANs, $((TRK1 - TRK0)) CLIENT TRACKING sent from its connection"
echo '{"password": "up-pass-1", "resp3": true, "down": true}' > "$D/up/ctl.json"
sleep 0.3
X=$(rc 'GET tok:dr1' 'HGETALL tok:dr2' 'JSON.GET tok:dr3' 'GET tok:dr5' | tr '\n' ' ')
T=$(rc 'TTL tok:dr1')
[ "$X" = "'d1' ['a', 'x'] '{\"k\":2}' 'd5' " ] && [ "$T" -ge 30 ] 2>/dev/null && [ "$T" -le 91 ] \
	&& ok "upstream switched off: the swept keys answer from the collection (tok:dr1 TTL $T s)" \
	|| bad "after the sweep, upstream down: $X (TTL $T)"
X=$(rc 'GET tok:new' 'GET tok:d2' 'GET tok:cnt' | tr '\n' ' ')
[ "$X" = "'mine' None '42' " ] && ok "the fleet's own versions were kept (written, deleted, promoted)" \
	|| bad "fleet's versions after the sweep: $X"
[ ! -s "$D/up/writes" ] && ok "still no write reached the upstream (drain)" \
	|| bad "WRITES REACHED THE UPSTREAM: $(head -3 "$D/up/writes" | tr '\n' '|')"
echo '{"password": "up-pass-1", "resp3": true}' > "$D/up/ctl.json"

echo "--- 14: one server"
# keys the upstream gains only now: case 13's sweep took over everything
# it held, and those answer from the collection without a fetch
python3 -c 'import json,sys
p=sys.argv[1]; d=json.load(open(p))
d["tok:rr"]={"type":"string","value":"rr1"}; d["tok:rr3"]={"type":"string","value":"r3v"}
json.dump(d, open(p+".t","w")); import os; os.replace(p+".t", p)' "$D/up/data.json"
# restart_node: a clean stop and a start, every upstream connection new
restart_node() {
	n0=$(grep -c "perfcached ready" "$D/n.log")
	kill -TERM $PID; wait $PID 2>/dev/null
	"$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
	PID=$!
	i=0; while [ $i -lt 100 ]; do [ "$(grep -c "perfcached ready" "$D/n.log")" -gt "$n0" ] && break; sleep 0.1; i=$((i+1)); done
}
# a round-robin proxy in front of several servers: the fake tells every
# connection it reached a different one.  The tracking connection's id
# means nothing (or somebody else) on the others, so a fetch connection
# there must stay untracked - this change is never announced
echo '{"password": "up-pass-1", "resp3": true, "run_id_per_conn": true}' > "$D/up/ctl.json"
restart_node
k=0; while [ "$(stat tracking)" != True ] && [ $k -lt 40 ]; do sleep 0.1; k=$((k+1)); done
TR0=$(grep -c "^TRACKING " "$D/up/counts")
A=$(rc 'GET tok:rr')
setdata tok:rr rr2
k=0; B=""; while [ $k -lt 32 ]; do B=$(rc 'GET tok:rr'); [ "$B" = "'rr2'" ] && break; sleep 0.25; k=$((k+1)); done
[ "$A" = "'rr1'" ] && [ "$B" = "'rr2'" ] \
	&& ok "a connection on another server is not tracked: the unannounced change was read after upstream_negative_ms (rr1 -> rr2 in $((k * 250)) ms)" \
	|| bad "another server: first $A, then $B after 8 s - a copy believed tracked kept the old value"
[ "$(grep -c "^TRACKING " "$D/up/counts")" = "$TR0" ] && [ "$(stat tracking_other_server)" -ge 1 ] 2>/dev/null \
	&& grep -q "reached another server" "$D/n.log" \
	&& ok "no CLIENT TRACKING sent from it; counted ($(stat tracking_other_server)) and logged" \
	|| bad "TRACKING sent $TR0 -> $(grep -c "^TRACKING " "$D/up/counts"), tracking_other_server $(stat tracking_other_server), log: $(grep -c "reached another server" "$D/n.log")"
# an upstream user without +info: perfcached cannot tell which server it
# reached, so no tracking at all - reads still work
echo '{"password": "up-pass-1", "resp3": true, "info_noperm": true}' > "$D/up/ctl.json"
restart_node
k=0; while ! grep -q "does not say which server" "$D/n.log" && [ $k -lt 40 ]; do sleep 0.1; k=$((k+1)); done
F0=$(cnt TYPE tok:rr3)
X=$(rc 'GET tok:rr3')
[ "$(stat tracking)" = False ] && [ "$X" = "'r3v'" ] && [ "$(cnt TYPE tok:rr3)" -gt "$F0" ] && grep -q "does not say which server" "$D/n.log" \
	&& ok "INFO refused: tracking stays off, says why, and reads still work" \
	|| bad "INFO refused: tracking $(stat tracking), read $X (fetched $F0 -> $(cnt TYPE tok:rr3)), log: $(grep -c "does not say which server" "$D/n.log")"
[ ! -s "$D/up/writes" ] && ok "still no write reached the upstream (one server)" \
	|| bad "WRITES REACHED THE UPSTREAM: $(head -3 "$D/up/writes" | tr '\n' '|')"
echo '{"password": "up-pass-1", "resp3": true}' > "$D/up/ctl.json"

echo "--- 15: a mapped RESP db index reads through"
# S338: the RESP gate was handed what SELECT named, not the collection it
# maps to - under resp_collections (7:0, PROD's 1:trial) it found no
# upstream for "7" and the read answered nil, never fetching
python3 -c 'import json,sys
p=sys.argv[1]; d=json.load(open(p)); d["tok:map1"]={"type":"string","value":"mapv"}
json.dump(d, open(p+".t","w")); import os; os.replace(p+".t", p)' "$D/up/data.json"
F0=$(cnt TYPE tok:map1)
X=$(rc 'SELECT 7' 'GET tok:map1' | tr '\n' ' ')
[ "$X" = "'+OK' 'mapv' " ] && [ "$(cnt TYPE tok:map1)" -gt "$F0" ] \
	&& ok "SELECT 7 (mapped to collection 0) reads tok:map1 through from the upstream" \
	|| bad "SELECT 7 then GET tok:map1: $X (fetched $F0 -> $(cnt TYPE tok:map1))"

echo "--- 16: what the collection counts"
# S340: a read answered from a fetched copy is a HIT of the collection; only
# an absent answer is a miss; the read-through's own looks (the gate's
# probes, the no-fall checks) count nowhere.  0.5.6.1 counted every
# read-through read a miss - twice, with the gate's probe - and the
# companion a miss per lookup: PROD's prod-redis read 0 hits while the SBC
# blocked devices from the keys it fetched.
colc() { python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats",timeout=5).read())
c={x["name"]:x for x in r["collections"]}
h=lambda x: x.get("hits_client", x.get("hits", 0)); m=lambda x: x.get("misses_client", x.get("misses", 0))
print(h(c["0"]), m(c["0"]), h(c["0.nofall"]) + m(c["0.nofall"]))' 2>/dev/null; }
python3 -c 'import json,sys
p=sys.argv[1]; d=json.load(open(p)); d["tok:cnt1"]={"type":"string","value":"c1"}
json.dump(d, open(p+".t","w")); import os; os.replace(p+".t", p)' "$D/up/data.json"
set -- $(colc); H0=${1:-0} M0=${2:-0} N0=${3:-0}
X=$(rc 'GET tok:cnt1' 'GET tok:cnt1' 'GET tok:cnt-none' 'GET tok:cnt-none' | tr '\n' ' ')
set -- $(colc); H1=${1:-0} M1=${2:-0} N1=${3:-0}
[ "$X" = "'c1' 'c1' None None " ] && [ $((H1 - H0)) = 2 ] && [ $((M1 - M0)) = 2 ] \
	&& ok "two reads of a fetched key are 2 hits, two of an absent key 2 misses (hits +$((H1 - H0)), misses +$((M1 - M0)))" \
	|| bad "counted: reads $X -> hits +$((H1 - H0)), misses +$((M1 - M0)) (want +2, +2)"
[ $((N1 - N0)) = 0 ] && ok "the companion 0.nofall counted none of those lookups" \
	|| bad "0.nofall counted $((N1 - N0)) hits+misses for 4 reads of its parent"
# the same through the BINARY door - what cachedb_perfd sends.  A read that
# waits for a fetch is parked as a frame and dispatched again when the
# fetch completes; PROD (0.5.6.1 ba2244c4) counted those reads neither hit
# nor miss: 30 s fleet-wide, 1,535 fetch-completed reads, 0 counted
python3 -c 'import json,sys
p=sys.argv[1]; d=json.load(open(p)); d["tok:cnt2"]={"type":"string","value":"c2"}
json.dump(d, open(p+".t","w")); import os; os.replace(p+".t", p)' "$D/up/data.json"
set -- $(colc); H0=${1:-0} M0=${2:-0} N0=${3:-0}
X=$(python3 - $TCP <<'P'
import socket, struct, sys
HDR = struct.Struct("<BBBBIQ")
def bframe(rid, key):
    k = key.encode(); c = b"0"
    body = bytes([2, len(c)]) + struct.pack("<H", len(k)) + c + k
    return HDR.pack(0x9E, 1, 1, 0, len(body), rid) + body
def rd(s):
    h = b""
    while len(h) < 16: h += s.recv(16 - len(h))
    _, _, _, fl, n, rid = HDR.unpack(h); b = b""
    while len(b) < n: b += s.recv(n - len(b))
    return (b[5:].decode() if b and b[0] == 1 else None) if not fl & 1 else "ERR"
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10)
out = []
for i, k in enumerate(("tok:cnt2", "tok:cnt2", "tok:cnt-none2", "tok:cnt-none2")):
    s.sendall(bframe(100 + i, k)); out.append(str(rd(s)))
print(" ".join(out))
P
)
set -- $(colc); H1=${1:-0} M1=${2:-0} N1=${3:-0}
[ "$X" = "c2 c2 None None" ] && [ $((H1 - H0)) = 2 ] && [ $((M1 - M0)) = 2 ] && [ $((N1 - N0)) = 0 ] \
	&& ok "binary door: the same four reads count 2 hits, 2 misses, the companion nothing" \
	|| bad "binary door counted: reads $X -> hits +$((H1 - H0)), misses +$((M1 - M0)), companion +$((N1 - N0)) (want +2, +2, +0)"

echo "--- 17: upstream_writes = refuse (S348)"
# A collection that mirrors its upstream and that clients can only READ:
# every write form on every door refused with one message, the upstream's
# value still read through after them; updrain refused (a takeover IS a
# write); no <name>.nofall companion; the refusals counted.  -C refuses the
# setting without upstream.  Its own node, so cases 1-16 are untouched.
printf '[daemon]\nworkers = 1\n[memory]\narena_mb = 32\n[secrets]\nclient = x\n[listen]\ntcp = 127.0.0.1:17999\nplaintext = loopback\n[collection plain]\nbuckets_log2 = 8\nupstream_writes = refuse\n' > "$D/noup.conf"
chmod 600 "$D/noup.conf"
OUT=$("$BIN" -C -f "$D/noup.conf" 2>&1); rcv=$?
[ $rcv -ne 0 ] && echo "$OUT" | grep -q "upstream_writes needs" \
	&& ok "-C refuses upstream_writes without upstream, and says why" \
	|| bad "-C with upstream_writes and no upstream: rc $rcv: $(echo "$OUT" | tail -1)"
TCP2=$((TCP + 10)); RESP2=$((RESP + 10)); HTTP2=$((HTTP + 10))
mkdir -p "$D/state2"
cat > "$D/n2.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state2
[memory]
arena_mb = 64
[secrets]
client = up-client-secret
upstream = up-pass-1
enable = up-enable-secret
[listen]
tcp = 127.0.0.1:$TCP2
resp = 127.0.0.1:$RESP2
resp_collections = 0:ro
http = 127.0.0.1:$HTTP2
plaintext = loopback
[collection ro]
buckets_log2 = 10
upstream = redis://127.0.0.1:$UP
upstream_prefixes = tok:*
upstream_timeout_ms = 2000
upstream_writes = refuse
C
chmod 600 "$D/n2.conf"
"$BIN" -f "$D/n2.conf" > "$D/n2.log" 2>&1 &
PID2=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n2.log" && break
	kill -0 $PID2 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
if grep -q "perfcached ready" "$D/n2.log"; then
	ok "a node with upstream_writes = refuse starts"
	MSG="refuses writes (upstream_writes = refuse)"
	R=$(python3 "$D/rc.py" $RESP2 "GET tok:p")
	[ "$R" = "'pv'" ] && ok "RESP: a read still reads through ($R)" || bad "RESP read: $R"
	R=$(python3 "$D/rc.py" $RESP2 "SET tok:p x" "DEL tok:p" "INCR tok:n2" "EXPIRE tok:p 10" "HSET tok:h f9 v9" "JSON.SET tok:j $ {}" "DECRBY tok:n2 1" "SETEX tok:p 10 x")
	n=$(echo "$R" | grep -c -- "$MSG")
	[ "$n" = 8 ] && ok "RESP: SET, DEL, INCR, EXPIRE, HSET, JSON.SET, DECRBY, SETEX - all 8 refused with the one message" \
		|| bad "RESP writes: $n of 8 refused: $(echo "$R" | tr '\n' ' ' | cut -c1-300)"
	R=$(python3 "$D/rc.py" $RESP2 "GET tok:p" "GET tok:n2")
	[ "$(echo "$R" | tr '\n' ' ')" = "'pv' '5' " ] && ok "and the upstream's values still read through after them" \
		|| bad "after the refused writes: $(echo "$R" | tr '\n' ' ')"
	R=$(python3 - $TCP2 <<'P'
import json, socket, struct, sys, pcnative
port = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.0.1", port), 10))
out = []
for m, kw in (("set", {"col": "ro", "key": "tok:p", "value": "x"}), ("del", {"col": "ro", "key": "tok:p"}),
              ("expire", {"col": "ro", "key": "tok:p", "ttl": 10}), ("add", {"col": "ro", "key": "tok:n2", "by": 1}),
              ("mset", {"col": "ro", "items": [{"key": "tok:p", "value": "1"}, {"key": "tok:sf", "value": "2"}]})):
    r = pcnative.call(f, m, **kw)
    out.append("cmd:%s=%s" % (m, json.dumps(r.get("error") or r.get("result"))))
s = socket.create_connection(("127.0.0.1", port), 10)
for i, (verb, key, val) in enumerate(((3, "tok:p", "x"), (4, "tok:p", ""))):
    pl = struct.pack("<BBH", verb, 2, len(key)) + b"ro" + key.encode() + val.encode()
    s.sendall(struct.pack("<BBBBIQ", 0x9E, 1, 1, 0, len(pl), 700 + i) + pl)
    while True:
        h = b""
        while len(h) < 16: h += s.recv(16 - len(h))
        n = struct.unpack("<I", h[4:8])[0]; b = b""
        while len(b) < n: b += s.recv(n - len(b))
        if struct.unpack("<Q", h[8:16])[0] != 0: break
    out.append("bin:%d=%s" % (verb, b.decode(errors="replace") if h[3] & 1 else "OK"))
r = pcnative.call(f, "enable", secret="up-enable-secret")
r = pcnative.call(f, "updrain", col="ro")
out.append("updrain=%s" % json.dumps(r.get("error") or r.get("result")))
print("\n".join(out))
P
)
	n=$(echo "$R" | grep -c -- "$MSG")
	[ "$n" = 7 ] && ok "native: the CMD set/del/expire/add/mset and the binary set/del - all 7 refused with the one message" \
		|| bad "native writes: $n of 7 refused: $(echo "$R" | tr '\n' ' ' | cut -c1-400)"
	echo "$R" | grep -q "updrain=.*refuses writes" && ok "updrain is refused - a takeover is a write" \
		|| bad "updrain on a refusing collection: $(echo "$R" | grep updrain)"
	R=$(python3 -c 'import json,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP2'/stats",timeout=5).read())
names=[c["name"] for c in r["collections"]]; u=[c for c in r["collections"] if c["name"]=="ro"][0]["upstream"]
print(" ".join(sorted(names)), u.get("writes"), u.get("writes_refused"), u.get("win_writes_refused"))')
	set -- $R
	[ "$1" = ro ] && [ "$2" = refuse ] && [ "${3:-0}" -ge 14 ] && [ "${4:-0}" -ge 14 ] \
		&& ok "/stats: no ro.nofall companion, writes refuse, $3 refused (window $4)" \
		|| bad "/stats after the refusals: $R"
else
	bad "the node with upstream_writes = refuse did not start: $(grep -m1 -E 'CRIT|ERR|unknown' "$D/n2.log" | cut -c1-160)"
fi
kill -9 $PID2 2>/dev/null

echo "--- 18: EXISTS, TTL and TYPE read no value (ST2)"
# A probe on a miss asks TYPE + PTTL (+ the pipelined GET, which carries a
# string's value for free) and keeps a PRESENT marker for a hash or a
# document - no HGETALL / JSON.GET, no value moved.  The marker answers the
# next probes with the upstream's TTL and type; a value read still fetches
# the value; an invalidation drops the marker; a read that joins a probe's
# fetch gets the value.  Fail-first: 0.5.6.6 fetches the document
# (JSON.GET 1 after an EXISTS) and has no fill_present / present_hits.
python3 -c 'import json,sys
p=sys.argv[1]; d=json.load(open(p))
d["tok:pj"]={"type":"json","value":{"a":1},"ttl_ms":50000}
d["tok:ph"]={"type":"hash","value":{"f":"v"}}
d["tok:ps"]={"type":"string","value":"sv"}
d["tok:pi"]={"type":"json","value":{"i":1}}
d["tok:pslow"]={"type":"json","value":{"s":2}}
d["tok:pn"]={"type":"json","value":{"n":3}}
json.dump(d, open(p+".t","w")); import os; os.replace(p+".t", p)' "$D/up/data.json"
# tracked fetches from here: case 14 ended with INFO refused (tracking off
# for that process) - a restart with the upstream answering INFO again
echo '{"password": "up-pass-1", "resp3": true}' > "$D/up/ctl.json"
restart_node
k=0; while [ "$(stat tracking)" != True ] && [ $k -lt 40 ]; do sleep 0.1; k=$((k+1)); done
[ "$(stat tracking)" = True ] || bad "case 18: tracking did not come back after the restart"
UT0=$(stat untracked_fills)
FP0=$(stat fill_present); PH0=$(stat present_hits)
R=$(rc 'EXISTS tok:pj')
[ "$R" = 1 ] && [ "$(cnt TYPE tok:pj)" = 1 ] && [ "$(cnt PTTL tok:pj)" = 1 ] && [ "$(cnt JSON.GET tok:pj)" = 0 ] \
	&& ok "EXISTS of an upstream document: 1, from TYPE + PTTL - no JSON.GET" \
	|| bad "EXISTS tok:pj: $R; TYPE $(cnt TYPE tok:pj), PTTL $(cnt PTTL tok:pj), JSON.GET $(cnt JSON.GET tok:pj) (want 1 1 0)"
R=$(rc 'EXISTS tok:pj' 'TTL tok:pj' 'PTTL tok:pj' 'TYPE tok:pj' | tr '\n' ' ')
# the fake's ttl_ms counts from its own start: compare with ITS PTTL now
UPT=$(python3 "$D/rc.py" $UP "AUTH up-pass-1" "PTTL tok:pj" | tail -1)
set -- $R
[ "$1" = 1 ] && [ "$4" = "'+ReJSON-RL'" ] && [ "$(cnt TYPE tok:pj)" = 1 ] \
	&& [ $(( $3 - UPT )) -le 1500 ] 2>/dev/null && [ $(( UPT - $3 )) -le 1500 ] \
	&& [ $(( $2 * 1000 - UPT )) -le 1500 ] && [ $(( UPT - $2 * 1000 )) -le 1500 ] \
	&& ok "the marker answers EXISTS, TTL ($2 s), PTTL ($3 ms, upstream $UPT) and TYPE ($4) - nothing asked upstream again" \
	|| bad "probes after the marker: $R (upstream PTTL $UPT); TYPE asked $(cnt TYPE tok:pj) time(s) (want 1)"
FP1=$(stat fill_present); PH1=$(stat present_hits)
[ $((FP1 - FP0)) = 1 ] && [ $((PH1 - PH0)) -ge 4 ] \
	&& ok "/stats: fill_present +$((FP1 - FP0)), present_hits +$((PH1 - PH0))" \
	|| bad "/stats ST2 counters: fill_present ${FP0}->${FP1}, present_hits ${PH0}->${PH1} (want +1, >= +4)"
J=$(rc 'JSON.GET tok:pj')
[ "$J" = "'{\"a\":1}'" ] && [ "$(cnt JSON.GET tok:pj)" = 1 ] \
	&& ok "a value read of a PRESENT key fetches the document ($J)" \
	|| bad "JSON.GET after the marker: $J, JSON.GET asked $(cnt JSON.GET tok:pj) time(s)"
R=$(rc 'EXISTS tok:ph' 'TYPE tok:ph' | tr '\n' ' ')
[ "$R" = "1 '+hash' " ] && [ "$(cnt HGETALL tok:ph)" = 0 ] \
	&& ok "a hash: EXISTS 1, TYPE hash - no HGETALL" || bad "hash probe: $R, HGETALL $(cnt HGETALL tok:ph)"
[ "$(rc 'HGET tok:ph f')" = "'v'" ] && [ "$(cnt HGETALL tok:ph)" = 1 ] \
	&& ok "and HGET then fetches it" || bad "HGET after the marker: $(rc 'HGET tok:ph f'), HGETALL $(cnt HGETALL tok:ph)"
R=$(rc 'EXISTS tok:ps' 'GET tok:ps' | tr '\n' ' ')
[ "$R" = "1 'sv' " ] && [ "$(cnt TYPE tok:ps)" = 1 ] \
	&& ok "a string: the probe's round trip stored its value - GET asks nothing more" \
	|| bad "string probe: $R, TYPE asked $(cnt TYPE tok:ps) time(s)"
# an invalidation drops the marker: the key deleted upstream reads 0
[ "$(rc 'EXISTS tok:pi')" = 1 ] || bad "EXISTS tok:pi before the invalidation: $(rc 'EXISTS tok:pi')"
python3 -c 'import json,sys
p=sys.argv[1]; d=json.load(open(p)); del d["tok:pi"]
json.dump(d, open(p+".t","w")); import os; os.replace(p+".t", p)' "$D/up/data.json"
echo '{"password": "up-pass-1", "resp3": true, "inv_seq": 18, "invalidate": ["tok:pi"]}' > "$D/up/ctl.json"
k=0; R=""; while [ $k -lt 30 ]; do R=$(rc 'EXISTS tok:pi'); [ "$R" = 0 ] && break; sleep 0.1; k=$((k+1)); done
[ "$R" = 0 ] && [ "$(cnt TYPE tok:pi)" = 2 ] \
	&& ok "an invalidation drops the marker: the next EXISTS asks again and reads 0" \
	|| bad "after the invalidation: EXISTS $R, TYPE asked $(cnt TYPE tok:pi) time(s) (want 0, 2)"
# a read that joins a probe's fetch gets the value: the probe parks on a
# slow key, the JSON.GET arrives while its fetch is in flight
echo '{"password": "up-pass-1", "resp3": true, "delay_ms": 400, "delay_keys": ["tok:pslow"]}' > "$D/up/ctl.json"
sleep 0.3
R=$(python3 - $RESP <<'P'
import socket, sys, threading, time
port = int(sys.argv[1]); out = {}
def one(name, cmd, wait):
    time.sleep(wait)
    s = socket.create_connection(("127.0.0.1", port), 10)
    parts = cmd.split(" ")
    s.sendall(b"*%d\r\n" % len(parts) + b"".join(b"$%d\r\n%s\r\n" % (len(p), p.encode()) for p in parts))
    b = b""
    while not b.endswith(b"\r\n") or (b[:1] == b"$" and b.count(b"\r\n") < 2):
        x = s.recv(4096)
        if not x: break
        b += x
    out[name] = b
t = [threading.Thread(target=one, args=("e", "EXISTS tok:pslow", 0)),
     threading.Thread(target=one, args=("g", "JSON.GET tok:pslow", 0.1))]
[x.start() for x in t]; [x.join() for x in t]
print("e=%s g=%s" % (out.get("e", b"").decode().strip(), out.get("g", b"").decode().replace("\r\n", "|")))
P
)
[ "$R" = 'e=:1 g=$7|{"s":2}|' ] \
	&& ok "a JSON.GET joining a probe's in-flight fetch gets the document ($(cnt TYPE tok:pslow) fetch(es))" \
	|| bad "join race: $R"
echo '{"password": "up-pass-1", "resp3": true}' > "$D/up/ctl.json"
# the native doors: binary EXISTS / TTL and CMD exists / ttl classify the same
R=$(python3 - $TCP <<'P'
import json, socket, struct, sys, pcnative
HDR = struct.Struct("<BBBBIQ")
def frame(verb, rid, key):
    k = key.encode(); c = b"0"
    body = bytes([verb, len(c)]) + struct.pack("<H", len(k)) + c + k
    return HDR.pack(0x9E, 1, 1, 0, len(body), rid) + body
def rd(s):
    h = b""
    while len(h) < 16: h += s.recv(16 - len(h))
    _, _, _, fl, n, rid = HDR.unpack(h); b = b""
    while len(b) < n: b += s.recv(n - len(b))
    return "ERR" if fl & 1 else b
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10)
s.sendall(frame(5, 300, "tok:pn")); e = rd(s)
s.sendall(frame(6, 301, "tok:pn")); t = rd(s)
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10))
ce = pcnative.call(f, "exists", col="0", key="tok:pn").get("result", {})
ct = pcnative.call(f, "ttl", col="0", key="tok:pn").get("result", {})
print(e[0] if e != "ERR" else "ERR", struct.unpack("<q", t[:8])[0] if t != "ERR" and len(t) >= 8 else t, ce.get("exists"), ct.get("ttl"))
P
)
[ "$R" = "1 -1 True -1" ] && [ "$(cnt TYPE tok:pn)" = 1 ] && [ "$(cnt JSON.GET tok:pn)" = 0 ] \
	&& ok "native: binary EXISTS/TTL and CMD exists/ttl answer from one TYPE + PTTL (exists, no expiry)" \
	|| bad "native probes: $R; TYPE $(cnt TYPE tok:pn), JSON.GET $(cnt JSON.GET tok:pn) (want '1 -1 True -1', 1, 0)"
[ "$(stat untracked_fills)" = "$UT0" ] && ok "every probe of case 18 was tracked (untracked stayed $UT0)" \
	|| bad "untracked fetches during case 18: $UT0 -> $(stat untracked_fills) - the TTLs above were capped"
# CMD exists of a key held as a FETCHED COPY (not a marker): true, as
# EXISTS on the other doors - 0.5.6.6 answered false
R=$(rc 'GET tok:ps')
R2=$(python3 - $TCP <<'P'
import socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", int(sys.argv[1])), 10))
print(pcnative.call(f, "exists", col="0", key="tok:ps").get("result", {}).get("exists"))
P
)
[ "$R" = "'sv'" ] && [ "$R2" = True ] && ok "CMD exists of a fetched copy: true (it was false)" \
	|| bad "CMD exists of a fetched copy: GET $R, exists $R2"
[ ! -s "$D/up/writes" ] && ok "still no write reached the upstream (ST2)" \
	|| bad "WRITES REACHED THE UPSTREAM: $(head -3 "$D/up/writes" | tr '\n' '|')"

echo "upstreamtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
