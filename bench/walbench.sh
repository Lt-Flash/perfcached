#!/bin/sh
# walbench.sh — what durability costs, and what it buys back when a node dies.
#
# Two questions, and the second is the one nothing had answered.
#
# PART 1, COST.  Writes are the only operations a WAL touches, so every
# arm is 100% SET, run twice: once one-request-at-a-time (what an
# application feels) and once pipelined (the daemon's ceiling, where the
# fsync policy has nowhere to hide).  Arms are memory-only, then
# fsync = no | everysec | always, then everysec with RDB snapshots on -
# so the snapshot walker's cost is separated from the WAL's rather than
# blended into it.
#
# PART 2, FAILOVER.  The mode matrix measures what SURVIVORS hold after a
# node dies.  It says nothing about what the dead node brings BACK, which
# is the whole point of having a WAL - and DESIGN 6.5 claims durability
# "matters more for proxy collections" precisely because peers cannot
# rehydrate those keys.  That claim has never been tested.  So per mode:
# fill through node 1, kill -9 node 1, RESTART it, and read the original
# keyspace back THROUGH NODE 1 with the fill skipped.  With no WAL the
# answer should be nothing; with one it should be its own share.
#
# usage: bench/walbench.sh [./perfcached] [./pcbench] [./mmclient] [./perfcli]
set -u

BIN=${1:-./perfcached}
PB=${2:-./pcbench}
MM=${3:-./mmclient}
CLI=${4:-./perfcli}
D=${WALBENCH_DIR:-/var/tmp/walbench}
KEYS=${KEYS:-20000}
PARTS=${PARTS:-"1 2"}
VAL=${VAL:-200}
SECRET=wb-client-secret
P1= P2= P3=

rm -rf "$D"; mkdir -p "$D"

BINREV=$("$BIN" -V 2>/dev/null | sed -n 's/.*(\(.*\)).*/\1/p')
if [ -z "$BINREV" ] || [ "$BINREV" = unknown ]; then
	echo "WALBENCH: refusing to measure an unstamped build ($BINREV)" >&2
	exit 1
fi

# SO_REUSEPORT never errors on a busy port - it binds alongside and the
# kernel splits the traffic between two unrelated daemons
for pf in 17901 17902 17903; do
	if ss -ltn 2>/dev/null | grep -q ":$pf[[:space:]]"; then
		echo "WALBENCH: port $pf already bound:" >&2
		ss -ltnp 2>/dev/null | grep ":$pf[[:space:]]" >&2
		exit 1
	fi
done

# NOTE: the trap must NOT rm -rf "$D" - results.tsv lives there, and a
# harness that deletes its own output on the way out is a long walk for
# nothing.  Only the daemons go.
trap 'for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 $v 2>/dev/null; \
     done' EXIT TERM INT

addr() { echo "127.0.45.$1"; }
port() { echo "$((17900 + $1))"; }
# part 2's fleet is encrypted, part 1's single node is plaintext (pcbench
# speaks plaintext only, and `plaintext = loopback` is EXCLUSIVE - an
# authenticated perfcli would be refused there, so the two need different
# stats callers rather than one with -a bolted on)
statj() { "$CLI" -h "$(addr $1)" -p "$(port $1)" -a "$SECRET" \
	-j '{"method":"stats"}' 2>/dev/null; }
statp() { "$CLI" -h "$(addr $1)" -p "$(port $1)" \
	-j '{"method":"stats"}' 2>/dev/null; }
entries_p() { statp "$1" | jget 'd["collections"][0]["entries"]'; }
jget() { python3 -c "import json,sys
try:
    d = json.load(sys.stdin)
    print($1)
except Exception:
    print('')" 2>/dev/null; }
# One retry: a momentarily unanswered stats call returns empty, and an
# empty read that gets treated as 0 is indistinguishable from a node that
# genuinely holds nothing - which is the exact number this harness is
# trying to measure.
entries() {
	e=$(statj "$1" | jget 'd["collections"][0]["entries"]')
	if [ -z "$e" ]; then
		sleep 0.3
		e=$(statj "$1" | jget 'd["collections"][0]["entries"]')
	fi
	echo "$e"
}
field() { printf '%s' "$1" | sed -n "s/.*$2=\([0-9-]*\).*/\1/p"; }
now_ms() { date +%s%3N; }

# ---- part 1: what durability costs -------------------------------------
printf '# build=%s date=%s host=%s keys=%s val=%s\n' "$BINREV" \
	"$(date -u +%Y-%m-%dT%H:%MZ)" "$(hostname)" "$KEYS" "$VAL" \
	> "$D/results.tsv"
printf 'section\tarm\trtt_med\trtt_lo\trtt_hi\tpipe_med\tpipe_lo\tpipe_hi\trounds\trecovered\trecover_ms\n' \
	>> "$D/results.tsv"

mk1() { # mk1 <wal-block>
	cat > "$D/n1.conf" <<EOF
[daemon]
workers = 4
log_level = notice
[memory]
arena_mb = 512
[secrets]
client = $SECRET
cluster = wb-cluster-secret
[listen]
tcp = $(addr 1):$(port 1)
plaintext = loopback
$1
[collection c]
buckets_log2 = 17
EOF
}
start1() {
	"$BIN" -f "$D/n1.conf" > "$D/n1.log" 2>&1 &
	P1=$!
	k=0
	while [ $k -lt 300 ]; do
		grep -q "perfcached ready" "$D/n1.log" 2>/dev/null && return 0
		sleep 0.1; k=$((k + 1))
	done
	return 1
}
stop1() { [ -n "$P1" ] && kill -9 $P1 2>/dev/null; P1=; wait 2>/dev/null; }

# the two measured drives, factored out so the warm-up is provably the
# SAME work as the measurement rather than an approximation of it
drive_rtt() { # drive_rtt [seconds]
	"$PB" -h "$(addr 1)" -p "$(port 1)" -P perf -C c -N -n "$KEYS" \
		-v "$VAL" -M 0 -d 1 -c 4 -T "${1:-5}" -q 2>&1 | tail -1 \
		| sed -n 's/.*: \([0-9]*\) ops\/s.*/\1/p'
}
drive_pipe() { # drive_pipe [seconds]
	"$PB" -h "$(addr 1)" -p "$(port 1)" -P perf -C c -N -n "$KEYS" \
		-v "$VAL" -M 0 -d 32 -c 8 -T "${1:-5}" -q 2>&1 | tail -1 \
		| sed -n 's/.*: \([0-9]*\) ops\/s.*/\1/p'
}

# ---- part 1 measurement: INTERLEAVED, median of N rounds -----------------
#
# Sequential single-pass measurement did not work and the reason is
# recorded: six consecutive pipelined drives against ONE unchanged arm
# spanned 945,640 to 1,572,177 - a range wide enough to contain nearly
# every "difference" the ranked table was reporting between arms.  Two
# full runs then disagreed about the ranking completely (memory-only went
# 5th to 2nd, wal-rdb 1st to 5th).
#
# Repeating the arms SEQUENTIALLY would not fix that: a slow patch of the
# box would still land entirely on whichever arm happened to be running.
# So the rounds are INTERLEAVED - every round drives every arm once, in
# the same order - and each arm reports the median of its rounds with the
# observed low and high beside it.  Drift is then shared by all arms
# instead of being attributed to one.
#
# Recovery is measured ONCE per arm, not per round: it is a WAL replay of
# a known keyspace, it is slow, and it is not what the noise was in.

ROUNDS=${ROUNDS:-7}

part() { case " $PARTS " in *" $1 "*) return 0;; esac; return 1; }

wal_block() { # wal_block <label> -> the [wal] section for that arm
	case $1 in
	memory)       printf '' ;;
	wal-no)       printf '[wal]\ndir = %s/wal\nprobe = no\nfsync = no\nsave = off\n' "$D" ;;
	wal-everysec) printf '[wal]\ndir = %s/wal\nprobe = no\nfsync = everysec\nsave = off\n' "$D" ;;
	wal-always)   printf '[wal]\ndir = %s/wal\nprobe = no\nfsync = always\nsave = off\n' "$D" ;;
	wal-rdb)      printf '[wal]\ndir = %s/wal\nprobe = no\nfsync = everysec\nsave = 5 1000\n' "$D" ;;
	esac
}
ARMS="memory wal-no wal-everysec wal-always wal-rdb"

# one round for one arm: fresh daemon, discarded warm-up, then the two
# measured drives.  Echoes "<rtt> <pipe>".
round_arm() { # round_arm <label>
	rm -rf "$D/wal"; mkdir -p "$D/wal"
	mk1 "$(wal_block "$1")"
	start1 || { echo "WALBENCH: $1 did not start" >&2; echo "0 0"; return 0; }
	# short warm-up: the box is already warm after round 1, this is here
	# for the fresh arena and the new connections
	drive_rtt 2 >/dev/null 2>&1
	drive_pipe 2 >/dev/null 2>&1
	R=$(drive_rtt); P=$(drive_pipe)
	stop1
	echo "${R:-0} ${P:-0}"
}

# median / low / high of the whitespace-separated numbers on stdin
stats3() { tr ' ' '\n' | grep -E '^[0-9]+$' | sort -n | awk '
	{v[NR]=$1}
	END {
		if (NR == 0) { print "0 0 0"; exit }
		m = (NR % 2) ? v[(NR+1)/2] : int((v[NR/2] + v[NR/2+1]) / 2)
		print m, v[1], v[NR]
	}'
}

if part 1; then
	echo "=== part 1: what durability costs ($ROUNDS interleaved rounds) ===" >&2
	for a in $ARMS; do
		eval "RTTS_$(echo "$a" | tr - _)=''"
		eval "PIPES_$(echo "$a" | tr - _)=''"
	done
	r=1
	while [ $r -le "$ROUNDS" ]; do
		for a in $ARMS; do
			set -- $(round_arm "$a")
			v=$(echo "$a" | tr - _)
			eval "RTTS_$v=\"\$RTTS_$v $1\""
			eval "PIPES_$v=\"\$PIPES_$v \$2\""
			echo "  round $r/$ROUNDS $a: rtt $1, pipelined $2" >&2
		done
		r=$((r + 1))
	done

	# recovery, once per arm, on a fresh daemon
	for a in $ARMS; do
		v=$(echo "$a" | tr - _)
		eval "REC_$v=n/a; RMS_$v=n/a"
		[ "$a" = memory ] && continue
		rm -rf "$D/wal"; mkdir -p "$D/wal"
		mk1 "$(wal_block "$a")"
		start1 || continue
		"$PB" -h "$(addr 1)" -p "$(port 1)" -P perf -C c -F -n "$KEYS" \
			-v "$VAL" >/dev/null 2>>"$D/$a.err"
		BEFORE=$(entries_p 1)
		kill -9 $P1 2>/dev/null; P1=; wait 2>/dev/null
		sleep 1
		T0=$(now_ms)
		start1 || continue
		LAST=-1; SAME=0; k=0
		while [ $k -lt 600 ]; do
			E=$(entries_p 1); E=${E:-0}
			if [ "$E" = "$LAST" ]; then
				SAME=$((SAME + 1)); [ $SAME -ge 3 ] && break
			else
				SAME=0
			fi
			LAST=$E; sleep 0.2; k=$((k + 1))
		done
		eval "RMS_$v=\$(( \$(now_ms) - T0 ))"
		eval "REC_$v=\${LAST:-0}"
		eval "echo \"  \$a: had \$BEFORE, recovered \$REC_$v in \${RMS_$v}ms\"" >&2
		stop1
	done

	# emit, and remember the widest arm's own spread as the resolution
	WIDEST=0
	for a in $ARMS; do
		v=$(echo "$a" | tr - _)
		eval "set -- \$(echo \$PIPES_$v | stats3)"
		PM=$1; PLO=$2; PHI=$3
		eval "set -- \$(echo \$RTTS_$v | stats3)"
		RM=$1; RLO=$2; RHI=$3
		[ "$PM" -gt 0 ] 2>/dev/null && {
			w=$(( (PHI - PLO) * 100 / PM ))
			[ "$w" -gt "$WIDEST" ] && WIDEST=$w
		}
		eval "REC=\$REC_$v; RMS=\$RMS_$v"
		printf 'cost\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
			"$a" "$RM" "$RLO" "$RHI" "$PM" "$PLO" "$PHI" \
			"$ROUNDS" "$REC" "$RMS" >> "$D/results.tsv"
		echo "  $a: rtt $RM ($RLO-$RHI), pipelined $PM ($PLO-$PHI)" >&2
	done
	printf 'cost\t#resolution\tpipe_widest_arm_spread_pct=%s\trounds=%s\n' \
		"$WIDEST" "$ROUNDS" >> "$D/results.tsv"
	echo "  RESOLUTION: the widest arm's own spread is ${WIDEST}% - any" \
		"difference between arms smaller than that is not a result" >&2

	# SANITY: memory-only does strictly less work than every WAL arm, so
	# it cannot be slower.  If it still is with medians in hand, the rig
	# is not measuring the daemon.  Shout; part 2 is unaffected.
	MEMP=$(awk -F'\t' '$1=="cost" && $2=="memory" {print $6}' "$D/results.tsv")
	BESTW=$(awk -F'\t' '$1=="cost" && $2!="memory" && $2!~/^#/ && $6+0>m {m=$6+0}
		END {print m+0}' "$D/results.tsv")
	if [ -n "$MEMP" ] && [ "$MEMP" -gt 0 ] 2>/dev/null &&
	   [ "$BESTW" -gt 0 ] 2>/dev/null &&
	   [ $((MEMP * 100 / BESTW)) -lt 95 ]; then
		echo "WALBENCH: COST NUMBERS ARE NOT USABLE - memory-only" \
			"median $MEMP is below the best WAL arm $BESTW even" \
			"across $ROUNDS interleaved rounds.  Do not publish" \
			"part 1." >&2
		printf 'cost\t!SUSPECT\tmemory=%s\tbest_wal=%s\n' \
			"$MEMP" "$BESTW" >> "$D/results.tsv"
	fi
fi

# ---- part 2: what a WAL buys back when a node dies ---------------------
part 2 && echo "=== part 2: WAL and failover, per mode ===" >&2

fleet() { # fleet <mode-block> <wal 0|1>
	i=1
	while [ $i -le 3 ]; do
		W=""
		[ "$2" = 1 ] && W="[wal]
dir = $D/w$i
probe = no
fsync = everysec
save = off"
		mkdir -p "$D/w$i"
		cat > "$D/n$i.conf" <<EOF
[daemon]
workers = 4
log_level = notice
[memory]
arena_mb = 512
[secrets]
client = $SECRET
cluster = wb-cluster-secret
[listen]
tcp = $(addr $i):$(port $i)
[cluster]
multicast = 239.255.77.147:17247
advertise = $(addr $i)
pull_timeout_ms = 400
$1
$W
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
		while [ $k -lt 300 ]; do
			grep -q "perfcached ready" "$D/n$i.log" 2>/dev/null && break
			sleep 0.1; k=$((k + 1))
		done
		i=$((i + 1))
	done
	sleep 4
}
stopfleet() {
	for v in "$P1" "$P2" "$P3"; do
		[ -n "$v" ] && kill -9 $v 2>/dev/null
	done
	wait 2>/dev/null
	P1= P2= P3=
	sleep 0.5
}

modearm() { # modearm <label> <mode-block> <wal 0|1>
	LABEL=$1
	rm -rf "$D/w1" "$D/w2" "$D/w3"
	fleet "$2" "$3"
	# fill through node 1 only
	"$MM" "$(addr 1)" "$(port 1)" "$SECRET" c "$KEYS" "$VAL" 0 0 \
		>/dev/null 2>>"$D/$LABEL.err"
	HELD=$(entries 1); HELD=${HELD:-0}

	# LET THE FLEET SETTLE FIRST.  eager replicates in the background, so
	# killing the moment the fill returns would measure it mid-push and
	# make eager look exactly like store - the very distinction this arm
	# exists to draw.  Wait for the peer census to stop moving.
	# eager's sweep pauses BETWEEN batches, so "unchanged for a moment"
	# is not "finished": the first version of this stopped at 18894 of
	# 20000 and the 1106 it had not yet pushed came back as data loss
	# that eager had not actually suffered.  Stop early only on FULL
	# convergence; otherwise demand a long quiet period.
	# The eager sweep runs on a ~10s TICK, so a few seconds of "no
	# change" only means we are between ticks.  Measured twice at
	# exactly 18894/20000 with a 4.8s quiet window - the same number
	# twice is a deterministic gap, not slow convergence, and here the
	# gap was the harness's patience rather than the daemon's sweep.
	# So: poll for FULL convergence for up to 60s, and only then fall
	# back to a quiet window long enough to span a tick.
	PL=""; PS=0; k=0
	while [ $k -lt 300 ]; do
		A=$(entries 2); B=$(entries 3)
		PN="${A:-0}/${B:-0}"
		if [ "${A:-0}" -ge "$KEYS" ] && [ "${B:-0}" -ge "$KEYS" ]; then
			PL="$PN"; break
		fi
		if [ "$PN" = "$PL" ]; then
			PS=$((PS + 1))
			# 40 x 0.4s = 16s, comfortably more than one sweep tick
			[ $PS -ge 40 ] && break
		else
			PS=0
		fi
		PL="$PN"
		sleep 0.4; k=$((k + 1))
	done
	echo "  $LABEL: peers settled at $PL before the kill" >&2

	kill -9 $P1 2>/dev/null; P1=; sleep 6
	# Restart the SAME node with the SAME wal dir, into a FRESH log.
	# Appending to the old one meant the ready marker from the FIRST
	# start was already there, so the wait returned immediately and
	# every number after it described a node that had not come up -
	# which is how all five arms reported "recovered 0" in ~750ms.
	T0=$(now_ms)
	"$BIN" -f "$D/n1.conf" > "$D/n1.restart.log" 2>&1 &
	P1=$!
	k=0; UP=0
	while [ $k -lt 300 ]; do
		grep -q "perfcached ready" "$D/n1.restart.log" 2>/dev/null && {
			UP=1; break; }
		sleep 0.1; k=$((k + 1))
	done
	if [ "$UP" != 1 ]; then
		echo "WALBENCH: $LABEL node 1 never came back:" >&2
		tail -3 "$D/n1.restart.log" >&2
		printf 'failover\t%s\t%s\tNOSTART\tNOSTART\tNOSTART\n' \
			"$LABEL" "$HELD" >> "$D/results.tsv"
		stopfleet
		return 0
	fi
	# WAIT FOR IT TO REJOIN, not just to listen.  A restarted node needs
	# its peers before it can pull or gather, and "perfcached ready"
	# comes first - so reading straight away measures the membership
	# race.  It showed up worst in the since-removed ec mode, where an
	# un-joined node could not gather k chunks and reported 3329 of
	# 20000 "missing" that a survivor read perfectly well.
	k=0
	while [ $k -lt 300 ]; do
		M=$("$CLI" -h "$(addr 1)" -p "$(port 1)" -a "$SECRET" \
			-j '{"method":"members"}' 2>/dev/null \
			| jget 'len(d["members"])')
		[ -n "$M" ] && [ "$M" -ge 3 ] && break
		sleep 0.2; k=$((k + 1))
	done
	echo "  $LABEL: rejoined with ${M:-?} members after $((k * 2))00ms" >&2
	LAST=-1; SAME=0; k=0
	while [ $k -lt 600 ]; do
		E=$(entries 1); E=${E:-0}
		if [ "$E" = "$LAST" ]; then
			SAME=$((SAME + 1)); [ $SAME -ge 3 ] && break
		else
			SAME=0
		fi
		LAST=$E
		sleep 0.2; k=$((k + 1))
	done
	RMS=$(( $(now_ms) - T0 ))
	BACK=${LAST:-0}
	# and what the ORIGINAL keyspace looks like read back through the
	# node that died - fill skipped, so this is recovery not rewriting
	SURV=$("$MM" "$(addr 1)" "$(port 1)" "$SECRET" c "$KEYS" "$VAL" 100 0 0 1 \
		2>>"$D/$LABEL.err")
	MISS=$(field "$SURV" missed); MISS=${MISS:-?}
	E2=$(entries 2); E3=$(entries 3)
	printf 'failover\t%s\t%s\t%s\t%s\t%s\t%s/%s\n' "$LABEL" "$HELD" \
		"$BACK" "$MISS" "$RMS" "${E2:-?}" "${E3:-?}" >> "$D/results.tsv"
	echo "  $LABEL: held $HELD, back $BACK, still missing $MISS of $KEYS, \
${RMS}ms, peers ${E2:-?}/${E3:-?}" >&2
	stopfleet
}

printf '\n# part 2 columns: arm / held_before_kill / entries_after_restart / \
missing_of_%s / recover_ms\n' "$KEYS" >> "$D/results.tsv"
part 2 && modearm store-nowal "mode = store
collections = c"      0
part 2 && modearm store-wal "mode = store
collections = c"      1
# eager is the counterpart to store: it pushes every key to every node,
# so the peers already hold what the dead node held.  If the fleet can
# cover for a node that came back empty, the log is not load-bearing
# there - and that is a different question from whether the node can
# cover for itself.
part 2 && modearm eager-nowal "mode = store
eager = 1
collections = c"      0
part 2 && modearm eager-wal "mode = store
eager = 1
collections = c"      1
part 2 && modearm proxy-nowal "mode = proxy
collections = c"      0
part 2 && modearm proxy-wal "mode = proxy
collections = c"      1
part 2 && modearm shard-wal "mode = shard
collections = c"      1

echo
echo "--- results ($D/results.tsv) ---"
cat "$D/results.tsv"
