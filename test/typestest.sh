#!/bin/sh
# typestest.sh - S280: a JSON document is a TYPE, not a string that
# happens to parse, and redis_types decides what the string commands do
# with it.
#
# Before S280 GET returned a document's text and JSON.GET served any
# string that parsed as JSON - a client that probes a key with GET and
# switches to JSON.GET on WRONGTYPE could never tell them apart.  Every
# record now carries its type (PCACHE_F_JSON), set by the JSON verbs and
# cleared by a plain SET, and it travels wherever the record does.  The
# expected answers are Redis 8.10.1's, probed 2026-09-30.
#
#   A. strict (the default), one node, all three doors: GET/INCR/SET NX
#      and XX/MGET/TYPE/EXPIRE/TTL/EXISTS/DEL against a document, JSON.*
#      against a string (Redis's exact words), path updates keep the type,
#      a SET over a document makes a string, native get/mget/add/jget as
#      CMD requests and the data verbs' own frames answer the same
#   B. the type survives a table resize (buckets grow, the copy is typed),
#      the dump (a "t" field), a restart from the WAL, and a restart from
#      the snapshot alone (the WAL removed)
#   C. loose: GET serves a document's text, JSON.GET a string that parses,
#      TYPE still reports the recorded type
#   D. three EAGER nodes: a document written on one is a document on the
#      others (the push), and on a node that rejoins EMPTY (the boot pull)
#   E. three SHARD nodes: GET through a non-owner (the pull answer) is
#      WRONGTYPE, INCR through one (the forwarded add) is WRONGTYPE, and a
#      JSON write through one to a string key (the forwarded JSON) is the
#      JSON refusal
# The standalone edition refuses [cluster]: D and E are SKIPPED, loudly.
# Fail-first: rc58 answers GET with the document's text and TYPE "string".
# Usage: test/typestest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pctyp.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0 skip=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
skp() { skip=$((skip+1)); echo "  SKIP $1"; }
for p in 18941 18942 18943 18951 18952 18953 18961 18962 18963; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "typestest: port $p busy" >&2; exit 1; }
done
SA=; "$BIN" -V 2>/dev/null | grep -q standalone && SA=1

conf() { # conf <n> <cluster mode or ""> <redis_types> <buckets_log2>
	mkdir -p "$D/s$1/wal"
	CL=
	[ -n "$2" ] && CL="[cluster]
multicast = 239.255.78.14:18945
advertise = 127.0.29.$1
mode = $2
collections = 0"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
allow_create = yes
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = typ-client-secret
cluster = typ-cluster-secret
enable = typ-enable
[listen]
tcp = 127.0.0.1:1894$1
resp = 127.0.0.1:1895$1
http = 127.0.0.1:1896$1
plaintext = loopback
$CL
[collection 0]
buckets_log2 = $4
redis_types = $3
[wal]
dir = $D/s$1/wal
probe = no
fsync = everysec
segment_mb = 8
segments = 4
save = off
C
	chmod 600 "$D/n$1.conf"
}
start() {
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 150 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"
	return 1
}
stop1() { [ -f "$D/n$1.pid" ] && { kill "$(cat "$D/n$1.pid")" 2>/dev/null; i=0; while kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done; rm -f "$D/n$1.pid"; }; }
stopall() { for n in 1 2 3; do stop1 $n; done; rm -rf "$D"/s*; }
st() { curl -s -m 3 "http://127.0.0.1:1896$1/stats" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["state"], d["cluster"]["peers_up"])' 2>/dev/null; }
formed() {
	i=0; while [ $i -lt 200 ]; do
		[ "$(st 1)$(st 2)$(st 3)" = "ready 2ready 2ready 2" ] && return 0
		sleep 0.2; i=$((i+1))
	done
	return 1
}
# drive <part>: the Python driver, ok/FAIL lines; ports are fixed by node
drive() {
	part=$1
	timeout 240 python3 - "$part" <<'PY' > "$D/py.$part" 2>&1
import socket, sys, time, json, pcnative, struct, urllib.request
part = sys.argv[1]
res = []
def ok(c, m): res.append(("ok" if c else "FAIL", m))
WT = "WRONGTYPE Operation against a key holding the wrong kind of value"
JT = "Existing key has wrong Redis type"
class R:
    def __init__(self, n):
        self.s = socket.create_connection(("127.0.0.1", 18950 + n), timeout=10); self.buf = b""
    def cmd(self, *a):
        self.s.sendall(("*%d\r\n" % len(a)).encode() + b"".join(("$%d\r\n" % len(x)).encode() + x.encode() + b"\r\n" for x in a))
        return self.read()
    def _line(self):
        while b"\r\n" not in self.buf:
            d = self.s.recv(65536)
            if not d: raise EOFError
            self.buf += d
        l, self.buf = self.buf.split(b"\r\n", 1); return l
    def read(self):
        l = self._line(); t, b = l[:1], l[1:]
        if t == b"+": return b.decode()
        if t == b"-": return "-" + b.decode()          # the raw error line
        if t == b":": return int(b)
        if t == b"$":
            n = int(b)
            if n < 0: return None
            while len(self.buf) < n + 2:
                d = self.s.recv(65536)
                if not d: raise EOFError
                self.buf += d
            v, self.buf = self.buf[:n], self.buf[n + 2:]; return v.decode(errors="replace")
        if t == b"*": return [self.read() for _ in range(int(b))]
        return "<?>"
class N:
    """the native door: rpc() sends CMD requests through pcnative on one
    connection; bin() sends the data verbs' own frames on a second one
    (the helper owns its socket and its read buffer)"""
    def __init__(self, n):
        self.port = 18940 + n
        self.s = socket.create_connection(("127.0.0.1", self.port), timeout=10); self.f = pcnative.wrap(self.s); self.id = 0
        self.bs = self.bf = None
    def rpc(self, method, **params):
        self.id += 1
        self.f.write((json.dumps({"jsonrpc": "2.0", "id": self.id, "method": method, "params": params}) + "\n").encode()); self.f.flush()
        return json.loads(self.f.readline())
    def bin(self, verb, key, val="", ttl=None, by=None):
        if self.bs is None:
            self.bs = socket.create_connection(("127.0.0.1", self.port), timeout=10); self.bf = self.bs.makefile("rb")
        self.id += 1
        col = "0"
        if verb in (8, 9):          # add/sub: by i64, ttl i64
            head = struct.pack("<BBHqq", verb, len(col), len(key), by or 0, ttl or 0)
        elif verb in (3, 7, 15, 16):
            head = struct.pack("<BBHq", verb, len(col), len(key), ttl or 0)
        else:
            head = struct.pack("<BBH", verb, len(col), len(key))
        pl = head + col.encode() + key.encode() + val.encode()
        self.bs.sendall(struct.pack("<BBBBIQ", 0x9E, 1, 1, 0, len(pl), self.id) + pl)
        hdr = self.bf.read(16); flags, n = hdr[3], struct.unpack("<I", hdr[4:8])[0]
        body = self.bf.read(n)
        if flags & 2:
            body = body[:-8]       # S317: a parked reply's moved stamp [term][seq] after the payload
        return ("ERR", body.decode(errors="replace")) if flags & 1 else body
def errmsg(r):
    e = r.get("error")
    if isinstance(e, dict): return e.get("message", "")
    if isinstance(r.get("result"), dict) and "error" in r["result"]: return r["result"]["error"]
    return ""
def stats(n):
    return json.load(urllib.request.urlopen("http://127.0.0.1:1896%d/stats" % n, timeout=5))
def col0(n):
    return [c for c in stats(n)["collections"] if c["name"] == "0"][0]
def settled(fn, want, secs=5.0):
    t = time.time() + secs
    while True:
        v = fn()
        if v == want or time.time() > t: return v
        time.sleep(0.05)

if part == "A":
    c = R(1)
    ok(c.cmd("JSON.SET", "doc", "$", '{"a":1,"b":{"c":[1,2]}}') == "OK", "A JSON.SET doc")
    ok(c.cmd("GET", "doc") == "-" + WT, "A GET on a document: %r" % c.cmd("GET", "doc"))
    ok(c.cmd("TYPE", "doc") == "ReJSON-RL", "A TYPE of a document: ReJSON-RL")
    ok(c.cmd("INCR", "doc") == "-" + WT, "A INCR on a document: WRONGTYPE")
    ok(c.cmd("SET", "plain", '{"b":2}') == "OK" and c.cmd("TYPE", "plain") == "string", "A SET of JSON text makes a string")
    ok(c.cmd("MGET", "doc", "plain", "none") == [None, '{"b":2}', None], "A MGET: nil for the document (%r)" % (c.cmd("MGET", "doc", "plain", "none"),))
    for args in (("JSON.GET", "plain"), ("JSON.SET", "plain", "$", '{"c":3}'), ("JSON.SET", "plain", "$.x", "1"),
                 ("JSON.NUMINCRBY", "plain", "$.b", "1"), ("JSON.DEL", "plain")):
        r = c.cmd(*args)
        ok(r == "-" + JT, "A %s on a string: %r" % (" ".join(args[:2]), r))
    ok(c.cmd("GET", "plain") == '{"b":2}', "A the string is untouched by the refused JSON writes")
    # path updates keep the document a document
    seq = [c.cmd("JSON.SET", "doc", "$.a", "5"), c.cmd("JSON.SET", "doc", "$.b.d", '"x"'),
           c.cmd("JSON.NUMINCRBY", "doc", "$.a", "2"), c.cmd("JSON.ARRAPPEND", "doc", "$.b.c", "3"),
           c.cmd("JSON.DEL", "doc", "$.b.d")]
    ok(c.cmd("TYPE", "doc") == "ReJSON-RL" and c.cmd("GET", "doc") == "-" + WT,
       "A path writes ($.a, $.b.d, NUMINCRBY, ARRAPPEND, DEL $.b.d) keep a document: %r" % (seq,))
    got = json.loads(c.cmd("JSON.GET", "doc", "$"))
    ok(got == [{"a": 7, "b": {"c": [1, 2, 3]}}], "A JSON.GET after the path writes: %r" % (got,))
    ok(c.cmd("JSON.SET", "new", "$.x", "1") is not None and c.cmd("TYPE", "new") == "none",
       "A a path write on an absent key creates nothing")
    ok(c.cmd("EXPIRE", "doc", "100") == 1 and 95 <= c.cmd("TTL", "doc") <= 100 and c.cmd("EXISTS", "doc") == 1,
       "A EXPIRE/TTL/EXISTS on a document")
    ok(c.cmd("SET", "doc", "7", "NX") is None and c.cmd("TYPE", "doc") == "ReJSON-RL", "A SET NX on a document: nil")
    c.cmd("JSON.SET", "d2", "$", '{"a":1}')
    ok(c.cmd("SET", "d2", "hello", "XX") == "OK" and c.cmd("TYPE", "d2") == "string" and c.cmd("GET", "d2") == "hello",
       "A SET XX over a document makes a string")
    c.cmd("JSON.SET", "d3", "$", '{"a":1}')
    ok(c.cmd("SETEX", "d3", "10", "v") == "OK" and c.cmd("TYPE", "d3") == "string", "A SETEX over a document makes a string")
    c.cmd("JSON.SET", "d4", "$", '{"a":1}')
    ok(c.cmd("DEL", "d4") == 1 and c.cmd("TYPE", "d4") == "none", "A DEL removes a document")
    # the native doors, uniform with RESP
    n = N(1)
    ok(WT in errmsg(n.rpc("get", col="0", key="doc")), "A native get on a document: WRONGTYPE")
    r = n.rpc("mget", col="0", keys=["doc", "plain"])["result"]["values"]
    ok(r[0] == {"found": False} and r[1].get("found") is True, "A native mget: a miss for the document")
    ok(WT in errmsg(n.rpc("add", col="0", key="doc", by=1)), "A native add on a document: WRONGTYPE")
    ok(JT in errmsg(n.rpc("jget", col="0", key="plain")), "A native jget on a string: the JSON refusal")
    ok(JT in errmsg(n.rpc("jdel", col="0", key="plain", path="$")) and c.cmd("EXISTS", "plain") == 1,
       "A native root jdel on a string: the JSON refusal, the key kept (as RESP JSON.DEL, as Redis)")
    ok(n.rpc("jset", col="0", key="nd", val='{"z":1}').get("result") == {"set": True} and c.cmd("TYPE", "nd") == "ReJSON-RL",
       "A native jset makes a document")
    b = n.bin(2, "doc")
    ok(isinstance(b, tuple) and WT in b[1], "A binary GET on a document: %r" % (b,))
    b = n.bin(8, "doc", by=1)
    ok(isinstance(b, tuple) and WT in b[1], "A binary ADD on a document: WRONGTYPE")
    b = n.bin(2, "plain")
    ok(isinstance(b, bytes) and b[:1] == b"\x01" and b[5:] == b'{"b":2}', "A binary GET of the string: served")
elif part == "A2":
    # B: a RESIZE - the verb, which builds a new table and copies every
    # record into it (store.c rs_copy_cb); the core's own growth only
    # moves pointers and would keep the type whatever the copy did
    c = R(1); n = N(1)
    ok("result" in n.rpc("enable", secret="typ-enable"), "B enable (resize and restore are privileged)")
    b0 = col0(1)["buckets"]
    r = n.rpc("resize", col="0", buckets_log2=12)
    b1 = settled(lambda: col0(1)["buckets"], 4096, 10)
    ok(b1 == 4096 and b0 != 4096, "B resize %d -> %d buckets: the table was rebuilt (%r)" % (b0, b1, r.get("result", r.get("error"))))
    ok(c.cmd("TYPE", "doc") == "ReJSON-RL" and c.cmd("GET", "doc") == "-" + WT and c.cmd("TYPE", "plain") == "string",
       "B after the resize: the document is still a document, the string a string")
    recs = {}; cursor = 0; first = True
    while first or cursor:
        first = False
        r = n.rpc("dump", col="0", cursor=cursor, count=512)["result"]
        for x in r["records"]: recs[x["k"]] = x
        cursor = r["cursor"]
    ok(recs.get("doc", {}).get("t") == "json" and "t" not in recs.get("plain", {"t": 1}),
       "B dump: the document carries \"t\":\"json\", the string nothing")
    # restore both under new names: the type comes back from "t"
    rd = dict(recs["doc"]); rd["k"] = "rdoc"; rs = dict(recs["plain"]); rs["k"] = "rplain"
    for x in (rd, rs): x.pop("ttl", None)
    r = n.rpc("restore", col="0", records=[rd, rs])
    ok(r.get("result", {}).get("stored") == 2 and c.cmd("TYPE", "rdoc") == "ReJSON-RL" and c.cmd("GET", "rdoc") == "-" + WT
       and c.cmd("TYPE", "rplain") == "string", "B restore: the dumped document comes back a document (%r)" % (r.get("result", r.get("error")),))
elif part in ("B1", "B2"):
    c = R(1)
    ok(c.cmd("TYPE", "doc") == "ReJSON-RL" and c.cmd("GET", "doc") == "-" + WT and c.cmd("TYPE", "plain") == "string"
       and json.loads(c.cmd("JSON.GET", "doc", "$"))[0]["a"] == 7,
       "B restarted from the %s: the document is a document, the string a string" % ("WAL" if part == "B1" else "snapshot alone"))
elif part == "Bsave":
    ok(N(1).rpc("save").get("result", {}).get("started") is True, "B save started")
elif part == "C":
    c = R(1)
    c.cmd("JSON.SET", "doc", "$", '{"a":1}'); c.cmd("SET", "plain", '{"b":2}')
    ok(c.cmd("GET", "doc") == '{"a":1}', "C loose: GET serves a document's text")
    ok(json.loads(c.cmd("JSON.GET", "plain", "$")) == [{"b": 2}], "C loose: JSON.GET reads a string that parses")
    ok(c.cmd("TYPE", "doc") == "ReJSON-RL" and c.cmd("TYPE", "plain") == "string", "C loose: TYPE still reports the recorded type")
    ok(c.cmd("MGET", "doc") == ['{"a":1}'], "C loose: MGET serves the document")
    ok(N(1).rpc("get", col="0", key="doc")["result"].get("found") is True, "C loose: native get serves the document")
elif part == "D":
    R(1).cmd("JSON.SET", "edoc", "$", '{"a":1}'); R(1).cmd("SET", "estr", "s")
    for n in (2, 3):
        ok(settled(lambda: R(n).cmd("TYPE", "edoc"), "ReJSON-RL") == "ReJSON-RL" and R(n).cmd("GET", "edoc") == "-" + WT,
           "D node %d holds the pushed document as a document" % n)
elif part == "D3":
    ok(settled(lambda: R(3).cmd("TYPE", "edoc"), "ReJSON-RL", 20) == "ReJSON-RL" and R(3).cmd("GET", "edoc") == "-" + WT
       and R(3).cmd("TYPE", "estr") == "string",
       "D node 3 rejoined EMPTY: the boot pull brought the document back as a document")
elif part == "E":
    # write through the native door on node 1: jset forwards to the owner
    n1 = N(1)
    for i in range(40):
        n1.rpc("jset", col="0", key="sd%d" % i, val='{"a":%d}' % i)
        n1.rpc("set", col="0", key="ss%d" % i, value="7")
    pulled = 0; fwd_add = 0; fwd_json = 0
    for i in range(40):
        for n in (1, 2, 3):
            r = R(n).cmd("GET", "sd%d" % i)
            if r == "-" + WT: pulled += 1
    ok(pulled == 120, "E GET of 40 documents through each of 3 nodes: %d/120 WRONGTYPE (two of three are pulls)" % pulled)
    for i in range(40):
        for n in (1, 2, 3):
            if R(n).cmd("INCR", "sd%d" % i) == "-" + WT: fwd_add += 1
            if JT in errmsg(N(n).rpc("jset", col="0", key="ss%d" % i, path="$.x", val="1")): fwd_json += 1
    ok(fwd_add == 120, "E INCR of 40 documents through each node: %d/120 WRONGTYPE (two of three forwarded)" % fwd_add)
    ok(fwd_json == 120, "E jset $.x on 40 strings through each node: %d/120 the JSON refusal (two of three forwarded)" % fwd_json)
    served = sum(int(stats(n)["cluster"].get("fwd_served", 0)) for n in (1, 2, 3))
    ok(served >= 80, "E the forward paths ran: %d forwards served" % served)
for k, m_ in res: print("  %-4s %s" % (k, m_))
print("PYDONE %d" % sum(1 for k, _ in res if k == "FAIL"))
PY
	grep -E "^  (ok|FAIL) " "$D/py.$part"
	pf=$(grep -oE "^PYDONE [0-9]+" "$D/py.$part" | awk '{print $2}')
	[ -n "$pf" ] || { bad "$part: the driver did not finish: $(tail -3 "$D/py.$part" | tr '\n' ' ' | cut -c1-300)"; pf=0; }
	pass=$((pass + $(grep -c "^  ok " "$D/py.$part"))); fail=$((fail + pf))
}

echo "--- A. strict, one node, three doors"
conf 1 "" strict 4
if start 1; then
	drive A
	echo "--- B. the type survives: resize, dump, the WAL, the snapshot"
	drive A2
	stop1 1; start 1 && drive B1
	drive Bsave
	# the snapshot lives beside the WAL segments: remove only the
	# segments, so the restart has the snapshot and nothing else
	i=0; while [ $i -lt 100 ] && [ ! -s "$D/s1/wal/dump.rdb" ]; do sleep 0.1; i=$((i+1)); done
	[ -s "$D/s1/wal/dump.rdb" ] || bad "B the snapshot was not written"
	stop1 1
	rm -f "$D/s1/wal"/wal-*.seg
	start 1 && drive B2
else bad "A: node did not start"; fi
stopall

echo "--- C. loose"
conf 1 "" loose 10
if start 1; then drive C; else bad "C: node did not start"; fi
stopall

echo "--- D. three eager nodes: the push and the boot pull"
if [ -n "$SA" ]; then skp "D: the standalone edition refuses a [cluster] section"; else
	for n in 1 2 3; do conf $n eager strict 10; done
	up=1; for n in 1 2 3; do start $n || up=; done
	if [ -n "$up" ] && formed; then
		drive D
		stop1 3; rm -rf "$D/s3"; mkdir -p "$D/s3/wal"
		start 3 && formed && drive D3
	else bad "D: the eager fleet did not form"; fi
	stopall
fi

echo "--- E. three shard nodes: the pull, the forwarded add, the forwarded JSON"
if [ -n "$SA" ]; then skp "E: the standalone edition refuses a [cluster] section"; else
	for n in 1 2 3; do conf $n shard strict 10; done
	up=1; for n in 1 2 3; do start $n || up=; done
	if [ -n "$up" ] && formed; then drive E; else bad "E: the shard fleet did not form"; fi
	stopall
fi
echo "typestest: $pass passed, $fail failed, $skip skipped"
[ $fail -eq 0 ]
