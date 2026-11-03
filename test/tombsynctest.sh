#!/bin/sh
# tombsynctest.sh - 12ny AT-12 (c): a member confirms a delete only once its
# WAL has synced it - so the fleet forgets nothing a device lost.
#
# A member that applied a delete but could not make it durable must not
# confirm it: the origins would forget the delete, the member's next start
# would not have it, and nobody could give it back.  fsync = everysec;
# node 3 under test/syncfailshim.c, its WAL's syncs refused on demand (the
# first refusal FAILS the node - S215 - and it stays a member that cannot
# make anything durable):
#   1. node 3's WAL syncs refused, then 100 deletes through node 1: the
#      shim refused node 3's syncs, and node 1 keeps every one of the 100
#      for it - node 3 has confirmed none of them
#   2. node 3 restarted on a healthy device: it replays its WAL, confirms
#      the 100, node 1 forgets them all, and node 3 holds none of the keys
# FAIL-FIRST: confirming on apply, node 1 forgets the 100 in step 1 while
# node 3's WAL has synced none of them.
# Usage: test/tombsynctest.sh [./perfcached] [./syncfailshim.so]
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./syncfailshim.so}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
T=$(cd "$(dirname "$0")" && pwd)
PYTHONPATH=$T${PYTHONPATH:+:$PYTHONPATH}; export PYTHONPATH
SANRT=$(ldd "$BIN" 2>/dev/null | awk "/libasan|libclang_rt\\.asan/ { print \$3; exit }")
D=$(mktemp -d /var/tmp/pctsync.XXXXXX)
PIDS=""
trap 'for p in $PIDS; do grep -qa -- "$D" /proc/$p/cmdline 2>/dev/null && kill -9 $p; done; [ -n "${KEEP:-}" ] && echo "kept $D" || rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
[ -f "$SHIM" ] || { echo "tombsynctest: no sync-fail shim at $SHIM" >&2; exit 1; }
for p in 19581 19582 19583; do
	ss -ltn 2>/dev/null | grep -qE ":$p[[:space:]]" && { echo "tombsynctest: port $p already bound" >&2; exit 1; }
done
for i in 1 2 3; do
	mkdir -p "$D/w$i" "$D/s$i"
	cat > "$D/n$i.conf" <<C
[daemon]
workers = 2
log_level = notice
state_dir = $D/s$i
[memory]
arena_mb = 64
[secrets]
client = ty-client-secret
cluster = ty-cluster-secret
[listen]
tcp = 127.0.154.$i:1958$i
plaintext = loopback
[cluster]
multicast = 239.255.77.174:17294
advertise = 127.0.154.$i
mode = eager
collections = c
[wal]
dir = $D/w$i
segment_mb = 8
probe = no
fsync = everysec
save = off
[collection c]
buckets_log2 = 12
C
	chmod 600 "$D/n$i.conf"
done
: > "$D/ctl3"; : > "$D/shim3"
start() { # start <i> [shim]
	: > "$D/n$1.log"
	if [ -n "${2:-}" ]; then
		PC_SYNCFAIL_CTL="$D/ctl3" PC_SYNCFAIL_LOG="$D/shim3" LD_PRELOAD="${SANRT:+$SANRT }$SHIM" \
			"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	else
		"$BIN" -f "$D/n$1.conf" >> "$D/n$1.log" 2>&1 &
	fi
	PIDS="$PIDS $!"; eval "P$1=$!"
	k=0; while [ $k -lt 150 ] && ! grep -q "perfcached ready" "$D/n$1.log"; do sleep 0.1; k=$((k+1)); done
	grep -q "perfcached ready" "$D/n$1.log" || { bad "node $1 did not start: $(tail -2 "$D/n$1.log" | tr '\n' ' ')"; exit 1; }
}
st() { # st <i> <python expr over s = /stats, c = its cluster>
	python3 - "$1" "$2" <<'PY' 2>/dev/null
import socket, sys, pcnative
i = int(sys.argv[1])
f = pcnative.wrap(socket.create_connection(("127.0.154.%d" % i, 19580 + i), 10))
s = pcnative.call(f, "stats")["result"]; c = s["cluster"]
print(eval(sys.argv[2]))
PY
}
ops() { # ops <set|del> <from> <to>, through node 1
	python3 - "$@" <<'PY'
import socket, sys, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.154.1", 19581), 10))
for i in range(int(sys.argv[2]), int(sys.argv[3])):
    if sys.argv[1] == "set":
        pcnative.call(f, "set", col="c", key="y:%d" % i, value="v")
    else:
        pcnative.call(f, "del", col="c", key="y:%d" % i)
PY
}
held3() {
	python3 - <<'PY' 2>/dev/null
import socket, pcnative
f = pcnative.wrap(socket.create_connection(("127.0.154.3", 19583), 10))
print(sum(1 for i in range(100) if (pcnative.call(f, "get", col="c", key="y:%d" % i).get("result") or {}).get("found")))
PY
}
start 1; start 2; start 3 shim
k=0; while [ $k -lt 100 ] && { [ "$(st 1 'c.get("peers_up")')" != 2 ] || [ "$(st 1 'c["acks"]["fleet"]["confirming"]')" != True ]; }; do sleep 0.2; k=$((k+1)); done
python3 "$T/waitready.py" 127.0.154.1:19581 127.0.154.2:19582 127.0.154.3:19583 || bad "a node is not ready"
ops set 0 150
sleep 2

# 1. node 3's syncs refused; 100 deletes
echo wal > "$D/ctl3"
ops del 0 100
k=0; while [ $k -lt 50 ] && [ ! -s "$D/shim3" ]; do sleep 0.2; k=$((k+1)); done
sleep 5
REF=$(grep -c "^wal " "$D/shim3" 2>/dev/null)
N1=$(st 1 'c["tombstones_retained"]["numbered"]')
S3=$(st 3 's.get("state")')
[ "${REF:-0}" -gt 0 ] && [ "${N1:-0}" -ge 100 ] 2>/dev/null &&
	ok "1. node 3's WAL syncs refused ($REF, node 3 $S3): node 1 keeps all ${N1} deletes for it - nothing node 3 could not make durable was confirmed" ||
	bad "1. refused syncs ${REF:-0} (want more than 0 - else nothing below is a test), node 1 keeps ${N1:-?} numbered (want 100 or more); node 3 is ${S3:-?}"

# 2. node 3 on a healthy device: it confirms, node 1 forgets
: > "$D/ctl3"
eval "kill -9 \$P3"; sleep 1
start 3
k=0; while [ $k -lt 150 ] && [ "$(st 1 'c["tombstones_retained"]["numbered"]')" != 0 ]; do sleep 0.2; k=$((k+1)); done
N1=$(st 1 'c["tombstones_retained"]["numbered"]'); H3=$(held3)
[ "$N1" = 0 ] && [ "$H3" = 0 ] &&
	ok "2. node 3 restarted on a healthy device: node 1 forgot every delete, and node 3 holds none of the 100 keys" ||
	bad "2. after node 3's restart: node 1 keeps ${N1:-?} (want 0), node 3 holds ${H3:-?} of the 100 deleted keys (want 0)"

for i in 1 2; do
	grep -q "CRIT" "$D/n$i.log" &&
		bad "node $i logged CRIT: $(grep -m2 'CRIT' "$D/n$i.log" | tr '\n' ' ' | cut -c1-220)"
done
echo "tombsynctest: $pass passed, $fail failed"
[ $fail -eq 0 ]
