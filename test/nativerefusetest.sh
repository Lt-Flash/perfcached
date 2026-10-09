#!/bin/sh
# nativerefusetest.sh - S368: an ENCRYPTED native client past the limit is
# refused at accept, before the Noise handshake - the daemon pays no crypto
# for a connection it will not keep.  10-10 on 245: 38,666 refused native
# clients each cost a full handshake (the refusal came at the first request,
# inside the channel), and the RESP probe's p99 rose 4 -> 37 ms beside them.
# It is told what a RESP client is told - "-ERR max number of clients
# reached" - and closed; a library that does not read that text sees the
# handshake fail, as on any close.  A PLAINTEXT native door (loopback, unix)
# keeps the error frame at the first request: it does no crypto
# (maxclientstest).  Both refusals: max_clients and the memory floor (S366).
# Fail-first: 0.5.6.5 accepts the client and waits for its handshake.
# Usage: test/nativerefusetest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcnatrefuse.XXXXXX); P=
trap '[ -n "$P" ] && kill -9 "$P" 2>/dev/null && wait "$P" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

conf() {   # conf <extra [daemon] line>
	rm -rf "$D/state"; mkdir -p "$D/state"
	cat > "$D/pc.conf" <<CONF
[daemon]
workers = 2
state_dir = $D/state
$1
[memory]
arena_mb = 32
[secrets]
client = natrefuse-client-secret
[listen]
tcp = 127.0.0.1:18351
http = 127.0.0.1:18353
plaintext = never
[collection 0]
buckets_log2 = 12
CONF
	chmod 640 "$D/pc.conf"
}
start() {
	"$BIN" -f "$D/pc.conf" -D > "$D/pc.log" 2>&1 & P=$!
	i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/pc.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "  daemon did not start:"; tail -3 "$D/pc.log"; return 1
}
stop() { [ -n "$P" ] && kill "$P" 2>/dev/null; wait "$P" 2>/dev/null; P=; }

probe() {  # probe <label> - A holds the one slot (or the floor is reached), B must be refused before any handshake
	timeout 30 python3 - "$1" <<'PY'
import json, socket, sys, time, urllib.request
label = sys.argv[1]
a = socket.create_connection(("127.0.0.1", 18351), timeout=5)     # sends nothing: counted, waits for its handshake
time.sleep(0.3)
b = socket.create_connection(("127.0.0.1", 18351), timeout=2)
got, how = b"", "EOF"
try:
    while True:
        x = b.recv(256)
        if not x: break
        got += x
except socket.timeout:
    how = "timeout - the daemon is waiting for a handshake"
except ConnectionResetError:
    how = "reset"
c = json.load(urllib.request.urlopen("http://127.0.0.1:18353/stats", timeout=5))["clients"]
want = b"-ERR max number of clients reached\r\n"
print("  %-4s %s: a client past it is refused before the handshake - told %r, then %s" % ("ok" if got == want and how == "EOF" else "FAIL", label, got, how))
print("  %-4s %s: /stats counts it (refused %d, open %d)" % ("ok" if c.get("refused") == 1 and c.get("open") == 1 else "FAIL", label, c.get("refused", -1), c.get("open", -1)))
a.close(); b.close()
PY
}

# ---- max_clients ------------------------------------------------------------
conf "max_clients = 1"
start || { echo "nativerefusetest: 0 passed, 1 failed"; exit 1; }
probe "max_clients = 1" > "$D/py.out" 2>&1; cat "$D/py.out"
pass=$((pass + $(grep -c "^  ok " "$D/py.out"))); fail=$((fail + $(grep -c "^  FAIL " "$D/py.out")))
[ "$(grep -c '^  ok \|^  FAIL ' "$D/py.out")" = 2 ] || bad "max_clients probe did not finish: $(tail -2 "$D/py.out")"
grep -q "handshake" "$D/pc.log" && bad "the refused client reached the handshake: $(grep handshake "$D/pc.log" | head -1)" || ok "nothing about a handshake in the log"
stop

# ---- the memory floor (S366)
printf 'MemTotal:        1048576 kB\nMemAvailable:     409600 kB\n' > "$D/meminfo"
mkdir -p "$D/cg"; echo "0::/none" > "$D/cgroup"
conf ""
export PERFCACHED_TEST_MEMINFO=$D/meminfo PERFCACHED_TEST_PROC_CGROUP=$D/cgroup PERFCACHED_TEST_CGROUP_ROOT=$D/cg
start || { echo "nativerefusetest: $pass passed, $((fail+1)) failed"; exit 1; }
# A gets in while there is room; then the squeeze (100 MB, under the 153 MB floor), then B
timeout 30 python3 - "$D" <<'PY' > "$D/py2.out" 2>&1
import json, os, socket, sys, time, urllib.request
D = sys.argv[1]
def budget(): return json.load(urllib.request.urlopen("http://127.0.0.1:18353/stats", timeout=3))["clients"]["memory"]["budget"]
end = time.time() + 5
while budget() <= 0 and time.time() < end: time.sleep(0.2)
a = socket.create_connection(("127.0.0.1", 18351), timeout=5)
open(D + "/meminfo.tmp", "w").write("MemTotal:        1048576 kB\nMemAvailable:     102400 kB\n"); os.rename(D + "/meminfo.tmp", D + "/meminfo")
end = time.time() + 5
while budget() > 0 and time.time() < end: time.sleep(0.2)
b = socket.create_connection(("127.0.0.1", 18351), timeout=2)
got, how = b"", "EOF"
try:
    while True:
        x = b.recv(256)
        if not x: break
        got += x
except socket.timeout:
    how = "timeout - the daemon is waiting for a handshake"
except ConnectionResetError:
    how = "reset"
m = json.load(urllib.request.urlopen("http://127.0.0.1:18353/stats", timeout=5))["clients"]["memory"]
want = b"-ERR max number of clients reached\r\n"
print("  %-4s memory floor: a client under it is refused before the handshake - told %r, then %s" % ("ok" if got == want and how == "EOF" else "FAIL", got, how))
print("  %-4s memory floor: counted at the floor (refused %d)" % ("ok" if m.get("refused", 0) >= 1 else "FAIL", m.get("refused", -1)))
a.close(); b.close()
PY
cat "$D/py2.out"
pass=$((pass + $(grep -c "^  ok " "$D/py2.out"))); fail=$((fail + $(grep -c "^  FAIL " "$D/py2.out")))
[ "$(grep -c '^  ok \|^  FAIL ' "$D/py2.out")" = 2 ] || bad "memory-floor probe did not finish: $(tail -2 "$D/py2.out")"
stop
echo "nativerefusetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
