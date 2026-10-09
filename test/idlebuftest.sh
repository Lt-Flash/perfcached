#!/bin/sh
# idlebuftest.sh - S366: an idle connection gives its buffers back, and a
# burst of freed connection memory goes back to the system.  10-09 on 245
# (0.5.6.4): a held RESP connection cost 12.0 KB of heap - pc_conn plus its
# in/out/wire buffers, kept for its life - and after 50,000 closed the
# daemon's RSS stayed at 606 MB (baseline 154 MB): glibc keeps a freed heap.
#
#   - N RESP connections, each answered once: RSS grows by ~12 KB each (the
#     positive control - the measurement sees buffers when they are there);
#   - left idle, every one releases its buffers (clients.buffers.held 0,
#     idle_released N) and, the burst over, malloc_trim runs: RSS growth is
#     under 3 KB a connection - pc_conn and its CLIENT LIST row;
#   - released connections still answer (the buffers grow back);
#   - a BUSY connection keeps its buffers (no allocator churn per request)
#     while the idle ones around it release theirs;
#   - closing N connections holding buffers trims again: RSS within 8 MB of
#     where it started.
# RSS assertions are skipped under a sanitizer (its allocator, its RSS);
# the counters and the behaviour are checked regardless.
# Fail-first: 0.5.6.4 releases nothing and has no clients.buffers.
# Usage: test/idlebuftest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcidlebuf.XXXXXX); P=
trap '[ -n "$P" ] && kill -9 "$P" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
mkdir -p "$D/state"
cat > "$D/pc.conf" <<CONF
[daemon]
workers = 2
state_dir = $D/state
[memory]
arena_mb = 32
reclaim_giveback = 0
[secrets]
client = idlebuf-client-secret
[listen]
tcp = 127.0.0.1:17397
resp = 127.0.0.1:17398
http = 127.0.0.1:17399
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 640 "$D/pc.conf"
# 6,000 clients need descriptors: a CI job's soft limit is 1024 - raise it to the hard one
ulimit -n "$(ulimit -H -n)" 2>/dev/null
if [ "$(ulimit -n)" != unlimited ] && [ "$(ulimit -n)" -lt 7000 ]; then
	echo "  skip descriptor limit $(ulimit -n) (hard $(ulimit -H -n)) < 7000 - nothing was tested"
	echo "idlebuftest: 0 passed, 0 failed (skipped)"; exit 0
fi
"$BIN" -f "$D/pc.conf" -D > "$D/pc.log" 2>&1 & P=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/pc.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/pc.log" || { echo "  daemon did not start:"; tail -3 "$D/pc.log"; echo "idlebuftest: 0 passed, 1 failed"; exit 1; }
SAN=0; [ -n "${ASAN_OPTIONS:-}${UBSAN_OPTIONS:-}" ] && SAN=1

timeout 150 python3 - "$P" "$SAN" <<'PY' > "$D/py.out" 2>&1
import json, resource, socket, sys, time, urllib.request
pid, san = int(sys.argv[1]), sys.argv[2] == "1"
N = 6000
res = []
def ok(c, m): res.append(("ok" if c else "FAIL", m))
def skip(m): res.append(("skip", m))
soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
resource.setrlimit(resource.RLIMIT_NOFILE, (min(hard, N + 512), hard))
def rss():
    for l in open("/proc/%d/status" % pid):
        if l.startswith("VmRSS:"):
            return int(l.split()[1]) * 1024
def st():   # clients, with buffers defaulted: 0.5.6.4 (no buffers) must reach the RSS checks
    c = json.load(urllib.request.urlopen("http://127.0.0.1:17399/stats", timeout=5))["clients"]
    c.setdefault("buffers", {"held": -1, "idle_released": 0, "trims": 0, "trim": False})
    return c
def until(pred, secs):
    end = time.time() + secs
    while time.time() < end:
        v = pred()
        if v: return v
        time.sleep(0.25)
    return pred()
PING = b"*1\r\n$4\r\nPING\r\n"
def ping_all(conns):
    for s in conns: s.sendall(PING)
    good = 0
    for s in conns:
        if s.recv(64) == b"+PONG\r\n": good += 1
    return good
time.sleep(1.0)
r0 = rss()
conns = [socket.create_connection(("127.0.0.1", 17398), timeout=10) for _ in range(N)]
ok(ping_all(conns) == N, "%d RESP connections each answer PING" % N)
r1 = rss()
grow1 = (r1 - r0) / N
if san:
    skip("RSS per connection with buffers (sanitizer build)")
else:
    ok(grow1 >= 8 * 1024, "positive control: answered once, each holds buffers - RSS +%.1f KB a connection (>= 8 KB)" % (grow1 / 1024))
c = until(lambda: (lambda c: c if c["buffers"]["held"] == 0 else None)(st()), 8)
c = c or st()
ok(c["buffers"]["held"] == 0 and c["buffers"]["idle_released"] >= N,
   "left idle, all release their buffers: held %d, idle_released %d" % (c["buffers"]["held"], c["buffers"]["idle_released"]))
if c["buffers"].get("trim"):
    t = until(lambda: st()["buffers"]["trims"] >= 1, 20)
    ok(t, "the burst over, the heap is trimmed (trims %d)" % st()["buffers"]["trims"])
else:
    skip("malloc_trim: not this allocator")
r2 = rss()
grow2 = (r2 - r0) / N
if san:
    skip("RSS per idle connection (sanitizer build)")
else:
    ok(grow2 < 3 * 1024, "an idle connection now costs %.2f KB of RSS (< 3 KB; it was %.1f KB with buffers)" % (grow2 / 1024, grow1 / 1024))
print("MEASURE rss0 %d rss_buffers %d rss_idle %d per_conn_buffers %.0f per_conn_idle %.0f" % (r0, r1, r2, grow1, grow2))
ok(ping_all(conns) == N, "every released connection still answers PING (its buffers grow back)")
# one busy, the rest idle: the busy one keeps its buffers
end = time.time() + 4.0
while time.time() < end:
    conns[0].sendall(PING); conns[0].recv(64); time.sleep(0.2)
c = st()
ok(c["buffers"]["held"] == 1, "after 4 s with one connection busy and %d idle, only the busy one holds buffers (held %d)" % (N - 1, c["buffers"]["held"]))
t0 = c["buffers"]["trims"]
ok(ping_all(conns) == N, "all answer once more")
for s in conns: s.close()
if c["buffers"].get("trim"):
    t = until(lambda: st()["buffers"]["trims"] > t0, 20)
    ok(t, "closing %d connections that held buffers trims the heap again (trims %d -> %d)" % (N, t0, st()["buffers"]["trims"]))
time.sleep(0.5)
r3 = rss()
print("MEASURE rss_closed %d" % r3)
if san:
    skip("RSS after the close (sanitizer build)")
else:
    ok(r3 - r0 < 8 << 20, "after the close RSS is back within 8 MB of the start (%+.1f MB)" % ((r3 - r0) / 1048576))
ok(st()["open"] == 0, "and no client is open")
for k, m in res: print("  %-4s %s" % (k, m))
print("PYDONE %d" % sum(1 for k, _ in res if k == "FAIL"))
PY
grep -E "^  (ok|FAIL|skip) |^MEASURE" "$D/py.out"
pf=$(grep -oE "^PYDONE [0-9]+" "$D/py.out" | awk '{print $2}')
pass=$(grep -c "^  ok " "$D/py.out")
if [ -z "$pf" ]; then echo "  FAIL the python driver did not finish: $(tail -3 "$D/py.out")"; pf=1; fi
grep -q "heap: .* malloc_trim gave back" "$D/pc.log" && echo "  (log) $(grep -o 'heap: .*' "$D/pc.log" | head -1)"
echo "idlebuftest: $pass passed, $pf failed"
[ "$pf" -eq 0 ]
