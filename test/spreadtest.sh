#!/bin/sh
# spreadtest.sh — S127 Z2: `mode = spread` places each record on K nodes.
#
# THE CLAIM, and the only one this makes: a write reaches its K HOLDERS
# and nobody else.  That is the whole reason the mode exists.  Under
# eager every node applies every write in the fleet, so per-node apply
# equals the fleet's total write rate and one node's apply path caps the
# fleet however many nodes are added.  Under a copy factor the passive
# work is (K-1)/P per node and FALLS as the fleet grows.  If the push
# still went to every peer, the mode would be eager with extra config.
#
# HOW IT IS ASSERTED, and why not with `get`: a get on a non-holder
# PULLS from a holder and keeps the copy, so reading the keys back is
# the one thing that destroys the property under test.  Per-collection
# `entries` from /stats is pull-free, so the fleet-wide SUM of entries
# is the observable: K*N under spread, P*N under eager.
#
# EAGER IS RUN AS THE CONTROL, in the same harness with the same keys.
# Without it a broken push that dropped records would also read as
# "fewer copies than eager" and pass.
#
# Usage: test/spreadtest.sh [./perfcached]
set -u
BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcspread.XXXXXX)
P1= P2= P3= P4=
trap 'for v in "$P1" "$P2" "$P3" "$P4"; do [ -n "$v" ] && kill -9 "$v" 2>/dev/null; done; rm -rf "$D"' EXIT INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

NKEYS=120
MC=239.255.77.44
MP=17144

node() { # node <n> <cli-port> <http-port> <cluster-block>
	mkdir -p "$D/s$1"
	cat > "$D/n$1.conf" <<EOF
[daemon]
workers = 2
log_level = info
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = spread-client-secret
cluster = spread-cluster-secret
[listen]
tcp = 127.0.0.1:$2
http = 127.0.0.1:$3
plaintext = loopback
[cluster]
multicast = $MC:$MP
advertise = 127.0.7.$1
pull_timeout_ms = 200
$4
collections = b
[collection b]
buckets_log2 = 12
pull = 1
EOF
}
start() { # start <n> <var>
	"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	eval "P$2=\$!"
	i=0
	while [ $i -lt 120 ]; do
		grep -q "perfcached ready" "$D/n$1.log" 2>/dev/null && return 0
		sleep 0.1; i=$((i+1))
	done
	echo "  node $1 did not start:"; tail -4 "$D/n$1.log"; return 1
}
stopall() {
	for v in "$P1" "$P2" "$P3" "$P4"; do
		[ -n "$v" ] && kill -9 "$v" 2>/dev/null
	done
	P1= P2= P3= P4=
	sleep 0.5
}
cli() { printf '%s\n' "$2" | timeout 8 python3 -c '
import json, socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=6)
f = s.makefile("rwb")
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    r = json.loads(line); r["id"] = 1; r["jsonrpc"] = "2.0"
    f.write(json.dumps(r).encode() + b"\n"); f.flush()
    d = json.loads(f.readline())
    print(json.dumps(d.get("result", d.get("error"))))' "$1"; }

entries() { # entries <http-port>  -> entries in collection b, or empty
	timeout 6 python3 -c '
import json, sys, urllib.request
try:
    d = json.load(urllib.request.urlopen("http://127.0.0.1:%s/stats" % sys.argv[1], timeout=4))
except Exception:
    sys.exit(0)
for c in d.get("collections", []):
    if c.get("name") == "b":
        print(c.get("entries", 0)); break' "$1" 2>/dev/null
}

ready4() { # every node ready and seeing 3 peers
	i=0
	while [ $i -lt 150 ]; do
		a=$(cli 17401 '{"method":"stats"}' | python3 -c \
			'import json,sys; d=json.load(sys.stdin); print(d["cluster"]["peers_up"], d["state"])' 2>/dev/null)
		[ "$a" = "3 ready" ] && return 0
		sleep 0.2; i=$((i+1))
	done
	return 1
}

# writes NKEYS through node 1 and returns the fleet-wide sum of entries
run_arm() { # run_arm <cluster-block> <label>
	node 1 17401 18401 "$1"; node 2 17402 18402 "$1"
	node 3 17403 18403 "$1"; node 4 17404 18404 "$1"
	start 1 1 || return 1
	start 2 2 || return 1
	start 3 3 || return 1
	start 4 4 || return 1
	ready4 || { echo "  $2: fleet never formed"; return 1; }
	i=0
	while [ $i -lt $NKEYS ]; do
		printf '{"method":"set","params":{"col":"b","key":"sk%03d","value":"v%03d","ttl":600}}\n' $i $i
		i=$((i+1))
	done | timeout 30 python3 -c '
import json, socket, sys
s = socket.create_connection(("127.0.0.1", 17401), timeout=10)
f = s.makefile("rwb")
n = 0
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    r = json.loads(line); r["id"] = 1; r["jsonrpc"] = "2.0"
    f.write(json.dumps(r).encode() + b"\n"); f.flush()
    f.readline(); n += 1
print("wrote", n, file=sys.stderr)' 2>/dev/null
	# WAIT FOR THE COUNT TO SETTLE, do not sleep a fixed time.  The
	# gather groups flush on a timer and the repair sweep runs on its
	# own period, so a fixed wait samples a moving number: the same arm
	# read 240 once and 283 a moment earlier, and neither was wrong -
	# the measurement was just taken mid-convergence.
	prev=-1; stable=0; w=0
	while [ $w -lt 60 ]; do
		E1=$(entries 18401); E2=$(entries 18402)
		E3=$(entries 18403); E4=$(entries 18404)
		SUM=$(( ${E1:-0} + ${E2:-0} + ${E3:-0} + ${E4:-0} ))
		if [ "$SUM" = "$prev" ] && [ "$SUM" -gt 0 ]; then
			stable=$((stable+1))
			[ $stable -ge 3 ] && break
		else
			stable=0
		fi
		prev=$SUM
		sleep 1; w=$((w+1))
	done
	[ $stable -ge 3 ] || echo "  $2: WARNING count never settled (last $SUM)"
	echo "  $2: per-node entries ${E1:-?}/${E2:-?}/${E3:-?}/${E4:-?}  sum=$SUM"
	return 0                       # the CALLER stops the fleet: the
}                                      # read-back below needs it alive

# ---- arm A: the CONTROL.  eager must put every key on every node -----
run_arm "mode = eager" "eager(control)" || { echo "eager arm failed"; exit 1; }
EAGER=$SUM
WANT=$(( NKEYS * 4 ))
[ "$EAGER" -ge $(( WANT * 90 / 100 )) ] && ok "eager holds ~4 copies of each key (sum $EAGER, want ~$WANT)" \
	|| bad "eager sum $EAGER, expected ~$WANT - the CONTROL is broken, so the spread arm proves nothing"
stopall

# ---- arm B: spread K=2 -----------------------------------------------
run_arm "mode = spread
replicas = 2" "spread K=2" || { echo "spread arm failed"; exit 1; }
K2=$SUM
# THE NON-WRITER NODES are where the property is asserted, and the
# arithmetic is worth writing down.  With K=2 of P=4 and every write
# entering at node 1: node 1 is a holder for about half the keys, so of
# the 2N copies the fleet owes, about N/2 land on node 1 and about
# 3N/2 land on nodes 2-4.  For N=120 that is ~180 across the three.
OTHERS=$(( ${E2:-0} + ${E3:-0} + ${E4:-0} ))
WANT_OTHERS=$(( NKEYS * 3 / 2 ))
LO=$(( WANT_OTHERS * 75 / 100 )); HI=$(( WANT_OTHERS * 125 / 100 ))
[ "$OTHERS" -ge "$LO" ] && [ "$OTHERS" -le "$HI" ] \
	&& ok "the non-writing nodes hold their K=2 share ($OTHERS, want ~$WANT_OTHERS)" \
	|| bad "non-writing nodes hold $OTHERS, expected $LO..$HI"

# no single non-writer may hold the whole keyspace, and none may hold
# nothing: either would be placement collapsing rather than spreading
for e in "${E2:-0}" "${E3:-0}" "${E4:-0}"; do
	[ "$e" -gt $(( NKEYS / 5 )) ] && [ "$e" -lt $(( NKEYS * 9 / 10 )) ] \
		&& ok "a non-writer holds a share, not all and not none ($e of $NKEYS)" \
		|| bad "a non-writer holds $e of $NKEYS - placement collapsed"
done

# EXACT-K RETENTION, decided 2026-09-13.  A node that is not in the
# top-K set may ACCEPT and FORWARD a write - any-member write is about
# admission, not placement - but must not keep it.  Every client here
# writes through node 1, so if non-holders retained, node 1 would hold
# all N; it must instead hold only its placement share, ~N*K/P.
WANT_W=$(( NKEYS * 2 / 4 ))
WLO=$(( WANT_W * 60 / 100 )); WHI=$(( WANT_W * 140 / 100 ))
[ "${E1:-0}" -ge "$WLO" ] && [ "${E1:-0}" -le "$WHI" ] \
	&& ok "the writer keeps only its share, not everything (${E1} of $NKEYS, want ~$WANT_W)" \
	|| bad "the writer holds ${E1:-?} of $NKEYS, expected $WLO..$WHI - non-holders are retaining"

# and the fleet total is now K*N, the number the mode's name promises
WANT_T=$(( NKEYS * 2 ))
TLO=$(( WANT_T * 85 / 100 )); THI=$(( WANT_T * 115 / 100 ))
[ "$K2" -ge "$TLO" ] && [ "$K2" -le "$THI" ] \
	&& ok "the fleet holds K*N copies (sum $K2, want ~$WANT_T)" \
	|| bad "fleet holds $K2, expected $TLO..$THI - copies are not bounded by K"

# The differential is the assertion that cannot be faked by a push that
# simply drops records: eager and spread ran the same keys through the
# same harness, and only the copy count differs.
[ "$K2" -lt "$EAGER" ] && ok "spread stores strictly fewer copies than eager ($K2 < $EAGER)" \
	|| bad "spread stored $K2 and eager $EAGER - the mode changed nothing"

# ---- DROPPING MUST NOT LOSE DATA -------------------------------------
# Node 1 accepted every write and kept only ~half.  Every key must still
# be readable THROUGH IT: the ones it holds locally, the ones it has to
# pull from a holder.  Counting copies proves the bound; this proves the
# bound was achieved by placing records rather than by losing them.
# It runs LAST because a get on a non-holder pulls and keeps a copy,
# which would corrupt the entry counts above.
MISS=$(i=0; while [ $i -lt $NKEYS ]; do
	printf '{"method":"get","params":{"col":"b","key":"sk%03d"}}\n' $i
	i=$((i+1))
done | timeout 40 python3 -c '
import json, socket, sys
s = socket.create_connection(("127.0.0.1", 17401), timeout=10)
f = s.makefile("rwb")
miss = 0
for line in sys.stdin:
    line = line.strip()
    if not line: continue
    r = json.loads(line); r["id"] = 1; r["jsonrpc"] = "2.0"
    f.write(json.dumps(r).encode() + b"\n"); f.flush()
    d = json.loads(f.readline()).get("result", {})
    if not isinstance(d, dict) or not d.get("found"):
        miss += 1
print(miss)' 2>/dev/null)
[ "${MISS:-999}" = "0" ] \
	&& ok "all $NKEYS keys still readable through the node that dropped half" \
	|| bad "${MISS:-?} of $NKEYS keys are GONE - dropping lost data"

echo "spreadtest: $pass passed, $fail failed"
[ $fail -eq 0 ] || exit 1
exit 0
