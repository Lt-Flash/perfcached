#!/bin/bash
# psnoise.sh [reps] [secs] - pub/sub on the native door, plaintext against
# Noise, one host: the daemon pinned to SERVER_CPUS, bench/psbench.c's
# subscriber and publisher processes pinned to CLIENT_CPUS, loopback.
# `plaintext = loopback` with the secret `-` is the plaintext arm;
# `plaintext = never` with the client secret seals every connection.
#
#   PERFCACHED=./perfcached PSBENCH=./psbench bench/psnoise.sh 2 8
#
# Environment: PERFCACHED, PSBENCH [./perfcached ./psbench], SERVER_CPUS
# [0-7], CLIENT_CPUS [8-15], WORKERS [4], OUT [/tmp/psnoise].
#
# psbench counts, it does not time: the rows are publishes and deliveries
# a second, whether any subscriber got nothing, the server's CPU over the
# middle of the publish window, and each client process's own CPU.
# expected_deliv_s is publishes x subscribers; a shortfall against it with
# slow_kills means the subscribers could not read what the server wrote -
# the CLIENT's ceiling (a Noise subscriber decrypts too, and one host's
# client cores are shared by publisher and subscriber).  Take server
# figures only from rows without a shortfall.
set -u
REPS=${1:-2}
SECS=${2:-8}
PERFCACHED=${PERFCACHED:-./perfcached}
PSBENCH=${PSBENCH:-./psbench}
SRV=${SERVER_CPUS:-0-7}
CLI=${CLIENT_CPUS:-8-15}
OUT=${OUT:-/tmp/psnoise}
SECRET=psnoise-client-secret-0123456789
TCK=$(getconf CLK_TCK)
mkdir -p "$OUT" || exit 1
TSV=$OUT/results.tsv
PID=
stopd() { [ -n "$PID" ] && kill $PID 2>/dev/null; sleep 1; [ -n "$PID" ] && kill -9 $PID 2>/dev/null; PID=; }
cleanup() {
	stopd
	for p in $(pgrep -x psbench); do [ "$(readlink /proc/$p/exe)" = "$(readlink -f "$PSBENCH")" ] && kill $p; done
	true
}
trap cleanup EXIT
trap 'exit 130' INT TERM
for mode in plain noise; do
	cat > "$OUT/$mode.conf" <<CONF
[daemon]
workers = ${WORKERS:-4}
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = $SECRET
[listen]
tcp = 127.0.0.1:17981
http = 127.0.0.1:18981
plaintext = $([ $mode = plain ] && echo loopback || echo never)
[collection 0]
buckets_log2 = 12
CONF
	chmod 600 "$OUT/$mode.conf"
done
cols="mode cell rep subs pub_s deliv_s expected_deliv_s shortfall_pct got_nothing srv_cores pub_client_cores sub_client_cores queue_dropped publish_paused slow_kills"
{
	echo "# date=$(date -Is) host=$(hostname) cpus=$(nproc) perfcached=$("$PERFCACHED" -V) reps=$REPS secs=$SECS workers=${WORKERS:-4} server_cpus=$SRV client_cpus=$CLI door=native-json dialect via libperfd"
	echo "$cols" | tr ' ' '\t'
} > "$TSV"
pstat() { curl -s -m 5 http://127.0.0.1:18981/stats | python3 -c 'import json,sys; p=json.load(sys.stdin)["pubsub"]; print(p["queue_dropped"], p["publish_paused"], p["slow_kills"])'; }
cell() { # cell <mode> <rep> <name> <subs> <subthreads> <pubs> <pubthreads> <valsize> [rate]
	mode=$1 rep=$2 name=$3 subs=$4 sthr=$5 pubs=$6 pthr=$7 val=$8 rate=${9:-}
	sec=-
	[ $mode = noise ] && sec=$SECRET
	set -- $(pstat)
	q0=$1 p0=$2 k0=$3
	taskset -c "$CLI" "$PSBENCH" sub 127.0.0.1 17981 $sec bench $subs $sthr $((SECS + 6)) > "$OUT/sub.out" 2>&1 &
	sp=$!
	sleep 3
	taskset -c "$CLI" "$PSBENCH" pub 127.0.0.1 17981 $sec bench $pubs $pthr $SECS $val $rate > "$OUT/pub.out" 2>&1 &
	pp=$!
	sleep 2
	c0=$(awk '{print $14+$15}' /proc/$PID/stat)
	sleep $((SECS - 3))
	c1=$(awk '{print $14+$15}' /proc/$PID/stat)
	wait $pp
	wait $sp
	set -- $(pstat)
	pub=$(grep -o 'published [0-9]* in [0-9.]*s = [0-9]*/s' "$OUT/pub.out" | grep -o '= [0-9]*' | cut -c3-)
	del=$(grep -o 'delivered [0-9]* in [0-9.]*s = [0-9]*/s' "$OUT/sub.out" | grep -o '= [0-9]*' | cut -c3-)
	none=$(grep -o 'got NOTHING: [0-9]*' "$OUT/sub.out" | grep -o '[0-9]*$')
	pc=$(grep -o '= [0-9.]* cores' "$OUT/pub.out" | grep -o '[0-9.]*')
	sc=$(grep -o '= [0-9.]* cores' "$OUT/sub.out" | grep -o '[0-9.]*')
	srv=$(python3 -c "print('%.2f' % (($c1-$c0)/$TCK/($SECS-3)))")
	exp=$(( ${pub:-0} * subs ))
	short=$(python3 -c "e=$exp; d=${del:-0}; print('%.1f' % (100.0 * (e - d) / e if e else 0))")
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $mode $name $rep $subs "$pub" "$del" $exp $short "$none" $srv "$pc" "$sc" $(($1 - q0)) $(($2 - p0)) $(($3 - k0)) >> "$TSV"
	echo "$mode $name rep $rep: pub $pub/s deliv $del/s (shortfall $short%, slow kills $(($3 - k0))) srv $srv cores"
}
for rep in $(seq 1 "$REPS"); do
	modes="plain noise"
	[ $((rep % 2)) = 0 ] && modes="noise plain"
	for mode in $modes; do
		taskset -c "$SRV" "$PERFCACHED" -f "$OUT/$mode.conf" > "$OUT/$mode.log" 2>&1 &
		PID=$!
		for i in $(seq 1 100); do grep -q "perfcached ready" "$OUT/$mode.log" && break; sleep 0.1; done
		cell $mode $rep s1-max 1 1 8 2 64
		cell $mode $rep s64-paced20k 64 4 8 2 64 20000
		cell $mode $rep s64-max 64 4 8 2 64
		cell $mode $rep s8-max-1KB 8 2 8 2 1024
		stopd
	done
done
echo "psnoise: $TSV"
