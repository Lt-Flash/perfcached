#!/bin/sh
# busypolltest.sh - S363: [daemon] busy_poll_us / busy_poll_prefer.
#  1. off by default: /stats io.busy_poll says 0 us, 0 workers; -E dumps it
#  2. busy_poll_us = 50, on each backend this host runs (io_uring if it
#     can, and epoll): -C says whether the host takes it; the daemon agrees
#     - every worker has it, or none has and /stats says why, and either
#     way it serves; /metrics carries the worker count
#  3. busy_poll_prefer = yes is shown; bad values are refused
#  4. a refusal (forced with PERFCACHED_BUSYPOLL_DENY=1, as an old kernel
#     refuses): -C says so, no worker has it, /stats says why, a WARNING,
#     and the daemon serves all the same
# A kernel before 6.9 refuses busy poll (the workers run without it); on
# loopback it never engages (no NAPI), so this proves the switch, not the
# latency - that is measured on a real NIC.
# Usage: test/busypolltest.sh ./perfcached
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
D=$(mktemp -d /var/tmp/pcbp.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
ss -ltn 2>/dev/null | grep -qE ":1980[12][[:space:]]" && { echo "busypolltest: port 19801/19802 already bound" >&2; exit 1; }

conf() { # conf <name> <extra [daemon] lines...>
	n=$1; shift
	mkdir -p "$D/s"
	{
		echo "[daemon]"; echo "workers = 2"; echo "log_level = notice"; echo "state_dir = $D/s"
		for l in "$@"; do echo "$l"; done
		cat <<EOF
[memory]
arena_mb = 32
[secrets]
client = bp-client-secret
[listen]
tcp = 127.0.0.1:19801
http = 127.0.0.1:19802
plaintext = loopback
[collection 0]
buckets_log2 = 10
EOF
	} > "$D/$n.conf"
	chmod 600 "$D/$n.conf"
}
bp() { python3 -c 'import json,sys,urllib.request
b=json.load(urllib.request.urlopen("http://127.0.0.1:19802/stats", timeout=5))["io"]
print(eval(sys.argv[1]))' "$1" 2>/dev/null; }
start() {
	rm -rf "$D/s"; mkdir -p "$D/s"
	"$BIN" -f "$D/$1.conf" > "$D/$1.log" 2>&1 & P=$!
	i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/$1.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/$1.log"
}
stop() { kill $P 2>/dev/null; wait $P 2>/dev/null; P=; }
serve() { python3 - <<'PY'
import socket
s = socket.create_connection(("127.0.0.1", 19801), 5); f = s.makefile("rb"); ok = 0
for i in range(200):
    k = b"k%d" % i
    s.sendall(b"*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$1\r\nv\r\n" % (len(k), k))
    ok += f.readline() == b"+OK\r\n"
print(ok)
PY
}

# ---- 1. off by default ---------------------------------------------------------
conf off
"$BIN" -E -f "$D/off.conf" 2>/dev/null | grep -qx "busy_poll_us = 0" && ok "1. -E: busy_poll_us = 0 by default" || bad "1. -E has no busy_poll_us = 0"
if start off; then
	[ "$(bp '"%d %d" % (b["busy_poll"]["us"], b["busy_poll"]["workers"])')" = "0 0" ] &&
		ok "1. /stats io.busy_poll: off, no worker" || bad "1. /stats io.busy_poll: $(bp 'b.get("busy_poll")')"
	! grep -qE "busy poll|busy_poll" "$D/off.log" && ok "1. nothing said about it" || bad "1. the log mentions busy poll when off"
	stop
else
	bad "1. the default config did not start"
fi

# ---- 2. on, per backend ----------------------------------------------------------
for io in io_uring epoll; do
	conf "on-$io" "io = $io" "busy_poll_us = 50"
	V=$("$BIN" -C -f "$D/on-$io.conf" 2>/dev/null | grep '^busy poll: ')
	case "$V" in
	"busy poll: 50 us, usable on this host"*) USABLE=1;;
	"busy poll: 50 us asked, but this host refuses it"*) USABLE=0;;
	*) USABLE=; bad "2. io = $io: -C has no busy poll verdict: '$V'";;
	esac
	[ -n "$USABLE" ] && ok "2. io = $io: -C: ${V#busy poll: }"
	if start "on-$io"; then
		W=$(bp 'b["busy_poll"]["workers"]'); R=$(bp 'b["busy_poll"]["refused"]')
		if [ "$USABLE" = 1 ]; then
			[ "$W" = 2 ] && [ -z "$R" ] && ok "2. io = $io: both workers busy poll, as -C said" ||
				bad "2. io = $io: workers $W, refused '$R' (-C said usable)"
			grep -q "NOTICE: busy poll: 50 us on the workers" "$D/on-$io.log" && ok "2. io = $io: the start line says it" ||
				bad "2. io = $io: no start line: $(grep -i busy "$D/on-$io.log" | head -1)"
		elif [ "$USABLE" = 0 ]; then
			[ "$W" = 0 ] && [ -n "$R" ] && ok "2. io = $io: refused - no worker has it, and /stats says why ($R)" ||
				bad "2. io = $io: workers $W, refused '$R' (-C said refused)"
			grep -q "WARNING: busy_poll_us = 50, but this host refuses it - the workers run without busy polling" "$D/on-$io.log" &&
				ok "2. io = $io: the refusal is a WARNING" || bad "2. io = $io: $(grep -i busy "$D/on-$io.log" | head -1)"
		fi
		[ "$(serve)" = 200 ] && ok "2. io = $io: it serves (200 SETs)" || bad "2. io = $io: serving failed"
		python3 -c 'import urllib.request
print([l for l in urllib.request.urlopen("http://127.0.0.1:19802/metrics").read().decode().splitlines() if l.startswith("perfcached_io_busy_poll_workers")][0])' 2>/dev/null |
			grep -q "perfcached_io_busy_poll_workers{us=\"50\"} $W" && ok "2. io = $io: /metrics carries it" || bad "2. io = $io: /metrics lacks perfcached_io_busy_poll_workers"
		stop
	else
		bad "2. io = $io, busy_poll_us = 50 did not start (a refusal must not stop it): $(tail -2 "$D/on-$io.log")"
	fi
done

# ---- 3. prefer, and the values -------------------------------------------------------
conf pref "busy_poll_us = 50" "busy_poll_prefer = yes"
if start pref; then
	[ "$(bp 'b["busy_poll"]["prefer"]')" = True ] && ok "3. busy_poll_prefer = yes is shown" || bad "3. prefer: $(bp 'b["busy_poll"]')"
	stop
else
	bad "3. busy_poll_prefer = yes did not start"
fi
# ---- 4. refused ------------------------------------------------------------------
conf deny "busy_poll_us = 50"
PERFCACHED_BUSYPOLL_DENY=1 "$BIN" -C -f "$D/deny.conf" 2>/dev/null | grep -q "^busy poll: 50 us asked, but this host refuses it - the workers would run without: refused by PERFCACHED_BUSYPOLL_DENY" &&
	ok "4. -C reports the refusal" || bad "4. -C: $(PERFCACHED_BUSYPOLL_DENY=1 "$BIN" -C -f "$D/deny.conf" 2>&1 | grep -i busy)"
rm -rf "$D/s"; mkdir -p "$D/s"
PERFCACHED_BUSYPOLL_DENY=1 "$BIN" -f "$D/deny.conf" > "$D/deny.log" 2>&1 & P=$!
i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/deny.log"; do sleep 0.1; i=$((i+1)); done
if grep -q "perfcached ready" "$D/deny.log"; then
	[ "$(bp 'b["busy_poll"]["workers"]')" = 0 ] && bp 'b["busy_poll"]["refused"]' | grep -q "PERFCACHED_BUSYPOLL_DENY" &&
		ok "4. no worker has it, and /stats says why" || bad "4. /stats io.busy_poll: $(bp 'b["busy_poll"]')"
	[ "$(grep -c "WARNING: busy_poll_us = 50, but this host refuses it" "$D/deny.log")" = 1 ] &&
		ok "4. one WARNING, not one a worker" || bad "4. warnings: $(grep -c 'refuses it' "$D/deny.log")"
	[ "$(serve)" = 200 ] && ok "4. and it serves (200 SETs)" || bad "4. serving failed"
	stop
else
	bad "4. a refused busy poll stopped the daemon: $(tail -2 "$D/deny.log")"
fi
for v in "busy_poll_us = -1" "busy_poll_us = 200000" "busy_poll_prefer = maybe"; do
	conf badv "$v"
	"$BIN" -C -f "$D/badv.conf" > /dev/null 2>&1 && bad "3. '$v' was accepted" || ok "3. '$v' is refused"
done
e=$(cat "$D"/*.log | grep -cE ' (ERROR|CRIT)')
[ "$e" = 0 ] && ok "no ERROR/CRIT" || bad "$e ERROR/CRIT: $(grep -hE ' (ERROR|CRIT)' "$D"/*.log | head -2)"
echo "busypolltest: $pass passed, $fail failed"
[ $fail -eq 0 ]
