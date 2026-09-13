#!/bin/sh
# concmatrix.sh — what N concurrent clients cost, and where the cost goes.
#
# Two questions, kept apart:
#
#   SCALING   one handle per thread, 1..8 threads, both dialects.  Reports
#             aggregate throughput AND per-connection throughput, because
#             a cache that doubles its total while halving each client's
#             share has not scaled for the client that is waiting.
#
#   OVERHEAD  at a fixed 4 connections, the same work three ways: the raw
#             wire (pcbench), libperfd in plaintext, libperfd over Noise.
#             The differences price the library and the encryption
#             SEPARATELY instead of reporting one blended gap.
#
# The overhead arms need two fleets, because 'plaintext = loopback' makes
# a loopback listener plaintext-EXCLUSIVE: a client that offers a Noise
# handshake to one is sniffed as RESP and dropped.  So the two fleets are
# identical in every other setting and started back to back on the same
# host, and the flag is the only variable.
#
# usage: bench/concmatrix.sh [./perfcached] [./concbench] [./pcbench]
set -u

BIN=${1:-./perfcached}
CB=${2:-./concbench}
PB=${3:-./pcbench}
D=${CONCMATRIX_DIR:-/var/tmp/concmatrix}
OPS=${OPS:-20000}
VAL=${VAL:-200}
GETPCT=${GETPCT:-90}
THREADS=${THREADS:-"1 2 4 8"}
REPS=${REPS:-3}
WORKERS=${WORKERS:-8}
SECRET=cm-client-secret
P1= P2= P3=

rm -rf "$D"; mkdir -p "$D"

# PROVENANCE.  The numbers are only worth as much as the build that
# produced them, so stamp the BINARY's revision (not the tree's - they
# differ the moment you run an older binary against a newer checkout)
# and refuse to run one that cannot name itself.  An unstamped build is
# how a page ends up quoting figures nobody can reproduce.
BINREV=$("$BIN" -V 2>/dev/null | sed -n 's/.*(\(.*\)).*/\1/p')
if [ -z "$BINREV" ] || [ "$BINREV" = unknown ]; then
	echo "CONCMATRIX: refusing to measure an unstamped build ($BINREV) - \
build from a git checkout so PC_BUILD_REV is set" >&2
	exit 1
fi

trap 'for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 $v 2>/dev/null; \
     done' EXIT TERM INT

addr() { echo "127.0.43.$1"; }
port() { echo "$((17500 + $1))"; }

start_fleet() { # start_fleet <plaintext-line>
	i=1
	while [ $i -le 3 ]; do
		cat > "$D/n$i.conf" <<EOF
[daemon]
workers = $WORKERS
log_level = notice
[memory]
arena_mb = 512
[secrets]
client = $SECRET
cluster = cm-cluster-secret
[listen]
tcp = $(addr $i):$(port $i)
$1
[cluster]
multicast = 239.255.77.146:17246
advertise = $(addr $i)
pull_timeout_ms = 400
mode = store
collections = c
[collection c]
buckets_log2 = 17
EOF
		"$BIN" -f "$D/n$i.conf" > "$D/n$i.log" 2>&1 &
		eval "P$i=\$!"
		i=$((i + 1))
	done
	i=1
	while [ $i -le 3 ]; do
		k=0
		while [ $k -lt 120 ]; do
			grep -q "perfcached ready" "$D/n$i.log" 2>/dev/null && break
			sleep 0.1; k=$((k + 1))
		done
		i=$((i + 1))
	done
	sleep 4
}
stop_fleet() {
	for v in "$P1" "$P2" "$P3"; do
		[ -n "$v" ] && kill -9 $v 2>/dev/null
	done
	wait 2>/dev/null
	P1= P2= P3=
	sleep 0.5
}
field() { printf '%s' "$1" | sed -n "s/.*$2=\([0-9-]*\).*/\1/p"; }

# 224 is shared with another session's daemons, so a single pass is not a
# measurement - the same arm moved 11% between two runs an hour apart, and
# once inverted the dialect ordering.  Every arm runs REPS times and the
# MEDIAN is reported, with the spread printed so a noisy arm is visible
# rather than averaged into looking solid.
med() { printf '%s\n' "$@" | sort -n | awk '{v[NR]=$1} END{print v[int((NR+1)/2)]}'; }
spread() { printf '%s\n' "$@" | sort -n | awk '{v[NR]=$1} END{
	printf "%d-%d", v[1], v[NR]}'; }

# a run that missed or errored is not a measurement
check() { # check <label> <output>
	m=$(field "$2" missed); e=$(field "$2" errors)
	if [ "${m:-1}" != "0" ] || [ "${e:-1}" != "0" ]; then
		echo "CONCMATRIX: $1 missed=$m errors=$e - NOT a result" >&2
		return 1
	fi
	return 0
}

printf '# build=%s date=%s host=%s keys=%s val=%s\n' "$BINREV" \
	"$(date -u +%Y-%m-%dT%H:%MZ)" "$(hostname)" "$OPS" "$VAL" > "$D/results.tsv"
printf 'section\tarm\tthreads\tdialect\tops_per_s\tper_conn\tp50us\tp99us\tspread\n' \
	>> "$D/results.tsv"

# ---- scaling: Noise fleet --------------------------------------------
echo "=== scaling (libperfd over Noise, 3-node store) ===" >&2
start_fleet ""
for t in $THREADS; do
	for b in 0 1; do
		[ "$b" = 0 ] && dial=json || dial=binary
		OPSL="" P50L="" P99L="" bad=0
		r=0
		while [ $r -lt "$REPS" ]; do
			o=$("$CB" "$(addr 1)" "$(port 1)" "$SECRET" c "$t" \
				"$OPS" "$GETPCT" 0 "$b" 2>>"$D/err")
			check "scaling $dial t=$t" "$o" || { bad=1; break; }
			OPSL="$OPSL $(field "$o" ops_per_s)"
			P50L="$P50L $(field "$o" p50)"
			P99L="$P99L $(field "$o" p99)"
			r=$((r + 1))
		done
		[ "$bad" = 1 ] && continue
		O=$(med $OPSL)
		echo "  $dial t=$t: $O ops/s (of $REPS: $(spread $OPSL))" >&2
		printf 'scaling\tnoise\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
			"$t" "$dial" "$O" "$((O / t))" \
			"$(med $P50L)" "$(med $P99L)" "$(spread $OPSL)" \
			>> "$D/results.tsv"
	done
done

# ---- overhead arm 1+2: same fleet, libperfd over Noise + raw wire ------
echo "=== overhead at 4 connections ===" >&2
for b in 0 1; do
	[ "$b" = 0 ] && dial=json || dial=binary
	OPSL="" P50L="" P99L=""; r=0
	while [ $r -lt "$REPS" ]; do
		o=$("$CB" "$(addr 1)" "$(port 1)" "$SECRET" c 4 "$OPS" \
			"$GETPCT" 0 "$b" 2>>"$D/err")
		check "overhead noise $dial" "$o" || break
		OPSL="$OPSL $(field "$o" ops_per_s)"
		P50L="$P50L $(field "$o" p50)"; P99L="$P99L $(field "$o" p99)"
		r=$((r + 1))
	done
	[ "$r" -eq "$REPS" ] || continue
	O=$(med $OPSL)
	echo "  libperfd+noise $dial: $O ops/s (of $REPS: $(spread $OPSL))" >&2
	printf 'overhead\tlibperfd+noise\t4\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$dial" "$O" "$((O / 4))" "$(med $P50L)" "$(med $P99L)" \
		"$(spread $OPSL)" >> "$D/results.tsv"
done
stop_fleet

# ---- overhead arm 3: the SAME fleet with plaintext eligible -----------
start_fleet "plaintext = loopback"
for b in 0 1; do
	[ "$b" = 0 ] && dial=json || dial=binary
	OPSL="" P50L="" P99L=""; r=0
	while [ $r -lt "$REPS" ]; do
		o=$("$CB" "$(addr 1)" "$(port 1)" - c 4 "$OPS" "$GETPCT" 0 \
			"$b" 2>>"$D/err")
		check "overhead plain $dial" "$o" || break
		OPSL="$OPSL $(field "$o" ops_per_s)"
		P50L="$P50L $(field "$o" p50)"; P99L="$P99L $(field "$o" p99)"
		r=$((r + 1))
	done
	[ "$r" -eq "$REPS" ] || continue
	O=$(med $OPSL)
	echo "  libperfd plain  $dial: $O ops/s (of $REPS: $(spread $OPSL))" >&2
	printf 'overhead\tlibperfd+plain\t4\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$dial" "$O" "$((O / 4))" "$(med $P50L)" "$(med $P99L)" \
		"$(spread $OPSL)" >> "$D/results.tsv"
done
# raw wire, same plaintext fleet: pcbench speaks the protocol directly
"$PB" -h "$(addr 1)" -p "$(port 1)" -P perf -C c -F -n "$OPS" -v "$VAL" \
	>/dev/null 2>&1
for pr in perf bin; do
	[ "$pr" = perf ] && dial=json || dial=binary
	OPSL="" P50L="" P99L=""; r=0
	while [ $r -lt "$REPS" ]; do
		o=$("$PB" -h "$(addr 1)" -p "$(port 1)" -P "$pr" -C c -N \
			-n "$OPS" -v "$VAL" -M "$GETPCT" -d 1 -c 4 -T 5 \
			2>&1 | tail -1)
		ops=$(printf '%s' "$o" | sed -n 's/.*: \([0-9]*\) ops\/s.*/\1/p')
		[ -n "$ops" ] || { echo "CONCMATRIX: rawwire $dial unparsed" >&2
			break; }
		OPSL="$OPSL $ops"
		P50L="$P50L $(printf '%s' "$o" | sed -n 's/.*p50=\([0-9]*\)us.*/\1/p')"
		P99L="$P99L $(printf '%s' "$o" | sed -n 's/.*p99=\([0-9]*\)us.*/\1/p')"
		r=$((r + 1))
	done
	[ "$r" -eq "$REPS" ] || continue
	O=$(med $OPSL)
	echo "  rawwire        $dial: $O ops/s (of $REPS: $(spread $OPSL))" >&2
	printf 'overhead\trawwire\t4\t%s\t%s\t%s\t%s\t%s\t%s\n' "$dial" \
		"$O" "$((O / 4))" "$(med $P50L)" "$(med $P99L)" \
		"$(spread $OPSL)" >> "$D/results.tsv"
done
stop_fleet

echo
echo "--- results ($D/results.tsv) ---"
cat "$D/results.tsv"
