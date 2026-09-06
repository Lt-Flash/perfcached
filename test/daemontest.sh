#!/bin/sh
# daemontest.sh — S6 verification: the daemon starts, accepts on TCP and
# UNIX listeners from every worker, spawns the expected thread set,
# refuses a second instance, and shuts down cleanly on SIGTERM.
# Red checks prove the probes are not vacuous. Usage: test/daemontest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d)
PID= P4=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; \
     [ -n "$P4" ] && kill -9 $P4 2>/dev/null; rm -rf "$D"' EXIT
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

cat > "$D/s6.conf" <<EOF
[daemon]
workers = 3
log_level = info
[memory]
arena_mb = 64
[secrets]
client = smoke-client-secret
cluster = smoke-cluster-secret
[listen]
tcp = 127.0.0.1:16479
unix = $D/pc.sock
plaintext = loopback
[collection th]
buckets_log2 = 12
EOF

"$BIN" -f "$D/s6.conf" > "$D/log" 2>&1 &
PID=$!

i=0
while [ $i -lt 50 ]; do
	grep -q "perfcached ready" "$D/log" && break
	kill -0 $PID 2>/dev/null || break
	sleep 0.1; i=$((i+1))
done
if grep -q "perfcached ready" "$D/log"; then ok; else
	bad "daemon never became ready"; cat "$D/log"; exit 1; fi

# TCP accept + a live ping round-trip (S7 contract: connections persist)
if python3 - <<'EOF'
import json, socket, sys
s = socket.create_connection(("127.0.0.1", 16479), timeout=2)
s.sendall(b'{"jsonrpc":"2.0","id":1,"method":"ping"}\n')
r = json.loads(s.makefile().readline())
sys.exit(0 if r["result"]["pong"] is True else 1)
EOF
then ok; else bad "tcp ping on 16479"; fi

# RED: an unconfigured port must refuse - proves the probe above is real
if timeout 2 bash -c 'exec 3<>/dev/tcp/127.0.0.1/16480' 2>/dev/null; then
	bad "port 16480 accepted a connection (vacuous probe?)"
else ok; fi

# UNIX accept + ping round-trip
if python3 - "$D/pc.sock" <<'EOF'
import json, socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(2); s.connect(sys.argv[1])
s.sendall(b'{"jsonrpc":"2.0","id":1,"method":"ping"}\n')
r = json.loads(s.makefile().readline())
sys.exit(0 if r["result"]["pong"] is True else 1)
EOF
then ok; else bad "unix ping"; fi

# thread census: main + 3 workers + maintenance + WAL stub = 6
NT=$(ls /proc/$PID/task | wc -l)
if [ "$NT" -eq 6 ]; then ok; else bad "thread count $NT != 6"; fi

# RED: a second instance must refuse (live unix socket detection)
if "$BIN" -f "$D/s6.conf" > "$D/log2" 2>&1; then
	bad "second instance did not refuse"
else
	if grep -q "another perfcached is running" "$D/log2"; then ok
	else bad "second instance failed for the wrong reason"; cat "$D/log2"; fi
fi

# RED: a second instance sharing only the TCP PORT must also refuse.
# The check above probes the unix socket, so it sees nothing when the
# two differ - and TCP listeners set SO_REUSEPORT, so the kernel does
# not refuse either: the second daemon would silently join the port and
# answer a share of the connections with its own data and its own
# cluster view.  This is the door that catches that.
cat > "$D/samesock.conf" <<EOF
[daemon]
workers = 1
log_level = info
[memory]
arena_mb = 32
[secrets]
client = smoke-client-secret
cluster = smoke-cluster-secret
[listen]
tcp = 127.0.0.1:16479
unix = $D/other.sock
plaintext = loopback
[collection th]
buckets_log2 = 10
EOF
# under `timeout`: if the check ever regresses, the second daemon STARTS
# and runs forever, and a foreground run would hang the suite instead of
# failing it.  timeout turns that into a non-zero exit and a red test.
if timeout 10 "$BIN" -f "$D/samesock.conf" > "$D/log3" 2>&1; then
	bad "a second daemon on the same TCP port exited 0 instead of refusing"
elif grep -q "perfcached ready" "$D/log3"; then
	bad "a second daemon STARTED on a port already in use - SO_REUSEPORT"
	bad "  means it would answer a share of the connections, with its own"
	bad "  data and its own cluster view, and log nothing about it"
elif grep -q "already in use by another process" "$D/log3"; then
	ok
else
	bad "same-port instance refused, but not for the port:"
	tail -2 "$D/log3"
fi
# GREEN: a different port on the same host must still start, or the
# check above would be refusing everything
sed 's/16479/16480/; s|other.sock|third.sock|' "$D/samesock.conf" \
	> "$D/otherport.conf"
"$BIN" -f "$D/otherport.conf" > "$D/log4" 2>&1 &
P4=$!
i=0
while [ $i -lt 60 ]; do
	grep -q "perfcached ready" "$D/log4" && break
	sleep 0.1; i=$((i+1))
done
if grep -q "perfcached ready" "$D/log4"; then ok
else bad "a daemon on a free port was refused"; tail -2 "$D/log4"; fi
kill -9 $P4 2>/dev/null

# clean shutdown on SIGTERM
kill -TERM $PID
i=0; rc=-1
while [ $i -lt 50 ]; do
	if ! kill -0 $PID 2>/dev/null; then wait $PID; rc=$?; break; fi
	sleep 0.1; i=$((i+1))
done
if [ "$rc" -eq 0 ] && grep -q "clean shutdown" "$D/log"; then ok
else bad "shutdown rc=$rc"; tail -5 "$D/log"; fi
[ ! -e "$D/pc.sock" ] && ok || bad "unix socket not removed on shutdown"
PID=

echo "daemontest: $pass passed, $fail failed"
[ $fail -eq 0 ]
