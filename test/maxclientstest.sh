#!/bin/sh
# maxclientstest.sh - S161: `max_clients` refuses the connection past the
# limit WITH A REASON, on every door, counts it, logs one line per
# transition, and keeps the HTTP door answering at the limit.
# Usage: test/maxclientstest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcmaxclients.XXXXXX); P=
trap '[ -n "$P" ] && kill -9 "$P" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
mkdir -p "$D/state"
cat > "$D/pc.conf" <<CONF
[daemon]
workers = 2
max_clients = 3
state_dir = $D/state
[memory]
arena_mb = 32
[secrets]
client = maxclients-client-secret
[listen]
tcp = 127.0.0.1:17451
resp = 127.0.0.1:17452
http = 127.0.0.1:18451
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 640 "$D/pc.conf"
"$BIN" -f "$D/pc.conf" -D > "$D/pc.log" 2>&1 & P=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/pc.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/pc.log" || { echo "  daemon did not start:"; tail -3 "$D/pc.log"; echo "maxclientstest: 0 passed, 1 failed"; exit 1; }
grep -q "clients: max_clients 3 (configured)" "$D/pc.log" && ok "boot names the limit in force" || bad "no boot line naming max_clients 3"

timeout 60 python3 - "$D" <<'PY' > "$D/py.out" 2>&1
import json, socket, sys, time, urllib.request
D = sys.argv[1]; res = []
def ok(c, m): res.append(("ok" if c else "FAIL", m))
def resp(s, *args):
    s.sendall(("*%d\r\n" % len(args)).encode() + b"".join(("$%d\r\n%s\r\n" % (len(a), a)).encode() for a in args))
    return s.recv(256)
held = []
for n in range(3):
    s = socket.create_connection(("127.0.0.1", 17452), timeout=5); held.append(s)
    ok(resp(s, "PING") == b"+PONG\r\n", "RESP connection %d answers PING" % (n + 1))
s4 = socket.create_connection(("127.0.0.1", 17452), timeout=5)
d = b""
try:
    while True:
        b = s4.recv(256)
        if not b: break
        d += b
except Exception as e:
    d += b""
ok(d == b"-ERR max number of clients reached\r\n", "the 4th RESP connection gets Redis's exact refusal then EOF (got %r)" % d)
j = socket.create_connection(("127.0.0.1", 17451), timeout=5)
j.sendall(b'{"jsonrpc":"2.0","id":7,"method":"ping"}\n')
d = b""
try:
    while True:
        b = j.recv(512)
        if not b: break
        d += b
except Exception: pass
try:
    r = json.loads(d.decode().strip().split("\n")[0]); ok(r.get("error", {}).get("code") == -32005 and "max number of clients" in r["error"]["message"], "a JSON client is refused at its first request with code -32005 (id null: answered before the request is parsed), then EOF (got %r)" % d[:120])
except Exception as e:
    ok(False, "a JSON client is refused at its first request (got %r)" % d[:120])
st = json.load(urllib.request.urlopen("http://127.0.0.1:18451/stats", timeout=5))
c = st.get("clients", {})
ok(c.get("open") == 3 and c.get("max") == 3 and c.get("refused") == 2, "/stats still answers at the limit and reads open=3 max=3 refused=2 (got %r)" % c)
m = urllib.request.urlopen("http://127.0.0.1:18451/metrics", timeout=5).read().decode()
ok("perfcached_clients_refused_total 2" in m and "perfcached_max_clients 3" in m, "/metrics carries the refusals and the limit")
for s in held: s.close()
time.sleep(0.5)
s5 = socket.create_connection(("127.0.0.1", 17452), timeout=5)
ok(resp(s5, "PING") == b"+PONG\r\n", "after the three close, a new connection is admitted again")
s5.close()
for k, m in res: print("  %-4s %s" % (k, m))
print("PYDONE %d" % sum(1 for k, _ in res if k == "FAIL"))
PY
grep -E "^  (ok|FAIL) " "$D/py.out"
pf=$(grep -oE "^PYDONE [0-9]+" "$D/py.out" | awk '{print $2}'); [ -n "$pf" ] || { bad "the python driver did not finish: $(tail -3 $D/py.out)"; pf=0; }
pass=$((pass + $(grep -c "^  ok " "$D/py.out"))); fail=$((fail + ${pf:-0}))
sleep 0.5
[ "$(grep -c "max_clients 3 reached" "$D/pc.log")" = 1 ] && ok "one WARNING for the whole burst" || bad "$(grep -c 'max_clients 3 reached' $D/pc.log) WARNING line(s) for two refusals, want 1"
grep -q "below max_clients 3 again" "$D/pc.log" && ok "one NOTICE when it cleared" || bad "no NOTICE when the limit cleared"
echo "maxclientstest: $pass passed, $fail failed"
[ $fail -eq 0 ]
