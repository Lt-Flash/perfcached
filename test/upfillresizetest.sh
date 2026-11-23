#!/bin/sh
# upfillresizetest.sh - the upstream threads read collection tables they found lock-free: a resize
# while the upfill sweep runs must not leave the sweep on the old table (retired, back in the arena).
# Collection 0 writes through to fake master M.  Usage: test/upfillresizetest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=$(readlink -f "${1:-./perfcached}")
T=$(cd "$(dirname "$0")" && pwd)
D=$(mktemp -d /var/tmp/pcufr.XXXXXX)
PM=16641 TCP=16642 RESP=16643 HTTP=16644
PID= FM=
trap 'kill -9 $PID $FM 2>/dev/null; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in $PM $TCP $RESP $HTTP; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "upfillresizetest: port $p already bound" >&2; exit 1; }
done
N=400
mkdir -p "$D/upm" "$D/state" "$D/wal"
echo '{}' > "$D/upm/data.json"
echo '{"role": "master", "accept_writes": true, "tracking": false}' > "$D/upm/ctl.json"
python3 "$T/fakeupstream.py" $PM "$D/upm" > "$D/fm.log" 2>&1 & FM=$!
i=0; while [ $i -lt 50 ] && [ ! -f "$D/upm/ready" ]; do sleep 0.1; i=$((i+1)); done
cat > "$D/n.conf" <<C
[daemon]
workers = 4
log_level = notice
state_dir = $D/state
allow_create = yes
[memory]
arena_mb = 64
[secrets]
client = ufr-client-secret
enable = ufr-enable-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
plaintext = loopback
[wal]
dir = $D/wal
probe = no
fsync = always
save = off
[collection 0]
buckets_log2 = 8
upstream = redis://127.0.0.1:$PM
upstream_writes = allow
upstream_prefixes = *
upstream_timeout_ms = 1000
upstream_drain_rate = 40
C
chmod 600 "$D/n.conf"
start() { # start [VAR=value ...]
	env "$@" "$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 & PID=$!
	i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/n.log"; do
		kill -0 $PID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/n.log"
}
alive() { kill -0 $PID 2>/dev/null && ! grep -q '^State:[[:space:]]*Z' /proc/$PID/status 2>/dev/null; }
wt() { python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:'$HTTP'/stats", timeout=5).read())
w=[c for c in s["collections"] if c["name"]=="0"][0]["upstream"]["write_through"]
print(eval(sys.argv[1]))' "$1" 2>/dev/null; }
call() { # call <method> <json params>: one privileged native call, its result or error
	python3 -c 'import json,socket,sys,pcnative
f=pcnative.wrap(socket.create_connection(("127.0.0.1",int(sys.argv[1])),10))
pcnative.call(f,"enable",secret="ufr-enable-secret")
r=pcnative.call(f,sys.argv[2],**json.loads(sys.argv[3]))
print(json.dumps(r.get("result", r.get("error"))))' $TCP "$1" "$2" 2>&1; }

start || { bad "the node did not start (allow): $(tail -3 "$D/n.log")"; echo "upfillresizetest: $pass passed, $fail failed"; exit 1; }
# the fleet's own keys, written under allow: none reaches M
python3 -c 'import socket, sys
n = int(sys.argv[2]); s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 20)
s.sendall(b"".join(b"*3\r\n$3\r\nSET\r\n$%d\r\nu:%d\r\n$2\r\nv1\r\n" % (len(b"u:%d" % i), i) for i in range(1, n + 1)))
f = s.makefile("rb")
print(sum(f.readline() == b"+OK\r\n" for _ in range(n)))' $RESP $N > "$D/sets" 2>&1
[ "$(cat "$D/sets")" = $N ] || bad "setup: $N SETs under allow answered OK $(cat "$D/sets")"
kill $PID; wait $PID 2>/dev/null; PID=
sed -i 's/^upstream_writes = allow$/upstream_writes = through/' "$D/n.conf"
start PERFCACHED_TEST_UPFILL_CTL_MS=300 || { bad "the node did not restart (through): $(tail -3 "$D/n.log")"; echo "upfillresizetest: $pass passed, $fail failed"; exit 1; }
i=0; while [ $i -lt 50 ] && [ "$(wt 'w["target"]')" != "127.0.0.1:$PM" ]; do sleep 0.1; i=$((i+1)); done

echo "--- 1: eight upfill calls at once, on four workers"
# PERFCACHED_TEST_UPFILL_CTL_MS=300: each start waits between its checks and the start; one sweep
# may run, the others are told it already does
python3 -c 'import json, socket, sys, threading, pcnative
port, out = int(sys.argv[1]), []
fs = []
for _ in range(8):
    f = pcnative.wrap(socket.create_connection(("127.0.0.1", port), 10))
    pcnative.call(f, "enable", secret="ufr-enable-secret")
    fs.append(f)
def one(f):
    r = pcnative.call(f, "upfill", col="0")
    e = r.get("error")
    out.append((e.get("message") if isinstance(e, dict) else e) if e else json.dumps(r["result"]))
ts = [threading.Thread(target=one, args=(f,)) for f in fs]
[t.start() for t in ts]
[t.join() for t in ts]
print("\n".join(out))' $TCP > "$D/ctl.out" 2>&1
S=$(grep -c started "$D/ctl.out"); A=$(grep -c "already running" "$D/ctl.out")
[ "$S" = 1 ] && [ "$A" = 7 ] && ok "1. eight upfill calls at once: one started, seven told one already runs" \
	|| bad "1. eight upfill calls at once: $S started, $A already running ($(sort "$D/ctl.out" | uniq -c | tr '\n' ' '))"
call upfill '{"col": "0", "stop": true}' > /dev/null
i=0; while [ $i -lt 100 ] && [ "$(wt 'w["upfill"]["state"]')" = running ]; do sleep 0.1; i=$((i+1)); done

echo "--- 2: a resize while the upfill sweep runs ($N keys at 40 a second)"
R=$(call upfill '{"col": "0"}')
echo "$R" | grep -q started || bad "2. upfill: $R"
sleep 1
RS=$(call resize '{"col": "0", "buckets_log2": 11}')
echo "$RS" | grep -q '"resizing": true' || bad "2. resize: $RS"
i=0; while [ $i -lt 400 ] && alive && [ "$(wt 'w["upfill"]["state"]')" != done ]; do sleep 0.1; i=$((i+1)); done
ST="state $(wt 'w["upfill"]["state"]') found $(wt 'w["upfill"]["found"]') written $(wt 'w["upfill"]["written"]') gone $(wt 'w["upfill"]["gone"]') errors $(wt 'w["upfill"]["errors"]')"
M=$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(sum(1 for k in d if k.startswith("u:")))' "$D/upm/data.json" 2>&1)
if ! alive; then
	bad "2. the node died during the sweep: $(grep -m3 -E 'ERROR|CRIT|AddressSanitizer|SEGV' "$D/n.log" | tr '\n' ' ')"
elif [ "$(wt 'w["upfill"]["state"]')" = done ] && [ "$(wt 'w["upfill"]["gone"]')" = 0 ] && [ "$M" = $N ]; then
	ok "2. the sweep read the resized collection to its end: every key on M ($ST)"
else
	bad "2. after the resize the sweep lost keys: $ST, M holds $M of $N"
fi
E=$(grep -c -E 'ERROR|CRIT|AddressSanitizer' "$D/n.log")
[ "$E" = 0 ] && ok "2. no ERROR, CRIT or sanitizer line in the node's log" || bad "2. the log: $(grep -m3 -E 'ERROR|CRIT|AddressSanitizer' "$D/n.log" | tr '\n' ' ')"
kill $PID 2>/dev/null; wait $PID 2>/dev/null; PID=

echo "upfillresizetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
