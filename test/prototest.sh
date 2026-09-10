#!/bin/sh
# prototest.sh — S7 verification: both dialects end to end over a live
# daemon.  Text: ping round-trip, pipelined out-of-order-capable replies,
# the decision-#1 b64 path with real NUL bytes, escape round-trips, error
# replies that keep the connection usable, garbage/oversize teardown.
# Binary: framed ping echo (NULs included), pipelining, unknown-verb
# error flag.  Usage: test/prototest.sh [./perfcached]
set -u

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
import json, socket, struct, sys

ADDR = ("127.0.0.1", 16479)
fails = []

def conn():
    s = socket.create_connection(ADDR, timeout=3)
    return s

def lines(s, n):
    buf = b""
    while buf.count(b"\n") < n:
        d = s.recv(65536)
        if not d:
            break
        buf += d
    return [json.loads(x) for x in buf.split(b"\n") if x]

def check(name, cond):
    if cond:
        print("ok:", name)
    else:
        fails.append(name)
        print("FAIL:", name)

# 1. text ping round-trip
s = conn()
s.sendall(b'{"jsonrpc":"2.0","id":1,"method":"ping"}\n')
r = lines(s, 1)[0]
check("text ping", r.get("id") == 1 and r["result"]["pong"] is True)

# 2. pipelining: three requests in one write, three matched replies
s.sendall(b'{"jsonrpc":"2.0","id":7,"method":"ping"}\n'
          b'{"jsonrpc":"2.0","id":8,"method":"ping"}\n'
          b'{"jsonrpc":"2.0","id":"nine","method":"ping"}\n')
rs = lines(s, 3)
check("text pipelining ids", sorted(str(x["id"]) for x in rs) == ["7", "8", "nine"])

# 3. plain echo stays plain (no enc sibling)
s.sendall(b'{"jsonrpc":"2.0","id":2,"method":"ping","params":{"echo":"hello"}}\n')
r = lines(s, 1)[0]
check("plain echo", r["result"]["echo"] == "hello" and "enc" not in r["result"])

# 4. decision #1: NUL/binary rides b64 both ways
import base64
raw = b"A\x00B\xffC"
b64 = base64.b64encode(raw).decode()
s.sendall(json.dumps({"jsonrpc": "2.0", "id": 3, "method": "ping",
    "params": {"echo": b64, "enc": "b64"}}).encode() + b"\n")
r = lines(s, 1)[0]
check("b64 NUL round-trip",
      r["result"].get("enc") == "b64" and
      base64.b64decode(r["result"]["echo"]) == raw)

# 5. escape round-trip (quotes, backslash, control char via backslash-u0001)
s.sendall(b'{"jsonrpc":"2.0","id":4,"method":"ping","params":{"echo":"a\\"b\\\\c\\u0001"}}\n')
r = lines(s, 1)[0]
check("escape round-trip", r["result"]["echo"] == 'a"b\\c\x01')

# 6. unknown method -> -32601, connection stays usable
s.sendall(b'{"jsonrpc":"2.0","id":5,"method":"nope"}\n')
r = lines(s, 1)[0]
check("unknown method", r["error"]["code"] == -32601)

# 7. parse error -> -32700 AND the connection survives it
s.sendall(b'{oops\n{"jsonrpc":"2.0","id":6,"method":"ping"}\n')
rs = lines(s, 2)
check("parse error + recovery",
      rs[0]["error"]["code"] == -32700 and rs[1]["id"] == 6)
s.close()

# 8. RED: a garbage first byte tears the connection down, no reply.
# Since S29 a LETTER first byte is a RESP inline command (netcat leg),
# so a typed typo answers -ERR loudly instead of tearing down - the
# true-garbage probe uses a byte no dialect claims.
s = conn()
s.sendall(b"XYZZY\n")
check("letter sniff answers RESP -ERR",
    s.recv(64).startswith(b"-ERR unknown command 'XYZZY'"))
s.close()
s = conn()
s.sendall(b"\x01\x02\x03\n")
check("garbage sniff closes", s.recv(64) == b"")
s.close()

# 9. RED: oversized request line drops the connection
s = conn()
try:
    s.sendall(b'{"pad":"' + b"a" * (2 * 1024 * 1024) + b'"}\n')
    got = s.recv(64)
except (BrokenPipeError, ConnectionResetError):
    got = b""
check("oversize drop", got == b"")
s.close()

# 10. binary: framed ping echo with NULs
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
        pl += s.recv(plen - len(pl))
    return ftype, flags, fid, pl

s = conn()
s.sendall(frame(1, 42, b"\x01" + raw))
t, f, fid, pl = read_frame(s)
check("binary ping echo", t == 2 and f == 0 and fid == 42 and pl == raw)

# 11. binary pipelining: two frames, one write
s.sendall(frame(1, 100, b"\x01one") + frame(1, 101, b"\x01two"))
r1 = read_frame(s); r2 = read_frame(s)
check("binary pipelining",
      {(r1[2], r1[3]), (r2[2], r2[3])} == {(100, b"one"), (101, b"two")})

# 12. RED: unknown binary verb -> error flag
s.sendall(frame(1, 200, b"\x7fjunk"))
t, f, fid, pl = read_frame(s)
check("binary unknown verb", t == 2 and (f & 1) and fid == 200)
s.close()

print("prototest: %d passed, %d failed" % (12 - len(fails), len(fails)))
sys.exit(1 if fails else 0)
EOF
rc=$?

kill -TERM $PID 2>/dev/null
wait $PID 2>/dev/null
PID=
exit $rc
