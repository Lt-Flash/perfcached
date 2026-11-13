#!/bin/sh
# loadtest.sh - the `restore` verb and perfload: a dumped fleet loads into empty shard and eager
# fleets with every value, version and absolute expiry intact.
BIN=${1:-./perfcached}
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
DUMP=${2:-./perfdump}
LOAD=${3:-./perfload}
D=$(mktemp -d /var/tmp/pclo.XXXXXX)
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
if command -v zstd >/dev/null 2>&1; then Z=3; else Z=0; fi
# mk <id> <addr> <port> <mode> [<shard collections>]
mk() {
	{
	cat <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 32
[secrets]
client = lo-client-secret
cluster = lo-cluster-secret
# restore is privileged: without this, perfload cannot raise privilege and every
# connection it opens is refused
enable = lo-enable-secret
[listen]
tcp = $2:$3
plaintext = loopback
[cluster]
multicast = $5
advertise = $2
CONF
	if [ "$4" = shard ]; then
		printf 'mode = shard\ncollections = dc,dl\n[collection dc]\nbuckets_log2 = 8\n[collection dl]\nbuckets_log2 = 8\n'
	else
		printf '[collection dc]\nbuckets_log2 = 8\nmode = eager\n[collection dl]\nbuckets_log2 = 8\nmode = eager\n'
	fi
	} > "$D/n$1.conf"
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
ready() { # ready <id>...: every node READY (a node still pulling its bootstrap refuses writes)
	i=0
	while [ $i -lt 100 ]; do
		all=1
		for n in "$@"; do grep -q -- "-> ready" "$D/n$n.log" || all=0; done
		[ $all = 1 ] && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "nodes $* never became ready"; exit 1
}
# drive <addr:port> <op> [args]: fill <n>, dump <col> (expiry absolute, unix s), get, set, entries,
# buckets, compare <src> <got> <tol> (exp_bad: expiries further apart than <tol> s)
drive() {
	python3 - "$@" <<'PYEOF'
import json, pcnative, socket, sys, time
op = sys.argv[2]
def conn():
    h, p = sys.argv[1].rsplit(":", 1)
    s = socket.create_connection((h, int(p)), timeout=20); return pcnative.wrap(s)
rid = [0]
def call(f, m, **p):
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
    f = conn(); n = int(sys.argv[3]); done = 0
    for i in range(n):
        p = {"col": "dc", "key": "dk%d" % i, "value": "v%d%s" % (i, "x" * (i % 50))}
        if i % 2 == 0: p["ttl"] = 300
        if "error" not in call(f, "set", **p): done += 1
    for i in range(0, n, 10):
        p = {"col": "dc", "key": "dk%d" % i, "value": "w%d" % i}
        if i % 2 == 0: p["ttl"] = 300
        call(f, "set", **p)
    if "error" not in call(f, "set", col="dc", key="dkshort", value="gone", ttl=15): done += 1
    print("sets=%d" % done)
elif op == "dump":
    # pinned to the table the first chunk walked, and restarted if the collection is
    # resized under it
    f = conn(); col = sys.argv[3]; cursor = 0; out = []; table = None; restarts = 0
    # each record once only while the table is QUIET (a split moves records ahead of the
    # cursor): wait for the bucket count to stand still 1.5 s with no resize running
    last = None; still = 0
    for _ in range(120):
        st = call(f, "stats")
        cc = [x for x in st.get("result", st)["collections"] if x["name"] == col][0]
        now_shape = (cc["buckets"], "resizing_to" in cc)
        still = still + 1 if now_shape == last and not now_shape[1] else 0
        if still >= 3: break
        last = now_shape; time.sleep(0.5)
    while True:
        p = {"col": col, "cursor": cursor, "count": 64}
        if table is not None: p["table"] = table
        r = call(f, "dump", **p)
        if "error" in r and "table resized" in json.dumps(r["error"]) and restarts < 5:
            restarts += 1; cursor = 0; out = []; table = None
            continue
        if "error" in r: print("ERROR %s" % json.dumps(r["error"])[:120]); sys.exit(0)
        r = r["result"]; table = r.get("table")
        now = int(time.time())
        for rec in r["records"]:
            exp = now + rec["ttl"] if rec["ttl"] >= 0 else -1
            out.append("%s %s %s %s" % (val_of(rec, "k"), val_of(rec), exp, rec["ver"]))
        cursor = r["cursor"]
        if not r["more"]: break
    for l in sorted(out): print(l)
elif op == "get":
    f = conn(); r = call(f, "get", col=sys.argv[3], key=sys.argv[4])
    if "error" in r or r.get("result") is None or not r["result"].get("found", True) or "value" not in r["result"]: print("MISSING")
    else:
        res = r["result"]; print("%s %s" % (val_of(res, "value"), res.get("ttl", -1)))
elif op == "set":
    f = conn(); r = call(f, "set", col=sys.argv[3], key=sys.argv[4], value=sys.argv[5])
    print("ERROR" if "error" in r else "OK")
elif op in ("entries", "buckets"):
    f = conn(); r = call(f, "stats"); r = r.get("result", r)
    print(sum(c.get(op, 0) for c in r.get("collections", []) if c.get("name") == sys.argv[3]))
elif op == "compare":
    tol = int(sys.argv[5]); src = {}; got = {}; dups = 0
    for l in open(sys.argv[3]):
        k, v, ttl, ver = l.split(); src[k] = (v, int(ttl), int(ver))
    for l in open(sys.argv[4]):
        k, v, ttl, ver = l.split()
        if k in got: dups += 1
        got[k] = (v, int(ttl), int(ver))
    same = dv = dver = tb = 0
    for k, (v, ttl, ver) in src.items():
        g = got.get(k)
        if not g: continue
        if g[0] != v: dv += 1
        if g[2] != ver: dver += 1
        if (ttl < 0) != (g[1] < 0) or (ttl >= 0 and abs(g[1] - ttl) > tol): tb += 1
        if g[0] == v and g[2] == ver: same += 1
    missing = [k for k in src if k not in got]; extra = [k for k in got if k not in src]
    print("same=%d diff_val=%d diff_ver=%d exp_bad=%d missing=%d extra=%d dups=%d" % (same, dv, dver, tb, len(missing), len(extra), dups))
    if missing: print("  missing e.g. %s" % " ".join(sorted(missing)[:5]))
    if extra: print("  extra e.g. %s" % " ".join(sorted(extra)[:5]))
PYEOF
}
settle_buckets() { # settle_buckets <addr:port>: the table has stood still for two seconds
	B0=$(drive $1 buckets dc); i=0
	while [ $i -lt 40 ]; do sleep 0.5; B1=$(drive $1 buckets dc); [ "$B1" = "$B0" ] && i=$((i+1)) || i=0; B0=$B1; [ $i -ge 4 ] && break; done
}
last() { tail -1 "$1"; }

# ---- the source: a three-node eager fleet, filled at node 1, dumped from node 3
A1=127.0.1.51:17651 A2=127.0.1.52:17652 A3=127.0.1.53:17653
mk 1 127.0.1.51 17651 eager 239.255.77.66:17166
mk 2 127.0.1.52 17652 eager 239.255.77.66:17166
mk 3 127.0.1.53 17653 eager 239.255.77.66:17166
start 1; start 2; start 3; ready 1 2 3
R=$(drive $A1 fill 2000)
[ "$R" = "sets=2001" ] && ok "2001 records written at node 1 (200 of them twice, one with a 2 s TTL)" || bad "fill: $R"
i=0; while [ $i -lt 300 ]; do [ "$(drive $A3 entries dc)" = 2001 ] && break; sleep 0.1; i=$((i+1)); done
[ "$(drive $A3 entries dc)" = 2001 ] && ok "node 3 holds all 2001 by the push" || bad "node 3 holds $(drive $A3 entries dc)"
settle_buckets $A3
drive $A3 dump dc > "$D/src.txt"
T=$(env -u PERFCLI_AUTH "$DUMP" --from $A3 --collections dc --threads 3 --count 16 --zstd $Z --out "$D/dump" 2>&1 | last /dev/stdin)
case "$T" in "perfdump: 2001 records"*) ok "dumped from node 3: $(ls "$D/dump" | grep -c '\.pcd') chunk files";; *) bad "dump: $T";; esac
grep -v "^dkshort " "$D/src.txt" > "$D/src_live.txt"

# ---- target 1: an EMPTY two-node SHARD fleet (a different size and a different mode)
B1=127.0.1.61:17661 B2=127.0.1.62:17662
mk 4 127.0.1.61 17661 shard 239.255.77.67:17164
mk 5 127.0.1.62 17662 shard 239.255.77.67:17164
start 4; start 5; ready 4 5
i=0; while [ $i -lt 300 ]; do [ "$(drive $A3 get dc dkshort)" = MISSING ] && break; sleep 0.1; i=$((i+1)); done
[ "$(drive $A3 get dc dkshort)" = MISSING ] && ok "dkshort expired at the source between the dump and the load" || bad "dkshort still at the source: $(drive $A3 get dc dkshort)"
sleep 2   # the daemon expires on whole ticks; the file holds the millisecond expiry, so give it the second
LOG="$D/load1.txt"
env -u PERFCLI_AUTH "$LOAD" --enable lo-enable-secret "$D/dump" --to $B1 --threads 2 --batch 100 > "$LOG" 2>&1; rc=$?
L=$(grep "^perfload: 2001 records" "$LOG")
case "$L" in
	*"stored 2000, older 0, existing 0, expired 1, refused 0, bad 0, rerouted 0"*) ok "loaded into the shard fleet: 2000 stored, the expired one skipped and counted (rc=$rc)";;
	*) bad "load 1: rc=$rc $(cat "$LOG" | tr '\n' '|')";;
esac
grep -q "perfload: peers settled" "$LOG" && ok "the final lines say when the members settled" || bad "no settle line: $(cat "$LOG" | tr '\n' '|')"
{ drive $B1 dump dc; drive $B2 dump dc; } > "$D/got1.txt"
C=$(drive x compare "$D/src_live.txt" "$D/got1.txt" 2 | head -1)
[ "$C" = "same=2000 diff_val=0 diff_ver=0 exp_bad=0 missing=0 extra=0 dups=0" ] \
	&& ok "every record on exactly one owner with its value, its VERSION and its absolute expiry" || bad "shard fleet holds: $C"
[ "$(drive $B1 get dc dkshort)" = MISSING ] && ok "the record that expired between dump and load is absent" || bad "dkshort: $(drive $B1 get dc dkshort)"
E=$(( $(drive $B1 entries dc) + $(drive $B2 entries dc) ))
[ "$E" = 2000 ] && ok "entries over both owners = 2000" || bad "entries $E"

# ---- policies over a fleet holding a NEWER copy of dk5
[ "$(drive $B1 set dc dk5 newer5)" = OK ] || bad "set dk5"
env -u PERFCLI_AUTH "$LOAD" --enable lo-enable-secret "$D/dump" --to $B1 --threads 2 --restart --no-settle > "$LOG" 2>&1
L=$(grep "^perfload: 2001 records" "$LOG")
case "$L" in *"stored 0, older 2000, existing 0, expired 1, refused 0"*) ok "newer: a re-load stores nothing, every record counted older (the same version is as old)";; *) bad "newer: $L";; esac
[ "$(drive $B1 get dc dk5)" = "newer5 -1" ] && ok "newer: the fleet's newer copy of dk5 stands" || bad "dk5 after newer: $(drive $B1 get dc dk5)"
env -u PERFCLI_AUTH "$LOAD" --enable lo-enable-secret "$D/dump" --to $B1 --threads 2 --restart --no-settle --policy skip > "$LOG" 2>&1
L=$(grep "^perfload: 2001 records" "$LOG")
case "$L" in *"stored 0, older 0, existing 2000, expired 1"*) ok "skip: every existing key left alone and counted";; *) bad "skip: $L";; esac
env -u PERFCLI_AUTH "$LOAD" --enable lo-enable-secret "$D/dump" --to $B1 --threads 2 --restart --no-settle --policy overwrite > "$LOG" 2>&1
L=$(grep "^perfload: 2001 records" "$LOG")
case "$L" in *"stored 2000, older 0, existing 0, expired 1"*) ok "overwrite: every record installed again under a fresh version";; *) bad "overwrite: $L";; esac
[ "$(drive $B1 get dc dk5)" = "v5xxxxx -1" ] && ok "overwrite: dk5 is the dumped value again" || bad "dk5 after overwrite: $(drive $B1 get dc dk5)"

# ---- the resume file: a finished load is skipped; one chunk taken off the file loads alone, without doubling anything
env -u PERFCLI_AUTH "$LOAD" --enable lo-enable-secret "$D/dump" --to $B1 --threads 2 --no-settle > "$LOG" 2>&1
grep -q "^perfload: 0 records" "$LOG" && grep -q "skipped as already loaded" "$LOG" && ok "a finished load is skipped by perfload.done" || bad "resume skip: $(cat "$LOG" | tr '\n' '|')"
N=$(wc -l < "$D/dump/perfload.done")
head -n $((N - 1)) "$D/dump/perfload.done" > "$D/done.tmp" && mv "$D/done.tmp" "$D/dump/perfload.done"
LASTF=$(ls "$D/dump" | grep '\.pcd' | while read f; do grep -q "^$f " "$D/dump/perfload.done" || echo "$f"; done | head -1)
env -u PERFCLI_AUTH "$LOAD" --enable lo-enable-secret "$D/dump" --to $B1 --threads 2 --no-settle > "$LOG" 2>&1
L=$(grep "^perfload: [0-9]* records" "$LOG")
case "$L" in "perfload: 0 records"*) bad "resume: nothing loaded: $L";; *"stored 0, older "*", existing 0, expired "*"resumed"*) ok "resume: only the chunk not marked done ($LASTF) is read again, and stores nothing twice";; *) bad "resume: $L";; esac
E=$(( $(drive $B1 entries dc) + $(drive $B2 entries dc) ))
[ "$E" = 2000 ] && ok "entries unchanged at 2000 after the resume" || bad "entries $E after resume"

# ---- into another collection, and the dry run
# without --no-settle: dl is NEW on the shard fleet, and a record still moving between owners is on both
env -u PERFCLI_AUTH "$LOAD" --enable lo-enable-secret "$D/dump" --to $B1 --threads 2 --restart --collection-map dc=dl > "$LOG" 2>&1
{ drive $B1 dump dl; drive $B2 dump dl; } > "$D/gotl.txt"
C=$(drive x compare "$D/src_live.txt" "$D/gotl.txt" 2 | head -1)
[ "$C" = "same=2000 diff_val=0 diff_ver=0 exp_bad=0 missing=0 extra=0 dups=0" ] && ok "--collection-map dc=dl: the same records, versions and expiries in dl" || bad "dl holds: $C"
env -u PERFCLI_AUTH "$LOAD" --enable lo-enable-secret "$D/dump" --to $B1 --threads 2 --restart --dry-run --collection-map dc=nope > "$LOG" 2>&1 && bad "a map onto a missing collection was accepted" || ok "a map onto a collection the target lacks is refused before anything loads"
env -u PERFCLI_AUTH "$LOAD" --enable lo-enable-secret "$D/dump" --to $B1 --threads 2 --restart --dry-run > "$LOG" 2>&1
grep -q "^perfload: dry run: 2001 records" "$LOG" && ok "dry run counts 2001 records and loads nothing" || bad "dry run: $(cat "$LOG" | tr '\n' '|')"

# ---- everything to ONE node of the shard fleet: the daemon names what belongs elsewhere, the loader re-sends it
env -u PERFCLI_AUTH "$LOAD" --enable lo-enable-secret "$D/dump" --to $B1 --threads 2 --restart --no-settle --no-route --policy overwrite > "$LOG" 2>&1
L=$(grep "^perfload: 2001 records" "$LOG")
RR=$(echo "$L" | sed -n 's/.*rerouted \([0-9]*\).*/\1/p')
case "$L" in *"stored 2000, older 0, existing 0, expired 1, refused 0, bad 0, rerouted "[1-9]*) ok "--no-route: $RR records came back named and were re-sent to their owner; 2000 stored, 0 refused";; *) bad "no-route: $L";; esac
{ drive $B1 dump dc; drive $B2 dump dc; } > "$D/got2.txt"
C=$(drive x compare "$D/src_live.txt" "$D/got2.txt" 2 | head -1)
case "$C" in "same=0 diff_val=0 diff_ver=2000 exp_bad=0 missing=0 extra=0 dups=0") ok "after the re-route every record sits on exactly one owner (fresh versions: overwrite)";; *) bad "after re-route: $C";; esac

# ---- the fail-first control: the same file set through plain SET (--via-set) gets fresh versions
env -u PERFCLI_AUTH "$LOAD" --enable lo-enable-secret "$D/dump" --to $B1 --threads 2 --restart --no-settle --via-set --collection-map dc=dl > "$LOG" 2>&1
{ drive $B1 dump dl; drive $B2 dump dl; } > "$D/gotv.txt"
C=$(drive x compare "$D/src_live.txt" "$D/gotv.txt" 2 | head -1)
case "$C" in *"diff_ver=2000"*"missing=0 extra=0"*) ok "control: a loader on plain SET fails the version assertion on all 2000 records";; *) bad "control: $C $(grep -v '^perfload:' "$LOG" | head -2 | tr '\n' '|')";; esac

# ---- target 2: an EMPTY two-node EAGER fleet: files spread over both members, the push carries the rest
C1=127.0.1.71:17671 C2=127.0.1.72:17672
mk 6 127.0.1.71 17671 eager 239.255.77.68:17162
mk 7 127.0.1.72 17672 eager 239.255.77.68:17162
start 6; start 7; ready 6 7
env -u PERFCLI_AUTH "$LOAD" --enable lo-enable-secret "$D/dump" --to $C1 --threads 2 --restart > "$LOG" 2>&1
L=$(grep "^perfload: 2001 records" "$LOG")
case "$L" in *"stored 2000, older 0, existing 0, expired 1, refused 0"*) ok "loaded into the eager fleet: 2000 stored";; *) bad "eager load: $L";; esac
S=$(grep "^perfload: peers settled" "$LOG")
case "$S" in *"=2000 "*"=2000") ok "the push carried every loaded record to the other member: $S";; *) bad "eager settle: $(cat "$LOG" | tr '\n' '|')";; esac
drive $C2 dump dc > "$D/gote.txt"
C=$(drive x compare "$D/src_live.txt" "$D/gote.txt" 2 | head -1)
[ "$C" = "same=2000 diff_val=0 diff_ver=0 exp_bad=0 missing=0 extra=0 dups=0" ] && ok "the copies on the member that took no file carry the dumped versions and expiries" || bad "eager member 2 holds: $C"

# a manifest naming anything but a file in the dump's directory is refused
# before a byte is read - no shell ever sees the name
mkdir -p "$D/evil"
cat > "$D/evil/manifest.json" <<'EOF'
{"format":"perfdump-format","format_version":2,"source":"x","files":[{"file":"../x'; touch s344-ran; echo '","collection":"c","records":0,"crc32":0,"complete":true}]}
EOF
LOADA=$(readlink -f "$LOAD")
(cd "$D/evil" && env -u PERFCLI_AUTH "$LOADA" --enable lo-enable-secret "$D/evil" --to $B1 > "$D/evil.out" 2>&1)
[ $? -ne 0 ] && [ ! -e "$D/evil/s344-ran" ] && grep -q "not a file in the dump's directory" "$D/evil.out" \
	&& ok "S344: a manifest naming a path is refused, and nothing ran" \
	|| bad "S344: $(ls "$D/evil" | tr '\n' ' ')/ $(tr '\n' ' ' < "$D/evil.out" | cut -c1-200)"

# ---- every type the daemon dumps, and a value over 64 KB: two standalone nodes with a RESP door,
# filled on one over RESP, dumped there and loaded into the other
mk1() { # mk1 <id> <addr> <port> <resp port>
	cat > "$D/n$1.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 32
[secrets]
client = lo-client-secret
enable = lo-enable-secret
[listen]
tcp = $2:$3
resp = $2:$4
plaintext = loopback
[collection 0]
buckets_log2 = 8
CONF
}
# rx <addr:port> <arg>...: one RESP command, its reply flattened ([a,b], nil, -ERR ..).  @big is
# a 100,000-byte binary value, @save/@exists a Symfony lock script's SHA1, @now the time
rx() {
	python3 - "$@" <<'PYEOF'
import hashlib, socket, sys, time
from symfony_lock import SHA
h, p = sys.argv[1].rsplit(":", 1)
big = bytes(range(256)) * 390 + bytes(160)
sub = {"@big": big, "@save": SHA["save"].encode(), "@exists": SHA["exists"].encode(),
       "@now": ("%.4f" % time.time()).encode()}
a = [sub.get(x, x.encode()) for x in sys.argv[2:]]
s = socket.create_connection((h, int(p)), timeout=10)
s.sendall(b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a))
f = s.makefile("rb")
def rd():
    l = f.readline().rstrip(b"\r\n").decode()
    t, v = l[0], l[1:]
    if t == "*": return "nil" if v == "-1" else "[" + ",".join(rd() for _ in range(int(v))) + "]"
    if t == "$":
        if v == "-1": return "nil"
        b = f.read(int(v) + 2)[:-2]
        if len(b) > 64: return "<%d bytes sha1 %s>" % (len(b), hashlib.sha1(b).hexdigest()[:16])
        return b.decode(errors="replace")
    return v if t in ":+" else "-" + v
print(rd())
PYEOF
}
mk1 8 127.0.1.81 17676 17678
mk1 9 127.0.1.82 17677 17679
start 8; start 9
S8=127.0.1.81:17678 S9=127.0.1.82:17679
rx $S8 SET s plain > /dev/null
rx $S8 SET big @big > /dev/null
rx $S8 HSET h f1 v1 f2 v2 > /dev/null
rx $S8 JSON.SET j '$' '{"a":1,"b":[1,2]}' > /dev/null
rx $S8 RL.HIT rl 600000 10 > /dev/null
rx $S8 RL.HIT rl 600000 10 > /dev/null
rx $S8 EVALSHA @save 1 L @now tokA 300000 > /dev/null
types() { for k in s big h j rl L; do printf '%s ' "$(rx $1 TYPE $k)"; done; }
TS=$(types $S8)
[ "$TS" = "string string hash ReJSON-RL ratelimit zset " ] && ok "the source holds a string, a 100 KB string, a hash, a document, a rate limit and a lock" || bad "source types: $TS"
env -u PERFCLI_AUTH "$DUMP" --from 127.0.1.81:17676 --collections 0 --threads 1 --zstd $Z --out "$D/dumpt" > "$LOG" 2>&1
grep -q "^perfdump: 6 records" "$LOG" && ok "dumped the six" || bad "typed dump: $(tr '\n' '|' < "$LOG")"
env -u PERFCLI_AUTH "$LOAD" --enable lo-enable-secret "$D/dumpt" --to 127.0.1.82:17677 --threads 1 > "$LOG" 2>&1
case "$(grep "^perfload: 6 records" "$LOG")" in
	*"stored 6, older 0, existing 0, expired 0, refused 0, bad 0"*) ok "loaded the six, the 100 KB value with them";;
	*) bad "typed load: $(tr '\n' '|' < "$LOG")";;
esac
TT=$(types $S9)
[ "$TT" = "$TS" ] && ok "every type came back: $TT" || bad "types after the load: $TT (the source: $TS)"
for c in "GET s" "GET big" "HGETALL h" "JSON.GET j"; do
	X=$(rx $S8 $c); Y=$(rx $S9 $c)
	[ "$X" = "$Y" ] && case "$X" in nil|-*) false;; esac && ok "$c answers as at the source: $Y" || bad "$c: $Y (the source: $X)"
done
case "$(rx $S9 GET big)" in "<100000 bytes "*) ok "the big value is 100,000 bytes";; *) bad "big: $(rx $S9 GET big)";; esac
[ "$(rx $S9 EVALSHA @exists 1 L @now tokA)" = 1 ] && ok "the lock is held by its token after the load" || bad "lock: $(rx $S9 EVALSHA @exists 1 L @now tokA)"
[ "$(rx $S9 RL.HIT rl 600000 10)" = "[3,1]" ] && ok "the rate limit counts on from the two hits it had" || bad "rate limit: $(rx $S9 RL.HIT rl 600000 10)"
# a format-1 dump: refused, with the reason, by perfload and by --inspect
mkdir "$D/dumpv1" && cp "$D/dumpt"/*.pcd* "$D/dumpv1/"
sed 's/"format_version":2,/"format_version":1,/' "$D/dumpt/manifest.json" > "$D/dumpv1/manifest.json"
env -u PERFCLI_AUTH "$LOAD" --enable lo-enable-secret "$D/dumpv1" --to 127.0.1.82:17677 --threads 1 --restart > "$LOG" 2>&1; rc=$?
"$DUMP" --inspect "$D/dumpv1" > "$D/inspv1.out" 2>&1; irc=$?
[ $rc -ne 0 ] && [ $irc -ne 0 ] && grep -q "is perfdump format 1; perfload [0-9.]* loads format 2 only" "$LOG" &&
	grep -q "is perfdump format 1; perfdump [0-9.]* reads format 2 only" "$D/inspv1.out" &&
	ok "a format-1 dump is refused by perfload and by --inspect, naming the format" ||
	bad "format 1: perfload rc=$rc $(tr '\n' '|' < "$LOG") / inspect rc=$irc $(tr '\n' '|' < "$D/inspv1.out")"

echo "loadtest: $pass passed, $fail failed"
[ $fail = 0 ]
