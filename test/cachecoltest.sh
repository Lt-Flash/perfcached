#!/bin/sh
# cachecoltest.sh - `durable = no` keeps a collection off the WAL and out of the snapshot, so it
# starts empty after any restart, and a reload cannot change it.  Usage: test/cachecoltest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pccache.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=16591
ss -ltn 2>/dev/null | grep -q ":$PORT[[:space:]]" && { echo "cachecoltest: port $PORT busy" >&2; exit 1; }

mkconf() { # mkconf <tmp's durable: yes|no>
	mkdir -p "$D/wal" "$D/state"
	cat > "$D/c.conf" <<EOF
[daemon]
workers = 2
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = cc-client-secret
[listen]
tcp = 127.0.0.1:$PORT
plaintext = loopback
[wal]
dir = $D/wal
probe = no
fsync = everysec
segment_mb = 2
segments = 4
save = off
[collection keep]
buckets_log2 = 10
[collection tmp]
buckets_log2 = 10
durable = $1
EOF
	chmod 600 "$D/c.conf"
}
start() {
	"$BIN" -f "$D/c.conf" >> "$D/log" 2>&1 &
	PID=$!
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/log" && return 0
		kill -0 $PID 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "daemon did not start"; tail -5 "$D/log"; exit 1
}
stop() { kill -TERM $PID 2>/dev/null; wait $PID 2>/dev/null; PID=; }
hardkill() { kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null; PID=; }
py() { # py <python body using call()> - prints what the body prints
	python3 - "$PORT" <<EOF 2>&1
import json, pcnative, socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10)
f = pcnative.wrap(s); rid = [0]
def call(m, **p):
    rid[0] += 1
    r = {"jsonrpc": "2.0", "id": rid[0], "method": m}
    if p: r["params"] = p
    f.write(json.dumps(r).encode() + b"\n"); f.flush()
    return json.loads(f.readline())
def count(col):
    cur, n = 0, 0
    while True:
        r = call("scan", col=col, cursor=cur, count=1000)["result"]
        n += len(r.get("items", []))
        cur = r.get("cursor", 0)
        if not cur: return n
$1
EOF
}
WRITE='
for c in ("keep", "tmp"):
    for i in range(100):
        call("set", col=c, key="k%03d" % i, value="v%03d" % i)
    call("del", col=c, key="k000")
    call("expire", col=c, key="k001", ttl=3600)
assert call("save")["result"]["started"] is True
while call("stats")["result"]["rdb"]["saves"] < 1:
    time.sleep(0.2)
for c in ("keep", "tmp"):
    for i in range(100, 120):
        call("set", col=c, key="k%03d" % i, value="v%03d" % i)
time.sleep(1.5)                          # fsync = everysec
print(count("keep"), count("tmp"))
'
COUNTS='print(count("keep"), count("tmp"), (call("get", col="keep", key="k119")["result"] or {}).get("value"))'

# ---- A ---------------------------------------------------------------
mkconf no
"$BIN" -f "$D/c.conf" -E > "$D/dump" 2>&1
awk '/^\[collection tmp\]/{f=1} f&&/^durable = no/{print; exit}' "$D/dump" | grep -q "durable = no" &&
	! awk '/^\[collection keep\]/{f=1;next} /^\[/{f=0} f' "$D/dump" | grep -q "^durable" &&
	ok "A. -E: [collection tmp] durable = no, keep says nothing (durable)" ||
	bad "A. -E: $(grep -n 'durable\|collection' "$D/dump" | head -6 | tr '\n' '|')"
start
R=$(py 'print(" ".join("%s=%s" % (c["name"], c.get("durable")) for c in call("stats")["result"]["collections"]))')
echo "$R" | grep -q "keep=True" && echo "$R" | grep -q "tmp=False" &&
	ok "A. /stats: keep durable, tmp not ($R)" || bad "A. /stats: $R"
R=$(py 'print(" ".join("%s=%s/%s" % (c["name"], c.get("keeps"), c.get("copies")) for c in call("stats")["result"]["collections"]))')
echo "$R" | grep -q "keep=recoverable/1" && echo "$R" | grep -q "tmp=ephemeral/1" &&
	grep -q "collection 'keep' keeps: recoverable - a restart replays its WAL (fsync everysec); copies: one" "$D/log" &&
	grep -q "collection 'tmp' keeps: ephemeral (durable = no) - a restart starts it empty here; copies: one" "$D/log" &&
	ok "A. what each keeps (OM2-4): $R, and the start says so" ||
	bad "A. what each keeps (OM2-4): $R; the start: $(grep 'keeps:' "$D/log" | tr '\n' '|')"

# ---- B ---------------------------------------------------------------
R=$(py "$WRITE")
[ "$R" = "119 119" ] && ok "B. 119 keys in each (100 written, one deleted, 20 after the snapshot)" ||
	bad "B. counts after the writes: $R"
stop
"$BIN" -W "$D/wal" > "$D/wd" 2>&1
NK=$(grep -c " keep/" "$D/wd"); NT=$(grep -c " tmp/" "$D/wd")
[ "$NK" -ge 120 ] && [ "$NT" = 0 ] &&
	ok "B. the WAL: $NK records of keep, none of tmp" ||
	bad "B. the WAL: $NK records of keep, $NT of tmp ($(tail -1 "$D/wd"))"

# ---- C ---------------------------------------------------------------
: > "$D/log"
start
R=$(py "$COUNTS")
[ "$R" = "119 0 v119" ] && ok "C. a clean stop and a start: keep back whole (119, k119 = v119), tmp empty" ||
	bad "C. after the restart: $R"
grep -q "of cache collections (durable = no) not loaded" "$D/log" &&
	bad "C. records of tmp were found to skip - it never had any: $(grep 'not loaded' "$D/log" | cut -c1-160)" ||
	ok "C. nothing of tmp in the snapshot or the WAL to skip"

# ---- D ---------------------------------------------------------------
py 'for c in ("keep", "tmp"):
    call("set", col=c, key="late", value="L")
time.sleep(1.5)
print("ok")' > /dev/null
hardkill
: > "$D/log"
start
R=$(py 'print(count("keep"), count("tmp"), (call("get", col="keep", key="late")["result"] or {}).get("value"))')
[ "$R" = "120 0 L" ] && ok "D. kill -9 and a start: keep 120 with the late write, tmp empty" ||
	bad "D. after kill -9: $R"
stop

# ---- E ---------------------------------------------------------------
rm -rf "$D/wal" "$D/state"
mkconf yes
: > "$D/log"
start
R=$(py "$WRITE")
[ "$R" = "119 119" ] || bad "E. setup counts: $R"
stop
mkconf no
: > "$D/log"
start
R=$(py "$COUNTS")
L=$(grep "of cache collections (durable = no) not loaded" "$D/log" | head -1)
[ "$R" = "119 0 v119" ] && echo "$L" | grep -Eq "recover: [1-9][0-9]* snapshot and [0-9]+ WAL record" &&
	ok "E. tmp made a cache: it starts empty, keep whole; the start says it (${L#*recover: })" ||
	bad "E. after making tmp a cache: $R; log: $(grep 'recover:' "$D/log" | tail -2 | cut -c1-200 | tr '\n' '|')"

# ---- F ---------------------------------------------------------------
mkconf yes
kill -HUP $PID
i=0; while [ $i -lt 50 ] && ! grep -q "reload refused" "$D/log"; do sleep 0.1; i=$((i+1)); done
grep -q "reload refused: \[collection tmp\] durable changed and cannot be reloaded" "$D/log" &&
	ok "F. a reload that changes durable is refused, naming it" ||
	bad "F. reload: $(grep 'reload' "$D/log" | tail -2 | cut -c1-200 | tr '\n' '|')"
kill -0 $PID 2>/dev/null && ok "F. and the daemon runs on" || bad "F. the daemon is gone"
stop

echo "cachecoltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
