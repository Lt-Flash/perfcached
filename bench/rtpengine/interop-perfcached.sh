#!/bin/sh
# S29 proof obligation 3: does rtpengine work against perfcached?
# See README.md.  Assumes a perfcached reachable on 127.0.0.1:6499 with
# collections named "0" and "1" - either running locally, or tunnelled
# from the host that has the binary:
#     ssh -N -L 6499:127.0.0.1:6499 <daemon-host> &
# (a Redis client cannot speak Noise, so RESP needs a plaintext
# listener, and plaintext is loopback-only by policy).
set -u
D=$(cd "$(dirname "$0")" && pwd)
PP=6499
NG=22224
RTP_PID=
cleanup() { [ -n "$RTP_PID" ] && kill $RTP_PID 2>/dev/null; sleep 0.3; return 0; }
trap cleanup EXIT INT TERM

cat > "$D/rtp2.conf" <<EOF
[rtpengine]
table = -1
interface = 127.0.0.1
listen-ng = 127.0.0.1:$NG
port-min = 41100
port-max = 41120
foreground = true
log-stderr = true
log-level = 6
redis = 127.0.0.1:$PP/1
redis-write = 127.0.0.1:$PP/1
redis-expires = 86400
recording-dir = $D
EOF

start_rtp() {
	/usr/bin/rtpengine --config-file "$D/rtp2.conf" > "$D/$1" 2>&1 &
	RTP_PID=$!
	sleep 3
	kill -0 $RTP_PID 2>/dev/null
}
jrpc() {
	printf '%s\n' "$1" | timeout 5 python3 -c "
import socket, sys
s = socket.create_connection(('127.0.0.1', $PP), timeout=5)
s.sendall(sys.stdin.buffer.read())
print('   ', s.recv(65535).decode()[:280])"
}

echo "=== leg 1: rtpengine starts against perfcached ==="
if start_rtp rtp2a.log; then
	echo "PASS: rtpengine started"
else
	echo "FAIL: rtpengine did not start"
fi
grep -iE "redis" "$D/rtp2a.log" | head -6

echo "=== leg 2: a call writes its state ==="
python3 "$D/ngcall.py" $NG 2>&1 | sed 's/^/    /'
sleep 1.5
echo "--- keys perfcached holds in collection 1 ---"
jrpc '{"jsonrpc":"2.0","id":1,"method":"keys","params":{"col":"1"}}'
echo "--- its ttl (rtpengine set 86400) ---"
jrpc '{"jsonrpc":"2.0","id":2,"method":"ttl","params":{"col":"1","key":"s29-capture-call-1"}}'

echo "=== leg 3: restart rtpengine - restore from perfcached? ==="
kill $RTP_PID 2>/dev/null; wait $RTP_PID 2>/dev/null; RTP_PID=
sleep 1
if start_rtp rtp2b.log; then
	echo "rtpengine restarted"
else
	echo "FAIL: did not restart"
fi
grep -iE "restor|redis|call" "$D/rtp2b.log" | head -10
echo "--- calls rtpengine now knows about ---"
python3 "$D/nglist.py" $NG 2>&1 | sed 's/^/    /'
cleanup
