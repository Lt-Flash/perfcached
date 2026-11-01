#!/bin/sh
# bulkgatetest.sh - CH-11: the bulk plane serves one connection at a time,
# so it reads only from a member's host and wants the opening message
# within a second.
#  - control: a joiner on another loopback address gets a 60,000-byte
#    record, which only the bulk plane carries (its client binds its own
#    host, which the gate expects);
#  - a connection from 127.0.0.1, no member's host, is closed at once and
#    the log says so;
#  - an idle connection from a member's host is closed within ~1 s, not
#    held for the 5 s receive timeout with the plane queued behind it.
# Fail-first: before CH-11 both connections are held about 5 s.
# Usage: test/bulkgatetest.sh [./perfcached] [./perfcli]
set -u
BIN=${1:-./perfcached}
CLI=${2:-./perfcli}
SEC=bg-client-secret
D=$(mktemp -d /var/tmp/pcbg.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; sleep 0.2; for p in $(pgrep -x perfcached 2>/dev/null); do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 "$p"; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -x "$CLI" ] || { echo "bulkgatetest: $CLI is missing (make perfcli)"; exit 1; }

PORT=24760                  # the cluster port: the clients are 2476<n>, n >= 1
# node <n>: 127.0.76.<n>:2476<n>; the bulk listener is <host>:$PORT
conf() {
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SEC
cluster = bg-cluster-secret
[listen]
tcp = 127.0.76.$1:2476$1
[cluster]
multicast = 239.255.81.11:$PORT
advertise = 127.0.76.$1
pull_timeout_ms = 400
mode = eager
collections = c
[collection c]
buckets_log2 = 10
EOF
	chmod 600 "$D/n$1.conf"
}
wait_ready() {
	i=0
	while [ $i -lt 300 ]; do
		grep -q "perfcached ready" "$1" 2>/dev/null && return 0
		sleep 0.1; i=$((i + 1))
	done
	echo "no daemon: $(tail -2 "$1" | tr '\n' ' ')"; return 1
}
# 200 keys and one of 60,000 bytes - over the datagram ceiling, so only the
# bulk plane carries it; small ones a joiner gets by the push backfill first
fill() {
	python3 -c 'import json
for i in range(200): print(json.dumps({"method": "set", "params": {"col": "c", "key": "k%d" % i, "value": "v%d" % i}}))
print(json.dumps({"method": "set", "params": {"col": "c", "key": "big", "value": "x" * 60000}}))' |
		"$CLI" -h 127.0.76.2 -p 24762 -a "$SEC" -q 2>/dev/null | grep -c .
}
entries() { # entries <n>: collection c's records on node n (no read, so no pull)
	printf '{"method":"stats"}\n' | "$CLI" -h 127.0.76.$1 -p 2476$1 -a "$SEC" -q 2>/dev/null |
		python3 -c 'import json,sys
try:
    r = json.loads(sys.stdin.readline()); r = r.get("result", r)
    print([c for c in r["collections"] if c["name"] == "c"][0]["entries"])
except Exception: print(-1)'
}
# probe <source host>: how the bulk listener of node 2 treats an idle connection
probe() {
	python3 - "$1" "$PORT" <<'EOF'
import socket, sys, time
s = socket.socket(); s.bind((sys.argv[1], 0)); s.settimeout(10)
t = time.time(); s.connect(("127.0.76.2", int(sys.argv[2])))
try:
    r = "eof" if s.recv(1) == b"" else "data"
except ConnectionResetError:
    r = "rst"
except socket.timeout:
    r = "held"
print("%s %d" % (r, int((time.time() - t) * 1000)))
EOF
}

for n in 1 2; do conf $n; done
"$BIN" -f "$D/n2.conf" > "$D/n2.log" 2>&1 &
PIDS="$PIDS $!"
wait_ready "$D/n2.log" || { bad "the founder did not start"; echo "bulkgatetest: $pass passed, $fail failed"; exit 1; }
N=$(fill)
"$BIN" -f "$D/n1.conf" > "$D/n1.log" 2>&1 &
PIDS="$PIDS $!"
wait_ready "$D/n1.log" || { bad "the joiner did not start"; echo "bulkgatetest: $pass passed, $fail failed"; exit 1; }
i=0
while [ $i -lt 120 ]; do
	[ "$(entries 1)" = 201 ] && break
	sleep 0.5; i=$((i + 1))
done
[ "$N" = 201 ] && [ "$(entries 1)" = 201 ] \
	&& ok "control: the joiner at 127.0.76.1 holds all 201 records, the 60,000-byte one included - only the bulk plane carries it, between two loopback aliases" \
	|| bad "the joiner holds $(entries 1) of $N records in 60 s (the big one only crosses on the bulk plane): joiner: $(grep 'bootstrap\|bulk' "$D/n1.log" | tail -3 | sed 's/.*cluster: //' | cut -c1-110 | tr '\n' '|') founder: $(grep 'bulk\|bootstrap' "$D/n2.log" | tail -3 | sed 's/.*cluster: //' | cut -c1-110 | tr '\n' '|')"

R=$(probe 127.0.0.1)
case "$R" in eof*|rst*) [ "${R##* }" -le 600 ] && ok "a connection from 127.0.0.1, no member's host, is closed in ${R##* } ms ($R)" \
	|| bad "the unknown host's connection closed only after ${R##* } ms";;
*) bad "the unknown host's connection: $R";; esac
grep -q "a bulk connection from 127.0.0.1, which is no member's host - closed" "$D/n2.log" \
	&& ok "and the founder's log says why" || bad "no log line for the refused host"

R=$(probe 127.0.76.1)
case "$R" in eof*|rst*) [ "${R##* }" -ge 700 ] && [ "${R##* }" -le 2500 ] \
	&& ok "an idle connection from a member's host is closed after ${R##* } ms - the opening message's second, not the 5 s receive timeout" \
	|| bad "the member host's idle connection closed after ${R##* } ms (want 0.7-2.5 s)";;
*) bad "the member host's idle connection: $R";; esac

echo "bulkgatetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
