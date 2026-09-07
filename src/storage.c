/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * storage.c — the storage identity walk (task S11).  See storage.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>

#include "compat/dprint.h"
#include "storage.h"

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif
#ifndef NFS_SUPER_MAGIC
#define NFS_SUPER_MAGIC 0x6969
#endif

/* ---- tiny sysfs readers ------------------------------------------------ */

static int read_line(const char *path, char *out, size_t cap)
{
	FILE *f = fopen(path, "r");
	size_t n;

	out[0] = 0;
	if (!f)
		return -1;
	if (!fgets(out, (int)cap, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	n = strlen(out);
	while (n && (out[n - 1] == '\n' || out[n - 1] == ' '))
		out[--n] = 0;
	return 0;
}

static int path_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

/* ---- mountinfo: longest mount-point prefix of @real ------------------- */

static int find_mount(const char *real, char *mnt, size_t mcap,
		char *fstype, size_t fcap, int *maj, int *min)
{
	FILE *f = fopen("/proc/self/mountinfo", "r");
	char line[1024], mp[256], mm[32], fs[64], src[256];
	size_t best = 0;
	char *p, *dash;

	if (!f)
		return -1;
	*maj = *min = -1;
	while (fgets(line, sizeof line, f)) {
		/* id parent maj:min root mountpoint ... - fstype source ... */
		if (sscanf(line, "%*d %*d %31s %*s %255s", mm, mp) != 2)
			continue;
		dash = strstr(line, " - ");
		if (!dash || sscanf(dash + 3, "%63s %255s", fs, src) != 2)
			continue;
		p = mp;                        /* octal-escape decode skipped:
		                                  bench/prod paths are plain */
		{
			size_t l = strlen(p);

			if (strncmp(real, p, l))
				continue;
			if (l > 1 && real[l] && real[l] != '/')
				continue;
			if (l < best)
				continue;
			best = l;
		}
		snprintf(mnt, mcap, "%s", p);
		snprintf(fstype, fcap, "%.31s", fs);
		sscanf(mm, "%d:%d", maj, min);
		/* btrfs (and friends) mount as an anonymous 0:xx device; the
		 * real block device is the SOURCE field - resolve it */
		if (*maj == 0 && !strncmp(src, "/dev/", 5)) {
			struct stat st;

			if (stat(src, &st) == 0 && S_ISBLK(st.st_mode)) {
				*maj = (int)major(st.st_rdev);
				*min = (int)minor(st.st_rdev);
			}
		}
	}
	fclose(f);
	return best ? 0 : -1;
}

/* ---- leaf classification ----------------------------------------------- */

static void classify_leaf(const char *syspath, const char *name,
		struct pc_st_leaf *lf)
{
	char p[PATH_MAX + 64], v[128], real[PATH_MAX];

	snprintf(lf->name, sizeof lf->name, "%.31s", name);
	lf->rotational = -1;
	lf->cls = PC_ST_LOCAL;
	snprintf(lf->kind, sizeof lf->kind, "block");

	snprintf(p, sizeof p, "%s/queue/rotational", syspath);
	if (read_line(p, v, sizeof v) == 0)
		lf->rotational = atoi(v);

	if (!strncmp(name, "nvme", 4)) {
		/* the NVMe-oF trap: the NAME never classifies - read the
		 * controller's transport file */
		snprintf(p, sizeof p, "%s/device/transport", syspath);
		if (read_line(p, v, sizeof v) == 0) {
			snprintf(lf->kind, sizeof lf->kind, "nvme-%.14s", v);
			lf->cls = strcmp(v, "pcie") ? PC_ST_NETWORK : PC_ST_LOCAL;
			if (!strcmp(v, "pcie")) {
				snprintf(p, sizeof p,
					"%s/device/device/current_link_speed", syspath);
				read_line(p, lf->rate, sizeof lf->rate);
			}
		} else {
			/* native NVMe multipath: the namespace hangs off the
			 * SUBSYSTEM, not a controller - count the controllers */
			DIR *d;
			struct dirent *de;
			int nctrl = 0;

			snprintf(lf->kind, sizeof lf->kind, "nvme-subsys");
			snprintf(p, sizeof p, "%s/device", syspath);
			d = opendir(p);
			if (d) {
				while ((de = readdir(d))) {
					const char *s = de->d_name + 4;

					/* controllers are nvme<digits> exactly - a
					 * namespace (nvme0n1) has the 'n' back */
					if (strncmp(de->d_name, "nvme", 4) || !*s)
						continue;
					while (*s >= '0' && *s <= '9')
						s++;
					if (!*s)
						nctrl++;
				}
				closedir(d);
			}
			snprintf(lf->extra, sizeof lf->extra,
				"native multipath, %d controller(s)", nctrl);
			/* transports live per-controller; report the worst-case
			 * honestly as unknown-fabric unless proven pcie */
			lf->cls = PC_ST_NETWORK;
		}
		return;
	}
	if (!strncmp(name, "vd", 2)) {
		snprintf(lf->kind, sizeof lf->kind, "virtio");
		snprintf(lf->extra, sizeof lf->extra,
			"backing unknown (VM), measurements only");
		lf->cls = PC_ST_VM_OPAQUE;
		return;
	}
	if (!strncmp(name, "nbd", 3) || !strncmp(name, "rbd", 3) ||
	        !strncmp(name, "drbd", 4)) {
		snprintf(lf->kind, sizeof lf->kind, "%.4s-net",
			name[0] == 'd' ? "drbd" : name);
		lf->cls = PC_ST_NETWORK;
		return;
	}
	if (!strncmp(name, "loop", 4)) {
		snprintf(lf->kind, sizeof lf->kind, "loop");
		snprintf(p, sizeof p, "%s/loop/backing_file", syspath);
		if (read_line(p, v, sizeof v) == 0)
			snprintf(lf->extra, sizeof lf->extra, "on %.58s", v);
		return;
	}
	if (!strncmp(name, "sd", 2)) {
		/* the device symlink's real path tells the bus story */
		snprintf(p, sizeof p, "%s/device", syspath);
		if (!realpath(p, real))
			real[0] = 0;
		if (strstr(real, "/session")) {
			snprintf(lf->kind, sizeof lf->kind, "iscsi");
			lf->cls = PC_ST_NETWORK;
			return;
		}
		snprintf(p, sizeof p, "%s/device/sas_address", syspath);
		if (path_exists(p)) {
			snprintf(lf->kind, sizeof lf->kind, "sas");
			return;                /* rate: per-phy, best-effort skip */
		}
		if (strstr(real, "/ata")) {
			snprintf(lf->kind, sizeof lf->kind, "sata");
			return;
		}
		if (strstr(real, "/virtio")) {
			snprintf(lf->kind, sizeof lf->kind, "virtio-scsi");
			snprintf(lf->extra, sizeof lf->extra,
				"backing unknown (VM), measurements only");
			lf->cls = PC_ST_VM_OPAQUE;
			return;
		}
		snprintf(lf->kind, sizeof lf->kind, "scsi");
		return;
	}
	snprintf(lf->kind, sizeof lf->kind, "%.20s", name);
}

/* ---- the recursive chain walk ------------------------------------------ */

struct walk {
	struct pc_st_id *id;
	int seen_maj[64], seen_min[64], nseen;
	size_t chain_len;
};

static void chain_add(struct walk *w, const char *fmt, const char *a,
		const char *b)
{
	size_t left = sizeof w->id->chain - w->chain_len;
	int n;

	if (left < 8)
		return;
	n = snprintf(w->id->chain + w->chain_len, left, fmt, a, b);
	if (n > 0)
		w->chain_len += (size_t)n < left ? (size_t)n : left - 1;
}

static void walk_dev(struct walk *w, int maj, int min, int depth);

static void walk_slaves(struct walk *w, const char *syspath, int depth)
{
	char sd[PATH_MAX + 64], p[PATH_MAX + 64], v[64];
	DIR *d;
	struct dirent *de;
	int maj, min;

	snprintf(sd, sizeof sd, "%s/slaves", syspath);
	d = opendir(sd);
	if (!d)
		return;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(p, sizeof p, "/sys/class/block/%s/dev", de->d_name);
		if (read_line(p, v, sizeof v) == 0 &&
		        sscanf(v, "%d:%d", &maj, &min) == 2)
			walk_dev(w, maj, min, depth + 1);
	}
	closedir(d);
}

static void walk_dev(struct walk *w, int maj, int min, int depth)
{
	struct pc_st_id *id = w->id;
	char sp[64], real[PATH_MAX], p[PATH_MAX + 64], v[128];
	const char *name;
	int i;

	if (depth > 8 || w->nseen >= 64)
		return;
	for (i = 0; i < w->nseen; i++)
		if (w->seen_maj[i] == maj && w->seen_min[i] == min)
			return;
	w->seen_maj[w->nseen] = maj;
	w->seen_min[w->nseen] = min;
	w->nseen++;

	snprintf(sp, sizeof sp, "/sys/dev/block/%d:%d", maj, min);
	if (!realpath(sp, real))
		return;
	name = strrchr(real, '/');
	name = name ? name + 1 : real;

	/* partition? hop to the parent disk */
	snprintf(p, sizeof p, "%s/partition", real);
	if (path_exists(p)) {
		char parent[PATH_MAX];

		snprintf(parent, sizeof parent, "%s", real);
		{
			char *sl = strrchr(parent, '/');

			if (sl)
				*sl = 0;
		}
		snprintf(p, sizeof p, "%s/dev", parent);
		if (read_line(p, v, sizeof v) == 0 &&
		        sscanf(v, "%d:%d", &maj, &min) == 2) {
			chain_add(w, "%s%s -> ", name, "");
			walk_dev(w, maj, min, depth + 1);
			return;
		}
	}

	/* dm (LVM / mpath / crypt / thin): describe, then recurse slaves */
	snprintf(p, sizeof p, "%s/dm/name", real);
	if (read_line(p, v, sizeof v) == 0) {
		char uu[160] = "", flavor[24] = "dm";

		snprintf(p, sizeof p, "%s/dm/uuid", real);
		read_line(p, uu, sizeof uu);
		if (!strncmp(uu, "LVM-", 4))
			snprintf(flavor, sizeof flavor, "lvm");
		else if (!strncmp(uu, "mpath-", 6))
			snprintf(flavor, sizeof flavor, "mpath");
		else if (!strncmp(uu, "CRYPT-", 6))
			snprintf(flavor, sizeof flavor, "crypt");
		chain_add(w, "%s(%s) -> ", v, flavor);
		walk_slaves(w, real, depth);
		return;
	}
	/* md raid: recurse slaves too */
	snprintf(p, sizeof p, "%s/md/level", real);
	if (read_line(p, v, sizeof v) == 0) {
		chain_add(w, "%s(%s) -> ", name, v);
		walk_slaves(w, real, depth);
		return;
	}

	/* a leaf */
	if (id->nleaf < PC_ST_MAXLEAF) {
		struct pc_st_leaf *lf = &id->leaf[id->nleaf];

		memset(lf, 0, sizeof *lf);
		classify_leaf(real, name, lf);
		chain_add(w, "%s(%s) ", name, lf->kind);
		id->nleaf++;
	}
}

/* ---- entry ------------------------------------------------------------- */

int pc_storage_identity(const char *dir, struct pc_st_id *id)
{
	struct statfs sf;
	struct walk w;
	char real[PATH_MAX];
	int maj, min, i;

	memset(id, 0, sizeof *id);
	if (!realpath(dir, real)) {
		LM_ERR("storage identity: %s does not resolve\n", dir);
		return -1;
	}
	if (statfs(real, &sf) != 0)
		return -1;
	if ((unsigned long)sf.f_type == TMPFS_MAGIC) {
		id->is_tmpfs = 1;
		id->cls = PC_ST_MEMORY;
	} else if ((unsigned long)sf.f_type == NFS_SUPER_MAGIC) {
		id->is_nfs = 1;
		id->cls = PC_ST_NETWORK;
	}
	if (find_mount(real, id->mount, sizeof id->mount, id->fstype,
	        sizeof id->fstype, &maj, &min) != 0) {
		snprintf(id->mount, sizeof id->mount, "?");
		return 0;                      /* fs-level identity only */
	}
	if (maj > 0 || (maj == 0 && !id->is_tmpfs && !id->is_nfs)) {
		memset(&w, 0, sizeof w);
		w.id = id;
		if (maj > 0)
			walk_dev(&w, maj, min, 0);
	}
	/* worst leaf wins (fs-level class as the floor) */
	for (i = 0; i < id->nleaf; i++)
		if (id->leaf[i].cls > id->cls)
			id->cls = id->leaf[i].cls;
	return 0;
}

const char *pc_st_class_str(enum pc_st_class c)
{
	switch (c) {
	case PC_ST_LOCAL:     return "local";
	case PC_ST_VM_OPAQUE: return "vm-opaque";
	case PC_ST_NETWORK:   return "network";
	case PC_ST_MEMORY:    return "memory";
	}
	return "?";
}

int pc_storage_format(const struct pc_st_id *id, char *buf, size_t cap)
{
	size_t n = 0;
	int i;

	n += (size_t)snprintf(buf + n, cap - n,
		"storage: mount %s (%s), failure class %s\n",
		id->mount, id->fstype, pc_st_class_str(id->cls));
	if (id->is_tmpfs && n < cap)
		n += (size_t)snprintf(buf + n, cap - n,
			"storage: WARNING tmpfs protects against process crash "
			"ONLY, not host/power loss\n");
	if (id->chain[0] && n < cap)
		n += (size_t)snprintf(buf + n, cap - n, "storage: chain %s\n",
			id->chain);
	for (i = 0; i < id->nleaf && n < cap; i++) {
		const struct pc_st_leaf *lf = &id->leaf[i];

		n += (size_t)snprintf(buf + n, cap - n,
			"storage: leaf %s: %s%s%s%s%s%s, class %s\n",
			lf->name, lf->kind,
			lf->rotational == 1 ? ", rotational" :
				lf->rotational == 0 ? ", non-rotational" : "",
			lf->rate[0] ? ", link " : "", lf->rate,
			lf->extra[0] ? ", " : "", lf->extra,
			pc_st_class_str(lf->cls));
	}
	return (int)(n < cap ? n : cap);
}
