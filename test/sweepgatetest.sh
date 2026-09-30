#!/bin/sh
# sweepgatetest.sh - S260: the repair sweep compares before it re-sends.
#
# The sweep re-sent every record a node authored above a peer's watermark
# about ten seconds after the write-path push had delivered it: on a
# 3-node spread fleet after 1M writes the holders refused 336k and 333k
# copies as older - the sweep doubled their apply work after every burst.
# Now an ordinary cycle digests its slots, asks the peer which differ, and
# walks only those.  Per mode (eager; spread K=2), three nodes:
#   1. 3,000 keys through node 1, then two sweep cycles: the peers refuse
#      almost none as older (under 5%), node 1 reports rounds answered
#      clean, and every key is where placement puts it;
#   2. node 3 cut off from node 1 while 500 more keys go through node 1
#      (the pushes to it are lost), then healed: node 3 holds all its keys
#      within a few cycles - the gate must still send what differs.
# Fail-first: the build before S260 has the peers refuse ~3,000 each.
# Usage: test/sweepgatetest.sh [./perfcached] [./netcutshim.so]
set -u
BIN=${1:-./perfcached}
SHIM=$(cd "$(dirname "${2:-./netcutshim.so}")" && pwd)/$(basename "${2:-./netcutshim.so}")
D=
trap '[ -n "$D" ] && { kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"; }' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 18881 18882 18883 18891 18892 18893; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "sweepgatetest: port $p busy" >&2; exit 1; }
done
[ -f "$SHIM" ] || { echo "sweepgatetest: no shim at $SHIM" >&2; exit 1; }
# a dynamic sanitizer runtime must come first in the preload list
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
PRELOAD="${SANRT:+$SANRT }$SHIM"

conf() { # conf <n> <cluster lines> <collection lines>
	mkdir -p "$D/s$1/wal"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = sg-client-secret
cluster = sg-cluster-secret
[listen]
tcp = 127.0.0.1:1888$1
http = 127.0.0.1:1889$1
plaintext = loopback
[cluster]
multicast = 239.255.78.4:18885
advertise = 127.0.22.$1
$2
[collection c]
buckets_log2 = 12
autoscale = off
$3
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
	: > "$D/n$1.log"
	LD_PRELOAD="$PRELOAD" PC_NETCUT_CTL="$D/cut$1" PC_NETCUT_LOG="$D/cutlog$1" \
		"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 150 ]; do grep -q "perfcached ready" "$D/n$1.log" && return 0; sleep 0.1; i=$((i+1)); done
	echo "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; return 1
}
st() { curl -s "http://127.0.0.1:1889$1/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin); x = r["cluster"]; c = [y for y in r["collections"] if y["name"] == "c"][0]
print(eval(sys.argv[1]))' "$2" 2>/dev/null; }
ready3() {
	i=0; while [ $i -lt 120 ]; do
		[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ] && \
			[ "$(st 1 'sum(1 for p in x["peers"] if p.get("up"))')$(st 2 'sum(1 for p in x["peers"] if p.get("up"))')$(st 3 'sum(1 for p in x["peers"] if p.get("up"))')" = 222 ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}
fill() { python3 - "18881" "$1" "$2" <<'PY'
import json, socket, sys
port, a, b = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
f = socket.create_connection(("127.0.0.1", port), timeout=30).makefile("rwb")
ok = 0
for x in range(a, b, 500):
    for i in range(x, min(b, x + 500)):
        f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": "set", "params": {"col": "c", "key": "k%05d" % i, "value": "v%05d" % i}}) + "\n").encode())
    f.flush()
    for i in range(x, min(b, x + 500)):
        ok += 1 if (json.loads(f.readline()).get("result") or {}).get("stored") else 0
print(ok)
PY
}
older() { st $1 'x.get("recv_older", 0)'; }
gate() { st 1 'json.dumps(x.get("sweep_gate"))'; }
ent() { st $1 'c["entries"]'; }
# short <copies> <keys>: how many of k00000.. are on fewer than <copies>
# nodes - each node's own keys by its node-scoped scan.  Surplus copies (a
# spread node keeping one a holder never acknowledged, S253) are allowed;
# a key short of its copies is not.
short() { python3 - "$1" "$2" <<'PY'
import json, socket, sys
want, n = int(sys.argv[1]), int(sys.argv[2])
seen = {}
for port in (18881, 18882, 18883):
    f = socket.create_connection(("127.0.0.1", port), timeout=30).makefile("rwb")
    cur = 0
    while True:
        f.write((json.dumps({"jsonrpc": "2.0", "id": 1, "method": "scan", "params": {"col": "c", "cursor": cur, "count": 512}}) + "\n").encode()); f.flush()
        r = json.loads(f.readline())["result"]
        for it in r["items"]:
            k = it["k"] if isinstance(it, dict) else it
            seen[k] = seen.get(k, 0) + 1
        cur = r["cursor"]
        if not cur:
            break
print(sum(1 for i in range(n) if seen.get("k%05d" % i, 0) < want))
PY
}

run() { # run <label> <copies> <cluster lines> <collection lines>
	D=$(mktemp -d /var/tmp/pcsweepgate.XXXXXX)
	echo "--- $1"
	for n in 1 2 3; do conf $n "$3" "$4"; done
	for n in 1 2 3; do start $n || { bad "$1: node $n did not start"; return; }; done
	ready3 || { bad "$1: the fleet did not form"; return; }
	F=$(fill 0 3000); [ "$F" = 3000 ] && ok "$1: 3,000 keys stored through node 1" || bad "$1: stored $F of 3,000"
	O2=$(older 2); O3=$(older 3)
	# two sweep cycles (10 s each) and the answer between them
	i=0; while [ $i -lt 45 ]; do
		[ "$(st 1 'x.get("sweep_gate", {}).get("clean", 0)')" -ge 2 ] 2>/dev/null && break
		sleep 1; i=$((i+1))
	done
	R2=$(( $(older 2) - O2 )); R3=$(( $(older 3) - O3 ))
	[ "$R2" -lt 150 ] && [ "$R3" -lt 150 ] \
		&& ok "$1: after the sweep, the peers refused $R2 / $R3 copies as older (the push already delivered them)" \
		|| bad "$1: the peers refused $R2 / $R3 copies as older - the sweep re-sent what the push delivered"
	G=$(gate)
	case "$G" in *'"clean": '[1-9]*) ok "$1: node 1's sweep rounds answered clean: $G";; *) bad "$1: no clean round on node 1: $G";; esac
	SH=$(short $2 3000)
	[ "$SH" = 0 ] && ok "$1: every one of 3,000 keys has its $2 copies" \
		|| bad "$1: $SH of 3,000 keys are short of their $2 copies"
	# 2: pushes to node 3 lost, then healed by the gated sweep
	echo "cut 127.0.22.1" > "$D/cut3"; echo "cut 127.0.22.3" > "$D/cut1"
	sleep 0.5
	F=$(fill 3000 3500); [ "$F" = 500 ] || bad "$1: stored $F of the 500"
	: > "$D/cut3"; : > "$D/cut1"
	i=0; while [ $i -lt 60 ]; do
		[ "$(short $2 3500)" = 0 ] && break
		sleep 2; i=$((i+2))
	done
	SH=$(short $2 3500)
	[ "$SH" = 0 ] && ok "$1: node 3 cut off for 500 writes, then healed: every one of 3,500 keys has its $2 copies after ${i} s - the gate sent what differed" \
		|| bad "$1: $SH of 3,500 keys still short of $2 copies after the heal - gate $(gate)"
	kill -9 $(cat "$D"/*.pid) 2>/dev/null
	rm -rf "$D"; D=
}

run "eager" 3 "" "mode = eager"
run "spread K=2" 2 "mode = spread
replicas = 2
collections = c" ""
echo "sweepgatetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
