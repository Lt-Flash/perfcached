/* PROVENANCE: vendored from the OpenSIPS cachedb_perf module
 * upstream: /dn/wt-pullshare-hg
 * branch: scratch/pullshare-t1
 * commit: 17bc82132e971cfd03ea0d9e1c150a83c5b42d59
 * path: modules/cachedb_perf/pcache_mem.h   synced: 2026-08-24
 * Local modifications ARE expected (compat-shim rewiring).
 * Check upstream drift with tools/sync-core.sh status|diff. */
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

#ifndef PCACHE_MEM_H
#define PCACHE_MEM_H

/* the four-tier huge-page ladder (DESIGN 2.6.1 / CP-20), best first.
 * PORTED (S3): backing is private anonymous memory now, not shmem. */
enum pcache_mem_tier {
	PCACHE_MEM_HUGETLB = 1,   /* mmap MAP_HUGETLB - best, 1.42x on chases */
	PCACHE_MEM_THP_ADVISE,    /* anon THP via MADV_HUGEPAGE, huge at fault */
	PCACHE_MEM_THP_COLLAPSE,  /* anon THP via MADV_COLLAPSE, post-fill */
	PCACHE_MEM_4K,            /* plain pages - always works */
	/* Not a backing tier at all: there is no dedicated reservation, so
	 * every allocation goes through the core's shm_malloc() and the real
	 * page backing is whatever the CORE allocator uses (under HG_MALLOC
	 * that is 2M hugepages).  Reporting 4K here was wrong - it named a
	 * property of an arena that does not exist and read as "your cache
	 * is on small pages" when it may well not be. */
	PCACHE_MEM_NO_ARENA = 99,
};

struct pcache_mem_info {
	enum pcache_mem_tier tier;
	int huge_static;          /* vm.nr_hugepages at probe time, -1 unknown */
	int huge_overcommit;      /* vm.nr_overcommit_hugepages, -1 unknown */
	unsigned long hugetlb_kb; /* probed hugetlb page size (never assume 2M) */
	unsigned long thp_pmd_kb; /* probed THP PMD size */
};

extern struct pcache_mem_info pcache_mem;

/* probe the ladder by trying each route on a scratch mapping; init only */
void pcache_mem_probe(void);
const char *pcache_mem_tier_str(enum pcache_mem_tier tier);

/* CP-20: reserve a huge-page-backed, mlock-pinned, PMD-aligned private
 * anon region for the arena (daemon init, never unmapped). NULL -> fall
 * back to plain allocation. */
void *pcache_mem_reserve(size_t size, enum pcache_mem_tier *tier,
		unsigned long *locked_mb);

#endif /* _PCACHE_MEM_H_ */
