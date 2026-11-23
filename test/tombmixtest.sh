#!/bin/sh
# tombmixtest.sh - a fleet switching tombstone_acks no -> yes one node at a time: a member on yes among
# members on no keeps the time rule (no rejoin, it names them), once all are on yes the marks forget, and a
# restart then crosses seqs a newer delete took over without a rebuild.
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./netcutshim.so}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
SANRT=$(ldd "$BIN" 2>/dev/null | awk "/libasan|libclang_rt\\.asan/ { print \$3; exit }")
D=$(mktemp -d /var/tmp/pctmix.XXXXXX)
PIDS="" DP=""
trap 'kill $DP 2>/dev/null; for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 $p; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -f "$SHIM" ] || { echo "tombmixtest: no netcut shim at $SHIM" >&2; exit 1; }
for p in 19691 19692 19693; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "tombmixtest: port $p already bound" >&2; exit 1; }
done
conf() { # conf <i> <yes|no>
	mkdir -p "$D/w$1" "$D/s$1"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = tm-client-secret
cluster = tm-cluster-secret
[listen]
tcp = 127.0.169.$1:1969$1
plaintext = loopback
[cluster]
multicast = 239.255.77.201:17321
advertise = 127.0.169.$1
mode = eager
collections = c
tombstone_acks = $2
[wal]
dir = $D/w$1
segment_mb = 8
probe = no
save = off
[collection c]
buckets_log2 = 12
C
	chmod 600 "$D/n$1.conf"
}
start() { # start <i>: node 3 under the netcut shim; its log restarts, the earlier one kept
	[ -f "$D/n$1.log" ] && cat "$D/n$1.log" >> "$D/n$1.old"
	: > "$D/n$1.log"
	if [ $1 = 3 ]; then
		LD_PRELOAD="${SANRT:+$SANRT }$SHIM${PC_NOSYNC_LIB:+ $PC_NOSYNC_LIB}" PC_NETCUT_CTL="$D/cut3" PC_NETCUT_LOG="$D/cutlog3" \
			"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	else
		"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	fi
	PIDS="$PIDS $!"; eval "P$1=$!"
	k=0; while [ $k -lt 150 ] && ! grep -q "perfcached ready" "$D/n$1.log"; do sleep 0.1; k=$((k+1)); done
	grep -q "perfcached ready" "$D/n$1.log" || { bad "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; exit 1; }
}
st() { # st <i> <python expr over c = /stats cluster>
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.169.%d" % i, 19690 + i), 10))
c = pcnative.call(f, "stats")["result"]["cluster"]
print(eval(sys.argv[2]))
PY
}
ready() { # ready <i>: up to 30 s for its first "-> ready" in this run's log
	k=0; while [ $k -lt 150 ] && ! grep -q "node state .* -> ready" "$D/n$1.log"; do sleep 0.2; k=$((k+1)); done
	grep -q "node state .* -> ready" "$D/n$1.log"
}
since_ready() { # since_ready <i> <pattern>: lines matching after the run's first ready
	awk -v p="$2" 'r && $0 ~ p { n++ } /node state .* -> ready/ { r = 1 } END { print n + 0 }' "$D/n$1.log"
}
calm() { # calm <i> <label> [floors]: the node, once ready, stays ready - no rejoin, no lost-deletes
	# rejoin even before; with "floors", floors past deletes a newer one took over are allowed
	R=$(since_ready "$1" "-> recovering"); L=$(since_ready "$1" "rejoin - ")
	F=$(since_ready "$1" "mark floored there"); Q=$(grep -c "deletes in question were lost" "$D/n$1.log")
	X=", no floor"; [ -n "${3:-}" ] && F=0 X=
	[ "$R" = 0 ] && [ "$L" = 0 ] && [ "$F" = 0 ] && [ "$Q" = 0 ] &&
		ok "$2: node $1 stayed ready (no recovering after its first ready, no rejoin, no lost-deletes rejoin$X)" ||
		bad "$2: node $1 left ready $R time(s), $L rejoin line(s), $F floor(s), $Q lost-deletes rejoin(s): $(grep -m3 'rejoin - \|floored there\|-> recovering' "$D/n$1.log" | cut -c1-200 | tr '\n' ' ')"
}

: > "$D/cut3"
for i in 1 2 3; do conf $i no; done
for i in 1 2 3; do start $i; done
python3 "$T/waitready.py" 127.0.169.1:19691 127.0.169.2:19692 127.0.169.3:19693 || bad "a node is not ready"
k=0; while [ $k -lt 75 ] && [ "$(st 1 'c.get("peers_up")')" != 2 ]; do sleep 0.2; k=$((k+1)); done
N1=$(st 1 'c["node"]'); N2=$(st 2 'c["node"]')

# deletes flowing through nodes 1 and 2 for the whole run, over 3000 records that stay: keys deleted
# once, and 50 deleted again and again, so an origin's newer delete takes over the one it retained
python3 - <<'PY' > "$D/driver" 2>&1 &
import socket, time, pcnative
def conn(i):
    return pcnative.wrap(socket.create_connection(("127.0.169.%d" % i, 19690 + i), 10))
f = [conn(1), conn(2)]
for i in range(3000):
    pcnative.call(f[i % 2], "set", col="c", key="k:%d" % i, value="v")
t0 = time.time(); i = 0
while time.time() - t0 < 300:
    t = time.time(); j = i % 2
    try:
        pcnative.call(f[j], "set", col="c", key="d:%d" % (i % 50), value="v")
        pcnative.call(f[j], "del", col="c", key="d:%d" % (i % 50))
        pcnative.call(f[j], "set", col="c", key="u:%d" % i, value="v")
        pcnative.call(f[j], "del", col="c", key="u:%d" % i)
    except Exception:
        time.sleep(0.2)
        try:
            f[j] = conn(j + 1)     # that node restarting: the other carries on
        except Exception:
            pass
    i += 1
    time.sleep(max(0, 1 / 100.0 - (time.time() - t)))
PY
DP=$!
sleep 3
# a few of node 1's and node 2's deletes lost on the way to node 3, while all are on no
echo "cut 127.0.169.1 rst" > "$D/cut3"; sleep 0.3; : > "$D/cut3"
echo "cut 127.0.169.2 rst" > "$D/cut3"; sleep 0.3; : > "$D/cut3"
sleep 3

# 1. node 3 restarted on yes among two on no: the time rule, no rejoin
kill -TERM $P3; wait $P3 2>/dev/null
conf 3 yes
start 3
ready 3 || bad "1. node 3 never turned ready: $(grep 'node state' "$D/n3.log" | tr '\n' ' ')"
sleep 30
calm 3 "1. yes among two on no, 30 s"
W=$(st 3 'sorted(c["acks"]["fleet"]["waiting_for"])'); C3=$(st 3 'c["acks"]["fleet"]["confirming"]')
[ "$C3" = False ] && [ "$W" = "$(python3 -c "print(sorted([$N1, $N2]))")" ] &&
	ok "1. node 3 keeps the time rule and names the members on no: waiting_for $W" ||
	bad "1. node 3 confirming $C3, waiting_for $W (want False, nodes $N1 and $N2)"
for i in 1 2; do calm $i "1. on no beside it"; done

# 2. node 1, then node 2, switched: the fleet confirms, the marks forget, nobody loops
H0=$(st 1 'c["tombstones_retained"]["numbered"]')
for i in 1 2; do
	eval "kill -TERM \$P$i; wait \$P$i 2>/dev/null"
	conf $i yes
	start $i
	ready $i || bad "2. node $i never turned ready: $(grep 'node state' "$D/n$i.log" | tr '\n' ' ')"
	sleep 15
	calm 3 "2. node 3 while node $i switched"
done
k=0; while [ $k -lt 75 ] && [ "$(st 1 'c["acks"]["fleet"]["confirming"]')$(st 2 'c["acks"]["fleet"]["confirming"]')$(st 3 'c["acks"]["fleet"]["confirming"]')" != TrueTrueTrue ]; do sleep 0.2; k=$((k+1)); done
[ "$(st 1 'c["acks"]["fleet"]["confirming"]')$(st 2 'c["acks"]["fleet"]["confirming"]')$(st 3 'c["acks"]["fleet"]["confirming"]')" = TrueTrueTrue ] &&
	ok "2. all three on yes: the fleet confirms on every node" ||
	bad "2. confirming on nodes 1/2/3: $(st 1 'c["acks"]["fleet"]') / $(st 3 'c["acks"]["fleet"]')"
k=0; while [ $k -lt 75 ]; do
	H1=$(st 1 'c["tombstones_retained"]["numbered"]')
	[ "${H1:-999999}" -lt 1000 ] 2>/dev/null && break
	sleep 0.2; k=$((k+1))
done
[ "${H1:-999999}" -lt 1000 ] 2>/dev/null &&
	ok "2. the marks forget: node 1 holds $H1 numbered deletes (from ${H0:-?} under the time rule)" ||
	bad "2. node 1 still holds ${H1:-?} numbered deletes (${H0:-?} under the time rule)"
sleep 10
for i in 1 2 3; do calm $i "2. all on yes" floors; done

# 4. node 3 restarted in the fleet all on yes: the deletes made while it was down are asked for, and
# a seq a newer delete of the same key took over is floored past - not a lost delete, no rebuild
gf() { a=$(st 1 'c["acks"]["gap_floored"]'); b=$(st 2 'c["acks"]["gap_floored"]'); echo $((${a:-0} + ${b:-0})); }
G0=$(gf)
kill -TERM $P3; wait $P3 2>/dev/null
start 3
ready 3 || bad "4. node 3 never turned ready: $(grep 'node state' "$D/n3.log" | tr '\n' ' ')"
sleep 30
calm 3 "4. restarted among yes, 30 s" floors
for i in 1 2; do calm $i "4. beside it" floors; done
G=$(( $(gf) - G0 ))
[ "$G" -gt 0 ] 2>/dev/null &&
	ok "4. nodes 1 and 2 floored node 3 past $G range(s) of seqs no longer held" ||
	bad "4. no floor answered node 3 (gap_floored +${G:-?}) - its hole crossed no superseded seq, nothing above is a test of it"

kill $DP 2>/dev/null; DP=
L=$(python3 - <<'PY' 2>/dev/null
import socket, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.169.3", 19693), 10))
print(sum(1 for i in range(3000) if (pcnative.call(f, "get", col="c", key="k:%d" % i).get("result") or {}).get("found")))
PY
)
[ "$L" = 3000 ] && ok "3. node 3 holds all 3000 records that were never deleted" ||
	bad "3. node 3 holds ${L:-?} of the 3000 kept records"
for i in 1 2 3; do
	grep -h 'an fsync has been stuck' "$D/n$i.log" 2>/dev/null | head -n 1 | sed "s/^/  ..   not counted, the runner's storage stalled: /"
	cat "$D/n$i.old" "$D/n$i.log" 2>/dev/null | grep "CRIT" | grep -qv 'an fsync has been stuck' &&
		bad "node $i logged CRIT: $(cat "$D/n$i.old" "$D/n$i.log" | grep 'CRIT' | grep -v 'an fsync has been stuck' | head -n 2 | tr '\n' ' ' | cut -c1-220)"
done
echo "tombmixtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
