#!/bin/sh
# shrinktest.sh - S150 step D: a table that sits below shrink_at_pct of
# its slot capacity past the cool-off is resized down - to the size
# midway between the two thresholds, never below one segment - and the
# old index goes back to the arena (B, C): retired, then punched out.
#
# Two collections: "0" at 2^18 with 100,000 keys (6% of its slots: it
# shrinks to 2^16, the size where 100,000 keys sit at 25%, between the
# 18% and 75% thresholds, so nothing fires again); "floor" at 2^14 with
# nothing in it (it shrinks to 2^12, one segment, and stops there).
# The cool-off (8 s) outlasts the fill, so the first decision sees the
# full table - shorter, and an empty configured table shrinks before its
# data arrives, which is the point of the 60 s default.
# Both old indexes retire and their slots are punched; a create then
# takes the punched slots back and the process RSS rises by what came
# back - the exact RSS isolation of the punch itself is regiontest's.
#
# Fail-first: the tree before D refuses the config (no such knobs), and
# with the knobs removed nothing ever shrinks.
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -x "$CLI" ] || { echo "shrinktest: $CLI not built - SKIPPED, and a skip is not a pass"; exit 0; }
D=$(mktemp -d /var/tmp/pcsh.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 "$PID" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
if ss -ltn 2>/dev/null | grep -qE ":1761[12][[:space:]]"; then
	echo "shrinktest: port 17611/17612 already bound" >&2; exit 1
fi
cat > "$D/n.conf" <<EOF
[daemon]
workers = 2
log_level = notice
allow_create = yes
shrink_cooloff_s = 8
[memory]
arena_mb = 64
reclaim_cooloff_s = 2
reclaim_quiet_s = 1
[secrets]
client = sh-client-secret
cluster = sh-cluster-secret
enable = sh-enable
[listen]
tcp = 127.0.0.1:17611
resp = 127.0.0.1:17612
plaintext = loopback
[collection 0]
buckets_log2 = 18
[collection floor]
buckets_log2 = 14
EOF
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
PID=$!
i=0
while [ $i -lt 80 ]; do
	grep -q "perfcached ready" "$D/n.log" && break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/n.log" || { bad "daemon did not start: $(tail -2 "$D/n.log" | tr '\n' ' ' | cut -c1-150)"; echo "shrinktest: $pass passed, $fail failed"; exit 1; }

python3 - "$CLI" "$PID" <<'EOF'
import json, socket, subprocess, sys, time

CLI, PID = sys.argv[1], sys.argv[2]
RESP, NATIVE = 17612, 17611
NKEYS = 100000
pass_n = fail_n = 0
def ok(m):
    global pass_n; pass_n += 1; print("  ok   " + m)
def bad(m):
    global fail_n; fail_n += 1; print("  FAIL " + m)

def conn():
    s = socket.create_connection(("127.0.0.1", RESP), 8)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(30)
    return s
def cmd(args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out
def native(method, **params):
    s = socket.create_connection(("127.0.0.1", NATIVE), timeout=10); f = s.makefile("rwb")
    f.write(json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode() + b"\n"); f.flush()
    r = json.loads(f.readline()); s.close()
    return r.get("result", r)
def stats():
    return native("stats")
def col(name):
    cols = native("collections")
    cols = cols.get("collections", cols) if isinstance(cols, dict) else cols
    return next((c for c in cols if str(c.get("name")) == name), None) if isinstance(cols, list) else None
def log2_of(c):
    if c is None: return None
    if "buckets_log2" in c: return c["buckets_log2"]
    b = c.get("buckets"); return b.bit_length() - 1 if b else None
def rss():
    with open("/proc/%s/statm" % PID) as f:
        return int(f.read().split()[1]) * 4096

# ---- fill "0" ----
s = conn(); parts = []; plen = 0; t_fill = time.time()
for i in range(NKEYS):
    c = cmd([b"SET", b"k:%09d" % i, b"v"]); parts.append(c); plen += len(c)
    if plen > 1 << 20:
        s.sendall(b"".join(parts)); parts = []; plen = 0
        s.setblocking(False)
        try:
            while s.recv(1 << 20): pass
        except BlockingIOError:
            pass
        s.setblocking(True); s.settimeout(30)
s.sendall(b"".join(parts) + cmd([b"DBSIZE"]))
buf = b""
while b":%d\r\n" % NKEYS not in buf:
    d = s.recv(1 << 20)
    if not d: sys.exit("fill connection died")
    buf = (buf + d)[-64:]
s.close()
t_fill = time.time() - t_fill
c0, cf = col("0"), col("floor")
if log2_of(c0) == 18 and log2_of(cf) == 14 and t_fill < 6:
    ok("filled %d keys in %.1fs, inside the cool-off: '0' is at 2^18 (6%% of its slots), 'floor' at 2^14 and empty" % (NKEYS, t_fill))
else:
    bad("start sizes: 0=%r floor=%r after a %.1fs fill (the cool-off is 8s)" % (log2_of(c0), log2_of(cf), t_fill))

# ---- the shrink: within the cool-off plus the copy ----
t0 = time.time(); l0 = lf = None; t_0 = t_f = None
while time.time() - t0 < 45:
    l0, lf = log2_of(col("0")), log2_of(col("floor"))
    if l0 == 16 and t_0 is None: t_0 = time.time() - t0
    if lf == 12 and t_f is None: t_f = time.time() - t0
    r = stats().get("retire") or {}
    if t_0 is not None and t_f is not None and r.get("cleared", 0) >= 2 and r.get("pending", 1) == 0:
        break
    time.sleep(0.5)
if t_0 is not None:
    ok("'0' shrank from 2^18 to 2^16 - midway between 18%% and 75%% for 100,000 keys - after %.1fs" % t_0)
else:
    bad("'0' did not shrink within 45s (at 2^%r) - the tree before D" % l0)
if t_f is not None:
    ok("'floor' shrank from 2^14 to 2^12, one segment, after %.1fs" % t_f)
else:
    bad("'floor' did not shrink within 45s (at 2^%r)" % lf)
r = stats().get("retire") or {}
if r.get("cleared") == 2 and r.get("pending") == 0 and r.get("returned_bytes", 0) >= 79 * 262144:
    ok("both old indexes retired once their readers parked: %d slots returned" % (r["returned_bytes"] // 262144))
else:
    bad("retire figures: %r" % (r,))
m = stats().get("memory") or {}
if m.get("arena_regions_retired", 0) >= 79:
    ok("the arena took %d slots back (2^18 = 71, 2^14 = 8)" % m["arena_regions_retired"])
else:
    bad("arena_regions_retired = %r" % m.get("arena_regions_retired"))

# ---- the floor and the band hold: nothing fires again ----
time.sleep(10)
l0, lf = log2_of(col("0")), log2_of(col("floor"))
if l0 == 16 and lf == 12:
    ok("10s on (past another cool-off): '0' still 2^16 (25% of its slots, inside the band), 'floor' still 2^12 (the floor)")
else:
    bad("sizes moved again: 0=2^%r floor=2^%r" % (l0, lf))

# ---- the punch, and the re-take ----
t1 = time.time(); cold = 0; stable = 0; last = -1
while time.time() - t1 < 30:
    m = stats().get("memory") or {}
    cold = m.get("arena_regions_free_cold", 0)
    if cold > 0 and cold == last:
        stable += 1
        if stable >= 3: break
    else:
        stable = 0
    last = cold
    time.sleep(0.5)
if cold >= 2 << 20:
    ok("retired slots punched out past the cool-off: %d MB cold, %d MB kept warm" % (cold >> 20, m.get("arena_regions_free_warm", 0) >> 20))
else:
    bad("nothing punched within 30s: free_cold=%r warm=%r" % (cold, m.get("arena_regions_free_warm")))
rss_s = rss()
out = subprocess.run([CLI, "-q", "-h", "127.0.0.1", "-p", str(NATIVE), "-E", "sh-enable",
                      "create", "again", "16"], capture_output=True, text=True).stdout
m2 = stats().get("memory") or {}
cold2 = m2.get("arena_regions_free_cold", 0)
rss_c = rss()
back = cold - cold2
if "OK" in out.upper() or "created" in out:
    ok("a create of 2^16 was accepted")
else:
    bad("create refused: %s" % out.strip()[:80])
if back >= 2 << 20 and m2.get("arena_region_reuse", 0) >= 20:
    ok("it took %d MB of punched slots back (reuse %d, cold %d -> %d MB)" % (back >> 20, m2["arena_region_reuse"], cold >> 20, cold2 >> 20))
else:
    bad("the create did not take punched slots: cold %r -> %r, reuse %r" % (cold, cold2, m2.get("arena_region_reuse")))
if rss_c - rss_s >= back * 3 // 4:
    ok("and the process RSS rose by what came back: %d -> %d KB (%d KB re-committed)" % (rss_s >> 10, rss_c >> 10, back >> 10))
else:
    bad("RSS did not rise with the re-take: %d -> %d KB for %d KB" % (rss_s >> 10, rss_c >> 10, back >> 10))

# ---- the data ----
s = conn(); s.sendall(cmd([b"DBSIZE"])); d = s.recv(64); s.close()
if d.strip() == b":%d" % NKEYS:
    ok("DBSIZE is still %d after the shrink" % NKEYS)
else:
    bad("DBSIZE after the shrink: %r" % d[:30])
print("shrinktest: %d passed, %d failed" % (pass_n, fail_n))
sys.exit(1 if fail_n else 0)
EOF
rc=$?
grep -h "shrinking to" "$D/n.log" | cut -c1-140 | head -3
kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; PID=
exit $rc
