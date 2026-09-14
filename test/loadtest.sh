#!/bin/sh
# loadtest.sh - S113: the `restore` verb and perfload.  A three-node eager
# fleet is filled and dumped with perfdump; the file set is loaded into an
# EMPTY two-node SHARD fleet and into an empty two-node eager fleet, and
# every record must arrive with the value, the VERSION and the remaining
# TTL the dump held (absolute expiry preserved, not re-based).  Then the
# policies over a fleet holding a newer copy, a record that expired between
# the dump and the load, the resume file, a load into another collection,
# a load that sends everything to one node of the shard fleet (the daemon
# names what belongs elsewhere and the loader re-routes it), and the
# comparator (--via-set) as the fail-first control: plain SET assigns fresh
# versions, so the version assertion MUST fail against it.
BIN=${1:-./perfcached}
DUMP=${2:-./perfdump}
LOAD=${3:-./perfload}
D=$(mktemp -d /var/tmp/pclo.XXXXXX)
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; pkill -9 -f "[p]erfcached -f $D" 2>/dev/null; rm -rf "$D"' EXIT TERM INT
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
# drive <addr:port> <op> [args]
#   fill <n>        -> n records dk<i> (value v<i>+x*(i%50), ttl 300 on even i) + every 10th overwritten (w<i>)
#                      + dkshort (ttl 15); prints sets=<n>
#   dump <col>      -> one line per record "key value expiry ver", sorted; expiry = unix seconds
#                      the record's TTL points at when the dump was read (-1 = none), so two
#                      dumps taken at different times compare on the ABSOLUTE expiry
#   get <col> <key> -> "value ttl" or MISSING
#   set <col> <key> <value> -> OK
#   entries <col>   -> entries of <col>
#   buckets <col>   -> buckets of <col>
#   compare <src> <got> <tol> -> same= diff_val= diff_ver= exp_bad= missing= extra= dups=
#                      (exp_bad: absolute expiries further apart than <tol> seconds, or one side never)
drive() {
	python3 - "$@" <<'PYEOF'
import json, socket, sys, base64, time
op = sys.argv[2]
def conn():
    h, p = sys.argv[1].rsplit(":", 1)
    s = socket.create_connection((h, int(p)), timeout=20); return s.makefile("rwb")
rid = [0]
def call(f, m, **p):
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
    f = conn(); col = sys.argv[3]; cursor = 0; out = []
    while True:
        r = call(f, "dump", col=col, cursor=cursor, count=64)
        if "error" in r: print("ERROR %s" % json.dumps(r["error"])[:120]); sys.exit(0)
        r = r["result"]
        now = int(time.time())
        for rec in r["records"]:
            exp = now + rec["ttl"] if rec["ttl"] >= 0 else -1
            out.append("%s %s %s %s" % (val_of(rec, "k", "k_enc"), val_of(rec), exp, rec["ver"]))
        cursor = r["cursor"]
        if not r["more"]: break
    for l in sorted(out): print(l)
elif op == "get":
    f = conn(); r = call(f, "get", col=sys.argv[3], key=sys.argv[4])
    if "error" in r or r.get("result") is None or not r["result"].get("found", True) or "value" not in r["result"]: print("MISSING")
    else:
        res = r["result"]; print("%s %s" % (val_of(res, "value", "enc"), res.get("ttl", -1)))
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
mk 4 127.0.1.61 17661 shard 239.255.77.67:17167
mk 5 127.0.1.62 17662 shard 239.255.77.67:17167
start 4; start 5; ready 4 5
i=0; while [ $i -lt 300 ]; do [ "$(drive $A3 get dc dkshort)" = MISSING ] && break; sleep 0.1; i=$((i+1)); done
[ "$(drive $A3 get dc dkshort)" = MISSING ] && ok "dkshort expired at the source between the dump and the load" || bad "dkshort still at the source: $(drive $A3 get dc dkshort)"
sleep 2   # the daemon expires on whole ticks; the file holds the millisecond expiry, so give it the second
LOG="$D/load1.txt"
env -u PERFCLI_AUTH "$LOAD" "$D/dump" --to $B1 --threads 2 --batch 100 > "$LOG" 2>&1; rc=$?
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
env -u PERFCLI_AUTH "$LOAD" "$D/dump" --to $B1 --threads 2 --restart --no-settle > "$LOG" 2>&1
L=$(grep "^perfload: 2001 records" "$LOG")
case "$L" in *"stored 0, older 2000, existing 0, expired 1, refused 0"*) ok "newer: a re-load stores nothing, every record counted older (the same version is as old)";; *) bad "newer: $L";; esac
[ "$(drive $B1 get dc dk5)" = "newer5 -1" ] && ok "newer: the fleet's newer copy of dk5 stands" || bad "dk5 after newer: $(drive $B1 get dc dk5)"
env -u PERFCLI_AUTH "$LOAD" "$D/dump" --to $B1 --threads 2 --restart --no-settle --policy skip > "$LOG" 2>&1
L=$(grep "^perfload: 2001 records" "$LOG")
case "$L" in *"stored 0, older 0, existing 2000, expired 1"*) ok "skip: every existing key left alone and counted";; *) bad "skip: $L";; esac
env -u PERFCLI_AUTH "$LOAD" "$D/dump" --to $B1 --threads 2 --restart --no-settle --policy overwrite > "$LOG" 2>&1
L=$(grep "^perfload: 2001 records" "$LOG")
case "$L" in *"stored 2000, older 0, existing 0, expired 1"*) ok "overwrite: every record installed again under a fresh version";; *) bad "overwrite: $L";; esac
[ "$(drive $B1 get dc dk5)" = "v5xxxxx -1" ] && ok "overwrite: dk5 is the dumped value again" || bad "dk5 after overwrite: $(drive $B1 get dc dk5)"

# ---- the resume file: a finished load is skipped; one chunk taken off the file loads alone, without doubling anything
env -u PERFCLI_AUTH "$LOAD" "$D/dump" --to $B1 --threads 2 --no-settle > "$LOG" 2>&1
grep -q "^perfload: 0 records" "$LOG" && grep -q "skipped as already loaded" "$LOG" && ok "a finished load is skipped by perfload.done" || bad "resume skip: $(cat "$LOG" | tr '\n' '|')"
N=$(wc -l < "$D/dump/perfload.done")
head -n $((N - 1)) "$D/dump/perfload.done" > "$D/done.tmp" && mv "$D/done.tmp" "$D/dump/perfload.done"
LASTF=$(ls "$D/dump" | grep '\.pcd' | while read f; do grep -q "^$f " "$D/dump/perfload.done" || echo "$f"; done | head -1)
env -u PERFCLI_AUTH "$LOAD" "$D/dump" --to $B1 --threads 2 --no-settle > "$LOG" 2>&1
L=$(grep "^perfload: [0-9]* records" "$LOG")
case "$L" in "perfload: 0 records"*) bad "resume: nothing loaded: $L";; *"stored 0, older "*", existing 0, expired "*"resumed"*) ok "resume: only the chunk not marked done ($LASTF) is read again, and stores nothing twice";; *) bad "resume: $L";; esac
E=$(( $(drive $B1 entries dc) + $(drive $B2 entries dc) ))
[ "$E" = 2000 ] && ok "entries unchanged at 2000 after the resume" || bad "entries $E after resume"

# ---- into another collection, and the dry run
env -u PERFCLI_AUTH "$LOAD" "$D/dump" --to $B1 --threads 2 --restart --no-settle --collection-map dc=dl > "$LOG" 2>&1
{ drive $B1 dump dl; drive $B2 dump dl; } > "$D/gotl.txt"
C=$(drive x compare "$D/src_live.txt" "$D/gotl.txt" 2 | head -1)
[ "$C" = "same=2000 diff_val=0 diff_ver=0 exp_bad=0 missing=0 extra=0 dups=0" ] && ok "--collection-map dc=dl: the same records, versions and expiries in dl" || bad "dl holds: $C"
env -u PERFCLI_AUTH "$LOAD" "$D/dump" --to $B1 --threads 2 --restart --dry-run --collection-map dc=nope > "$LOG" 2>&1 && bad "a map onto a missing collection was accepted" || ok "a map onto a collection the target lacks is refused before anything loads"
env -u PERFCLI_AUTH "$LOAD" "$D/dump" --to $B1 --threads 2 --restart --dry-run > "$LOG" 2>&1
grep -q "^perfload: dry run: 2001 records" "$LOG" && ok "dry run counts 2001 records and loads nothing" || bad "dry run: $(cat "$LOG" | tr '\n' '|')"

# ---- everything to ONE node of the shard fleet: the daemon names what belongs elsewhere, the loader re-sends it
env -u PERFCLI_AUTH "$LOAD" "$D/dump" --to $B1 --threads 2 --restart --no-settle --no-route --policy overwrite > "$LOG" 2>&1
L=$(grep "^perfload: 2001 records" "$LOG")
RR=$(echo "$L" | sed -n 's/.*rerouted \([0-9]*\).*/\1/p')
case "$L" in *"stored 2000, older 0, existing 0, expired 1, refused 0, bad 0, rerouted "[1-9]*) ok "--no-route: $RR records came back named and were re-sent to their owner; 2000 stored, 0 refused";; *) bad "no-route: $L";; esac
{ drive $B1 dump dc; drive $B2 dump dc; } > "$D/got2.txt"
C=$(drive x compare "$D/src_live.txt" "$D/got2.txt" 2 | head -1)
case "$C" in "same=0 diff_val=0 diff_ver=2000 exp_bad=0 missing=0 extra=0 dups=0") ok "after the re-route every record sits on exactly one owner (fresh versions: overwrite)";; *) bad "after re-route: $C";; esac

# ---- the fail-first control: the same file set through plain SET (--via-set) gets fresh versions
env -u PERFCLI_AUTH "$LOAD" "$D/dump" --to $B1 --threads 2 --restart --no-settle --via-set --collection-map dc=dl > "$LOG" 2>&1
{ drive $B1 dump dl; drive $B2 dump dl; } > "$D/gotv.txt"
C=$(drive x compare "$D/src_live.txt" "$D/gotv.txt" 2 | head -1)
case "$C" in *"diff_ver=2000"*"missing=0 extra=0"*) ok "control: a loader on plain SET fails the version assertion on all 2000 records";; *) bad "control: $C $(grep -v '^perfload:' "$LOG" | head -2 | tr '\n' '|')";; esac

# ---- target 2: an EMPTY two-node EAGER fleet: files spread over both members, the push carries the rest
C1=127.0.1.71:17671 C2=127.0.1.72:17672
mk 6 127.0.1.71 17671 eager 239.255.77.68:17168
mk 7 127.0.1.72 17672 eager 239.255.77.68:17168
start 6; start 7; ready 6 7
env -u PERFCLI_AUTH "$LOAD" "$D/dump" --to $C1 --threads 2 --restart > "$LOG" 2>&1
L=$(grep "^perfload: 2001 records" "$LOG")
case "$L" in *"stored 2000, older 0, existing 0, expired 1, refused 0"*) ok "loaded into the eager fleet: 2000 stored";; *) bad "eager load: $L";; esac
S=$(grep "^perfload: peers settled" "$LOG")
case "$S" in *"=2000 "*"=2000") ok "the push carried every loaded record to the other member: $S";; *) bad "eager settle: $(cat "$LOG" | tr '\n' '|')";; esac
drive $C2 dump dc > "$D/gote.txt"
C=$(drive x compare "$D/src_live.txt" "$D/gote.txt" 2 | head -1)
[ "$C" = "same=2000 diff_val=0 diff_ver=0 exp_bad=0 missing=0 extra=0 dups=0" ] && ok "the copies on the member that took no file carry the dumped versions and expiries" || bad "eager member 2 holds: $C"

echo "loadtest: $pass passed, $fail failed"
[ $fail = 0 ]
