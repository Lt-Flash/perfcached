#!/bin/sh
# arenafreetest.sh - S445: a dropped table's index goes back where it can.
#
# Index regions are carved downward from the zone's frontier (rtop) and a
# retired one goes to the zone's own free set, for the next index; records
# are never carved there.  rtop only grew down, so a dropped index stayed
# out of the records' room until a restart, and arena_used kept counting
# it (245-247, 2026-10-23: thbench's 4,194,304 buckets, 286 MB).  One node:
#   1. create big at 2^18 (~71 slots), then small at 2^12 (one segment) -
#      small's index lands below big's;
#   2. drop big: its slots are the zone's - arena_free_tables holds them,
#      arena_used no longer counts them, and arena_free has not moved
#      (small's slot sits between them and the frontier);
#   3. drop small: the frontier rises over both - arena_free_tables back to
#      nothing, arena_free back where it started, arena_regions_raised
#      counting the slots.
# The reclaim tick is kept away (600 s): a zone punch in flight holds its
# slots in neither free map, and this test reads the maps, not the punch.
# FAIL-FIRST: the build before S445 has no arena_free_tables and keeps
# both indexes' room as used.
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcaf.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 "$PID" 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
if ss -ltn 2>/dev/null | grep -qE ":1763[12][[:space:]]"; then
	echo "arenafreetest: port 17631/17632 already bound" >&2; exit 1
fi
cat > "$D/n.conf" <<EOF
[daemon]
workers = 2
log_level = notice
allow_create = yes
[memory]
arena_mb = 64
reclaim_cooloff_s = 600
reclaim_quiet_s = 600
[secrets]
client = af-client-secret
cluster = af-cluster-secret
enable = af-enable
[listen]
tcp = 127.0.0.1:17631
resp = 127.0.0.1:17632
plaintext = loopback
[collection 0]
buckets_log2 = 12
EOF
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
PID=$!
i=0
while [ $i -lt 80 ]; do
	grep -q "perfcached ready" "$D/n.log" && break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/n.log" || { bad "daemon did not start: $(tail -2 "$D/n.log" | tr '\n' ' ' | cut -c1-150)"; echo "arenafreetest: $pass passed, $fail failed"; exit 1; }

python3 - <<'PY'
import pcnative, socket, sys, time

f = pcnative.wrap(socket.create_connection(("127.0.0.1", 17631), timeout=20))
passed = failed = 0
def ok(m):
    global passed; passed += 1; print("  ok   " + m)
def bad(m):
    global failed; failed += 1; print("  FAIL " + m)
def call(m, **p):
    return pcnative.call(f, m, **p)
def mem():
    return (call("stats").get("result") or {}).get("memory") or {}
def retire():
    return (call("stats").get("result") or {}).get("retire") or {}
def mb(x):
    return "%.1f MB" % ((x or 0) / 1048576.0)
SLOT = 262144

call("enable", secret="af-enable")
m0 = mem()
if not m0.get("arena_capacity_valid"):
    print("arenafreetest: the arena's capacity is not measured on this backing - SKIPPED, and a skip is not a pass")
    sys.exit(0)
free0 = m0["arena_free"]

r = call("create", col="big", buckets_log2=18)
r2 = call("create", col="small", buckets_log2=12)
m1 = mem()
held_big = free0 - m1["arena_free"]
(ok if "error" not in r and "error" not in r2 and held_big >= 60 * SLOT else bad)(
    "big at 2^18 and small at 2^12 created: %s of index carved (free %s -> %s)" % (mb(held_big), mb(free0), mb(m1["arena_free"])))

def dropped(col, cleared0):
    r = call("drop", col=col)
    t = time.time()
    while time.time() - t < 20:
        rt = retire()
        if rt.get("cleared", 0) > cleared0 and rt.get("pending", 1) == 0:
            return r, rt
        time.sleep(0.2)
    return r, retire()

c0 = retire().get("cleared", 0)
r, rt = dropped("big", c0)
m2 = mem()
ft, used1, used2 = m2.get("arena_free_tables"), m1.get("arena_used"), m2.get("arena_used")
(ok if "error" not in r and rt.get("cleared", 0) > c0 else bad)(
    "big dropped, its index retired (retire %r)" % ({k: rt.get(k) for k in ("cleared", "pending", "returned_bytes")},))
(ok if isinstance(ft, int) and ft >= 60 * SLOT else bad)(
    "big's index is the zone's, said apart: arena_free_tables %s" % (mb(ft) if isinstance(ft, int) else ft))
(ok if isinstance(ft, int) and used2 is not None and used1 - used2 >= 60 * SLOT and
     m2["arena_total"] == used2 + m2["arena_free"] + ft else bad)(
    "and no longer counted as used: arena_used %s -> %s; used + free + free_tables = total" % (mb(used1), mb(used2)))
(ok if abs(m2["arena_free"] - m1["arena_free"]) <= 4 * SLOT else bad)(
    "arena_free has not moved (%s -> %s): small's slot lies between big's and the frontier" % (mb(m1["arena_free"]), mb(m2["arena_free"])))

c1 = rt.get("cleared", 0)
r, rt = dropped("small", c1)
m3 = mem()
(ok if "error" not in r and rt.get("cleared", 0) > c1 else bad)("small dropped, its index retired")
ft3 = m3.get("arena_free_tables")
(ok if isinstance(ft3, int) and ft3 <= 4 * SLOT else bad)(
    "the frontier rose over both: arena_free_tables %s" % (mb(ft3) if isinstance(ft3, int) else ft3))
(ok if abs(m3["arena_free"] - free0) <= 4 * SLOT else bad)(
    "arena_free back where it started: %s (was %s)" % (mb(m3["arena_free"]), mb(free0)))
(ok if m3.get("arena_regions_raised", 0) >= 60 else bad)(
    "arena_regions_raised counts the slots: %r" % m3.get("arena_regions_raised"))
r = call("create", col="again", buckets_log2=18)
m4 = mem()
(ok if "error" not in r and free0 - m4["arena_free"] >= 60 * SLOT else bad)(
    "a create of 2^18 carves from the frontier again: free %s -> %s" % (mb(m3["arena_free"]), mb(m4["arena_free"])))
print("arenafreetest: %d passed, %d failed" % (passed, failed))
sys.exit(1 if failed else 0)
PY
rc=$?
if grep -E "ERROR|CRIT" "$D/n.log" | grep -v "tls_verify" >/dev/null; then
	echo "  FAIL ERROR/CRIT in the log:"; grep -E "ERROR|CRIT" "$D/n.log" | head -3 | sed 's/^/        /'
	rc=1
fi
exit $rc
