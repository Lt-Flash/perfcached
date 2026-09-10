#!/bin/sh
# tailtest.sh - S119: the never-carved tail of the reservation is given
# back.  The daemon pins and populates its whole reservation at start, and
# the give-back used to walk only the groups below the chunk frontier, so on
# a node that never needed its whole reservation RSS = held + the untouched
# tail for the life of the process (245: 70 MB beside 102 MB held).
#
# The daemon runs UNPINNED here - CAP_IPC_LOCK dropped from the bounding
# set and an 8 MB memlock limit, the fleet's own shape - because a pinned
# arena refuses the punch by design (that refusal latches give-back off).
# The kernel's figure is asserted, not a counter: the arena mapping's Rss
# from the daemon's own /proc/<pid>/smaps.
# Fail-first: a daemon without the tail punch keeps the whole 32 MB resident
# beside ~1.5 MB held; a host that cannot drop the capability SKIPs loudly.
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pctl.XXXXXX); SEC=tail-client-secret
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
P=""
trap '[ -n "$P" ] && grep -qa -- "$D" /proc/$P/cmdline 2>/dev/null && kill -9 "$P"; rm -rf "$D"' EXIT TERM INT
# root holds CAP_IPC_LOCK and would pin the arena: drop it from the bounding
# set.  An unprivileged runner (GitHub's, a developer's shell) cannot pin
# past RLIMIT_MEMLOCK in the first place, and cannot apply a bounding set
# either - it runs the daemon plainly.
ROOT=0; [ "$(id -u)" = 0 ] && ROOT=1
if [ $ROOT = 1 ] && ! command -v setpriv >/dev/null 2>&1; then
	echo "SKIP: setpriv not found - cannot drop CAP_IPC_LOCK, the arena would pin and the punch be refused"
	echo "tailtest: 0 passed, 0 failed (SKIPPED)"; exit 0
fi
cat > "$D/n1.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 32
reclaim_keep = 1
reclaim_quiet_s = 1
reclaim_cooloff_s = 1
shrink_step_mb = 16
[secrets]
client = $SEC
cluster = tail-cluster-secret
[listen]
plaintext = loopback
tcp = 127.0.63.1:17981
[collection c]
buckets_log2 = 12
CONF
ulimit -l 8192 2>/dev/null
if [ $ROOT = 1 ]; then
	setpriv --bounding-set=-ipc_lock --inh-caps=-ipc_lock "$BIN" -f "$D/n1.conf" >> "$D/n1.log" 2>&1 &
else
	"$BIN" -f "$D/n1.conf" >> "$D/n1.log" 2>&1 &
fi
P=$!
i=0
while [ $i -lt 200 ]; do
	grep -q "perfcached ready" "$D/n1.log" 2>/dev/null && break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/n1.log" || { echo "node did not start"; cat "$D/n1.log"; exit 1; }
grep -q "continuing unpinned" "$D/n1.log" && echo "  ..   mlock refused under the 8 MB limit without CAP_IPC_LOCK: the arena runs unpinned" \
	|| echo "  ..   the log says pinned - under the sanitizer mlock is intercepted and reports success without locking; the latch below is the truth"

# drive <op> [args] - fill <n> <ttl> (60 KB values; prints stored/full),
# mem <field> (stats.memory.<field>, dotted path; MISSING when absent),
# entries (collection c)
drive() {
	python3 - "$@" <<'PYEOF'
import json, socket, sys
s = socket.create_connection(("127.0.63.1", 17981), timeout=10)
f = s.makefile("rwb"); rid = [0]
def call(m, **p):
    rid[0] += 1
    r = {"jsonrpc": "2.0", "id": rid[0], "method": m}
    if p: r["params"] = p
    f.write((json.dumps(r) + "\n").encode()); f.flush()
    return json.loads(f.readline())
op = sys.argv[1]
if op == "fill":
    n, ttl = int(sys.argv[2]), int(sys.argv[3]); stored = full = 0
    v = "x" * 60000
    for i in range(n):
        r = call("set", col="c", key="tk%04d" % i, value=v, ttl=ttl)
        if "error" in r:
            if "full" in json.dumps(r["error"]): full += 1
        else: stored += 1
    print("stored=%d full=%d" % (stored, full))
elif op == "entries":
    r = call("stats"); r = r.get("result", r)
    print(sum(c.get("entries", 0) for c in r.get("collections", []) if c.get("name") == "c"))
else:
    r = call("stats"); r = r.get("result", r).get("memory", {})
    for k in sys.argv[2].split("."):
        r = r.get(k) if isinstance(r, dict) else None
    print("MISSING" if r is None else r)
PYEOF
}
# The arena mapping's resident bytes, the kernel's figure from the daemon's
# own smaps - and the mapping is identified by its SIZE, which the daemon
# publishes exactly as arena_reserved, not by "the biggest resident thing
# in roughly the right size band".  A sanitizer build carries other
# read-write anonymous mappings within a few hundred kB of the arena's
# size, and picking the most resident of them read the WRONG mapping
# whenever one of those was busy: the rc27 and rc28 tag runs on the public
# runner failed and passed alternately, on identical commits, for that
# reason alone.  mlock can split the reservation into several adjacent
# mappings, so an adjacent RUN whose sizes sum to the reservation counts
# as the arena; anything ambiguous says so instead of guessing.
arena_rss() {
	python3 - "$P" "$(drive mem arena_reserved)" <<'PYEOF'
import sys
pid, want = sys.argv[1], sys.argv[2]
if want in ("MISSING", "None", ""):
    print("MISSING"); raise SystemExit
want = int(want)
vmas = []
for line in open("/proc/%s/smaps" % pid):
    if line[0] in "0123456789abcdef" and "-" in line.split()[0]:
        p = line.split(); lo, hi = [int(x, 16) for x in p[0].split("-")]
        anon = p[1].startswith("rw") and (len(p) < 6 or p[5] in ("", "[anon]"))
        vmas.append([lo, hi, anon, 0])
    elif line.startswith("Rss:") and vmas:
        vmas[-1][3] = int(line.split()[1]) * 1024
hits, i = [], 0
while i < len(vmas):
    if not vmas[i][2]:
        i += 1; continue
    j, size, rss = i, 0, 0
    while j < len(vmas) and vmas[j][2] and (j == i or vmas[j][0] == vmas[j - 1][1]):
        size += vmas[j][1] - vmas[j][0]; rss += vmas[j][3]; j += 1
        if size == want:
            hits.append(rss)
    i = j
print(hits[0] if len(hits) == 1 else ("AMBIGUOUS" if hits else "MISSING"))
PYEOF
}
# the give-back latch is the truth about pinning: a pinned arena refuses
# the first punch and latches give-back off - then this suite cannot run
# and says so.  A daemon without the tail punch never punches at start, so
# neither figure moves and the assertions below fail on their own terms.
i=0
while [ $i -lt 30 ]; do
	T=$(drive mem reclaim.tail_released); G=$(drive mem reclaim.giveback_off)
	[ "$G" = True ] && { echo "SKIP: the arena is pinned - the punch was refused and give-back latched off"; echo "tailtest: 0 passed, 0 failed (SKIPPED)"; exit 0; }
	[ "$T" != MISSING ] && [ "$T" -gt 0 ] && break
	sleep 0.2; i=$((i+1))
done
# The windows below are generous on purpose.  The give-back is a 1 Hz
# maintenance duty sharing its thread with the expiry sweep, the held walk
# and (since S69) a resize tick, and under a sanitizer on a two-processor
# runner those take long enough that a punch gets a fraction of the ticks
# it gets here.  A bounded wait that is too short turns "slower than this
# host" into "the tail was not given back" - which is what happened to the
# rc26 tag run on the public runner while the same snapshot passed on
# master.  The ASSERTION is unchanged: the mapping must still come within
# 8 MB of held, and the failure says how much was released so a real
# regression still reads as one.
await_le() { # await_le <secs>: rss - held <= 8 MB within <secs>; sets R and H
	i=0
	while [ $i -lt $(( $1 * 5 )) ]; do
		H=$(drive mem arena_held); R=$(arena_rss)
		[ "$R" != MISSING ] && [ "$H" != MISSING ] && [ $(( R - H )) -le $(( 8 << 20 )) ] && return 0
		sleep 0.2; i=$((i+1))
	done
	return 1
}

# 1. at start: the reservation is populated, ~1.5 MB is held; the tail
#    must go within the cooloff (1 s) and a few 16 MB ticks
await_le 45 && ok "at start the arena mapping is resident within 8 MB of held: rss $R, held $H - the never-carved tail was given back" \
	|| bad "the never-carved tail stays resident: rss $R, held $H, tail_released $(drive mem reclaim.tail_released), giveback_off $(drive mem reclaim.giveback_off) (S119)"
T=$(drive mem reclaim.tail_released)
[ "$T" != MISSING ] && [ "$T" -ge $(( 16 << 20 )) ] && ok "tail_released counts it: $T bytes" \
	|| bad "tail_released $T (S119)"

# 2. the frontier case: a burst carves into the punched tail (re-faults),
#    expires, and the give-back returns the drained groups; the bound holds
B=$(drive fill 200 2); echo "  ..   burst: $B"
# positive control: the instrument must see the burst resident before it is
# trusted to see the tail gone
R1=$(arena_rss)
[ "$R1" != MISSING ] && [ "$R1" -ge $(( 12 << 20 )) ] && ok "the mapping reader sees the burst resident: rss $R1" \
	|| bad "the mapping reader saw $R1 with 12 MB just written - the instrument cannot fail"
i=0; while [ $i -lt 100 ]; do [ "$(drive entries)" = 0 ] && break; sleep 0.2; i=$((i+1)); done
[ "$(drive entries)" = 0 ] && ok "the burst expired" || bad "records did not expire: $(drive entries) left"
await_le 60 && ok "after the burst and its give-back the mapping is again within 8 MB of held: rss $R, held $H" \
	|| bad "after the burst the mapping stays resident: rss $R, held $H, tail_released $(drive mem reclaim.tail_released), giveback_off $(drive mem reclaim.giveback_off) (S119: the re-faulted groups did not go back)"
echo "tailtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
