/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * storage.h — the storage identity walk (task S11).
 *
 * Resolves a directory to its mount, filesystem, and the FULL device
 * chain underneath (dm/LVM/md recursed to leaf devices), classifying
 * each leaf's transport from sysfs - never from the device NAME (the
 * NVMe-oF trap: /dev/nvmeXnY may be TCP fabric).  Identity is a LABEL;
 * policy always follows measurement (S12's probe).  Failure semantics
 * aggregate as WORST LEAF WINS: any network leaf makes the whole stack
 * network-class (stalls are hangs, not errors).
 *
 * Pure /proc + /sys reads; every field is best-effort and reported as
 * absent when the kernel does not expose it - nothing is guessed.
 */
#ifndef PC_STORAGE_H
#define PC_STORAGE_H

#include <stddef.h>

/* failure-semantics classes, best to worst (aggregation picks the max) */
enum pc_st_class {
	PC_ST_LOCAL = 0,               /* local block device */
	PC_ST_VM_OPAQUE,               /* virtio etc: backing unknown (VM) */
	PC_ST_NETWORK,                 /* fabric/network: stalls hang */
	PC_ST_MEMORY,                  /* tmpfs: crash-safe only, NOT power-safe */
};

#define PC_ST_MAXLEAF 16

struct pc_st_leaf {
	char name[32];                 /* kernel device name */
	char kind[24];                 /* nvme-pcie nvme-tcp sata sas virtio.. */
	char rate[24];                 /* negotiated link rate, "" = unknown */
	char extra[64];                /* backing file, subsystem, ctrl count */
	int  rotational;               /* -1 unknown */
	enum pc_st_class cls;
};

struct pc_st_id {
	char mount[256];
	char fstype[32];
	char chain[512];               /* human chain: dm-N(lvm) -> sda ... */
	struct pc_st_leaf leaf[PC_ST_MAXLEAF];
	int  nleaf;
	enum pc_st_class cls;          /* the worst leaf (or fs class) */
	int  is_tmpfs, is_nfs;
};

/* resolve @dir; returns 0 (id filled) or -1 (dir unusable).  Chain
 * resolution deeper than the mount is best-effort: a walk failure
 * degrades to zero leaves with the fs-level class kept. */
int pc_storage_identity(const char *dir, struct pc_st_id *id);

/* multi-line human report into @buf; returns bytes written */
int pc_storage_format(const struct pc_st_id *id, char *buf, size_t cap);

const char *pc_st_class_str(enum pc_st_class c);

/* the WAL dir's identity as resolved at startup (daemon.c), for the
 * stats verb; NULL when the daemon runs without a WAL */
const struct pc_st_id *pc_wal_identity(void);

#endif /* PC_STORAGE_H */
