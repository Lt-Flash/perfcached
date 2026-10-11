#!/bin/sh
# iouringtest.sh - S361 (0.6.0) step 1: [daemon] io, the startup check and
# the fallback to epoll.
#  1. the default (io_uring): -C says what THIS host would run - usable
#     (with its buffer tier) or not (with the reason); the daemon's start
#     line, /stats io.backend and /metrics agree with it
#  2. io = epoll: no check, epoll, no fallback reason
#  3. PERFCACHED_URING_DENY=1 makes the check refuse: the default falls
#     back with a NOTICE, a configured io_uring with a WARNING; the reason
#     is in /stats and /metrics says active="epoll"
#  4. io = <anything else> is a config error; -E dumps the value
#  6. a reply never reaches another connection: 1000 times, client A sends
#     a request that is answered with an error and closes unread, and
#     client B, connected straight after, must get exactly +PONG.  A queued
#     SEND names its socket by fd NUMBER; if A's fd closed before the SQE
#     was submitted, B's accept took the number and got A's reply (found by
#     fuzztest on GitHub; the fix submits before the close)
#  5. on a usable host the workers RUN io_uring: every worker's ring is up,
#     requests are served (RESP, pipelined and one at a time, a 200 KB value
#     both ways), input arrives by receive completions and output leaves
#     by sends (step 3) - no read(), no write()
# Usage: test/iouringtest.sh ./perfcached
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
D=$(mktemp -d /var/tmp/pciou.XXXXXX)
P=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
for p in 19731 19741; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "iouringtest: port $p already bound" >&2; exit 1; }
done

conf() { # conf <name> [io line]
	mkdir -p "$D/s"
	{
		echo "[daemon]"
		echo "workers = 2"
		echo "log_level = notice"
		echo "state_dir = $D/s"
		[ -n "${2:-}" ] && echo "$2"
		cat <<EOF
[memory]
arena_mb = 32
[secrets]
client = iou-client-secret
[listen]
tcp = 127.0.0.1:19731
http = 127.0.0.1:19741
plaintext = loopback
[collection 0]
buckets_log2 = 10
EOF
	} > "$D/$1.conf"
	chmod 600 "$D/$1.conf"
}
backend() { # backend <python expr over b> - from /stats io.backend
	python3 -c 'import json,sys,urllib.request
b=json.loads(urllib.request.urlopen("http://127.0.0.1:19741/stats", timeout=5).read())["io"]["backend"]
print(eval(sys.argv[1]))' "$1" 2>/dev/null
}
info() { # the /metrics io_backend_info line
	python3 -c 'import urllib.request
for l in urllib.request.urlopen("http://127.0.0.1:19741/metrics", timeout=5).read().decode().splitlines():
    if l.startswith("perfcached_io_backend_info{"): print(l)' 2>/dev/null
}
start() { # start <conf> [env assignment] - runs it, waits ready
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

# ---- 1. the default --------------------------------------------------------------
conf dflt
C=$("$BIN" -C -f "$D/dflt.conf" 2>/dev/null | grep '^io: ')
USABLE= TIER=-
case "$C" in
"io: io_uring is usable on this host ("*) USABLE=1
	TIER=$(echo "$C" | sed -n 's/^io: io_uring is usable on this host (\([^)]*\)).*/\1/p')
	ok "1. -C: io_uring is usable here ($TIER)";;
"io: epoll - io_uring is not usable on this host: "*) USABLE=0; TIER=none
	ok "1. -C: io_uring is not usable here - ${C#io: epoll - io_uring is not usable on this host: }";;
*) USABLE=; bad "1. -C has no io verdict: '$C'";;
esac
"$BIN" -E -f "$D/dflt.conf" 2>/dev/null | grep -qx "io = io_uring" && ok "1. -E: io = io_uring is the default" ||
	bad "1. -E does not dump io = io_uring"
if start dflt; then
	[ "$(backend 'b["configured"]')" = io_uring ] && ok "1. /stats: configured io_uring" ||
		bad "1. /stats io.backend.configured: $(backend 'b.get("configured")')"
	[ "$(backend 'b["buffers"]')" = "$TIER" ] && ok "1. /stats: buffers '$TIER', as -C said" ||
		bad "1. /stats buffers '$(backend 'b.get("buffers")')' vs -C '$TIER'"
	if [ "$USABLE" = 1 ]; then
		[ "$(backend '"%s %d %s" % (b["active"], b["workers_uring"], b["fallback"] or "-")')" = "io_uring 2 -" ] &&
			ok "1. usable: the workers run io_uring (2 of 2), no fallback" ||
			bad "1. usable host: $(backend 'b')"
		grep -q "NOTICE: io: io_uring ($TIER" "$D/dflt.log" && ok "1. the start line says it" ||
			bad "1. no start line: $(grep 'io:' "$D/dflt.log" | head -1)"
		# 5. it serves, and its input comes by receive completions
		R0=$(python3 -c 'import json,urllib.request; s=json.loads(urllib.request.urlopen("http://127.0.0.1:19741/stats").read())["io"]; print(s["reads"] + s["writes"])')
		python3 - > "$D/serve.out" 2>&1 <<'PY'
import socket
s = socket.create_connection(("127.0.0.1", 19731), 10)
f = s.makefile("rb")
n = 300
s.sendall(b"".join(b"*3\r\n$3\r\nSET\r\n$%d\r\nk%d\r\n$5\r\nhello\r\n" % (len(b"k%d" % i), i) for i in range(n)))
ok = sum(1 for _ in range(n) if f.readline().startswith(b"+OK"))
for i in range(200):
    s.sendall(b"*2\r\n$3\r\nGET\r\n$%d\r\nk%d\r\n" % (len(b"k%d" % i), i))
    if f.readline().startswith(b"$5") and f.readline() == b"hello\r\n":
        ok += 1
big = b"x" * 200000
s.sendall(b"*3\r\n$3\r\nSET\r\n$3\r\nbig\r\n$%d\r\n%s\r\n" % (len(big), big))
ok += f.readline().startswith(b"+OK")
s.sendall(b"*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n")
h = f.readline()
ok += h == b"$200000\r\n" and f.read(200002) == big + b"\r\n"
print("ok", ok)
PY
		R1=$(python3 -c 'import json,urllib.request; s=json.loads(urllib.request.urlopen("http://127.0.0.1:19741/stats").read())["io"]; print(s["reads"] + s["writes"])')
		grep -q "^ok 502$" "$D/serve.out" && ok "5. served on io_uring: 300 pipelined SETs, 200 GETs one at a time, a 200 KB value both ways" ||
			bad "5. serving on io_uring: $(cat "$D/serve.out")"
		python3 - > "$D/cross.out" 2>&1 <<'PY'
import socket
bad = []
for i in range(1000):
    a = socket.create_connection(("127.0.0.1", 19731), 5)
    a.sendall(b"*2\r\n$3\r\nSET\r\n$1\r\nk\r\n")      # wrong arity: an error reply
    a.close()                                                # unread
    b = socket.create_connection(("127.0.0.1", 19731), 5)
    b.settimeout(5)
    b.sendall(b"*1\r\n$4\r\nPING\r\n")
    r = b.recv(64)
    b.close()
    if r != b"+PONG\r\n":
        bad.append((i, r[:40]))
print("bad", len(bad), bad[:3])
PY
		grep -q "^bad 0 " "$D/cross.out" && ok "6. 1000 times, a reply never reached another connection" ||
			bad "6. replies crossed connections: $(cat "$D/cross.out")"
		[ "$R1" = "$R0" ] && [ "$(backend 'b["active"]')" = io_uring ] &&
			python3 -c 'import json,sys,urllib.request; s=json.loads(urllib.request.urlopen("http://127.0.0.1:19741/stats").read())["io"]; sys.exit(0 if s["uring_receives"] > 0 and s["uring_sends"] > 0 and s["uring_enters"] > 0 else 1)' &&
			ok "5. no read() or write() calls - input by receive completions, output by sends (step 3)" ||
			bad "5. reads + writes moved $R0 -> $R1, or no receive/send completions"
	else
		[ "$(backend 'b["active"]')" = epoll ] && [ -n "$(backend 'b["fallback"]')" ] &&
			ok "1. falls back to epoll with the reason in /stats" ||
			bad "1. active $(backend 'b.get("active")'), fallback '$(backend 'b.get("fallback")')'"
		grep -q "NOTICE: io: epoll - io_uring (the default) is not usable here" "$D/dflt.log" &&
			! grep -q "WARNING: io = io_uring" "$D/dflt.log" &&
			ok "1. a defaulted fallback is a NOTICE, not a WARNING" ||
			bad "1. start line: $(grep 'io' "$D/dflt.log" | head -2)"
	fi
	info | grep -q "configured=\"io_uring\",active=\"$(backend 'b["active"]')\",buffers=\"$TIER\"" &&
		ok "1. /metrics agrees: $(info)" || bad "1. /metrics: '$(info)'"
	stop
else
	bad "1. the default config did not start: $(tail -2 "$D/dflt.log")"
fi

# ---- 2. io = epoll -----------------------------------------------------------------
conf ep "io = epoll"
"$BIN" -C -f "$D/ep.conf" 2>/dev/null | grep -qx "io: epoll (io = epoll)" && ok "2. -C: epoll, as configured" ||
	bad "2. -C: $("$BIN" -C -f "$D/ep.conf" 2>&1 | grep io)"
if start ep; then
	[ "$(backend '"%s %s %s %s" % (b["configured"], b["active"], b["buffers"], b["fallback"] or "-")')" = "epoll epoll none -" ] &&
		ok "2. /stats: configured epoll, active epoll, no fallback reason" ||
		bad "2. /stats io.backend: $(backend 'b')"
	grep -q "NOTICE: io: epoll (io = epoll)" "$D/ep.log" && ok "2. the start line" || bad "2. no start line"
	stop
else
	bad "2. io = epoll did not start: $(tail -2 "$D/ep.log")"
fi

# ---- 3. the check refuses ------------------------------------------------------------
if start dflt PERFCACHED_URING_DENY=1; then
	backend 'b["fallback"]' | grep -q "PERFCACHED_URING_DENY" && [ "$(backend 'b["active"]')" = epoll ] &&
		ok "3. refused: epoll, the reason in /stats" || bad "3. /stats io.backend: $(backend 'b')"
	grep -q "NOTICE: io: epoll - io_uring (the default) is not usable here: refused by PERFCACHED_URING_DENY" "$D/dflt.log" &&
		! grep -q "WARNING: io" "$D/dflt.log" &&
		ok "3. the default's fallback is a NOTICE" || bad "3. start: $(grep 'io' "$D/dflt.log" | head -2)"
	info | grep -q 'active="epoll",buffers="none"' && ok "3. /metrics: active epoll" || bad "3. /metrics: '$(info)'"
	stop
else
	bad "3. did not start under PERFCACHED_URING_DENY=1"
fi
conf ex "io = io_uring"
if start ex PERFCACHED_URING_DENY=1; then
	grep -q "WARNING: io = io_uring, but this host cannot run it - the workers use epoll: refused by" "$D/ex.log" &&
		ok "3. a configured io_uring that falls back is a WARNING" ||
		bad "3. start: $(grep 'io' "$D/ex.log" | head -2)"
	[ "$(backend 'b["active"]')" = epoll ] && ok "3. and runs epoll" || bad "3. active $(backend 'b.get("active")')"
	stop
else
	bad "3. io = io_uring under PERFCACHED_URING_DENY=1 did not start (it must fall back, not fail)"
fi
PERFCACHED_URING_DENY=1 "$BIN" -C -f "$D/ex.conf" 2>/dev/null | grep -q "^io: epoll - io_uring is not usable on this host: refused by" &&
	ok "3. -C reports the refusal" || bad "3. -C: $(PERFCACHED_URING_DENY=1 "$BIN" -C -f "$D/ex.conf" 2>&1 | grep io)"

# ---- 4. the value ------------------------------------------------------------------
conf bad "io = kqueue"
"$BIN" -C -f "$D/bad.conf" > "$D/bad.out" 2>&1
[ $? -ne 0 ] && grep -q "io: io_uring | epoll, not 'kqueue'" "$D/bad.out" && ok "4. io = kqueue is refused" ||
	bad "4. io = kqueue: $(cat "$D/bad.out" | tail -2)"
"$BIN" -E -f "$D/ep.conf" 2>/dev/null | grep -qx "io = epoll" && ok "4. -E dumps io = epoll" || bad "4. -E: no io = epoll"

e=$(cat "$D"/*.log | grep -cE ' (ERROR|CRIT)')
[ "$e" = 0 ] && ok "no ERROR/CRIT" || bad "$e ERROR/CRIT: $(grep -hE ' (ERROR|CRIT)' "$D"/*.log | head -2)"
echo "iouringtest: $pass passed, $fail failed"
[ $fail -eq 0 ]
