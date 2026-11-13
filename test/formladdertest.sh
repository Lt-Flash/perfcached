#!/bin/sh
# formladdertest.sh - FORM_SIZES nodes started at once form one fleet under one founder (a second founder
# yields to it), and FORM_KILL survive the master's death mid-formation; ASan builds SKIP.
set -u
BIN=${1:-./perfcached}
SIZES=${FORM_SIZES:-3 4 6 8 12 16}
KILL=${FORM_KILL:-24}
if ldd "$BIN" 2>/dev/null | grep -qE 'libasan|libclang_rt\.asan'; then
	echo "formladdertest: SKIPPED - an ASan build (12oj 7 runs under plain check only)"
	exit 0
fi
D=$(mktemp -d /var/tmp/pcform.XXXXXX)
trap 'for f in "$D"/*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
PORT=24953
for p in 24951 24952 $PORT $((PORT + 2)); do
	ss -lntu 2>/dev/null | grep -qE "127\.0\.115\.[0-9]+:$p[[:space:]]" && { echo "formladdertest: port $p already bound" >&2; exit 1; }
done
SEEDS="127.0.115.1:$PORT,127.0.115.2:$PORT,127.0.115.3:$PORT"

conf() { # conf <dir> <n>: a blank eager node
	mkdir -p "$1/s$2"
	cat > "$1/n$2.conf" <<EOF
[daemon]
workers = 1
log_level = notice
state_dir = $1/s$2
[memory]
arena_mb = 16
[secrets]
client = fl-client-secret
cluster = fl-cluster-secret
[listen]
tcp = 127.0.115.$2:24951
http = 127.0.115.$2:24952
plaintext = loopback
[cluster]
discovery = unicast
port = $PORT
seeds = $SEEDS
advertise = 127.0.115.$2
mode = eager
collections = c
[collection c]
buckets_log2 = 10
EOF
	chmod 600 "$1/n$2.conf"
}
start() { "$BIN" -f "$1/n$2.conf" >> "$1/n$2.log" 2>&1 & echo $! > "$D/p$2.pid"; }
stopall() {
	for f in "$D"/p*.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; rm -f "$f"; done
	wait 2>/dev/null
}
# view <n> [skip]: every node 1..n but <skip> read at once; prints "ok <master> <rx_drops>" for one
# fleet of them all READY, else "no <what is wrong>"; with "mid": "<master> <members> <joiners...>"
view() {
	python3 - "$@" <<'EOF'
import json, sys, urllib.request
from concurrent.futures import ThreadPoolExecutor
n, skip = int(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 0
mid = len(sys.argv) > 3
nodes = [i for i in range(1, n + 1) if i != skip]
def get(i):
    try:
        return json.loads(urllib.request.urlopen("http://127.0.115.%d:24952/stats" % i, timeout=3).read())
    except Exception:
        return None
def find(d, k):
    if isinstance(d, dict):
        if k in d:
            return d[k]
        for v in d.values():
            r = find(v, k)
            if r is not None:
                return r
    return None
with ThreadPoolExecutor(16) as ex:
    rs = dict(zip(nodes, ex.map(get, nodes)))
dead = [i for i in nodes if not rs[i] or not rs[i].get("cluster")]
cl = {i: rs[i]["cluster"] for i in nodes if i not in dead}
masters = [i for i in cl if cl[i].get("role") == "master"]
if mid:
    m = masters[0] if len(masters) == 1 else 0
    late = [i for i in cl if cl[i].get("role") == "joining" or rs[i].get("state") == "connecting"]
    print(m, cl[m].get("peers_up", 0) if m else 0, " ".join(map(str, late)))
    sys.exit(0)
ids = {cl[i].get("cluster_id") for i in cl}
ups = [cl[i].get("peers_up") for i in cl]
states = {}
for i in cl:
    states[rs[i].get("state")] = states.get(rs[i].get("state"), 0) + 1
if not dead and len(masters) == 1 and len(ids) == 1 and set(ups) == {len(nodes) - 1} and \
        states == {"ready": len(nodes)}:
    print("ok", masters[0], find(cl[masters[0]], "rx_drops"))
else:
    print("no dead %s masters %s ids %d peers_up %s..%s states %s" % (dead[:8], masters[:8], len(ids),
          min(ups) if ups else "-", max(ups) if ups else "-", states))
EOF
}
wait_view() { # wait_view <s> <view args...>: until view says ok; V holds its last answer
	k=0
	while [ $k -lt $(($1 * 2)) ]; do
		V=$(view "$2" ${3:-})
		case "$V" in ok*) return 0;; esac
		sleep 0.5; k=$((k+1))
	done
	return 1
}
upto() { i=1; while [ $i -le $1 ]; do echo $i; i=$((i+1)); done; }

for N in $SIZES; do
	Z="$D/z$N"
	echo "--- L: $N nodes started at once"
	for n in $(upto $N); do conf "$Z" $n; done
	t0=$(date +%s)
	for n in $(upto $N); do start "$Z" $n; done
	LIM=$((60 + N * 3))
	M=0
	if wait_view $LIM $N; then
		M=$(echo "$V" | awk '{print $2}')
		ok "L$N. one fleet of $N, one master (node $(echo "$V" | awk '{print $2}')), every node READY, in $(( $(date +%s) - t0 )) s"
		DROPS=$(echo "$V" | awk '{print $3}')
		[ "$DROPS" = 0 ] && ok "L$N. no datagram dropped on the master's cluster socket" \
			|| bad "L$N. the master's cluster.rx_drops: $DROPS"
	else
		bad "L$N. not one fleet in $LIM s: $V"
	fi
	# two founders at once is accepted (a non-seed hears the others late on unicast seeds); the
	# one that stays is the master, every other yielded to it, and nobody was elected
	F=$(grep -l "founded the cluster" "$Z"/n*.log 2>/dev/null | wc -l)
	E=$(grep -l "elected master" "$Z"/n*.log 2>/dev/null | wc -l)
	NY=0
	for f in $(grep -l "founded the cluster" "$Z"/n*.log 2>/dev/null); do
		[ "$f" = "$Z/n$M.log" ] || grep -q "yielding mastership" "$f" || NY=$((NY+1))
	done
	grep -q "founded the cluster" "$Z/n$M.log" 2>/dev/null && [ "$NY" = 0 ] && [ "$E" = 0 ] \
		&& ok "L$N. node $M founded and stayed; $((F - 1)) other founder(s) yielded to it, none was elected" \
		|| bad "L$N. $F founded ($NY did not yield), $E elected, master $M: $(grep -lE 'founded the cluster|elected master' "$Z"/n*.log | sed 's|.*/||' | tr '\n' ' ')"
	S=$(cat "$Z"/n*.log | grep -c "admission stuck")
	echo "  info L$N: $S admission-stuck line(s); admitted ONLINE: $(cat "$Z"/n*.log | grep -c 'is ONLINE - all')"
	stopall
	grep -l CRIT "$Z"/n*.log >/dev/null 2>&1 && bad "L$N. CRIT logged: $(grep -h -m1 CRIT "$Z"/n*.log | head -1 | cut -c1-160)"
done

if [ "$KILL" -gt 0 ]; then
	Z="$D/k$KILL"
	echo "--- K: $KILL nodes, the master killed with joiners still queued"
	for n in $(upto $KILL); do conf "$Z" $n; done
	for n in $(upto $KILL); do start "$Z" $n; done
	M=0 LATE=
	k=0
	while [ $k -lt 240 ]; do
		set -- $(view $KILL 0 mid)
		if [ $# -ge 2 ] && [ "$1" != 0 ] && [ "$2" -ge 4 ] && [ $(($# - 2)) -ge 4 ]; then
			M=$1; shift 2; LATE="$*"; break
		fi
		sleep 0.5; k=$((k+1))
	done
	if [ "$M" = 0 ]; then
		bad "K. never caught the master with 4 members and 4 joiners: $(view $KILL)"
	else
		# a cold start's second founder yields before the kill: judge each log from the kill on
		for n in $LATE; do eval "K$n=\$(wc -l < \"\$Z/n\$n.log\")"; done
		kill -9 "$(cat "$D/p$M.pid")" 2>/dev/null; rm -f "$D/p$M.pid"
		ok "K. master node $M killed, $(echo $LATE | wc -w) node(s) still queued or CONNECTING"
		LIM=$((90 + KILL * 3))
		if wait_view $LIM $KILL $M; then
			NM=$(echo "$V" | awk '{print $2}')
			ok "K. one fleet of $((KILL - 1)) under node $NM, every node READY"
			case " $LATE " in *" $NM "*) bad "K. node $NM, queued or CONNECTING at the kill, is the new master";;
				*) ok "K. the new master was a member at the kill";; esac
		else
			bad "K. not one fleet of $((KILL - 1)) in $LIM s: $V"
		fi
		W= KY=0
		for n in $LATE; do
			eval "KC=\${K$n:-0}"
			head -n "$KC" "$Z/n$n.log" | grep -q "founded the cluster" && KY=$((KY+1))
			KL=$(tail -n +"$((KC + 1))" "$Z/n$n.log" | grep -m1 -E "founded the cluster|elected master" | cut -c1-120)
			[ -n "$KL" ] && W="$W $n ($KL)"
		done
		echo "  info K: $KY of the queued or CONNECTING had founded at the cold start and yielded before the kill"
		[ -z "$W" ] && ok "K. none of the queued or CONNECTING founded or was elected after the kill" \
			|| bad "K. founded or elected after the kill while queued or CONNECTING:$W"
	fi
	stopall
	grep -l CRIT "$Z"/n*.log >/dev/null 2>&1 && bad "K. CRIT logged: $(grep -h -m1 CRIT "$Z"/n*.log | head -1 | cut -c1-160)"
fi
echo "formladdertest: $pass passed, $fail failed"
[ $fail -eq 0 ]
