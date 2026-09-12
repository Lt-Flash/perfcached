#!/bin/sh
# reservetest.sh - S90: a departed member's binding is reserved.  A node
# that leaves cleanly keeps its id reserved on the master; a replacement
# at the same address inherits it; the identity, returning, gets it back.
# usage: test/reservetest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcrsv.XXXXXX)
P1= P2= P3=
trap '[ -n "$P1" ] && kill -9 $P1 2>/dev/null; [ -n "$P2" ] && kill -9 $P2 2>/dev/null; [ -n "$P3" ] && kill -9 $P3 2>/dev/null; rm -rf "$D"' EXIT TERM INT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
conf() { # conf <n> <port> <advertise-last-octet> <state_dir>
	mkdir -p "$4"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
state_dir = $4
[memory]
arena_mb = 64
[secrets]
client = rv-client-secret
cluster = rv-cluster-secret
[listen]
tcp = 127.0.1.9$3:$2
plaintext = loopback
[cluster]
multicast = 239.255.77.175:17275
advertise = 127.0.1.9$3
mode = store
collections = c
[collection c]
buckets_log2 = 10
EOF
}
start() { # start <n>
	: > "$D/n$1.log"
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$1=\$!"
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		eval "kill -0 \$P$1" 2>/dev/null || return 1
		sleep 0.1; i=$((i+1))
	done
	return 1
}
stop() { eval "kill -TERM \$P$1" 2>/dev/null; eval "wait \$P$1" 2>/dev/null; eval "P$1="; }
CLI="$D/cli.py"
cat > "$CLI" <<'EOF'
import json, socket, sys
port = int(sys.argv[1]); what = sys.argv[2]
s = socket.create_connection(("127.0.1.9" + sys.argv[3], port), timeout=8)
f = s.makefile("rwb")
f.write(json.dumps({"jsonrpc":"2.0","id":1,"method":"stats"}).encode()+b"\n"); f.flush()
c = json.loads(f.readline())["result"]["cluster"]
print(c["node"] if what == "node" else c.get("reserved_ids", "absent"))
EOF
node()     { python3 "$CLI" "$1" node "$2" 2>/dev/null; }
reserved() { python3 "$CLI" "$1" reserved "$2" 2>/dev/null; }
settle() { sleep 3; }
conf 1 18091 1 "$D/s1"; conf 2 18092 2 "$D/s2"
start 1 || { echo "node 1 did not start: $(tail -2 "$D/n1.log")"; exit 1; }
start 2 || { echo "node 2 did not start: $(tail -2 "$D/n2.log")"; exit 1; }
settle
ID2=$(node 18092 2); echo "  node 2 joined as id $ID2"
[ -n "$ID2" ] && [ "$ID2" != 0 ] && ok "two nodes formed a fleet" || bad "node 2 has no id"
stop 2; settle
R=$(reserved 18091 1)
[ "$R" = 1 ] && ok "a clean departure leaves one reserved id on the master" \
	|| bad "S90: reserved_ids on the master after a departure: '$R'"
grep -q "its id is reserved for it" "$D/n1.log" && ok "the master says so" || bad "S90: no reservation notice on the master"
# a replacement at the SAME address (fresh identity) inherits the id
conf 3 18092 2 "$D/s3"
start 3 || { bad "node 3 (the replacement) did not start: $(tail -2 "$D/n3.log")"; }
settle
ID3=$(node 18092 2); echo "  the replacement at node 2's address joined as id $ID3"
[ "$ID3" = "$ID2" ] && ok "a replacement at the same address inherits the id" \
	|| bad "S90: the replacement got id '$ID3', the departed had $ID2"
grep -q "returns as node $ID2 (reserved, matched by address)" "$D/n1.log" && ok "matched by address, as logged" \
	|| bad "S90: no 'matched by address' line: $(grep -E 'reserved|returns' "$D/n1.log" | tail -1)"
stop 3; settle
# the original identity returns and gets its id back
start 2 || { bad "node 2 did not restart: $(tail -2 "$D/n2.log")"; }
settle
ID2b=$(node 18092 2); echo "  node 2 returned as id $ID2b"
[ "$ID2b" = "$ID2" ] && ok "the returning identity is itself again" \
	|| bad "S90: the identity came back as '$ID2b', had $ID2"
R=$(reserved 18091 1)
[ "$R" = 0 ] && ok "the reservation was consumed" || bad "S90: reserved_ids after the return: '$R'"
echo "reservetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
