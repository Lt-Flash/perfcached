#!/bin/sh
# dumptest.sh - S112 part 1: the `dump` verb.  A three-node eager fleet is
# filled with records carrying TTLs (some none) and overwritten once so
# versions differ; a chunked walk of `dump` from a node that took NONE of
# the writes must yield every live record exactly once, with the value,
# the TTL and the version the door reports for it, over several chunks
# and with a slot range that splits the keyspace into two halves whose
# union is the whole.  Fail-first: a daemon without `dump` answers
# method-not-found and every assertion here fails.
BIN=${1:-./perfcached}
TOOL=${2:-./perfdump}
D=$(mktemp -d /var/tmp/pcdu.XXXXXX)
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
# daemons die by the pids this script started - each checked against its
# command line - with the name-bound pattern as a fallback
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT TERM INT
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
# drive <port> <op> [args]: ops print one line
#   fill <n>            -> writes n records: key dk<i>, value v<i> + "x"*(i%50), ttl 300 on even i, none on odd;
#                          then overwrites every 10th with value "w<i>" (a second version); prints "sets=<n>"
#   dump <count> [lo hi] -> walks dump in chunks of <count> buckets (optional slot range) and prints
#                           (DUMP_TAIL_LIMIT in the environment lowers the tail's per-call limit)
#                           "chunks=<c> records=<r> dup=<d>" then one line per record "key value ttl ver"
#   get <key>           -> "value ttl" from get (ttl -1 = none), or MISSING
#   entries             -> entries of dc
#   buckets             -> buckets of dc
drive() {
	python3 - "$@" <<'PYEOF'
import json, socket, sys, base64, os
port = int(sys.argv[1]); op = sys.argv[2]
s = socket.create_connection(("127.0.0.1", port), timeout=20)
f = s.makefile("rwb"); rid = [0]
def call(m, **p):
    rid[0] += 1
    r = {"jsonrpc": "2.0", "id": rid[0], "method": m}
    if p: r["params"] = p
    f.write((json.dumps(r) + "\n").encode()); f.flush()
    return json.loads(f.readline())
def val_of(rec, k="v", enc="enc"):
    v = rec.get(k)
    if rec.get(enc) == "b64": v = base64.b64decode(v).decode("latin1")
    return v
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
    cursor = 0; chunks = 0; seen = {}; dup = 0; out = []; maxn = 0
    while True:
        p = {"col": "dc", "cursor": cursor, "count": count}
        if lo is not None: p["slot_lo"] = lo; p["slot_hi"] = hi
        if os.environ.get("DUMP_TAIL_LIMIT"): p["tail_limit"] = int(os.environ["DUMP_TAIL_LIMIT"])
        r = call("dump", **p)
        if "error" in r: print("ERROR %s" % json.dumps(r["error"])[:120]); sys.exit(0)
        r = r["result"]; chunks += 1; maxn = max(maxn, len(r["records"]))
        for rec in r["records"]:
            k = val_of(rec, "k", "k_enc")
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
        res = r["result"]; print("%s %s" % (val_of(res, "value", "enc") if "value" in res else val_of(res), res.get("ttl", -1)))
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
# 2000 records in 256 buckets is load factor 7.8: the maintenance thread
# splits 128 buckets a second until it is under four, and a split moves
# half a bucket's records AHEAD of a walk's cursor, where it sees them
# again - the cursor's at-least-once contract under a resize, the same
# one a tool must document.  Every assertion below wants a quiet table,
# so wait until the bucket count has stood still for two seconds.
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

# 5. the overflow leg is chunked, not drained in one reply: 2000 records in
#    256 buckets of 6 slots put hundreds in the leg; with 16-bucket chunks
#    and the tail's limit lowered to 64 walked records (its default is the
#    reply's budget, which this small leg would fit in one call) every
#    chunk stays within its budget and the walk still yields every record
#    exactly once
DUMP_TAIL_LIMIT=64 drive 17643 dump 16 > "$D/small.out"; HS=$(head -1 "$D/small.out")
NS=$(echo "$HS" | sed -n 's/.*records=\([0-9]*\).*/\1/p'); DS=$(echo "$HS" | sed -n 's/.*dup=\([0-9]*\).*/\1/p'); CS=$(echo "$HS" | sed -n 's/chunks=\([0-9]*\).*/\1/p')
[ "$NS" = 2000 ] && [ "$DS" = 0 ] && ok "16-bucket chunks: every record once again ($CS chunks)" || bad "16-bucket chunks: $HS"
[ "${CS:-0}" -gt 20 ] && ok "the tail came out over several chunks ($CS chunks for 256 buckets)" || bad "only $CS chunks: the overflow leg was drained in one reply (S112: the tail must be chunked)"
MX=$(echo "$HS" | sed -n 's/.*maxchunk=\([0-9]*\).*/\1/p')
[ "${MX:-999}" -le 96 ] && ok "no chunk exceeded its budget: largest $MX records (16 buckets x 6 slots, or 64 walked in the tail)" || bad "a chunk carried $MX records: the budget did not hold (S112)"

# 6. a count covering the whole table is two chunks - every bucket but the
#    last, then the tail - never one reply that drains the leg, and never a cut
drive 17643 dump 16384 > "$D/big.out"; R=$(head -1 "$D/big.out")
case "$R" in
	chunks=2\ records=2000\ dup=0*) ok "a whole-table count is two chunks, the buckets then the tail ($R)";;
	ERROR*halve*) ok "an oversized chunk is refused whole with the halving hint";;
	*) bad "whole-table count: $R";;
esac
# 7. the tool: three connections on bucket ranges dump the collection to a
#    directory; every record once with its value, ttl and version equal to
#    the verb's; the manifest verifies; then the same through zstd
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
    assert b[:4] == b"PCD1", f
    off = 4
    while True:
        kl = struct.unpack_from("<I", b, off)[0]
        if kl == 0xFFFFFFFF:
            n = struct.unpack_from("<Q", b, off + 4)[0]; break
        vl, exp, ver, flags = struct.unpack_from("<IQQB", b, off + 4)
        off += 25
        k = b[off:off + kl].decode(); v = b[off + kl:off + kl + vl].decode(); off += kl + vl
        if k in seen: bad += 1
        seen[k] = 1
        w = want.get(k)
        if not w: bad += 1; continue
        if w[0] != v or w[2] != ver: bad += 1
        if w[1] < 0 and exp != 0: bad += 1
        if w[1] >= 0 and not (now + (w[1] - 20) * 1000 <= exp <= now + (w[1] + 5) * 1000): bad += 1
print("records=%d bad=%d" % (len(seen), bad))
PYEOF
)
	[ "$RB" = "records=2000 bad=0" ] && ok "the chunk files hold every record once, values, versions and absolute expiries agreeing with the verb" \
		|| bad "chunk files: $RB"
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
