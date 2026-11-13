#!/bin/sh
# existsplacetest.sh - EXISTS, TYPE, TTL and PTTL through every member of a shard and a spread fleet answer as the key's
# node: written and read-through keys, RESP and both native doors.  Usage: test/existsplacetest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pcexpl.XXXXXX)
UP=20750
FPID= P1ID= P2ID= P3ID=
trap 'for p in $P1ID $P2ID $P3ID $FPID; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $UP 20759 20769 20761 20762 20763 20771 20772 20773 20781 20782 20783; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "existsplacetest: port $p already bound" >&2; exit 1; }
done
mkdir -p "$D/up"
python3 -c 'import json,sys; json.dump({"ro:t": {"type": "string", "value": "rt", "ttl_ms": 900000},
	"ro:p": {"type": "string", "value": "rp"}, "ro:h": {"type": "hash", "value": {"f1": "a", "f2": "b"}},
	"ro:j": {"type": "json", "value": {"n": 1, "s": "x"}}}, open(sys.argv[1], "w"))' "$D/up/data.json"
echo '{"role": "master"}' > "$D/up/ctl.json"
python3 "$T/fakeupstream.py" $UP "$D/up" > "$D/fake.log" 2>&1 & FPID=$!
i=0; while [ ! -f "$D/up/ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done

node() { # node <n> <shard|spread> - tcp 2076n, http 2077n, resp 2078n
	rm -rf "$D/state$1"; mkdir -p "$D/state$1"
	if [ "$2" = shard ]; then mc=239.255.79.76:20759 ex=; else mc=239.255.79.77:20769 ex="replicas = 2"; fi
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state$1
[memory]
arena_mb = 32
[secrets]
client = expl-client-secret
cluster = expl-cluster-secret
[listen]
tcp = 127.0.0.1:2076$1
http = 127.0.0.1:2077$1
resp = 127.0.0.1:2078$1
resp_collections = 0
plaintext = loopback
[cluster]
multicast = $mc
advertise = 127.0.89.$1
mode = $2
$ex
collections = 0
pull_timeout_ms = 200
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$UP
upstream_prefixes = ro:*
upstream_timeout_ms = 1000
upstream_negative_ms = 600000
C
	chmod 600 "$D/n$1.conf"
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P${1}ID=\$!"
	i=0; while [ $i -lt 150 ] && ! grep -q "perfcached ready" "$D/n$1.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/n$1.log" || { bad "member $1 ($2) did not start: $(tail -2 "$D/n$1.log")"; echo "existsplacetest: $pass passed, $fail failed"; exit 1; }
}
st() { # st <n> <python expr over s (the member's /stats)>
	python3 - "$1" "$2" <<'PY' 2>&1
import json, sys, urllib.request
s = json.load(urllib.request.urlopen("http://127.0.0.1:2077%s/stats" % sys.argv[1], timeout=5))
print(eval(sys.argv[2]))
PY
}
fleet() { # fleet <shard|spread> - three members up and ready
	for p in $P1ID $P2ID $P3ID; do kill $p 2>/dev/null; wait $p 2>/dev/null; done
	node 1 $1; node 2 $1; node 3 $1
	i=0; while [ $i -lt 100 ] && [ "$(st 1 "s['cluster']['peers_up']")" != 2 ]; do sleep 0.2; i=$((i+1)); done
	for n in 1 2 3; do i=0; while [ $i -lt 100 ] && [ "$(st $n "s['state']")" != ready ]; do sleep 0.2; i=$((i+1)); done; done
	sleep 1
	[ "$(st 1 "s['cluster']['peers_up']") $(st 1 "s['state']") $(st 2 "s['state']") $(st 3 "s['state']")" = "2 ready ready ready" ] ||
		bad "$1: the members - peers_up $(st 1 "s['cluster']['peers_up']"), states $(st 1 "s['state']") $(st 2 "s['state']") $(st 3 "s['state']")"
}
write() { # write - the w: keys through member 1 (JSON.SET where RESP serves it); the wrong replies only
	python3 - <<'PY' 2>&1
import socket
def conn(n):
    k = socket.create_connection(("127.0.0.1", 20780 + n), 10); return k, k.makefile("rb")
def cmd(c, *a):
    k, f = c
    k.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in (y.encode() for y in a)))
    l = f.readline(); return l[:-2].decode()
c1, bad = conn(1), []
for a, want in ((("SET", "w:t", "vt", "EX", "600"), "+OK"), (("SET", "w:p", "vp"), "+OK"),
                (("HSET", "w:h", "f1", "a", "f2", "b"), ":2")):
    r = cmd(c1, *a)
    if r != want: bad.append("%s=%r" % (a[1], r))
js = [cmd(conn(n), "JSON.SET", "w:j", "$", '{"n":1,"s":"x"}') for n in (1, 2, 3)]
if "+OK" not in js: bad.append("w:j=%r" % (js,))
print(" ".join(bad))
PY
}
probe() { # probe - every key through every member; prints "<leg> <the wrong answers>" for legs 1-4
	python3 - <<'PY' 2>&1
import socket, struct
import pcnative
# key: EXISTS, TYPE, TTL (-1 none, -2 absent, a range); "copy": a fetched copy's,
# at most upstream_negative_ms while nothing tracks it, -1 where tracked
ANY = (1, 900)
WANT = {"w:t": (1, "string", (560, 600)), "w:p": (1, "string", -1), "w:h": (1, "hash", -1),
        "w:j": (1, "ReJSON-RL", -1), "w:none": (0, "none", -2),
        "ro:t": (1, "string", "copy"), "ro:p": (1, "string", "copy"), "ro:h": (1, "hash", "copy"),
        "ro:j": (1, "ReJSON-RL", "copy"), "ro:none": (0, "none", -2)}
class Resp:
    def __init__(s, n):
        s.k = socket.create_connection(("127.0.0.1", 20780 + n), 10); s.f = s.k.makefile("rb")
    def cmd(s, *a):
        s.k.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in (y.encode() for y in a)))
        l = s.f.readline(); t, r = l[:1], l[1:-2]
        return int(r) if t == b":" else (t + r).decode()
class Bin:
    def __init__(s, n):
        s.s = socket.create_connection(("127.0.0.1", 20760 + n), 10); s.b = b""; s.id = 0
    def call(s, verb, key):
        s.id += 1
        pay = bytes([verb, 1]) + struct.pack("<H", len(key)) + b"0" + key.encode()
        s.s.sendall(struct.pack("<BBBBIQ", 0x9E, 1, 1, 0, len(pay), s.id) + pay)
        while True:
            while len(s.b) < 16: s.b += s.s.recv(65536)
            typ, fl, pl = s.b[2], s.b[3], struct.unpack("<I", s.b[4:8])[0]
            while len(s.b) < 16 + pl: s.b += s.s.recv(65536)
            p, s.b = s.b[16:16 + pl], s.b[16 + pl:]
            if typ == 3: continue
            if fl & 1: return "error " + p.decode(errors="replace")
            if fl & 2: p = p[:-8]       # a parked reply's moved stamp
            return p[0] if verb == 5 else struct.unpack("<q", p[:8])[0]
def inrange(v, spec, scale=1):
    if not isinstance(v, int) or isinstance(v, bool):
        return False
    if isinstance(spec, int):
        return v == spec
    lo, hi = ANY if spec == "copy" else spec
    return (spec == "copy" and v == -1) or lo * scale <= v <= hi * scale + scale - 1
legs = {1: [], 2: [], 3: [], 4: []}
resp = {n: Resp(n) for n in (1, 2, 3)}
nat = {n: pcnative.wrap(socket.create_connection(("127.0.0.1", 20760 + n), 10)) for n in (1, 2, 3)}
binc = {n: Bin(n) for n in (1, 2, 3)}
for key, (ex, ty, tw) in WANT.items():
    leg = 1 if key.startswith("w:") else 2
    got = {n: (resp[n].cmd("EXISTS", key), resp[n].cmd("TYPE", key), resp[n].cmd("TTL", key),
               resp[n].cmd("PTTL", key)) for n in (1, 2, 3)}
    for n, (e, t, tl, pt) in got.items():
        if e != ex or t != "+" + ty or not inrange(tl, tw) or not inrange(pt, tw, 1000):
            legs[leg].append("%s@%d=%s/%s/%s/%s" % (key, n, e, t, tl, pt))
    tls = [g[2] for g in got.values() if isinstance(g[2], int)]
    if tls and (max(tls) - min(tls) > 2 or (min(tls) < 0 and len(set(tls)) > 1)):
        legs[leg].append("%s:TTLs=%s" % (key, tls))
    for n in (1, 2, 3):
        e = pcnative.call(nat[n], "exists", col="0", key=key)
        t = pcnative.call(nat[n], "ttl", col="0", key=key)
        ce = (e.get("result") or {}).get("exists", e.get("error"))
        ct = (t.get("result") or {}).get("ttl", t.get("error"))
        be, bt = binc[n].call(5, key), binc[n].call(6, key)
        if ce is not bool(ex) or not inrange(ct, tw) or be != ex or not inrange(bt, tw):
            legs[3].append("%s@%d=%s/%s/%s/%s" % (key, n, ce, ct, be, bt))
keys = list(WANT)
for n in (1, 2, 3):
    r = resp[n].cmd("EXISTS", *keys)
    if r != sum(w[0] for w in WANT.values()):
        legs[4].append("@%d=%r" % (n, r))
for l in (1, 2, 3, 4):
    print(l, " ".join(legs[l]))
PY
}
legs() { # legs <mode> <first leg number> - the probe's four lines as ok/bad
	out=$(probe)
	n=$2
	for l in 1 2 3 4; do
		r=$(echo "$out" | sed -n "s/^$l //p; s/^$l\$//p" | head -1)
		echo "$out" | grep -qE "^$l( |\$)" || r="no answer: $(echo "$out" | tail -3 | tr '\n' ' ' | cut -c1-240)"
		case $l in
		1) what="RESP EXISTS/TYPE/TTL/PTTL of written keys (TTL, none, hash, JSON, missing) through members 1-3" ;;
		2) what="RESP EXISTS/TYPE/TTL/PTTL of read-through keys through members 1-3" ;;
		3) what="native exists/ttl and the EXISTS/TTL data verbs of all of them through members 1-3" ;;
		4) what="EXISTS of all ten keys at once through members 1-3: 8" ;;
		esac
		if [ -z "$r" ]; then
			ok "$n. $1: $what - each as the key's node answers"
		else
			bad "$n. $1: $what, wrong: $(echo "$r" | cut -c1-300) ($(echo "$r" | wc -w) answers)"
		fi
		n=$((n+1))
	done
}

for mode in shard spread; do
	fleet $mode
	r=$(write)
	[ -n "$r" ] && bad "$mode: writing the keys through member 1: $(echo "$r" | cut -c1-200)"
	sleep 1
	if [ $mode = shard ]; then legs shard 1; else legs spread 5; fi
	for n in 1 2 3; do grep -q "CRIT" "$D/n$n.log" && bad "$mode: member $n logged CRIT: $(grep -m2 CRIT "$D/n$n.log" | tr '\n' ' ' | cut -c1-200)"; done
done
echo "existsplacetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
