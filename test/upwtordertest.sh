#!/bin/sh
# upwtordertest.sh - S429 (0.7.1; 0.7.1-rc1's upwritethroughtest 15, under
# ASan): writes to one key from many clients at once, written through.  The
# read-backs come back from Redis in the order Redis applied the writes, but
# each is applied on the worker that queued its write - two could be applied
# the other way round, and the node keep the older value ("Redis v46, node
# v45") with nothing left to correct it.  S429 stamps each completion in the
# writer's order and skips a read-back older than one already applied.
#   A. 30 rounds of 8 clients writing one key at once behind a slow master:
#      after each round the node answers what Redis holds
#   B. stale_readbacks_skipped counts the read-backs that came in late (the
#      race this suite exists for happened - a pass on a quiet runner where
#      it never did is reported, not counted as proof)
# Usage: test/upwtordertest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcwtord.XXXXXX)
PM=17981 P1=17982 TCP=17984 RESP=17985 HTTP=17986
PID= FM= F1=
trap 'kill $PID 2>/dev/null; for p in $FM $F1; do kill -9 $p 2>/dev/null; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $PM $P1 $TCP $RESP $HTTP; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upwtordertest: port $p already bound" >&2; exit 1; }
done
for n in m 1; do mkdir -p "$D/up$n"; echo '{}' > "$D/up$n/data.json"; done
echo '{"role": "master", "accept_writes": true, "write_delay_ms": 20, "replicate_to": ["'"$D"'/up1"]}' > "$D/upm/ctl.json"
echo '{"role": "replica", "master_host": "127.0.0.1", "master_port": '$PM'}' > "$D/up1/ctl.json"
python3 "$T/fakeupstream.py" $PM "$D/upm" > "$D/fm.log" 2>&1 & FM=$!
python3 "$T/fakeupstream.py" $P1 "$D/up1" > "$D/f1.log" 2>&1 & F1=$!
i=0; while [ $i -lt 50 ] && ! { [ -f "$D/upm/ready" ] && [ -f "$D/up1/ready" ]; }; do sleep 0.1; i=$((i+1)); done
mkdir -p "$D/state"
cat > "$D/n.conf" <<C
[daemon]
workers = 4
log_level = notice
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = wtord-client-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
plaintext = loopback
[collection 0]
buckets_log2 = 10
upstream = redis://127.0.0.1:$P1, redis://127.0.0.1:$PM
upstream_role = replica
upstream_writes = through
upstream_prefixes = *
upstream_timeout_ms = 5000
upstream_negative_ms = 1000
C
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 & PID=$!
i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/n.log"; do sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { bad "the node did not start: $(tail -3 "$D/n.log")"; exit 1; }
wt() { python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())
w=[c for c in s["collections"] if c["name"]=="0"][0]["upstream"]["write_through"]
print(eval(sys.argv[1]))' "$1" 2>/dev/null; }
i=0; while [ $i -lt 50 ] && [ "$(wt 'w["target"]')" != "127.0.0.1:$PM" ]; do sleep 0.1; i=$((i+1)); done
r=$(python3 - $RESP "$D/upm" <<'PY'
import json, os, socket, sys, threading, time
port, md = int(sys.argv[1]), sys.argv[2]
def cmd(*a):
    s = socket.create_connection(("127.0.0.1", port), 20); f = s.makefile("rb")
    s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
    l = f.readline()
    v = f.read(int(l[1:]) + 2)[:-2].decode() if l.startswith(b"$") and not l.startswith(b"$-1") else l.decode().strip()
    s.close(); return v
bad = []
for rnd in range(30):
    t = [threading.Thread(target=cmd, args=(b"SET", b"ord:k", b"r%dv%d" % (rnd, i))) for i in range(8)]
    [x.start() for x in t]; [x.join() for x in t]
    time.sleep(0.3)                     # the echoes and any refetch land
    redis = json.load(open(md + "/data.json")).get("ord:k", {}).get("value")
    node = cmd(b"GET", b"ord:k")
    if node != redis:
        time.sleep(1.0); node2 = cmd(b"GET", b"ord:k")
        bad.append("round %d: Redis %s, node %s (1 s later %s)" % (rnd, redis, node, node2))
print("%d/30 rounds the node disagreed%s" % (len(bad), (" - " + bad[0]) if bad else ""))
PY
)
case "$r" in "0/30 rounds"*) ok "A. 30 rounds of 8 writers on one key: the node agrees with Redis every round";; *) bad "A. $r";; esac
st=$(wt 'w.get("stale_readbacks_skipped")')
case "$st" in
None) bad "B. no stale_readbacks_skipped in the write-through stats (a build before S429)";;
0) ok "B. stale_readbacks_skipped 0 - the race did not happen on this run (A held without it being tested)";;
*) ok "B. stale_readbacks_skipped $st - read-backs came in late and were not applied";;
esac
echo "upwtordertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
