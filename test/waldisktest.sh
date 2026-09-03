#!/bin/sh
# waldisktest.sh — a WAL that does not fit must be refused, not attempted.
#
# Segments are provisioned with REAL zero writes (fallocate does not
# commit thin/sparse/COW backends), so without a pre-flight check the
# check IS the write: the daemon fills the filesystem and only then
# discovers it cannot finish - and, before this, left the partial
# segment behind, so a refused start took the whole volume down with it
# and every supervisor retry re-filled whatever had been freed.
# Usage: test/waldisktest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d /var/tmp/pcdisk.XXXXXX)
MNT="$D/mnt"
LOOP=
cleanup() {
	umount "$MNT" 2>/dev/null
	[ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
	rm -rf "$D"
}
trap cleanup EXIT
pass=0 fail=0
ok()  { pass=$((pass+1)); echo "  ok   $1"; }
bad() { fail=$((fail+1)); echo "  FAIL $1"; }

echo "=== waldisktest ==="
# A small filesystem, built for the test.  Gate on the fixture BUILDING,
# not on the tools existing: a container without loop devices must SKIP
# loudly, never pass silently.
if ! command -v losetup >/dev/null || ! command -v mkfs.ext4 >/dev/null; then
	echo "  SKIP no losetup/mkfs.ext4 (fixture not buildable - NOT a pass)"
	echo "waldisktest: 0 passed, 0 failed"; exit 0
fi
mkdir -p "$MNT"
dd if=/dev/zero of="$D/img" bs=1M count=64 2>/dev/null
LOOP=$(losetup -f --show "$D/img" 2>/dev/null) || LOOP=
if [ -z "$LOOP" ] || ! mkfs.ext4 -q -F "$LOOP" 2>/dev/null || \
   ! mount "$LOOP" "$MNT" 2>/dev/null; then
	echo "  SKIP could not build the loop fixture (NOT a pass)"
	echo "waldisktest: 0 passed, 0 failed"; exit 0
fi
mkdir -p "$MNT/wal"
FREE0=$(df -k "$MNT" | tail -1 | awk '{print $4}')

cat > "$D/c.conf" <<CONF
[daemon]
workers = 2
[memory]
arena_mb = 64
[secrets]
client = disk-client-secret
cluster = disk-cluster-secret
[listen]
tcp = 127.0.0.1:16499
plaintext = loopback
[wal]
dir = $MNT/wal
probe = no
fsync = everysec
# 4 x 64 MB = 256 MB wanted on a 64 MB filesystem
segment_mb = 64
segments = 4
save = off
[collection th]
buckets_log2 = 10
CONF

"$BIN" -f "$D/c.conf" > "$D/log" 2>&1
RC=$?
[ "$RC" -ne 0 ] && ok "refused to start (exit $RC)" \
	|| bad "started anyway on a filesystem that cannot hold the WAL"
grep -q "Refusing to start rather than filling" "$D/log" \
	&& ok "said why, before writing anything" \
	|| bad "no pre-flight refusal in the log: $(tail -2 "$D/log")"
grep -qE "needs [0-9]+ MB" "$D/log" && ok "named the requirement" \
	|| bad "the refusal cites no size"

# the point of the exercise: the volume must be untouched
FREE1=$(df -k "$MNT" | tail -1 | awk '{print $4}')
LOST=$((FREE0 - FREE1))
[ "$LOST" -lt 1024 ] && ok "left the filesystem alone (${LOST}KB consumed)" \
	|| bad "consumed ${LOST}KB before giving up - a refused start must cost nothing"
[ -z "$(ls "$MNT/wal"/*.seg 2>/dev/null)" ] && ok "no partial segment left behind" \
	|| bad "a partial segment survived: $(ls -la "$MNT/wal"/*.seg | head -1)"

# GREEN: the same daemon starts when the WAL does fit
sed -i 's/^segment_mb = 64$/segment_mb = 4/;s/^segments = 4$/segments = 2/' "$D/c.conf"
"$BIN" -f "$D/c.conf" > "$D/log2" 2>&1 &
P=$!
i=0; while [ $i -lt 300 ]; do grep -q "perfcached ready" "$D/log2" && break
	sleep 0.1; i=$((i+1)); done
grep -q "perfcached ready" "$D/log2" && ok "starts when the WAL fits (2 x 4 MB)" \
	|| bad "refused a WAL that fits: $(tail -2 "$D/log2")"
kill -TERM $P 2>/dev/null; wait $P 2>/dev/null

echo "waldisktest: $pass passed, $fail failed"
[ $fail -eq 0 ]
