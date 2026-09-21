#!/bin/sh
# aheadtest.sh - S166: the arena commits 2 MB groups AHEAD of the chunk
# frontier, so a client's write never populates one.
#
# What it is about: a carve that crosses into an uncommitted group calls
# MADV_POPULATE_WRITE over the whole 2 MB, on the worker holding the
# client's write.  Measured 2026-09-20 on an idle 16-vCPU box: 1.0-3.2 ms
# on 4K pages, 2.2-4.0 ms on THP; the 4-vCPU fleet logged a 10.1 ms SET.
# The reclaim tick (1/s) now keeps PCACHE_AHEAD_GROUPS groups committed
# above the frontier, and the give-back leaves that window alone.
#
# The assertion is the PROPERTY, not a latency: `commits_inline` counts
# groups a carve had to commit itself.  A latency threshold would be a
# flake on a busy box; the counter is exact and says the same thing.
#
# The shape matters: arena_mb is the INITIAL commit and arena_cap_mb the
# reservation, so writing past arena_mb is what makes the frontier cross
# into groups nobody committed yet.  The writes go in steps with a tick
# between them - growth faster than 1/s outruns any window by design and
# is counted, not prevented.
# Usage: test/aheadtest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcahd.XXXXXX)
P1=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

cat > "$D/a.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 16
arena_cap_mb = 128
reclaim_cooloff_s = 1
reclaim_quiet_s = 1
[secrets]
client = ahead-client-secret
[listen]
resp = 127.0.0.1:17694
http = 127.0.0.1:17695
plaintext = loopback
[collection 0]
buckets_log2 = 12
EOF
chmod 600 "$D/a.conf"

"$BIN" -f "$D/a.conf" > "$D/a.log" 2>&1 &
P1=$!
i=0
while [ $i -lt 100 ]; do
	grep -q "perfcached ready" "$D/a.log" && break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/a.log" || { echo "daemon did not start"; cat "$D/a.log"; exit 1; }

python3 - "$D" <<'PYEOF'
import json, socket, sys, time, urllib.request

D = sys.argv[1]
pass_n = fail_n = 0
def ok(m):
    global pass_n; pass_n += 1; print("  ok   " + m)
def bad(m):
    global fail_n; fail_n += 1; print("  FAIL " + m)

def mem():
    s = json.loads(urllib.request.urlopen("http://127.0.0.1:17695/stats", timeout=10).read())
    return s["memory"]

m0 = mem()
TIER = m0.get("tier", "?")
# The four backing tiers commit differently, so the property differs:
#   MAP_HUGETLB  - the ahead window is SKIPPED on purpose (a finite pool
#                  shared with the host; group_secure owns those pages)
#   THP advise / THP collapse / plain 4K - all commit through
#                  pcache_mem_commit, so the window applies
#   no arena     - there is no reservation at all; nothing to commit
HUGETLB = "MAP_HUGETLB" in TIER
# a heap-backed daemon reports no tier at all ("unknown"/"none"): there is
# no reservation, so neither counter can move
NOARENA = ("no dedicated arena" in TIER) or TIER in ("unknown", "none")
print("  ..   tier: %s" % TIER)
if "commits_inline" not in m0 or "commits_ahead" not in m0 or "commits_index" not in m0:
    bad("/stats memory has no commits_ahead / commits_inline (S166 is not in this build)")
    print("aheadtest: %d passed, %d failed" % (pass_n, fail_n + 1))
    sys.exit(1)

s = socket.create_connection(("127.0.0.1", 17694), 8); s.settimeout(30)
f = s.makefile("rwb")

def write(n, base, val, pfx):
    out = b"".join(b"*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n" %
                   (len(k), k, len(val), val)
                   for k in (b"%s:%d" % (pfx, base + j) for j in range(n)))
    s.sendall(out)
    for _ in range(n):
        f.readline()

# Two growths, because the arena has two frontiers and they are committed
# by different threads.  RECORDS: 24 MB past a 16 MB initial commit, 2 MB
# at a time with a tick between, carved upward by the worker taking the
# write.  INDEX: 160,000 small keys so the 4,096-bucket table splits over
# and over, carved DOWNWARD from the top of the reservation by the
# maintenance thread's growth slice.  A step per tick either way: growth
# faster than the tick outruns any window by design.
for step in range(12):
    write(250, step * 250, b"v" * 8192, b"ahead")
    time.sleep(1.2)
for step in range(8):
    write(20000, step * 20000, b"v" * 48, b"idx")
    time.sleep(1.2)

m1 = mem()
if not NOARENA:
    grew = m1["arena_held"] > m0["arena_held"]
    (ok if grew else bad)("the frontier grew past the initial commit: held %d -> %d bytes"
                          % (m0["arena_held"], m1["arena_held"]))
ahead = m1["commits_ahead"] - m0["commits_ahead"]
inline = m1["commits_inline"] - m0["commits_inline"]
if HUGETLB:
    (ok if ahead == 0 else bad)("hugetlb: the pool is not drawn on speculatively (commits_ahead +%d)" % ahead)
    (ok if inline > 0 else bad)("hugetlb: a carve secures its own group, as group_secure owns the pool (+%d)" % inline)
elif NOARENA:
    (ok if ahead == 0 and inline == 0 else bad)("no arena: nothing is committed either way (+%d ahead, +%d inline)" % (ahead, inline))
else:
    (ok if ahead > 0 else bad)("the reclaim tick committed groups ahead of the frontier (%d)" % ahead)
    (ok if inline == 0 else bad)("no carve had to commit a group itself (commits_inline +%d)" % inline)
    # The index zone is NOT windowed (S168 withdrawn): a table split
    # commits its own groups, on the maintenance thread rather than on a
    # client's write.  The test pins that it is counted where it lands,
    # so the two costs can never be confused again.
    idx = m1["commits_index"] - m0["commits_index"]
    (ok if idx > 0 else bad)("the index zone's splits committed their own groups, counted apart (%d)" % idx)
(ok if m1["nomem"] == 0 else bad)("no write was refused for memory (nomem %d)" % m1["nomem"])

print("aheadtest: %d passed, %d failed" % (pass_n, fail_n))
sys.exit(1 if fail_n else 0)
PYEOF
rc=$?
[ $rc -eq 0 ] || fail=1
exit $rc
