#!/bin/sh
# termstalltest.sh - the mastership term and the cluster id are written to disk off the cluster thread:
# with their fsyncs held 2.5 s a fleet forms and elects a new master after its master is killed, and no
# held sync is the cluster thread's.  Usage: test/termstalltest.sh [./perfcached] [./syncfailshim.so]
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./syncfailshim.so}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
[ -f "$SHIM" ] || { echo "termstalltest: $SHIM is missing (make syncfailshim.so)" >&2; exit 1; }
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
D=
trap '[ -n "$D" ] && { kill -9 $(cat "$D"/*.pid 2>/dev/null) 2>/dev/null; rm -rf "$D"; }' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
finish() { echo "termstalltest: $pass passed, $fail failed"; [ $fail -eq 0 ]; exit; }
for p in 23671 23672 23673 23681 23682 23683; do
	ss -ltn 2>/dev/null | grep -q ":$p[[:space:]]" && { echo "termstalltest: port $p busy" >&2; exit 1; }
done
D=$(mktemp -d /var/tmp/pcts.XXXXXX)

conf() { # conf <n>
	mkdir -p "$D/s$1"
	cat > "$D/n$1.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$1
[memory]
arena_mb = 64
[secrets]
client = ts-client-secret
cluster = ts-cluster-secret
[listen]
tcp = 127.0.0.1:2367$1
http = 127.0.0.1:2368$1
plaintext = loopback
[cluster]
multicast = 239.255.78.62:23699
advertise = 127.0.237.$1
pull_timeout_ms = 400
mode = store
collections = c
[collection c]
buckets_log2 = 10
C
	chmod 600 "$D/n$1.conf"
}
start() { # start <n>: under the shim, stateslow armed from the start
	: > "$D/n$1.log"
	LD_PRELOAD="${SANRT:+$SANRT }$SHIM${PC_NOSYNC_LIB:+ $PC_NOSYNC_LIB}" PC_SYNCFAIL_CTL="$D/ctl" PC_SYNCFAIL_LOG="$D/sync.log" \
		PC_SYNCFAIL_STATE_NAME="node-term.tmp,cluster-id.tmp" PC_SYNCFAIL_STATE_MS=2500 \
		"$BIN" -f "$D/n$1.conf" > "$D/n$1.log" 2>&1 &
	echo $! > "$D/n$1.pid"
	i=0; while [ $i -lt 300 ]; do
		grep -q "perfcached ready" "$D/n$1.log" && return 0
		kill -0 "$(cat "$D/n$1.pid")" 2>/dev/null || break
		sleep 0.1; i=$((i+1))
	done
	echo "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"
	return 1
}
st() { curl -s "http://127.0.0.1:2368$1/stats" | python3 -c '
import json, sys
r = json.load(sys.stdin)
print(eval(sys.argv[1]))' "$2" 2>/dev/null; }
role() { st "$1" 'r["cluster"]["role"]'; }
term() { st "$1" '(r["cluster"].get("map") or {}).get("term", 0)'; }
# masters <nodes...>: how many of them say master, then the one that does
masters() { c=0; m=0; for n in "$@"; do [ "$(role $n)" = master ] && { c=$((c+1)); m=$n; }; done; echo "$c $m"; }
pc_notes() { grep -c "^stateslow pc-cluster " "$D/sync.log" 2>/dev/null; }

for n in 1 2 3; do conf $n; done
echo stateslow > "$D/ctl"
for n in 1 2 3; do start $n || { bad "node $n did not start"; finish; }; done

# the fleet forms: the founder claims a term and mints the id, the others take both
i=0; M=0
while [ $i -lt 120 ]; do
	read -r C M <<E
$(masters 1 2 3)
E
	[ "$C" = 1 ] && [ "$(st 1 'r["state"]')$(st 2 'r["state"]')$(st 3 'r["state"]')" = readyreadyready ] && break
	sleep 0.5; i=$((i+1))
done
T1=$(term "$M")
[ "$C" = 1 ] && [ "${T1:-0}" -ge 1 ] \
	&& ok "with the term's and the id's fsyncs held 2.5 s, one fleet formed under node $M (term $T1)" \
	|| bad "no single fleet with the hold: $C master(s), roles $(role 1)/$(role 2)/$(role 3), term ${T1:-?}"
CL0=$(pc_notes)

# a new master: the survivors elect one, which claims a later term, and the other takes it
[ "$M" != 0 ] && kill -9 "$(cat "$D/n$M.pid")" 2>/dev/null
REST=""; for n in 1 2 3; do [ "$n" = "$M" ] || REST="$REST $n"; done
i=0; C2=0; M2=0
while [ $i -lt 120 ]; do
	# shellcheck disable=SC2086
	read -r C2 M2 <<E
$(masters $REST)
E
	[ "$C2" = 1 ] && [ "${M2:-0}" != 0 ] && [ "$(term "$M2")" -gt "${T1:-0}" ] 2>/dev/null && break
	sleep 0.5; i=$((i+1))
done
T2=$(term "${M2:-0}")
[ "$C2" = 1 ] && [ "${T2:-0}" -gt "${T1:-0}" ] 2>/dev/null \
	&& ok "the master killed, node $M2 was elected at term $T2 (was $T1)" \
	|| bad "no new master after node $M was killed: $C2 master(s) among$REST, term ${T2:-?} (was ${T1:-?})"

# the cause, from the shim's notes: which thread waited on which file
# (not the starts' own writes, on the main thread)
NT=$(grep -v "^stateslow perfcached " "$D/sync.log" 2>/dev/null | grep -c "^stateslow .*/node-term.tmp")
NC=$(grep -v "^stateslow perfcached " "$D/sync.log" 2>/dev/null | grep -c "^stateslow .*/cluster-id.tmp")
CL=$(pc_notes)
[ "${NT:-0}" -ge 3 ] && [ "${NC:-0}" -ge 3 ] \
	&& ok "the held syncs were reached: $NT of the term file, $NC of the cluster id" \
	|| bad "the term file was synced ${NT:-0} and the cluster id ${NC:-0} times under the hold - the stall was never reached"
[ "${CL:-0}" = 0 ] && ok "no held sync was the cluster thread's" \
	|| bad "the cluster thread waited on $CL held syncs (${CL0:-0} by the time the fleet formed): $(grep '^stateslow pc-cluster ' "$D/sync.log" | head -3 | sed "s|$D/||g" | tr '\n' ';')"
[ $fail -eq 0 ] || sed "s|$D/||g; s|\.tmp\.[0-9]*|.tmp|" "$D/sync.log" 2>/dev/null | sort | uniq -c | head -12 | sed 's/^/        /'
finish
