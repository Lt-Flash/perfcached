#!/bin/sh
# concmatrix.sh - what N concurrent clients cost: SCALING (1..8 handles, aggregate and per-connection)
# and OVERHEAD at 4 connections (raw wire, libperfd plaintext, libperfd over Noise, each priced apart).
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
dial=binary

rm -rf "$D"; mkdir -p "$D"

# stamp the BINARY's revision (not the tree's) and refuse one that cannot name itself: an unstamped
# build yields figures nobody can reproduce
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

# a single pass on a shared host is not a measurement: every arm runs REPS times and reports the
# median, with the spread printed so a noisy arm is visible
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
	OPSL="" P50L="" P99L="" bad=0
	r=0
	while [ $r -lt "$REPS" ]; do
		o=$("$CB" "$(addr 1)" "$(port 1)" "$SECRET" c "$t" \
			"$OPS" "$GETPCT" 0 2>>"$D/err")
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

# ---- overhead arm 1+2: same fleet, libperfd over Noise + raw wire ------
echo "=== overhead at 4 connections ===" >&2
OPSL="" P50L="" P99L=""; r=0
while [ $r -lt "$REPS" ]; do
	o=$("$CB" "$(addr 1)" "$(port 1)" "$SECRET" c 4 "$OPS" \
		"$GETPCT" 0 2>>"$D/err")
	check "overhead noise $dial" "$o" || break
	OPSL="$OPSL $(field "$o" ops_per_s)"
	P50L="$P50L $(field "$o" p50)"; P99L="$P99L $(field "$o" p99)"
	r=$((r + 1))
done
if [ "$r" -eq "$REPS" ]; then
	O=$(med $OPSL)
	echo "  libperfd+noise $dial: $O ops/s (of $REPS: $(spread $OPSL))" >&2
	printf 'overhead\tlibperfd+noise\t4\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$dial" "$O" "$((O / 4))" "$(med $P50L)" "$(med $P99L)" \
		"$(spread $OPSL)" >> "$D/results.tsv"
fi
stop_fleet

# ---- overhead arm 3: the SAME fleet with plaintext eligible -----------
# restarted: 'plaintext = loopback' makes a loopback listener plaintext-only (Noise reads as RESP)
start_fleet "plaintext = loopback"
OPSL="" P50L="" P99L=""; r=0
while [ $r -lt "$REPS" ]; do
	o=$("$CB" "$(addr 1)" "$(port 1)" - c 4 "$OPS" "$GETPCT" 0 \
		2>>"$D/err")
	check "overhead plain $dial" "$o" || break
	OPSL="$OPSL $(field "$o" ops_per_s)"
	P50L="$P50L $(field "$o" p50)"; P99L="$P99L $(field "$o" p99)"
	r=$((r + 1))
done
if [ "$r" -eq "$REPS" ]; then
	O=$(med $OPSL)
	echo "  libperfd plain  $dial: $O ops/s (of $REPS: $(spread $OPSL))" >&2
	printf 'overhead\tlibperfd+plain\t4\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$dial" "$O" "$((O / 4))" "$(med $P50L)" "$(med $P99L)" \
		"$(spread $OPSL)" >> "$D/results.tsv"
fi
# raw wire, same plaintext fleet: pcbench speaks the data verbs directly
"$PB" -h "$(addr 1)" -p "$(port 1)" -P bin -C c -F -n "$OPS" -v "$VAL" \
	>/dev/null 2>&1
OPSL="" P50L="" P99L=""; r=0
while [ $r -lt "$REPS" ]; do
	o=$("$PB" -h "$(addr 1)" -p "$(port 1)" -P bin -C c -N \
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
if [ "$r" -eq "$REPS" ]; then
	O=$(med $OPSL)
	echo "  rawwire        $dial: $O ops/s (of $REPS: $(spread $OPSL))" >&2
	printf 'overhead\trawwire\t4\t%s\t%s\t%s\t%s\t%s\t%s\n' "$dial" \
		"$O" "$((O / 4))" "$(med $P50L)" "$(med $P99L)" \
		"$(spread $OPSL)" >> "$D/results.tsv"
fi
stop_fleet

echo
echo "--- results ($D/results.tsv) ---"
cat "$D/results.tsv"
