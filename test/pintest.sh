#!/bin/sh
# pintest.sh - [memory] pin and huge_pages: what cannot be honoured is refused, 4K can be asked for, and
# pin = no leaves the arena unlocked so give-back works.
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcpin.XXXXXX)
chmod 755 "$D"
PIDS=""
trap 'for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM trap that returns lets the script run on
pass=0 fail=0 PINSKIP=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

conf() { # conf <file> <extra memory lines>
	cat > "$1" <<EOF
[daemon]
workers = 2
log_level = notice
[memory]
arena_mb = 32
$2
[secrets]
client = pin-client-secret
[listen]
resp = 127.0.0.1:17692
http = 127.0.0.1:17693
plaintext = loopback
[collection 0]
buckets_log2 = 12
EOF
	chmod 644 "$1"
}

# start as an unprivileged uid with no memlock allowance (root bypasses RLIMIT_MEMLOCK through
# CAP_IPC_LOCK), or as we are
start() { # start <conf> <log> <unpriv 0|1>
	# emptied HERE, before the loop reads it: a `> log &` empties it in the child, so the loop could
	# read the previous start's refusal first
	: > "$2"
	if [ "$3" = 1 ] && [ "$(id -u)" = 0 ]; then
		setpriv --reuid=65534 --regid=65534 --clear-groups \
			sh -c "ulimit -l 0; exec $BIN -f $1" >> "$2" 2>&1 &
	else
		sh -c "ulimit -l 0 2>/dev/null; exec $BIN -f $1" >> "$2" 2>&1 &
	fi
	PIDS="$PIDS $!"
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$2" 2>/dev/null && return 0
		grep -q "not starting\|pin = require" "$2" 2>/dev/null && return 1
		sleep 0.1; i=$((i+1))
	done
	return 1
}

stats() { # stats <python expr over the memory block>
	python3 - "$1" <<'PYEOF'
import json, sys, urllib.request
m = json.loads(urllib.request.urlopen("http://127.0.0.1:17693/stats", timeout=10).read())["memory"]
print(eval(sys.argv[1], {}, {"m": m}))
PYEOF
}

# ---- 1. the config refuses what it cannot honour ---------------------
conf "$D/bad.conf" "pin = sometimes"
"$BIN" -f "$D/bad.conf" -C > "$D/bad.log" 2>&1
grep -q "pin: auto|require|no" "$D/bad.log" \
	&& ok "an unknown pin value is refused, naming the choices" \
	|| bad "pin = sometimes was not refused: $(tail -1 "$D/bad.log")"
conf "$D/bad2.conf" "huge_pages = maybe"
"$BIN" -f "$D/bad2.conf" -C > "$D/bad2.log" 2>&1
grep -q "huge_pages: auto|off" "$D/bad2.log" \
	&& ok "an unknown huge_pages value is refused, naming the choices" \
	|| bad "huge_pages = maybe was not refused: $(tail -1 "$D/bad2.log")"
conf "$D/bad3.conf" "pin = require
backing = heap"
"$BIN" -f "$D/bad3.conf" -C > "$D/bad3.log" 2>&1
grep -q "pin = require needs backing = own" "$D/bad3.log" \
	&& ok "pin = require with a heap-backed daemon is refused at parse time" \
	|| bad "pin = require + backing = heap was accepted: $(tail -1 "$D/bad3.log")"

# ---- 2. huge_pages = off gives the 4K tier ---------------------------
conf "$D/k4.conf" "huge_pages = off"
if start "$D/k4.conf" "$D/k4.log" 0; then
	T=$(stats 'm["tier"]')
	case "$T" in
		*"4K"*) ok "huge_pages = off: the arena is on plain 4K pages ($T)";;
		*) bad "huge_pages = off gave tier: $T";;
	esac
	grep -q "MADV_HUGEPAGE\|MAP_HUGETLB" "$D/k4.log" \
		&& bad "the log still names a huge-page tier" \
		|| ok "and no huge-page tier is claimed in the log"
else
	bad "the daemon did not start with huge_pages = off: $(tail -2 "$D/k4.log")"
fi
for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; PIDS=""; sleep 0.3

# ---- 3. pin = require refuses a start it cannot honour ---------------
# a host that grants an unprivileged uid the lock anyway (CAP_IPC_LOCK reaching it) SKIPS loudly
conf "$D/req.conf" "pin = require"
if start "$D/req.conf" "$D/req.log" 1 && [ "$(stats 'm["locked_bytes"]')" != 0 ]; then
	echo "  ..   SKIP the refusal half: this host locked the arena for an"
	echo "  ..        unprivileged uid under 'ulimit -l 0', so there is no"
	echo "  ..        refusal to observe (locked_bytes $(stats 'm["locked_bytes"]'))"
	PINSKIP=1
elif [ "${PINSKIP:-0}" = 0 ] && start "$D/req.conf" "$D/req.log" 1; then
	bad "pin = require started although the arena could not be locked"
else
	grep -q "pin = require, but the arena is not locked" "$D/req.log" \
		&& ok "pin = require refuses the start when the arena cannot be locked" \
		|| bad "it did not start, but not for the pin reason: $(tail -2 "$D/req.log")"
	grep -q "RLIMIT_MEMLOCK" "$D/req.log" \
		&& ok "and the refusal names the limit that refused it" \
		|| bad "the refusal does not name RLIMIT_MEMLOCK"
fi
for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; PIDS=""; sleep 0.3

# ---- 4. pin = auto: a warning, and it runs ----------------------------
conf "$D/auto.conf" "pin = auto"
if [ "${PINSKIP:-0}" = 1 ]; then
	echo "  ..   SKIP the unpinned half for the same reason"
elif start "$D/auto.conf" "$D/auto.log" 1; then
	ok "pin = auto runs unpinned, as it always did"
	grep -q "continuing unpinned" "$D/auto.log" \
		&& ok "and says so" \
		|| bad "no unpinned warning in the log"
	L=$(stats 'm["locked_bytes"]')
	[ "$L" = 0 ] \
		&& ok "/stats reports locked_bytes 0, so the state is visible" \
		|| bad "locked_bytes is $L on an unpinned arena"
else
	bad "pin = auto did not start: $(tail -2 "$D/auto.log")"
fi
for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; PIDS=""; sleep 0.3

# ---- 5. where locking works, it is reported ---------------------------
# root is not enough: a container drops CAP_IPC_LOCK and a rootless engine caps RLIMIT_MEMLOCK
can_lock() {
	cap=$(awk '/^CapEff:/ {print $2}' /proc/$$/status 2>/dev/null)
	[ -n "$cap" ] && [ $(( (0x$cap >> 14) & 1 )) = 1 ] && return 0  # CAP_IPC_LOCK
	[ "$(ulimit -l)" = unlimited ]
}
if [ "$(id -u)" = 0 ] && ! can_lock; then
	echo "  ..   root without CAP_IPC_LOCK and memlock $(ulimit -l) KB (a container's limit): the locking half is skipped"
elif [ "$(id -u)" = 0 ]; then
	conf "$D/lock.conf" "pin = require"
	"$BIN" -f "$D/lock.conf" > "$D/lock.log" 2>&1 &
	PIDS="$PIDS $!"
	i=0
	while [ $i -lt 100 ]; do
		grep -q "perfcached ready" "$D/lock.log" && break
		sleep 0.1; i=$((i+1))
	done
	if grep -q "perfcached ready" "$D/lock.log"; then
		L=$(stats 'm["locked_bytes"]')
		[ "$L" -ge 33554432 ] \
			&& ok "a locked arena reports locked_bytes ($L)" \
			|| bad "the arena started pinned but locked_bytes is $L"
		grep -q "locked in RAM (pin = require)" "$D/lock.log" \
			&& ok "and the start says how much is locked" \
			|| bad "no locked-in-RAM notice"
		P=$(stats 'm["pin_lost"]')
		[ "$P" = 0 ] && ok "pin_lost is 0 while every group is locked" \
			|| bad "pin_lost is $P on a freshly pinned arena"
	else
		bad "pin = require did not start as root: $(tail -2 "$D/lock.log")"
	fi
else
	echo "  ..   not root: the locking half is skipped"
fi

# ---- 6. pin = no: never locked, as root too, so idle groups can go back -----
for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; PIDS=""; sleep 0.3     # 5's node holds the port
conf "$D/no.conf" "pin = no
huge_pages = off"
if start "$D/no.conf" "$D/no.log" 0; then
	L=$(stats 'm["locked_bytes"]'); G=$(stats 'm["reclaim"]["giveback_off"]')
	[ "$L" = 0 ] && [ "$G" = False ] && grep -q "pin = no: the arena is not locked" "$D/no.log" \
		&& ok "pin = no: nothing locked (locked_bytes 0), give-back on, and the start says so" \
		|| bad "pin = no: locked_bytes $L, giveback_off $G; $(grep -i 'pin' "$D/no.log" | tail -1)"
else
	bad "pin = no did not start: $(tail -2 "$D/no.log")"
fi
for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; PIDS=""; sleep 0.3

echo "pintest: $pass passed, $fail failed"
[ $fail -eq 0 ]
