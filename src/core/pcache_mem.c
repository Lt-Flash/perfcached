/* PROVENANCE: vendored from the OpenSIPS cachedb_perf module
 * upstream: /dn/wt-pullshare-hg
 * branch: scratch/pullshare-t1
 * commit: 17bc82132e971cfd03ea0d9e1c150a83c5b42d59
 * path: modules/cachedb_perf/pcache_mem.c   synced: 2026-08-24
 * Local modifications ARE expected (compat-shim rewiring).
 * Check upstream drift with tools/sync-core.sh status|diff.
 *
 * PORTED (task S3): shmem backing -> private anonymous memory.  Threads
 * share the address space, so nothing here needs MAP_SHARED any more:
 * - MAP_SHARED -> MAP_PRIVATE on every mapping;
 * - the shmem VA/file-offset congruence dance is gone (anon THP needs
 *   only VA alignment);
 * - MADV_COLLAPSE verification via /proc/self/smaps directly - the anon
 *   collapse installs the PMD in the caller's page table, so the
 *   ShmemHugePages global-counter workaround is unnecessary;
 * - page sizes probed at runtime (never assume 2M): hugetlb size from
 *   /proc/meminfo Hugepagesize, THP PMD size from
 *   /sys/kernel/mm/transparent_hugepage/hpage_pmd_size;
 * - 32-bit builds refuse absurd reservations (VA is 3GB there).
 */
/*
 * cachedb_perf - high-performance local memory cache
 *
 * Copyright (C) 2026 Yury Kirsanov
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/*
 * Huge-page tier detection (DESIGN 2.6.1 / CP-20, detection half).
 *
 * Every tier is detected by TRYING it on a scratch mapping and verifying
 * the result through /proc/self/smaps - never inferred from the kernel
 * version or from sysfs configuration (the 6.8/6.12 MADV_COLLAPSE
 * divergence proves such checks lie).  The scratch mapping is unmapped
 * after the probe; the never-unmap invariant (DESIGN 3.2) applies to the
 * arena, which holds entries - not to a probe that never does.
 *
 * The probe is advisory: the arena allocator (CP-02/CP-20) re-runs the
 * ladder per chunk, so a pool that appears or drains after startup is
 * handled at allocation time.  Runs once at daemon init, before workers.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

#include "../compat/dprint.h"

#include "pcache_mem.h"

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#define PC_HPS_DEF (2UL * 1024 * 1024)   /* fallback when probing fails */

struct pcache_mem_info pcache_mem;

/* runtime page sizes - NEVER assume 2M: a 64K-granule arm64 kernel has
 * 512M hugetlb pages and a 512M PMD (portability rule, DESIGN.md par 8) */
static size_t pc_hugetlb_sz;
static size_t pc_thp_sz;

static int read_vm_int(const char *path)
{
	FILE *f;
	int v = -1;

	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%d", &v) != 1)
		v = -1;
	fclose(f);
	return v;
}

static long read_meminfo_kb(const char *tag, size_t taglen)
{
	FILE *f;
	char line[256];
	long kb = -1;

	f = fopen("/proc/meminfo", "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, tag, taglen)) {
			kb = strtol(line + taglen, NULL, 10);
			break;
		}
	}
	fclose(f);
	return kb;
}

static void pc_sizes_init(void)
{
	long v;

	if (pc_hugetlb_sz)
		return;

	v = read_meminfo_kb("Hugepagesize:", 13);
	pc_hugetlb_sz = v > 0 ? (size_t)v * 1024 : PC_HPS_DEF;

	pc_thp_sz = PC_HPS_DEF;
	v = read_vm_int("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size");
	if (v > 0)
		pc_thp_sz = (size_t)v;

	pcache_mem.hugetlb_kb = (unsigned long)(pc_hugetlb_sz >> 10);
	pcache_mem.thp_pmd_kb = (unsigned long)(pc_thp_sz >> 10);
}

/* is the huge range starting at @addr PMD-mapped in this process?
 * ("verify, never infer" - DESIGN 2.6.1).  Anon collapse installs the
 * PMD right here, so smaps answers for tiers 2 AND 3. */
static int range_is_huge(unsigned long addr, size_t hps)
{
	FILE *f;
	char line[256], *p;
	unsigned long start, end, kb;
	int in_range = 0, huge = 0;

	f = fopen("/proc/self/smaps", "r");
	if (!f)
		return 0;

	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "%lx-%lx ", &start, &end) == 2) {
			in_range = (start <= addr && addr < end);
			continue;
		}
		if (!in_range)
			continue;
		if (!strncmp(line, "AnonHugePages:", 14) ||
		        !strncmp(line, "ShmemPmdMapped:", 15) ||
		        !strncmp(line, "FilePmdMapped:", 14)) {
			p = strchr(line, ':');
			kb = strtoul(p + 1, NULL, 10);
			if (kb >= hps / 1024) {
				huge = 1;
				break;
			}
		}
	}

	fclose(f);
	return huge;
}

void pcache_mem_probe(void)
{
	char *resv, *aligned;
	void *p;
	size_t len;
	int rc;

	memset(&pcache_mem, 0, sizeof pcache_mem);
	pcache_mem.tier = PCACHE_MEM_4K;
	pc_sizes_init();

	pcache_mem.huge_static =
		read_vm_int("/proc/sys/vm/nr_hugepages");
	pcache_mem.huge_overcommit =
		read_vm_int("/proc/sys/vm/nr_overcommit_hugepages");

	/* tier 1: MAP_HUGETLB.  Pages are secured against the pool (static
	 * or overcommit) at mmap time, so a successful map plus one touched
	 * byte proves the route; failure (ENOMEM/EINVAL) drops a tier */
	p = mmap(NULL, pc_hugetlb_sz, PROT_READ|PROT_WRITE,
	         MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB, -1, 0);
	if (p != MAP_FAILED) {
		*(volatile char *)p = 1;
		munmap(p, pc_hugetlb_sz);
		pcache_mem.tier = PCACHE_MEM_HUGETLB;
		return;
	}

	/* Tiers 2 and 3 need a PMD-aligned anon scratch.  Anon THP needs
	 * only VA alignment (the shmem file-offset congruence rule does not
	 * exist here), but a bare mmap gives no alignment promise: reserve
	 * VA PROT_NONE first, then MAP_FIXED at a PMD boundary inside the
	 * reservation - an atomic replace, no race with other mappings. */
	len = 2 * pc_thp_sz;
	resv = mmap(NULL, len, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (resv == MAP_FAILED)
		return;
	aligned = (char *)(((unsigned long)resv + pc_thp_sz - 1)
	                   & ~((unsigned long)pc_thp_sz - 1));
	p = mmap(aligned, pc_thp_sz, PROT_READ|PROT_WRITE,
	         MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
	if (p == MAP_FAILED) {
		munmap(resv, len);
		return;
	}

	/* tier 2: advice set before first touch -> huge at fault time */
	rc = madvise(aligned, pc_thp_sz, MADV_HUGEPAGE);
	memset(aligned, 1, pc_thp_sz);
	if (rc == 0 && range_is_huge((unsigned long)aligned, pc_thp_sz)) {
		pcache_mem.tier = PCACHE_MEM_THP_ADVISE;
		goto out;
	}

	/* tier 3: collapse the already-faulted 4K pages in place; the anon
	 * collapse PMD-maps them here, so smaps is the verification */
	rc = madvise(aligned, pc_thp_sz, MADV_COLLAPSE);
	if (rc == 0 && range_is_huge((unsigned long)aligned, pc_thp_sz)) {
		pcache_mem.tier = PCACHE_MEM_THP_COLLAPSE;
		goto out;
	}
	if (rc != 0)
		LM_DBG("MADV_COLLAPSE: %s\n", strerror(errno));

out:
	munmap(resv, len);
}

/*
 * CP-20: reserve a large PMD-aligned private-anon region for the arena,
 * backed by huge pages via the same ladder as the probe, mlock-pinned
 * against swap.  Created at daemon init and never unmapped; every thread
 * shares it by construction (the invariant the lock-free read path and
 * CP-09 growth both need).  Returns the base (NULL on total failure ->
 * caller falls back to plain allocation), sets *tier to what was achieved
 * and *locked_mb to the pinned amount.
 */
void *pcache_mem_reserve(size_t size, enum pcache_mem_tier *tier,
		unsigned long *locked_mb)
{
	size_t asize, hsize;
	char *resv, *base;
	void *p;

	*locked_mb = 0;
	*tier = PCACHE_MEM_4K;
	pc_sizes_init();

	/* 32-bit builds have ~3GB of VA total: refuse reservations that
	 * could not possibly serve (config caps the real number - S5) */
	if (sizeof(void *) == 4 && size > 0x60000000UL) {
		LM_ERR("a %zu MB arena does not fit a 32-bit address space\n",
			size >> 20);
		return NULL;
	}

	/* tier 1: MAP_HUGETLB - unswappable, exempt from RLIMIT_MEMLOCK.
	 * Length must be a hugetlb-page multiple; on exotic page sizes
	 * (512M on 64K-granule arm64) skip the tier rather than balloon a
	 * small arena to the giant page multiple. */
	hsize = (size + pc_hugetlb_sz - 1) & ~(pc_hugetlb_sz - 1);
	if (hsize - size <= hsize / 2 || hsize - size <= 64UL << 20) {
		p = mmap(NULL, hsize, PROT_READ|PROT_WRITE,
		         MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB, -1, 0);
		if (p != MAP_FAILED) {
			memset(p, 0, hsize);           /* commit the pool pages */
			*tier = PCACHE_MEM_HUGETLB;
			/*
			 * No mlock() call needed here (tier 1 is already unswappable by
			 * construction, per the comment above) - but report the nominal
			 * size as pinned anyway, matching HG_MALLOC's own hg_mem_reserve()
			 * convention for the identical tier-1 case. Leaving this at the
			 * init'd 0 was technically true (no mlock() syscall happened) but
			 * reads, side by side with HG_MALLOC's own tier-1 NOTICE line, as
			 * "this reservation is unprotected against swap" - which is false;
			 * it is exactly as protected as HG_MALLOC's, just via a different
			 * mechanism. Caught live during a real diagnosis session (2026-08-07)
			 * by the same kind of confusion the tier_probe/tier_active split
			 * above was written to eliminate.
			 */
			*locked_mb = hsize >> 20;
			return p;
		}
	}

	/* tiers 2-4: PMD-aligned private anon (reserve PROT_NONE, then
	 * MAP_FIXED at a PMD boundary - VA alignment is all anon THP needs) */
	asize = (size + pc_thp_sz - 1) & ~(pc_thp_sz - 1);
	resv = mmap(NULL, asize + pc_thp_sz, PROT_NONE,
	            MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (resv == MAP_FAILED)
		return NULL;
	base = (char *)(((unsigned long)resv + pc_thp_sz - 1)
	                & ~((unsigned long)pc_thp_sz - 1));
	p = mmap(base, asize, PROT_READ|PROT_WRITE,
	         MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
	if (p == MAP_FAILED) {
		munmap(resv, asize + pc_thp_sz);
		return NULL;
	}

	/* advise huge before first touch (tier 2), then pin+populate: a cold
	 * mlock populates to pin, so it doubles as the pre-fault (DESIGN 2.6.2) */
	madvise(base, asize, MADV_HUGEPAGE);
	if (mlock(base, asize) == 0) {
		*locked_mb = asize >> 20;
	} else {
		LM_WARN("mlock of the %zu MB arena failed (%s): continuing "
			"unpinned (swappable). If running under systemd, add "
			"LimitMEMLOCK=infinity to the unit.\n",
			asize >> 20, strerror(errno));
		memset(base, 0, asize);        /* still pre-fault */
	}

	if (range_is_huge((unsigned long)base, pc_thp_sz)) {
		*tier = PCACHE_MEM_THP_ADVISE;
	} else if (madvise(base, asize, MADV_COLLAPSE) == 0 &&
	           range_is_huge((unsigned long)base, pc_thp_sz)) {
		*tier = PCACHE_MEM_THP_COLLAPSE;
	} else {
		*tier = PCACHE_MEM_4K;         /* reserved+pinned but 4K */
	}
	return base;
}

const char *pcache_mem_tier_str(enum pcache_mem_tier tier)
{
	switch (tier) {
	case PCACHE_MEM_HUGETLB:
		return "MAP_HUGETLB huge pages";
	case PCACHE_MEM_THP_ADVISE:
		return "THP huge pages via MADV_HUGEPAGE (huge at fault)";
	case PCACHE_MEM_THP_COLLAPSE:
		return "THP huge pages via MADV_COLLAPSE (post-fill retrofit)";
	case PCACHE_MEM_4K:
		return "plain 4K pages";
	case PCACHE_MEM_NO_ARENA:
		return "no dedicated arena; page backing follows the heap "
			"allocator";
	}
	return "unknown";
}
