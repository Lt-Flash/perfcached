#!/bin/sh
# metricsresettest.sh - S358: a stats reset starts /stats again and leaves
# /metrics alone.  /metrics is what Prometheus scrapes, and it must never
# read a reset as a counter going back to zero; /stats and the page are
# the human views, and they count from the last reset - which now takes
# the read-through figures with it.
#   - one node, collection 0 reading through a fake upstream
#   - hits, misses, a fetch from the upstream and an unknown command
#   - POST /reset-stats (the page's button)
#   - /stats: the collection's hits and misses and the upstream's fetches
#     are 0 again, since.reset_at is the reset's time
#   - /metrics: collection hits/misses and unknown commands are exactly
#     what they were before the reset
# Fail-first: on 0.5.6.3 /metrics drops to 0 and the read-through fetches
# survive the reset.
# Usage: test/metricsresettest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcmetricsreset.XXXXXX); P1= FPID=
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; [ -n "$FPID" ] && kill "$FPID" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
UP=18930 TCP=18931 RESP=18932 HTTP=18933
mkdir -p "$D/up" "$D/state"
echo '{"tok:a": {"type": "string", "value": "va"}}' > "$D/up/data.json"
echo '{"password": "mr-up-pass", "resp3": true}' > "$D/up/ctl.json"
python3 "$T/fakeupstream.py" $UP "$D/up" > "$D/fake.log" 2>&1 &
FPID=$!
i=0; while [ ! -f "$D/up/ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
cat > "$D/n.conf" <<CONF
[daemon]
workers = 1
state_dir = $D/state
[memory]
arena_mb = 32
[secrets]
client = mr-client-secret
upstream = mr-up-pass
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
plaintext = loopback
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$UP
upstream_prefixes = tok:*
upstream_timeout_ms = 2000
CONF
chmod 640 "$D/n.conf"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 & P1=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break; kill -0 $P1 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { bad "the daemon did not start: $(grep -m1 -E 'ERR|CRIT' "$D/n.log" | cut -c1-140)"; echo "metricsresettest: $pass passed, $fail failed"; exit 1; }

RES="$D/res"; : > "$RES"
RESFILE="$RES" RESP=$RESP HTTP=$HTTP timeout 60 python3 - <<'PY' 2> "$D/py.err"
import json, os, re, socket, time, urllib.request
res = open(os.environ["RESFILE"], "a")
def check(c, m): res.write(("P " if c else "F ") + m + "\n"); res.flush()
HTTP = int(os.environ["HTTP"])
s = socket.create_connection(("127.0.0.1", int(os.environ["RESP"])), 5)
f = s.makefile("rwb")
def cmd(*a):
    out = b"*%d\r\n" % len(a)
    for x in a:
        x = x.encode() if isinstance(x, str) else x
        out += b"$%d\r\n%s\r\n" % (len(x), x)
    s.sendall(out)
    h = f.readline(); t = h[:1]
    if t == b"$":
        n = int(h[1:]); return None if n < 0 else f.read(n + 2)[:-2]
    return h[1:].strip()
def http(path, post=False):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (HTTP, path), data=b"" if post else None, method="POST" if post else "GET")
    return urllib.request.urlopen(req, timeout=5).read().decode()
def col0(st): return [c for c in st["collections"] if c.get("name") == "0"][0]
def mval(mt, pat):
    m = re.search(pat + r" (\d+)", mt)
    return int(m.group(1)) if m else None
def munk(mt): return sum(int(v) for v in re.findall(r'perfcached_commands_unknown_total\{[^}]*\} (\d+)', mt))

check(cmd("GET", "tok:a") == b"va", "a key read through from the upstream (positive control)")
cmd("SET", "k1", "v1")
for _ in range(3): cmd("GET", "k1")              # hits
for _ in range(2): cmd("GET", "nokey")           # misses, outside the prefixes
cmd("FOOBARX")                                   # unknown
time.sleep(0.3)
st = json.loads(http("/stats")); c = col0(st); u = c.get("upstream") or {}
mt = http("/metrics")
h1 = mval(mt, r'perfcached_collection_hits_total\{collection="0"\}')
m1 = mval(mt, r'perfcached_collection_misses_total\{collection="0"\}')
u1 = munk(mt)
check(c.get("hits", 0) > 0 and c.get("misses", 0) > 0 and u.get("fetches", 0) >= 1,
      "before: /stats counts hits %s, misses %s, upstream fetches %s" % (c.get("hits"), c.get("misses"), u.get("fetches")))
check(h1 and m1 and u1, "before: /metrics counts hits %s, misses %s, unknown commands %s" % (h1, m1, u1))

r = json.loads(http("/reset-stats", post=True))
time.sleep(0.3)
st = json.loads(http("/stats")); c = col0(st); u = c.get("upstream") or {}
check(r.get("reset") is True and st.get("since", {}).get("reset_at") == r.get("at"),
      "the reset answered at %s and /stats counts from it (since.reset_at %s)" % (r.get("at"), st.get("since", {}).get("reset_at")))
check(c.get("hits") == 0 and c.get("misses") == 0,
      "after: /stats collection hits and misses start again (%s, %s)" % (c.get("hits"), c.get("misses")))
check(u.get("fetches") == 0 and u.get("absent", 0) == 0,
      "after: /stats read-through fetches start again too (%s)" % u.get("fetches"))
mt = http("/metrics")
h2 = mval(mt, r'perfcached_collection_hits_total\{collection="0"\}')
m2 = mval(mt, r'perfcached_collection_misses_total\{collection="0"\}')
u2 = munk(mt)
check(h2 == h1 and m2 == m1,
      "after: /metrics collection hits and misses did not drop (%s -> %s, %s -> %s)" % (h1, h2, m1, m2))
check(u2 == u1, "after: /metrics unknown commands did not drop (%s -> %s)" % (u1, u2))
cmd("GET", "k1")
time.sleep(0.2)
mt = http("/metrics")
check(mval(mt, r'perfcached_collection_hits_total\{collection="0"\}') == h1 + 1,
      "a hit after the reset adds to /metrics' count, it does not restart it")
PY
while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac; done < "$RES"
[ -s "$RES" ] || bad "the python driver produced nothing"
[ -s "$D/py.err" ] && { bad "the python driver raised:"; tail -3 "$D/py.err"; }
[ -s "$D/up/writes" ] && bad "a write reached the upstream: $(head -2 "$D/up/writes" | tr '\n' ' ')"
echo "metricsresettest: $pass passed, $fail failed"
[ $fail -eq 0 ]
