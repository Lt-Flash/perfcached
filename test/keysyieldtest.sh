#!/bin/sh
# keysyieldtest.sh — S40: a KEYS walk must not hold the worker.
#
# Deterministic oracle, not a latency threshold: ONE worker, a KEYS
# whose walk takes tens of milliseconds, and a PING sent on a second
# connection while the walk runs.  Serial worker (the old behaviour):
# the PONG cannot arrive until the whole KEYS reply is out.  Cooperative
# walk: the PONG overtakes it.  The margin is structural - the walk is
# hundreds of chunks long, the PING needs one turn - so neither verdict
# rides on machine speed.
#
# Also asserted, because the walk must not break what it optimises:
#  - KEYS answers are exactly right (a MATCH that hits nothing walks
#    everything and returns *0; a MATCH that hits 5 returns those 5);
#  - replies on ONE connection stay ordered: a PING pipelined BEHIND a
#    KEYS must answer after it, however long the walk takes;
#  - a client that vanishes mid-walk does not hurt the daemon.
# Usage: test/keysyieldtest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcky.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

cat > "$D/n.conf" <<EOF
[daemon]
workers = 1
log_level = info
[memory]
arena_mb = 256
[secrets]
client = ky-client-secret
cluster = ky-cluster-secret
[listen]
tcp = 127.0.0.1:17531
resp = 127.0.0.1:17532
plaintext = loopback
[collection 0]
buckets_log2 = 17
EOF
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
PID=$!
i=0
while [ $i -lt 80 ]; do
	grep -q "perfcached ready" "$D/n.log" && break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/n.log" || { echo "daemon did not start"; exit 1; }

python3 - <<'EOF'
import select, socket, sys, time

PORT = 17532
NKEYS = 300000

def conn():
    s = socket.create_connection(("127.0.0.1", PORT), 8)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(30)
    return s

def cmd(args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out

pass_n = fail_n = 0
def ok(m):
    global pass_n; pass_n += 1; print("  ok   " + m)
def bad(m):
    global fail_n; fail_n += 1; print("  FAIL " + m)

# ---- fill: pipelined SETs, no containers needed ----
s = conn()
batch = b""
for i in range(NKEYS):
    batch += cmd([b"SET", b"k:%09d" % i, b"v"])
    if len(batch) > 1 << 20:
        s.sendall(batch); batch = b""
        # drain what's ready so buffers never jam
        s.setblocking(False)
        try:
            while s.recv(1 << 20): pass
        except BlockingIOError:
            pass
        s.setblocking(True); s.settimeout(30)
for i in range(5):
    batch += cmd([b"special:%d" % i, b"v"]) if False else cmd([b"SET", b"special:%d" % i, b"v"])
s.sendall(batch + cmd([b"DBSIZE"]))
s.settimeout(30)
buf = b""
while b":%d\r\n" % (NKEYS + 5) not in buf:
    d = s.recv(1 << 20)
    if not d: sys.exit("fill connection died")
    buf = (buf + d)[-64:]
ok("filled %d keys" % (NKEYS + 5))
s.close()

# ---- 1. the yield itself: PING on conn B overtakes KEYS on conn A ----
# KEYS zzz* walks EVERY key (the filter runs post-snapshot) but
# matches none: a full-length walk with a 4-byte reply, so neither
# build can shortcut it through reply-size limits.
a, b = conn(), conn()
a.sendall(cmd([b"KEYS", b"zzz*"]))
time.sleep(0.01)                 # the walk is several times longer
b.sendall(cmd([b"PING"]))
t0 = time.time()
pong = b.recv(64)
t_pong = time.time() - t0
# The ordering ORACLE, not a stopwatch: on a serial worker the KEYS
# reply was fully written BEFORE the PING was even read, so at PONG
# time conn A is already readable.  On a cooperative worker the walk
# (hundreds of chunks) cannot have finished, so A is silent.  This is
# an order-of-events fact; no machine-speed threshold decides it.
a_ready = bool(select.select([a], [], [], 0)[0])
if not pong.startswith(b"+PONG"):
    bad("PING got %r" % pong[:30])
elif a_ready:
    bad("PONG (%.1fms) arrived only AFTER the KEYS reply was done -"
        " the walk held the worker" % (t_pong * 1000))
else:
    ok("PONG overtook the running KEYS walk (%.1fms)" % (t_pong * 1000))
r = b""
while b"\r\n" not in r:
    r += a.recv(64)
if r == b"*0\r\n":
    ok("the no-match KEYS reply is exactly *0")
else:
    bad("no-match KEYS said %r" % r[:30])
a.sendall(cmd([b"KEYS", b"special:*"]))
r = b""
while r.count(b"\r\n") < 11:
    d = a.recv(4096)
    if not d: break
    r += d
if r.startswith(b"*5\r\n") and r.count(b"special:") == 5:
    ok("a 5-hit MATCH returns exactly its 5 keys")
else:
    bad("MATCH special:* said %r" % r[:60])
a.close(); b.close()

# ---- 2. ordering on ONE connection: pipelined PING waits for KEYS ----
a = conn()
a.sendall(cmd([b"KEYS", b"zzz*"]) + cmd([b"PING"]))
data = b""
a.settimeout(30)
while b"+PONG\r\n" not in data:
    d = a.recv(1 << 20)
    if not d: sys.exit("conn died in ordering test")
    data += d
if data == b"*0\r\n+PONG\r\n":
    ok("pipelined PING answered AFTER the KEYS reply, in order")
else:
    bad("ordering broke: %r" % data[:40])
a.close()

# ---- 3. a client that vanishes mid-walk ----
a = conn()
a.sendall(cmd([b"KEYS", b"zzz*"]))
time.sleep(0.005)
a.close()                        # gone, mid-walk
time.sleep(0.3)
b = conn()
b.sendall(cmd([b"PING"]))
if b.recv(64).startswith(b"+PONG"):
    ok("daemon healthy after a client vanished mid-walk")
else:
    bad("daemon unhealthy after mid-walk disconnect")
b.close()

print("PYRESULT %d %d" % (pass_n, fail_n))
sys.exit(1 if fail_n else 0)
EOF
rc=$?
[ $rc -eq 0 ] && ok "python assertions all passed" || bad "python assertions failed (rc=$rc)"

kill -TERM $PID 2>/dev/null
wait $PID 2>/dev/null
grep -qiE "segfault|assert|sanitizer" "$D/n.log" && bad "daemon log has faults" \
	|| ok "daemon log clean"

echo "keysyieldtest: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
