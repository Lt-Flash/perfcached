#!/bin/sh
# prototest.sh — S7 verification: the native door end to end over a live
# daemon, one protocol (S317: the JSON-RPC text dialect is gone).
# CMD frames: ping round-trip, pipelined replies matched by id, raw bytes
# (NUL, 0xFF, quotes, control bytes) ride a bulk untouched, error frames
# that keep the connection usable (unknown method, malformed tree, missing
# method), the first-byte rule (a letter is RESP; anything that is neither
# a frame nor RESP gets the one-line refusal and the close), oversize
# frame teardown.  Fixed-layout verbs: framed ping echo (NULs included),
# pipelining, unknown-verb error flag.  Usage: test/prototest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH

BIN=${1:-./perfcached}
D=$(mktemp -d)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT

cat > "$D/s7.conf" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = smoke-client-secret
cluster = smoke-cluster-secret
[listen]
tcp = 127.0.0.1:16479
plaintext = loopback
[collection th]
buckets_log2 = 12
EOF

"$BIN" -f "$D/s7.conf" > "$D/log" 2>&1 &
PID=$!
i=0
while [ $i -lt 50 ]; do
	grep -q "perfcached ready" "$D/log" && break
	kill -0 $PID 2>/dev/null || { echo "FAIL: daemon died"; cat "$D/log"; exit 1; }
	sleep 0.1; i=$((i+1))
done

python3 - <<'EOF'
import socket, struct, sys
import pcnative

ADDR = ("127.0.0.1", 16479)
GONE = b"perfcached: this door speaks binary frames or RESP"
fails = []
total = 0

def conn():
    s = socket.create_connection(ADDR, timeout=3)
    return s

def check(name, cond):
    global total
    total += 1
    if cond:
        print("ok:", name)
    else:
        fails.append(name)
        print("FAIL:", name)

def frame(ftype, fid, payload, flags=0):
    return struct.pack("<BBBBIQ", 0x9E, 1, ftype, flags, len(payload), fid) + payload

def read_frame(s):
    hdr = b""
    while len(hdr) < 16:
        d = s.recv(16 - len(hdr))
        if not d:
            return None
        hdr += d
    magic, ver, ftype, flags, plen, fid = struct.unpack("<BBBBIQ", hdr)
    pl = b""
    while len(pl) < plen:
        d = s.recv(plen - len(pl))
        if not d:
            return None
        pl += d
    return ftype, flags, fid, pl

def reply(s):
    """one CMD reply: (id, error text or None, result tree)"""
    r = read_frame(s)
    if r is None:
        return None
    ftype, flags, fid, pl = r
    if ftype != 2:
        return (fid, "frame type %d" % ftype, None)
    if flags & 1:
        return (fid, pl.decode("utf-8", "replace"), None)
    return (fid, None, pcnative.decode(pl)[0] if pl else None)

def replies(s, n):
    out = []
    while len(out) < n:
        r = reply(s)
        if r is None:
            break
        out.append(r)
    return out

def read_all(s):
    buf = b""
    try:
        while True:
            d = s.recv(4096)
            if not d:
                break
            buf += d
    except (ConnectionResetError, socket.timeout):
        pass
    return buf

# a CMD payload by hand: [verb 19][mlen u8][method][tree]
def u32(n): return struct.pack("<I", n)
def cmdpay(method, tree=b""):
    return bytes([19, len(method)]) + method + tree
def bulk(b): return b"b" + u32(len(b)) + b

# 1. CMD ping round-trip
s = conn()
s.sendall(pcnative.frame(1, "ping"))
fid, err, res = reply(s)
check("cmd ping", fid == 1 and err is None and res["pong"] is True)

# 2. pipelining: three requests in one write, three matched replies
s.sendall(pcnative.frame(7, "ping") + pcnative.frame(8, "ping") +
          pcnative.frame(9, "ping"))
rs = replies(s, 3)
check("cmd pipelining ids", sorted(x[0] for x in rs) == [7, 8, 9] and
      all(x[1] is None and x[2]["pong"] is True for x in rs))

# 3. plain echo comes back as the bytes sent, nothing beside it
s.sendall(pcnative.frame(2, "ping", {"echo": "hello"}))
fid, err, res = reply(s)
check("plain echo", res["echo"] == "hello" and set(res) == {"pong", "echo"})

# 4. NUL and non-UTF-8 bytes ride a bulk untouched (no encoding sibling:
# the tree carries bytes; the decoder hands them back via surrogateescape)
raw = b"A\x00B\xffC"
s.sendall(pcnative.frame(3, "ping", {"echo": raw}))
fid, err, res = reply(s)
check("raw NUL round-trip",
      res["echo"].encode("utf-8", "surrogateescape") == raw and
      "enc" not in res)

# 5. quotes, backslash, a control byte: nothing to escape on a tree, so
# they must come back byte for byte
s.sendall(pcnative.frame(4, "ping", {"echo": 'a"b\\c\x01'}))
fid, err, res = reply(s)
check("quote/backslash/control round-trip", res["echo"] == 'a"b\\c\x01')

# 6. unknown method -> an error frame carrying the message
s.sendall(pcnative.frame(5, "nope"))
fid, err, res = reply(s)
check("unknown method", fid == 5 and err == "method not found")

# 7. a malformed tree -> 'bad params', AND the connection survives it:
# three bad trees and a good ping in one write, four replies
bad = (frame(1, 60, cmdpay(b"ping", b"x"))                       # unknown tag
       + frame(1, 61, cmdpay(b"ping", b"m" + u32(1) + b"b" + u32(999999) + b"echo"))  # length past the frame
       + frame(1, 62, cmdpay(b"ping", b"i" + struct.pack("<q", 1)))  # root not a map
       + pcnative.frame(6, "ping"))
s.sendall(bad)
rs = replies(s, 4)
check("bad params + recovery",
      [x[0] for x in rs] == [60, 61, 62, 6] and
      all(x[1] == "bad params" for x in rs[:3]) and
      rs[3][1] is None and rs[3][2]["pong"] is True)

# 8. a missing or oversized method field -> 'invalid request', same rule
s.sendall(frame(1, 70, bytes([19]))                              # no length byte
          + frame(1, 71, bytes([19, 0]) + b"ping")               # length 0
          + frame(1, 72, bytes([19, 255]) + b"ping")             # length past the frame
          + pcnative.frame(73, "ping"))
rs = replies(s, 4)
check("invalid request + recovery",
      [x[0] for x in rs] == [70, 71, 72, 73] and
      all(x[1] == "invalid request" for x in rs[:3]) and
      rs[3][2]["pong"] is True)
s.close()

# 9. RED: the first-byte rule.  A LETTER first byte is a RESP inline
# command (S29, the netcat leg), so a typed typo answers -ERR loudly.
s = conn()
s.sendall(b"XYZZY\n")
check("letter sniff answers RESP -ERR",
    s.recv(64).startswith(b"-ERR unknown command 'XYZZY'"))
s.close()
# A byte that is neither a frame nor RESP is told so in one plain line,
# then the connection is closed (S317): true garbage...
s = conn()
s.sendall(b"\x01\x02\x03\n")
got = read_all(s)
check("garbage sniff: the refusal line, then the close",
      got.startswith(GONE) and got.endswith(b"\n"))
s.close()
# ...and what used to open the JSON-RPC text dialect
s = conn()
s.sendall(b'{"jsonrpc":"2.0","id":1,"method":"ping"}\n')
got = read_all(s)
check("a JSON-RPC line gets the same refusal and the close",
      got.startswith(GONE) and b"JSON-RPC dialect was removed" in got)
s.close()

# 10. RED: a frame whose length passes the ceiling (PC_MAX_REQ, 1 MB)
# drops the connection - on the header, before the body is read
s = conn()
try:
    s.sendall(struct.pack("<BBBBIQ", 0x9E, 1, 1, 0, 2 * 1024 * 1024, 9) +
              b"\x01" + b"a" * 65536)
    got = read_all(s)
except (BrokenPipeError, ConnectionResetError):
    got = b""
check("oversize frame drop", got == b"")
s.close()

# 11. fixed-layout verbs: framed ping echo with NULs
s = conn()
s.sendall(frame(1, 42, b"\x01" + raw))
t, f, fid, pl = read_frame(s)
check("binary ping echo", t == 2 and f == 0 and fid == 42 and pl == raw)

# 12. binary pipelining: two frames, one write
s.sendall(frame(1, 100, b"\x01one") + frame(1, 101, b"\x01two"))
r1 = read_frame(s); r2 = read_frame(s)
check("binary pipelining",
      {(r1[2], r1[3]), (r2[2], r2[3])} == {(100, b"one"), (101, b"two")})

# 13. RED: unknown binary verb -> error flag
s.sendall(frame(1, 200, b"\x7fjunk"))
t, f, fid, pl = read_frame(s)
check("binary unknown verb", t == 2 and (f & 1) and fid == 200)
s.close()

print("prototest: %d passed, %d failed" % (total - len(fails), len(fails)))
sys.exit(1 if fails else 0)
EOF
rc=$?

kill -TERM $PID 2>/dev/null
wait $PID 2>/dev/null
PID=
exit $rc
