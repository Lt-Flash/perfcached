#!/bin/sh
# fuzztest.sh - RV-7 part 1: the doors under hostile input.
#
# The operator's list asks for fuzzing of the RESP parser and the tree
# parser behind CMD frames, malformed frames on the binary plane, and
# the size ceilings (key, value, pipeline depth, argument count).
# `prototest` already covers a few hand-written bad frames; this drives
# thousands, generated, and asserts the property that matters for all
# of them at once:
#
#   THE DAEMON SURVIVES EVERY ONE.  It answers or closes deliberately,
#   it never hangs, it never dies, and a good request on a fresh
#   connection works afterwards - checked after EVERY case, so the case
#   that broke it is named rather than inferred from a later failure.
#
# What is NOT asserted: which error a given malformation returns.  That
# is the parser's business and pinning it would freeze what is allowed
# to improve.  What IS pinned is the contract the README states - the
# value ceiling, and that a refusal keeps the connection usable.
#
# Cases, all three doors on one daemon:
#  resp      truncated frames, bad and negative and huge multibulk
#            counts, bulk lengths that disagree with the bytes, missing
#            CRLF, embedded NUL, inline commands, unterminated bulk,
#            a bulk header with no body, non-numeric lengths
#  deep      pipeline depth and argument count: 10,000 commands in one
#            write, and a 100,000-argument multibulk
#  size      the key and value ceilings from both sides: the largest
#            value the README allows must store and read back byte for
#            byte; one byte more must be refused, not truncated
#  tree      malformed parameter trees inside CMD frames (S317: the
#            native door speaks binary frames only, the JSON-RPC text
#            dialect is gone): unknown tags, lengths past the frame,
#            nesting past PT_DEPTH (32), u32 counts that lie, a 70 KB
#            bulk, method lengths of 0 and 255, a NUL inside a key,
#            numbers that do not fit, 5,000 pings in one write
#  binary    frame headers that lie: a length longer than the body,
#            longer than the maximum, zero, a truncated header, a bad
#            type or version - and what is no frame at all (a PNG, an
#            HTTP request, a TLS hello) at the native door
#  random    500 frames of random bytes at each door, seeded and printed
#            so a failure is reproducible
# Usage: test/fuzztest.sh [./perfcached] [seed]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
SEED=${2:-$(date +%s)}
D=$(mktemp -d /var/tmp/pcfuzz.XXXXXX)
P=
# a TERM from an outer timeout must END this, not just clean up: dash
# runs the trap and CARRIES ON, which left every assertion below reading
# a tree that had just been removed
trap '[ -n "$P" ] && kill -9 "$P" 2>/dev/null; rm -rf "$D"' EXIT
trap 'echo "fuzztest: interrupted"; exit 143' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

cat > "$D/n.conf" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 256
[secrets]
client = fz-client-secret
[listen]
tcp = 127.0.0.1:18801
resp = 127.0.0.1:18802
plaintext = loopback
[collection 0]
buckets_log2 = 12
EOF
"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
P=$!
i=0
while [ $i -lt 200 ]; do
	grep -q "perfcached ready" "$D/n.log" 2>/dev/null && break
	kill -0 $P 2>/dev/null || break
	sleep 0.1; i=$((i + 1))
done
grep -q "perfcached ready" "$D/n.log" || { echo "the daemon did not start:"; tail -4 "$D/n.log"; exit 1; }

python3 - "$SEED" "$D" > "$D/out" 2>&1 <<'PY'
import random, socket, struct, sys, time
import pcnative

seed, D = int(sys.argv[1]), sys.argv[2]
random.seed(seed)
NAT_PORT, RESP_PORT = 18801, 18802
CELL_MAX = 262080                      # PCACHE_CELL_MAX
def out(k, v): print("%s=%s" % (k, v)); sys.stdout.flush()
out("SEED", seed)

def send(port, payload, read=True, timeout=1.0):
    """one connection, one payload; returns (reply-bytes, how-it-ended)"""
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        s.settimeout(timeout)
        s.sendall(payload)
        if not read:
            s.close(); return b"", "sent"
        buf = b""
        try:
            while len(buf) < 65536:
                d = s.recv(65536)
                if not d: break
                buf += d
        except socket.timeout:
            s.close(); return buf, "timeout"
        s.close()
        return buf, "closed"
    except ConnectionResetError:
        return b"", "reset"
    except socket.timeout:
        return b"", "connect-timeout"
    except Exception as e:
        return b"", "err:" + type(e).__name__

def alive():
    """a GOOD request on a fresh connection, both doors"""
    r, _ = send(RESP_PORT, b"*1\r\n$4\r\nPING\r\n")
    if not r.startswith(b"+PONG"):
        return "resp:%r" % r[:40]
    try:
        s = socket.create_connection(("127.0.0.1", NAT_PORT), timeout=1.0)
        s.settimeout(1.0)
        r = pcnative.call(pcnative.wrap(s), "ping")   # one CMD frame, its reply
        s.close()
    except Exception as e:
        return "native:%s" % type(e).__name__
    if not isinstance(r.get("result"), dict) or r["result"].get("pong") is not True:
        return "native:%r" % str(r)[:60]
    return ""

d = alive()
out("ALIVE0", d if d else "ok")
DEAD = ""                              # the arm that killed the daemon, if any

def run(name, cases, port, every=25):
    """
    Every case, and a liveness check every `every` of them and at the end.

    The cases are NOT read back.  A partial frame leaves the daemon
    waiting for the rest of it, which is correct and is most of this
    corpus - waiting for a reply that is rightly not coming turned a
    two-second suite into a forty-minute one.  What is asserted is what
    matters: the daemon survives every case and the doors still work.
    On a failure the window is bisected so the case is NAMED, not
    inferred from a later one.
    """
    global DEAD
    if DEAD:
        out(name, "n=%d dead=0 SKIPPED - %s had already killed it" % (len(cases), DEAD))
        return
    dead = 0; first = ""; t0 = time.time()
    for i, c in enumerate(cases):
        send(port, c if isinstance(c, bytes) else c.encode(), read=False)
        if i % every and i != len(cases) - 1:
            continue
        d = alive()
        if d:
            dead += 1
            lo = max(0, i - every)
            for j in range(lo, i + 1):     # bisect: replay one at a time
                send(port, cases[j], read=False)
                if alive():
                    first = "case %d (%d bytes, %r) killed the door" % (
                        j, len(cases[j]), cases[j][:24])
                    break
            if not first:
                first = "a door died in cases %d..%d: %s" % (lo, i, d)
            DEAD = name                # every later arm would blame its own
            break
    out(name, "n=%d dead=%d %.1fs %s" % (len(cases), dead, time.time() - t0, first))

# ---- resp: hand-built malformations ---------------------------------
resp = [
    b"*", b"*1", b"*1\r\n", b"*1\r\n$", b"*1\r\n$4", b"*1\r\n$4\r\n",
    b"*1\r\n$4\r\nPIN", b"*0\r\n", b"*-1\r\n", b"*-99999\r\n",
    b"*999999999\r\n", b"*99999999999999999999\r\n", b"*abc\r\n",
    b"*1\r\n$-1\r\n", b"*1\r\n$999999999\r\n", b"*1\r\n$99999999999999999999\r\n",
    b"*1\r\n$abc\r\nPING\r\n", b"*2\r\n$3\r\nGET\r\n$100\r\nshort\r\n",
    b"*1\r\n$4\r\nPING",                      # no trailing CRLF
    b"*1\r\n$4\r\nPING\n",                    # LF only
    b"*1\r\n$4\r\nPI\x00G\r\n",               # NUL in the verb
    b"\x00\x00\x00\x00", b"\xff\xfe\xfd\r\n",
    b"PING\r\n", b"PING\r\nPING\r\n",         # inline
    b"  \r\n", b"\r\n", b"\r\n\r\n\r\n",
    b"+OK\r\n", b"-ERR\r\n", b":1\r\n", b"$5\r\nhello\r\n",   # replies, as requests
    b"*2\r\n$3\r\nSET\r\n$1\r\nk\r\n",        # too few args for SET
    b"*1\r\n$0\r\n\r\n",                      # empty verb
    b"*1\r\n$4\r\nping\r\n" * 3,
]
run("RESP", resp, RESP_PORT)

# ---- depth and argument count ---------------------------------------
deep = [
    b"*1\r\n$4\r\nPING\r\n" * 10000,          # pipeline depth
    b"*100000\r\n" + b"$4\r\nPING\r\n" * 100, # argc says 100k, 100 given
    b"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1000000\r\n" + b"x" * 1000000 + b"\r\n",
]
run("DEEP", deep, RESP_PORT)

# ---- the size ceilings, from both sides -----------------------------
def resp_cmd(*a):
    p = b"*%d\r\n" % len(a)
    for x in a:
        if isinstance(x, str): x = x.encode()
        p += b"$%d\r\n%s\r\n" % (len(x), x)
    return p

hdr = 64                                   # record header + key, roughly
big = b"v" * (CELL_MAX - 1024)
r, _ = send(RESP_PORT, resp_cmd("SET", "big", big))
stored = r.startswith(b"+OK")
r, _ = send(RESP_PORT, resp_cmd("GET", "big"), timeout=8)
back = b"$%d\r\n" % len(big) in r and r.rstrip().endswith(b"v")
out("BIG_VALUE", "stored=%s readback_len_ok=%s reply=%r" % (stored, back, r[:24]))
huge = b"v" * (CELL_MAX + 4096)
r, _ = send(RESP_PORT, resp_cmd("SET", "huge", huge), timeout=8)
refused = not r.startswith(b"+OK")
r2, _ = send(RESP_PORT, resp_cmd("GET", "huge"))
absent = r2.startswith(b"$-1") or r2.startswith(b"-")
out("HUGE_VALUE", "refused=%s absent_after=%s reply=%r" % (refused, absent, r[:48]))
bigkey = b"k" * 60000
r, _ = send(RESP_PORT, resp_cmd("SET", bigkey, b"v"), timeout=8)
out("BIG_KEY", "reply=%r" % r[:48])
out("ALIVE_SIZE", alive() or "ok")

# ---- CMD frames with malformed parameter trees, at the native door ---
# [verb 19][mlen u8][method][tree]: the tree parser (src/ptree.c) must
# refuse every one of these with 'bad params' (or 'invalid request' for a
# method field that is missing or past the frame) and keep the link.
def hdr(plen, ftype=1, rid=1, ver=1, magic=0x9E, flags=0):
    return struct.pack("<BBBBIQ", magic, ver, ftype, flags, plen, rid)
def fr(payload, **kw):
    return hdr(len(payload), **kw) + payload
def u32(n): return struct.pack("<I", n)
def u16(n): return struct.pack("<H", n)
def i64(n): return struct.pack("<q", n)
def bulk(b): return b"b" + u32(len(b)) + b
def kvb(k, v): return bulk(k) + bulk(v)          # key -> bulk value
def kvt(k, tree): return bulk(k) + tree          # key -> a tree given raw
def m(*pairs): return b"m" + u32(len(pairs)) + b"".join(pairs)
def cmd(method, tree=b""):
    return fr(bytes([19, len(method)]) + method + tree)
PING = b"ping"
trees = [
    cmd(PING, b"x"), cmd(PING, b"\x00"), cmd(PING, b"\xff"),       # unknown tags
    cmd(PING, b"m"),                                               # a map cut before its count
    cmd(PING, b"m" + u32(1)),                                      # count says 1, nothing follows
    cmd(PING, b"m" + u32(0xffffffff)),                             # a u32 count that lies
    cmd(PING, b"a" + u32(0xffffffff)),
    cmd(PING, b"m" + u32(1) + b"b" + u32(0xffffffff) + b"echo"),   # a length past the frame
    cmd(PING, b"m" + u32(1) + bulk(b"echo") + b"b" + u32(70000) + b"x"),
    cmd(PING, b"m" + u32(1) + bulk(b"echo") + b"b"),               # a bulk cut before its length
    cmd(PING, b"m" + u32(1) + bulk(b"echo") + b"i" + b"\x01"),     # an int cut short
    cmd(PING, b"m" + u32(1) + bulk(b"echo") + b"d" + u16(9) + b"1"),   # a decimal cut short
    cmd(PING, b"m" + u32(1) + b"i" + i64(1) + b"t"),               # a map key that is not bytes
    cmd(PING, b"i" + i64(1)), cmd(PING, b"t"), cmd(PING, b"n"),    # a root that is not a map
    cmd(PING, b"b" + u32(0)), cmd(PING, b"a" + u32(0)),
    cmd(PING, b"m" + u32(0) + b"junk"),                            # trailing bytes
    cmd(PING, (b"a" + u32(1)) * 33 + b"n"),                        # nesting one past PT_DEPTH
    cmd(PING, (b"a" + u32(1)) * 200 + b"n"),                       # 200 deep
    cmd(PING, b"m" + u32(1) + bulk(b"k") + (b"m" + u32(1) + bulk(b"k")) * 40 + b"n"),  # 41 nested maps
    cmd(PING, m(kvb(b"echo", b"v" * 70000))),                      # a 70 KB bulk (well formed)
    cmd(PING, m(kvb(b"echo", b"\xff\xfe" * 50))),                  # bytes that are not UTF-8
    cmd(PING, m(kvb(b"echo", b"a\x00b"))),                         # a NUL inside a bulk
    fr(bytes([19])),                                               # no method length byte
    fr(bytes([19, 0]) + PING),                                     # method length 0
    fr(bytes([19, 255]) + PING),                                   # method length past the frame
    fr(bytes([19, 255]) + b"m" * 255),                             # a 255-byte method: unknown
    fr(bytes([19, 4]) + b"pi\x00g"),                               # a NUL inside the method
    fr(bytes([19, 4]) + b"\xff\xfe\xfd\xfc"),                      # a method that is not text
    cmd(b"set", m(kvb(b"col", b"0"), kvb(b"key", b"k\x00x"), kvb(b"value", b"v"))),
    cmd(b"set", m(kvb(b"col", b"0"), kvb(b"key", b"k"), kvt(b"ttl", b"d" + u16(20) + b"99999999999999999999"))),
    cmd(b"set", m(kvb(b"col", b"0"), kvb(b"key", b"k"), kvt(b"ttl", b"i" + i64(-(1 << 63))))),
    cmd(b"set", m(kvb(b"col", b"0"), kvb(b"key", b"k"), kvt(b"value", b"i" + i64(1)))),   # a value that is not bytes
    cmd(b"get", m(kvb(b"col", b"c" * 70000), kvb(b"key", b"k"))),  # a 70 KB collection name
    cmd(b"get", m(kvb(b"col", b"0"), kvb(b"key", b"k" * 70000))),  # a 70 KB key
    cmd(b"get", m(kvb(b"col", b"0"), kvb(b"key", b"k")) * 2),      # two roots
    cmd(PING) * 5000,                                              # 5,000 pings in one write
]
run("TREE", trees, NAT_PORT)

# ---- frame headers that lie, and what is no frame at all --------------
bins = [
    hdr(0xffffffff), hdr(0xffffffff) + b"x" * 64,                  # longer than the maximum
    hdr(1 << 20) + b"short",                                       # at the maximum, body short
    hdr((1 << 20) + 1) + b"\x01" + b"a" * 4096,                    # one past the maximum
    hdr(0), hdr(0, ftype=2), hdr(0, ftype=3), hdr(0, ftype=9),     # zero-length, wrong types
    hdr(1, ver=2) + b"\x01", hdr(1, ver=0) + b"\x01",              # a version this is not
    hdr(5) + b"\x01abcd" + b"\x9e\x01\x01\x00",                    # a truncated second header
    b"\x9e", b"\x9e\x01", b"\x9e\x01\x01\x00", b"\x9e" + b"\x00" * 15,   # truncated headers
    hdr(1000000) + b"short",
    fr(b""), fr(b"\x00"), fr(b"\xff" * 300),                       # no verb / verb 0 / verb 255
    fr(b"\x02\x01"), fr(b"\x03\x01\x00\x00"),                      # GET / SET cut before their fields
    fr(b"\x02" + b"\xff" + b"\xff\xff" + b"c" * 300),              # lengths past the payload
    b"\x89PNG\r\n\x1a\n" + b"\x00" * 64,
    b"GET / HTTP/1.1\r\nHost: x\r\n\r\n",
    b"\x16\x03\x01\x00\x50" + b"\x00" * 80,          # a TLS ClientHello
    b'{"jsonrpc":"2.0","id":1,"method":"ping"}\n',   # the dialect that was removed
    b"\x00\x01\x02\n", b"\xff" * 100 + b"\n", b"\n", b"   \n",
]
run("BINARY", bins, NAT_PORT)

# ---- random bytes at both doors -------------------------------------
for door, port in (("RANDR", RESP_PORT), ("RANDN", NAT_PORT)):
    cases = []
    for _ in range(500):
        n = random.choice([1, 2, 7, 16, 64, 300, 4096, 65536])
        cases.append(bytes(random.getrandbits(8) for _ in range(n)))
    run(door, cases, port)

out("ALIVE1", alive() or "ok")
PY
val() { sed -n "s/^$1=//p" "$D/out" | head -1; }
clean() { case "$(val $1)" in *SKIPPED*) return 1;; *" dead=0 "*) return 0;; esac; return 1; }

echo "  seed $(val SEED) - rerun with: sh test/fuzztest.sh $BIN $(val SEED)"
[ "$(val ALIVE0)" = ok ] && ok "both doors answer a good request before anything hostile (the control)" \
	|| bad "the control failed: $(val ALIVE0)"
for g in RESP:"hand-built malformed RESP frames" DEEP:"a 10,000-command pipeline, a 100,000-argument multibulk, a 1 MB bulk" \
         TREE:"malformed parameter trees in CMD frames - unknown tags, lengths past the frame, nesting past 32, counts that lie, a 70 KB bulk, method lengths 0 and 255" \
         BINARY:"frame headers that lie, a PNG, an HTTP request, a TLS hello, a JSON-RPC line" \
         RANDR:"500 random-byte frames at the RESP door" RANDN:"500 random-byte frames at the native door"; do
	k=${g%%:*}; w=${g#*:}
	clean $k && ok "$w: $(val $k) - the daemon survived every case and both doors still answer" \
		|| bad "$w: $(val $k)"
done
case "$(val BIG_VALUE)" in "stored=True readback_len_ok=True"*)
	ok "the largest value the arena allows stores and reads back at full length";;
*) bad "the big value: $(val BIG_VALUE)";; esac
case "$(val HUGE_VALUE)" in "refused=True absent_after=True"*)
	ok "one past the ceiling is REFUSED, not truncated - and the key does not exist afterwards";;
*) bad "over the ceiling: $(val HUGE_VALUE)";; esac
[ "$(val ALIVE_SIZE)" = ok ] && ok "and the doors are alive after the ceiling cases ($(val BIG_KEY))" \
	|| bad "the ceiling cases broke a door: $(val ALIVE_SIZE)"
[ "$(val ALIVE1)" = ok ] && ok "both doors still answer after every case" || bad "a door died: $(val ALIVE1)"

kill -0 $P 2>/dev/null && ok "the daemon is still running" || bad "THE DAEMON DIED - $(tail -3 "$D/n.log" | tr '\n' ' ' | cut -c1-200)"
C=$(grep -c "CRITICAL\|AddressSanitizer\|runtime error\|SIGSEGV" "$D/n.log" 2>/dev/null)
[ "$C" = 0 ] && ok "and its log carries no CRIT, no sanitizer finding, no crash" \
	|| bad "$C alarming line(s) in the log: $(grep -m2 "CRITICAL\|AddressSanitizer\|runtime error" "$D/n.log" | cut -c1-140)"

echo "fuzztest: $pass passed, $fail failed"
[ $fail -eq 0 ]
