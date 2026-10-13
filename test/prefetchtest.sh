#!/bin/sh
# prefetchtest.sh - S232: the RESP lookahead never changes an answer.
#
# With a table past [daemon] prefetch_min_keys, the RESP drain parses up to
# eight frames ahead, prefetches their buckets and records, and hands the
# frame about to run the hash it computed while parsing ahead ("hash once" -
# pcache_key_hash takes it when the key pointer and length match).  Every
# suite in make check uses a keyspace under the 65,536-key default, so none
# of that runs there.  This forces it on (prefetch_min_keys = 1) and checks
# every reply of a long pipelined mix against a model:
#   - 60,000 commands in pipelines of 2,000 over 1,000 overlapping keys:
#     SET, GET, DEL, EXISTS, INCR, MGET (keys ahead of and behind the one
#     that runs), EXPIRE, PING - keyed, multi-key and keyless frames mixed,
#     so a hash handed to the wrong frame would answer for the wrong key
#   - every reply equal to the model's
# Fail-first: a build whose hint hands the NEXT frame's hash fails at once
# (checked by mutation when this was written).
# Usage: test/prefetchtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcprefetch.XXXXXX); P1=
trap '[ -n "$P1" ] && kill -9 "$P1" 2>/dev/null; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

cat > "$D/n.conf" <<CONF
[daemon]
workers = 2
prefetch_min_keys = 1
[memory]
arena_mb = 64
[secrets]
client = prefetch-client-secret
[listen]
resp = 127.0.0.1:17594
plaintext = loopback
[collection 0]
buckets_log2 = 12
autoscale = off
CONF
chmod 600 "$D/n.conf"
"$BIN" -f "$D/n.conf" -D > "$D/n.log" 2>&1 & P1=$!
i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break; sleep 0.1; i=$((i+1)); done
if ! grep -q "perfcached ready" "$D/n.log"; then
	bad "the daemon did not start: $(tail -1 "$D/n.log")"
	echo "prefetchtest: $pass passed, $fail failed"; exit 1
fi

timeout 120 python3 - > "$D/res" 2> "$D/py.err" <<'PY'
import random, socket
rnd = random.Random(232)
s = socket.create_connection(("127.0.0.1", 17594), timeout=30); f = s.makefile("rb")
def enc(*a):
    return b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a)
def reply():
    l = f.readline()
    t = l[:1]
    if t == b"$":
        n = int(l[1:])
        return None if n < 0 else f.read(n + 2)[:-2]
    if t == b"*":
        return [reply() for _ in range(int(l[1:]))]
    return l.strip()
model = {}
def expect(cmd):
    op = cmd[0]
    if op == b"SET":
        model[cmd[1]] = cmd[2]; return b"+OK"
    if op == b"GET":
        return model.get(cmd[1])
    if op == b"DEL":
        return b":%d" % (1 if model.pop(cmd[1], None) is not None else 0)
    if op == b"EXISTS":
        return b":%d" % (1 if cmd[1] in model else 0)
    if op == b"INCR":
        v = int(model.get(cmd[1], b"0")) + 1; model[cmd[1]] = b"%d" % v; return b":%d" % v
    if op == b"MGET":
        return [model.get(k) for k in cmd[1:]]
    if op == b"EXPIRE":
        return b":%d" % (1 if cmd[1] in model else 0)
    if op == b"PING":
        return b"+PONG"
key = lambda: b"key:%04d" % rnd.randrange(1000)
ctr = lambda: b"ctr:%04d" % rnd.randrange(100)
def cmd():
    r = rnd.random()
    if r < 0.30: return (b"SET", key(), b"v%d" % rnd.randrange(10**9))
    if r < 0.60: return (b"GET", key())
    if r < 0.66: return (b"DEL", key())
    if r < 0.72: return (b"EXISTS", key())
    if r < 0.80: return (b"INCR", ctr())
    if r < 0.88: return (b"MGET", key(), key(), key())
    if r < 0.94: return (b"EXPIRE", key(), b"3600")
    return (b"PING",)
# warm: the table past the (forced) gate, and every key known to the model
s.sendall(b"".join(enc(b"SET", b"key:%04d" % i, b"w%d" % i) for i in range(1000)))
for i in range(1000):
    reply(); model[b"key:%04d" % i] = b"w%d" % i
bad = total = 0; first = None
for batch in range(30):
    cmds = [cmd() for _ in range(2000)]
    s.sendall(b"".join(enc(*c) for c in cmds))
    for c in cmds:
        got, want = reply(), expect(c)
        total += 1
        if got != want:
            bad += 1
            if first is None: first = (c, want, got)
print("TOTAL %d BAD %d" % (total, bad))
if first: print("FIRST %r want %r got %r" % first)
PY
T=$(sed -n 's/^TOTAL \([0-9]*\) .*/\1/p' "$D/res"); B=$(sed -n 's/.* BAD \([0-9]*\)$/\1/p' "$D/res")
[ "$T" = 60000 ] && ok "60,000 pipelined commands (SET/GET/DEL/EXISTS/INCR/MGET/EXPIRE/PING) with the lookahead forced on" \
	|| bad "the driver ran $T of 60,000"
[ "$B" = 0 ] && ok "every reply matched the model" || bad "$B replies differed: $(sed -n 's/^FIRST //p' "$D/res")"
[ -s "$D/py.err" ] && { bad "the python driver raised:"; tail -3 "$D/py.err"; }
kill "$P1" 2>/dev/null; wait "$P1" 2>/dev/null; P1=
echo "prefetchtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
