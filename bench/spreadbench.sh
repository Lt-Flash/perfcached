#!/bin/sh
# spreadbench.sh - spread mode at CAPACITY, from an off-box farm: throughput
# and latency by K and P, routed and un-routed, with eager on the same fleet
# as the yardstick.  Neither of the two spread measurements before this was
# a speed: Z6 (DESIGN 12cz) measured apply RATIOS at a fixed rate, and the
# 2026-09-14 RESP run drove one node at pipeline 1 from one host - "what
# one client gets per request", bounded by the round trip.
#
# Shape (the Z6 rig): P bare daemons from THIS tree's binary on the server
# host, distinct ports, advertise 127.0.7.i, multicast on the host; natbench
# (this tree's, over libperfd, BINARY dialect, pipelined) on the client
# host(s), which must not be the server host - see the client-bound lesson.
# route=0 sends every request to node 1, which forwards writes to the
# holders and pulls reads from them; route=1 lets libperfd 0.2.8 compute
# each key's holders and send to one - the shape a cluster-aware client has.
#
# Every cell records the server's CPU-seconds per node, so a figure can be
# read as a server ceiling or as a client-bound one, and the forward / pull
# / spread counters, so the mode's mechanics are visible beside its rate.
#
# Addresses: P daemons on one host need P distinct addresses for their peer
# sockets, and the advertise address is ALSO what the members reply hands a
# routed client - 127.0.7.i (the Z6 rig) is loopback on the server and a
# routed client on another host is refused.  So the server gets P private
# addresses VNET.1..P on its NIC and every client host a route to VNET/24
# via the server; both are removed on exit, and the harness refuses to
# start if either already exists and is not its own.
#
# Cells: route=0 runs one natbench per node, conns split evenly - the shape
# of a dumb load-balanced client, where a spread node holds a key K of P
# times and pulls the rest; route=1 runs ONE natbench that lets libperfd
# send each key to a holder.  eager has no route=1 cell: libperfd does not
# route it, and every node holds everything anyway.
#
# Usage (from a client host; drives the server host over ssh as root):
#   SERVER=<server ip> CLIENTS="<this host's ip> [<more client ips>]" \
#       bench/spreadbench.sh                   # ARMS below, REPS=1, ~30 min
#   ... ARMS="spread:3:6" REPS=3 bench/spreadbench.sh
#   ... DRY=1 bench/spreadbench.sh             # one arm up, one 5 s cell, down
# The daemons' HTTP doors admit every client host's /24; the client
# secret is the harness's own and the doors bind the private addresses.
# Results: /var/tmp/spreadbench/results.tsv (appended), logs beside it.
set -u
SERVER=${SERVER:-}
CLIENTS=${CLIENTS:-}
[ -n "$SERVER" ] && [ -n "$CLIENTS" ] || { echo "spreadbench: SERVER=<ip> and CLIENTS=\"<ip> ...\" are required" >&2; exit 2; }
HTTP_ALLOW=$(for c in $CLIENTS; do echo "$c" | awk -F. '{printf "%s.%s.%s.0/24\n", $1, $2, $3}'; done | sort -u | paste -sd, - | sed 's/,/, /g')
ARMS=${ARMS:-"eager:0:3 spread:2:3 spread:3:3 eager:0:6 spread:2:6 spread:3:6"}
REPS=${REPS:-1}
SECS=${SECS:-30}
FILL_SECS=${FILL_SECS:-20}
KEYS=${KEYS:-200000}
VAL=${VAL:-200}
CONNS=${CONNS:-64}
THREADS=${THREADS:-8}
DEPTH=${DEPTH:-32}
MIXES=${MIXES:-"set100 get100 get90"}
ROUTES=${ROUTES:-"0 1"}
DRY=${DRY:-0}
D=${D:-/var/tmp/spreadbench}
RD=/var/tmp/spreadbench-srv
VNET=${VNET:-10.77.0}
SNIC=${SNIC:-}
SSH="ssh -o BatchMode=yes -o ConnectTimeout=8"
mkdir -p "$D"
TSV=$D/results.tsv
HERE=$(cd "$(dirname "$0")/.." && pwd)
REV=$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo unknown)
STAMP=$(date +%Y%m%d-%H%M%S)
LOG=$D/run-$STAMP.log
say() { echo "$*" | tee -a "$LOG"; }
die() { say "spreadbench: $*"; exit 1; }
[ "$DRY" = 1 ] && { ARMS="spread:2:3"; REPS=1; SECS=5; FILL_SECS=5; MIXES="get90"; ROUTES="0 1"; }

# ---- the hosts must be idle, and ours ----
LOCAL_IP=$(hostname -I | awk '{print $1}')
on() { h=$1; shift; if [ "$h" = "$LOCAL_IP" ]; then sh -c "$*"; else $SSH root@$h "$*"; fi; }
for h in $SERVER $CLIENTS; do
	l=$(on $h 'cut -d" " -f1 /proc/loadavg; pgrep -xc perfcached' 2>/dev/null | tr '\n' ' ')
	set -- $l
	[ -n "${1:-}" ] || die "$h unreachable"
	awk -v l="$1" -v h="$h" -v max="$([ "$h" = "$SERVER" ] && echo 1.5 || echo 3.0)" 'BEGIN{ if (l+0 > max+0) { printf "spreadbench: %s load %s > %s - a benchmark on a busy box measures the other tenant\n", h, l, max; exit 1 } }' || exit 1
	[ "$h" = "$SERVER" ] && [ "${2:-0}" != 0 ] && die "$SERVER already runs $2 perfcached process(es)"
done

# ---- the private addresses on the server, and a route to them on every client ----
[ -n "$SNIC" ] || SNIC=$($SSH root@$SERVER "ip -o -4 addr show | awk -v ip=$SERVER '\$4 ~ ip\"/\" {print \$2; exit}'")
[ -n "$SNIC" ] || die "cannot find the NIC carrying $SERVER"
MAXP=$(echo "$ARMS" | tr ' ' '\n' | awk -F: '{if ($3+0 > m) m = $3+0} END{print m}')
net_up() {
	if $SSH root@$SERVER "ip -o -4 addr show dev $SNIC | grep -q ' $VNET\.'"; then
		$SSH root@$SERVER "ip -o -4 addr show dev $SNIC | grep ' $VNET\\.' | grep -vq ':sb[0-9]'" && die "$SERVER already carries $VNET.x on $SNIC and it is not ours (no :sb label)"
	fi
	i=1
	while [ $i -le $MAXP ]; do
		$SSH root@$SERVER "ip addr add $VNET.$i/24 dev $SNIC label $SNIC:sb$i 2>/dev/null; true"
		i=$((i+1))
	done
	for c in $CLIENTS; do
		on $c "ip route show $VNET.0/24" | grep -q "$VNET" && ! on $c "ip route show $VNET.0/24" | grep -q "via $SERVER" && die "$c already routes $VNET.0/24 elsewhere"
		on $c "ip route replace $VNET.0/24 via $SERVER"
	done
	say "  net: $VNET.1..$MAXP on $SERVER:$SNIC, route via $SERVER on [$CLIENTS]"
}
net_down() {
	i=1
	while [ $i -le $MAXP ]; do
		$SSH root@$SERVER "ip addr del $VNET.$i/24 dev $SNIC 2>/dev/null; true"
		i=$((i+1))
	done
	for c in $CLIENTS; do on $c "ip route del $VNET.0/24 2>/dev/null; true"; done
	left=$($SSH root@$SERVER "ip -o -4 addr show dev $SNIC | grep -c ' $VNET\.'")
	say "  net down: $left $VNET.x address(es) left on $SERVER (want 0)"
}

# ---- ship the binaries, assert what runs ----
[ -x "$HERE/perfcached" ] && [ -x "$HERE/natbench" ] || die "build perfcached and natbench first"
scp -q "$HERE/perfcached" root@$SERVER:$RD.perfcached || die "scp to $SERVER"
SREV=$($SSH root@$SERVER "$RD.perfcached -V 2>&1 | head -1")
say "server $SERVER runs: $SREV   (tree $REV)"
echo "$SREV" | grep -q "($REV)" || say "  NOTE: the server binary's stamp is not the tree's HEAD - it is the build's; read the line above, not the tree"
for c in $CLIENTS; do
	[ "$c" = "$LOCAL_IP" ] && continue
	scp -q "$HERE/natbench" root@$c:$RD.natbench || die "scp natbench to $c"
done
nb_path() { [ "$1" = "$LOCAL_IP" ] && echo "$HERE/natbench" || echo "$RD.natbench"; }

# ---- server-side control: up <mode> <K> <P>, down, cpu, stats ----
srv() { $SSH root@$SERVER "sh -s" ; }
fleet_up() {
	mode=$1; k=$2; p=$3
	w=$(( 15 / p )); [ $w -lt 2 ] && w=2
	modeblk="mode = $mode"; [ "$mode" = spread ] && modeblk="mode = spread
replicas = $k"
	srv <<EOF
set -u
mkdir -p $RD; rm -rf $RD/node.* $RD/state.*; : > $RD/pids
i=1
while [ \$i -le $p ]; do
	mkdir -p $RD/state.\$i
	cat > $RD/node.\$i.conf <<CONF
[daemon]
workers = $w
log_level = crit
state_dir = $RD/state.\$i
shrink_cooloff_s = 86400
[memory]
arena_mb = $(( p <= 3 ? 1024 : 512 ))
[secrets]
client = spreadbench-client-secret
cluster = spreadbench-cluster-secret
[listen]
tcp = $VNET.\$i:\$((17400 + i))
http = $SERVER:\$((18400 + i))
http_allow = $HTTP_ALLOW
plaintext = loopback
[cluster]
multicast = 239.255.77.70:17170
advertise = $VNET.\$i
$modeblk
collections = b
[collection b]
buckets_log2 = 17
pull = 1
CONF
	$RD.perfcached -f $RD/node.\$i.conf > $RD/node.\$i.log 2>&1 &
	echo \$! >> $RD/pids
	i=\$((i+1))
done
EOF
	# converge: every node's /members lists P members
	t=0
	while [ $t -lt 60 ]; do
		n=0; i=1
		while [ $i -le $p ]; do
			m=$(curl -s -m 3 http://$SERVER:$((18400 + i))/members | python3 -c 'import json,sys
try: print(len(json.load(sys.stdin)["members"]))
except Exception: print(0)' 2>/dev/null)
			[ "${m:-0}" = "$p" ] && n=$((n+1))
			i=$((i+1))
		done
		[ $n = $p ] && break
		sleep 1; t=$((t+1))
	done
	[ $n = $p ] || die "fleet $mode K=$k P=$p did not converge in 60s ($n of $p nodes see $p members)"
	say "  fleet up: $mode K=$k P=$p, $w workers/node, converged in ${t}s"
}
fleet_down() {
	srv <<EOF
for pid in \$(cat $RD/pids 2>/dev/null); do kill \$pid 2>/dev/null; done
sleep 1
for pid in \$(cat $RD/pids 2>/dev/null); do kill -9 \$pid 2>/dev/null; done
: > $RD/pids
EOF
}
cpu_ticks() {   # sum of utime+stime over the fleet, and the max single node
	srv <<EOF
tot=0; max=0
for pid in \$(cat $RD/pids); do
	t=\$(awk '{print \$14+\$15}' /proc/\$pid/stat 2>/dev/null || echo 0)
	tot=\$((tot + t)); [ \$t -gt \$max ] && max=\$t
done
echo \$tot \$max \$(getconf CLK_TCK)
EOF
}
counters() {   # sum over nodes of the named counters, wherever they nest
	i=1; out=""
	while [ $i -le $1 ]; do out="$out $(curl -s -m 3 http://$SERVER:$((18400 + i))/stats)"; i=$((i+1)); done
	printf '%s' "$out" | python3 -c '
import json, sys, re
raw = sys.stdin.read()
want = ["fwd_sent", "pull_sent", "pull_served", "spread_not_held", "entries"]
tot = dict((w, 0) for w in want)
dec = json.JSONDecoder(); i = 0
while i < len(raw):
    j = raw.find("{", i)
    if j < 0: break
    try:
        obj, end = dec.raw_decode(raw, j)
    except Exception:
        i = j + 1; continue
    def walk(o):
        if isinstance(o, dict):
            for k, v in o.items():
                if k in tot and isinstance(v, (int, float)): tot[k] += v
                walk(v)
        elif isinstance(o, list):
            for v in o: walk(v)
    walk(obj); i = end
print(" ".join(str(tot[w]) for w in want))'
}

# ---- one cell: natbench on every client host, in parallel ----
# route=0: one instance per node, conns split - a dumb load-balanced client.
# route=1: one instance at node 1 that lets libperfd fan out to the holders.
nb() {   # <client> <host> <port> <conns> <threads> <getpct> <route> <secs> <outfile>
	c=$1; h=$2; pt=$3; cn=$4; th=$5; gp=$6; rt=$7; sc=$8; of=$9
	if [ "$c" = "$LOCAL_IP" ]; then
		"$(nb_path $c)" $h $pt spreadbench-client-secret b $cn $th $DEPTH $sc $KEYS $VAL $gp 1 50 -1 $rt > $of 2>&1 &
	else
		$SSH root@$c "$RD.natbench $h $pt spreadbench-client-secret b $cn $th $DEPTH $sc $KEYS $VAL $gp 1 50 -1 $rt" > $of 2>&1 &
	fi
}
cell() {   # <getpct> <route> <secs> <P> -> prints: ops p50 p99 p999 ccores hits missed instances
	getpct=$1; route=$2; secs=$3; p=$4
	rm -f $D/cell.*.out
	for c in $CLIENTS; do
		if [ "$route" = 1 ]; then
			nb $c $VNET.1 17401 $CONNS $THREADS $getpct 1 $secs $D/cell.$c.r.out
		else
			cn=$(( CONNS / p )); th=$(( THREADS / p )); [ $th -lt 1 ] && th=1
			i=1
			while [ $i -le $p ]; do
				nb $c $VNET.$i $((17400 + i)) $cn $th $getpct 0 $secs $D/cell.$c.n$i.out
				i=$((i+1))
			done
		fi
	done
	wait
	cat $D/cell.*.out | python3 -c '
import re, sys
ops = ccpu = 0.0; p50 = p99 = p999 = 0.0; hits = missed = 0; n = 0
for line in sys.stdin:
    m = re.search(r"([\d.]+) ops/s over", line)
    if m: ops += float(m.group(1)); n += 1
    m = re.search(r"p50=([\d.]+)ms p99=([\d.]+)ms p999=([\d.]+)ms", line)
    if m: p50 = max(p50, float(m.group(1))); p99 = max(p99, float(m.group(2))); p999 = max(p999, float(m.group(3)))
    m = re.search(r"= ([\d.]+) cores", line)
    if m: ccpu += float(m.group(1))
    m = re.search(r"payload: (\d+)", line)
    if m: hits += int(m.group(1))
    m = re.search(r"route_missed=(\d+)", line)
    if m: missed += int(m.group(1))
print("%.0f %.3f %.3f %.3f %.2f %d %d %d" % (ops, p50, p99, p999, ccpu, hits, missed, n))'
}

[ -s "$TSV" ] || printf 'stamp\trev\trep\tmode\tK\tP\tmix\troute\tops_s\tp50_ms\tp99_ms\tp999_ms\tclient_cores\tserver_cores\tserver_cores_max\thits\troute_missed\tfwd_sent\tpull_sent\tpull_served\tspread_not_held\tclients\n' > "$TSV"
say "spreadbench $STAMP: server=$SERVER clients=[$CLIENTS] arms=[$ARMS] reps=$REPS secs=$SECS keys=$KEYS val=$VAL conns=$CONNS threads=$THREADS depth=$DEPTH"
NET_UP=0
cleanup() { [ "$NET_UP" = 1 ] || return 0; NET_UP=0; fleet_down; net_down; }
trap 'say "interrupted: fleet and net down"; cleanup; exit 130' INT TERM
trap 'cleanup' EXIT
net_up; NET_UP=1
rep=1
while [ $rep -le $REPS ]; do
	for arm in $ARMS; do
		mode=${arm%%:*}; rest=${arm#*:}; k=${rest%%:*}; p=${rest#*:}
		say "== rep $rep  $mode K=$k P=$p =="
		fleet_up $mode $k $p
		# the fill: writes at holders, then every node must hold something
		fr=1; [ "$mode" = eager ] && fr=0
		r=$(cell 0 $fr $FILL_SECS $p); set -- $r
		say "  fill (route=$fr): $1 ops/s over ${FILL_SECS}s, $8 instance(s), route_missed=$7"
		set -- $(counters $p)
		say "  after the fill: entries=$5 across the fleet (keys=$KEYS, K=$k -> want ~$(( KEYS * (k > 0 ? k : p) )))"
		for mix in $MIXES; do
			getpct=${mix#set}; getpct=${getpct#get}; case $mix in set*) getpct=0;; esac
			for route in $ROUTES; do
				[ "$mode" = eager ] && [ "$route" = 1 ] && continue
				set -- $(cpu_ticks); t0=$1; m0=$2; hz=$3
				set -- $(counters $p); f0=$1; ps0=$2; pv0=$3; sn0=$4
				r=$(cell $getpct $route $SECS $p); set -- $r
				ops=$1; p50=$2; p99=$3; p999=$4; ccores=$5; hits=$6; missed=$7; nclients=$8
				set -- $(cpu_ticks); t1=$1; m1=$2
				set -- $(counters $p); f1=$1; ps1=$2; pv1=$3; sn1=$4
				scores=$(awk -v a=$t0 -v b=$t1 -v hz=$hz -v s=$SECS 'BEGIN{printf "%.2f", (b-a)/hz/s}')
				smax=$(awk -v a=$m0 -v b=$m1 -v hz=$hz -v s=$SECS 'BEGIN{printf "%.2f", (b-a)/hz/s}')
				printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
					"$STAMP" "$REV" "$rep" "$mode" "$k" "$p" "$mix" "$route" "$ops" "$p50" "$p99" "$p999" "$ccores" "$scores" "$smax" "$hits" "$missed" "$((f1-f0))" "$((ps1-ps0))" "$((pv1-pv0))" "$((sn1-sn0))" "$nclients" >> "$TSV"
				say "  $mix route=$route: $ops ops/s  p50=${p50}ms p99=${p99}ms  client ${ccores} cores  server ${scores} cores (max node ${smax})  fwd=$((f1-f0)) pull=$((ps1-ps0)) served=$((pv1-pv0)) not_held=$((sn1-sn0)) missed=$missed hits=$hits"
			done
		done
		fleet_down
		say "  fleet down"
	done
	rep=$((rep+1))
done
cleanup
say "done: $TSV"
