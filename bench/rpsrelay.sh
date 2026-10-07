#!/bin/bash
# rpsrelay.sh [reps] [secs] - pub/sub across two nodes: publishers on node
# A, subscribers on node B, the driver (bench/rpsbench.c) on a third host,
# so a publish crosses the fleet's relay (perfcached) or the cluster bus
# (a two-master Redis Cluster) before it is delivered, and the latency is
# read against one clock.  Run it ON node A; B and the driver are reached
# over ssh (keys, no passwords).
#
#   A_HOST=<A's address> B_HOST=<B's address> DRIVER=<driver's address> \
#   PERFCACHED=./perfcached RPSBENCH=./rpsbench bench/rpsrelay.sh 2 8
#
# Environment (defaults in brackets):
#   A_HOST, B_HOST, DRIVER    addresses; required
#   PERFCACHED, RPSBENCH      binaries to copy out [./perfcached ./rpsbench]
#   RUNTIME_A, RUNTIME_B      container runtimes for Redis [podman]
#   REDIS_IMG                 [docker.io/library/redis:8]
#   SERVER_CPUS               the servers' cores on A and B [0-7]
#   WORKERS                   perfcached workers [4]
#   ARMS                      "pc redis"; ONLY picks cells by name
#   PUBHOST                   publish somewhere other than A (a control)
#   IFACE_B, IFACE_DRIVER     NICs to sample B's tx and the driver's softirq
#   DRIVER_RPS                e.g. ff: spread the driver's receive softirq
#                             over its CPUs for the run, restored after
#   MULTICAST                 the fleet's group [239.255.77.62:17262]
#   OUT                       where results.tsv goes [/tmp/rpsrelay-out]
#
# READ THE DRIVER'S RECEIVE PATH FIRST.  A driver whose NIC has one
# receive queue and no RPS does all receive softirq on one CPU.  perfcached
# with four workers writes 2.4-3.5x the packets one Redis thread does for
# the same bytes, and that one CPU saturates: measured, 64 subscribers at
# 20k/s read p50 9.7 ms with RPS off and 1.06 ms with it on - a figure of
# the driver, not the server.  The script warns when it sees that shape;
# DRIVER_RPS fixes it for the run.  driver_softirq_cores is in every row.
#
# Cells, 64-byte payloads, 8 publisher connections on 2 threads:
#   s1-paced20k, s1-max, s64-paced20k (64 subscribers on 5 threads), s64-max
# Before each cell B must stop taking in the last one's traffic; after it,
# the relay's counters (perfcached) or the bus's (Redis) and B's client
# output buffers (Redis) are recorded.  Flat out the two fail differently:
# the relay is at-most-once and counts its losses (relay_lost); the bus
# never drops, lags, and disconnects subscribers at their output limit.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPS=${1:-2}
SECS=${2:-8}
: "${A_HOST:?A_HOST}" "${B_HOST:?B_HOST}" "${DRIVER:?DRIVER}"
PERFCACHED=${PERFCACHED:-./perfcached}
RPSBENCH=${RPSBENCH:-./rpsbench}
ARMS=${ARMS:-"pc redis"}
OUT=${OUT:-/tmp/rpsrelay-out}
W=/tmp/rpsrelay
SSH="ssh -n -o BatchMode=yes -o ConnectTimeout=6"
TCK=$(getconf CLK_TCK)
PORT_PC=17992
PORT_REDIS=17993
mkdir -p "$OUT" || exit 1
TSV=$OUT/results.tsv
NENV="WORKDIR=$W SERVER_CPUS=${SERVER_CPUS:-0-7} REDIS_IMG=${REDIS_IMG:-docker.io/library/redis:8}"
onA() { env $NENV RUNTIME=${RUNTIME_A:-podman} sh $W/rpsrelay-node.sh "$@"; }
onB() { timeout 60 $SSH "$B_HOST" "env $NENV RUNTIME=${RUNTIME_B:-podman} sh $W/rpsrelay-node.sh $*"; }
onC() { timeout 60 $SSH "$DRIVER" "$*"; }
say() { echo "$(date +%H:%M:%S) $*"; }
note() { echo "# $*" >> "$TSV"; say "$*"; }

RPS_Q=
RPS_OLD=
cleanup() {
	onA pc-stop; onB pc-stop
	onA redis-stop; onB redis-stop
	onC "for p in \$(pgrep -x rpsbench); do [ \"\$(readlink /proc/\$p/exe)\" = $W/rpsbench ] && kill \$p; done; true"
	if [ -n "$RPS_Q" ]; then
		onC "echo $RPS_OLD > $RPS_Q"
		say "driver rps restored: $(onC "cat $RPS_Q")"
	fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

cat > "$OUT/node.tpl" <<CONF
[daemon]
workers = ${WORKERS:-4}
log_level = notice
state_dir = $W/state
[memory]
arena_mb = 64
[secrets]
client = rpsrelay-client-secret-0123456789
cluster = rpsrelay-cluster-secret-0123456789
[listen]
tcp = 0.0.0.0:17991
http = 127.0.0.1:18991
resp = SELFIP:$PORT_PC
resp_allow = $DRIVER/32
plaintext = loopback
[cluster]
multicast = ${MULTICAST:-239.255.77.62:17262}
advertise = SELFIP
mode = store
collections = 0
pubsub_rx_threads = 2
pubsub_relay = all
[collection 0]
buckets_log2 = 12
CONF
rm -rf $W && mkdir -p $W || exit 1
cp "$PERFCACHED" $W/perfcached && cp "$HERE/rpsrelay-node.sh" $W/ || exit 1
sed "s/SELFIP/$A_HOST/" "$OUT/node.tpl" > $W/n.conf
sed "s/SELFIP/$B_HOST/" "$OUT/node.tpl" > "$OUT/nB.conf"
timeout 30 $SSH "$B_HOST" "rm -rf $W && mkdir -p $W" || exit 1
timeout 60 scp -q "$PERFCACHED" "$HERE/rpsrelay-node.sh" "$B_HOST:$W/" && timeout 30 scp -q "$OUT/nB.conf" "$B_HOST:$W/n.conf" || exit 1
timeout 30 $SSH "$DRIVER" "rm -rf $W && mkdir -p $W" && timeout 60 scp -q "$RPSBENCH" "$DRIVER:$W/rpsbench" || exit 1

if [ -n "${IFACE_DRIVER:-}" ]; then
	nq=$(onC "ls -d /sys/class/net/$IFACE_DRIVER/queues/rx-* | wc -l")
	rps=$(onC "cat /sys/class/net/$IFACE_DRIVER/queues/rx-0/rps_cpus")
	if [ -n "${DRIVER_RPS:-}" ]; then
		RPS_Q=/sys/class/net/$IFACE_DRIVER/queues/rx-0/rps_cpus
		RPS_OLD=$rps
		onC "echo $DRIVER_RPS > $RPS_Q" || exit 1
		say "driver rps_cpus $RPS_OLD -> $(onC "cat $RPS_Q") for the run"
	elif [ "$nq" = 1 ] && [ "$(echo "$rps" | tr -d '0,')" = "" ]; then
		say "WARNING: the driver has one receive queue and no RPS - fan-out latency will measure the driver (set DRIVER_RPS)"
	fi
fi

cols="arm cell rep pub_s deliv_s p50_us p99_us p999_us lost gaps eof drain_s a_cores b_cores b_tx_mbit b_tx_pkts_s driver_softirq_cores drv_cores busiest_thr relay_sent relay_recv relay_lost queue_dropped bus_publish_sent bus_publish_received b_clients_max_outbuf b_outbuf_disconnects"
{
	echo "# date=$(date -Is) perfcached=$($W/perfcached -V) redis=$(onA redis-start > /dev/null && onA redis-cli info server | tr -d '\r' | grep -o 'redis_version:[0-9.]*' | cut -d: -f2; onA redis-stop) reps=$REPS secs=$SECS workers=${WORKERS:-4} server_cpus=${SERVER_CPUS:-0-7} driver_rps=${DRIVER_RPS:-unchanged} arms=$ARMS cells=${ONLY:-all} publish_on=$([ -n "${PUBHOST:-}" ] && echo elsewhere || echo A) payload=64B"
	echo "# roles: A publishes (perfcached node / Redis master), B subscribes, the driver runs rpsbench -H A -B B"
	echo "$cols" | tr ' ' '\t'
} > "$TSV"

start_arm() {
	case $1 in
	pc)
		onA pc-start; onB pc-start
		for i in $(seq 1 60); do
			r=$(onA members | python3 -c 'import json,sys
m=[x for x in json.load(sys.stdin)["members"] if x["addr"]==sys.argv[1]]
print(m[0]["relay"]["direct"] if m else "none")' "$B_HOST" 2>/dev/null)
			[ "$r" = True ] && break
			sleep 1
		done
		[ "$r" = True ] || { note "ERROR the fleet did not form (B's relay direct: $r)"; return 1; }
		PA=$(onA pc-pid); PB=$(onB pc-pid); PORT=$PORT_PC ;;
	redis)
		PA=$(onA redis-start); PB=$(onB redis-start)
		sleep 1
		onA redis-cli cluster addslotsrange 0 8191 > /dev/null
		onB redis-cli cluster addslotsrange 8192 16383 > /dev/null
		onA redis-cli cluster meet "$B_HOST" $PORT_REDIS > /dev/null
		for i in $(seq 1 60); do
			sa=$(onA redis-cli cluster info | tr -d '\r' | grep -E '^cluster_state|^cluster_known_nodes' | tr '\n' ' ')
			sb=$(onB redis-cli cluster info | tr -d '\r' | grep -E '^cluster_state|^cluster_known_nodes' | tr '\n' ' ')
			case "$sa$sb" in *state:ok*known_nodes:2*state:ok*known_nodes:2*) break ;; esac
			sleep 1
		done
		case "$sa$sb" in *state:ok*known_nodes:2*state:ok*known_nodes:2*) ;; *) note "ERROR the Redis cluster did not form: A[$sa] B[$sb]"; return 1 ;; esac
		PORT=$PORT_REDIS ;;
	esac
}

stop_arm() {
	case $1 in
	pc) onA pc-stop; onB pc-stop ;;
	redis) onA redis-stop; onB redis-stop ;;
	esac
	sleep 2
}

counters() { # counters <arm> -> relay_sent relay_recv relay_lost queue_dropped bus_sent bus_received outbuf disconnects
	case $1 in
	pc)
		a=$(onA stats | python3 -c 'import json,sys; p=json.load(sys.stdin)["pubsub"]; print(p["relay_sent"])')
		b=$(onB stats | python3 -c 'import json,sys; p=json.load(sys.stdin)["pubsub"]; print(p["relay_recv"], p["relay_lost"], p["queue_dropped"])')
		echo "$a $b 0 0 0 0" ;;
	redis)
		a=$(onA redis-cli cluster info | tr -d '\r' | awk -F: '/^cluster_stats_messages_publish_sent/{print $2}')
		b=$(onB redis-cli cluster info | tr -d '\r' | awk -F: '/^cluster_stats_messages_publish_received/{print $2}')
		o=$(onB redis-cli info clients | tr -d '\r' | awk -F: '/^client_recent_max_output_buffer/{print $2}')
		k=$(onB redis-cli info stats | tr -d '\r' | awk -F: '/^client_output_buffer_limit_disconnections/{print $2}')
		echo "0 0 0 0 ${a:-0} ${b:-0} ${o:-0} ${k:-0}" ;;
	esac
}

received() {
	case $1 in
	pc) onB stats | python3 -c 'import json,sys; p=json.load(sys.stdin)["pubsub"]; print(p["relay_recv"] + p["relay_lost"])' ;;
	redis) onB redis-cli cluster info | tr -d '\r' | awk -F: '/^cluster_stats_messages_publish_received/{print $2}' ;;
	esac
}

settle() {
	last=-1
	for i in $(seq 1 60); do
		now=$(received "$1")
		[ "$now" = "$last" ] && return 0
		last=$now
		sleep 1
	done
	note "cell $2: B still receiving the last cell's traffic after 60 s"
}

sample() { # sample -> a_ticks b_ticks b_txbytes b_txpkts driver_softirq
	echo "$(onA cpu "$PA") $(onB cpu "$PB") $([ -n "${IFACE_B:-}" ] && onB net "$IFACE_B" || echo "0 0") $(onC "awk '/^cpu /{print \$8}' /proc/stat")"
}

cell() { # cell <arm> <rep> <name> <rpsbench args...>
	arm=$1 rep=$2 name=$3
	shift 3
	settle "$arm" "$name"
	set -- $(counters "$arm") "$@"
	c0="$1 $2 $3 $4 $5 $6 $7 $8"
	shift 8
	out=$OUT/drive-$arm-$name-$rep.txt
	timeout $((SECS + 90)) $SSH "$DRIVER" "$W/rpsbench -H ${PUBHOST:-$A_HOST} -p $PORT -B $B_HOST -b $PORT -d $SECS -v 64 $*" > "$out" 2>&1 &
	dp=$!
	sleep 1.6
	s0=$(sample)
	sleep "$SECS"
	s1=$(sample)
	wait $dp
	c1=$(counters "$arm")
	python3 - "$out" "$arm" "$name" "$rep" "$s0" "$s1" "$c0" "$c1" "$TCK" "$SECS" "$cols" >> "$TSV" <<'PY'
import re, sys
out, arm, name, rep, s0, s1, c0, c1, tck, secs, cols = sys.argv[1:]
tck, secs = float(tck), float(secs)
line = next((l for l in open(out) if l.startswith("RESULT")), "")
r = dict(re.findall(r"(\w+)=(\S+)", line))
s0, s1 = [float(x) for x in s0.split()], [float(x) for x in s1.split()]
c0, c1 = [int(x) for x in c0.split()], [int(x) for x in c1.split()]
d = [b - a for a, b in zip(c0, c1)]
v = {"arm": arm, "cell": name, "rep": rep,
     "a_cores": "%.2f" % ((s1[0] - s0[0]) / tck / secs), "b_cores": "%.2f" % ((s1[1] - s0[1]) / tck / secs),
     "b_tx_mbit": "%.0f" % ((s1[2] - s0[2]) * 8 / 1e6 / secs), "b_tx_pkts_s": "%.0f" % ((s1[3] - s0[3]) / secs),
     "driver_softirq_cores": "%.2f" % ((s1[4] - s0[4]) / tck / secs),
     "relay_sent": d[0], "relay_recv": d[1], "relay_lost": d[2], "queue_dropped": d[3],
     "bus_publish_sent": d[4], "bus_publish_received": d[5],
     "b_clients_max_outbuf": c1[6], "b_outbuf_disconnects": d[7]}
print("\t".join(str(v.get(c, r.get(c, ""))) for c in cols.split()))
PY
	say "$arm $name rep $rep: $(tail -1 "$TSV" | cut -f4-11 | tr '\t' ' ')"
	sleep 1
}

CELLS=(
	"s1-paced20k -S 1 -s 1 -P 8 -t 2 -r 20000 -w 64"
	"s1-max -S 1 -s 1 -P 8 -t 2 -r 0 -w 16"
	"s64-paced20k -S 64 -s 5 -P 8 -t 2 -r 20000 -w 64"
	"s64-max -S 64 -s 5 -P 8 -t 2 -r 0 -w 16"
)
for rep in $(seq 1 "$REPS"); do
	arms=$ARMS
	[ $((rep % 2)) = 0 ] && arms=$(echo "$ARMS" | tr ' ' '\n' | tac | tr '\n' ' ')
	for arm in $arms; do
		start_arm "$arm" || exit 1
		for c in "${CELLS[@]}"; do
			set -- $c
			n=$1
			shift
			case " ${ONLY:-$n} " in *" $n "*) cell "$arm" "$rep" "$n" "$@" ;; esac
		done
		stop_arm "$arm"
	done
done
say "rpsrelay: $TSV"
