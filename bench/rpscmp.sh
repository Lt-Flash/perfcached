#!/bin/bash
# rpscmp.sh - RESP pub/sub, Redis against perfcached, one client
# (bench/rpsbench.c) on one host.  Each server runs in a container pinned
# to SRV_CPUS on the host network, bound to loopback; the driver runs on
# the host pinned to CLI_CPUS, so publishers and subscribers share a
# clock and the servers never share the client's cores.
#
#   IMG_PC=localhost/perfcached:<rev> bench/rpscmp.sh [reps] [secs]
#
# IMG_PC is a bench/Containerfile.debian image (required).  Environment:
#   REDIS_IMG  docker.io/library/redis:8       RUNTIME   podman
#   ARMS       "redis-io1 redis-io4 pc-w4"      (also pc-w1)
#   SRV_CPUS   0-7    CLI_CPUS   8-15           OUT       /var/tmp/rpscmp
#
# Cells, 64-byte payloads, 8 publisher connections on 2 threads:
#   s1-paced100k   1 subscriber, 100k publishes/s     (latency at a rate)
#   s1-max         1 subscriber, closed loop, 16 in flight per connection
#   s64-paced20k   64 subscribers on 5 threads, 20k publishes/s
#   s64-max        64 subscribers, closed loop
# Arms alternate order between reps.  Writes $OUT/results.tsv: '# ' lines
# naming the builds and each arm's server, then a row per cell and rep.
# Read drv_cores and busiest_thr before a number: a busiest driver thread
# near 1.00 measured the client.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPS=${1:-2}
SECS=${2:-8}
RT=${RUNTIME:-podman}
REDIS_IMG=${REDIS_IMG:-docker.io/library/redis:8}
ARMS=${ARMS:-"redis-io1 redis-io4 pc-w4"}
SRV=${SRV_CPUS:-0-7}
CLI=${CLI_CPUS:-8-15}
OUT=${OUT:-/var/tmp/rpscmp}
[ -n "${IMG_PC:-}" ] || { echo "rpscmp: set IMG_PC to a perfcached bench image"; exit 2; }
mkdir -p "$OUT" || exit 1
TSV=$OUT/results.tsv
NAME_R=rpscmp-redis-$$
NAME_P=rpscmp-pc-$$

cleanup() { $RT rm -f "$NAME_R" "$NAME_P" >/dev/null 2>&1; }
trap cleanup EXIT
trap 'cleanup; exit 1' INT TERM

cc -O2 -std=gnu11 -Wall -Wextra -o "$OUT/rpsbench" "$HERE/rpsbench.c" -lpthread || exit 1
for w in 1 4; do
	# plaintext = loopback: the native door is unused here, and 0.3.8-rc1
	# through 0.4.0-rc3 crash with an encrypted door and no cluster secret
	cat > "$OUT/pc-w$w.conf" <<CONF
[daemon]
workers = $w
log_level = notice
[memory]
arena_mb = 64
[secrets]
client = rpscmp-client-secret-0123456789
[listen]
tcp = 127.0.0.1:16479
resp = 127.0.0.1:16380
http = 127.0.0.1:16481
plaintext = loopback
[collection 0]
buckets_log2 = 12
CONF
	chmod 640 "$OUT/pc-w$w.conf"
done

ping_port() { # a RESP PING on loopback, answered PONG within ~10 s
	for _ in $(seq 1 100); do
		if exec 3<>"/dev/tcp/127.0.0.1/$1" 2>/dev/null; then
			printf '*1\r\n$4\r\nPING\r\n' >&3
			read -r -t 1 line <&3
			exec 3<&- 3>&-
			case "$line" in +PONG*) return 0 ;; esac
		fi
		sleep 0.1
	done
	return 1
}

start_arm() {
	case $1 in
	redis-io*)
		NAME=$NAME_R PORT=16379
		$RT run -d --name "$NAME" --network host --cpuset-cpus "$SRV" "$REDIS_IMG" \
			redis-server --bind 127.0.0.1 --port $PORT --save '' --appendonly no \
			--io-threads "${1#redis-io}" >/dev/null || return 1 ;;
	pc-w*)
		NAME=$NAME_P PORT=16380
		$RT run -d --name "$NAME" --network host --cpuset-cpus "$SRV" \
			--ulimit memlock=-1 -v "$OUT/$1.conf:/etc/perfcached.conf:ro" \
			"$IMG_PC" >/dev/null || return 1 ;;
	*)
		echo "rpscmp: unknown arm $1"; return 1 ;;
	esac
	ping_port $PORT || { echo "rpscmp: $1 did not answer"; $RT logs "$NAME" 2>&1 | tail -3; return 1; }
	PID=$($RT inspect -f '{{.State.Pid}}' "$NAME")
	echo "# arm=$1 comm=$(cat /proc/$PID/comm) threads=$(ls /proc/$PID/task | wc -l) $(grep Cpus_allowed_list /proc/$PID/status | tr -d '\t ')" >> "$TSV"
}

stop_arm() {
	if [ "$NAME" = "$NAME_R" ]; then
		echo "# arm=$1 after: $($RT exec "$NAME" redis-cli -p $PORT info stats | grep -E '^client_output_buffer_limit_disconnections' | tr -d '\r')" >> "$TSV"
	else
		echo "# arm=$1 after: $(curl -s -m 5 http://127.0.0.1:16481/stats | python3 -c 'import json,sys; p=json.load(sys.stdin)["pubsub"]; print(" ".join("%s=%s" % (k, p[k]) for k in ("published","delivered","queue_dropped","slow_kills")))')" >> "$TSV"
	fi
	$RT rm -f "$NAME" >/dev/null 2>&1
	sleep 1
}

CELLS=(
	"s1-paced100k -S 1 -s 1 -P 8 -t 2 -r 100000 -w 64"
	"s1-max -S 1 -s 1 -P 8 -t 2 -r 0 -w 16"
	"s64-paced20k -S 64 -s 5 -P 8 -t 2 -r 20000 -w 64"
	"s64-max -S 64 -s 5 -P 8 -t 2 -r 0 -w 16"
)
COLS="pub_s deliv_s p50_us p99_us p999_us max_us lost gaps ooo eof errs srv_cores srv_ctxsw_s drv_cores busiest_thr"

{
	echo "# date=$(date -Is) host=$(hostname) cpus=$(nproc) srv_cpus=$SRV cli_cpus=$CLI reps=$REPS secs=$SECS val=64"
	echo "# perfcached=$($RT run --rm --entrypoint perfcached "$IMG_PC" -V) redis=$($RT run --rm --entrypoint redis-server "$REDIS_IMG" --version | grep -o 'v=[0-9.]*')"
	printf 'arm\tcell\trep'
	for c in $COLS; do printf '\t%s' "$c"; done
	printf '\n'
} > "$TSV"

for rep in $(seq 1 "$REPS"); do
	arms=$ARMS
	[ $((rep % 2)) = 0 ] && arms=$(echo "$ARMS" | tr ' ' '\n' | tac | tr '\n' ' ')
	for arm in $arms; do
		start_arm "$arm" || exit 1
		for c in "${CELLS[@]}"; do
			set -- $c
			cell=$1
			shift
			line=$(timeout $((SECS + 40)) taskset -c "$CLI" "$OUT/rpsbench" \
				-p $PORT -x "$PID" -d "$SECS" -v 64 "$@" 2>&1 | grep '^RESULT')
			printf '%s\t%s\t%s' "$arm" "$cell" "$rep" >> "$TSV"
			for col in $COLS; do
				printf '\t%s' "$(echo "$line" | grep -o "\b$col=[^ ]*" | cut -d= -f2)" >> "$TSV"
			done
			printf '\n' >> "$TSV"
			echo "$arm $cell rep $rep: $(echo "$line" | cut -c1-160)"
			sleep 1
		done
		stop_arm "$arm"
	done
done
echo "rpscmp: $TSV"
