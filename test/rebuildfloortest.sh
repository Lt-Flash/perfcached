#!/bin/sh
# rebuildfloortest.sh - S463 x S455: a rebuild a floor asks for is not an
# absence, and never S455's resync.
#
# THE BUG.  An origin asked for deletes it no longer holds floors the
# member's mark and asks it to rebuild with the age 0xffffffff - "every
# record".  The member took the age as an absence: ~4,294,967 s, past
# resync_after_s, so S455 dropped every record in question and pulled the
# fleet's whole state - the node back to RECOVERING after it had said
# ready, requests refused meanwhile.  AR13 on 0.7.4.4-rc1 (245-247): after
# both kills of 245 and both clean restarts of 246 under a delete stream.
#
# Two eager nodes, resync_after_s 20; node 2 holds node 1's 100 keys, node 1
# up 32 s (a resync source); clinject plays a member (its ALIVE) and sends
# node 2 one rebuild ask.  Its ALIVE says no start kind, so the reconcile
# waits until it stops counting as live (10 s).  A fresh fleet per arm:
#   A. the age 25 s, past resync_after_s - the premise: S455 resyncs node 2
#   B. the age 0xffffffff, a floor's - node 2 takes the rebuild, does not
#      resync in as long as A took and more, stays ready, keeps all 100 keys
# Fail-first (GitLab 112573, without the fix): A resynced after 7 s; B took the
# ask for ~4,294,967 s away, resynced, and went ready -> recovering.
# Usage: test/rebuildfloortest.sh [./perfcached] [./clinject]
set -u
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
BIN=${1:-./perfcached}
INJ=${2:-./clinject}
D=$(mktemp -d /var/tmp/pcrbf.XXXXXX)
PIDS=""
down() { for p in $PIDS; do kill -9 $p 2>/dev/null; done; wait $PIDS 2>/dev/null; PIDS=""; }
trap 'down; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=25190
SECRET=rbf-cluster-secret
[ -x "$INJ" ] || { echo "rebuildfloortest: $INJ not built" >&2; exit 1; }
for p in 25171 25172 25181 25182; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "rebuildfloortest: port $p already bound" >&2; exit 1; }
done

conf() { # conf <i>
	mkdir -p "$D/s$1" "$D/w$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 32
[secrets]
client = rbf-client-secret
cluster = $SECRET
[listen]
tcp = 127.0.0.1:2517$1
http = 127.0.0.1:2518$1
plaintext = loopback
[cluster]
discovery = unicast
port = $PORT
seeds = 127.0.164.1
advertise = 127.0.164.$1
mode = eager
resync_after_s = 20
[wal]
dir = $D/w$1
probe = no
save = off
[collection c]
buckets_log2 = 10
mode = eager
EOF
	chmod 600 "$D/n$1.conf"
}
start() { "$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 & PIDS="$PIDS $!"; }
st() { # st <i> <python expr over r (the /stats document)>
	python3 -c 'import json,sys,urllib.request
r=json.loads(urllib.request.urlopen("http://127.0.0.1:%d/stats" % (25180 + int(sys.argv[1])),timeout=5).read())
print(eval(sys.argv[2]))' "$1" "$2" 2>/dev/null || echo "?"
}
wait_for() { # wait_for <secs> <shell condition>
	k=0
	while [ $k -lt $(($1 * 5)) ]; do
		eval "$2" && return 0
		sleep 0.2; k=$((k+1))
	done
	return 1
}
fill() { python3 - <<'PY' 2>&1
import socket, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.0.1", 25171), 10))
print(sum(1 for i in range(100) if "error" not in pcnative.call(f, "set", col="c", key="k%d" % i, value="v%d" % i)))
PY
}
inj() { "$INJ" -s "$SECRET" -c "$CID" -f 127.0.164.9:25199 -t 127.0.164.2:$PORT "$@" 2>&1; }

arm() { # arm <name> <age hex, LE> <what the ask stands for> <seconds to watch>
	RS=? RB=? STATE=? HELD=? BACK=? TOOK=0
	down; rm -rf "$D"/s* "$D"/w* "$D"/n*.log
	conf 1; conf 2
	start 1
	wait_for 30 '[ "$(st 1 "r[\"state\"]")" = ready ]' || { bad "$1: node 1 did not start: $(tail -2 "$D/n1.log" | tr '\n' ' ')"; return; }
	start 2
	T0=$(date +%s)
	wait_for 30 '[ "$(st 2 "r[\"state\"]")" = ready ] && [ "$(st 2 "r[\"cluster\"][\"peers_up\"]")" = 1 ]' \
		|| { bad "$1: node 2 did not join: $(tail -2 "$D/n2.log" | tr '\n' ' ')"; return; }
	# a just-formed fleet may refuse its first writes; the keys are the same each try
	wait_for 20 '[ "$(fill)" = 100 ]' || { bad "$1: node 1 took $(fill) of 100 writes"; return; }
	wait_for 20 '[ "$(st 2 "sum(c[\"entries\"] for c in r[\"collections\"] if c[\"name\"] == \"c\")")" = 100 ]' \
		|| { bad "$1: node 2 holds $(st 2 'sum(c["entries"] for c in r["collections"] if c["name"] == "c")') of 100, peers_up $(st 2 'r["cluster"]["peers_up"]'); $(grep -E "WARNING|ERROR" "$D/n2.log" | tail -3 | cut -c1-200 | tr '\n' ' ')"; return; }
	# node 1 a resync source: up 30 s (WITNESS_GRACE_MS) first
	w=$((32 - $(date +%s) + T0)); [ $w -gt 0 ] && sleep $w
	CID=$(st 2 'r["cluster"]["cluster_id"]')
	inj alive 88 > "$D/inj-alive.out"
	sleep 0.3
	B=$(wc -l < "$D/n2.log")
	inj raw "3300$2" > "$D/inj-ask.out"
	T1=$(date +%s)
	wait_for 5 '[ "$(st 2 "r[\"cluster\"][\"acks\"][\"rebuilds\"]")" = 1 ]' \
		|| { bad "$1: the ask never reached node 2's handler (rebuilds $(st 2 'r["cluster"]["acks"]["rebuilds"]'), peers_up $(st 2 'r["cluster"]["peers_up"]')): $(tail -n +$((B + 1)) "$D/n2.log" | tail -3 | cut -c1-200 | tr '\n' ' ')"; return; }
	wait_for "$4" '[ "$(st 2 "r[\"cluster\"][\"resyncs\"]")" -ge 1 ] 2>/dev/null' || true
	TOOK=$(($(date +%s) - T1))
	RS=$(st 2 'r["cluster"]["resyncs"]'); RB=$(st 2 'r["cluster"]["acks"]["rebuilds"]')
	STATE=$(st 2 'r["state"]')
	HELD=$(st 2 'sum(c["entries"] for c in r["collections"] if c["name"] == "c")')
	BACK=$(tail -n +$((B + 1)) "$D/n2.log" | grep -c "ready -> recovering")
	SAID=$(tail -n +$((B + 1)) "$D/n2.log" | grep -E "resync -|says this node was away|no longer holds deletes|reconcile SKIPPED|reconcile - node" | head -3 | cut -c1-170 | tr '\n' ' ')
	echo "  ..   $1 ($3): after $TOOK s rebuilds $RB, resyncs $RS, state $STATE, holds $HELD, ready->recovering $BACK; $SAID"
}

echo "--- A. an ask 25 s old, past resync_after_s (20 s): the premise"
arm A a8610000 "an absence of 25 s" 25
ATOOK=${TOOK:-0}
[ "$RS" -ge 1 ] 2>/dev/null \
	&& ok "A. node 2 took S455's resync for an absence past resync_after_s (resyncs $RS)" \
	|| bad "A. no resync for an absence past resync_after_s: resyncs ${RS:-?}, rebuilds ${RB:-?} - the suite cannot see the one B must not take"

echo "--- B. a floor's ask (0xffffffff)"
w=$((ATOOK + 3)); [ $w -lt 15 ] && w=15
arm B ffffffff "a floor - no absence" $w
[ "$RB" = 1 ] 2>/dev/null && ok "B. node 2 took the floor's rebuild (rebuilds 1)" \
	|| bad "B. node 2's rebuilds ${RB:-?} after the floor's ask (want 1)"
[ "$RS" = 0 ] && [ "$BACK" = 0 ] && [ "$STATE" = ready ] \
	&& ok "B. no resync for a floor: node 2 stayed ready (resyncs 0, never back to recovering)" \
	|| bad "B. a floor taken as an absence: resyncs ${RS:-?}, ready->recovering $BACK, state ${STATE:-?}"
[ "$HELD" = 100 ] && ok "B. node 2 holds all 100 keys after the rebuild" \
	|| bad "B. node 2 holds ${HELD:-?} of 100 keys after the rebuild"

echo "rebuildfloortest: $pass passed, $fail failed"
[ $fail -eq 0 ]
