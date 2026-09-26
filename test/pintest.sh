#!/bin/sh
# pintest.sh - S167: [memory] pin and huge_pages.
#
# Both knobs exist because the daemon used to decide alone and only
# report in prose.  Pinning is attempted on every start and degrades to
# a WARNING, so a node can believe it is resident while it is swappable
# - and it is RLIMIT_MEMLOCK that refuses, which in an unprivileged
# container is the CONTAINER's limit, not the unit's.  The page tier was
# probed with no override, so "plain 4K" could not be asked for; 4K
# matters because growing the arena then costs many cheap faults instead
# of one 2 MB fault that may wait for compaction (S166).
#
# The daemon runs as an unprivileged uid for the failing cases: root
# bypasses RLIMIT_MEMLOCK through CAP_IPC_LOCK, so `ulimit -l 0` alone
# would prove nothing (a trap this suite paid for once already).
# Usage: test/pintest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcpin.XXXXXX)
chmod 755 "$D"
PIDS=""
trap 'for p in $PIDS; do kill -9 "$p" 2>/dev/null; done; rm -rf "$D"' EXIT TERM INT
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

# start as an unprivileged uid with no memlock allowance, or as we are
start() { # start <conf> <log> <unpriv 0|1>
	if [ "$3" = 1 ] && [ "$(id -u)" = 0 ]; then
		setpriv --reuid=65534 --regid=65534 --clear-groups \
			sh -c "ulimit -l 0; exec $BIN -f $1" > "$2" 2>&1 &
	else
		sh -c "ulimit -l 0 2>/dev/null; exec $BIN -f $1" > "$2" 2>&1 &
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
grep -q "pin: auto|require" "$D/bad.log" \
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
#
# The PREMISE is that an unprivileged uid under `ulimit -l 0` cannot
# lock 32 MB.  A host that grants the lock anyway - a container with
# CAP_IPC_LOCK reaching the child, or a kernel that does not charge the
# tier this arena landed on - cannot test the refusal at all, and the
# GitLab runner is such a host (rc14, check and check-asan).  Detect it
# and SKIP loudly: a skip is not a pass, and pretending otherwise would
# make this suite green for the wrong reason.
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

# ---- 4. pin = auto keeps the old behaviour: a warning, and it runs ----
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
if [ "$(id -u)" = 0 ]; then
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

echo "pintest: $pass passed, $fail failed"
[ $fail -eq 0 ]
