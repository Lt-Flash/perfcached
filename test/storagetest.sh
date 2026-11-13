#!/bin/sh
# storagetest.sh - the storage identity walk on real shapes: tmpfs (memory, not power-safe), the
# host's filesystems, and a dm-linear over loop chain built for the test (needs root).
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d)
LOOP=""
DM=""
trap '[ -n "$DM" ] && dmsetup remove "$DM" 2>/dev/null; \
     [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null; rm -rf "$D"' EXIT
trap 'exit 1' INT TERM   # a TERM with no trap kills the script before the EXIT trap runs
pass=0 fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }

# tmpfs: memory class + the explicit warning
mkdir -p /dev/shm/pcst.$$
R=$("$BIN" -I /dev/shm/pcst.$$)
rmdir /dev/shm/pcst.$$
echo "$R" | grep -q "failure class memory" && ok || bad "tmpfs class: $R"
echo "$R" | grep -q "not host/power loss" && ok || bad "tmpfs warning: $R"

# the current directory's real mount resolves with a chain or leaves
R=$("$BIN" -I /)
echo "--- identity of / ---"; echo "$R"
echo "$R" | grep -q "storage: mount" && ok || bad "mount line"
# a container's / is an overlay with no block device under it: say so and skip the two leaf
# checks rather than fail them
ROOTFS=$(awk '$2 == "/" { t = $3 } END { print t }' /proc/mounts)
if [ "$ROOTFS" = overlay ]; then
	echo "SKIP: / is an overlay (a container) - no block device leaf to" \
		"resolve; the leaf checks need a host root"
elif echo "$R" | grep -q "leaf"; then
	ok
else
	bad "no leaves resolved for /"
fi

# a VM host must be honest about opacity (virtio leaves, vm-opaque class)
if [ "$ROOTFS" != overlay ] && { [ -e /sys/block/vda ] ||
		ls /sys/block/sd* 2>/dev/null | head -1 | grep -q .; }; then
	if echo "$R" | grep -qE "virtio|vm-opaque|sata|sas|nvme|scsi"; then ok
	else bad "leaf kind missing: $R"; fi
fi

# RED: a nonexistent dir refuses
if "$BIN" -I /no/such/dir 2>/dev/null; then bad "nonexistent dir accepted"
else ok; fi

# built chain: dm-linear -> loop -> file, gated on the fixture BUILDING: an unbuildable fixture
# is a loud SKIP, never a FAIL, while one that identifies wrongly still fails
if command -v dmsetup >/dev/null && command -v losetup >/dev/null; then
	FIXTURE=ok
	dd if=/dev/zero of="$D/img" bs=1M count=64 2>/dev/null
	LOOP=$(losetup -f --show "$D/img") || FIXTURE=no
	if [ "$FIXTURE" = ok ]; then
		SECT=$(blockdev --getsz "$LOOP") || FIXTURE=no
	fi
	if [ "$FIXTURE" = ok ]; then
		DM=pcst$$
		dmsetup create "$DM" --table "0 $SECT linear $LOOP 0" || FIXTURE=no
	fi
	if [ "$FIXTURE" = ok ]; then
		mkfs.ext4 -q "/dev/mapper/$DM" || FIXTURE=no
	fi
	if [ "$FIXTURE" = ok ]; then
		mkdir -p "$D/mnt"
		mount "/dev/mapper/$DM" "$D/mnt" || FIXTURE=no
	fi
	if [ "$FIXTURE" = ok ]; then
		R=$("$BIN" -I "$D/mnt")
		echo "--- identity of the built dm->loop chain ---"; echo "$R"
		umount "$D/mnt"
		echo "$R" | grep -q "$DM(dm)" && ok || bad "dm hop missing: $R"
		echo "$R" | grep -q "loop.*on $D/img" && ok || bad "loop backing: $R"
		echo "$R" | grep -q "failure class local" && ok || bad "chain class: $R"
	else
		echo "SKIP: cannot build the dm->loop fixture here (no privilege" \
			"for loop/dm/mount) - built-chain checks not run"
	fi
else
	echo "notice: dmsetup/losetup unavailable - built-chain checks SKIPPED"
fi

echo "storagetest: $pass passed, $fail failed"
[ $fail -eq 0 ]
