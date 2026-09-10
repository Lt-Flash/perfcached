#!/bin/sh
# routepair.sh - what per-key routing is worth, measured apart: the SAME
# 3-node host fleet, natbench at 50 connections / depth 32, GET-only over
# a seeded keyspace, route=0 (dial one node, daemon forwards) against
# route=1 (client computes the owner), store and shard.  This is the
# README's "1.5x from spreading / 11.5x for shard" decomposition as a
# script instead of a hand-run.  Fleet recipe is modematrix.sh's.
set -u
# usage: bench/routepair.sh [./perfcached] [./natbench] [./perfcli]
BIN=${1:-./perfcached}; NB=${2:-./natbench}; CLI=${3:-./perfcli}
REPS=${REPS:-3}; CONNS=50; THREADS=4; DEPTH=32; SECS=${SECS:-5}
KEYS=20000; VAL=200; SECRET=mm-client-secret
OUT=/var/tmp/routepair; mkdir -p "$OUT"
for x in $BIN $NB $CLI; do [ -x $x ] || { echo "missing $x"; exit 1; }; done
BUSY=$(ps -eo comm --no-headers | grep -cE "^(perfcached|pcbench|make|fio|redis-benchmark|natbench)$")
[ "$BUSY" -gt 0 ] && { echo "BOX NOT IDLE ($BUSY)"; exit 1; }
D=$(mktemp -d /var/tmp/rp.XXXXXX); P1= P2= P3=
addr() { echo "127.0.40.$1"; }
port() { echo "$((18000 + $1))"; }
cnum() { "$CLI" -h "$(addr $1)" -p "$(port $1)" -a "$SECRET" -j '{"method":"stats"}' 2>/dev/null \
	| python3 -c "import json,sys; d=json.load(sys.stdin); c=d.get('cluster') or {}; print(c.get('$2', '?'))" 2>/dev/null; }
members() { "$CLI" -h "$(addr $1)" -p "$(port $1)" -a "$SECRET" -j '{"method":"members"}' 2>/dev/null \
	| python3 -c "import json,sys; print(len(json.load(sys.stdin)['members']))" 2>/dev/null; }
# reads of a key another node owns are PULLS (pull_sent), writes that are
# placed elsewhere are FORWARDS (fwd_sent) - a routed client drives both
# to zero, an un-routed GET load shows up in the first
ctotal() { t=0; for n in 1 2 3; do v=$(cnum $n "$1"); case "$v" in ''|*[!0-9]*) v=0;; esac; t=$((t + v)); done; echo $t; }
start_fleet() { # start_fleet <cluster-block>
	i=1
	while [ $i -le 3 ]; do
		cat > "$D/n$i.conf" <<EOC
[daemon]
workers = 4
log_level = notice
[memory]
arena_mb = 256
[secrets]
client = $SECRET
cluster = mm-cluster-secret
[listen]
tcp = $(addr $i):$(port $i)
[cluster]
multicast = 239.255.77.130:17230
advertise = $(addr $i)
pull_timeout_ms = 400
$1
[collection c]
buckets_log2 = 16
EOC
		"$BIN" -f "$D/n$i.conf" > "$D/n$i.log" 2>&1 &
		eval "P$i=\$!"
		i=$((i + 1))
	done
	i=1
	while [ $i -le 3 ]; do
		k=0; while [ $k -lt 120 ]; do
			grep -q "perfcached ready" "$D/n$i.log" 2>/dev/null && break
			sleep 0.1; k=$((k + 1)); done
		i=$((i + 1))
	done
	sleep 4
}
stop_fleet() {
	for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 $v 2>/dev/null; done
	wait 2>/dev/null; P1= P2= P3=; sleep 0.5
}
trap 'stop_fleet; rm -rf "$D"' EXIT

nat() { # nat <getpct> <route> <secs> -> natbench output
	"$NB" "$(addr 1)" "$(port 1)" "$SECRET" c $CONNS $THREADS $DEPTH "$3" \
		$KEYS $VAL "$1" 1 0 -1 "$2" 2>&1
}
BINREV=$($BIN -V | sed -n 's/.*(\(.*\)).*/\1/p' | head -1)
printf '# build=%s date=%s host=%s conns=%s depth=%s secs=%s keys=%s val=%s reps=%s\n' \
	"$BINREV" "$(date -u +%FT%TZ)" "$(hostname)" $CONNS $DEPTH $SECS $KEYS $VAL $REPS > "$OUT/results.tsv"
printf 'mode\troute\trep\tget_per_s\tpulls\tfwds\tnote\n' >> "$OUT/results.tsv"

for mode in store shard; do
	start_fleet "mode = $mode
collections = c"
	echo "--- $mode: fleet up ($(members 1) members), seeding $KEYS keys"
	nat 0 1 3 > "$D/seed.log" 2>&1
	grep -E "ops/s|ZERO|error" "$D/seed.log" | head -2 | sed 's/^/    /'
	rep=1
	while [ $rep -le $REPS ]; do
		for r in 0 1; do
			pb=$(ctotal pull_sent); fb=$(ctotal fwd_sent)
			o=$(nat 100 $r $SECS)
			pa=$(ctotal pull_sent); fa=$(ctotal fwd_sent)
			pd=$((pa - pb)); fd=$((fa - fb))
			ops=$(printf '%s\n' "$o" | sed -n 's/^ *\([0-9]*\) ops\/s.*/\1/p' | head -1)
			note=ok
			printf '%s\n' "$o" | grep -q "ZERO HITS" && note=ZERO_HITS
			printf '%s\n' "$o" | grep -qE "errors=[1-9]" && note=errors
			[ -z "$ops" ] && { ops=0; note=no_result; }
			printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$mode" "$r" "$rep" "$ops" "$pd" "$fd" "$note" >> "$OUT/results.tsv"
			echo "    $mode route=$r rep=$rep: $ops GET/s  pulls +$pd  fwds +$fd  $note"
		done
		rep=$((rep + 1))
	done
	stop_fleet
done

echo; head -1 "$OUT/results.tsv"
awk -F'\t' 'NR>2 && $7=="ok" { v[$1 FS $2] = v[$1 FS $2] " " $4 }
	END { for (k in v) { n=split(v[k], a, " "); asort_n(a, n); m=(n%2)?a[(n+1)/2]:(a[n/2]+a[n/2+1])/2; med[k]=m }
	      printf "%-6s %12s %12s %8s\n", "mode", "unrouted", "routed", "gain"
	      for (m in med) { split(m, p, FS); if (p[2]=="0" && ((p[1] FS "1") in med))
		printf "%-6s %12.0f %12.0f %7.1fx\n", p[1], med[m], med[p[1] FS "1"], med[p[1] FS "1"]/med[m] } }
	function asort_n(a, n,  i, j, t) { for (i=2;i<=n;i++) { t=a[i]+0; j=i-1; while (j>0 && a[j]+0>t) { a[j+1]=a[j]; j-- } a[j+1]=t } }' "$OUT/results.tsv"
