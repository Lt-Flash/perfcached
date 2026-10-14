#!/bin/sh
# hashbench.sh - S318: HGET and HSET ops/s on a hash - one daemon, one
# RESP client on plaintext loopback.  The number the hash path's lock and
# copy work is judged by (DESIGN 12ig).
#
# Two hashes: 100 fields of 495 bytes (~50 KB - a 65,536-byte cell) and 10
# fields of 41 bytes (~500 B).  Four cases, one line each:
#   hashbench: HGET 50KB N ops/s (server U us cpu/op, pipe P, ...)
#   hashbench: HSET 50KB ...   hashbench: HGET 500B ...   hashbench: HSET 500B ...
# HGET reads the middle field (the lookup is linear, so half the record).
# HSET writes the middle field with a value of the SAME length whose bytes
# differ every op: the record's size does not change between ops, and a
# byte-identical HSET would take the store's versionless TTL bump and skip
# the very copy this measures.  Every reply is checked against what the
# command must answer, and the hash's shape is checked after each HSET run.
#
# One connection, PIPE commands in flight (default 64): one at a time
# measures the loopback round trip (~30 us), not the hash path.  The driver
# is Python, so on the small hash the ops/s can be the CLIENT's ceiling; the
# daemon's CPU per op (the sum of its threads' on-CPU time from /proc, in
# ns) is what the server paid regardless, and is the figure to compare
# between builds.  No WAL unless WAL=1 (fsync = everysec): 50 KB through
# the WAL per HSET measures the pump, not the hash path.
#
# Usage: test/hashbench.sh [./perfcached]
#   env: PIPE=64 OPS=50000 WORKERS=1 WAL=0 KEEP=<dir to keep the run's files>
set -u
BIN=${1:-./perfcached}
PIPE=${PIPE:-64}; OPS=${OPS:-50000}; WORKERS=${WORKERS:-1}; WAL=${WAL:-0}
TCP=18771; RESP=18772; HTTP=18773
for p in $TCP $RESP $HTTP; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "hashbench: port $p busy" >&2; exit 1; }
done
[ -x "$BIN" ] || { echo "hashbench: $BIN is not executable" >&2; exit 1; }
D=$(mktemp -d /var/tmp/pchbench.XXXXXX)
trap '[ -f "$D/n.pid" ] && kill -9 "$(cat "$D/n.pid")" 2>/dev/null; [ -n "${KEEP:-}" ] && cp -r "$D" "$KEEP"; rm -rf "$D"' EXIT INT TERM
mkdir -p "$D/s/wal"
WALSEC=
[ "$WAL" = 1 ] && WALSEC="[wal]
dir = $D/s/wal
probe = no
fsync = everysec
segment_mb = 64
segments = 8"
cat > "$D/n.conf" <<C
[daemon]
workers = $WORKERS
log_level = notice
state_dir = $D/s
[memory]
arena_mb = 64
[secrets]
client = hb-client-secret
cluster = hb-cluster-secret
[listen]
tcp = 127.0.0.1:$TCP
resp = 127.0.0.1:$RESP
http = 127.0.0.1:$HTTP
plaintext = loopback
[collection 0]
buckets_log2 = 12
$WALSEC
C
chmod 600 "$D/n.conf"
: > "$D/n.log"
"$BIN" -f "$D/n.conf" >> "$D/n.log" 2>&1 &
echo $! > "$D/n.pid"
i=0; while [ $i -lt 300 ]; do
	grep -q "perfcached ready" "$D/n.log" && break
	kill -0 "$(cat "$D/n.pid")" 2>/dev/null || break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/n.log" || { echo "hashbench: the daemon did not start: $(tail -2 "$D/n.log" | tr '\n' ' ')" >&2; exit 1; }
echo "hashbench: $BIN pid $(cat "$D/n.pid"), workers $WORKERS, wal $WAL, pipe $PIPE, $OPS ops per case"

python3 - "$RESP" "$PIPE" "$OPS" "$(cat "$D/n.pid")" <<'PY'
import glob, os, socket, sys, time

port, pipe, ops, pid = (int(x) for x in sys.argv[1:5])

def fail(m):
    print("hashbench: FAIL " + m, flush=True)
    sys.exit(1)

def cpu():
    """the daemon's on-CPU seconds, every thread: schedstat is ns, the
    stat fallback whole clock ticks (10 ms - coarse for a short case)"""
    tot = 0
    try:
        for p in glob.glob("/proc/%d/task/*/schedstat" % pid):
            with open(p) as f:
                tot += int(f.read().split()[0])
        if tot:
            return tot / 1e9
    except (OSError, ValueError, IndexError):
        pass
    with open("/proc/%d/stat" % pid) as f:
        st = f.read()
    fld = st[st.rindex(")") + 2:].split()
    return (int(fld[11]) + int(fld[12])) / os.sysconf("SC_CLK_TCK")

def enc(*args):
    out = [b"*%d\r\n" % len(args)]
    for a in args:
        if isinstance(a, str):
            a = a.encode()
        out.append(b"$%d\r\n%s\r\n" % (len(a), a))
    return b"".join(out)

s = socket.create_connection(("127.0.0.1", port), timeout=60)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
rf = s.makefile("rb")

def rd():
    l = rf.readline()
    if not l:
        fail("the connection closed")
    t, v = l[:1], l[1:-2]
    if t == b"$":
        n = int(v)
        return None if n < 0 else rf.read(n + 2)[:-2]
    if t == b"*":
        return [rd() for _ in range(int(v))]
    if t == b"-":
        fail("error reply: " + v.decode())
    return v                      # + and : answers, as bytes

def cmd(*a):
    s.sendall(enc(*a))
    return rd()

def run(gen, want, count):
    """count commands from gen(i), pipe in flight; every reply must be
    want.  Returns (seconds, daemon cpu seconds)."""
    t0, c0 = time.perf_counter(), cpu()
    done = 0
    while done < count:
        batch = min(pipe, count - done)
        s.sendall(b"".join(gen(done + i) for i in range(batch)))
        for i in range(batch):
            r = rd()
            if r != want:
                fail("reply %r, want %r" % (r, want))
        done += batch
    return time.perf_counter() - t0, cpu() - c0

def bench(label, gen, want):
    run(gen, want, pipe)                       # warm-up, unmeasured
    dt, dc = run(gen, want, ops)
    print("hashbench: %s %d ops/s (server %.2f us cpu/op, pipe %d, %d ops, %.2f s)"
          % (label, ops / dt, dc / ops * 1e6, pipe, ops, dt), flush=True)

# 100 x 495 B: 8 + 100 x (6 + 4 + 495) = 50,508 bytes;
# 10 x 41 B: 8 + 10 x (6 + 4 + 41) = 518 bytes
cases = [("50KB", "hb:big", 100, 495), ("500B", "hb:small", 10, 41)]
for label, key, nf, vl in cases:
    cmd("DEL", key)
    args = ["HSET", key]
    for i in range(nf):
        args += ["f%03d" % i, ("v%d." % i).ljust(vl, "x")]
    if cmd(*args) != b"%d" % nf:
        fail("building %s" % key)
    if cmd("HLEN", key) != b"%d" % nf:
        fail("%s has the wrong field count" % key)

for label, key, nf, vl in cases:
    mid = "f%03d" % (nf // 2)
    midv = ("v%d." % (nf // 2)).ljust(vl, "x").encode()
    bench("HGET " + label, lambda i, key=key, mid=mid: enc("HGET", key, mid),
          midv)
    # a value of the same length, different bytes every op (the op number)
    bench("HSET " + label,
          lambda i, key=key, mid=mid, vl=vl: enc("HSET", key, mid,
                                                 ("w%d." % i).ljust(vl, "x")),
          b"0")
    if cmd("HLEN", key) != b"%d" % nf or cmd("HSTRLEN", key, mid) != b"%d" % vl:
        fail("%s changed shape under HSET" % key)
    got = cmd("HGET", key, mid)
    if got is None or len(got) != vl or not got.startswith(b"w"):
        fail("%s: the last HSET did not land" % key)
print("hashbench: done", flush=True)
PY
rc=$?
p=$(cat "$D/n.pid"); kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; rm -f "$D/n.pid"
e=$(grep -cE ' (ERROR|CRIT)' "$D/n.log")
[ "$e" = 0 ] || { echo "hashbench: $e ERROR/CRIT in the daemon log: $(grep -hE ' (ERROR|CRIT)' "$D/n.log" | head -2)" >&2; rc=1; }
exit $rc
