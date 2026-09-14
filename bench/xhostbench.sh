#!/bin/sh
# xhostbench.sh — the comparison run PROPERLY: redis-benchmark on one
# host, both servers on another, a real NIC between them.
#
# The README's "Across a real network" table (build 78b122b) was produced
# by hand.  This is that method as a script, so it can be re-run for a
# release instead of re-invented: MTU 9000 verified end to end with
# `ping -M do -s 8972` BEFORE anything is trusted, both servers bound to
# the server host's NIC address, arms ALTERNATED so drift cannot favour
# either, median of REPS per cell, and the build/versions stamped into
# the result.
#
# CONTAINERISED 2026-09-12.  Neither host needs redis-server, redis-
# benchmark or a perfcached build any more: the servers run from this
# tree's image on the server host and the load generator from redis:8 on
# the client host.  Both run with --network host, and that is not a
# shortcut - this bench exists to measure a 9000-byte path between two
# physical NICs, and a container bridge would put a 1500-byte hop in the
# middle and report it as the network.  --network host keeps the wire the
# wire; the containers supply only the binaries.
#
# It cannot use bench/containerlib.sh: that library drives containers on
# the machine it is sourced on, and every container here lives on one of
# two OTHER hosts, reached over ssh.
#
# Runs from a third machine (or the client) and drives both hosts over
# ssh.  Refuses to start if the server host is busy: a benchmark on a
# shared box measures the other tenant.
#
#   SERVER=<server-ip> CLIENT=<client-ip> bench/xhostbench.sh [reps]
set -u

SERVER=${SERVER:?set SERVER=<ip of the host that runs both servers>}
CLIENT=${CLIENT:?set CLIENT=<ip of the host that runs redis-benchmark>}
REPS=${1:-3}
SRCDIR=${SRCDIR:-/dn/perfcached}       # tree on the SERVER host
# Same parameters respbench.sh uses, so the two tables are comparable:
# 100k requests over a 20k keyspace, 200 B values, FOUR workers - not
# nproc, which would give perfcached 16 cores against a 1-thread Redis.
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

# A runtime on each host.  `info` rather than `command -v`: an installed
# client whose daemon is down passes the PATH test and then fails every
# call, which is the confusing failure this ordering avoids.
findrt='for c in podman nerdctl docker; do command -v $c >/dev/null 2>&1 && $c info >/dev/null 2>&1 && { echo $c; break; }; done'
RTS=${RUNTIME_SRV:-$(ss_ "$findrt")}
RTC=${RUNTIME_CLI:-$(sc_ "$findrt")}
[ -n "$RTS" ] || { echo "xhostbench: no working container runtime on $SERVER"; exit 1; }
[ -n "$RTC" ] || { echo "xhostbench: no working container runtime on $CLIENT"; exit 1; }

# ---- images ------------------------------------------------------------
# Built on the SERVER from its own checkout, so the binary measured is
# the one that tree produces - not one carried over from elsewhere.
ss_ "cd $SRCDIR && $RTS build --platform linux/amd64 \
	-f bench/Containerfile.debian \
	--build-arg REV=\$(git rev-parse --short HEAD 2>/dev/null || echo unknown) \
	-t $IMG . >/dev/null 2>&1" \
	|| { echo "xhostbench: image build failed on $SERVER"; exit 1; }
sc_ "$RTC pull $REDIS_IMG >/dev/null 2>&1" \
	|| { echo "xhostbench: could not pull $REDIS_IMG on $CLIENT"; exit 1; }

# PROVENANCE: read every version out of the IMAGE that will run, never
# from the host or the directory name.
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

# --network host on BOTH servers: the point of this bench is the real
# 9000-byte path, and a bridge would insert a 1500-byte hop and report
# it as the network.  It also removes the old ssh-channel problem
# entirely - a detached container holds no fd of the ssh session, so the
# "background a subshell and sshd never returns" dance is gone.
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
# --ulimit memlock=-1: a container's default memlock sits far below the
# arena, so mlock() fails and the arena runs UNPINNED and swappable.
ss_ "$RTS run -d --name $PCON --network host --ulimit memlock=-1 \
	-v $D/pc.conf:/etc/perfcached.conf:ro $IMG >/dev/null 2>&1" \
	|| { echo "xhostbench: perfcached container did not start"; exit 1; }
i=0; while [ $i -lt 100 ]; do
	ss_ "$RTS logs $PCON 2>&1 | grep -q 'perfcached ready'" && break
	sleep 0.2; i=$((i+1)); done
ss_ "$RTS logs $PCON 2>&1 | grep -q 'perfcached ready'" || {
	echo "perfcached did not start"; ss_ "$RTS logs $PCON 2>&1 | tail -5"; exit 1; }

# a PERSISTENT driver on the client, so the per-cell cost is an exec and
# not a container start
sc_ "$RTC run -d --name $DCON --network host --entrypoint tail \
	$REDIS_IMG -f /dev/null >/dev/null 2>&1" \
	|| { echo "xhostbench: driver container did not start on $CLIENT"; exit 1; }
sc_ "$RTC exec $DCON redis-cli -h $SERVER -p $RPORT PING" | grep -q PONG || {
	echo "redis not answering"; exit 1; }
sc_ "$RTC exec $DCON redis-cli -h $SERVER -p $PPORT PING" | grep -q PONG || {
	echo "perfcached RESP door not answering"; exit 1; }

# ---- one cell, on the CLIENT ------------------------------------------
# Requests per cell scale with the pipeline depth, as in respbench.sh:
# redis-benchmark times a run in WHOLE MILLISECONDS, so 100k requests at
# 1.3M ops/s is a 77 ms run reported on a 1.3% grid.  N x P keeps every
# cell above a second, capped at N_MAX so one connection at depth 64
# does not run for a minute.
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
