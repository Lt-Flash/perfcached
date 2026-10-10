/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * memguard.c - S366: the memory a node may still use, and what it lets its
 * clients take.  The why is in memguard.h; DESIGN 12ks has the numbers.
 *
 * Sources, re-read every second (a cgroup's limit can be changed live):
 *   /proc/meminfo     MemTotal, MemAvailable - the machine's, or under lxcfs
 *                     the container's (245-247: the 1 GB cap lives on a
 *                     parent cgroup the container cannot see, so this is the
 *                     only place it shows)
 *   cgroup v2         memory.max / memory.current / memory.stat inactive_file
 *                     on every level of /proc/self/cgroup's path that has them
 *   cgroup v1         memory.limit_in_bytes / memory.usage_in_bytes /
 *                     memory.stat total_inactive_file, the same walk
 * A cgroup's headroom is limit - usage + its inactive file pages (what
 * reclaim takes first without hurting anyone); the node's available memory
 * is the smallest of MemAvailable and every headroom, its limit the
 * smallest of MemTotal and every limit.
 *
 * A hugetlb pool (ST1, DESIGN 12kw) is allocated at boot and never counted in
 * MemAvailable: under a meminfo limit it is taken off MemTotal - the heap
 * can never have it - and an arena on MAP_HUGETLB commits from that pool,
 * not from the memory this guard watches.  Its uncommitted part is a claim
 * on ordinary memory only beyond the pool's free pages, and only when
 * vm.nr_overcommit_hugepages lets the kernel make surplus pages.  A cgroup
 * limit needs no correction: hugetlb pages are not charged to it.
 *
 * Test hooks (logged when used): PERFCACHED_TEST_MEMINFO=<file> stands in
 * for /proc/meminfo, PERFCACHED_TEST_PROC_CGROUP=<file> for
 * /proc/self/cgroup and PERFCACHED_TEST_CGROUP_ROOT=<dir> for /sys/fs/cgroup;
 * PERFCACHED_TEST_ARENA="<hugetlb|thp> <reserved MB> <committed MB>
 * [<overcommit pages>]" stands in for the arena's tier and figures.
 */
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "compat/dprint.h"
#include "compat/timer.h"
#include "config.h"
#include "memguard.h"
#include "core/pcache_arena.h"
#include "core/pcache_mem.h"

#define MG_LEVELS     16
#define MG_V1_NOLIMIT (1ull << 60)    /* v1 says "unlimited" with ~2^63 */
#define MG_MARGIN     (32ull << 20)   /* on top of the measured process base */
/* the heap give-back: trim once this much connection memory was freed since
 * the last trim, when less than MG_TRIM_QUIET was freed over the last
 * MG_QUIET_S seconds (the burst is over), at most every MG_TRIM_GAP_S */
#define MG_TRIM_AFTER (32ull << 20)
#define MG_TRIM_QUIET (4ull << 20)
#define MG_QUIET_S    5
#define MG_TRIM_GAP_S 10

static struct {
	int enabled;
	int pct;                           /* memory_floor = N% (0 = MB form) */
	unsigned long long floor_cfg;      /* memory_floor = N MB, in bytes */
	int cgv;                           /* 2, 1, 0 = no cgroup levels found */
	int nlvl;
	char lvl[MG_LEVELS][PATH_MAX];
	size_t root_len;                   /* the cgroup root's length in lvl[] */
	const char *meminfo;

	pthread_mutex_t mx;                /* the sampled figures, for stats */
	char source[96];
	unsigned long long limit, avail, floor, hyst, reserve;
	unsigned long long pool, pool_free;  /* ST1: the hugetlb pool, bytes */
	long long room;

	/* ST1: PERFCACHED_TEST_ARENA - the arena's tier and figures */
	int t_set, t_huge;
	unsigned long long t_reserved, t_committed;
	long t_overcommit;

	/* the admission path's - atomics */
	long long budget;
	int refusing;
	unsigned int refusing_since;
	unsigned long long refused, refused_at_engage, engaged;
	unsigned long long freed, idle_released;

	/* maintenance-thread only */
	unsigned long long freed_prev, freed_at_trim, quiet[MG_QUIET_S];
	unsigned int last_trim;
	unsigned long long trims;
	long long trim_last;
	int can_trim;
	long max_mem;
} G = { .mx = PTHREAD_MUTEX_INITIALIZER, .max_mem = -1 };

/* ---- files -------------------------------------------------------------- */

static int slurp(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC), n, got = 0;

	buf[0] = '\0';                     /* a failed read leaves an empty text */
	if (fd < 0)
		return -1;
	while ((size_t)got < cap - 1 &&
	       (n = (int)read(fd, buf + got, cap - 1 - (size_t)got)) > 0)
		got += n;
	close(fd);
	buf[got] = '\0';
	return got;
}

/* a file holding one number; "max" = unlimited (1, *v untouched) */
static int read_num(const char *path, unsigned long long *v)
{
	char b[64], *e;
	unsigned long long x;

	if (slurp(path, b, sizeof b) <= 0)
		return -1;
	if (!strncmp(b, "max", 3))
		return 1;
	x = strtoull(b, &e, 10);
	if (e == b)
		return -1;
	*v = x;
	return 0;
}

/* the value of "@key N" in a key-per-line file (memory.stat, /proc/meminfo
 * with the colon); -1 when absent */
static long long field(const char *text, const char *key)
{
	size_t kl = strlen(key);
	const char *p = text;

	while (p && *p) {
		if (!strncmp(p, key, kl) && (p[kl] == ' ' || p[kl] == ':' || p[kl] == '\t'))
			return strtoll(p + kl + 1 + strspn(p + kl + 1, " :\t"), NULL, 10);
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return -1;
}

static int file_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

/* ---- discovery ---------------------------------------------------------- */

/* collect @base@path and each parent down to @base itself, keeping those
 * that hold @probe */
static void walk_up(const char *base, const char *path, const char *probe)
{
	char d[PATH_MAX], f[PATH_MAX + 32];
	size_t bl = strlen(base), l;

	if ((size_t)snprintf(d, sizeof d, "%s%s", base, path) >= sizeof d)
		return;
	G.root_len = bl;
	l = strlen(d);
	while (l > bl && d[l - 1] == '/')  /* "/" (a namespace's root) = base */
		d[--l] = '\0';
	for (;;) {
		snprintf(f, sizeof f, "%s/%s", d, probe);
		if (G.nlvl < MG_LEVELS && file_exists(f))
			snprintf(G.lvl[G.nlvl++], PATH_MAX, "%s", d);
		if (l <= bl)
			break;
		while (l > bl && d[l - 1] != '/')   /* the last component */
			l--;
		if (l > bl)
			l--;                       /* and its slash */
		d[l] = '\0';
	}
}

static void discover(void)
{
	const char *pcg = getenv("PERFCACHED_TEST_PROC_CGROUP");
	const char *root = getenv("PERFCACHED_TEST_CGROUP_ROOT");
	char buf[8192], v2[PATH_MAX] = "", v1[PATH_MAX] = "", *line, *save = NULL;
	int have2 = 0, have1 = 0;

	if (pcg)
		LM_NOTICE("memory: TEST HOOK - /proc/self/cgroup read from %s\n", pcg);
	if (root)
		LM_NOTICE("memory: TEST HOOK - cgroup root is %s\n", root);
	else
		root = "/sys/fs/cgroup";
	if (slurp(pcg ? pcg : "/proc/self/cgroup", buf, sizeof buf) <= 0)
		return;
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *c1 = strchr(line, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL;

		if (!c2)
			continue;
		*c1 = *c2 = '\0';
		if (!strcmp(line, "0") && !c1[1]) {
			snprintf(v2, sizeof v2, "%s", c2 + 1);
			have2 = 1;
		} else {
			char *tok, *s2 = NULL;

			for (tok = strtok_r(c1 + 1, ",", &s2); tok; tok = strtok_r(NULL, ",", &s2))
				if (!strcmp(tok, "memory")) {
					snprintf(v1, sizeof v1, "%s", c2 + 1);
					have1 = 1;
				}
		}
	}
	if (have2) {
		walk_up(root, v2, "memory.max");
		if (G.nlvl)
			G.cgv = 2;
	}
	if (!G.cgv && have1) {
		char base[PATH_MAX];

		snprintf(base, sizeof base, "%s/memory", root);
		walk_up(base, v1, "memory.limit_in_bytes");
		if (G.nlvl)
			G.cgv = 1;
	}
}

/* ---- the sample --------------------------------------------------------- */

static void sample(unsigned long long *limit, unsigned long long *avail,
		char *src, size_t srclen)
{
	char b[8192], f[PATH_MAX + 32];
	long long mt = -1, ma = -1;
	int i;

	*limit = *avail = 0;
	snprintf(src, srclen, "none");
	if (slurp(G.meminfo, b, sizeof b) > 0) {
		mt = field(b, "MemTotal");
		ma = field(b, "MemAvailable");
		if (ma < 0) {                  /* a kernel before 3.14 */
			long long fr = field(b, "MemFree"), ca = field(b, "Cached"),
				bu = field(b, "Buffers");

			ma = (fr > 0 ? fr : 0) + (ca > 0 ? ca : 0) + (bu > 0 ? bu : 0);
		}
		{
			long long ht = field(b, "HugePages_Total"),
				hf = field(b, "HugePages_Free"),
				hs = field(b, "Hugepagesize");   /* kB */

			G.pool = ht > 0 && hs > 0
				? (unsigned long long)ht * ((unsigned long long)hs << 10) : 0;
			G.pool_free = hf > 0 && hs > 0
				? (unsigned long long)hf * ((unsigned long long)hs << 10) : 0;
		}
		if (mt > 0) {
			*limit = (unsigned long long)mt << 10;
			if (G.pool && G.pool < *limit)
				*limit -= G.pool;          /* ST1: never the heap's */
			*avail = (unsigned long long)(ma > 0 ? ma : 0) << 10;
			snprintf(src, srclen, G.pool ? "meminfo, less the hugetlb pool"
				: "meminfo");
		}
	}
	for (i = 0; i < G.nlvl; i++) {
		unsigned long long lim = 0, use = 0, head;
		long long inact;
		const char *rel = G.lvl[i] + G.root_len;

		snprintf(f, sizeof f, "%s/%s", G.lvl[i],
			G.cgv == 2 ? "memory.max" : "memory.limit_in_bytes");
		if (read_num(f, &lim) != 0 || lim >= MG_V1_NOLIMIT)
			continue;                  /* unlimited at this level */
		snprintf(f, sizeof f, "%s/%s", G.lvl[i],
			G.cgv == 2 ? "memory.current" : "memory.usage_in_bytes");
		if (read_num(f, &use) != 0)
			continue;
		snprintf(f, sizeof f, "%s/memory.stat", G.lvl[i]);
		inact = -1;
		if (slurp(f, b, sizeof b) > 0) {
			inact = field(b, G.cgv == 2 ? "inactive_file"
				: "total_inactive_file");
			if (inact < 0 && G.cgv == 1)   /* v1 without hierarchy */
				inact = field(b, "inactive_file");
		}
		head = lim > use ? lim - use : 0;
		if (inact > 0)
			head += (unsigned long long)inact;
		if (head > lim)
			head = lim;
		if (!*limit || lim < *limit) {
			*limit = lim;
			snprintf(src, srclen, "cgroup v%d %s", G.cgv, *rel ? rel : "/");
		}
		if (head < *avail || !*avail)
			*avail = head;
	}
}

static unsigned long long floor_for(unsigned long long limit)
{
	unsigned long long f;

	if (!G.pct)
		return G.floor_cfg;
	f = limit / 100 * (unsigned long long)G.pct;
	if (f < PC_MEMFLOOR_MIN)
		f = PC_MEMFLOOR_MIN;
	if (f > PC_MEMFLOOR_MAX)
		f = PC_MEMFLOOR_MAX;
	if (f > limit / 2)                 /* a tiny limit: half of it, at most */
		f = limit / 2;
	return f;
}

/* the arena's tier and figures (or the test hook's) */
static void arena_figures(int *huge, unsigned long long *reserved,
		unsigned long long *committed, long *overcommit)
{
	struct pcache_arena_pressure pr;

	if (G.t_set) {
		*huge = G.t_huge;
		*reserved = G.t_reserved;
		*committed = G.t_committed;
		*overcommit = G.t_overcommit;
		return;
	}
	memset(&pr, 0, sizeof pr);
	pcache_arena_pressure(&pr);
	*huge = pcache_arena_tier() == PCACHE_MEM_HUGETLB;
	*reserved = pr.reserved_bytes;
	*committed = pr.committed_bytes;
	*overcommit = pcache_mem.huge_overcommit;
}

/* what the arena may still take from the memory this guard watches; the
 * arena's ceiling into *ceiling, its tier into *huge */
static unsigned long long arena_reserve(unsigned long long *ceiling, int *huge)
{
	unsigned long long res, com, unc;
	long oc;
	int h;

	arena_figures(&h, &res, &com, &oc);
	if (ceiling)
		*ceiling = res;
	if (huge)
		*huge = h;
	unc = res > com ? res - com : 0;
	if (!h)
		return unc;
	/* ST1: a hugetlb arena commits from the pool, already outside
	 * MemAvailable; only surplus pages beyond its free part, which the
	 * kernel makes from ordinary memory, are a claim on it */
	if (oc == 0 || unc <= G.pool_free)   /* -1 = unknown: it may */
		return 0;
	return unc - G.pool_free;
}

static unsigned long long rss_bytes(void)
{
	char b[128];
	unsigned long long sz = 0, res = 0;

	if (slurp("/proc/self/statm", b, sizeof b) <= 0 ||
	    sscanf(b, "%llu %llu", &sz, &res) != 2)
		return 0;
	return res * (unsigned long long)sysconf(_SC_PAGESIZE);
}

/* sample, then the budget; the caller is the maintenance thread (or init) */
static void resample(int at_init)
{
	unsigned long long lim, av, fl, hy, rs;
	long long room, eff, bud;
	char src[96];

	sample(&lim, &av, src, sizeof src);
	rs = arena_reserve(NULL, NULL);
	fl = floor_for(lim);
	hy = lim / 20;
	if (hy < (16ull << 20))
		hy = 16ull << 20;
	room = (long long)av - (long long)fl - (long long)rs;
	pthread_mutex_lock(&G.mx);
	snprintf(G.source, sizeof G.source, "%s", src);
	__atomic_store_n(&G.limit, lim, __ATOMIC_RELAXED);
	__atomic_store_n(&G.avail, av, __ATOMIC_RELAXED);
	__atomic_store_n(&G.floor, fl, __ATOMIC_RELAXED);
	__atomic_store_n(&G.hyst, hy, __ATOMIC_RELAXED);
	__atomic_store_n(&G.reserve, rs, __ATOMIC_RELAXED);
	G.room = room;
	pthread_mutex_unlock(&G.mx);
	if (!G.enabled || !lim)
		return;
	eff = room;
	if (__atomic_load_n(&G.refusing, __ATOMIC_ACQUIRE)) {
		eff = room - (long long)hy;
		if (eff > 0 && !at_init &&
		    __atomic_exchange_n(&G.refusing, 0, __ATOMIC_ACQ_REL)) {
			unsigned long long n = __atomic_load_n(&G.refused, __ATOMIC_RELAXED)
				- G.refused_at_engage;

			LM_NOTICE("clients: memory above the floor again - %llu MB "
				"available (floor %llu MB, arena reserve %llu MB); "
				"%llu connection(s) refused in %us\n", av >> 20, fl >> 20,
				rs >> 20, n, get_ticks() - G.refusing_since);
			eff = room;
		}
	}
	bud = eff > 0 ? eff / (long long)PC_CONN_COST_ACCEPT : 0;
	__atomic_store_n(&G.budget, bud, __ATOMIC_RELEASE);
}

/* ---- the API ------------------------------------------------------------ */

long pc_memguard_init(const struct pc_config *cfg)
{
	unsigned long long ceiling = 0, committed, base, rss, lim, fl, arena_part;
	const char *mi = getenv("PERFCACHED_TEST_MEMINFO");
	const char *ta = getenv("PERFCACHED_TEST_ARENA");
	char how[32], kind[16] = "";
	long long room;
	int huge = 0;

	G.pct = cfg->memory_floor_pct;
	G.floor_cfg = (unsigned long long)cfg->memory_floor_mb << 20;
	G.enabled = G.pct > 0 || G.floor_cfg > 0;
	if (mi)
		LM_NOTICE("memory: TEST HOOK - /proc/meminfo read from %s\n", mi);
	G.meminfo = mi ? mi : "/proc/meminfo";
	if (ta) {
		unsigned long rmb = 0, cmb = 0;
		long oc = 0;

		if (sscanf(ta, "%15s %lu %lu %ld", kind, &rmb, &cmb, &oc) >= 3) {
			G.t_set = 1;
			G.t_huge = !strcmp(kind, "hugetlb");
			G.t_reserved = (unsigned long long)rmb << 20;
			G.t_committed = (unsigned long long)cmb << 20;
			G.t_overcommit = oc;
			LM_NOTICE("memory: TEST HOOK - arena taken as %s, %lu MB "
				"reserved, %lu MB committed, overcommit %ld\n",
				G.t_huge ? "hugetlb" : "not hugetlb", rmb, cmb, oc);
		}
	}
#ifdef __GLIBC__
	G.can_trim = !strcmp(PC_ALLOC_NAME, "libc");
#endif
	discover();
	resample(1);
	/* the process base, MEASURED: what is resident beyond the arena's
	 * committed part now that the WAL rings, the stage buffer and the
	 * tables exist - plus a margin for the per-worker scratch that faults
	 * in at first use */
	{
		unsigned long long r, c;
		long oc;

		arena_figures(&huge, &r, &c, &oc);
		ceiling = r;
		committed = c;
	}
	rss = rss_bytes();
	/* hugetlb pages are not in VmRSS: there RSS is the base already */
	if (huge)
		base = rss + MG_MARGIN;
	else
		base = (rss > committed ? rss - committed : 0) + MG_MARGIN;
	/* under a meminfo limit the pool is gone from it already, and a cgroup
	 * is never charged for hugetlb pages - the hugetlb arena is outside */
	arena_part = huge ? 0 : ceiling;
	lim = __atomic_load_n(&G.limit, __ATOMIC_RELAXED);
	fl = __atomic_load_n(&G.floor, __ATOMIC_RELAXED);
	if (!lim) {
		LM_WARN("memory: no memory limit could be read (no %s, no "
			"limited cgroup) - the client guard is off and max_clients "
			"counts descriptors only\n", G.meminfo);
		G.enabled = 0;
		return -1;
	}
	if (lim > fl + arena_part + base)
		G.max_mem = (long)((lim - fl - arena_part - base) / PC_CONN_COST_IDLE);
	else {
		LM_WARN("memory: the limit %llu MB leaves nothing for clients after "
			"the floor %llu MB, the arena ceiling %llu MB and the process "
			"base %llu MB - lower [memory] arena_mb or memory_floor\n",
			lim >> 20, fl >> 20, arena_part >> 20, base >> 20);
		G.max_mem = 0;
	}
	if (G.max_mem < 16)
		G.max_mem = 16;
	room = G.room;
	if (G.pct)
		snprintf(how, sizeof how, "%d%%", G.pct);
	else
		snprintf(how, sizeof how, "%d MB", cfg->memory_floor_mb);
	if (G.enabled)
		LM_NOTICE("memory: limit %llu MB (%s), %llu MB available; new "
			"connections are refused once less than the floor %llu MB "
			"(memory_floor %s) stays free beyond the arena's uncommitted "
			"%llu MB - room now for ~%lld connections\n", lim >> 20,
			G.source, __atomic_load_n(&G.avail, __ATOMIC_RELAXED) >> 20,
			fl >> 20, how,
			__atomic_load_n(&G.reserve, __ATOMIC_RELAXED) >> 20,
			room > 0 ? room / (long long)PC_CONN_COST_ACCEPT : 0);
	else
		LM_NOTICE("memory: limit %llu MB (%s) - the client guard is OFF "
			"(memory_floor = off): nothing refuses a connection before "
			"memory runs out\n", lim >> 20, G.source);
	if (G.pool)
		LM_NOTICE("memory: a %llu MB hugetlb pool (%llu MB free) is outside "
			"that limit; the arena %s\n", G.pool >> 20, G.pool_free >> 20,
			huge ? "commits from it, so its uncommitted part claims only "
			"surplus pages beyond the pool's free ones"
			: "is not on it, so it counts against the limit");
	return G.max_mem;
}

void pc_memguard_tick(void)
{
	unsigned long long freed, recent = 0;
	unsigned int now = get_ticks();
	int i;

	resample(0);
	if (!G.can_trim)
		return;
#ifdef __GLIBC__
	freed = __atomic_load_n(&G.freed, __ATOMIC_RELAXED);
	G.quiet[now % MG_QUIET_S] = freed - G.freed_prev;
	G.freed_prev = freed;
	for (i = 0; i < MG_QUIET_S; i++)
		recent += G.quiet[i];
	if (freed - G.freed_at_trim >= MG_TRIM_AFTER && recent < MG_TRIM_QUIET &&
	    (!G.last_trim || now - G.last_trim >= MG_TRIM_GAP_S)) {
		unsigned long long r0 = rss_bytes(), r1;

		malloc_trim(0);
		r1 = rss_bytes();
		pthread_mutex_lock(&G.mx);
		G.trims++;
		G.trim_last = (long long)r0 - (long long)r1;
		pthread_mutex_unlock(&G.mx);
		LM_NOTICE("heap: %llu MB of connection memory freed since the last "
			"trim - malloc_trim gave back %lld MB (RSS %llu -> %llu MB)\n",
			(freed - G.freed_at_trim) >> 20, G.trim_last / 1048576,
			r0 >> 20, r1 >> 20);
		G.freed_at_trim = freed;
		G.last_trim = now;
	}
#else
	(void)freed; (void)recent; (void)now; (void)i;
#endif
}

int pc_memguard_admit(void)
{
	if (!G.enabled)
		return 1;
	if (__atomic_sub_fetch(&G.budget, 1, __ATOMIC_RELAXED) >= 0)
		return 1;
	__atomic_add_fetch(&G.refused, 1, __ATOMIC_RELAXED);
	if (!__atomic_load_n(&G.refusing, __ATOMIC_RELAXED) &&
	    !__atomic_exchange_n(&G.refusing, 1, __ATOMIC_ACQ_REL)) {
		unsigned long long fl = __atomic_load_n(&G.floor, __ATOMIC_RELAXED),
			rs = __atomic_load_n(&G.reserve, __ATOMIC_RELAXED),
			hy = __atomic_load_n(&G.hyst, __ATOMIC_RELAXED);

		G.refusing_since = get_ticks();
		G.refused_at_engage = __atomic_load_n(&G.refused, __ATOMIC_RELAXED) - 1;
		__atomic_add_fetch(&G.engaged, 1, __ATOMIC_RELAXED);
		LM_WARN("clients: memory floor reached - %llu MB available, floor "
			"%llu MB, arena reserve %llu MB: refusing new connections "
			"until %llu MB are free\n",
			__atomic_load_n(&G.avail, __ATOMIC_RELAXED) >> 20, fl >> 20,
			rs >> 20, (fl + rs + hy) >> 20);
	}
	return 0;
}

void pc_memguard_freed(size_t bytes, int idle)
{
	__atomic_add_fetch(&G.freed, (unsigned long long)bytes, __ATOMIC_RELAXED);
	if (idle)
		__atomic_add_fetch(&G.idle_released, 1, __ATOMIC_RELAXED);
}

void pc_memguard_stats(struct pc_memguard_stats *out)
{
	memset(out, 0, sizeof *out);
	pthread_mutex_lock(&G.mx);
	out->enabled = G.enabled;
	snprintf(out->source, sizeof out->source, "%s", G.source);
	out->limit = G.limit;
	out->available = G.avail;
	out->floor = G.floor;
	out->arena_reserve = G.reserve;
	out->hugetlb_pool = G.pool;
	out->hugetlb_pool_free = G.pool_free;
	out->room = G.room;
	out->trims = G.trims;
	out->trim_last_bytes = G.trim_last;
	pthread_mutex_unlock(&G.mx);
	out->budget = __atomic_load_n(&G.budget, __ATOMIC_RELAXED);
	out->refusing = __atomic_load_n(&G.refusing, __ATOMIC_RELAXED);
	out->refusing_since = out->refusing ? G.refusing_since : 0;
	out->refused = __atomic_load_n(&G.refused, __ATOMIC_RELAXED);
	out->engaged = __atomic_load_n(&G.engaged, __ATOMIC_RELAXED);
	out->freed_bytes = __atomic_load_n(&G.freed, __ATOMIC_RELAXED);
	out->idle_released = __atomic_load_n(&G.idle_released, __ATOMIC_RELAXED);
	out->trim_supported = G.can_trim;
	out->max_clients_memory = G.max_mem;
}
