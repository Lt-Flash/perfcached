#!/bin/sh
# vertest.sh — task A1: a record's version survives death.
#
# `ver` is the Lamport stamp that says which copy of a key is newer.  It
# is assigned on every local write and, until this task, lived only in
# RAM: recovery re-stored every record and the table stamped each one
# with a FRESH tick.  Recovery runs before the node joins the cluster
# (daemon.c: pc_recover well ahead of pc_cluster_init), so the clock was
# still at 0 and a rejoiner's whole dataset came back numbered 1..N while
# the fleet it was about to rejoin sat in the billions.  Every recovered
# record would lose the first comparison anyone made.
#
#  1. a write's version is visible, and successive writes increase it
#  2. THE POINT: after a restart every key's version is what it was
#  3. the clock resumes ABOVE the recovered data - a post-recovery write
#     outranks every record that came back
#  4. cross-version: a store written by a PRE-A1 binary still replays,
#     numbered 0, instead of being discarded (run only when that binary
#     is passed as $2)
# Usage: test/vertest.sh [./perfcached] [./perfcached.pre]
set -u

BIN=${1:-./perfcached}
PRE=${2:-}
D=$(mktemp -d /var/tmp/pcver.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

mkdir -p "$D/wal"
cat > "$D/c.conf" <<EOF
[daemon]
workers = 2
[memory]
arena_mb = 64
[secrets]
client = ver-client-secret
cluster = ver-cluster-secret
[listen]
tcp = 127.0.0.1:16491
plaintext = loopback
[wal]
dir = $D/wal
probe = no
fsync = always
segment_mb = 2
segments = 4
save = off
[collection th]
buckets_log2 = 10
EOF

start() {
	: > "$D/log"
	"${1:-$BIN}" -f "$D/c.conf" >> "$D/log" 2>&1 &
	PID=$!
	i=0
	while [ $i -lt 80 ]; do
		grep -q "perfcached ready" "$D/log" && return 0
		kill -0 $PID 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "daemon did not start"; tail -5 "$D/log"; exit 1
}
stopd() { kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null; PID=; }

# rpc <<'EOF' ... python body with call() bound ... EOF
rpc() {
	python3 - "$@" <<'PY'
import json, socket, sys
s = socket.create_connection(("127.0.0.1", 16491), timeout=5)
f = s.makefile("rwb"); rid=[0]
def call(m, **p):
    rid[0]+=1
    r={"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
def ver(k):
    r = call("exists", col="th", key=k).get("result", {})
    if "ver" not in r:
        sys.exit("no ver in exists response: %r" % (r,))
    return r["ver"]
exec(open(sys.argv[1]).read())
PY
}

# ---- 1. a version is visible and moves forward -------------------------
start
cat > "$D/p1.py" <<'PY'
for i in range(5):
    call("set", col="th", key="k%d" % i, value="v%d" % i)
vs = [ver("k%d" % i) for i in range(5)]
if any(v <= 0 for v in vs):
    sys.exit("a stored record has no version: %r" % vs)
if vs != sorted(vs) or len(set(vs)) != len(vs):
    sys.exit("versions did not increase across writes: %r" % vs)
open("VERS", "w").write(" ".join(str(v) for v in vs))
PY
(cd "$D" && rpc p1.py) && ok "a write stamps a version, and it advances" \
	|| bad "versions are not visible or do not advance"

# more keys, a snapshot, then a WAL tail on top of it - so recovery has
# to carry the version through BOTH paths, not just one
cat > "$D/p1b.py" <<'PY'
import time
call("save")
while call("stats")["result"]["rdb"]["saves"] < 1:
    time.sleep(0.2)
for i in range(5, 10):
    call("set", col="th", key="k%d" % i, value="v%d" % i)
time.sleep(0.5)
open("VERS", "w").write(" ".join(str(ver("k%d" % i)) for i in range(10)))
PY
(cd "$D" && rpc p1b.py) || bad "phase-1 snapshot + tail"
BEFORE=$(cat "$D/VERS" 2>/dev/null)
stopd

# ---- 2. the versions survive the restart ------------------------------
start
grep -q "recover: rdb loaded" "$D/log" && ok "rdb loaded" || bad "rdb not loaded"
cat > "$D/p2.py" <<'PY'
open("VERS", "w").write(" ".join(str(ver("k%d" % i)) for i in range(10)))
PY
(cd "$D" && rpc p2.py) || bad "phase-2 read-back"
AFTER=$(cat "$D/VERS" 2>/dev/null)
[ -n "$BEFORE" ] && [ "$BEFORE" = "$AFTER" ] \
	&& ok "every version survived the restart" \
	|| bad "versions were renumbered by recovery
         before: $BEFORE
          after: $AFTER"

# ---- 3. the clock resumes above the recovered data --------------------
cat > "$D/p3.py" <<'PY'
recovered = max(ver("k%d" % i) for i in range(10))
call("set", col="th", key="fresh", value="x")
v = ver("fresh")
if v <= recovered:
    sys.exit("a post-recovery write got ver %d, not above the recovered "
             "high-water mark %d" % (v, recovered))
PY
(cd "$D" && rpc p3.py) \
	&& ok "a post-recovery write outranks everything recovered" \
	|| bad "the clock did not resume above the recovered records"
grep -q "lamport" "$D/log" && ok "recovery reported the restored clock" \
	|| bad "recovery said nothing about the clock it restored"
stopd

# ---- 4. cross-version: a pre-A1 store still replays -------------------
if [ -n "$PRE" ] && [ -x "$PRE" ]; then
	rm -rf "$D/wal"; mkdir -p "$D/wal"
	start "$PRE"
	cat > "$D/p4.py" <<'PY'
import time
for i in range(5):
    call("set", col="th", key="old%d" % i, value="oldval%d" % i)
call("save")
while call("stats")["result"]["rdb"]["saves"] < 1:
    time.sleep(0.2)
for i in range(5, 10):
    call("set", col="th", key="old%d" % i, value="oldval%d" % i)
time.sleep(0.5)
PY
	# the OLD binary has no ver in exists, so drive it with plain sets
	(cd "$D" && python3 - p4.py <<'PY'
import json, socket, sys
s = socket.create_connection(("127.0.0.1", 16491), timeout=5)
f = s.makefile("rwb"); rid=[0]
def call(m, **p):
    rid[0]+=1
    r={"jsonrpc":"2.0","id":rid[0],"method":m}
    if p: r["params"]=p
    f.write(json.dumps(r).encode()+b"\n"); f.flush()
    return json.loads(f.readline())
exec(open(sys.argv[1]).read())
PY
	) || bad "phase-4 pre-A1 writes"
	stopd

	start
	cat > "$D/p4b.py" <<'PY'
missing = [i for i in range(10)
           if not call("exists", col="th", key="old%d" % i)
                      .get("result", {}).get("exists")]
if missing:
    sys.exit("a pre-A1 store was DISCARDED, not replayed: old%r" % missing)
vs = [ver("old%d" % i) for i in range(10)]
if any(v != 0 for v in vs):
    sys.exit("a pre-A1 record came back with a fabricated version: %r" % vs)
PY
	(cd "$D" && rpc p4b.py) \
		&& ok "a pre-A1 store replays, numbered 0, instead of being lost" \
		|| bad "pre-A1 data did not survive the format change"
	stopd
else
	echo "  skip pre-A1 cross-version arm (pass the old binary as \$2)"
fi

echo "vertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
