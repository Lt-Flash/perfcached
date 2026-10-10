#!/bin/sh
# walringsizetest.sh - S266: a WAL ring's size follows what its thread
# writes.
#
# Every registered thread had a ring of ring_kb, and since S265 every one
# is resident: on the test fleet (ring_kb 16384, 4 workers) 10 rings and
# the stage buffer made 164 MB per node.  Measured over 13 WAL+cluster
# suites, only the workers, the peer thread and (for non-replica
# migrations) the bulk thread write; main, maintenance, the WAL pump, RDB
# and the heartbeat never do.  They now get min(ring_kb, 512 KB) - two
# maximum records - and the rest keep ring_kb.  Unclustered, the peer and
# bulk threads do not exist.
#
# wal.buffer_bytes is asserted EXACTLY, 2 workers (8 rings):
#   1. standalone, ring_kb 4096: 2 x 4 MB + 6 x 512 KB + 4 MB stage = 15 MB
#   2. a 1-node cluster, ring_kb 4096: 4 x 4 MB + 4 x 512 KB + 4 MB = 22 MB
#   3. a 1-node cluster, ring_kb 256: the small size never exceeds
#      ring_kb, so 8 x 256 KB + 4 MB = 6 MB
# and in each, a write reaches the WAL and the startup line says it.
# The standalone EDITION refuses a [cluster] section, so there only case 1
# runs and 2-3 are SKIPPED, loudly.
# Fail-first: the build before S266 has 8 x ring_kb in cases 1 and 2
# (36 MB).
# Usage: test/walringsizetest.sh [./perfcached]
set -u
PYTHONPATH=$(cd "$(dirname "$0")" && pwd)${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
D=
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; [ -n "$D" ] && rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0 skip=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
skp() { skip=$((skip+1)); echo "  SKIP $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 18721 18722; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "walringsizetest: port $p busy" >&2; exit 1; }
done
SA=; "$BIN" -V 2>/dev/null | grep -q standalone && SA=1
MB=1048576

run() { # run <label> <ring_kb> <clustered 0|1> <expected bytes>
	D=$(mktemp -d /var/tmp/pcwrs.XXXXXX)
	mkdir -p "$D/wal"
	CL=
	[ "$3" = 1 ] && CL="[cluster]
multicast = 239.255.78.9:18725
advertise = 127.0.26.1
mode = eager
collections = 0"
	cat > "$D/n.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D
[memory]
arena_mb = 16
[secrets]
client = wrs-client-secret
cluster = wrs-cluster-secret
[listen]
tcp = 127.0.0.1:18721
http = 127.0.0.1:18722
plaintext = loopback
$CL
[collection 0]
buckets_log2 = 10
[wal]
dir = $D/wal
probe = no
fsync = everysec
ring_kb = $2
segment_mb = 8
segments = 4
save = off
C
	chmod 600 "$D/n.conf"
	"$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
	P=$!
	i=0; while [ $i -lt 100 ]; do grep -q "perfcached ready" "$D/n.log" && break; sleep 0.1; i=$((i+1)); done
	if ! grep -q "perfcached ready" "$D/n.log"; then
		bad "$1: did not start: $(tail -2 "$D/n.log" | tr '\n' ' ')"
	else
		GOT=$(curl -s "http://127.0.0.1:18722/stats" | python3 -c 'import json,sys; print(json.load(sys.stdin)["wal"]["buffer_bytes"])' 2>/dev/null)
		[ "$GOT" = "$4" ] \
			&& ok "$1: wal.buffer_bytes $GOT ($(($4 / MB)) MB), as the thread roles say" \
			|| bad "$1: wal.buffer_bytes ${GOT:-?}, want $4 ($(($4 / MB)) MB)"
		L=$(grep -o "wal: [0-9]* ring(s) of [0-9]* KB for the threads that write, [0-9]* of [0-9]* KB for the rest" "$D/n.log" | head -1)
		[ -n "$L" ] && ok "$1: the node says so: $L" || bad "$1: no startup line naming the ring sizes"
		# a write still reaches the WAL
		B0=$(curl -s "http://127.0.0.1:18722/stats" | python3 -c 'import json,sys; print(json.load(sys.stdin)["wal"]["bytes"])' 2>/dev/null)
		printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"set","params":{"col":"0","key":"k","value":"v"}}' | \
			timeout 5 python3 -c 'import pcnative,socket,sys; s=socket.create_connection(("127.0.0.1",18721),5); f=pcnative.wrap(s); f.write(sys.stdin.buffer.read()); f.flush(); f.readline()'
		sleep 1.5
		B1=$(curl -s "http://127.0.0.1:18722/stats" | python3 -c 'import json,sys; print(json.load(sys.stdin)["wal"]["bytes"])' 2>/dev/null)
		[ "${B1:-0}" -gt "${B0:-0}" ] 2>/dev/null \
			&& ok "$1: a write reaches the WAL ($B0 -> $B1 bytes)" \
			|| bad "$1: the WAL did not grow on a write ($B0 -> $B1)"
	fi
	kill $P 2>/dev/null; wait $P 2>/dev/null; P=
	rm -rf "$D"; D=
}

S4=$((4096 * 1024)); SM=$((512 * 1024)); STAGE=$((4 * MB))
run "standalone, ring_kb 4096" 4096 0 $((2 * S4 + 6 * SM + STAGE))
if [ -n "$SA" ]; then
	skp "1-node cluster cases: the standalone edition refuses a [cluster] section"
else
	run "1-node cluster, ring_kb 4096" 4096 1 $((4 * S4 + 4 * SM + STAGE))
	run "1-node cluster, ring_kb 256" 256 1 $((8 * 256 * 1024 + STAGE))
fi
echo "walringsizetest: $pass passed, $fail failed, $skip skipped"
[ $fail -eq 0 ]
