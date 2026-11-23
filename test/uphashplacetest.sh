#!/bin/sh
# uphashplacetest.sh - a hash or JSON read through a shard non-owner or spread non-holder reads what the key's
# own node holds or fetches, read-through or not.  Usage: test/uphashplacetest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
D=$(mktemp -d /var/tmp/pcuphp.XXXXXX)
UP=20710
FPID= P1ID= P2ID= P3ID=
trap 'for p in $P1ID $P2ID $P3ID $FPID; do kill -9 $p 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $UP 20719 20721 20722 20723 20729 20731 20732 20733 20741 20742 20743; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "uphashplacetest: port $p already bound" >&2; exit 1; }
done
mkdir -p "$D/up"
python3 -c 'import json,sys; d = {"ro:h%d" % i: {"type": "hash", "value": {"f1": "a%d" % i, "f2": "b%d" % i}} for i in range(1, 13)}; d.update({"ro:j%d" % i: {"type": "json", "value": {"n": i, "s": "x%d" % i}} for i in range(1, 13)}); json.dump(d, open(sys.argv[1], "w"))' "$D/up/data.json"
echo '{"role": "master"}' > "$D/up/ctl.json"
python3 "$T/fakeupstream.py" $UP "$D/up" > "$D/fake.log" 2>&1 & FPID=$!
i=0; while [ ! -f "$D/up/ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done

node() { # node <n> <shard|spread> - tcp 2072n, http 2073n, resp 2074n
	rm -rf "$D/state$1"; mkdir -p "$D/state$1"
	if [ "$2" = shard ]; then mc=239.255.79.74:20719 ex=; else mc=239.255.79.75:20729 ex="replicas = 2"; fi
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state$1
[memory]
arena_mb = 32
[secrets]
client = uphp-client-secret
cluster = uphp-cluster-secret
[listen]
tcp = 127.0.0.1:2072$1
http = 127.0.0.1:2073$1
resp = 127.0.0.1:2074$1
resp_collections = 0
plaintext = loopback
[cluster]
multicast = $mc
advertise = 127.0.87.$1
mode = $2
$ex
collections = 0
pull_timeout_ms = 200
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$UP
upstream_prefixes = ro:*
upstream_timeout_ms = 1000
upstream_negative_ms = 5000
C
	chmod 600 "$D/n$1.conf"
	: > "$D/n$1.log"
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P${1}ID=\$!"
	i=0; while [ $i -lt 150 ] && ! grep -q "perfcached ready" "$D/n$1.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/n$1.log" || { bad "member $1 ($2) did not start: $(tail -2 "$D/n$1.log")"; echo "uphashplacetest: $pass passed, $fail failed"; exit 1; }
}
st() { # st <n> <python expr over s (the member's /stats)>
	python3 - "$1" "$2" <<'PY' 2>&1
import json, sys, urllib.request
s = json.load(urllib.request.urlopen("http://127.0.0.1:2073%s/stats" % sys.argv[1], timeout=5))
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
resp() { # resp <n> <mode>: hgetall <prefix> | hget | hset <prefix> - the wrong answers only
	python3 - "$1" "$2" "${3:-}" <<'PY' 2>&1
import socket, sys
n, mode, pre = int(sys.argv[1]), sys.argv[2], sys.argv[3]
k = socket.create_connection(("127.0.0.1", 20740 + n), 10); f = k.makefile("rb")
def enc(*a):
    return b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in (y.encode() for y in a))
def rd():
    l = f.readline(); t, r = l[:1], l[1:-2]
    if t == b":": return int(r)
    if t == b"$":
        m = int(r)
        return None if m < 0 else f.read(m + 2)[:-2].decode()
    if t in (b"*", b"%"):
        m = int(r) * (2 if t == b"%" else 1)
        return None if m < 0 else [rd() for _ in range(m)]
    if t == b"_": return None
    return (t + r).decode()
def cmd(*a):
    k.sendall(enc(*a)); return rd()
bad = []
for i in range(1, 13):
    key = "%s%d" % (pre, i)
    if mode == "hset":
        r = cmd("HSET", key, "f1", "a%d" % i, "f2", "b%d" % i)
        if r != 2: bad.append("%s=%r" % (key, r))
    elif mode == "hgetall":
        r = cmd("HGETALL", key)
        got = dict(zip(r[0::2], r[1::2])) if isinstance(r, list) else r
        if got != {"f1": "a%d" % i, "f2": "b%d" % i}: bad.append("%s=%r" % (key, got))
    else:
        r = cmd("HGET", key, "f2")
        if r != "b%d" % i: bad.append("%s.f2=%r" % (key, r))
if mode == "hget":
    r = cmd("HGETALL", "ro:none")
    if r not in ([], {}): bad.append("ro:none=%r" % (r,))
print(" ".join(bad))
PY
}
jdocs() { # jdocs <n> <prefix> <get|set> - JSON.GET and jget (or JSON.SET) of 12 documents, the wrong ones only
	python3 - "$1" "$2" "$3" <<'PY' 2>&1
import json, socket, sys
import pcnative
n, pre, mode = int(sys.argv[1]), sys.argv[2], sys.argv[3]
k = socket.create_connection(("127.0.0.1", 20740 + n), 10); f = k.makefile("rb")
def cmd(*a):
    k.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in (y.encode() for y in a)))
    l = f.readline(); t, r = l[:1], l[1:-2]
    if t == b"$":
        m = int(r)
        return None if m < 0 else f.read(m + 2)[:-2].decode()
    return (t + r).decode()
nat = pcnative.wrap(socket.create_connection(("127.0.0.1", 20720 + n), 10))
bad = []
for i in range(1, 13):
    key, want = "%s%d" % (pre, i), {"n": i, "s": "x%d" % i}
    if mode == "set":
        r = cmd("JSON.SET", key, "$", json.dumps(want))
        if r != "+OK": bad.append("%s=%r" % (key, r))
        continue
    r = cmd("JSON.GET", key)
    try: ok = json.loads(r) == want
    except Exception: ok = False
    if not ok: bad.append("RESP %s=%r" % (key, r))
    j = pcnative.call(nat, "jget", col="0", key=key)
    res = j.get("result") or {}
    try: ok = res.get("found") and json.loads(res.get("value")) == want
    except Exception: ok = False
    if not ok: bad.append("jget %s=%r" % (key, j.get("error") or res))
print(" ".join(bad))
PY
}
sum3() { echo $(( $(st 1 "$1") + $(st 2 "$1") + $(st 3 "$1") )); }
UPF="[c for c in s['collections'] if c['name']=='0'][0]['upstream']['fetches']"

# ---- shard ---------------------------------------------------------------------------
fleet shard
r=$(resp 1 hgetall ro:h)
[ -z "$r" ] && ok "1. shard: HGETALL of 12 hashes through member 1, every field" ||
	bad "1. shard: HGETALL through member 1, wrong: $(echo "$r" | cut -c1-240) ($(echo "$r" | wc -w) of 12)"
r=$(resp 1 hget ro:h)
[ -z "$r" ] && ok "2. shard: HGET of each through member 1 right, an absent hash empty" ||
	bad "2. shard: HGET through member 1, wrong: $(echo "$r" | cut -c1-240)"
f=$(sum3 "$UPF"); pf=$(( $(st 2 "s['cluster']['pull_fetching']") + $(st 3 "s['cluster']['pull_fetching']") ))
[ "$f" = 13 ] && [ "$pf" -gt 0 ] && ok "3. shard: 13 fetches in all (12 hashes, the absent one), members 2 and 3 fetched for $pf pulls" ||
	bad "3. shard: fetches $f (13: 12 hashes and ro:none), pull_fetching on 2 and 3: $pf (over 0)"
r=$(jdocs 1 ro:j get)
[ -z "$r" ] && ok "6. shard: 12 read-through documents through member 1, jget and JSON.GET" ||
	bad "6. shard: documents through member 1, wrong: $(echo "$r" | cut -c1-260) ($(echo "$r" | wc -w) of 24)"

# ---- spread --------------------------------------------------------------------------
fleet spread
r=$(resp 1 hgetall ro:h)
[ -z "$r" ] && ok "4. spread: HGETALL of 12 read-through hashes through member 1, every field" ||
	bad "4. spread: HGETALL through member 1, wrong: $(echo "$r" | cut -c1-240) ($(echo "$r" | wc -w) of 12)"
r=$(resp 2 hset w:h)
if [ -n "$r" ]; then
	bad "5. spread: HSET through member 2: $(echo "$r" | cut -c1-200)"
else
	sleep 1
	r1=$(resp 1 hgetall w:h); r3=$(resp 3 hgetall w:h)
	[ -z "$r1$r3" ] && ok "5. spread: 12 hashes written through member 2 read whole through members 1 and 3" ||
		bad "5. spread: written through member 2, wrong through 1: $(echo "$r1" | wc -w) ($(echo "$r1" | cut -c1-120)), through 3: $(echo "$r3" | wc -w)"
fi

r=$(jdocs 2 w:j set)
if [ -n "$r" ]; then
	bad "7. spread: JSON.SET through member 2: $(echo "$r" | cut -c1-200)"
else
	sleep 1
	r1=$(jdocs 1 w:j get); r3=$(jdocs 3 w:j get)
	[ -z "$r1$r3" ] && ok "7. spread: 12 documents written through member 2 read through members 1 and 3, both doors" ||
		bad "7. spread: written through member 2, wrong through 1: $(echo "$r1" | wc -w) ($(echo "$r1" | cut -c1-160)), through 3: $(echo "$r3" | wc -w)"
fi

for n in 1 2 3; do grep -h 'an fsync has been stuck' "$D/n$n.log" 2>/dev/null | head -n 1 | sed "s/^/  ..   not counted, the runner's storage stalled: /"; grep "CRIT" "$D/n$n.log" | grep -qv 'an fsync has been stuck' && bad "member $n logged CRIT: $(grep CRIT "$D/n$n.log" | grep -v 'an fsync has been stuck' | head -n 2 | tr '\n' ' ' | cut -c1-200)"; done
echo "uphashplacetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
