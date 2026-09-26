#!/bin/sh
# dumpresizetest.sh - a `dump` walk across a table resize.
#
# THE BUG.  `dump` pages a collection by BUCKET cursor and resolved the
# collection's current table on every call.  A resize publishes a new
# table of another shape, so a cursor handed out by the old one indexes
# the new one wrongly: after a GROW, records of buckets already walked
# reappear past the cursor (duplicates); after a SHRINK, buckets not yet
# walked fold in behind it (records MISSED, silently).  perfdump is the
# backup tool.  Found by CI: rc32's check-asan loadtest dumped an eager
# member still growing from its push and got 114 records twice.
#
# THE FIX.  Every reply names the table it walked (`table`, with its
# `buckets`); a caller that hands `table` back is refused ("table resized
# during the dump") the moment the collection has been republished, and
# restarts that collection.  perfdump pins each collection's walkers to one
# table, plans their bucket ranges over THAT table, and dumps a resized
# collection again from scratch.  A caller that sends no `table` is served
# as before, so the hazard stays demonstrable (arms 1 and 2).
#
# One node, 3000 records, a collection of 1024 buckets, resizes forced
# with the privileged `resize` verb between two chunks:
#   1. GROW, no token: the walk repeats records (the hazard, shown);
#   2. SHRINK, no token: the walk misses records (the hazard, shown);
#   3. GROW, token: the chunk after the swap is refused, and a restart
#      yields all 3000 exactly once;
#   4. SHRINK, token: the same;
#   5. perfdump with a resize forced mid-run: it says it restarted the
#      collection, and its files hold all 3000 keys exactly once.
# FAIL-FIRST: 3, 4 and 5 fail on a build without the fix (no `table` in
# the reply, no refusal; perfdump writes the duplicates).
# usage: test/dumpresizetest.sh [./perfcached] [./perfcli] [./perfdump]
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
DUMP=${3:-./perfdump}
for b in "$CLI" "$DUMP"; do
	[ -x "$b" ] || { echo "dumpresizetest: $b not built - SKIPPED, and a skip is not a pass"; exit 0; }
done
D=$(mktemp -d /var/tmp/pcdr.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=17991
N=3000

mkdir -p "$D/s"
cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
log_level = notice
allow_create = yes
state_dir = $D/s
grow_at_pct = 100
[memory]
arena_mb = 128
[secrets]
client = dr-client
enable = dr-enable
[listen]
tcp = 127.0.0.1:$PORT
plaintext = loopback
[collection c]
buckets_log2 = 10
CONF
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
PID=$!
i=0
while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { echo "did not start: $(tail -2 "$D/n.log")"; exit 1; }

drive() { timeout 60 python3 - "$PORT" "$N" "$@" <<'PY'
import json, socket, sys
port, n, op = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
s = socket.create_connection(("127.0.0.1", port), timeout=30)
f = s.makefile("rwb")
rid = [0]
def call(m, **p):
    rid[0] += 1
    f.write((json.dumps({"jsonrpc": "2.0", "id": rid[0], "method": m, "params": p}) + "\n").encode()); f.flush()
    return json.loads(f.readline())
if op == "fill":
    for i in range(n):
        f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": "set",
                 "params": {"col": "c", "key": "k%05d" % i, "value": "v%d" % i}}) + "\n").encode())
    f.flush()
    bad = sum(1 for _ in range(n) if "error" in json.loads(f.readline()))
    print("stored %d" % (n - bad))
elif op == "buckets":
    r = call("stats")["result"]
    # " R" while a resize still runs: its two passes AFTER the swap keep it
    # running (a second resize is refused) once the bucket count has moved
    c = [c for c in r["collections"] if c["name"] == "c"][0]
    print("%d%s" % (c["buckets"], " R" if "resizing_to" in c else ""))
elif op == "first":
    # one chunk; prints the cursor, and the table token when asked for
    r = call("dump", col="c", cursor=0, count=int(sys.argv[4]))
    r = r["result"]
    open(sys.argv[5], "w").write(json.dumps({"keys": [x["k"] for x in r["records"]],
        "cursor": r["cursor"], "table": r.get("table")}))
    print("cursor %d table %s" % (r["cursor"], r.get("table")))
elif op == "rest":
    # carry on from the saved chunk; with "token", hand the table back and
    # restart the collection on a refusal
    st = json.load(open(sys.argv[4])); token = sys.argv[5] == "token"
    keys = list(st["keys"]); cursor = st["cursor"]; table = st["table"]; refused = 0
    while cursor:
        p = {"col": "c", "cursor": cursor, "count": 256}
        if token: p["table"] = table
        r = call("dump", **p)
        if "error" in r:
            if token and "table resized" in r["error"].get("message", ""):
                refused += 1
                if refused > 3: print("REFUSED FOREVER"); sys.exit(0)
                keys, cursor, table = [], 0, None
                r = call("dump", col="c", cursor=0, count=256)["result"]
                keys += [x["k"] for x in r["records"]]; cursor = r["cursor"]; table = r.get("table")
                continue
            print("ERROR %s" % json.dumps(r["error"])[:120]); sys.exit(0)
        r = r["result"]
        keys += [x["k"] for x in r["records"]]; cursor = r["cursor"]
    print("records %d distinct %d dups %d missing %d refused %d" % (len(keys), len(set(keys)),
          len(keys) - len(set(keys)), n - len(set(keys)), refused))
PY
}
resize() { # resize <log2> : start it, and wait until it has FINISHED - swap and after
	"$CLI" -q -h 127.0.0.1 -p $PORT -E dr-enable resize c "$1" > "$D/rs.out" 2>&1
	grep -q '"resizing"' "$D/rs.out" || { echo "    resize to 2^$1 refused: $(head -1 "$D/rs.out")"; return 1; }
	want=$((1 << $1)); j=0
	while [ $j -lt 100 ]; do
		[ "$(drive buckets)" = "$want" ] && return 0
		sleep 0.1; j=$((j + 1))
	done
	echo "    the resize to $want buckets did not finish"; return 1
}

R=$(drive fill)
[ "$R" = "stored $N" ] && ok "filled: $N records in 1024 buckets" || bad "fill: $R"

echo "--- 1. GROW between two chunks, no token (the hazard)"
drive first 256 "$D/st" > /dev/null
resize 12 || bad "resize 1024 -> 4096"
R=$(drive rest "$D/st" legacy)
echo "    $R"
# the new table is not the old one's buckets split in place, so a grow can
# lose records as well as repeat them - either shows the hazard
D1=$(echo "$R" | sed -n 's/.* dups \([0-9]*\) .*/\1/p')
M1=$(echo "$R" | sed -n 's/.* missing \([0-9]*\) .*/\1/p')
[ -n "$D1" ] && [ -n "$M1" ] && [ $((D1 + M1)) -gt 0 ] \
	&& ok "a cursor carried into the grown table repeats $D1 records and misses $M1 - the bug CI hit" \
	|| bad "the grow walk came out exact: this arm no longer shows the hazard ($R)"

echo "--- 2. SHRINK between two chunks, no token (the hazard)"
drive first 1024 "$D/st" > /dev/null
resize 10 || bad "resize 4096 -> 1024"
R=$(drive rest "$D/st" legacy)
echo "    $R"
M2=$(echo "$R" | sed -n 's/.* missing \([0-9]*\) .*/\1/p')
[ -n "$M2" ] && [ "$M2" -gt 0 ] \
	&& ok "a cursor carried into the shrunk table MISSES $M2 of $N records, silently" \
	|| bad "nothing missed after a shrink: $R"

echo "--- 3. GROW between two chunks, the reply's table handed back"
R=$(drive first 256 "$D/st")
case "$R" in *"table None"*) bad "the dump reply names no table";; esac
resize 12 || bad "resize 1024 -> 4096"
R=$(drive rest "$D/st" token)
echo "    $R"
[ "$R" = "records $N distinct $N dups 0 missing 0 refused 1" ] \
	&& ok "the chunk after the swap was refused, and the restart dumped all $N exactly once" \
	|| bad "grow with the token: $R"

echo "--- 4. SHRINK between two chunks, the reply's table handed back"
drive first 1024 "$D/st" > /dev/null
resize 10 || bad "resize 4096 -> 1024"
R=$(drive rest "$D/st" token)
echo "    $R"
[ "$R" = "records $N distinct $N dups 0 missing 0 refused 1" ] \
	&& ok "refused after the shrink too, and the restart dumped all $N exactly once" \
	|| bad "shrink with the token: $R"

echo "--- 5. perfdump with a resize forced mid-run"
# paced (~6 s: 1500 records per connection at 250/s) and small-chunked so
# the resize's SWAP - two maintenance ticks after it starts - lands inside
# the run; at 600/s the run ended first and nothing was tested.
# Uncompressed, so the files can be read back here.
"$DUMP" --from 127.0.0.1:$PORT --out "$D/dump" --threads 2 --count 8 \
	--rate 250 --zstd 0 > "$D/pd.out" 2>&1 &
DP=$!
sleep 1
resize 12 || bad "resize under perfdump"
wait $DP
PRC=$?
echo "    $(grep -E "^perfdump: " "$D/pd.out" | tr '\n' ' ' | cut -c1-230)"
grep -q "was resized during the dump" "$D/pd.out" \
	&& ok "perfdump saw the resize and dumped the collection again" \
	|| bad "perfdump did not notice the resize (rc $PRC)"
R=$(python3 - "$D/dump" <<'PY'
import glob, struct, sys
keys = []
for fn in glob.glob(sys.argv[1] + "/*.pcd"):
    b = open(fn, "rb").read(); p = 4
    while p + 4 <= len(b):
        kl = struct.unpack_from("<I", b, p)[0]
        if kl == 0xFFFFFFFF: break
        vl = struct.unpack_from("<I", b, p + 4)[0]; p += 4 + 4 + 8 + 8 + 1
        keys.append(b[p:p + kl]); p += kl + vl
print("records %d distinct %d" % (len(keys), len(set(keys))))
PY
)
echo "    files: $R"
[ $PRC -eq 0 ] && [ "$R" = "records $N distinct $N" ] \
	&& ok "and its files hold all $N keys exactly once" \
	|| bad "perfdump's files: $R (rc $PRC)"

echo "dumpresizetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
