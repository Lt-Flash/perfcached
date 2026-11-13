#!/bin/sh
# utf8statstest.sh - /stats is valid UTF-8 (RFC 8259) whatever a client stores: client bytes in the slow
# log's argv and client names must be escaped, or the whole body stops parsing.
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcu8.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on

cat > "$D/a.conf" <<CONF
[daemon]
workers = 2
log_level = notice
slowlog_usec = 1
[memory]
arena_mb = 64
[secrets]
client = u8-client-secret
[listen]
resp = 127.0.0.1:17710
http = 127.0.0.1:17711
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
chmod 600 "$D/a.conf"
"$BIN" -f "$D/a.conf" > "$D/a.log" 2>&1 &
P=$!
i=0
while [ $i -lt 100 ]; do
	grep -q "perfcached ready" "$D/a.log" && break
	sleep 0.1; i=$((i+1))
done
grep -q "perfcached ready" "$D/a.log" || { echo "daemon did not start"; cat "$D/a.log"; exit 1; }

python3 - <<'PY_EOF'
import json, socket, sys, urllib.request

pass_n = fail_n = 0
def ok(m):
    global pass_n; pass_n += 1; print("  ok   " + m)
def bad(m):
    global fail_n; fail_n += 1; print("  FAIL " + m)

def cmd(*a):
    o = b"*%d\r\n" % len(a)
    for x in a:
        if isinstance(x, str): x = x.encode()
        o += b"$%d\r\n%s\r\n" % (len(x), x)
    return o
s = socket.create_connection(("127.0.0.1", 17710), 8); s.settimeout(20)
f = s.makefile("rwb")

# every shape of client-supplied bytes that reaches the JSON: a binary
# VALUE, a binary KEY, and the name a client announces for itself
s.sendall(cmd("SET", b"bin:value", bytes(range(256)) * 8)); f.flush(); f.readline()
s.sendall(cmd("SET", b"k\xd8\xff:bin", b"v")); f.flush(); f.readline()
s.sendall(cmd("GET", b"k\xd8\xff:bin")); f.flush(); f.readline(); f.readline()
s.sendall(cmd("CLIENT", "SETNAME", b"nm\xc3\x28bad")); f.flush(); f.readline()

raw = urllib.request.urlopen("http://127.0.0.1:17711/stats", timeout=10).read()
try:
    txt = raw.decode("utf-8")
    ok("/stats is valid UTF-8 with binary keys, values and a client name")
except UnicodeDecodeError as e:
    lo = max(0, e.start - 60)
    bad("/stats is not valid UTF-8 at byte %d: ...%s..."
        % (e.start, raw[lo:e.start + 40].decode("utf-8", "replace")))
    txt = raw.decode("utf-8", "replace")

try:
    st = json.loads(txt)
    ok("and parses as JSON")
except Exception as ex:
    bad("it does not parse: %s" % ex)
    st = {}

sl = st.get("slowlog") or []
(ok if sl else bad)("the slow log captured the commands (%d entries)" % len(sl))
# the replacement character is the point (shown, not dropped or passed raw), checked
# in the RAW body: a decode with errors="replace" would make U+FFFD by itself
(ok if b"\\ufffd" in raw else bad)(
    "the DAEMON escaped them as \\ufffd - a broken build emits the raw byte")

print("utf8statstest: %d passed, %d failed" % (pass_n, fail_n))
sys.exit(1 if fail_n else 0)
PY_EOF
rc=$?
exit $rc
