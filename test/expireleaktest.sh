#!/bin/sh
# expireleaktest.sh - EXPIRE does not leak the value it re-reads.
#
# S213 made a TTL re-arm travel as a write: after the touch, the record is
# read back whole (pcache_ht_fetch_full - value, expiry and version out of
# one read) so the WAL and the eager push carry the right bytes.  That read
# hands back a malloc'd copy the caller must free, and op_expire_local never
# did: every EXPIRE that hit leaked the value.  Found by S254 as a scaling
# fault - EXPIRE fell from 1.42M/s at 4 workers to 1.21M/s at 12, the
# workers queued on the mm lock in mprotect (glibc growing the per-thread
# heaps to hold the leak, ~300 MB/s at 256 B values).
#   - 200,000 EXPIREs on 1,000 keys of 1 KB: the daemon's RSS grows by less
#     than 32 MB (the leak is ~200 MB); the arena is pinned and prefaulted,
#     so the store itself does not move RSS
#   - every EXPIRE answered 1, and the keys are still there with a TTL
# Fail-first: the unfixed build grows by ~220 MB.
# Usage: test/expireleaktest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcexpleak.XXXXXX); P1=
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

cat > "$D/e.conf" <<CONF
[daemon]
workers = 2
[memory]
arena_mb = 64
[secrets]
client = expleak-client-secret
[listen]
tcp = 127.0.0.1:17584
resp = 127.0.0.1:17585
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 640 "$D/e.conf"
"$BIN" -f "$D/e.conf" -D > "$D/e.log" 2>&1 & P1=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/e.log" && break; sleep 0.1; i=$((i+1)); done
if ! grep -q "perfcached ready" "$D/e.log"; then
	bad "the daemon did not start: $(tail -1 "$D/e.log")"
	echo "expireleaktest: $pass passed, $fail failed"; exit 1
fi

PID=$P1 timeout 120 python3 - > "$D/e.res" 2> "$D/e.py.err" <<'PY'
import os, socket
pid = os.environ["PID"]
def rss():
    for l in open("/proc/%s/status" % pid):
        if l.startswith("VmRSS:"):
            return int(l.split()[1]) * 1024
s = socket.create_connection(("127.0.0.1", 17585), timeout=30)
f = s.makefile("rb")
def cmd(*a):
    return b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a)
def replies(n):
    out = []
    for _ in range(n):
        l = f.readline()
        if l[:1] == b"$":
            n2 = int(l[1:])
            out.append(f.read(n2 + 2)[:-2] if n2 >= 0 else None)
        else:
            out.append(l.strip())
    return out
K, R, V = 1000, 200, b"v" * 1024
s.sendall(b"".join(cmd(b"SET", b"k%04d" % i, V) for i in range(K)))
set_ok = replies(K).count(b"+OK")
# settle what the fill itself cost (per-thread scratch, first touches)
s.sendall(b"".join(cmd(b"EXPIRE", b"k%04d" % i, b"3600") for i in range(K)))
replies(K)
r0 = rss()
ones = 0
for r in range(R):
    s.sendall(b"".join(cmd(b"EXPIRE", b"k%04d" % i, b"3600") for i in range(K)))
    ones += replies(K).count(b":1")
r1 = rss()
s.sendall(cmd(b"TTL", b"k0000") + cmd(b"GET", b"k0999"))
ttl, val = replies(2)
print("P" if set_ok == K else "F", "the fill stored %d of %d 1 KB values" % (set_ok, K))
print("P" if ones == K * R else "F", "every EXPIRE answered 1 (%d of %d)" % (ones, K * R))
print("P" if (r1 - r0) < (32 << 20) else "F",
      "%d EXPIREs on 1 KB values grew RSS by %d KB (under 32 MB; the leak was ~%d MB)"
      % (K * R, (r1 - r0) >> 10, (K * R * len(V)) >> 20))
print("P" if ttl.startswith(b":") and 0 < int(ttl[1:]) <= 3600 and val == V else "F",
      "the keys are still there with their TTL (TTL %s, value intact %s)" % (ttl.decode(), val == V))
PY
while IFS= read -r l; do case "$l" in P\ *) ok "${l#P }" ;; F\ *) bad "${l#F }" ;; esac; done < "$D/e.res"
[ -s "$D/e.res" ] || bad "the python driver produced nothing"
[ -s "$D/e.py.err" ] && { bad "the python driver raised:"; tail -3 "$D/e.py.err"; }
kill "$P1" 2>/dev/null; wait "$P1" 2>/dev/null; P1=
echo "expireleaktest: $pass passed, $fail failed"
[ $fail -eq 0 ]
