#!/bin/sh
# dumptest.sh - a chunked `dump` walk from a node that took none of the writes yields every live record
# of an eager fleet exactly once, with the door's value, TTL and version; slot ranges split it cleanly.
BIN=${1:-./perfcached}
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
TOOL=${2:-./perfdump}
D=$(mktemp -d /var/tmp/pcdu.XXXXXX)
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
# daemons die by the pids this script started - each checked against its
# command line - with the name-bound pattern as a fallback
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
mk() { # mk <id> <port>
	cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 32
[secrets]
client = du-client-secret
cluster = du-cluster-secret
[listen]
tcp = 127.0.0.1:$2
plaintext = loopback
[cluster]
multicast = 239.255.77.64:17164
advertise = 127.0.1.4$1
[collection dc]
buckets_log2 = 8
mode = eager
CONF
}
start() { # start <id>
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	PIDS="$PIDS $!"
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; cat "$D/n$1.log"; exit 1
}
# drive <port> <op> [args], one line out: fill <n> (dk<i>, TTL on even i, every 10th overwritten), dump
# <count> [lo hi] (chunks/records/dup, then "key value ttl ver" lines), get <key>, entries, buckets
drive() {
	python3 - "$@" <<'PYEOF'
import json, pcnative, socket, sys, os, time
port = int(sys.argv[1]); op = sys.argv[2]
s = socket.create_connection(("127.0.0.1", port), timeout=20)
f = pcnative.wrap(s); rid = [0]
def call(m, **p):
    rid[0] += 1
    r = {"jsonrpc": "2.0", "id": rid[0], "method": m}
    if p: r["params"] = p
    f.write((json.dumps(r) + "\n").encode()); f.flush()
    return json.loads(f.readline())
def val_of(rec, k="v"):
    # a key or value is a bulk on the wire: pcnative hands it back as the
    # bytes themselves (a str via surrogateescape), never an enc/k_enc sibling
    return rec.get(k)
if op == "fill":
    n = int(sys.argv[3]); done = 0
    for i in range(n):
        p = {"col": "dc", "key": "dk%d" % i, "value": "v%d%s" % (i, "x" * (i % 50))}
        if i % 2 == 0: p["ttl"] = 300
        if "error" not in call("set", **p): done += 1
    for i in range(0, n, 10):
        p = {"col": "dc", "key": "dk%d" % i, "value": "w%d" % i}
        if i % 2 == 0: p["ttl"] = 300
        call("set", **p)
    print("sets=%d" % done)
elif op == "dump":
    count = int(sys.argv[3]); lo = int(sys.argv[4]) if len(sys.argv) > 4 else None; hi = int(sys.argv[5]) if len(sys.argv) > 5 else None
    cursor = 0; chunks = 0; seen = {}; dup = 0; out = []; maxn = 0; table = None; restarts = 0
    # each record once only while the table is QUIET (a split moves records ahead of the
    # cursor): wait for the bucket count to stand still 1.5 s with no resize running
    last = None; still = 0
    for _ in range(120):
        st = call("stats")
        cc = [x for x in st.get("result", st)["collections"] if x["name"] == "dc"][0]
        now_shape = (cc["buckets"], "resizing_to" in cc)
        still = still + 1 if now_shape == last and not now_shape[1] else 0
        if still >= 3: break
        last = now_shape; time.sleep(0.5)
    while True:
        p = {"col": "dc", "cursor": cursor, "count": count}
        if lo is not None: p["slot_lo"] = lo; p["slot_hi"] = hi
        if table is not None: p["table"] = table
        r = call("dump", **p)
        # the node took none of the writes and grows as the push lands: a
        # resize under the walk restarts it (dumpresizetest has the why)
        if "error" in r and "table resized" in json.dumps(r["error"]) and restarts < 5:
            restarts += 1; cursor = 0; chunks = 0; seen = {}; dup = 0; out = []; maxn = 0; table = None
            continue
        if "error" in r: print("ERROR %s" % json.dumps(r["error"])[:120]); sys.exit(0)
        r = r["result"]; table = r.get("table"); chunks += 1; maxn = max(maxn, len(r["records"]))
        for rec in r["records"]:
            k = val_of(rec, "k")
            if k in seen: dup += 1
            seen[k] = 1
            out.append("%s %s %s %s" % (k, val_of(rec), rec["ttl"], rec["ver"]))
        cursor = r["cursor"]
        if not r["more"]: break
    print("chunks=%d records=%d dup=%d maxchunk=%d" % (chunks, len(seen), dup, maxn))
    for l in out: print(l)
elif op == "get":
    r = call("get", col="dc", key=sys.argv[3])
    if "error" in r or r.get("result") is None: print("MISSING")
    else:
        res = r["result"]; print("%s %s" % (val_of(res, "value") if "value" in res else val_of(res), res.get("ttl", -1)))
elif op == "entries":
    r = call("stats"); r = r.get("result", r)
    print(sum(c.get("entries", 0) for c in r.get("collections", []) if c.get("name") == "dc"))
elif op == "buckets":
    r = call("stats"); r = r.get("result", r)
    print(sum(c.get("buckets", 0) for c in r.get("collections", []) if c.get("name") == "dc"))
PYEOF
}
await_entries() { # await_entries <port> <want> <secs>
	i=0
	while [ $i -lt $(( $3 * 10 )) ]; do
		[ "$(drive $1 entries)" = "$2" ] && return 0
		sleep 0.1; i=$((i+1))
	done
	return 1
}

mk 1 17641; mk 2 17642; mk 3 17643
start 1; start 2; start 3
# every node READY before the writes: a node still pulling its bootstrap refuses them
i=0; while [ $i -lt 100 ]; do grep -q "recovering -> ready" "$D/n1.log" && grep -q "recovering -> ready" "$D/n2.log" && grep -q "recovering -> ready" "$D/n3.log" && break; sleep 0.1; i=$((i+1)); done

# 1. 2000 records at node 1 (1000 with a TTL, 200 overwritten), replicated to 2 and 3
R=$(drive 17641 fill 2000)
[ "$R" = "sets=2000" ] && ok "2000 records written at node 1, 200 of them twice" || bad "fill: $R"
await_entries 17643 2000 30 && ok "node 3 holds all 2000 by the push" || bad "node 3 holds $(drive 17643 entries), want 2000"
# 2000 records in 256 buckets make the maintenance thread split, and a split moves records ahead of a
# walk's cursor (at-least-once under a resize): wait until the bucket count stands still for 2 s
B0=$(drive 17643 buckets); i=0
while [ $i -lt 40 ]; do sleep 0.5; B1=$(drive 17643 buckets); [ "$B1" = "$B0" ] && i=$((i+1)) || i=0; B0=$B1; [ $i -ge 4 ] && break; done
echo "  ..   table settled at $B0 buckets"

# 2. dump from node 3 - which took none of the writes - in 64-bucket chunks
drive 17643 dump 64 > "$D/dump.out"
H=$(head -1 "$D/dump.out")
case "$H" in
	chunks=*) ;;
	*) bad "dump failed: $H";;
esac
C=$(echo "$H" | sed -n 's/chunks=\([0-9]*\).*/\1/p'); N=$(echo "$H" | sed -n 's/.*records=\([0-9]*\).*/\1/p'); DUP=$(echo "$H" | sed -n 's/.*dup=\([0-9]*\).*/\1/p')
[ "$N" = 2000 ] && ok "every record came out: $N" || bad "dump yielded $N records, want 2000 ($H)"
[ "$DUP" = 0 ] && ok "no record twice across $C chunks" || bad "$DUP records repeated across $C chunks (S112: chunks must end on a bucket boundary)"
[ "${C:-0}" -gt 1 ] && ok "the walk took several chunks ($C of 64 buckets)" || bad "one chunk only ($C): the budget is not a chunk"

# 3. each dumped record matches the door: value and TTL of get; versions differ for the overwritten
V=$(tail -n +2 "$D/dump.out" | awk '$1=="dk10"{print $2, $3, $4}'); set -- $V
[ "$1" = "w10" ] && ok "dk10 carries its second value (w10)" || bad "dk10 value '$1', want w10"
[ "${2:-0}" -gt 250 ] && [ "${2:-0}" -le 300 ] && ok "dk10 carries its TTL ($2 s left of 300)" || bad "dk10 ttl '$2', want 250..300"
E11=$(python3 -c 'print("v11"+"x"*11)')
V1=$(tail -n +2 "$D/dump.out" | awk '$1=="dk11"{print $2, $3, $4}'); set -- $V1
[ "$1" = "$E11" ] && ok "dk11 carries its value ($E11)" || bad "dk11 value '$1', want $E11"
[ "$2" = "-1" ] && ok "dk11 has no TTL (-1)" || bad "dk11 ttl '$2', want -1"
VA=$(tail -n +2 "$D/dump.out" | awk '$1=="dk20"{print $4}'); VB=$(tail -n +2 "$D/dump.out" | awk '$1=="dk21"{print $4}')
[ -n "$VA" ] && [ -n "$VB" ] && [ "$VA" -gt "$VB" ] 2>/dev/null && ok "the overwritten record carries the newer version (dk20 $VA > dk21 $VB)" \
	|| bad "versions: dk20 '$VA' dk21 '$VB' (S112: the version must ride the record)"
GV=$(drive 17643 get dk11 | cut -d' ' -f1); [ "$GV" = "$E11" ] && ok "the door agrees on dk11's value" || bad "get dk11 says '$GV'"

# 4. a slot range splits the keyspace: two halves, union = everything, no overlap
drive 17643 dump 64 0 8191 > "$D/lo.out"; drive 17643 dump 64 8192 16383 > "$D/hi.out"
NL=$(head -1 "$D/lo.out" | sed -n 's/.*records=\([0-9]*\).*/\1/p'); NH=$(head -1 "$D/hi.out" | sed -n 's/.*records=\([0-9]*\).*/\1/p')
[ $(( ${NL:-0} + ${NH:-0} )) = 2000 ] && [ "${NL:-0}" -gt 500 ] && [ "${NH:-0}" -gt 500 ] \
	&& ok "slot halves split the keyspace: $NL + $NH = 2000" || bad "slot halves: $NL + $NH (want 2000, both sides populated)"
OV=$(cat "$D/lo.out" "$D/hi.out" | tail -n +2 | awk '$1 ~ /^dk/{print $1}' | sort | uniq -d | wc -l)
[ "$OV" = 0 ] && ok "no key in both halves" || bad "$OV keys in both slot halves"

# 5. the overflow leg is chunked, not drained in one reply: with 16-bucket chunks the walk budgets
#    the leg itself (whole chains, at most one over) and still yields every record exactly once
drive 17643 dump 16 > "$D/small.out"; HS=$(head -1 "$D/small.out")
NS=$(echo "$HS" | sed -n 's/.*records=\([0-9]*\).*/\1/p'); DS=$(echo "$HS" | sed -n 's/.*dup=\([0-9]*\).*/\1/p'); CS=$(echo "$HS" | sed -n 's/chunks=\([0-9]*\).*/\1/p')
[ "$NS" = 2000 ] && [ "$DS" = 0 ] && ok "16-bucket chunks: every record once again ($CS chunks)" || bad "16-bucket chunks: $HS"
[ "${CS:-0}" -gt 20 ] && ok "the tail came out over several chunks ($CS chunks for 256 buckets)" || bad "only $CS chunks: the overflow leg was drained in one reply (S112: the tail must be chunked)"
MX=$(echo "$HS" | sed -n 's/.*maxchunk=\([0-9]*\).*/\1/p')
[ "${MX:-999}" -le 104 ] && ok "no chunk exceeded its budget: largest $MX records (16 buckets x 6 slots, plus at most one chain of the leg)" || bad "a chunk carried $MX records: the budget did not hold (S121)"

# 6. a count covering the whole table is never a cut: the leg is walked FIRST and the call that
#    finishes it goes on through the buckets, so the whole table is one chunk
drive 17643 dump 16384 > "$D/big.out"; R=$(head -1 "$D/big.out")
case "$R" in
	chunks=1\ records=2000\ dup=0*) ok "a whole-table count is one chunk, the leg then the buckets ($R)";;
	ERROR*halve*) ok "an oversized chunk is refused whole with the halving hint";;
	*) bad "whole-table count: $R";;
esac
# 6b. a chunk the door cannot queue (past PC_MAX_OUTQ, 8 MB) is refused with the halving hint, never a
#     dropped connection: 5000 values of 2000 bytes are a 10 MB whole-table chunk.  The table is sized for
#     them and the dump waits until it stands still: a split under the walk moves records past its cursor
cat > "$D/n4.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = du-client-secret
[listen]
tcp = 127.0.0.1:17644
plaintext = loopback
[collection dc]
buckets_log2 = 13
CONF
start 4
R=$(python3 - <<'PYEOF'
import json, pcnative, socket, time
s = socket.create_connection(("127.0.0.1", 17644), timeout=30); f = pcnative.wrap(s)
reqs = [json.dumps({"jsonrpc": "2.0", "id": i, "method": "set",
    "params": {"col": "dc", "key": "big%d" % i, "value": "%04d" % i + "x" * 1996}}) for i in range(5000)]
f.write(("\n".join(reqs) + "\n").encode()); f.flush()
for _ in reqs: f.readline()
last = None; still = 0
for _ in range(60):
    f.write((json.dumps({"jsonrpc": "2.0", "id": 8, "method": "stats", "params": {"col": "dc"}}) + "\n").encode()); f.flush()
    c = json.loads(f.readline())["result"]["collections"][0]
    shape = (c["entries"], c["buckets"], "resizing_to" in c)
    still = still + 1 if shape == last and not shape[2] else 0
    if still >= 3: break
    last = shape; time.sleep(0.5)
f.write((json.dumps({"jsonrpc": "2.0", "id": 9, "method": "dump",
    "params": {"col": "dc", "cursor": 0, "count": 16384}}) + "\n").encode()); f.flush()
l = f.readline()
if not l: print("closed"); raise SystemExit
r = json.loads(l)
print(("ERROR " + json.dumps(r["error"])) if "error" in r else "records=%d more=%s, the table %s" %
    (len(r["result"]["records"]), r["result"]["more"], shape))
PYEOF
)
case "$R" in
	ERROR*halve*) ok "a chunk past what the door can queue is refused with the halving hint";;
	*) bad "a 10 MB chunk: $R (the connection must stay and say halve)";;
esac
# 7. the tool: three connections on bucket ranges dump to a directory, every record once and equal
#    to the verb's; the manifest verifies; then the same through zstd
if [ -x "$TOOL" ]; then
	T=$("$TOOL" --from 127.0.0.1:17643 --collections dc --threads 3 --count 16 --zstd 0 --out "$D/dump" 2>&1 | tail -1)
	case "$T" in
		"perfdump: 2000 records"*) ok "the tool dumped every record over three bucket ranges ($T)";;
		*) bad "perfdump: $T";;
	esac
	I=$("$TOOL" --inspect "$D/dump" 2>&1 | tail -1)
	case "$I" in
		*"2000 records verified, 0 bad") ok "inspect verifies the manifest and every checksum ($I)";;
		*) bad "inspect: $I";;
	esac
	# read the chunk files back and compare with the verb's own dump, record by record
	RB=$(python3 - "$D/dump" "$D/dump.out" <<'PYEOF'
import sys, os, struct, time
d, ref = sys.argv[1], sys.argv[2]
want = {}
for line in open(ref).read().split("\n")[1:]:
    if not line: continue
    k, v, ttl, ver = line.split(" ")
    want[k] = (v, int(ttl), int(ver))
seen = {}; bad = 0; now = time.time() * 1000
for f in sorted(os.listdir(d)):
    if not f.endswith(".pcd"): continue
    b = open(os.path.join(d, f), "rb").read()
    assert b[:4] == b"PCD2", f
    off = 4
    while True:
        kl = struct.unpack_from("<I", b, off)[0]
        if kl == 0xFFFFFFFF:
            n = struct.unpack_from("<Q", b, off + 4)[0]; break
        vl, exp, ver, rtype = struct.unpack_from("<IQQB", b, off + 4)
        off += 25
        k = b[off:off + kl].decode(); v = b[off + kl:off + kl + vl].decode(); off += kl + vl
        if k in seen: bad += 1
        seen[k] = 1
        w = want.get(k)
        if not w: bad += 1; continue
        if w[0] != v or w[2] != ver or rtype != 0: bad += 1
        if w[1] < 0 and exp != 0: bad += 1
        if w[1] >= 0 and not (now + (w[1] - 20) * 1000 <= exp <= now + (w[1] + 5) * 1000): bad += 1
print("records=%d bad=%d" % (len(seen), bad))
PYEOF
)
	[ "$RB" = "records=2000 bad=0" ] && ok "the chunk files hold every record once, values, versions and absolute expiries agreeing with the verb" \
		|| bad "chunk files: $RB"
	# a manifest's file names reach no shell - a quote runs nothing, a path is refused
	mkdir -p "$D/evil"
	cat > "$D/evil/manifest.json" <<'EOF'
{"format":"perfdump-format","format_version":2,"source":"x","files":[{"file":"a'; touch s344-ran; echo '","collection":"c","records":0,"crc32":0,"complete":true},{"file":"../outside","collection":"c","records":0,"crc32":0,"complete":true}]}
EOF
	TOOLA=$(readlink -f "$TOOL")
	(cd "$D/evil" && "$TOOLA" --inspect "$D/evil" > "$D/evil.out" 2>&1)
	[ ! -e "$D/evil/s344-ran" ] && [ "$(grep -c 'cannot read' "$D/evil.out")" = 2 ] \
		&& ok "S344: a quote in a manifest's file name runs nothing, and a path is refused" \
		|| bad "S344: $(ls "$D/evil" | tr '\n' ' ')/ $(tr '\n' ' ' < "$D/evil.out" | cut -c1-200)"
	if ! command -v zstd >/dev/null 2>&1; then
		echo "  ..   SKIP the compressed case: no zstd binary on this host (the tool shells out to it, and --zstd 0 above already covered the format)"
		echo "dumptest: $pass passed, $fail failed"
		[ $fail = 0 ]
		exit
	fi
	T2=$("$TOOL" --from 127.0.0.1:17643 --collections dc --threads 2 --count 32 --zstd 3 --out "$D/dumpz" 2>&1 | tail -1)
	I2=$("$TOOL" --inspect "$D/dumpz" 2>&1 | tail -1)
	case "$T2|$I2" in
		"perfdump: 2000 records"*"2000 records verified, 0 bad") ok "through zstd: every record, every checksum ($(ls "$D/dumpz" | grep -c '\.zst$') compressed files)";;
		*) bad "zstd dump: $T2 / $I2";;
	esac
else
	echo "  ..   $TOOL not built: the tool cases are skipped"
fi
echo "dumptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
