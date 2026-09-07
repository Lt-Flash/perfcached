#!/bin/sh
# resptest.sh — the RESP compatibility dialect (task S29, cut 1).
# Asserted:
#  - RESP2 multibulk framing end to end: PING/ECHO/SET/GET/DEL/EXISTS/
#    EXPIRE/TTL/INCR family/KEYS/SCAN/MGET/TYPE/DBSIZE/SELECT, binary-
#    safe values, pipelining;
#  - honest refusals: unknown commands, unsupported SET options, AUTH
#    without a password plane, HELLO 3, FLUSHDB;
#  - inline commands (the netcat leg);
#  - dialect isolation: RESP and JSON-RPC interleave on ONE connection;
#  - cluster: a RESP GET on node2 of a key written via node1 parks on
#    the pull and completes as a RESP bulk (store mode), and a fleet
#    miss completes as $-1;
#  - QUIT answers +OK and closes.
# Usage: test/resptest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcresp.XXXXXX)
P1= P2=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; \
     [ -n "$P2" ] && kill -9 $P2 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

node() { # node <n> <cliport>  (advertises 127.0.2.2<n>)
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = resp-client-secret
cluster = resp-cluster-secret
[listen]
tcp = 127.0.0.1:$2
plaintext = loopback
[cluster]
multicast = 239.255.77.52:17152
advertise = 127.0.2.2$1
pull_timeout_ms = 300
[collection 0]
buckets_log2 = 10
pull = 1
[collection 1]
buckets_log2 = 10
pull = 1
[collection 2]
buckets_log2 = 10
mode = proxy
EOF
}
start() {
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$1=\$!"
	i=0
	while [ $i -lt 60 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start"; cat "$D/n$1.log"; exit 1
}

# a tiny RESP2 client: encodes argv lines to multibulk, decodes replies.
# stdin: one command per line, args separated by \x1f (unit separator) so
# values may contain spaces; a line "RAWJSON <json>" sends a JSON-RPC
# line over the SAME socket (the dialect-mixing leg).
RC="$D/respc.py"
cat > "$RC" <<'EOF'
import socket, sys

port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=8)
f = s.makefile("rwb")

def rd():
    line = f.readline()
    if not line:
        return "<closed>"
    t, body = chr(line[0]), line[1:-2].decode("latin1")
    if t == "+": return body
    if t == "-": return body
    if t == ":": return "int:" + body
    if t == "$":
        n = int(body)
        if n < 0: return "nil"
        data = f.read(n + 2)[:-2]
        return "bulk:" + data.decode("latin1")
    if t == "*":
        n = int(body)
        if n < 0: return "nil"
        return "arr:[" + "|".join(rd() for _ in range(n)) + "]"
    return "?" + t + body

for line in sys.stdin:
    line = line.rstrip("\n")
    if not line:
        continue
    if line.startswith("RAWJSON "):
        f.write(line[8:].encode() + b"\n"); f.flush()
        print("json:" + f.readline().decode().strip())
        continue
    args = [a.encode("latin1") for a in line.split("\x1f")]
    out = b"*%d\r\n" % len(args)
    for a in args:
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    f.write(out); f.flush()
    print(rd())
EOF
US=$(printf '\037')
rcall() { printf '%s\n' "$2" | python3 "$RC" "$1"; }

node 1 17091
node 2 17092
start 1
start 2
sleep 3.5

# ---- the command battery on node1 --------------------------------------
OUT=$(python3 "$RC" 17091 <<EOF
PING
PING${US}hello
ECHO${US}echo me
SET${US}k1${US}v1
GET${US}k1
GET${US}nosuch
EXISTS${US}k1${US}nosuch${US}k1
TYPE${US}k1
TYPE${US}nosuch
SET${US}k2${US}v2${US}EX${US}500
TTL${US}k2
PTTL${US}k2
TTL${US}k1
TTL${US}nosuch
SETEX${US}k3${US}300${US}v3
TTL${US}k3
EXPIRE${US}k1${US}400
TTL${US}k1
INCR${US}ctr
INCRBY${US}ctr${US}9
DECR${US}ctr
DECRBY${US}ctr${US}4
INCR${US}k1
DEL${US}k3
DEL${US}k3
MGET${US}k1${US}nosuch${US}k2
DBSIZE
SELECT${US}1
SET${US}iso${US}other-db
DBSIZE
SELECT${US}0
GET${US}iso
SELECT${US}9
LPUSH${US}a${US}b
SET${US}k9${US}v9${US}NX
AUTH${US}whatever
HELLO${US}3
FLUSHDB
COMMAND${US}DOCS
RAWJSON {"jsonrpc":"2.0","id":7,"method":"ping"}
GET${US}k2
EOF
)
expect() { # expect <lineno> <exact>
	L=$(printf '%s\n' "$OUT" | sed -n "${1}p")
	[ "$L" = "$2" ] && ok || bad "line $1: got '$L' want '$2'"
}
expect 1 "PONG"
expect 2 "bulk:hello"
expect 3 "bulk:echo me"
expect 4 "OK"
expect 5 "bulk:v1"
expect 6 "nil"
expect 7 "int:2"
expect 8 "string"
expect 9 "none"
expect 10 "OK"
L=$(printf '%s\n' "$OUT" | sed -n 11p)
case "$L" in int:49[0-9]|int:500) ok;; *) bad "ttl k2: $L";; esac
L=$(printf '%s\n' "$OUT" | sed -n 12p)
case "$L" in int:49[0-9]000|int:500000) ok;; *) bad "pttl k2: $L";; esac
expect 13 "int:-1"
expect 14 "int:-2"
expect 15 "OK"
L=$(printf '%s\n' "$OUT" | sed -n 16p)
case "$L" in int:29[0-9]|int:300) ok;; *) bad "ttl k3: $L";; esac
expect 17 "int:1"
L=$(printf '%s\n' "$OUT" | sed -n 18p)
case "$L" in int:39[0-9]|int:400) ok;; *) bad "ttl k1 after expire: $L";; esac
expect 19 "int:1"
expect 20 "int:10"
expect 21 "int:9"
expect 22 "int:5"
expect 23 "ERR value is not an integer or out of range"
expect 24 "int:1"
expect 25 "int:0"
expect 26 "arr:[bulk:v1|nil|bulk:v2]"
expect 27 "int:3"
expect 28 "OK"
expect 29 "OK"
expect 30 "int:1"
expect 31 "OK"
expect 32 "nil"
L=$(printf '%s\n' "$OUT" | sed -n 33p)
case "$L" in "ERR DB index is out of range"*) ok;; *) bad "select 9: $L";; esac
L=$(printf '%s\n' "$OUT" | sed -n 34p)
case "$L" in "ERR unknown command 'LPUSH'") ok;; *) bad "lpush: $L";; esac
expect 35 "ERR unsupported SET option"
expect 36 "ERR Client sent AUTH, but no password is set"
L=$(printf '%s\n' "$OUT" | sed -n 37p)
case "$L" in NOPROTO*) ok;; *) bad "hello 3: $L";; esac
L=$(printf '%s\n' "$OUT" | sed -n 38p)
case "$L" in "ERR FLUSHDB"*) ok;; *) bad "flushdb: $L";; esac
expect 39 "arr:[]"
L=$(printf '%s\n' "$OUT" | sed -n 40p)
case "$L" in 'json:{"jsonrpc":"2.0","id":7,"result":{"pong":true}}') ok;; \
	*) bad "mixed-dialect json: $L";; esac
expect 41 "bulk:v2"

# ---- S48: TIME / EXPIREAT / PEXPIREAT / MEMORY USAGE -------------------
NOW=$(date +%s)
O48=$(python3 "$RC" 17091 <<EOF
TIME
SET${US}s48${US}hello48
EXPIREAT${US}s48${US}$((NOW + 300))
TTL${US}s48
PEXPIREAT${US}s48${US}$(( (NOW + 200) * 1000 ))
TTL${US}s48
MEMORY${US}USAGE${US}s48
MEMORY${US}USAGE${US}s48${US}SAMPLES${US}0
MEMORY${US}USAGE${US}nosuch48
MEMORY${US}DOCTOR
EXPIREAT${US}s48${US}$((NOW - 10))
EXISTS${US}s48
EXPIREAT${US}nosuch48${US}$((NOW + 100))
EOF
)
e48() { # e48 <lineno> <case-glob> <label>
	L=$(printf '%s\n' "$O48" | sed -n "${1}p")
	case "$L" in $2) ok;; *) bad "$3: got '$L'";; esac
}
# TIME: [seconds, microseconds]; seconds within 30s of the shell's own
TS=$(printf '%s\n' "$O48" | sed -n 1p | sed 's/arr:\[bulk:\([0-9]*\).*/\1/')
[ -n "$TS" ] && [ "$TS" -ge "$NOW" ] && [ "$TS" -le $((NOW + 30)) ] \
	&& ok || bad "TIME seconds: '$TS' vs shell $NOW"
e48 1 "arr:?bulk:[0-9]*" "TIME shape"
e48 3 "int:1" "EXPIREAT set"
# alternation must be literal in a case PATTERN - through a variable
# the | is just a character (the first run failed on its own correct
# answers here)
L=$(printf '%s\n' "$O48" | sed -n 4p)
case "$L" in int:29[0-9]|int:300) ok;; *) bad "TTL after EXPIREAT: got '$L'";; esac
e48 5 "int:1" "PEXPIREAT overrides"
L=$(printf '%s\n' "$O48" | sed -n 6p)
case "$L" in int:19[0-9]|int:200) ok;; *) bad "TTL after PEXPIREAT: got '$L'";; esac
e48 7 "int:34" "MEMORY USAGE (24 hdr + 3 key + 7 val)"
e48 8 "int:34" "MEMORY USAGE with SAMPLES"
e48 9 "nil" "MEMORY USAGE miss"
e48 10 "ERR MEMORY supports only*" "MEMORY subcommand refusal"
e48 11 "int:1" "past EXPIREAT deletes, reports 1"
e48 12 "int:0" "the key is gone"
e48 13 "int:0" "EXPIREAT on a missing key"

# ---- binary-safe value through RESP ------------------------------------
R=$(python3 - <<'EOF'
import socket
s = socket.create_connection(("127.0.0.1", 17091), timeout=8)
f = s.makefile("rwb")
val = bytes(range(256)) * 4
def cmd(*args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    f.write(out); f.flush()
cmd(b"SET", b"bin", val)
assert f.readline() == b"+OK\r\n"
cmd(b"GET", b"bin")
hdr = f.readline()
assert hdr == b"$%d\r\n" % len(val), hdr
back = f.read(len(val) + 2)[:-2]
print("BINOK" if back == val else "BINBAD")
EOF
)
[ "$R" = "BINOK" ] && ok || bad "binary value roundtrip: $R"

# ---- INFO carries a replication role (rtpengine REFUSES to start
# without one: "Asking Redis whether it's master or slave... Failed to
# connect" - found by pointing a real rtpengine at the daemon) --------
R=$(timeout 5 python3 - <<'EOF'
import socket
s = socket.create_connection(("127.0.0.1", 17091), timeout=5)
s.sendall(b"*1\r\n$4\r\nINFO\r\n")
d = b""
while b"# Keyspace" not in d:
    c = s.recv(4096)
    if not c:
        break
    d += c
t = d.decode("latin1")
print("ROLEOK" if "role:master" in t and "redis_version:" in t else "ROLEBAD")
EOF
)
[ "$R" = "ROLEOK" ] && ok || bad "INFO lacks role:master / redis_version: $R"

# ---- KEYS / SCAN full census -------------------------------------------
OUT2=$(python3 "$RC" 17091 <<EOF
KEYS${US}k*
KEYS${US}*
EOF
)
L=$(printf '%s\n' "$OUT2" | sed -n 1p)
N=$(printf '%s' "$L" | tr '|' '\n' | grep -c "bulk:k")
[ "$N" -eq 2 ] && ok || bad "KEYS k* (want k1,k2): $L"
R=$(python3 - <<'EOF'
import socket
s = socket.create_connection(("127.0.0.1", 17091), timeout=8)
f = s.makefile("rwb")
def cmd(*args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    f.write(out); f.flush()
def rd():
    line = f.readline()
    t, body = chr(line[0]), line[1:-2]
    if t == "$":
        n = int(body)
        return f.read(n + 2)[:-2] if n >= 0 else None
    if t == "*":
        return [rd() for _ in range(int(body))]
    return body
seen, cur = set(), b"0"
for _ in range(1000):
    cmd(b"SCAN", cur, b"COUNT", b"64")
    cur, keys = rd()
    seen.update(keys)
    if cur == b"0":
        break
print(len(seen))
EOF
)
[ "$R" -eq 4 ] && ok || bad "SCAN census: $R (want 4: k1 k2 ctr bin)"

# ---- inline commands (the netcat leg) ----------------------------------
R=$(timeout 5 python3 - <<'EOF'
import socket
s = socket.create_connection(("127.0.0.1", 17091), timeout=5)
s.sendall(b"PING\r\nGET k1\r\n")
import time; time.sleep(0.3)
print(s.recv(4096).decode("latin1").replace("\r\n", "/"))
EOF
)
case "$R" in "+PONG/\$2/v1/"*) ok;; *) bad "inline: $R";; esac

# ---- QUIT closes after +OK ---------------------------------------------
R=$(python3 - <<'EOF'
import socket
s = socket.create_connection(("127.0.0.1", 17091), timeout=5)
s.sendall(b"*1\r\n$4\r\nQUIT\r\n")
data = s.recv(64)
more = s.recv(64)
print(data.decode() .strip(), "closed" if more == b"" else "open")
EOF
)
[ "$R" = "+OK closed" ] && ok || bad "quit: $R"

# ---- cluster leg: RESP GET parks on the pull ---------------------------
rcall 17091 "SET${US}pulled${US}from-node1" > /dev/null
R=$(rcall 17092 "GET${US}pulled")
[ "$R" = "bulk:from-node1" ] && ok || bad "parked pull completion: $R"
R=$(rcall 17092 "GET${US}fleet-miss-xyz")
[ "$R" = "nil" ] && ok || bad "fleet miss completion: $R"
# and the pulled copy now serves locally (store semantics intact)
R=$(rcall 17092 "GET${US}pulled")
[ "$R" = "bulk:from-node1" ] && ok || bad "pulled copy local: $R"

# ---- INCR preserves the TTL (the Redis contract, local path) -----------
OUT3=$(python3 "$RC" 17091 <<EOF
SETEX${US}rl${US}300${US}5
INCR${US}rl
TTL${US}rl
INCR${US}fresh-ctr
TTL${US}fresh-ctr
EOF
)
L=$(printf '%s\n' "$OUT3" | sed -n 2p)
[ "$L" = "int:6" ] && ok || bad "incr over setex: $L"
L=$(printf '%s\n' "$OUT3" | sed -n 3p)
case "$L" in int:29[0-9]|int:300) ok;; *) bad "INCR lost the ttl: $L";; esac
L=$(printf '%s\n' "$OUT3" | sed -n 5p)
[ "$L" = "int:-1" ] && ok || bad "fresh counter not immortal: $L"

# ---- and through the forward plane (proxy col, sentinel on the wire) ---
# node1 places the key locally (self-preference); node2's INCR forwards
# with the preserve sentinel; node1 reads the surviving TTL as holder
rcall 17091 "SELECT${US}2" > /dev/null
OUT4=$(python3 "$RC" 17091 <<EOF
SELECT${US}2
SETEX${US}wct${US}300${US}41
EOF
)
L=$(printf '%s\n' "$OUT4" | sed -n 2p)
[ "$L" = "OK" ] && ok || bad "proxy setex: $L"
sleep 0.3
OUT5=$(python3 "$RC" 17092 <<EOF
SELECT${US}2
INCR${US}wct
EOF
)
L=$(printf '%s\n' "$OUT5" | sed -n 2p)
[ "$L" = "int:42" ] && ok || bad "forwarded INCR: $L"
OUT6=$(python3 "$RC" 17091 <<EOF
SELECT${US}2
TTL${US}wct
GET${US}wct
EOF
)
L=$(printf '%s\n' "$OUT6" | sed -n 2p)
case "$L" in int:29[0-9]|int:300) ok;; *) bad "forwarded INCR lost the ttl: $L";; esac
L=$(printf '%s\n' "$OUT6" | sed -n 3p)
[ "$L" = "bulk:42" ] && ok || bad "holder value after fwd incr: $L"

# ---- redis-cli interop (bonus leg: only where redis-cli exists) --------
if command -v redis-cli >/dev/null 2>&1; then
	R=$(redis-cli -p 17091 --no-raw PING 2>&1)
	[ "$R" = "PONG" ] && ok || bad "redis-cli ping: $R"
	R=$(redis-cli -p 17091 SET rk rv 2>&1)
	[ "$R" = "OK" ] && ok || bad "redis-cli set: $R"
	R=$(redis-cli -p 17091 GET rk 2>&1)
	[ "$R" = "rv" ] && ok || bad "redis-cli get: $R"
	R=$(redis-cli -p 17091 INCR rctr 2>&1)
	[ "$R" = "1" ] || [ "$R" = "(integer) 1" ] && ok || bad "redis-cli incr: $R"
else
	echo "redis-cli not installed: interop leg skipped"
fi

kill -TERM $P1 $P2 2>/dev/null; wait 2>/dev/null; P1= P2=
echo "resptest: $pass passed, $fail failed"
[ $fail -eq 0 ]
