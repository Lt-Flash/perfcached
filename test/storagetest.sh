#!/bin/sh
# storagetest.sh — S11 verification: the identity walk against real
# shapes on the host: tmpfs (MEMORY + the not-power-safe WARNING), the
# host filesystems (on a VM: virtio leaves, vm-opaque class; btrfs's
# anonymous 0:xx device resolved through its source), and a chain BUILT
# for the test - dm-linear over a loop device - proving the recursive
# slaves walk end to end.  Needs root (loop/dm setup); dm checks are
# skipped with a notice when dmsetup is unavailable.
# Usage: test/storagetest.sh [./perfcached]
set -u

BIN=${1:-./perfcached}
D=$(mktemp -d)
LOOP=""
DM=""
trap '[ -n "$DM" ] && dmsetup remove "$DM" 2>/dev/null; \
     [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null; rm -rf "$D"' EXIT
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
echo "$R" | grep -q "leaf" && ok || bad "no leaves resolved for /"

# a VM host must be honest about opacity (224 is virtio - adjust if not)
if [ -e /sys/block/vda ] || ls /sys/block/sd* 2>/dev/null | head -1 \
		| grep -q .; then
	if echo "$R" | grep -qE "virtio|vm-opaque|sata|sas|nvme|scsi"; then ok
	else bad "leaf kind missing: $R"; fi
fi

# RED: a nonexistent dir refuses
if "$BIN" -I /no/such/dir 2>/dev/null; then bad "nonexistent dir accepted"
else ok; fi

# built chain: dm-linear -> loop -> file.  Gate on the fixture actually
# BUILDING, not on the tools existing: CI runners (containers, non-root
# shells) have losetup/dmsetup yet may not create loop or dm devices or
# mount - an unbuildable fixture is a SKIP said loudly, never a FAIL,
# while a built fixture that identifies wrongly still fails.
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
