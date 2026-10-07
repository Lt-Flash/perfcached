#!/bin/sh
# jsonalloctest.sh - S283: the JSON verbs allocate nothing per request.
#
# Every JSON verb tokenizes its document, and until S283 the token array
# was malloc'd per call and freed (JSON.SET tokenized twice: the value,
# then the document).  One malloc/free pair per request is nothing to
# glibc's per-thread cache, and on musl's allocator it was the whole
# 50-130x JSON.SET collapse (DESIGN 12hd: ~2 page faults and 2-4 context
# switches per command).  The array is now per-thread scratch, grown to
# the largest document seen.
#
# 0.5.0 port: the same three allocations were still there on 0.4.6 - a
# copy of the document, a VAL_MAX result and the token array, on both
# doors (counted with this shim) - and the hash forward ops that share
# pc_json_rmw allocated their fragment too.  Every fragment is now
# borrowed from the thread's two buffers.
#
# test/allocshim.so counts every malloc/calloc/realloc in the daemon into
# a shared file.  One node, no WAL, both doors:
#   0. positive control: the daemon allocated at startup (the shim is in
#      effect - under a sanitizer or on another libc it is not: SKIP)
#   1. after a warm-up of every verb, 2,000 of each - JSON.SET (root and
#      path), JSON.GET, JSON.NUMINCRBY, JSON.ARRAPPEND, JSON.DEL on the
#      RESP door; jset, jget, jincr, jarrappend, jdel on the native door
#      - and the allocations per request, net of what the idle daemon
#      allocated over as long a window, are below 0.01 for each
# Deterministic: it counts calls, it does not time them - the assertion
# that guards musl runs on a glibc runner.
# Fail-first: the build before S283 allocates 1 (2 for JSON.SET and jset)
# per request; 0.4.6 allocates 3 for every JSON command on both doors.
# Usage: test/jsonalloctest.sh [./perfcached] [./allocshim.so]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
SHIM=${2:-./allocshim.so}
[ -f "$SHIM" ] || { echo "jsonalloctest: $SHIM is missing (make allocshim.so)"; exit 1; }
case "$SHIM" in /*) ;; *) SHIM="$PWD/$SHIM";; esac
if ldd "$BIN" 2>/dev/null | grep -qE 'libasan|libclang_rt\.asan'; then
	echo "  SKIP a sanitizer runtime answers malloc itself - nothing to count"
	echo "jsonalloctest: 0 passed, 0 failed, 1 skipped"; exit 0
fi
D=$(mktemp -d /var/tmp/pcjal.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=17702 RPORT=17703
if ss -ltn 2>/dev/null | grep -qE ":1770[23][[:space:]]"; then
	echo "jsonalloctest: port $PORT/$RPORT already bound" >&2; exit 1
fi
cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = ja-client
[listen]
tcp = 127.0.0.1:$PORT
resp = 127.0.0.1:$RPORT
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 600 "$D/n.conf"
PC_ALLOC_COUNT="$D/count" LD_PRELOAD="$SHIM" "$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
PID=$!
i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/n.log"; do sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { echo "jsonalloctest: the node did not start: $(tail -3 "$D/n.log")"; exit 1; }

python3 - "$PORT" "$RPORT" "$D/count" > "$D/run.out" 2>&1 <<'PY'
import json, pcnative, socket, struct, sys, time
port, rport, cf = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
def count():
    with open(cf, "rb") as f: return struct.unpack("<Q", f.read(8))[0]
def out(k, v): print("%s=%s" % (k, v)); sys.stdout.flush()
out("STARTUP", count())
# RESP: pipelined in batches of 200, every reply read
rs = socket.create_connection(("127.0.0.1", rport), 10); rf = rs.makefile("rb")
def enc(args):
    return b"*%d\r\n" % len(args) + b"".join(b"$%d\r\n%s\r\n" % (len(a), a) for a in (x.encode() for x in args))
def rreply():
    l = rf.readline()
    if l[:1] == b"$":
        n = int(l[1:])
        if n >= 0: rf.read(n + 2)
    elif l[:1] == b"*":
        for _ in range(int(l[1:])): rreply()
    return l
def resp(cmds):
    for b in range(0, len(cmds), 200):
        rs.sendall(b"".join(enc(c) for c in cmds[b:b + 200]))
        for _ in cmds[b:b + 200]:
            l = rreply()
            if l[:1] == b"-": raise SystemExit("RESP error: %r" % l)
# the native door, through pcnative (binary CMD frames), pipelined
js = socket.create_connection(("127.0.0.1", port), 10); jf = pcnative.wrap(js)
def rpc(reqs):
    for b in range(0, len(reqs), 200):
        jf.write("".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": m, "params": p}) + "\n"
            for i, (m, p) in enumerate(reqs[b:b + 200])).encode())
        jf.flush()
        for _ in reqs[b:b + 200]:
            r = json.loads(jf.readline())
            if "error" in r: raise SystemExit("native error: %r" % r)
N = 2000
doc = json.dumps({"rr": "x" * 200, "n": 1, "arr": [1, 2], "gone": 1})
R = {   # RESP door: key prefix r
    "JSON.SET root": lambda k: ["JSON.SET", k, "$", doc],
    "JSON.SET path": lambda k: ["JSON.SET", k, "$.rr", '"y"'],
    "JSON.GET": lambda k: ["JSON.GET", k, "$.n"],
    "JSON.NUMINCRBY": lambda k: ["JSON.NUMINCRBY", k, "$.n", "1"],
    "JSON.ARRAPPEND": lambda k: ["JSON.ARRAPPEND", k, "$.arr", "3"],
    "JSON.DEL path": lambda k: ["JSON.DEL", k, "$.gone"],
}
J = {   # native door: key prefix j
    "jset root": lambda k: ("jset", {"col": "0", "key": k, "val": json.loads(doc)}),
    "jset path": lambda k: ("jset", {"col": "0", "key": k, "path": "$.rr", "val": "y"}),
    "jget": lambda k: ("jget", {"col": "0", "key": k, "path": "$.n"}),
    "jincr": lambda k: ("jincr", {"col": "0", "key": k, "path": "$.n", "by": 1}),
    "jarrappend": lambda k: ("jarrappend", {"col": "0", "key": k, "path": "$.arr", "val": 3}),
    "jdel path": lambda k: ("jdel", {"col": "0", "key": k, "path": "$.gone"}),
}
def batch(name, keys):
    if name in R: resp([R[name]("r%d" % i) for i in keys])
    else: rpc([J[name]("j%d" % i) for i in keys])
# warm-up: every verb on keys the measured pass never touches, so each
# grow-once buffer (scratch, reply, connection) reaches its size first;
# then the documents the measured pass edits (DEL last: it removes $.gone)
for name in list(R) + list(J):
    batch(name, range(N, N + 200))
batch("JSON.SET root", range(N)); batch("jset root", range(N))
# the idle daemon's own allocations, per second, subtracted below
c0 = count(); t0 = time.time(); time.sleep(1.0); idle = (count() - c0) / (time.time() - t0)
out("IDLE_PER_S", "%.1f" % idle)
for name in list(R) + list(J):
    c0 = count(); t0 = time.time()
    batch(name, range(N))
    dt = time.time() - t0; d = count() - c0
    out("CASE", "%s|%d|%.3f|%.4f" % (name, d, dt, max(0.0, d - idle * dt) / N))
PY
[ -s "$D/run.out" ] && grep -q '^STARTUP=' "$D/run.out" || { echo "jsonalloctest: the driver failed: $(tail -3 "$D/run.out")"; exit 1; }
grep -q '^CASE=' "$D/run.out" || { echo "jsonalloctest: the driver failed: $(tail -3 "$D/run.out")"; exit 1; }
ST=$(sed -n 's/^STARTUP=//p' "$D/run.out")
if [ "${ST:-0}" -eq 0 ] 2>/dev/null; then
	echo "  SKIP the shim counted nothing at startup - it is not in effect here (not glibc?)"
	echo "jsonalloctest: 0 passed, 0 failed, 1 skipped"; exit 0
fi
ok "0. the shim is in effect: $ST allocation(s) at startup; idle daemon $(sed -n 's/^IDLE_PER_S=//p' "$D/run.out")/s"
sed -n 's/^CASE=//p' "$D/run.out" | while IFS='|' read -r name raw secs per; do
	if python3 -c "import sys; sys.exit(0 if float('$per') < 0.01 else 1)"; then
		echo "  ok   1. $name: $per allocation(s) per request ($raw in ${secs} s, 2000 requests)"
	else
		echo "  FAIL 1. $name: $per allocation(s) per request ($raw in ${secs} s, 2000 requests)"
	fi
done > "$D/cases.out"
cat "$D/cases.out"
pass=$((pass + $(grep -c '^  ok' "$D/cases.out"))); fail=$((fail + $(grep -c '^  FAIL' "$D/cases.out")))
echo "jsonalloctest: $pass passed, $fail failed"
[ $fail -eq 0 ]
