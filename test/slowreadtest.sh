#!/bin/sh
# slowreadtest.sh - S372: a client whose pipelined replies outrun its
# socket is held back, not killed; one that reads nothing is closed.
#  1. a prompt reader pipelines 60 GETs of a 250 KB value (15 MB of
#     replies, past the 8 MB cap): it receives all 60, in order, and is
#     not closed - its requests waited while its replies drained
#     (clients.output.pauses moved).  Before S372 it was killed after 34.
#     The log says "throttling client <ip:port>" once, and its disconnect
#     line ends ", throttled N times".
#  2. a reader that reads nothing is closed after output_stall_s (3 s
#     here) as a slow consumer - logged so, counted in stall_kills
#  3. while it waits, another client on the same daemon is answered
#  4. no single reply reaches the cap: one is built in the reply scratch
#     (PC_MAX_REQ + 4 KB), so an MGET of 40 x 250 KB (10 MB) is answered
#     "-ERR reply too large" and the connection carries on.  After S372 the
#     8 MB cap is met only by pub/sub pushes (pushbatchtest).
# Runs on whichever backend the host gives (S361).
# Usage: test/slowreadtest.sh ./perfcached
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
D=$(mktemp -d /var/tmp/pcslow.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
ss -ltn 2>/dev/null | grep -qE ":1979[12][[:space:]]" && { echo "slowreadtest: port 19791/19792 already bound" >&2; exit 1; }
mkdir -p "$D/s"
cat > "$D/c.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s
output_stall_s = 3
[memory]
arena_mb = 64
[secrets]
client = slow-client-secret
[listen]
tcp = 127.0.0.1:19791
http = 127.0.0.1:19792
plaintext = loopback
[collection 0]
buckets_log2 = 10
C
chmod 600 "$D/c.conf"
"$BIN" -f "$D/c.conf" > "$D/c.log" 2>&1 & P=$!
i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/c.log"; do sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/c.log" || { echo "slowreadtest: the daemon did not start: $(tail -2 "$D/c.log")"; exit 1; }
out() { # out <python expr over o> - from /stats clients.output
	python3 -c 'import json,sys,urllib.request
o=json.load(urllib.request.urlopen("http://127.0.0.1:19792/stats", timeout=5))["clients"]["output"]
print(eval(sys.argv[1]))' "$1" 2>/dev/null
}
echo "  ..   io: $(python3 -c 'import json,urllib.request; print(json.load(urllib.request.urlopen("http://127.0.0.1:19792/stats"))["io"]["backend"]["active"])' 2>/dev/null || echo epoll)"
python3 - > "$D/py.out" 2>&1 <<'PY'
import socket, time
v = b"y" * 250000
s = socket.create_connection(("127.0.0.1", 19791), 5)
for k in [b"big"] + [b"m%d" % i for i in range(40)]:
    s.sendall(b"*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n" % (len(k), k, len(v), v))
    assert s.recv(16).startswith(b"+OK")
print("set ok")
rep = b"$250000\r\n" + v + b"\r\n"
# 1. a prompt reader: 60 x 250 KB pipelined, read as it comes
fast = socket.create_connection(("127.0.0.1", 19791), 20)
fast.sendall(b"*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n" * 60)
want, got = 60 * len(rep), b""
while len(got) < want:
    d = fast.recv(1 << 20)
    if not d:
        break
    got += d
print("fast", len(got) == want and got == rep * 60, len(got))
fast.sendall(b"*1\r\n$4\r\nPING\r\n")
print("fast-alive", fast.recv(16).startswith(b"+PONG"))
fast.close()
# 2./3. a reader that reads nothing; meanwhile another client is answered
slow = socket.socket(); slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
slow.connect(("127.0.0.1", 19791))
slow.sendall(b"*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n" * 60)
time.sleep(1)
t0 = time.time()
other = socket.create_connection(("127.0.0.1", 19791), 5)
other.sendall(b"*1\r\n$4\r\nPING\r\n")
print("other", other.recv(16).startswith(b"+PONG"), "%.3f" % (time.time() - t0))
other.close()
open("/dev/null", "w")
print("slow-waiting", flush=True)
time.sleep(6)                 # output_stall_s = 3, checked once a second
slow.close()
# 4. one reply larger than any reply may be: MGET of 40 x 250 KB = 10 MB
big = socket.create_connection(("127.0.0.1", 19791), 10)
f = big.makefile("rb")
big.sendall(b"*41\r\n$4\r\nMGET\r\n" + b"".join(b"$%d\r\nm%d\r\n" % (len(b"m%d" % i), i) for i in range(40)))
first = f.readline()
big.sendall(b"*1\r\n$4\r\nPING\r\n")
print("mget", first == b"-ERR reply too large\r\n", f.readline() == b"+PONG\r\n", first)
PY
sleep 1
grep -q "^fast True" "$D/py.out" && ok "1. a prompt reader got all 60 x 250 KB (15 MB) in order, past the 8 MB cap" ||
	bad "1. the prompt reader: $(grep '^fast' "$D/py.out" | head -1) $(grep -i error "$D/py.out" | head -2)"
grep -q "^fast-alive True" "$D/py.out" && ok "1. and was not closed - it answers a PING after" || bad "1. the prompt reader was closed"
# the log: the first throttle said once, the count in its disconnect line
TL=$(grep -c "throttling client 127.0.0.1:[0-9]*: [0-9]* MB of replies unread - its further requests wait until they drain" "$D/c.log")
[ "$TL" -ge 1 ] && ok "1. the log says it: $(grep -m1 'throttling client' "$D/c.log" | cut -d']' -f2- | cut -c2-)" ||
	bad "1. no 'throttling client' line in the log"
grep -qE "disconnected \(peer closed\) after [0-9]+s, 61 requests, throttled [0-9]+ times?" "$D/c.log" &&
	ok "1. its disconnect line counts the throttles: $(grep -E '61 requests, throttled' "$D/c.log" | grep -oE 'throttled [0-9]+ times?')" ||
	bad "1. its disconnect line: $(grep -E 'disconnected.*61 requests' "$D/c.log" | cut -d']' -f2-)"
[ "$(out 'o["pauses"]')" -ge 1 ] 2>/dev/null && ok "1. its requests waited while its replies drained (pauses $(out 'o["pauses"]'))" ||
	bad "1. no output pause was counted ($(out 'o'))"
grep -q "^other True" "$D/py.out" && ok "3. another client is answered while one waits ($(grep '^other' "$D/py.out" | cut -d' ' -f3) s)" ||
	bad "3. the other client: $(grep '^other' "$D/py.out")"
grep -q "disconnected (slow consumer: replies unread, no output progress within output_stall_s)" "$D/c.log" &&
	ok "2. the reader that read nothing was closed as a slow consumer, after output_stall_s" ||
	bad "2. the non-reader's close: $(grep -E 'disconnected' "$D/c.log" | cut -d']' -f2- | tr '\n' '|')"
[ "$(out 'o["stall_kills"]')" = 1 ] && ok "2. counted: stall_kills 1" || bad "2. stall_kills $(out 'o["stall_kills"]')"
[ "$(out 'o["paused"]')" = 0 ] && ok "no connection is left paused" || bad "paused now: $(out 'o["paused"]')"
grep -q "^mget True True" "$D/py.out" &&
	ok "4. a 10 MB MGET is answered -ERR reply too large, and the connection carries on" ||
	bad "4. the MGET: $(grep '^mget' "$D/py.out")"
e=$(grep -cE ' (ERROR|CRIT)' "$D/c.log")
[ "$e" = 0 ] && ok "no ERROR/CRIT" || bad "$e ERROR/CRIT: $(grep -E ' (ERROR|CRIT)' "$D/c.log" | head -2)"
echo "slowreadtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
