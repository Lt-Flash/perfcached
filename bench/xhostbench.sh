#!/bin/sh
# xhostbench.sh - redis-benchmark on one host against redis and perfcached on another, over a real
# NIC (MTU 9000 verified), arms alternated.  Usage: SERVER=<ip> CLIENT=<ip> bench/xhostbench.sh [reps]
set -u

SERVER=${SERVER:?set SERVER=<ip of the host that runs both servers>}
CLIENT=${CLIENT:?set CLIENT=<ip of the host that runs redis-benchmark>}
REPS=${1:-3}
SRCDIR=${SRCDIR:-/dn/perfcached}       # tree on the SERVER host
# respbench.sh's parameters, so the tables compare; four workers, not nproc against a
# one-thread Redis
N=${N:-100000}; KEYSPACE=${KEYSPACE:-20000}; VAL=${VAL:-200}
N_MAX=${N_MAX:-2000000}   # cap on N x pipeline per cell, as respbench.sh
CLIENTS=${CLIENTS:-50}
PIPES=${PIPES:-"1 64"}
RPORT=16600     # redis
PPORT=16601     # perfcached's RESP door
NPORT=16602     # perfcached's native door (unused, must exist)
OUT=${OUT:-bench/results/xhostbench.tsv}
IMG=${IMG:-perfcached:bench}
REDIS_IMG=${REDIS_IMG:-docker.io/library/redis:8}
RCON=xhb-redis; PCON=xhb-perf; DCON=xhb-drive

ss_() { ssh -o ConnectTimeout=10 "root@$SERVER" "$@"; }
sc_() { ssh -o ConnectTimeout=10 "root@$CLIENT" "$@"; }

# ---- preconditions, all of them loud ----------------------------------
BUSY=$(ss_ 'ps -eo comm --no-headers | grep -cE "^(perfcached|pcbench|make|fio|redis-benchmark|redis-server)$"')
[ "${BUSY:-1}" -gt 0 ] && { echo "xhostbench: $SERVER is busy ($BUSY procs) - refusing"; exit 1; }
sc_ "ping -c1 -W2 -M do -s 8972 $SERVER >/dev/null 2>&1" \
	|| { echo "xhostbench: jumbo path $CLIENT -> $SERVER NOT verified (8972 fragments) - refusing"; exit 1; }

# a runtime on each host: `info`, not `command -v`, so a client whose daemon is down fails here
findrt='for c in podman nerdctl docker; do command -v $c >/dev/null 2>&1 && $c info >/dev/null 2>&1 && { echo $c; break; }; done'
RTS=${RUNTIME_SRV:-$(ss_ "$findrt")}
RTC=${RUNTIME_CLI:-$(sc_ "$findrt")}
[ -n "$RTS" ] || { echo "xhostbench: no working container runtime on $SERVER"; exit 1; }
[ -n "$RTC" ] || { echo "xhostbench: no working container runtime on $CLIENT"; exit 1; }

# ---- images, built on the server from its own checkout
ss_ "cd $SRCDIR && $RTS build --platform linux/amd64 \
	-f bench/Containerfile.debian \
	--build-arg REV=\$(git rev-parse --short HEAD 2>/dev/null || echo unknown) \
	-t $IMG . >/dev/null 2>&1" \
	|| { echo "xhostbench: image build failed on $SERVER"; exit 1; }
sc_ "$RTC pull $REDIS_IMG >/dev/null 2>&1" \
	|| { echo "xhostbench: could not pull $REDIS_IMG on $CLIENT"; exit 1; }

# read every version out of the image that will run, never from the host or the directory name
BINREV=$(ss_ "$RTS run --rm --entrypoint perfcached $IMG -V 2>/dev/null | sed -n 's/.*(\(.*\)).*/\1/p'")
RSV=$(ss_ "$RTS run --rm --entrypoint redis-server $REDIS_IMG --version | grep -oE 'v=[0-9.]+' | cut -c3-")
RBV=$(sc_ "$RTC run --rm --entrypoint redis-benchmark $REDIS_IMG --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+'")
[ -n "$BINREV" ] && [ "$BINREV" != unknown ] || {
	echo "xhostbench: refusing to measure an unstamped build ($BINREV)"; exit 1; }
WORKERS=${WORKERS:-4}
MTU=$(sc_ "ip route get $SERVER | grep -oE 'dev [a-z0-9]+' | awk '{print \$2}' | xargs -I{} cat /sys/class/net/{}/mtu")

echo "server $SERVER: perfcached $BINREV, redis $RSV, $WORKERS workers ($RTS)"
echo "client $CLIENT: redis-benchmark $RBV, MTU $MTU, jumbo path verified ($RTC)"

# ---- servers on the SERVER host, bound to its NIC address --------------
D=$(ss_ 'mktemp -d /var/tmp/xhb.XXXXXX')
cleanup() {
	ss_ "$RTS rm -f $RCON $PCON >/dev/null 2>&1"
	sc_ "$RTC rm -f $DCON >/dev/null 2>&1"
	ss_ "rm -rf $D"
}
trap cleanup EXIT
ss_ "$RTS rm -f $RCON $PCON >/dev/null 2>&1; $RTC rm -f $DCON >/dev/null 2>&1" || true

# --network host on both servers: a bridge would insert a 1500-byte hop into the 9000-byte path,
# and a detached container holds no fd of the ssh session
ss_ "$RTS run -d --name $RCON --network host $REDIS_IMG \
	--port $RPORT --bind $SERVER --protected-mode no --save '' \
	--appendonly no >/dev/null 2>&1" \
	|| { echo "xhostbench: redis container did not start"; exit 1; }
ss_ "cat > $D/pc.conf <<EOF
[daemon]
workers = $WORKERS
log_level = notice
[memory]
arena_mb = 512
[secrets]
client = xhb-client-secret
cluster = xhb-cluster-secret
[listen]
tcp = $SERVER:$NPORT
resp_allow = $CLIENT/32
resp = $SERVER:$PPORT
[collection 0]
buckets_log2 = 17
EOF"
# a container's default memlock is below the arena: mlock() would fail and leave it swappable
ss_ "$RTS run -d --name $PCON --network host --ulimit memlock=-1 \
	-v $D/pc.conf:/etc/perfcached.conf:ro $IMG >/dev/null 2>&1" \
	|| { echo "xhostbench: perfcached container did not start"; exit 1; }
# readiness is the driver's PING below, not the logs: a detached container's logs are empty

# a PERSISTENT driver on the client, so the per-cell cost is an exec and
# not a container start
sc_ "$RTC run -d --name $DCON --network host --entrypoint tail \
	$REDIS_IMG -f /dev/null >/dev/null 2>&1" \
	|| { echo "xhostbench: driver container did not start on $CLIENT"; exit 1; }
ping_wait() { # ping_wait <port> <what>: up to 20 s for a PONG
	i=0; while [ $i -lt 100 ]; do
		sc_ "$RTC exec $DCON redis-cli -h $SERVER -p $1 PING 2>/dev/null" | grep -q PONG && return 0
		sleep 0.2; i=$((i+1)); done
	echo "$2 not answering on $SERVER:$1"; return 1
}
ping_wait $RPORT "redis" || exit 1
ping_wait $PPORT "perfcached RESP door" || exit 1

# ---- one cell, on the client.  redis-benchmark times in whole milliseconds, so N x pipeline
# (capped at N_MAX) keeps every cell above a second, as in respbench.sh
reqs() { r=$((N * $1)); [ "$r" -gt "$N_MAX" ] && r=$N_MAX; echo "$r"; }
point() { # point <port> <clients> <pipeline>  -> "sets gets sp50 gp50 sp99 gp99"
	sc_ "$RTC exec $DCON redis-benchmark -h $SERVER -p $1 -t set,get \
		-n $(reqs $3) -c $2 -P $3 -r $KEYSPACE -d $VAL --csv 2>/dev/null" \
	| awk -F',' '
		function unq(s) { gsub(/"/, "", s); return s }
		unq($1) == "SET" { sr = unq($2); sp50 = unq($5); sp99 = unq($7) }
		unq($1) == "GET" { gr = unq($2); gp50 = unq($5); gp99 = unq($7) }
		END { if (sr == "" || gr == "") exit 1
			printf "%.0f %.0f %s %s %s %s\n", sr, gr, sp50, gp50, sp99, gp99 }'
}
med() { tr ' ' '\n' | grep -E '^[0-9]+(\.[0-9]+)?$' | sort -g | awk '
	{v[NR]=$1} END { if (NR==0) {print 0; exit}
	print (NR%2) ? v[(NR+1)/2] : (v[NR/2]+v[NR/2+1])/2 }'; }

TMP=$(mktemp -d)
trap 'cleanup; rm -rf "$TMP"' EXIT
for c in $CLIENTS; do for p in $PIPES; do
	for rep in $(seq 1 "$REPS"); do
		# ALTERNATE arms within every rep - drift cannot favour one
		for arm in redis perfcached; do
			[ "$arm" = redis ] && port=$RPORT || port=$PPORT
			r=$(point $port $c $p) && echo "$r" >> "$TMP/$arm-$c-$p" \
				|| echo "  $arm c=$c P=$p rep $rep: no result" >&2
		done
	done
done; done

# ---- report, stamped --------------------------------------------------
mkdir -p "$(dirname "$OUT")"
{
	printf '# build=%s date=%s hosts=2 containers=%s/%s redis=%s redis-benchmark=%s mtu=%s keys=%s reqs=%sxP nmax=%s val=%s reps=%s\n' \
		"$BINREV" "$(date -u +%FT%TZ)" "$RTS" "$RTC" "$RSV" "$RBV" "$MTU" "$KEYSPACE" "$N" "$N_MAX" "$VAL" "$REPS"
	printf 'arm\tclients\tpipeline\tset_per_s\tget_per_s\tset_p50ms\tget_p50ms\tset_p99ms\tget_p99ms\tset_reps\n'
	for c in $CLIENTS; do for p in $PIPES; do for arm in redis perfcached; do
		f="$TMP/$arm-$c-$p"; [ -s "$f" ] || continue
		printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$arm" "$c" "$p" \
			"$(cut -d' ' -f1 $f | med)" "$(cut -d' ' -f2 $f | med)" \
			"$(cut -d' ' -f3 $f | med)" "$(cut -d' ' -f4 $f | med)" \
			"$(cut -d' ' -f5 $f | med)" "$(cut -d' ' -f6 $f | med)" \
			"$(cut -d' ' -f1 $f | tr '\n' '/' | sed 's|/$||')"
	done; done; done
} > "$OUT"

echo; head -1 "$OUT"
awk -F'\t' 'NR>1 { r[$1 FS $2 FS $3]=$0 }
	END { for (k in r) if (k ~ /^redis/) {
		split(r[k], a, FS); pk=k; sub(/^redis/, "perfcached", pk)
		if (!(pk in r)) continue; split(r[pk], b, FS)
		printf "c=%-4s P=%-3s  SET %8d vs %8d (x%.2f)   GET %8d vs %8d (x%.2f)   SET p99 %.2f vs %.2f ms\n",
			a[2], a[3], b[4], a[4], b[4]/a[4], b[5], a[5], b[5]/a[5], b[8], a[8] } }' "$OUT" | sort
echo "(perfcached vs redis; medians of $REPS; per-rep SETs in the tsv)"
