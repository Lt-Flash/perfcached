#!/bin/sh
# resizeheldtest.sh - S255: a fleet resize reaches every node, even one that
# was busy or missed the announce.
#
# A resize reaches the peers as ONE datagram (M_COL_SET, op RESIZE).  Two
# ways it used to stop short, each leaving a node at the old size for good
# while the rest of the fleet moved on - nothing ever reconciled the size:
#   - held:  a peer still running the PREVIOUS resize refused the new one
#            (pc_store_resize_start -2), and the refusal was discarded;
#   - lost:  a peer that did not receive the datagram never heard of it.
# Three eager nodes, collection c at 2^12, 400 keys:
#   1. held - node 3's maintenance thread (pc-maint, which runs resizes) is
#      frozen (test/threadfreeze.c); resize to 2^14 through node 1, so node
#      3 STARTS it and cannot finish; once nodes 1 and 2 are done, resize to
#      2^10 through node 2.  Node 3 must end at 1,024 when it is released;
#   2. lost - node 3 cut off from node 1 (test/netcutshim.so) for 2 s around
#      a resize to 2^13 through node 1, then healed: node 3 must reach 8,192
#      from the announce's repeats;
#   3. order - a repeat of an OLDER resize must not undo a newer one: right
#      after (2), resize to 2^11 through node 2 while node 1's repeats of
#      2^13 are still going; every node must settle at 2,048 and stay.
#   and every step: all 400 keys on every node, reading right.
# Fail-first: rc48 leaves node 3 at 16,384 in (1) and at 4,096... in (2).
# Usage: test/resizeheldtest.sh [./perfcached] [./threadfreeze] [./netcutshim.so]
set -u
BIN=${1:-./perfcached}
TF=$(cd "$(dirname "${2:-./threadfreeze}")" && pwd)/$(basename "${2:-./threadfreeze}")
SHIM=$(cd "$(dirname "${3:-./netcutshim.so}")" && pwd)/$(basename "${3:-./netcutshim.so}")
D=$(mktemp -d /var/tmp/pcrzheld.XXXXXX)
# the pid files hold threadfreeze's pid; the daemon is its child
cleanup() {
	for f in "$D"/*.pid; do
		[ -f "$f" ] || continue
		p=$(cat "$f"); kill -9 $(pgrep -P "$p") "$p" 2>/dev/null
	done
	rm -rf "$D"
}
trap cleanup EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 18861 18862 18863 18871 18872 18873; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "resizeheldtest: port $p busy" >&2; exit 1; }
done
[ -x "$TF" ] && [ -f "$SHIM" ] || { echo "resizeheldtest: needs $TF and $SHIM" >&2; exit 1; }
# a dynamic sanitizer runtime (gcc's check-asan) must come FIRST in the
# preload list, the shim after it - as spreadparttest and eagerparttest do.
# rc49's check-asan: "ASan runtime does not come first in initial library
# list", and node 1 did not start.
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
PRELOAD="${SANRT:+$SANRT }$SHIM"

for n in 1 2 3; do
	mkdir -p "$D/s$n/wal"
	cat > "$D/n$n.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$n
allow_create = yes
[memory]
arena_mb = 64
[secrets]
client = rh-client-secret
cluster = rh-cluster-secret
enable = rh-enable-secret
[listen]
tcp = 127.0.0.1:1886$n
http = 127.0.0.1:1887$n
plaintext = loopback
[cluster]
multicast = 239.255.78.3:18865
advertise = 127.0.21.$n
[collection c]
buckets_log2 = 12
autoscale = off
mode = eager
[wal]
dir = $D/s$n/wal
probe = no
fsync = everysec
segment_mb = 8
segments = 4
save = off
C
	chmod 600 "$D/n$n.conf"
done
start() { # start <n>: under threadfreeze, with the netcut shim
	: > "$D/n$1.log"; : > "$D/tf$1"
	LD_PRELOAD="$PRELOAD" PC_NETCUT_CTL="$D/cut$1" PC_NETCUT_LOG="$D/cutlog$1" \
		"$TF" "$D/tf$1" -- "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0
	while [ $i -lt 150 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; return 1
}
st() { curl -s "http://127.0.0.1:1887$1/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin); c = [x for x in r["collections"] if x["name"] == "c"][0]
print(eval(sys.argv[1]))' "$2" 2>/dev/null; }
bk() { st $1 'c["buckets"]'; }
busy() { st $1 '1 if c.get("resizing_to") else 0'; }
ent() { st $1 'c["entries"]'; }
ready3() {
	i=0; while [ $i -lt 120 ]; do
		[ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ] && \
			[ "$(st 1 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))')$(st 2 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))')$(st 3 'sum(1 for p in r["cluster"]["peers"] if p.get("up"))')" = 222 ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}
resize() { # resize <n> <log2>: privileged
	python3 - "1886$1" "$2" <<'PY'
import json, socket, sys
f = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=30).makefile("rwb")
for i, (m, p) in enumerate([("enable", {"secret": "rh-enable-secret"}), ("resize", {"col": "c", "buckets_log2": int(sys.argv[2])})]):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": m, "params": p}) + "\n").encode()); f.flush()
    last = f.readline().decode().strip()
print(last)
PY
}
fill() { python3 - "18861" <<'PY'
import json, socket, sys
f = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=30).makefile("rwb")
for i in range(400):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": "set", "params": {"col": "c", "key": "k%03d" % i, "value": "v%03d" % i}}) + "\n").encode())
f.flush()
print(sum(1 for _ in range(400) if (json.loads(f.readline()).get("result") or {}).get("stored")))
PY
}
wrong() { python3 - "1886$1" <<'PY'
import json, socket, sys
f = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=30).makefile("rwb")
for i in range(400):
    f.write((json.dumps({"jsonrpc": "2.0", "id": i, "method": "get", "params": {"col": "c", "key": "k%03d" % i}}) + "\n").encode())
f.flush()
print(sum(1 for i in range(400) if (json.loads(f.readline()).get("result") or {}).get("value") != "v%03d" % i))
PY
}
# at <buckets> <seconds>: every node at <buckets>, none resizing
at() {
	i=0; while [ $i -lt $(( $2 * 2 )) ]; do
		[ "$(bk 1)/$(bk 2)/$(bk 3)" = "$1/$1/$1" ] && [ "$(busy 1)$(busy 2)$(busy 3)" = 000 ] && return 0
		sleep 0.5; i=$((i+1))
	done
	return 1
}
report() { # report <label> <buckets> <seconds>
	if at $2 $3; then
		ok "$1: every node at $2 buckets, none still resizing"
	else
		bad "$1: buckets $(bk 1)/$(bk 2)/$(bk 3), resizing $(busy 1)$(busy 2)$(busy 3) (want $2 on all three)"
		for n in 1 2 3; do grep -h "resiz\|held" "$D/n$n.log" | tail -4 | sed "s/^/        node $n: /"; done
	fi
	E="$(ent 1)/$(ent 2)/$(ent 3)"; W="$(wrong 1)$(wrong 2)$(wrong 3)"
	[ "$E" = "400/400/400" ] && [ "$W" = 000 ] && ok "$1: all 400 keys on every node, every one reading right" \
		|| bad "$1: entries $E, wrong reads per node $W"
}

for n in 1 2 3; do start $n || exit 1; done
ready3 || { bad "the fleet did not form"; echo "resizeheldtest: $pass passed, $fail failed"; exit 1; }
F=$(fill); [ "$F" = 400 ] && ok "400 keys stored through node 1" || bad "stored $F of 400"
i=0; while [ $i -lt 40 ] && [ "$(ent 2)$(ent 3)" != 400400 ]; do sleep 0.5; i=$((i+1)); done

echo "--- 1. held: a peer still running the previous resize"
echo "1 pc-maint 12000" > "$D/tf3"
i=0; while [ $i -lt 50 ] && ! grep -q "^1 frozen" "$D/tf3.ack" 2>/dev/null; do sleep 0.1; i=$((i+1)); done
grep -q "^1 frozen" "$D/tf3.ack" 2>/dev/null && ok "node 3's pc-maint frozen for 12 s" || bad "node 3's pc-maint did not freeze: $(cat "$D/tf3.ack" 2>/dev/null)"
R=$(resize 1 14); case "$R" in *resiz*) ok "resize c 14 accepted through node 1";; *) bad "resize 14 refused: $R";; esac
i=0; while [ $i -lt 40 ] && [ "$(bk 1)$(bk 2)$(busy 1)$(busy 2)" != 163841638400 ]; do sleep 0.5; i=$((i+1)); done
[ "$(busy 3)" = 1 ] && ok "nodes 1 and 2 finished 2^14; node 3 is still running it (frozen)" \
	|| bad "node 3 is not mid-resize (buckets $(bk 3), resizing $(busy 3)) - the premise failed"
R=$(resize 2 10); case "$R" in *resiz*) ok "resize c 10 accepted through node 2 while node 3 is busy";; *) bad "resize 10 refused: $R";; esac
report "held" 1024 30

echo "--- 2. lost: a peer that missed the announce"
B3=$(bk 3)
echo "cut 127.0.21.1" > "$D/cut3"; echo "cut 127.0.21.3" > "$D/cut1"
sleep 0.3
R=$(resize 1 13); case "$R" in *resiz*) ok "resize c 13 accepted through node 1, node 3 cut off from it";; *) bad "resize 13 refused: $R";; esac
sleep 2
[ "$(bk 3)" = "$B3" ] && ok "node 3 did not hear it while cut (still $B3)" || bad "node 3 went $B3 -> $(bk 3) while cut - the cut did not hold"
: > "$D/cut3"; : > "$D/cut1"
report "lost" 8192 20

echo "--- 3. order: a repeat of an older resize does not undo a newer one"
T0=$(date +%s)
R=$(resize 1 12); case "$R" in *resiz*) ;; *) bad "resize 12 refused: $R";; esac
at 4096 15 || bad "the resize to 2^12 did not land: $(bk 1)/$(bk 2)/$(bk 3)"
T1=$(( $(date +%s) - T0 ))
R=$(resize 2 11)
# node 1 repeats 12 at +1, +3 and +7 s: 11 must start before the last one
case "$R" in *resiz*)
	[ $T1 -lt 7 ] && ok "c 12 through node 1 landed in ${T1} s; c 11 through node 2 started before node 1's last repeat of 12 (+7 s)" 		|| bad "c 12 took ${T1} s - node 1's repeats were over before c 11 started, so this proves nothing";;
*) bad "resize 11 refused: $R";; esac
report "order" 2048 25
sleep 8
[ "$(bk 1)/$(bk 2)/$(bk 3)" = "2048/2048/2048" ] && ok "and still 2048 on all three after node 1's repeats ran out" \
	|| bad "after the repeats: $(bk 1)/$(bk 2)/$(bk 3) - an older resize came back"

echo "resizeheldtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
