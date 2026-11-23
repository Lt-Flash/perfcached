#!/bin/sh
# upnoroomtest.sh - a read whose fetched copy finds no room in the arena answers the fetched value,
# relayed without keeping it (GET, MGET, hashes, JSON, EXISTS/TYPE); a change never runs on a guess.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=$(readlink -f "${1:-./perfcached}")
T=$(cd "$(dirname "$0")" && pwd)
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
UP=19631 TCP=19632 RESP=19633 HTTP=19634
for p in $UP $TCP $RESP $HTTP; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "upnoroomtest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcupnoroom.XXXXXX)
PID= FPID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; [ -n "$FPID" ] && kill -9 $FPID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM

mkdir -p "$D/up" "$D/state/wal"
python3 - "$D/up/data.json" <<'P'
import json, sys
d = {"tok:s%d" % i: {"type": "string", "value": "v-s%d" % i} for i in range(1, 5)}
d["tok:h1"] = {"type": "hash", "value": {"f": "x", "g": "y"}}
d["tok:h2"] = {"type": "hash", "value": {"f": "x"}}
d["tok:h3"] = {"type": "hash", "value": {"f": "x"}}
d["tok:j1"] = {"type": "json", "value": {"a": 1, "b": "two"}}
d["tok:c1"] = {"type": "string", "value": "41"}
json.dump(d, open(sys.argv[1], "w"))
P
echo '{"password": "nr-pass-1", "resp3": true}' > "$D/up/ctl.json"
python3 "$T/fakeupstream.py" $UP "$D/up" > "$D/fake.log" 2>&1 &
FPID=$!
i=0; while [ ! -f "$D/up/ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done

cat > "$D/n.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 16
[secrets]
client = nr-client-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
resp_collections = 0
http = 127.0.0.1:$HTTP
plaintext = loopback
[wal]
dir = $D/state/wal
probe = no
segment_mb = 8
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$UP
upstream_password = nr-pass-1
upstream_prefixes = tok:*
upstream_timeout_ms = 2000
C
chmod 600 "$D/n.conf"
: > "$D/n.log"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
PID=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break
	kill -0 $PID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
if ! grep -q "perfcached ready" "$D/n.log"; then
	bad "the node did not start: $(grep -m1 -E 'CRIT|ERR' "$D/n.log" | cut -c1-160)"
	echo "upnoroomtest: $pass passed, $fail failed"
	exit 1
fi
k=0; while [ $k -lt 40 ] && ! python3 -c 'import json,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats",timeout=5).read())
raise SystemExit(0 if [c for c in r["collections"] if c["name"]=="0"][0]["upstream"]["tracking"] else 1)' 2>/dev/null; do sleep 0.1; k=$((k+1)); done

R=$(python3 - $RESP $HTTP <<'P'
import json, socket, sys, urllib.request
def enc(*a):
    return b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in
        (y if isinstance(y, bytes) else y.encode() for y in a))
class C:
    def __init__(s, port):
        s.k = socket.create_connection(("127.0.0.1", port), 10); s.f = s.k.makefile("rb")
    def rd(s):
        l = s.f.readline(); t, r = l[:1], l[1:-2]
        if t == b":": return int(r)
        if t == b"$":
            n = int(r)
            return None if n < 0 else s.f.read(n + 2)[:-2].decode()
        if t == b"*":
            n = int(r)
            return None if n < 0 else [s.rd() for _ in range(n)]
        if t == b"_": return None
        return (t + r).decode()
    def cmd(s, *a):
        s.k.sendall(enc(*a)); return s.rd()
    def pipe(s, cmds):
        s.k.sendall(b"".join(enc(*c) for c in cmds)); return [s.rd() for _ in cmds]
def ff():
    r = json.loads(urllib.request.urlopen("http://127.0.0.1:%s/stats" % sys.argv[2], timeout=5).read())
    u = [c for c in r["collections"] if c["name"] == "0"][0]["upstream"]
    m = r.get("memory", {})
    return u.get("fill_failed", 0), "used %s free %s" % (m.get("arena_used"), m.get("arena_free"))
pc = C(int(sys.argv[1]))
# the arena filled: every size written until it is refused
n, refused, first_err = 0, 0, None
for size in (2048, 512, 128, 32, 4):
    v = b"x" * size
    while n < 400000:
        rs = pc.pipe([("SET", "pad:%d" % (n + j), v) for j in range(256)])
        n += 256
        errs = [x for x in rs if isinstance(x, str) and x.startswith("-")]
        if errs:
            refused += 1
            first_err = first_err or errs[0]
            break
ff0, ar0 = ff()
out = {"pads": n, "refused": refused, "err": first_err, "arena": ar0}
out["s1"] = [pc.cmd("GET", "tok:s1"), pc.cmd("GET", "tok:s1")]
out["mget"] = pc.cmd("MGET", "tok:s2", "tok:s3", "tok:none2")
out["hget"] = pc.cmd("HGET", "tok:h1", "f")
out["hgetall"] = pc.cmd("HGETALL", "tok:h1")
j = pc.cmd("JSON.GET", "tok:j1")
try:
    out["json"] = json.loads(j) if isinstance(j, str) and not j.startswith("-") else j
except ValueError:
    out["json"] = j
out["exists"] = [pc.cmd("EXISTS", "tok:s4"), pc.cmd("EXISTS", "tok:h2")]
out["type"] = pc.cmd("TYPE", "tok:h3")
out["none"] = pc.cmd("GET", "tok:none")
out["incr"] = pc.cmd("INCR", "tok:c1")
out["ff"] = ff()[0] - ff0
print(json.dumps(out))
P
)
echo "$R" > "$D/result.json"
J() { python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(eval(sys.argv[2]))' "$D/result.json" "$1" 2>/dev/null; }
GETS=$(grep -c "^GET tok:s1\$" "$D/up/counts" 2>/dev/null)

[ "$(J 'd["refused"]')" = 5 ] && [ "$(J 'd["ff"] > 0')" = True ] &&
	ok "0. the arena refused every size ($(J 'd["pads"]') writes, '$(J 'd["err"]')', arena $(J 'd["arena"]')), and the reads counted fill_failed +$(J 'd["ff"]')" ||
	bad "0. the premise: refused $(J 'd["refused"]') of 5 sizes after $(J 'd["pads"]') writes ('$(J 'd["err"]')'), fill_failed +$(J 'd["ff"]') - nothing below found the arena full; $R"
[ "$(J 'd["s1"]')" = "['v-s1', 'v-s1']" ] && [ "${GETS:-0}" -ge 2 ] &&
	ok "1. GET twice: v-s1 both times, the upstream asked $GETS times (relayed, not kept)" ||
	bad "1. GET twice: $(J 'd["s1"]') (want v-s1 twice), the upstream asked ${GETS:-0} times (want 2 or more)"
[ "$(J 'd["mget"]')" = "['v-s2', 'v-s3', None]" ] &&
	ok "2. MGET: v-s2, v-s3 and nil for the absent key" ||
	bad "2. MGET: $(J 'd["mget"]') (want ['v-s2', 'v-s3', None])"
[ "$(J 'd["hget"]')" = x ] && [ "$(J 'sorted(zip(d["hgetall"][::2], d["hgetall"][1::2])) if isinstance(d["hgetall"], list) else d["hgetall"]')" = "[('f', 'x'), ('g', 'y')]" ] &&
	ok "3. HGET and HGETALL of a hash" ||
	bad "3. HGET $(J 'd["hget"]') (want x), HGETALL $(J 'd["hgetall"]') (want f x g y)"
[ "$(J 'd["json"] == {"a": 1, "b": "two"}')" = True ] &&
	ok "4. JSON.GET of a document" ||
	bad "4. JSON.GET: $(J 'd["json"]') (want {a: 1, b: two})"
[ "$(J 'd["exists"]')" = "[1, 1]" ] && [ "$(J 'd["type"]')" = "+hash" ] &&
	ok "5. EXISTS of a string and of a hash 1 and 1, TYPE of a hash 'hash'" ||
	bad "5. EXISTS $(J 'd["exists"]') (want [1, 1]), TYPE $(J 'd["type"]') (want +hash)"
[ "$(J 'd["none"]')" = None ] &&
	ok "6. an absent key stays nil" ||
	bad "6. an absent key answered $(J 'd["none"]')"
[ "$(J 'd["incr"] != 1')" = True ] &&
	ok "7. INCR of a key the upstream holds as 41: $(J 'd["incr"]') - not 1" ||
	bad "7. INCR of a key the upstream holds as 41 answered 1 - a change ran on a guess"

# the fill's own alert aside - it fires when the fill outlasts the alert's window
grep -h 'an fsync has been stuck' "$D/n.log" 2>/dev/null | head -n 1 | sed "s/^/  ..   not counted, the runner's storage stalled: /"
grep CRIT "$D/n.log" | grep -v 'an fsync has been stuck' | grep -qv "writes refused, the arena is full" &&
	bad "the node logged CRIT: $(grep CRIT "$D/n.log" | grep -v 'an fsync has been stuck' | grep -v "writes refused, the arena is full" | head -2 | tr '\n' ' ' | cut -c1-200)"
echo "upnoroomtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
