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
RPORT=16600     # redis-server
PPORT=16601     # perfcached's RESP door
NPORT=16602     # perfcached's native door (unused, must exist)
OUT=${OUT:-bench/results/xhostbench.tsv}

ss_() { ssh -o ConnectTimeout=10 "root@$SERVER" "$@"; }
sc_() { ssh -o ConnectTimeout=10 "root@$CLIENT" "$@"; }

# ---- preconditions, all of them loud ----------------------------------
BUSY=$(ss_ 'ps -eo comm --no-headers | grep -cE "^(perfcached|pcbench|make|fio|redis-benchmark)$"')
[ "${BUSY:-1}" -gt 0 ] && { echo "xhostbench: $SERVER is busy ($BUSY procs) - refusing"; exit 1; }
sc_ "ping -c1 -W2 -M do -s 8972 $SERVER >/dev/null 2>&1" \
	|| { echo "xhostbench: jumbo path $CLIENT -> $SERVER NOT verified (8972 fragments) - refusing"; exit 1; }
sc_ 'command -v redis-benchmark >/dev/null' || { echo "xhostbench: no redis-benchmark on $CLIENT"; exit 1; }
ss_ 'command -v redis-server >/dev/null' || { echo "xhostbench: no redis-server on $SERVER"; exit 1; }

BINREV=$(ss_ "$SRCDIR/perfcached -V 2>/dev/null | sed -n 's/.*(\(.*\)).*/\1/p'")
RSV=$(ss_ 'redis-server --version | grep -oE "v=[0-9.]+" | cut -c3-')
RBV=$(sc_ 'redis-benchmark --version | grep -oE "[0-9]+\.[0-9]+\.[0-9]+"')
WORKERS=${WORKERS:-4}
MTU=$(sc_ "ip route get $SERVER | grep -oE 'dev [a-z0-9]+' | awk '{print \$2}' | xargs -I{} cat /sys/class/net/{}/mtu")

echo "server $SERVER: perfcached $BINREV, redis-server $RSV, $WORKERS workers"
echo "client $CLIENT: redis-benchmark $RBV, MTU $MTU, jumbo path verified"

# ---- servers on the SERVER host, bound to its NIC address --------------
D=$(ss_ 'mktemp -d /var/tmp/xhb.XXXXXX')
RPID=; PPID_=
cleanup() {
	[ -n "$RPID" ] && ss_ "kill $RPID 2>/dev/null"
	[ -n "$PPID_" ] && ss_ "kill $PPID_ 2>/dev/null"
	ss_ "rm -rf $D"
}
trap cleanup EXIT

RPID=$(ss_ "redis-server --port $RPORT --bind $SERVER --protected-mode no --save '' --appendonly no \
	--daemonize no >$D/redis.log 2>&1 </dev/null & echo \$!")
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
# Every fd of a backgrounded remote daemon must leave the ssh channel or
# sshd keeps the session open until the daemon exits - and "cd X && d &"
# backgrounds a SUBSHELL holding the channel's fds, whatever d's own
# redirections say.  Redirect the subshell, exec the daemon inside it.
PPID_=$(ss_ "(cd $SRCDIR && exec ./perfcached -f $D/pc.conf) >$D/pc.log 2>&1 </dev/null & echo \$!")
i=0; while [ $i -lt 100 ]; do
	ss_ "grep -q 'perfcached ready' $D/pc.log" && break; sleep 0.2; i=$((i+1)); done
ss_ "grep -q 'perfcached ready' $D/pc.log" || { echo "perfcached did not start"; ss_ "tail -5 $D/pc.log"; exit 1; }
sc_ "redis-cli -h $SERVER -p $RPORT PING" | grep -q PONG || { echo "redis not answering"; exit 1; }
sc_ "redis-cli -h $SERVER -p $PPORT PING" | grep -q PONG || { echo "perfcached RESP door not answering"; exit 1; }

# ---- one cell, on the CLIENT ------------------------------------------
# Requests per cell scale with the pipeline depth, as in respbench.sh:
# redis-benchmark times a run in WHOLE MILLISECONDS, so 100k requests at
# 1.3M ops/s is a 77 ms run reported on a 1.3% grid.  N x P keeps every
# cell above a second, capped at N_MAX so one connection at depth 64
# does not run for a minute.
reqs() { r=$((N * $1)); [ "$r" -gt "$N_MAX" ] && r=$N_MAX; echo "$r"; }
point() { # point <port> <clients> <pipeline>  -> "sets gets sp50 gp50 sp99 gp99"
	sc_ "redis-benchmark -h $SERVER -p $1 -t set,get -n $(reqs $3) -c $2 -P $3 \
		-r $KEYSPACE -d $VAL --csv 2>/dev/null" \
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
	printf '# build=%s date=%s hosts=2 redis-server=%s redis-benchmark=%s mtu=%s keys=%s reqs=%sxP nmax=%s val=%s reps=%s\n' \
		"$BINREV" "$(date -u +%FT%TZ)" "$RSV" "$RBV" "$MTU" "$KEYSPACE" "$N" "$N_MAX" "$VAL" "$REPS"
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
