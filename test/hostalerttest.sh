#!/bin/sh
# hostalerttest.sh - S380 (0.6.0.1): alarms when the host's CPU, the state
# device or the host's memory run high, and the arena at the same
# thresholds; S381: /stats process.cpus.  With PERFCACHED_ALERT_FAST=60
# (every hold and clear time, and the one-minute window, divided by 60).
#  1. [alerts]: -E dumps the defaults (warning 65, critical 75); a warning
#     not below its critical, a value outside 1-100 and an unknown key are
#     config errors
#  2. /stats alerts.host: CPU busy and perfcached's share, the CPUs, memory
#     used, the watched disk or why it is not, the thresholds;
#     /stats process.cpus is what nproc says (no quota on a test host) and
#     /metrics carries perfcached_process_cpus
#  3. thresholds at 1/2 %: host memory is critical at once; the arena after
#     ~2 MB stored; CPU under a one-core busy loop; the disk under a write
#     and fsync loop (SKIPPED, loudly, where the host gives no statistics
#     for the device)
# Fail-first: 0.6.0 refuses [alerts] as an unknown section.
# Usage: test/hostalerttest.sh ./perfcached
set -u
BIN=${1:-./perfcached}
case "$BIN" in /*) ;; *) BIN=$(pwd)/$BIN;; esac
D=$(mktemp -d /var/tmp/pchostal.XXXXXX)
P= L=
trap '[ -n "$P" ] && kill -9 $P 2>/dev/null; [ -n "$L" ] && kill $L 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
export PERFCACHED_ALERT_FAST=60
for p in 19751 19752 19753; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "hostalerttest: port $p already bound" >&2; exit 1; }
done

conf() { # conf <name> [alerts lines...]
	n=$1; shift
	mkdir -p "$D/s"
	{
		cat <<EOF
[daemon]
workers = 2
state_dir = $D/s
[memory]
arena_mb = 32
[secrets]
client = hostal-client-secret
[listen]
tcp = 127.0.0.1:19751
resp = 127.0.0.1:19752
http = 127.0.0.1:19753
plaintext = loopback
[collection 0]
buckets_log2 = 10
EOF
		if [ $# -gt 0 ]; then
			echo "[alerts]"
			for l in "$@"; do echo "$l"; done
		fi
	} > "$D/$n.conf"
	chmod 600 "$D/$n.conf"
}
st() { # st <python expr over s> - from /stats
	python3 -c 'import json,sys,urllib.request
s=json.loads(urllib.request.urlopen("http://127.0.0.1:19753/stats", timeout=5).read())
print(eval(sys.argv[1]))' "$1" 2>/dev/null
}
start() {
	rm -rf "$D/s"; mkdir -p "$D/s"
	"$BIN" -f "$D/$1.conf" > "$D/$1.log" 2>&1 & P=$!
	i=0; while [ $i -lt 100 ] && ! grep -q "perfcached ready" "$D/$1.log"; do sleep 0.1; i=$((i+1)); done
	grep -q "perfcached ready" "$D/$1.log"
}
stop() { kill $P 2>/dev/null; wait $P 2>/dev/null; P=; }
active() { # active <key> - its level, or "none"
	st 'next((a["level"] for a in s["alerts"]["active"] if a["key"] == "'"$1"'"), "none")'
}
until_active() { # until_active <key> <secs> - waits, prints the level
	n=0; while [ $n -lt $(($2 * 4)) ]; do
		v=$(active "$1"); [ "$v" != none ] && [ -n "$v" ] && { echo "$v"; return 0; }
		sleep 0.25; n=$((n+1))
	done
	echo none; return 1
}

# ---- 1. [alerts] -------------------------------------------------------------------
conf dflt
"$BIN" -E -f "$D/dflt.conf" 2>/dev/null > "$D/dflt.dump"
grep -q '^\[alerts\]' "$D/dflt.dump" && grep -qx "cpu_warn_pct = 65" "$D/dflt.dump" &&
	grep -qx "cpu_crit_pct = 75" "$D/dflt.dump" && grep -qx "arena_crit_pct = 75" "$D/dflt.dump" &&
	ok "1. -E: [alerts] with the defaults 65 / 75" || bad "1. -E: $(grep -A8 'alerts' "$D/dflt.dump" | tr '\n' ' ')"
conf inv "cpu_warn_pct = 80" "cpu_crit_pct = 70"
"$BIN" -C -f "$D/inv.conf" > "$D/inv.out" 2>&1
[ $? -ne 0 ] && grep -q "cpu_warn_pct must be below cpu_crit_pct" "$D/inv.out" &&
	ok "1. a warning not below its critical is refused" || bad "1. warn >= crit: $(tail -2 "$D/inv.out")"
conf rng "io_warn_pct = 0"
"$BIN" -C -f "$D/rng.conf" > "$D/rng.out" 2>&1
[ $? -ne 0 ] && ok "1. io_warn_pct = 0 is refused" || bad "1. io_warn_pct = 0 accepted"
conf unk "load_warn_pct = 5"
"$BIN" -C -f "$D/unk.conf" > "$D/unk.out" 2>&1
[ $? -ne 0 ] && grep -q "unknown key 'load_warn_pct'" "$D/unk.out" && ok "1. an unknown [alerts] key is refused" ||
	bad "1. unknown key: $(tail -2 "$D/unk.out")"

# ---- 2. what is measured -------------------------------------------------------------
if start dflt; then
	sleep 3
	h=$(st 's["alerts"]["host"]')
	echo "    host: $h"
	[ "$(st '0 <= s["alerts"]["host"]["memory_used_pct"] <= 100')" = True ] &&
		ok "2. memory_used_pct $(st 's["alerts"]["host"]["memory_used_pct"]')" || bad "2. memory_used_pct: $h"
	[ "$(st 's["alerts"]["host"]["cpu_busy_pct"] is not None and s["alerts"]["host"]["cpus"] > 0')" = True ] &&
		ok "2. cpu_busy_pct $(st 's["alerts"]["host"]["cpu_busy_pct"]') of $(st 's["alerts"]["host"]["cpus"]') CPUs" ||
		bad "2. cpu figures: $h"
	[ "$(st '(s["alerts"]["host"]["disk"] != "") != (s["alerts"]["host"]["disk_unwatched"] != "")')" = True ] &&
		ok "2. the disk: '$(st 's["alerts"]["host"]["disk"] or s["alerts"]["host"]["disk_unwatched"]')'" ||
		bad "2. disk neither watched nor explained: $h"
	[ "$(st 's["alerts"]["host"]["thresholds"]["memory_crit_pct"]')" = 75 ] && ok "2. thresholds shown" ||
		bad "2. thresholds: $h"
	nc=$(nproc 2>/dev/null || echo 0)
	[ "$(st 's["process"]["cpus"]')" = "$nc" ] && ok "2. process.cpus $nc = nproc" ||
		bad "2. process.cpus '$(st 's["process"].get("cpus")')' vs nproc $nc"
	python3 -c 'import urllib.request,sys
m=urllib.request.urlopen("http://127.0.0.1:19753/metrics", timeout=5).read().decode()
sys.exit(0 if any(l.startswith("perfcached_process_cpus ") for l in m.splitlines()) else 1)' &&
		ok "2. /metrics perfcached_process_cpus" || bad "2. no perfcached_process_cpus in /metrics"
	# memory only: a CI runner's other jobs may well hold its CPU over 65%
	[ "$(active host.memory)" = none ] && ok "2. at the defaults host memory is not raised" ||
		bad "2. raised at the defaults: $(st 's["alerts"]["active"]')"
	stop
else
	bad "2. did not start: $(tail -3 "$D/dflt.log")"
fi

# ---- 3. raised ----------------------------------------------------------------------
conf low "cpu_warn_pct = 1" "cpu_crit_pct = 2" "io_warn_pct = 1" "io_crit_pct = 2" \
	"memory_warn_pct = 1" "memory_crit_pct = 2" "arena_warn_pct = 1" "arena_crit_pct = 2"
if start low; then
	v=$(until_active host.memory 6)
	[ "$v" = critical ] && st 'next(a["text"] for a in s["alerts"]["active"] if a["key"] == "host.memory")' |
		grep -q "critical at 2%" && ok "3. host memory: critical, its text names the threshold" ||
		bad "3. host.memory: '$v' $(st 's["alerts"]["active"]')"
	python3 - <<'PY' 2>/dev/null
import socket
s = socket.create_connection(("127.0.0.1", 19752)); v = b"x" * 8000
for i in range(300):
    k = b"k%d" % i
    s.sendall(b"*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n" % (len(k), k, len(v), v)); s.recv(64)
PY
	v=$(until_active memory.arena 8)
	[ "$v" = critical ] && ok "3. arena: critical after ~2.4 MB stored ($(st 'next(a["text"] for a in s["alerts"]["active"] if a["key"] == "memory.arena")'))" ||
		bad "3. memory.arena: '$v' $(st 's["alerts"]["host"]') $(st 's["alerts"]["active"]')"
	timeout 15 sh -c 'while :; do :; done' & L=$!
	v=$(until_active host.cpu 14)
	kill $L 2>/dev/null; wait $L 2>/dev/null; L=
	[ "$v" = critical ] && ok "3. CPU: critical under a one-core busy loop ($(st 'next(a["text"] for a in s["alerts"]["active"] if a["key"] == "host.cpu")'))" ||
		bad "3. host.cpu: '$v' $(st 's["alerts"]["host"]')"
	if [ -n "$(st 's["alerts"]["host"]["disk"]')" ]; then
		python3 - "$D/s/ddload" <<'PY' &
import os, sys, time
fd = os.open(sys.argv[1], os.O_WRONLY | os.O_CREAT, 0o600); b = b"x" * 65536; end = time.time() + 14
while time.time() < end:
    os.write(fd, b); os.fsync(fd)
os.close(fd)
PY
		L=$!
		v=$(until_active host.io 14)
		kill $L 2>/dev/null; wait $L 2>/dev/null; L=
		[ "$v" = critical ] || [ "$v" = warning ] &&
			ok "3. disk: $v under a write and fsync loop ($(st 'next(a["text"] for a in s["alerts"]["active"] if a["key"] == "host.io")'))" ||
			bad "3. host.io: '$v' $(st 's["alerts"]["host"]')"
	else
		echo "  skip 3. disk: not watched here - $(st 's["alerts"]["host"]["disk_unwatched"]')"
	fi
	grep -q "alert.*host.memory\|alert:.*host memory" "$D/low.log" && ok "3. the raise is logged" ||
		bad "3. no host memory alert in the log: $(grep -i alert "$D/low.log" | head -3)"
	stop
else
	bad "3. did not start: $(tail -3 "$D/low.log")"
fi

# a raised critical alert is logged CRIT by design (alerts.c) - anything else is not
e=$(cat "$D"/*.log | grep -E ' (ERROR|CRIT)' | grep -vc ' alert: ')
[ "$e" = 0 ] && ok "no ERROR/CRIT besides the alerts raised" || bad "$e ERROR/CRIT: $(grep -hE ' (ERROR|CRIT)' "$D"/*.log | grep -v ' alert: ' | head -2)"
echo "hostalerttest: $pass passed, $fail failed"
[ $fail -eq 0 ]
