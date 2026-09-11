#!/bin/sh
# resplistener.sh — the dedicated RESP listener (task S33): a plaintext
# door for LAN Redis clients that is NARROWER than the native one.
# Asserted:
#  - a non-loopback RESP listener REFUSES TO START without resp_allow
#    (the dialect has no handshake, so the allow-list is the door);
#  - with one, it starts and serves RESP off-box;
#  - DIALECT PINNING: JSON-RPC and binary frames are refused on the
#    RESP listener - so the native verb surface, admin verbs included,
#    is unreachable from it - while the native listener is unchanged;
#  - the allow-list actually rejects a peer outside the CIDR;
#  - AUTH: NOAUTH before, WRONGPASS on a bad password, OK on the right
#    one, PING gated too (Redis requirepass behaviour);
#  - resp_collections scopes which collections a RESP client can see;
#  - MANY CLIENTS AT ONCE: 200 concurrent connections all serve.
# Usage: test/resplistener.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrl.XXXXXX)
P1=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

# the host's own LAN address - the listener must be non-loopback for the
# allow-list rules to apply at all
LANIP=$(ip -4 -o addr show scope global 2>/dev/null \
	| awk '{print $4}' | cut -d/ -f1 | head -1)
[ -n "$LANIP" ] || { echo "no global IPv4 address: skipping"; exit 0; }
LANNET=$(echo "$LANIP" | cut -d. -f1-3).0/24

mkconf() { # mkconf <file> <listen-block> [secrets-lines...]
	f=$1; rblock=$2; shift 2
	cat > "$f" <<EOF
[daemon]
workers = 2
log_level = info
[memory]
arena_mb = 64
[secrets]
client = rl-client-secret
cluster = rl-cluster-secret
$(printf '%s\n' "$@")
[listen]
tcp = 127.0.0.1:17101
plaintext = loopback
$rblock
[collection 0]
buckets_log2 = 10
[collection 1]
buckets_log2 = 10
EOF
}

start() { # start <conf>; 0 = came up
	"$BIN" -f "$1" > "$D/log" 2>&1 &
	P1=$!
	i=0
	while [ $i -lt 60 ]; do
		grep -q "perfcached ready" "$D/log" && return 0
		kill -0 $P1 2>/dev/null || return 1
		sleep 0.1; i=$((i+1))
	done
	return 1
}
stop() { [ -n "$P1" ] && kill -TERM $P1 2>/dev/null; wait 2>/dev/null; P1=; }

RC="$D/r.py"
cat > "$RC" <<'EOF'
import socket, sys
host, port = sys.argv[1], int(sys.argv[2])
s = socket.create_connection((host, port), timeout=6)
f = s.makefile("rwb")
def rd():
    line = f.readline()
    if not line:
        return "<closed>"
    t, body = chr(line[0]), line[1:-2].decode("latin1")
    if t in "+-": return body
    if t == ":": return "int:" + body
    if t == "$":
        n = int(body)
        if n < 0: return "nil"
        return "bulk:" + f.read(n + 2)[:-2].decode("latin1")
    if t == "*":
        n = int(body)
        return "arr:[" + "|".join(rd() for _ in range(n)) + "]"
    return "?" + t + body
for line in sys.stdin:
    line = line.rstrip("\n")
    if not line: continue
    args = [a.encode("latin1") for a in line.split("\x1f")]
    out = b"*%d\r\n" % len(args)
    for a in args:
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    f.write(out); f.flush()
    print(rd())
EOF
US=$(printf '\037')
rc() { printf '%s\n' "$3" | python3 "$RC" "$1" "$2"; }

# ---- 1. a LAN RESP listener without an allow-list must REFUSE --------
mkconf "$D/noallow.conf" "resp = $LANIP:17102"
if start "$D/noallow.conf"; then
	bad "started a LAN RESP listener with no resp_allow"
	stop
else
	grep -q "resp_allow" "$D/log" && ok \
		|| bad "refused, but not for the allow-list reason: $(tail -2 "$D/log")"
fi

# ---- 2. with an allow-list it starts and serves ---------------------
mkconf "$D/ok.conf" "resp = $LANIP:17102
resp_allow = $LANNET"
start "$D/ok.conf" || { echo "did not start:"; cat "$D/log"; exit 1; }
ok
R=$(rc "$LANIP" 17102 "PING")
[ "$R" = "PONG" ] && ok || bad "RESP over the LAN listener: $R"
R=$(rc "$LANIP" 17102 "SET${US}k1${US}v1")
[ "$R" = "OK" ] && ok || bad "SET on the RESP listener: $R"
R=$(rc "$LANIP" 17102 "GET${US}k1")
[ "$R" = "bulk:v1" ] && ok || bad "GET on the RESP listener: $R"

# ---- 3. DIALECT PINNING: no JSON-RPC, no binary, no admin -----------
R=$(timeout 5 python3 - "$LANIP" <<'EOF'
import socket, sys
s = socket.create_connection((sys.argv[1], 17102), timeout=5)
s.sendall(b'{"jsonrpc":"2.0","id":1,"method":"stats"}\n')
try:
    print("closed" if s.recv(256) == b"" else "SERVED")
except ConnectionResetError:
    print("closed")
EOF
)
[ "$R" = "closed" ] && ok || bad "JSON-RPC served on the RESP listener: $R"
R=$(timeout 5 python3 - "$LANIP" <<'EOF'
import socket, sys
s = socket.create_connection((sys.argv[1], 17102), timeout=5)
s.sendall(b"\x9e\x01\x00\x00" + b"\x00" * 12)
try:
    print("closed" if s.recv(256) == b"" else "SERVED")
except ConnectionResetError:
    print("closed")
EOF
)
[ "$R" = "closed" ] && ok || bad "binary frame served on the RESP listener: $R"
# the native listener is untouched
R=$(printf '{"jsonrpc":"2.0","id":1,"method":"ping"}\n' | timeout 5 python3 -c '
import socket,sys
s=socket.create_connection(("127.0.0.1",17101),timeout=5)
s.sendall(sys.stdin.buffer.read()); print(s.recv(4096).decode().strip())')
echo "$R" | grep -q '"pong"' && ok || bad "native listener broke: $R"
# admin verbs are simply not in the RESP vocabulary
R=$(rc "$LANIP" 17102 "SAVE")
case "$R" in "ERR unknown command"*) ok;; *) bad "SAVE not refused: $R";; esac
stop

# ---- 4. the allow-list actually rejects ------------------------------
mkconf "$D/deny.conf" "resp = $LANIP:17102
resp_allow = 10.99.99.0/24"
start "$D/deny.conf" || { echo "deny cfg did not start"; cat "$D/log"; exit 1; }
R=$(timeout 5 python3 - "$LANIP" <<'EOF'
import socket, sys
try:
    s = socket.create_connection((sys.argv[1], 17102), timeout=4)
    s.sendall(b"*1\r\n$4\r\nPING\r\n")
    print("closed" if s.recv(64) == b"" else "SERVED")
except (ConnectionResetError, ConnectionRefusedError, socket.timeout):
    print("closed")
EOF
)
[ "$R" = "closed" ] && ok || bad "peer outside resp_allow was served: $R"
RJ=$(printf '{"jsonrpc":"2.0","id":1,"method":"stats"}\n' | timeout 5 python3 -c '
import json,socket,sys
s=socket.create_connection(("127.0.0.1",17101),timeout=5)
s.sendall(sys.stdin.buffer.read())
print(json.loads(s.recv(200000))["result"]["resp"]["rejected"])' 2>/dev/null)
[ "${RJ:-0}" -ge 1 ] 2>/dev/null && ok || bad "resp_rejected did not count: $RJ"
stop

# ---- 5. AUTH ---------------------------------------------------------
mkconf "$D/auth.conf" "resp = $LANIP:17102
resp_allow = $LANNET" "resp = rl-resp-password"
start "$D/auth.conf" || { echo "auth cfg did not start"; cat "$D/log"; exit 1; }
OUT=$(python3 "$RC" "$LANIP" 17102 <<EOF
PING
GET${US}k1
AUTH${US}wrong-one
AUTH${US}rl-resp-password
PING
SET${US}ak${US}av
GET${US}ak
EOF
)
L() { printf '%s\n' "$OUT" | sed -n "${1}p"; }
case "$(L 1)" in NOAUTH*) ok;; *) bad "PING not gated: $(L 1)";; esac
case "$(L 2)" in NOAUTH*) ok;; *) bad "GET not gated: $(L 2)";; esac
case "$(L 3)" in WRONGPASS*) ok;; *) bad "bad password: $(L 3)";; esac
[ "$(L 4)" = "OK" ] && ok || bad "good password: $(L 4)"
[ "$(L 5)" = "PONG" ] && ok || bad "PING after AUTH: $(L 5)"
[ "$(L 7)" = "bulk:av" ] && ok || bad "data after AUTH: $(L 7)"
AF=$(printf '{"jsonrpc":"2.0","id":1,"method":"stats"}\n' | timeout 5 python3 -c '
import json,socket,sys
s=socket.create_connection(("127.0.0.1",17101),timeout=5)
s.sendall(sys.stdin.buffer.read())
print(json.loads(s.recv(200000))["result"]["resp"]["authfail"])' 2>/dev/null)
[ "${AF:-0}" -ge 1 ] 2>/dev/null && ok || bad "resp_authfail did not count: $AF"
stop

# ---- 6. collection scoping -------------------------------------------
mkconf "$D/scope.conf" "resp = $LANIP:17102
resp_allow = $LANNET
resp_collections = 0"
start "$D/scope.conf" || { echo "scope cfg did not start"; cat "$D/log"; exit 1; }
OUT=$(python3 "$RC" "$LANIP" 17102 <<EOF
SELECT${US}0
SELECT${US}1
EOF
)
[ "$(printf '%s\n' "$OUT" | sed -n 1p)" = "OK" ] && ok || bad "SELECT 0 refused"
case "$(printf '%s\n' "$OUT" | sed -n 2p)" in
	"ERR DB index is out of range"*) ok;;
	*) bad "SELECT 1 not scoped out: $(printf '%s\n' "$OUT" | sed -n 2p)";;
esac
stop

# ---- 7. MANY CLIENTS AT ONCE (the point of the task) -----------------
mkconf "$D/many.conf" "resp = $LANIP:17102
resp_allow = $LANNET"
start "$D/many.conf" || { echo "many cfg did not start"; cat "$D/log"; exit 1; }
R=$(timeout 120 python3 - "$LANIP" <<'EOF'
import socket, sys
HOST, N = sys.argv[1], 200
socks = []
try:
    for i in range(N):
        s = socket.create_connection((HOST, 17102), timeout=10)
        s.sendall(b"*3\r\n$3\r\nSET\r\n$6\r\nmk%04d\r\n$5\r\nv%04d\r\n"
                  % (i, i))
        socks.append(s)
    okc = sum(1 for s in socks if s.recv(64) == b"+OK\r\n")
    # every connection still usable afterwards
    for i, s in enumerate(socks):
        s.sendall(b"*2\r\n$3\r\nGET\r\n$6\r\nmk%04d\r\n" % i)
    good = 0
    for i, s in enumerate(socks):
        if s.recv(128) == b"$5\r\nv%04d\r\n" % i:
            good += 1
    print("%d %d" % (okc, good))
finally:
    for s in socks:
        s.close()
EOF
)
set -- $R
[ "${1:-0}" -eq 200 ] && ok || bad "200 concurrent SETs: ${1:-none} stored"
[ "${2:-0}" -eq 200 ] && ok || bad "200 concurrent GETs: ${2:-none} correct"
CN=$(printf '{"jsonrpc":"2.0","id":1,"method":"stats"}\n' | timeout 5 python3 -c '
import json,socket,sys
s=socket.create_connection(("127.0.0.1",17101),timeout=5)
s.sendall(sys.stdin.buffer.read())
print(json.loads(s.recv(200000))["result"]["resp"]["conns"])' 2>/dev/null)
[ "${CN:-0}" -ge 200 ] 2>/dev/null && ok || bad "resp_conns counted $CN of 200+"
stop

echo "resplistener: $pass passed, $fail failed"
[ $fail -eq 0 ]
