#!/bin/sh
# bintest.sh — the binary dialect end to end, from raw frames (no
# libperfd - this is the WIRE contract a third-party client codes
# against) over a plaintext loopback listener.
#  Part 1, single node: every data verb's request/response layout,
#  raw bytes in values (magic byte, NUL, newline INSIDE a value),
#  error frames, per-MESSAGE dialect mixing (binary frames and JSON
#  lines interleaved pipelined on one connection, replies matched by
#  id), a 60KB value, and junk-byte connection teardown.
#  Part 2, two proxy nodes: the PARKED paths - a binary get answered
#  by a cluster pull, a binary set/add/del answered by a holder
#  forward - prove pc_proto_pull_complete speaks the request's
#  dialect (PULL / FWD_SET / FWD_ADD / FWD_DEL shapes).
# Usage: test/bintest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcbin.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT TERM INT

RES="$D/results"
: > "$RES"

# ---- part 1: single node ------------------------------------------------
cat > "$D/one.conf" <<EOF
[daemon]
workers = 2
log_level = warn
[memory]
arena_mb = 64
[secrets]
client = bin-client-secret
cluster = bin-cluster-secret
[listen]
tcp = 127.0.0.1:17061
plaintext = loopback
[collection c]
buckets_log2 = 12
EOF
"$BIN" -f "$D/one.conf" > "$D/one.log" 2>&1 &
P1=$!
i=0
while [ $i -lt 100 ]; do
	grep -q "perfcached ready" "$D/one.log" && break
	sleep 0.1; i=$((i+1))
done

RESFILE="$RES" PORT=17061 python3 - <<'EOF'
import json, os, socket, struct

MAGIC = 0x9E
res = open(os.environ["RESFILE"], "a")
def ok(cond, name):
    res.write(("P\n" if cond else "F %s\n" % name))
    if not cond:
        print("FAIL:", name)

def freq(payload, rid):
    return struct.pack("<BBBBIQ", MAGIC, 1, 1, 0, len(payload), rid) + payload
def vh(verb, col, key, extra=b"", val=b""):
    return bytes([verb, len(col)]) + struct.pack("<H", len(key)) + \
        extra + col + key + val
def i64(v): return struct.pack("<q", v)

class Conn:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=15)
        self.b = b""
    def send(self, x): self.s.sendall(x)
    def _fill(self):
        d = self.s.recv(65536)
        if not d: raise EOFError
        self.b += d
    def msg(self):
        """next reply: ('B', id, flags, payload) or ('T', id, obj)"""
        while True:
            while not self.b: self._fill()
            if self.b[0] == MAGIC:
                while len(self.b) < 16: self._fill()
                _, ver, typ, fl, pl = struct.unpack("<BBBBI", self.b[:8])
                rid = struct.unpack("<Q", self.b[8:16])[0]
                while len(self.b) < 16 + pl: self._fill()
                pay, self.b = self.b[16:16+pl], self.b[16+pl:]
                assert ver == 1 and typ in (2, 3)
                if typ == 3: continue
                return ("B", rid, fl, pay)
            while b"\n" not in self.b: self._fill()
            ln, self.b = self.b.split(b"\n", 1)
            o = json.loads(ln)
            if o.get("id") is None: continue
            return ("T", o["id"], o)

c = Conn(int(os.environ["PORT"]))

# ping: payload mirrored, id echoed
c.send(freq(bytes([1]) + b"hello\x00world", 42))
k, rid, fl, pay = c.msg()
ok(k == "B" and rid == 42 and fl == 0 and pay == b"hello\x00world",
    "ping echo")

# set/get: adversarial bytes (frame magic, NUL, newline) ride raw
blob = b"\x9e\x00\nplain\x9e" * 100
c.send(freq(vh(3, b"c", b"k1", i64(0), blob), 2))
k, rid, fl, pay = c.msg()
ok(rid == 2 and fl == 0 and pay == b"\x01", "set stored")
c.send(freq(vh(2, b"c", b"k1"), 3))
k, rid, fl, pay = c.msg()
ok(rid == 3 and fl == 0 and pay[0] == 1 and pay[5:] == blob and
    struct.unpack("<I", pay[1:5])[0] == 0, "get exact, no ttl")

# miss
c.send(freq(vh(2, b"c", b"nope"), 4))
ok(c.msg() == ("B", 4, 0, b"\x00"), "get miss")

# exists
c.send(freq(vh(5, b"c", b"k1"), 5)); ok(c.msg() == ("B", 5, 0, b"\x01"), "exists yes")
c.send(freq(vh(5, b"c", b"nope"), 6)); ok(c.msg() == ("B", 6, 0, b"\x00"), "exists no")

# ttl / expire
c.send(freq(vh(6, b"c", b"k1"), 7))
k, rid, fl, pay = c.msg()
ok(struct.unpack("<q", pay)[0] == -1, "ttl no expiry")
c.send(freq(vh(6, b"c", b"nope"), 8))
ok(struct.unpack("<q", c.msg()[3])[0] == -2, "ttl absent")
c.send(freq(vh(7, b"c", b"k1", i64(120)), 9))
ok(c.msg() == ("B", 9, 0, b"\x01"), "expire re-arms")
c.send(freq(vh(6, b"c", b"k1"), 10))
t = struct.unpack("<q", c.msg()[3])[0]
ok(115 <= t <= 120, "ttl after expire")
c.send(freq(vh(7, b"c", b"nope", i64(9)), 11))
ok(c.msg() == ("B", 11, 0, b"\x00"), "expire absent")

# counters
c.send(freq(vh(8, b"c", b"ctr", i64(7) + i64(0)), 12))
ok(struct.unpack("<q", c.msg()[3])[0] == 7, "add creates at 7")
c.send(freq(vh(9, b"c", b"ctr", i64(3) + i64(0)), 13))
ok(struct.unpack("<q", c.msg()[3])[0] == 4, "sub to 4")
c.send(freq(vh(8, b"c", b"k1", i64(1) + i64(0)), 14))
k, rid, fl, pay = c.msg()
ok(fl == 1 and b"integer" in pay, "add on string errs")

# error frames
c.send(freq(bytes([99]), 15))
k, rid, fl, pay = c.msg()
ok(fl == 1 and pay == b"unknown verb", "unknown verb")
c.send(freq(vh(2, b"zz", b"k1"), 16))
k, rid, fl, pay = c.msg()
ok(fl == 1 and pay == b"no such collection", "unknown collection")
c.send(freq(b"\x02\x01", 17))            # GET cut before klen
k, rid, fl, pay = c.msg()
ok(fl == 1 and pay == b"short request", "short request")

# per-message dialect mixing: 4 messages, one send
burst = freq(vh(3, b"c", b"mA", i64(0), b"from-bin"), 20)
burst += (b'{"jsonrpc":"2.0","id":21,"method":"set","params":'
          b'{"col":"c","key":"mB","value":"from-text"}}\n')
burst += freq(vh(2, b"c", b"mB"), 22)
burst += (b'{"jsonrpc":"2.0","id":23,"method":"get","params":'
          b'{"col":"c","key":"mA"}}\n')
c.send(burst)
got = {}
for _ in range(4):
    m = c.msg()
    got[m[1]] = m
ok(got[20][3] == b"\x01", "mixed: bin set")
ok(got[21][2]["result"]["stored"] is True, "mixed: text set")
ok(got[22][0] == "B" and got[22][3][5:] == b"from-text",
    "mixed: bin reads text write")
ok(got[23][0] == "T" and got[23][2]["result"]["value"] == "from-bin",
    "mixed: text reads bin write")

# 60KB value
big = bytes(range(256)) * 240
c.send(freq(vh(3, b"c", b"big", i64(0), big), 30))
ok(c.msg()[3] == b"\x01", "60KB set")
c.send(freq(vh(2, b"c", b"big"), 31))
ok(c.msg()[3][5:] == big, "60KB get exact")

# junk first byte tears the connection down
j = Conn(int(os.environ["PORT"]))
j.send(b"\x00\x01\x02")
try:
    while True: j._fill()
except socket.timeout:
    ok(False, "junk byte closes")
except (EOFError, OSError):
    ok(True, "junk byte closes")
res.close()
EOF
[ $? -eq 0 ] || echo "F part1-crashed" >> "$RES"

kill -9 $P1 2>/dev/null; wait $P1 2>/dev/null; P1=

# ---- part 2: two proxy nodes - the parked binary paths ------------------
node() { # node <n> <cliport>
	mkdir -p "$D/wal$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = warn
[memory]
arena_mb = 64
[secrets]
client = bin-client-secret
cluster = bin-cluster-secret
[listen]
tcp = 127.0.0.1:$2
plaintext = loopback
[cluster]
multicast = 239.255.77.36:17136
advertise = 127.0.0.4$1
pull_timeout_ms = 400
[collection px]
buckets_log2 = 12
mode = proxy
EOF
}
node 1 17062
node 2 17063

# node 2 first, ALONE: everything seeded here holds here
"$BIN" -f "$D/n2.conf" > "$D/n2.log" 2>&1 &
P2=$!
i=0
while [ $i -lt 100 ]; do
	grep -q "perfcached ready" "$D/n2.log" && break
	sleep 0.1; i=$((i+1))
done
python3 - <<'EOF'
import json, socket
s = socket.create_connection(("127.0.0.1", 17063), timeout=10)
f = s.makefile("rwb")
for i, (k, v) in enumerate([("pk1", "held-by-2"), ("pk2", "doomed"),
        ("pctr", "100")]):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": "set",
        "params": {"col": "px", "key": k, "value": v}}) + "\n").encode())
f.flush()
for _ in range(3):
    assert json.loads(f.readline())["result"]["stored"]
EOF
[ $? -eq 0 ] || echo "F seed-crashed" >> "$RES"

"$BIN" -f "$D/n1.conf" > "$D/n1.log" 2>&1 &
P1=$!
i=0
while [ $i -lt 100 ]; do
	grep -q "perfcached ready" "$D/n1.log" && break
	sleep 0.1; i=$((i+1))
done
sleep 3.5   # membership

RESFILE="$RES" PORT=17062 python3 - <<'EOF'
import json, os, socket, struct

MAGIC = 0x9E
res = open(os.environ["RESFILE"], "a")
def ok(cond, name):
    res.write(("P\n" if cond else "F %s\n" % name))
    if not cond:
        print("FAIL:", name)
def freq(payload, rid):
    return struct.pack("<BBBBIQ", MAGIC, 1, 1, 0, len(payload), rid) + payload
def vh(verb, col, key, extra=b"", val=b""):
    return bytes([verb, len(col)]) + struct.pack("<H", len(key)) + \
        extra + col + key + val
def i64(v): return struct.pack("<q", v)

s = socket.create_connection(("127.0.0.1", int(os.environ["PORT"])),
    timeout=15)
buf = b""
def msg():
    global buf
    while True:
        while len(buf) < 16:
            d = s.recv(65536)
            if not d: raise EOFError
            buf += d
        assert buf[0] == MAGIC
        _, ver, typ, fl, pl = struct.unpack("<BBBBI", buf[:8])
        rid = struct.unpack("<Q", buf[8:16])[0]
        while len(buf) < 16 + pl:
            buf += s.recv(65536)
        pay, buf = buf[16:16+pl], buf[16+pl:]
        if typ == 3: continue
        return rid, fl, pay

# binary get of a key held ONLY by node 2 -> parked cluster pull
s.sendall(freq(vh(2, b"px", b"pk1"), 100))
rid, fl, pay = msg()
ok(rid == 100 and fl == 0 and pay[0] == 1 and pay[5:] == b"held-by-2",
    "parked pull answers binary")

# binary set of that key -> the locator knows the holder -> FWD_SET
s.sendall(freq(vh(3, b"px", b"pk1", i64(0), b"rewritten"), 101))
rid, fl, pay = msg()
ok(rid == 101 and pay == b"\x01", "parked forward-set acks binary")
s.sendall(freq(vh(2, b"px", b"pk1"), 102))
rid, fl, pay = msg()
ok(pay[0] == 1 and pay[5:] == b"rewritten",
    "forwarded write landed at the holder")

# counter: learn the holder, then a forwarded add
s.sendall(freq(vh(2, b"px", b"pctr"), 103)); msg()
s.sendall(freq(vh(8, b"px", b"pctr", i64(5) + i64(0)), 104))
rid, fl, pay = msg()
ok(rid == 104 and fl == 0 and struct.unpack("<q", pay)[0] == 105,
    "parked forward-add returns 105")

# delete: learn the holder, then a forwarded del
s.sendall(freq(vh(2, b"px", b"pk2"), 105)); msg()
s.sendall(freq(vh(4, b"px", b"pk2"), 106))
rid, fl, pay = msg()
ok(rid == 106 and pay == b"\x01", "parked forward-del acks binary")
res.close()
EOF
[ $? -eq 0 ] || echo "F part2-crashed" >> "$RES"

kill -9 $P1 $P2 2>/dev/null; wait $P1 $P2 2>/dev/null; P1= P2=

pass=$(grep -c '^P' "$RES" || true)
fail=$(grep -c '^F' "$RES" || true)
echo "bintest: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
