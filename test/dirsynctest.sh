#!/bin/sh
# dirsynctest.sh - a file made durable is found after a power loss only once its directory is synced
# too: the WAL's provisioned segments and the runtime collections file each sync their directory.
# Usage: test/dirsynctest.sh [./perfcached] [./syncfailshim.so] [./perfcli]
set -u
BIN=${1:-./perfcached}
SHIM=${2:-./syncfailshim.so}
CLI=${3:-./perfcli}
case $SHIM in /*) ;; *) SHIM=$(pwd)/$SHIM ;; esac
[ -f "$SHIM" ] || { echo "dirsynctest: $SHIM is missing (make syncfailshim.so)"; exit 1; }
[ -x "$CLI" ] || { echo "dirsynctest: $CLI is missing (make perfcli)"; exit 1; }
SANRT=$(ldd "$BIN" 2>/dev/null | awk '/libasan|libclang_rt\.asan/ { print $3; exit }')
D=$(mktemp -d /var/tmp/pcdsy.XXXXXX)
PID=
trap '[ -n "$PID" ] && kill -9 $PID 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }
ss -ltn 2>/dev/null | grep -q ":16497[[:space:]]" && { echo "dirsynctest: port 16497 busy" >&2; exit 1; }
R=$(cd "$D" && pwd -P)                 # the shim notes the directory's real path
mkdir -p "$D/wal" "$D/state"
cat > "$D/n.conf" <<EOF
[daemon]
workers = 2
log_level = info
state_dir = $D/state
[memory]
arena_mb = 64
[secrets]
client = dsy-client-secret
enable = dsy-enable-secret
[listen]
tcp = 127.0.0.1:16497
[collection c]
buckets_log2 = 10
[wal]
dir = $D/wal
probe = no
fsync = everysec
segment_mb = 8
segments = 4
save = off
EOF
chmod 600 "$D/n.conf"
echo dirlog > "$D/ctl"
: > "$D/sync.log" ; : > "$D/n.log"
LD_PRELOAD="${SANRT:+$SANRT }$SHIM${PC_NOSYNC_LIB:+ $PC_NOSYNC_LIB}" PC_SYNCFAIL_CTL="$D/ctl" \
	PC_SYNCFAIL_LOG="$D/sync.log" "$BIN" -f "$D/n.conf" > "$D/n.log" 2>&1 &
PID=$!
i=0; while [ $i -lt 200 ] && ! grep -q "perfcached ready" "$D/n.log"; do
	kill -0 $PID 2>/dev/null || break; sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/n.log" || { bad "the node did not start: $(tail -3 "$D/n.log" | tr '\n' ' ')"
	echo "dirsynctest: $pass passed, $fail failed"; exit 1; }

# 1. a first start provisions four segments: their directory is synced
grep -q "provisioning segment" "$D/n.log" && ok "the start provisioned its segments" || bad "no segment provisioned"
grep -qx "dirlog $R/wal" "$D/sync.log" && ok "the WAL directory was synced after its segments were made" \
	|| bad "no sync of $R/wal: $(tr '\n' ' ' < "$D/sync.log" | cut -c1-200)"

# 2. a collection made at runtime: the collections file's directory is synced
: > "$D/sync.log"
o=$("$CLI" -h 127.0.0.1 -p 16497 -a dsy-client-secret -E dsy-enable-secret -q create c2 2>&1)
i=0; while [ $i -lt 50 ] && ! [ -s "$D/state/collections" ]; do sleep 0.1; i=$((i+1)); done
[ -s "$D/state/collections" ] && grep -q "collection c2" "$D/state/collections" \
	&& ok "create c2 wrote the collections file" || bad "create c2: $o; collections: $(cat "$D/state/collections" 2>/dev/null)"
i=0; while [ $i -lt 30 ] && ! grep -qx "dirlog $R/state" "$D/sync.log"; do sleep 0.1; i=$((i+1)); done
grep -qx "dirlog $R/state" "$D/sync.log" && ok "its directory was synced after the rename" \
	|| bad "no sync of $R/state: $(tr '\n' ' ' < "$D/sync.log" | cut -c1-200)"
kill $PID 2>/dev/null; wait $PID 2>/dev/null; PID=
echo "dirsynctest: $pass passed, $fail failed"
[ $fail -eq 0 ]
