#!/bin/sh
# iobusytest.sh - S379 (0.6.1): each worker's busy share - its time outside
# epoll_wait / io_uring_enter - and the alert that tells a node on epoll
# whose workers run out of time the lever it has.  One worker, pcbench
# (8 connections, depth 32) to saturate it, PERFCACHED_ALERT_FAST=60 (the
# alert's hold 5 s, its clear 1 s, its one-minute window 3 s) and
# [alerts] epoll_busy_pct = 20.
#  1. idle: /stats io.workers has a row per worker, under 10% busy;
#     /metrics has perfcached_io_worker_busy_seconds_total per worker
#  2. io = epoll under the load: the worker over 50% busy, io.epoll_busy a
#     warning with the advice for this host - "change io = epoll to io =
#     io_uring" where io_uring is usable, "not usable here either" where
#     not; cleared once the load stops
#  3. the default refused (PERFCACHED_URING_DENY=1), epoll as its fallback:
#     the same load raises it, naming the refusal
#  4. io_uring (where usable): the same load raises nothing
# Fail-first: 0.6.0.1 has no io.workers and no io.epoll_busy.
# Usage: test/iobusytest.sh ./perfcached [./pcbench]
set -u
BIN=${1:-./perfcached}
PB=${2:-./pcbench}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
case "$PB" in /*) ;; *) PB=$(pwd)/$PB;; esac
D=$(mktemp -d /var/tmp/pciobusy.XXXXXX)
P= L=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; [ -n "$L" ] && kill $L 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -x "$PB" ] || { echo "iobusytest: no pcbench at $PB (make pcbench)" >&2; exit 1; }
for p in 19781 19782 19783; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "iobusytest: port $p already bound" >&2; exit 1; }
done
export PERFCACHED_ALERT_FAST=60

conf() { # conf <name> [daemon lines...]
	n=$1; shift
	{
		echo "[daemon]"
		echo "workers = 1"
		echo "state_dir = $D/s"
		for l in "$@"; do echo "$l"; done
		cat <<C
[memory]
arena_mb = 64
[secrets]
client = iobusy-client-secret
[listen]
tcp = 127.0.0.1:19781
resp = 127.0.0.1:19782
http = 127.0.0.1:19783
plaintext = loopback
[collection 0]
buckets_log2 = 14
[alerts]
epoll_busy_pct = 20
C
	} > "$D/$n.conf"
	chmod 600 "$D/$n.conf"
}
st() { # st <python expr over s> - from /stats
	python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:19783/stats", timeout=5).read())
print(eval(sys.argv[1]))' "$1" 2>/dev/null
}
start() { # start <conf> [env assignment]
	rm -rf "$D/s"; mkdir -p "$D/s"
	if [ -n "${2:-}" ]; then
		env "$2" "$BIN" -f "$D/$1.conf" > "$D/$1.log" 2>&1 & P=$!
	else
		"$BIN" -f "$D/$1.conf" > "$D/$1.log" 2>&1 & P=$!
	fi
	i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/$1.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/$1.log"
}
stop() { kill $P 2>/dev/null; wait $P 2>/dev/null; P=; }
load() { # load <secs> - pcbench at the RESP door, in the background
	"$PB" -h 127.0.0.1 -p 19782 -P resp -c 8 -d 32 -n 20000 -v 100 -T "$1" -w 0 -q > "$D/load.out" 2>&1 & L=$!
}
unload() { [ -n "$L" ] && { wait $L 2>/dev/null; L=; }; }
alert() { # the io.epoll_busy alert: "level|text", or "none"
	st 'next(("%s|%s" % (a["level"], a["text"]) for a in s["alerts"]["active"] if a["key"] == "io.epoll_busy"), "none")'
}
until_alert() { # until_alert <secs>
	n=0; while [ $n -lt $(($1 * 4)) ]; do
		v=$(alert); [ "$v" != none ] && [ -n "$v" ] && { echo "$v"; return 0; }
		sleep 0.25; n=$((n+1))
	done
	echo none; return 1
}

conf dflt
if "$BIN" -C -f "$D/dflt.conf" 2>/dev/null | grep -q "^io: io_uring is usable on this host"; then USABLE=1; else USABLE=0; fi
echo "--- io_uring usable on this host: $USABLE"

# ---- 1. idle ----------------------------------------------------------------------
conf ep "io = epoll"
if start ep; then
	sleep 3
	w=$(st 'len(s["io"]["workers"])') b=$(st 's["io"]["workers"][0]["busy_pct_1m"]')
	[ "$w" = 1 ] && [ -n "$b" ] && [ "$b" != None ] && [ "$b" -lt 10 ] 2>/dev/null &&
		ok "1. idle: io.workers has the one worker, ${b}% busy" || bad "1. idle: workers '$w', busy '$b'"
	python3 -c 'import urllib.request,sys
m=urllib.request.urlopen("http://127.0.0.1:19783/metrics", timeout=5).read().decode()
sys.exit(0 if any(l.startswith("perfcached_io_worker_busy_seconds_total{worker=\"0\"} ") for l in m.splitlines()) else 1)' &&
		ok "1. /metrics perfcached_io_worker_busy_seconds_total{worker=\"0\"}" || bad "1. no busy seconds in /metrics"

	# ---- 2. io = epoll, saturated -----------------------------------------------------
	load 14
	v=$(until_alert 12)
	b=$(st 's["io"]["workers"][0]["busy_pct_1m"]')
	[ "$b" -gt 50 ] 2>/dev/null && ok "2. under the load the worker is ${b}% busy" || bad "2. busy under load: '$b' ($(cat "$D/load.out"))"
	if [ "$USABLE" = 1 ]; then
		case "$v" in "warning|"*"io_uring is usable here: change io = epoll to io = io_uring"*)
			ok "2. io.epoll_busy, a warning, says to change io = epoll to io = io_uring (${v#warning|})";;
			*) bad "2. io.epoll_busy: '$v'";; esac
	else
		case "$v" in "warning|"*"is not usable here either"*) ok "2. io.epoll_busy, a warning: io_uring not usable here either";;
			*) bad "2. io.epoll_busy: '$v'";; esac
	fi
	unload
	n=0; while [ $n -lt 40 ] && [ "$(alert)" != none ]; do sleep 0.25; n=$((n+1)); done
	[ "$(alert)" = none ] && ok "2. cleared once the load stopped" || bad "2. still raised: $(alert)"
	grep -q "alert.*epoll" "$D/ep.log" && ok "2. the raise is logged" || bad "2. no alert in the log"
	stop
else
	bad "1. io = epoll did not start: $(tail -3 "$D/ep.log")"
fi

# ---- 3. the default refused: epoll as a fallback -------------------------------------
if start dflt PERFCACHED_URING_DENY=1; then
	load 12
	v=$(until_alert 10)
	unload
	case "$v" in "warning|"*"io_uring was refused: refused by PERFCACHED_URING_DENY"*)
		ok "3. a fallback under load: io.epoll_busy names the refusal";;
		*) bad "3. io.epoll_busy: '$v'";; esac
	stop
else
	bad "3. the default did not start under PERFCACHED_URING_DENY=1: $(tail -3 "$D/dflt.log")"
fi

# ---- 4. io_uring: never ----------------------------------------------------------------
if [ "$USABLE" = 1 ]; then
	if start dflt; then
		[ "$(st 's["io"]["backend"]["active"]')" = io_uring ] || bad "4. not on io_uring: $(st 's["io"]["backend"]')"
		load 10
		v=$(until_alert 9)
		b=$(st 's["io"]["workers"][0]["busy_pct_1m"]')
		unload
		[ "$v" = none ] && ok "4. io_uring under the same load (${b}% busy): nothing raised" || bad "4. on io_uring: '$v'"
		stop
	else
		bad "4. the default did not start: $(tail -3 "$D/dflt.log")"
	fi
else
	echo "  skip 4. io_uring is not usable on this host"
fi

e=$(cat "$D"/*.log | grep -E ' (ERROR|CRIT)' | grep -vc ' alert: ')
[ "$e" = 0 ] && ok "no ERROR/CRIT besides the alerts raised" || bad "$e ERROR/CRIT: $(grep -hE ' (ERROR|CRIT)' "$D"/*.log | grep -v ' alert: ' | head -2)"
echo "iobusytest: $pass passed, $fail failed"
[ $fail -eq 0 ]
