#!/bin/sh
# masterlosstest.sh - RV-4b: shard and spread fleets losing their MASTER.
#
# RV-4's map (DESIGN 12gb): master loss was asserted only for store and
# eager.  Shard and spread place every key by the master's map, and the
# suites that kill a node there kill it by NUMBER (shardswitchtest's
# victim "may be the master", unasserted).  Here the victim is chosen by
# ROLE.  Per mode (shard; spread with replicas = 2), three nodes with a
# WAL:
#   1. 300 keys through node 1: every node answers every key the same,
#      and the fleet holds each key exactly once (shard) / exactly K = 2
#      times (spread);
#   2. kill -9 the node whose role is master: the survivors elect one
#      master under a HIGHER term and agree on the map;
#   3. through both survivors every key answers the SAME (no split
#      ownership).  Shard may miss the keys the dead master owned - it
#      keeps one copy; spread must serve all of them, and repairs to K
#      copies on the survivors;
#   4. 100 new keys through the survivors land once (shard) / K times
#      (spread) and read from both;
#   5. the old master restarts from its WAL: one master, one map term,
#      and within 60 s every key reads the same from all three and the
#      fleet holds each key exactly once / exactly K times - reshard's
#      invariant: a miss is allowed while a node is down, two live copies
#      of a shard key never are.
# Usage: test/masterlosstest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcml.XXXXXX)
trap 'kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
MC=239.255.77.93
MP=17193
N=300
for p in 17361 17362 17363; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "masterlosstest: port $p already bound" >&2; exit 1; }
done

conf() { # conf <n> <cluster lines>
	mkdir -p "$D/s$1/wal"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = ml-client-secret
cluster = ml-cluster-secret
[listen]
tcp = 127.0.0.1:1736$1
plaintext = loopback
[cluster]
multicast = $MC:$MP
advertise = 127.0.14.$1
pull_timeout_ms = 400
$2
collections = c
[collection c]
buckets_log2 = 12
[wal]
dir = $D/s$1/wal
probe = no
fsync = everysec
segment_mb = 8
segments = 4
save = off
EOF
	chmod 600 "$D/n$1.conf"
}
start() { # start <n>
	: > "$D/n$1.log"
	"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0
	while [ $i -lt 150 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; return 1
}
stop_kill() { kill -9 "$(cat "$D/n$1.pid" 2>/dev/null)" 2>/dev/null; rm -f "$D/n$1.pid"; }
st() { # st <n>: "role mapnodes term seq entries"
	printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"stats"}' | timeout 10 python3 -c '
import json, socket, sys
try:
    s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
    s.sendall(sys.stdin.read().encode())
    r = json.loads(s.makefile("rb").readline())["result"]; c = r["cluster"]; m = c["map"]
    e = [x.get("entries") for x in r.get("collections", []) if x.get("name") == "c"]
    print(c.get("role"), m.get("nodes"), m.get("term"), m.get("seq"), e[0] if e else "?")
except Exception: print("? ? ? ? ?")' "1736$1" 2>/dev/null || echo "? ? ? ? ?"; }
w() { echo "$1" | cut -d' ' -f"$2"; }
put() { # put <n> <prefix> <from> <to>
	timeout 60 python3 -c '
import json, socket, sys
f = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=20).makefile("rwb")
p, a, b = sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
for i in range(a, b):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": "set",
        "params": {"col": "c", "key": "%s%03d" % (p, i), "value": "v%s%03d" % (p, i)}}) + "\n").encode())
f.flush()
bad = 0
for _ in range(a, b):
    r = json.loads(f.readline())
    if not (r.get("result") or {}).get("stored"): bad += 1
print(bad)' "1736$1" "$2" "$3" "$4"; }
# answers <n>: one line per key k000-k299 and x000-x099: the value node n
# returns, "-" for a miss - so two nodes can be compared line by line
answers() { timeout 60 python3 -c '
import json, socket, sys
f = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=20).makefile("rwb")
keys = ["k%03d" % i for i in range(300)] + ["x%03d" % i for i in range(100)]
for j, k in enumerate(keys):
    f.write((json.dumps({"jsonrpc": "2.0", "id": j, "method": "get", "params": {"col": "c", "key": k}}) + "\n").encode())
f.flush()
out = [None] * len(keys)
for _ in keys:
    m = json.loads(f.readline()); r = m.get("result") or {}
    out[m["id"]] = r.get("value") or "-"
print("\n".join(out))' "1736$1" 2>/dev/null; }
hits() { printf '%s\n' "$1" | grep -vc '^-$'; }
recl() { s=0; for n in "$@"; do r=$(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"stats"}' | timeout 10 python3 -c '
import json, socket, sys
try:
    s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5); s.sendall(sys.stdin.read().encode())
    print(json.loads(s.makefile("rb").readline())["result"]["cluster"].get("spread_reclaimed", 0))
except Exception: print(0)' "1736$n" 2>/dev/null); s=$((s + ${r:-0})); done; echo $s; }
sumentries() { s=0; for n in "$@"; do e=$(w "$(st $n)" 5); [ "$e" = "?" ] && e=0; s=$((s + e)); done; echo $s; }

run() { # run <label> <K> <cluster lines>
	label=$1; K=$2
	echo "--- $label"
	for n in 1 2 3; do stop_kill $n; done
	rm -rf "$D"/s? "$D"/n?.log
	for n in 1 2 3; do conf $n "$3"; done
	start 1 && sleep 1 && start 2 && start 3 || { bad "$label: the fleet did not start"; return; }
	i=0; while [ $i -lt 60 ]; do [ "$(w "$(st 1)" 2)" = 3 ] && [ "$(w "$(st 3)" 2)" = 3 ] && break; sleep 0.5; i=$((i+1)); done
	sleep 2
	B=$(put 1 k 0 $N)
	i=0; while [ $i -lt 60 ]; do [ "$(sumentries 1 2 3)" = $((K * N)) ] && break; sleep 0.5; i=$((i+1)); done
	A1=$(answers 1); A2=$(answers 2); A3=$(answers 3); T=$(sumentries 1 2 3)
	[ "$B" = 0 ] && [ "$A1" = "$A2" ] && [ "$A1" = "$A3" ] && [ "$(hits "$A1")" = $N ] && [ "$T" = $((K * N)) ] \
		&& ok "$label: $N keys, every node answers every key the same, the fleet holds $T copies ($K each)" \
		|| { bad "$label: formed badly - refused $B, hits $(hits "$A1")/$(hits "$A2")/$(hits "$A3"), copies $T of $((K * N))"; return; }

	# ---- the master dies ----
	M=0; for n in 1 2 3; do [ "$(w "$(st $n)" 1)" = master ] && M=$n; done
	[ $M != 0 ] || { bad "$label: no master to kill"; return; }
	T0=$(w "$(st $M)" 3)
	S=""; for n in 1 2 3; do [ $n = $M ] || S="$S $n"; done
	set -- $S; SA=$1; SB=$2
	stop_kill $M
	i=0; while [ $i -lt 80 ]; do
		a=$(st $SA); b=$(st $SB)
		ma=0; [ "$(w "$a" 1)" = master ] && ma=$((ma+1)); [ "$(w "$b" 1)" = master ] && ma=$((ma+1))
		[ $ma = 1 ] && [ "$(w "$a" 2)" = 2 ] && [ "$(w "$b" 2)" = 2 ] && [ "$(w "$a" 3)" = "$(w "$b" 3)" ] \
			&& [ "$(w "$a" 4)" = "$(w "$b" 4)" ] && [ "$(w "$a" 3)" -gt "$T0" ] 2>/dev/null && break
		sleep 0.5; i=$((i+1))
	done
	[ $ma = 1 ] && [ "$(w "$a" 3)" -gt "$T0" ] 2>/dev/null && [ "$(w "$a" 3) $(w "$a" 4)" = "$(w "$b" 3) $(w "$b" 4)" ] \
		&& ok "$label: master node $M killed; nodes $SA+$SB elected one master at term $(w "$a" 3) (was $T0) and agree on map ($(w "$a" 3),$(w "$a" 4)) with 2 nodes, after $((i / 2)) s" \
		|| { bad "$label: after the master's loss: node $SA '$a', node $SB '$b' (term was $T0)"; return; }
	sleep 3
	AA=$(answers $SA); AB=$(answers $SB); hA=$(hits "$AA")
	if [ $K = 1 ]; then
		[ "$AA" = "$AB" ] \
			&& ok "$label: both survivors answer every key the same ($hA of $N served; the rest were the dead master's single copies)" \
			|| bad "$label: the survivors disagree on $(printf '%s\n' "$AA" | paste - /dev/fd/3 3<<EOF | awk -F'\t' '$1!=$2' | wc -l
$AB
EOF
) keys - split ownership"
	else
		i=0; while [ $i -lt 120 ]; do [ "$(sumentries $SA $SB)" = $((2 * N)) ] && break; sleep 0.5; i=$((i+1)); done
		AA=$(answers $SA); AB=$(answers $SB); hA=$(hits "$AA"); T=$(sumentries $SA $SB)
		[ "$AA" = "$AB" ] && [ "$hA" = $N ] && [ "$T" = $((2 * N)) ] \
			&& ok "$label: every key still served, the same through both survivors, and repaired to $T copies ($K on 2 nodes) in $((i / 2)) s" \
			|| bad "$label: survivors serve $hA/$(hits "$AB") of $N, same=$([ "$AA" = "$AB" ] && echo y || echo n), copies $T of $((2 * N))"
	fi
	TB=$(sumentries $SA $SB)
	b1=$(put $SA x 0 50); b2=$(put $SB x 50 100)
	sleep 2
	AA=$(answers $SA); AB=$(answers $SB); TA=$(sumentries $SA $SB)
	nx=$(printf '%s\n' "$AA" | tail -100 | grep -vc '^-$')
	[ "$b1$b2" = 00 ] && [ "$AA" = "$AB" ] && [ "$nx" = 100 ] && [ $((TA - TB)) = $((K * 100)) ] \
		&& ok "$label: 100 keys written after the failover land $K time(s) each and read the same from both survivors" \
		|| bad "$label: after the failover: refused $b1+$b2, new keys read $nx/100, copies +$((TA - TB)) (want +$((K * 100)))"

	# ---- the old master returns ----
	start $M || { bad "$label: node $M did not restart"; return; }
	# up to 150 s: a spread return leaves surplus copies (the survivors'
	# re-placed ones) that the SETTLED reclaim pass drops - spreadtest
	# allows 120 s for it
	i=0; while [ $i -lt 300 ]; do
		s1=$(st 1); s2=$(st 2); s3=$(st 3)
		ms=0; for s in "$s1" "$s2" "$s3"; do [ "$(w "$s" 1)" = master ] && ms=$((ms+1)); done
		A1=$(answers 1); A2=$(answers 2); A3=$(answers 3); T=$(sumentries 1 2 3)
		[ $ms = 1 ] && [ "$(w "$s1" 2)$(w "$s2" 2)$(w "$s3" 2)" = 333 ] \
			&& [ "$A1" = "$A2" ] && [ "$A1" = "$A3" ] && [ "$(hits "$A1")" = $((N + 100)) ] && [ "$T" = $((K * (N + 100))) ] && break
		sleep 0.5; i=$((i+1))
	done
	RC=$(recl 1 2 3)
	[ $ms = 1 ] && [ "$(w "$s1" 3)" = "$(w "$s2" 3)" ] && [ "$(w "$s1" 3)" = "$(w "$s3" 3)" ] \
		&& ok "$label: node $M back from its WAL: one master, one map term ($(w "$s1" 3))" \
		|| bad "$label: after node $M returned: '$s1' | '$s2' | '$s3'"
	[ "$A1" = "$A2" ] && [ "$A1" = "$A3" ] && [ "$(hits "$A1")" = $((N + 100)) ] && [ "$T" = $((K * (N + 100))) ] \
		&& ok "$label: in $((i / 2)) s every key reads the same from all three, and the fleet holds $T copies - exactly $K per key (reclaimed $RC)" \
		|| bad "$label: after $((i / 2)) s: hits $(hits "$A1")/$(hits "$A2")/$(hits "$A3") of $((N + 100)), same=$([ "$A1" = "$A2" ] && [ "$A1" = "$A3" ] && echo y || echo n), copies $T (want $((K * (N + 100))))"
	for n in 1 2 3; do stop_kill $n; done
}

run "shard" 1 "mode = shard"
run "spread K=2" 2 "mode = spread
replicas = 2"
echo "masterlosstest: $pass passed, $fail failed"
[ $fail -eq 0 ]
