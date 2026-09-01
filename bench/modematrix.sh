#!/bin/sh
# modematrix.sh — every collection mode, measured the same way, through
# a REAL client, plus a node failure in each.
#
# The load generator is mmclient, which drives libperfd: it learns the
# fleet, holds standby connections, and can route each key to its owner.
# Every mode is therefore measured TWICE - once as a plain client that
# talks to one node, once cluster-aware - because on shard the
# difference is the whole story, and on proxy there is deliberately
# none.  One operation at a time: an application's latency is per
# request, and pipelining hides exactly the round trip this exposes.
#
# Also measured per mode: where the keys landed, per-node memory (a
# single node's arena says nothing about a mode that spreads), whether
# the fleet converged, what a read costs on a node that did NOT write
# the data (100% GET with the fill skipped, through node 2), and what
# killing the node that HOLDS the data costs - read back through a
# survivor, with the victim's death asserted, because a failure test
# that cannot fail proves nothing.
#
# Every figure for a mode comes from ONE run of that mode, in the order
# written here.  Mixing runs is how a page ends up claiming a throughput
# from one fleet and a key census from another.
#
# usage: bench/modematrix.sh [./perfcached] [./mmclient] [./perfcli]
set -u

BIN=${1:-./perfcached}
MM=${2:-./mmclient}
CLI=${3:-./perfcli}
D=${MODEMATRIX_DIR:-/var/tmp/modematrix}
KEYS=${KEYS:-20000}
VAL=${VAL:-200}
SECRET=mm-client-secret
P1= P2= P3=

rm -rf "$D"; mkdir -p "$D"

# PROVENANCE.  The numbers are only worth as much as the build that
# produced them, so stamp the BINARY's revision (not the tree's - they
# differ the moment you run an older binary against a newer checkout)
# and refuse to run one that cannot name itself.  An unstamped build is
# how a page ends up quoting figures nobody can reproduce.
BINREV=$("$BIN" -V 2>/dev/null | sed -n 's/.*(\(.*\)).*/\1/p')
if [ -z "$BINREV" ] || [ "$BINREV" = unknown ]; then
	echo "MODEMATRIX: refusing to measure an unstamped build ($BINREV) - \
build from a git checkout so PC_BUILD_REV is set" >&2
	exit 1
fi

trap 'for v in "$P1" "$P2" "$P3"; do [ -n "$v" ] && kill -9 $v 2>/dev/null; \
     done' EXIT TERM INT

# PORT PRE-FLIGHT: SO_REUSEPORT means a fleet left behind by an
# interrupted run binds ALONGSIDE this one rather than failing, and the
# kernel then splits requests between two unrelated fleets.  Refuse.
ports_free() { # ports_free <port>...
	for pf in "$@"; do
		if ss -ltn 2>/dev/null | grep -q ":$pf[[:space:]]"; then
			echo "MODEMATRIX: port $pf is already bound - a leftover \
fleet would silently split this run's traffic:" >&2
			ss -ltnp 2>/dev/null | grep ":$pf[[:space:]]" >&2
			exit 1
		fi
	done
}
ports_free 18001 18002 18003

addr() { echo "127.0.40.$1"; }
port() { echo "$((18000 + $1))"; }
statj() { "$CLI" -h "$(addr $1)" -p "$(port $1)" -a "$SECRET" \
	-j '{"method":"stats"}' 2>/dev/null; }
jget() { python3 -c "import json,sys
try:
    d = json.load(sys.stdin)
    print($1)
except Exception:
    print('')" 2>/dev/null; }
entries() { statj "$1" | jget 'd["collections"][0]["entries"]'; }
live()    { statj "$1" | jget 'd["memory"]["arena_live"]'; }
cnum()    { statj "$1" | jget "d[\"cluster\"][\"$2\"]"; }
members() { "$CLI" -h "$(addr $1)" -p "$(port $1)" -a "$SECRET" \
	-j '{"method":"members"}' 2>/dev/null | jget 'len(d["members"])'; }
field()   { printf '%s' "$1" | sed -n "s/.*$2=\([0-9-]*\).*/\1/p"; }

start_fleet() { # start_fleet <cluster-block>
	i=1
	while [ $i -le 3 ]; do
		cat > "$D/n$i.conf" <<EOF
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
	sleep 4                        # membership + election
}
stop_fleet() {
	for v in "$P1" "$P2" "$P3"; do
		[ -n "$v" ] && kill -9 $v 2>/dev/null
	done
	wait 2>/dev/null
	P1= P2= P3=
	sleep 0.5
}

run_mode() { # run_mode <label> <cluster-block>
	LABEL=$1
	echo "=== $LABEL ===" >&2
	start_fleet "$2"

	# --- the same work twice: plain client, then cluster-aware ------
	# four runs per mode: both DIALECTS x plain/cluster-aware.  The
	# wire cost (JSON line vs binary frame) and the cluster cost (a
	# forwarded op vs a routed one) are separate questions and are
	# kept separate here.
	drive() { # drive <route> <binary>
		"$MM" "$(addr 1)" "$(port 1)" "$SECRET" c "$KEYS" "$VAL" 90 \
			"$1" "$2" 2>>"$D/$LABEL.err"
	}
	# Forwards attributable to THIS run, not the mode's lifetime total.
	# The claim "routing removes the hop" needs the plain and the routed
	# run counted separately; a single lifetime total cannot show it, and
	# quoting one from a side experiment is how the page ended up with a
	# forward count nothing could reproduce.
	fwdtot() {
		tf=0
		for n in 1 2 3; do
			v=$(cnum $n fwd_sent); tf=$((tf + ${v:-0}))
		done
		echo $tf
	}
	drivef() { # drive, with the forwards it caused appended
		fa=$(fwdtot)
		od=$(drive "$1" "$2")
		fb=$(fwdtot)
		echo "$od fwdrun=$((fb - fa))"
	}
	# WARM-UP, discarded.  The first fill into a proxy collection pays
	# probe-before-place on every key, so an unwarmed first run reads
	# ~2.5x slower than the ones after it - measured with routing OFF
	# throughout, so it is the locator warming and nothing else.  Left
	# in, it would have shown up as a routing benefit that does not
	# exist (proxy is not routed at all).
	drive 0 0 >/dev/null 2>&1

	JP=$(drivef 0 0); JR=$(drivef 1 0)
	BP=$(drivef 0 1); BR=$(drivef 1 1)

	# --- placement + convergence ------------------------------------
	CONV=""; CONVPCT=0; t=0
	while [ $t -lt ${CONV_WAIT:-120} ]; do
		A=$(entries 1); B=$(entries 2); C=$(entries 3)
		A=${A:-0}; B=${B:-0}; C=${C:-0}
		TOT=$((A + B + C))
		case "$LABEL" in
		store|eager)
			LOW=$A
			[ "$B" -lt "$LOW" ] && LOW=$B
			[ "$C" -lt "$LOW" ] && LOW=$C
			CONVPCT=$((LOW * 100 / KEYS))
			[ "$LOW" -ge "$KEYS" ] && CONV=$t;;
		*)
			CONVPCT=$((TOT * 100 / KEYS))
			[ "$TOT" -ge "$KEYS" ] && CONV=$t;;
		esac
		[ -n "$CONV" ] && break
		sleep 2; t=$((t + 2))
	done
	[ -n "$CONV" ] || CONV="none(${CONVPCT}%)"
	E1=$(entries 1); E2=$(entries 2); E3=$(entries 3)

	# --- per-node vitals --------------------------------------------
	L1=$(live 1); L2=$(live 2); L3=$(live 3)
	LIVE="$(( ${L1:-0} / 1024 ))k/$(( ${L2:-0} / 1024 ))k/$(( ${L3:-0} / 1024 ))k"
	F=0; U=0
	for n in 1 2 3; do
		v=$(cnum $n fwd_sent); F=$((F + ${v:-0}))
		v=$(cnum $n pull_sent); U=$((U + ${v:-0}))
	done

	# --- cross-node read: the cost on a node that did NOT write ------
	# 100% GET with the fill SKIPPED, so it reads the keyspace node 1
	# wrote.  Ordering is deliberate: after placement and vitals, so it
	# cannot distort them, and before the kill, because in store mode
	# what this pulls onto node 2 is exactly what node 2 still has when
	# node 1 dies - the two rows have to be read together.
	XR=$("$MM" "$(addr 2)" "$(port 2)" "$SECRET" c "$KEYS" "$VAL" 100 0 0 1 \
		2>>"$D/$LABEL.err")
	echo "  xnode read via node2: $XR" >&2

	# --- failure: kill the node that HOLDS the data -----------------
	VICTIM=$P1
	kill -9 $P1 2>/dev/null; P1=
	# WAIT FOR THE FLEET TO NOTICE - do not guess.  PEER_PURGE_MS is
	# 6000, so the fixed `sleep 6` this replaces sat exactly on the
	# detection boundary: proxy reported "writes still accepted" in one
	# run and "refused" in the next, from the same code, because the
	# sample landed on either side of the purge.  Poll the survivor's
	# own membership view and record how long it took, so the degraded
	# columns describe the steady state rather than the race.
	DET=""; t=0
	while [ $t -lt 30 ]; do
		M=$(members 2)
		[ -n "$M" ] && [ "$M" -le 2 ] && { DET=$t; break; }
		sleep 1; t=$((t + 1))
	done
	if [ -z "$DET" ]; then
		echo "MODEMATRIX: node 2 still lists ${M:-?} members after \
${t}s - $LABEL degraded columns are suspect" >&2
		DET="none"
	fi
	if [ -z "$VICTIM" ] || kill -0 "$VICTIM" 2>/dev/null; then
		echo "MODEMATRIX: victim did not die - $LABEL failure column \
is meaningless" >&2
		DEAD=no
	else
		DEAD=yes
	fi
	# HOW MUCH DATA SURVIVED - read the ORIGINAL keyspace through a
	# survivor with the fill SKIPPED.  The previous version let mmclient
	# fill first and then read back the 200 keys it had just written,
	# which reports 0 lost for EVERY mode including shard, where a third
	# of the keyspace had certainly gone.  A test that writes what it is
	# about to read cannot measure loss - the same defect this harness
	# already fixed once by killing the holder instead of an idle node.
	SURV=$("$MM" "$(addr 2)" "$(port 2)" "$SECRET" c "$KEYS" "$VAL" 100 0 0 1 \
		2>>"$D/$LABEL.err")
	MISS=$(field "$SURV" missed)
	[ -n "$MISS" ] || MISS="?"
	# ...and, separately, whether the degraded fleet still takes a write
	# at all.  A FRESH key, so that no locator left over from before the
	# kill points it at the dead node, and read back rather than trusted:
	# assert the effect, not the reply.
	"$CLI" -h "$(addr 2)" -p "$(port 2)" -a "$SECRET" \
		-j '{"method":"set","params":{"col":"c","key":"pc-afterkill","value":"alive"}}' \
		>/dev/null 2>&1
	if "$CLI" -h "$(addr 2)" -p "$(port 2)" -a "$SECRET" \
	    -j '{"method":"get","params":{"col":"c","key":"pc-afterkill"}}' 2>/dev/null \
	    | grep -q 'alive'; then
		DEGW=yes
	else
		DEGW=no
	fi

	four() { printf '%s/%s/%s/%s' "$(field "$JP" $1)" "$(field "$JR" $1)" \
		"$(field "$BP" $1)" "$(field "$BR" $1)"; }
	printf '%s\t%s\t%s\t%s\t%s\t%s/%s/%s\t%s\t%s\t%s\t%s\t%s\t%s/%s/%s/%s\t%s\t%s\t%s\t%s\n' \
		"$LABEL" "$(four fill)" "$(four mix)" "$(four p50)" \
		"$(four p99)" \
		"${E1:-?}" "${E2:-?}" "${E3:-?}" "$CONV" \
		"$LIVE" "$F" "$U" "$(four fwdrun)" \
		"$(field "$XR" mix)" "$(field "$XR" p50)" "$(field "$XR" p99)" \
		"$(field "$XR" missed)" \
		"${MISS:-?}" "$DEGW" "$DEAD" "$DET" >> "$D/results.tsv"
	echo "  json  plain/routed: $JP | $JR" >&2
	echo "  bin   plain/routed: $BP | $BR" >&2
	stop_fleet
}

printf '# build=%s date=%s host=%s keys=%s val=%s\n' "$BINREV" \
	"$(date -u +%Y-%m-%dT%H:%MZ)" "$(hostname)" "$KEYS" "$VAL" > "$D/results.tsv"
printf 'mode\tfill_jp/jr/bp/br\tmix_jp/jr/bp/br\tp50_jp/jr/bp/br\tp99_jp/jr/bp/br\tentries_n1/n2/n3\tconverge\tarena_n1/n2/n3\tfwd\tpull\tfwdrun_jp/jr/bp/br\txnode_ops/p50/p99/miss\tlost_of_keys\tdegr_write\tvictim_dead\tdetect_s\n' \
	>> "$D/results.tsv"

only() { case " ${MODES:-store eager proxy shard} " in *" $1 "*) return 0;; esac; return 1; }

only store && run_mode store "mode = store
collections = c"
only eager && run_mode eager "mode = store
eager = 1
collections = c"
only proxy && run_mode proxy "mode = proxy
collections = c"
only shard && run_mode shard "mode = shard
collections = c"

echo
echo "--- results ($D/results.tsv) ---"
cat "$D/results.tsv"
