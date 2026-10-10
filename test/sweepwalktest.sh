#!/bin/sh
# sweepwalktest.sh - S367: a busy client is not slowed by idle ones.  The
# HTTP sweep walked EVERY connection on its worker on EVERY turn - with no
# HTTP connection open at all - so each request on a node holding many
# clients paid a walk of all of them (50,000 on 245: 12,500 list nodes a
# turn a worker).  One worker here, so every connection shares the list:
# the median round trip of sequential PINGs with 20,000 idle connections
# held must stay within 3x (+50 us - a busy CI runner is noisy; the defect
# is 12x) of the same client's median with none.  Fail-first: 0.5.6.4 walks
# 20,000 nodes per PING - 60 -> 750 us on 222.
# Usage: test/sweepwalktest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcsweepwalk.XXXXXX); P=
# wait for the daemon too: closing 20,000 sockets takes a while, and the next
# suite may want a port it still holds
trap '[ -n "$P" ] && kill -9 "$P" 2>/dev/null && wait "$P" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
mkdir -p "$D/state"
cat > "$D/pc.conf" <<CONF
[daemon]
workers = 1
state_dir = $D/state
[memory]
arena_mb = 32
[secrets]
client = sweepwalk-client-secret
[listen]
tcp = 127.0.0.1:18334
resp = 127.0.0.1:18335
http = 127.0.0.1:18336
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 640 "$D/pc.conf"
# the daemon needs a descriptor per client: raise the soft limit toward the hard one
ulimit -n "$(ulimit -H -n)" 2>/dev/null
"$BIN" -f "$D/pc.conf" -D > "$D/pc.log" 2>&1 & P=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/pc.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/pc.log" || { echo "  daemon did not start:"; tail -3 "$D/pc.log"; echo "sweepwalktest: 0 passed, 1 failed"; exit 1; }

timeout 180 python3 - <<'PY' > "$D/py.out" 2>&1
import resource, socket, statistics, time
soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
N = min(20000, hard - 1000)
resource.setrlimit(resource.RLIMIT_NOFILE, (min(hard, N + 512), hard))
res = []
def ok(c, m): res.append(("ok" if c else "FAIL", m))
PING = b"*1\r\n$4\r\nPING\r\n"
def median_rtt(s, n=3000):
    for _ in range(200):                       # warm
        s.sendall(PING); s.recv(16)
    v = []
    for _ in range(n):
        t = time.perf_counter(); s.sendall(PING)
        if s.recv(16) != b"+PONG\r\n": return None
        v.append((time.perf_counter() - t) * 1e6)
    return statistics.median(v)
if N < 5000:
    print("  skip only %d descriptors available (hard limit %d) - nothing was tested" % (N, hard))
    print("PYDONE 0"); raise SystemExit
probe = socket.create_connection(("127.0.0.1", 18335), timeout=10)
probe.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
m0 = median_rtt(probe)
idle = [socket.create_connection(("127.0.0.1", 18335), timeout=10) for _ in range(N)]
time.sleep(1.0)
m1 = median_rtt(probe)
print("MEASURE idle %d median_rtt_us before %.1f with_idle %.1f" % (N, m0 or -1, m1 or -1))
ok(m0 is not None and m1 is not None, "the probe's %d PINGs all answered, before and with %d idle connections" % (3000, N))
if m0 and m1:
    ok(m1 <= m0 * 3 + 50, "with %d idle connections on its worker a PING takes %.0f us at the median, %.0f us with none (limit 3x + 50 us)" % (N, m1, m0))
for s in idle: s.close()
probe.close()
for k, m in res: print("  %-4s %s" % (k, m))
print("PYDONE %d" % sum(1 for k, _ in res if k == "FAIL"))
PY
grep -E "^  (ok|FAIL|skip) |^MEASURE" "$D/py.out"
pf=$(grep -oE "^PYDONE [0-9]+" "$D/py.out" | awk '{print $2}')
pass=$(grep -c "^  ok " "$D/py.out")
if [ -z "$pf" ]; then echo "  FAIL the python driver did not finish: $(tail -3 "$D/py.out")"; pf=1; fi
echo "sweepwalktest: $pass passed, $pf failed"
[ "$pf" -eq 0 ]
