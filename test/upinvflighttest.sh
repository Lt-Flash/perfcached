#!/bin/sh
# upinvflighttest.sh - a read-through fetch overtaken by an invalidation of its key is refetched, and
# a copy kept for waiters is capped as an untracked fill.  Usage: test/upinvflighttest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcinvfl.XXXXXX)
UP=17941 RESP=17942 HTTP=17943 TCP=17944
PID= FP=
trap 'kill $PID 2>/dev/null; kill -9 $FP 2>/dev/null; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $UP $RESP $HTTP $TCP; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upinvflighttest: port $p already bound" >&2; exit 1; }
done
mkdir -p "$D/up" "$D/state"
python3 - "$D/up" <<'PY'
import json, sys
d = sys.argv[1]
x = {"inf:%d" % i: {"type": "string", "value": "old"} for i in range(10)}
x["inf:churn"] = {"type": "string", "value": "c0"}          # D
json.dump(x, open(d + "/data.json", "w"))
json.dump({"password": "infl-up-pw", "resp3": True, "reply_delay_ms": 400,
           "reply_delay_keys": ["inf:%d" % i for i in range(10)] + ["inf:churn"],
           "reply_delay_cmds": ["GET"]}, open(d + "/ctl.json", "w"))
PY
python3 "$T/fakeupstream.py" $UP "$D/up" > "$D/fake.log" 2>&1 & FP=$!
i=0; while [ $i -lt 50 ] && [ ! -f "$D/up/ready" ]; do sleep 0.1; i=$((i+1)); done
cat > "$D/n.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = infl-client-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
plaintext = loopback
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$UP
upstream_password = infl-up-pw
upstream_prefixes = inf:*
upstream_timeout_ms = 2000
upstream_negative_ms = 3000
C
chmod 600 "$D/n.conf"
: > "$D/n.log"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 & PID=$!
i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/n.log"; do sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { bad "the node did not start: $(tail -3 "$D/n.log")"; exit 1; }
up() { python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())
u=[c for c in s["collections"] if c["name"]=="0"][0]["upstream"]
print(u.get(sys.argv[1]))' "$1" 2>/dev/null; }
i=0; while [ $i -lt 50 ] && [ "$(up tracking)" != True ]; do sleep 0.1; i=$((i+1)); done
[ "$(up tracking)" = True ] || bad "tracking never came up: $(up tracking)"
r=$(python3 - $RESP "$D/up" <<'PY'
import json, os, socket, sys, threading, time
port, d = int(sys.argv[1]), sys.argv[2]
def conn():
    k = socket.create_connection(("127.0.0.1", port), 5); k.settimeout(5); return k, k.makefile("rb")
def get(c, key):
    k, f = c
    k.sendall(b"*2\r\n$3\r\nGET\r\n$%d\r\n%s\r\n" % (len(key), key.encode()))
    l = f.readline()
    if l.startswith(b"$-1") or l.startswith(b"_"): return None
    if l.startswith(b"$"): return f.read(int(l[1:]) + 2)[:-2].decode()
    return l.decode().strip()
seq = [0]
def change(key):
    p = d + "/data.json"; x = json.load(open(p)); x[key]["value"] = "new"
    json.dump(x, open(p + ".t", "w")); os.replace(p + ".t", p)
    p = d + "/ctl.json"; c = json.load(open(p)); seq[0] += 1
    c["inv_seq"] = seq[0]; c["invalidate"] = [key]
    json.dump(c, open(p + ".t", "w")); os.replace(p + ".t", p)
stale = []
for i in range(10):
    key = "inf:%d" % i
    first = {}
    t = threading.Thread(target=lambda: first.__setitem__("v", get(conn(), key)))
    t.start(); time.sleep(0.15)          # the fetch has read "old" and sleeps
    change(key); t0 = time.time()        # the key changes; the push goes out (the fake's 50 ms watcher)
    t.join()
    c = conn(); seen = []
    time.sleep(max(0, 0.6 - (time.time() - t0)))
    while time.time() - t0 < 2.0:
        seen.append(get(c, key)); time.sleep(0.1)
    if any(v != "new" for v in seen):
        stale.append((key, first.get("v"), [v for v in seen if v != "new"][:2]))
print("%d/10 stale%s" % (len(stale), (" - first: %s answered %r, then %r" % stale[0]) if stale else ""))
PY
)
[ "$r" = "0/10 stale" ] && ok "A. a fetch overtaken by its key's invalidation: every read 0.6-2 s after the change answers the new value ($r)" || bad "A. stale after the change: $r"
ii=$(up inflight_invalidated); ir=$(up inflight_refetches); ik=$(up inflight_kept)
[ "${ii:-0}" -ge 10 ] 2>/dev/null && [ "${ir:-0}" -ge 10 ] 2>/dev/null && [ "${ik:-x}" = 0 ] && ok "B. the race happened and was handled: inflight_invalidated $ii, inflight_refetches $ir, kept $ik" || bad "B. inflight_invalidated '$ii', inflight_refetches '$ir', inflight_kept '$ik'"
[ ! -s "$D/up/writes" ] && ok "C. no write reached the upstream" || bad "C. WRITES REACHED THE UPSTREAM: $(head -3 "$D/up/writes" | tr '\n' '|')"
k0=$(up inflight_kept)
r=$(python3 - $RESP "$D/up" $HTTP <<'PY'
import json, os, socket, sys, threading, time, urllib.request
port, d, http = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])
key = "inf:churn"
def get(key):
    k = socket.create_connection(("127.0.0.1", port), 5); k.settimeout(5); f = k.makefile("rb")
    k.sendall(b"*2\r\n$3\r\nGET\r\n$%d\r\n%s\r\n" % (len(key), key.encode()))
    l = f.readline()
    if l.startswith(b"$-1") or l.startswith(b"_"): return None
    if l.startswith(b"$"): return f.read(int(l[1:]) + 2)[:-2].decode()
    return l.decode().strip()
def refetches():
    s = json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % http, timeout=5).read())
    return [c for c in s["collections"] if c["name"] == "0"][0]["upstream"].get("inflight_refetches", 0)
seq = [100]
def change(v):
    p = d + "/data.json"; x = json.load(open(p)); x[key]["value"] = v
    json.dump(x, open(p + ".t", "w")); os.replace(p + ".t", p)
    p = d + "/ctl.json"; c = json.load(open(p)); seq[0] += 1
    c["inv_seq"] = seq[0]; c["invalidate"] = [key]
    json.dump(c, open(p + ".t", "w")); os.replace(p + ".t", p)
first = {}
r0 = refetches()
t = threading.Thread(target=lambda: first.__setitem__("v", get(key)))
t.start()
n = 0
t0 = time.time()
# a change every 150 ms overtakes each 400 ms reply until the last round has started;
# then ONE change while that round is in flight, and none after it
while refetches() - r0 < 3 and time.time() - t0 < 10:
    time.sleep(0.15); n += 1; change("c%d" % n)
time.sleep(0.15); n += 1; change("c%d" % n)
last = "c%d" % n
t.join()
time.sleep(6.0)
print("%s %s %s" % (last, get(key), first.get("v")))
PY
)
k1=$(up inflight_kept)
set -- $r
[ "$#" = 3 ] && [ "$1" = "$2" ] && [ "${k1:-0}" -gt "${k0:-0}" ] 2>/dev/null &&
	ok "D. four rounds overtaken, the copy kept for its waiters (inflight_kept $k0 -> $k1, the first read answered $3), and 6 s after the last change ($1) the node answers it" ||
	bad "D. after the last change ($1) the node answers '$2' (inflight_kept $k0 -> $k1; first read $3)"
echo "upinvflighttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
