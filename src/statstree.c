/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * statstree.c - what the stats and members verbs answer: pc_stats_tree,
 * the whole /stats tree (the process, the doors, the table, the WAL, the
 * cluster and proxy planes, the S165 fleet table of unknown commands),
 * pc_members_tree, and the S123/S358 reset machinery with the baselines
 * it keeps for the planes whose counters it does not own.  The maps verb
 * shares respmap_rows (verbsint.h).  Moved from verbs.c unchanged (the
 * 0.7.2 split, wave 1 - doc/MODULARITY.md).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "compat/dprint.h"
#include "compat/compat.h"
#include "compat/timer.h"
#include "reload.h"
#include "quiesce.h"
#include "json.h"
#include "ptree.h"
#include "winctr_tw.h"
#include "store.h"
#include "config.h"
#include "verbs.h"
#include "upstream.h"
#include "pcaddr.h"
#include "ackstate.h"
#include "fnv1a.h"
#include "upgrade.h"
#include "mail.h"
#include "alerts.h"
#include "iostat.h"
#include "uring.h"
#include "core/pcache_htable.h"
#include "core/pcache_arena.h"
#include "core/pcache_mem.h"
#include "wal.h"
#include "rdb.h"
#include "cluster.h"
#include "clane.h"
#include "clretain.h"
#include "clterm.h"
#include "clunk.h"
#include "obs.h"
#include "metrics.h"
#include "proto.h"
#include "memguard.h"
#include "pubsub.h"
#include "daemon.h"
#include "version.h"
#include "storage.h"
#include "walprobe.h"
#include "respmap.h"
#include "verbsint.h"
#include "statsreset.h"
#include "statscluster.h"            /* the cluster block (wave 1) */

/* IPv4 dotted-quad without pulling inet_ntoa's static buffer into a
 * multithreaded reply path */
static const char *pc_inet_ntop4(struct in_addr a, char *buf, size_t cap)
{
	/* by hand: `members` formats one per member per call, and the
	 * snprintf family was 31% of its worker time */
	unsigned int v = ntohl(a.s_addr);
	char *p = buf;
	int sh;

	if (cap < 16) {
		if (cap)
			buf[0] = 0;
		return buf;
	}
	for (sh = 24; sh >= 0; sh -= 8) {
		unsigned int o = (v >> sh) & 0xff;

		if (o >= 100)
			*p++ = (char)('0' + o / 100);
		if (o >= 10)
			*p++ = (char)('0' + o / 10 % 10);
		*p++ = (char)('0' + o % 10);
		if (sh)
			*p++ = '.';
	}
	*p = 0;
	return buf;
}

/* lowercase hex of @n bytes into @out (2n chars, no terminator) */
static void hex_bytes(char *out, const unsigned char *b, size_t n)
{
	static const char hx[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < n; i++) {
		out[2 * i] = hx[b[i] >> 4];
		out[2 * i + 1] = hx[b[i] & 15];
	}
}

/* A SHORT token, not pcache_mem_tier_str()'s sentence.  That string is
 * written for a human reading a startup log; this one is compared by
 * machines and scanned down a column of nodes, which is the whole point
 * of putting the tier on the fleet view - a node that differs should be
 * obvious at a glance, not at the end of a line of prose. */
static const char *mem_tier_token(int t)
{
	switch (t) {
	case PCACHE_MEM_HUGETLB:      return "hugetlb";
	case PCACHE_MEM_THP_ADVISE:   return "thp-advise";
	case PCACHE_MEM_THP_COLLAPSE: return "thp-collapse";
	case PCACHE_MEM_4K:           return "4k";
	case PCACHE_MEM_NO_ARENA:     return "no-arena";
	default:                      return "unknown";
	}
}

/* S165: the commands clients sent that no door implements, for the
 * whole fleet - this node's table folded with every live peer's latest
 * (cluster.c), newest first.  `count` is fleet-wide, `here` this node's
 * share, `node` the member that saw it last; `members` / `reporting` say
 * how much of the fleet the list covers.  Names only, never arguments. */
static void unknown_tree(struct pc_tw *out)
{
	struct clunk_table *self = malloc(sizeof *self);
	struct clunk_table *fl = malloc(sizeof *fl);
	int members = 1, reporting = 1, i;

	if (!self || !fl) {
		free(self);
		free(fl);
		return;
	}
	pc_obs_unknown_local(self);
	pc_cluster_unknown_fleet(self, fl, &members, &reporting);
	pc_tw_key(out, "unknown_commands");
	pc_tw_map(out);
	pc_tw_key(out, "members");
	pc_tw_i64(out, members);
	pc_tw_key(out, "reporting");
	pc_tw_i64(out, reporting);
	pc_tw_ctr(out, "unk.other", "other", (unsigned long long)fl->other);
	pc_tw_ctr(out, "unk.preauth", "preauth", (unsigned long long)fl->preauth);
	{
		/* peers' tables heard, and frames refused whole (clunk_parse) */
		unsigned long long rcv = 0, bad = 0;

		pc_cluster_unknown_figures(&rcv, &bad);
		pc_tw_key(out, "frames_received");
		pc_tw_i64(out, (long long)rcv);
		pc_tw_key(out, "frames_refused");
		pc_tw_i64(out, (long long)bad);
	}
	pc_tw_key(out, "rows");
	pc_tw_arr(out);
	for (i = 0; i < fl->n; i++) {
		const struct clunk_row *r = &fl->rows[i];

		pc_tw_map(out);
		pc_tw_key(out, "dialect");
		pc_tw_str(out, clunk_dialect_name(r->dialect));
		pc_tw_key(out, "name");
		pc_tw_bulk(out, r->name, r->nlen);
		{
			/* S351: keyed by a hash of the name - a CLIENT's bytes, of
			 * any length - so the window table holds a bounded key */
			char uk[64];
			unsigned long long uh = 1469598103934665603ULL;
			unsigned int q;

			for (q = 0; q < r->nlen; q++)
				uh = (uh ^ (unsigned char)r->name[q]) * 1099511628211ULL;
			snprintf(uk, sizeof uk, "unk.%d.%016llx.count", (int)r->dialect, uh);
			pc_tw_ctr(out, uk, "count", (unsigned long long)r->count);
			snprintf(uk, sizeof uk, "unk.%d.%016llx.here", (int)r->dialect, uh);
			pc_tw_ctr(out, uk, "here", (unsigned long long)r->here);
		}
		pc_tw_key(out, "first_s");
		pc_tw_i64(out, (long long)r->first_s);
		pc_tw_key(out, "last_s");
		pc_tw_i64(out, (long long)r->last_s);
		pc_tw_key(out, "addr");
		pc_tw_bulk(out, r->addr, strlen(r->addr));
		pc_tw_key(out, "client");
		pc_tw_bulk(out, r->client, strlen(r->client));
		pc_tw_key(out, "node");
		pc_tw_i64(out, r->node);
		pc_tw_end(out);
	}
	pc_tw_end(out);
	pc_tw_end(out);
	free(self);
	free(fl);
}

/* S37: the fleet as a tree, rendered from THIS node's vantage point.
 * Shared by the `members` verb and the status page's /members route
 * (which renders the same tree as JSON), so the two cannot drift - a
 * page disagreeing with the verb would be worse than no page. */
/*
 * S67: the `stats` body, callable from the verb AND from the HTTP
 * door.  The dashboard needs exactly these numbers, and having no way
 * to reach them is why the page could only ever show what /members
 * carries - which is how it ended up reporting the RESP door's
 * connection count and nothing about the native one every client uses.
 *
 * @only_col NULL = every collection, else just that one.
 */
/* The facts about the PROCESS that the store cannot supply: how long it
 * has been up, how much CPU it has burned, and how much memory the
 * kernel believes it holds.  A dashboard had none of these, so its CPU
 * panel could not be written at all, and RSS - the only figure that
 * predicts an OOM kill - was invisible next to an arena number that
 * deliberately does not track it.
 *
 * perfcached is threaded, not forked, so /proc/self is the whole daemon:
 * Linux aggregates every thread's CPU into the process entry.
 *
 * CPU is CUMULATIVE milliseconds, never a percentage.  A percentage
 * needs two samples and a window, and only the caller knows its own
 * polling interval; a rate computed here could only be the mean since
 * startup, which flattens with age and hides exactly the recent spike
 * someone opened the page to find.
 */
/* S258: the /proc readings, at most once a second PER WORKER.  Opening,
 * parsing and closing /proc/self/stat and /proc/self/statm was ~25% of a
 * stats call (~160 us, 6.6 KB of JSON).  The figures are for display -
 * CPU time and RSS on the page, gate.sh - so a reading up to a second old
 * is exact enough, and a thread-local copy needs no lock: a connection
 * stays on its worker, so the page's poll reuses the one it made. */
static __thread unsigned int proc_tick;
static __thread int proc_have;
static __thread unsigned long long proc_ut, proc_st, proc_rss;
static __thread long long proc_thr;

static void stats_process(struct pc_tw *out)
{
	long tck = sysconf(_SC_CLK_TCK), pg = sysconf(_SC_PAGESIZE);
	unsigned long long ut = 0, st = 0, rss_pages = 0;
	long long thr = 0;
	unsigned int now = get_ticks();
	FILE *f;
	char buf[512], *p;

	if (tck <= 0)
		tck = 100;
	if (pg <= 0)
		pg = 4096;
	if (proc_have && proc_tick == now) {
		ut = proc_ut;
		st = proc_st;
		rss_pages = proc_rss;
		thr = proc_thr;
		goto have;
	}

	f = fopen("/proc/self/statm", "r");
	if (f) {
		if (fscanf(f, "%*s %llu", &rss_pages) != 1)
			rss_pages = 0;
		fclose(f);
	}
	f = fopen("/proc/self/stat", "r");
	if (f) {
		/* comm sits in parentheses and may contain them itself,
		 * so the numeric fields start after the LAST ')'. */
		if (fgets(buf, sizeof buf, f)) {
			p = strrchr(buf, ')');
			if (p && sscanf(p + 1, " %*c %*d %*d %*d %*d %*d %*u"
					" %*u %*u %*u %*u %llu %llu %*d %*d"
					" %*d %*d %lld",
					&ut, &st, &thr) != 3) {
				ut = st = 0;
				thr = 0;
			}
		}
		fclose(f);
	}
	proc_ut = ut;
	proc_st = st;
	proc_rss = rss_pages;
	proc_thr = thr;
	proc_tick = now;
	proc_have = 1;
have:
	pc_tw_key(out, "process");
	pc_tw_map(out);
	pc_tw_key(out, "allocator");               /* S282 */
	pc_tw_str(out, PC_ALLOC_NAME);
	pc_tw_key(out, "build");                   /* S357: release | debug | asan */
	pc_tw_str(out, PC_BUILD_FLAVOR);
	pc_tw_key(out, "uptime_s");
	pc_tw_i64(out, (long long)pc_metrics_uptime());
	pc_tw_key(out, "pid");
	pc_tw_i64(out, (long long)getpid());
	pc_tw_key(out, "threads");
	pc_tw_i64(out, thr);
	{                                       /* S381: what cpu is a share of */
		double nc = pc_process_cpus();
		char nb[32];
		int nl = snprintf(nb, sizeof nb, "%.2f", nc);

		while (nl > 1 && nb[nl - 1] == '0')    /* 4.00 -> 4, 1.50 -> 1.5 */
			nb[--nl] = 0;
		if (nl > 1 && nb[nl - 1] == '.')
			nb[--nl] = 0;
		pc_tw_key(out, "cpus");
		pc_tw_num(out, nb, (size_t)nl);
	}
	pc_tw_ctr(out, "process.cpu_user_ms", "cpu_user_ms", (unsigned long long)((ut * 1000ULL / (unsigned long long)tck)));
	pc_tw_ctr(out, "process.cpu_sys_ms", "cpu_sys_ms", (unsigned long long)((st * 1000ULL / (unsigned long long)tck)));
	pc_tw_key(out, "rss_bytes");
	pc_tw_i64(out, (long long)(rss_pages * (unsigned long long)pg));
	pc_tw_end(out);
	/* S123: what the running totals below count from - the start, or
	 * the last reset */
	{
		long long ra, nr;

		statsreset_times(&ra, &nr);
		pc_tw_key(out, "since");
		pc_tw_map(out);
		pc_tw_key(out, "reset_at");
		pc_tw_i64(out, ra);
		/* S358: when the schedule resets them next; 0 = no schedule */
		pc_tw_key(out, "next_reset");
		pc_tw_i64(out, nr);
		pc_tw_key(out, "s");
		pc_tw_i64(out, ra ? (long long)time(NULL) - ra
			: (long long)pc_metrics_uptime());
		/* S351: how far back every win_ figure reaches: all of them
		 * take their minute's snapshot in the same build, so a key
		 * whose value is the clock itself measures it - the uptime
		 * while that is under five minutes, 240-300 s after */
		{
			unsigned int ws = 0;

			(void)pc_win_delta("since.clock", get_ticks(), 1, &ws);
			pc_tw_key(out, "win_s");
			pc_tw_i64(out, ws);
		}
		pc_tw_end(out);
	}
	/* S89: the doors this daemon is serving, from the config it runs
	 * with.  The startup log says this once and is gone; "which ports is
	 * this thing actually serving" is a stats question a week later. */
	pc_tw_key(out, "listeners");
	pc_tw_arr(out);
	{
		int i, n = pc_listener_count();

		for (i = 0; i < n; i++) {
			const struct pc_listener *l = pc_listener_at(i);
			int plain = pc_listener_plaintext(l);
			int secret = l->http ? pc_http_token() != NULL
				: l->resp ? pc_resp_password_set() : !plain;

			pc_tw_map(out);
			pc_tw_key(out, "kind");
			pc_tw_str(out, l->type == PC_LISTEN_UNIX ? "unix"
				: l->http ? "http" : l->resp ? "resp" : "native");
			pc_tw_key(out, "addr");
			pc_tw_bulk(out, l->addr, strlen(l->addr));
			pc_tw_key(out, "port");
			pc_tw_i64(out, l->port);
			pc_tw_key(out, "plaintext");
			pc_tw_bool(out, (plain));
			pc_tw_key(out, "allow");
			pc_tw_i64(out, pc_listener_allow(l));
			pc_tw_key(out, "secret");
			pc_tw_bool(out, (secret));
			pc_tw_end(out);
		}
	}
	pc_tw_end(out);
	/* 0.6.2: the HTTP door's limits and what they did */
	{
		unsigned long long lim, hits, builds;

		pc_http_counters(&lim, &hits, &builds);
		pc_tw_key(out, "http");
		pc_tw_map(out);
		pc_tw_key(out, "rate");
		pc_tw_i64(out, pc_http_rate());
		pc_tw_key(out, "cache_ms");
		pc_tw_i64(out, pc_http_cache_ms());
		pc_tw_ctr(out, "http.rate_limited", "rate_limited", lim);
		pc_tw_ctr(out, "http.cache_hits", "cache_hits", hits);
		pc_tw_ctr(out, "http.builds", "builds", builds);
		pc_tw_end(out);
	}
}

/* S432: every DB index the RESP door answers - what it reaches, and who
 * put it there: a runtime map, an entry of resp_collections, or an open
 * door (no list) that reaches the collection of the index's own name.
 * stats' resp.map and the maps verb write the same rows. */
void respmap_rows(struct pc_tw *out)
{
	int i;

	pc_tw_arr(out);
	for (i = 0; i < PC_RESPMAP_MAX; i++) {
		char c[PC_COL_NAME_MAX];
		int src = pc_respmap_resolve(i, c, sizeof c);
		int there;

		if (!c[0])
			continue;
		there = pc_store_find(c, strlen(c)) != NULL;
		/* an open door answers every index as the collection of that
		 * name: only the ones that exist are worth a row */
		if (src == PC_RESPMAP_OPEN && !there)
			continue;
		pc_tw_map(out);
		pc_tw_key(out, "index");
		pc_tw_i64(out, i);
		pc_tw_key(out, "col");
		pc_tw_str(out, c);
		pc_tw_key(out, "source");
		pc_tw_str(out, src == PC_RESPMAP_RUNTIME ? "runtime"
			: src == PC_RESPMAP_CONFIG ? "config" : "open");
		pc_tw_key(out, "exists");
		pc_tw_bool(out, there);
		pc_tw_end(out);
	}
	pc_tw_end(out);
}

void pc_stats_tree(struct pc_tw *out, const char *only_col)
{
	int i;

	/* B1: the node's lifecycle state is a property of the NODE,
	 * not of the cluster - a node with no cluster still has one,
	 * and a readiness gate would have to answer for it too.  It
	 * sat inside the cluster block first, where an unclustered
	 * node could not report it at all. */
	/* S250: which build answered - standalone has no cluster code */
#ifdef PC_EDITION_STANDALONE
#define PC_EDITION_NAME "standalone"
#else
#define PC_EDITION_NAME "clustered"
#endif
	pc_tw_map(out);
	pc_tw_key(out, "version");
	pc_tw_str(out, PC_VERSION);
	pc_tw_key(out, "rev");
	pc_tw_str(out, PC_BUILD_REV);
	pc_tw_key(out, "edition");
	pc_tw_str(out, PC_EDITION_NAME);
	pc_tw_key(out, "state");
	pc_tw_str(out, pc_node_state_name(pc_node_state()));
	pc_tw_key(out, "state_reason");                              /* S186 */
	pc_tw_str(out, pc_node_reason_text(pc_node_reason()));
	stats_process(out);
	pc_tw_key(out, "memory");
	{
		unsigned long mt = 0, mu = 0, mf = 0;
		int mact = 0;

		pcache_arena_hugepage_capacity(&mact, &mt, &mu, &mf);
		/* These three describe the HUGE-PAGE arena, and the
		 * accessor zeroes all of them when that arena is not
		 * the backing in use - its header says @active must be
		 * checked before any of them is trusted.  Published as
		 * plain zeros they read as "no memory left" on a node
		 * that simply never measured, which is the opposite of
		 * the truth, so they go out as null instead: a consumer
		 * can forget to check a flag, but it cannot mistake
		 * null for a quantity.  arena_held, arena_max,
		 * arena_live and headroom_pct below are backing-
		 * independent and stay meaningful either way. */
		pc_tw_map(out);
		pc_tw_key(out, "arena_capacity_valid");
		pc_tw_bool(out, (mact));
		pc_tw_key(out, "arena_total");
		if (mact)
			pc_tw_i64(out, (long long)mt);
		else
			pc_tw_nil(out);
		pc_tw_key(out, "arena_used");
		if (mact)
			pc_tw_i64(out, (long long)mu);
		else
			pc_tw_nil(out);
		pc_tw_key(out, "arena_free");
		if (mact)
			pc_tw_i64(out, (long long)mf);
		else
			pc_tw_nil(out);
		/* S445: a retired index's slots, kept for the next index -
		 * neither used nor room for records (it read as used) */
		pc_tw_key(out, "arena_free_tables");
		if (mact)
			pc_tw_i64(out, (long long)pcache_arena_zone_free_bytes());
		else
			pc_tw_nil(out);
		pc_tw_key(out, "arena_live");
		pc_tw_i64(out, (long long)pcache_arena_live_bytes());
		/* arena_total/used/free above are HUGE-PAGE figures -
		 * they pin at the reservation while the process keeps
		 * growing, which is how a 64 MB arena reached 348 MB
		 * of RSS unnoticed.  These two are the whole node:
		 * what it holds from the host, and the ceiling it is
		 * allowed to hold. */
		pc_tw_key(out, "arena_held");
		pc_tw_i64(out, (long long)pcache_arena_held_bytes());
		pc_tw_key(out, "arena_max");
		pc_tw_i64(out, (long long)pcache_arena_max_bytes);
		/* S47: the pressure surface - what an operator alerts
		 * on BEFORE the arena starts refusing writes.  tier =
		 * the page backing actually in use; headroom_pct =
		 * how much of the ceiling is still free (0 at the
		 * cliff); nomem = writes already refused arena-full;
		 * reclaim = the give-back machinery's own counters. */
		{
			struct pcache_arena_pressure pr;
			unsigned long held = pcache_arena_held_bytes();
			unsigned long mx = pcache_arena_max_bytes;
			int hpct = mx ? (mx > held ?
				(int)(100ULL * (mx - held) / mx) : 0)
				: 100;

			pcache_arena_pressure(&pr);
			pc_tw_key(out, "tier");
			pc_tw_bulk(out, pr.tier, strlen(pr.tier));
			pc_tw_key(out, "headroom_pct");
			pc_tw_i64(out, hpct);
			pc_tw_ctr(out, "memory.nomem", "nomem", (unsigned long long)(pr.refused));
			/* S244: of nomem, the overwrites that fit the cell
			 * they replaced and landed anyway; and older copies
			 * dropped because the newer one could not land */
			pc_tw_key(out, "nomem_inplace");
			pc_tw_i64(out, (long long)__atomic_load_n(
				&pcache_nomem_inplace, __ATOMIC_RELAXED));
			pc_tw_key(out, "nomem_stale_dropped");
			pc_tw_i64(out, (long long)__atomic_load_n(
				&pcache_nomem_dropped, __ATOMIC_RELAXED));
			pc_tw_key(out, "pool_empty");
			pc_tw_i64(out, (long long)pr.pool_empty);
			/* S166: the groups committed ahead of the frontier, and
			 * the ones a carve had to commit on the write path */
			pc_tw_key(out, "commits_ahead");
			pc_tw_i64(out, (long long)pr.commits_ahead);
			pc_tw_key(out, "commits_inline");
			pc_tw_i64(out, (long long)pr.commits_inline);
			pc_tw_key(out, "commits_index");
			pc_tw_i64(out, (long long)pr.commits_index);
			/* S167: what is really locked, and whether a group
			 * could not be locked after a pinned start */
			pc_tw_key(out, "locked_bytes");
			pc_tw_i64(out, (long long)pr.locked_bytes);
			pc_tw_key(out, "pin_lost");
			pc_tw_i64(out, (long long)pr.pin_lost);
			/* S114: what held is made of - the index regions (never
			 * freed) and the free slots kept resident */
			pc_tw_key(out, "arena_regions");
			pc_tw_i64(out, (long long)pr.regions_bytes);
			pc_tw_key(out, "arena_warm_free");
			pc_tw_i64(out, (long long)pr.warm_free_bytes);
			/* S150 C: retired index slots, resident and punched, and
			 * how the carve has been served */
			pc_tw_key(out, "arena_regions_free_warm");
			pc_tw_i64(out, (long long)pr.regions_free_warm_bytes);
			pc_tw_key(out, "arena_regions_free_cold");
			pc_tw_i64(out, (long long)pr.regions_free_cold_bytes);
			pc_tw_key(out, "arena_regions_retired");
			pc_tw_i64(out, (long long)pr.regions_retired);
			pc_tw_key(out, "arena_region_reuse");
			pc_tw_i64(out, (long long)pr.region_reuse);
			pc_tw_key(out, "arena_regions_raised");         /* S445 */
			pc_tw_i64(out, (long long)pr.regions_raised);
			/* S118: the chunks the size classes own and the shm pages'
			 * alignment slots - with the two above, held exactly */
			pc_tw_key(out, "arena_sparse_chunks");    /* S190 */
			pc_tw_i64(out, (long long)pr.sparse_chunks);
			pc_tw_key(out, "arena_evacuated");
			pc_tw_i64(out, (long long)pr.evacuated);
			pc_tw_key(out, "arena_class_chunks");
			pc_tw_i64(out, (long long)pr.class_chunk_bytes);
			pc_tw_key(out, "arena_page_slack");
			pc_tw_i64(out, (long long)pr.page_slack_bytes);
			pc_tw_key(out, "arena_committed");
			pc_tw_i64(out, (long long)pr.committed_bytes);
			pc_tw_key(out, "arena_reserved");
			pc_tw_i64(out, (long long)pr.reserved_bytes);
			pc_tw_key(out, "at_ceiling");
			pc_tw_bool(out, (pr.at_ceiling_since));
			pc_tw_key(out, "at_ceiling_since");
			pc_tw_i64(out, (long long)pr.at_ceiling_since);
			pc_tw_key(out, "reclaim");
			pc_tw_map(out);
			pc_tw_key(out, "retired");
			pc_tw_i64(out, (long long)pr.retired);
			pc_tw_key(out, "pages_freed");
			pc_tw_i64(out, (long long)pr.pages_freed);
			pc_tw_key(out, "released_bytes");
			pc_tw_i64(out, (long long)pr.released_bytes);
			/* S119: how much of that was the never-carved tail */
			pc_tw_key(out, "tail_released");
			pc_tw_i64(out, (long long)pr.tail_released_bytes);
			pc_tw_key(out, "cold_bytes");
			pc_tw_i64(out, (long long)pr.cold_bytes);
			pc_tw_key(out, "punch_calls");
			pc_tw_i64(out, (long long)pr.punch_calls);
			pc_tw_key(out, "punch_groups");
			pc_tw_i64(out, (long long)pr.punch_groups);
			pc_tw_key(out, "shrink_step_bytes");
			pc_tw_i64(out, (long long)pr.shrink_step);
			pc_tw_key(out, "flushes");
			pc_tw_i64(out, (long long)pr.flushes);
			pc_tw_key(out, "giveback_off");
			pc_tw_bool(out, (pr.giveback_off));
			pc_tw_end(out);
		}
		pc_tw_end(out);
	}
	/* S120: the budget - what the collections cost, from what the daemon
	 * knows exactly: the index regions each table was carved (noted at
	 * creation and growth) and the records as the cells they occupy (the
	 * walk rounds every record to its class), beside the ceiling and the
	 * reservation.  The README's sizing recipe is the same arithmetic. */
	{
		unsigned long long bi = 0, br = 0;
		unsigned long bt = 0, bu = 0, bf = 0;
		int c, nc = pc_store_count(), ba = 0;

		for (c = 0; c < nc; c++) {
			bi += pc_store_index_bytes(c);
			br += pc_store_held_cells(c);
		}
		pcache_arena_hugepage_capacity(&ba, &bt, &bu, &bf);
		pc_tw_key(out, "budget");
		pc_tw_map(out);
		pc_tw_key(out, "index");
		pc_tw_i64(out, (long long)bi);
		pc_tw_key(out, "records");
		pc_tw_i64(out, (long long)br);
		pc_tw_key(out, "held");
		pc_tw_i64(out, (long long)pcache_arena_held_bytes());
		pc_tw_key(out, "ceiling");
		pc_tw_i64(out, (long long)pcache_arena_max_bytes);
		pc_tw_key(out, "reservation");
		pc_tw_i64(out, (long long)bt);
		pc_tw_end(out);
	}
	/* RESP listeners (S33) live OUTSIDE the cluster block: a
	 * RESP listener works on a standalone daemon, and burying
	 * its counters in "cluster" made them null exactly where a
	 * single-node operator would look for them. */
	/* S161: the client limit and what it refused; HTTP is outside it */
	pc_tw_key(out, "clients");
	pc_tw_map(out);
	pc_tw_key(out, "open");
	pc_tw_i64(out, (long long)PC_RESP_READ(pc_clients_open));
	pc_tw_key(out, "max");
	pc_tw_i64(out, pc_max_clients);
	pc_tw_ctr(out, "clients.refused", "refused", (unsigned long long)(__atomic_load_n(&pc_clients_refused,
		__ATOMIC_RELAXED)));
	/* S366: what the limit was derived from, the memory guard, and the
	 * idle connections' buffers */
	pc_tw_key(out, "max_basis");
	pc_tw_str(out, pc_max_clients_basis);
	{
		struct pc_memguard_stats mg;

		pc_memguard_stats(&mg);
		pc_tw_key(out, "memory");
		pc_tw_map(out);
		pc_tw_key(out, "guard");
		pc_tw_bool(out, mg.enabled);
		pc_tw_key(out, "source");
		pc_tw_str(out, mg.source);
		pc_tw_key(out, "limit");
		pc_tw_i64(out, (long long)mg.limit);
		pc_tw_key(out, "available");
		pc_tw_i64(out, (long long)mg.available);
		pc_tw_key(out, "floor");
		pc_tw_i64(out, (long long)mg.floor);
		pc_tw_key(out, "arena_reserve");
		pc_tw_i64(out, (long long)mg.arena_reserve);
		pc_tw_key(out, "hugetlb_pool");            /* ST1 */
		pc_tw_i64(out, (long long)mg.hugetlb_pool);
		pc_tw_key(out, "hugetlb_pool_free");
		pc_tw_i64(out, (long long)mg.hugetlb_pool_free);
		pc_tw_key(out, "room");
		pc_tw_i64(out, mg.room);
		pc_tw_key(out, "budget");
		pc_tw_i64(out, mg.budget);
		pc_tw_key(out, "refusing");
		pc_tw_bool(out, mg.refusing);
		pc_tw_key(out, "refusing_since");
		pc_tw_i64(out, (long long)mg.refusing_since);
		pc_tw_ctr(out, "clients.memory.refused", "refused", mg.refused);
		pc_tw_ctr(out, "clients.memory.engaged", "engaged", mg.engaged);
		pc_tw_key(out, "max_clients");
		pc_tw_i64(out, mg.max_clients_memory);
		pc_tw_end(out);
		pc_tw_key(out, "buffers");
		pc_tw_map(out);
		pc_tw_key(out, "held");
		pc_tw_i64(out, pc_conn_bufs_held_all());
		pc_tw_ctr(out, "clients.buffers.idle_released", "idle_released",
			mg.idle_released);
		pc_tw_ctr(out, "clients.buffers.freed_bytes", "freed_bytes",
			mg.freed_bytes);
		pc_tw_key(out, "trim");
		pc_tw_bool(out, mg.trim_supported);
		pc_tw_ctr(out, "clients.buffers.trims", "trims", mg.trims);
		pc_tw_key(out, "trim_last_bytes");
		pc_tw_i64(out, mg.trim_last_bytes);
		pc_tw_end(out);
		/* S372: connections whose replies outran their socket */
		{
			struct pc_out_pause_stats op;

			pc_conn_out_pause_stats(&op);
			pc_tw_key(out, "output");
			pc_tw_map(out);
			pc_tw_key(out, "paused");
			pc_tw_i64(out, (long long)op.paused_now);
			pc_tw_key(out, "pause_at");
			pc_tw_i64(out, PC_OUT_PAUSE_AT);
			pc_tw_key(out, "resume_at");
			pc_tw_i64(out, PC_OUT_RESUME_AT);
			pc_tw_key(out, "stall_s");
			pc_tw_i64(out, pc_output_stall_s);
			pc_tw_ctr(out, "clients.output.pauses", "pauses", op.pauses);
			pc_tw_ctr(out, "clients.output.stall_kills", "stall_kills",
				op.stall_kills);
			pc_tw_end(out);
		}
		/* ST5: Noise handshakes a worker turn put off past its cap */
		pc_tw_key(out, "handshakes");
		pc_tw_map(out);
		pc_tw_key(out, "per_turn");
		pc_tw_i64(out, PC_HS_PER_TURN);
		pc_tw_ctr(out, "clients.handshakes.deferred", "deferred",
			pc_conn_hs_deferred_total());
		pc_tw_end(out);
	}
	pc_tw_end(out);
	{
		struct pc_pubsub_stats ps;   /* PS1/PS3 */

		pc_pubsub_stats(&ps);
		pc_tw_key(out, "pubsub");
		pc_tw_map(out);
		pc_tw_key(out, "channels");
		pc_tw_i64(out, ps.channels);
		pc_tw_key(out, "patterns");
		pc_tw_i64(out, ps.patterns);
		pc_tw_key(out, "subscribers");
		pc_tw_i64(out, ps.subscribers);
		pc_tw_ctr(out, "pubsub.published", "published", (unsigned long long)(ps.published));
		pc_tw_ctr(out, "pubsub.delivered", "delivered", (unsigned long long)(ps.delivered));
		pc_tw_ctr(out, "pubsub.slow_kills", "slow_kills", (unsigned long long)(ps.slow_kills));
		pc_tw_ctr(out, "pubsub.relay_sent", "relay_sent", (unsigned long long)(ps.relay_sent));
		pc_tw_ctr(out, "pubsub.relay_recv", "relay_recv", (unsigned long long)(ps.relay_recv));
		pc_tw_ctr(out, "pubsub.relay_lost", "relay_lost", (unsigned long long)(ps.relay_lost));
		pc_tw_ctr(out, "pubsub.relay_dropped", "relay_dropped", (unsigned long long)(ps.relay_dropped));
		pc_tw_ctr(out, "pubsub.alloc_failed", "alloc_failed", (unsigned long long)(ps.alloc_failed));
		pc_tw_ctr(out, "pubsub.relay_duplicates", "relay_duplicates", (unsigned long long)(ps.relay_duplicates));
		pc_tw_ctr(out, "pubsub.queue_dropped", "queue_dropped", (unsigned long long)(ps.queue_dropped));
		pc_tw_key(out, "queue_bytes");
		pc_tw_i64(out, (long long)ps.queue_bytes);
		pc_tw_ctr(out, "pubsub.publish_paused", "publish_paused", (unsigned long long)(ps.publish_paused));
		{
			/* PS5: pushes over UDP */
			struct pc_udp_figures uf;

			pc_conn_udp_figures(&uf);
			pc_tw_key(out, "udp");
			pc_tw_map(out);
			pc_tw_key(out, "streams");
			pc_tw_i64(out, uf.streams);
			pc_tw_key(out, "probes");
			pc_tw_i64(out, (long long)uf.probes);
			pc_tw_key(out, "confirmed");
			pc_tw_i64(out, (long long)uf.confirmed);
			pc_tw_key(out, "expired");
			pc_tw_i64(out, (long long)uf.expired);
			pc_tw_key(out, "pushed");
			pc_tw_i64(out, (long long)uf.pushed);
			pc_tw_key(out, "oversized_to_tcp");
			pc_tw_i64(out, (long long)uf.oversized);
			pc_tw_key(out, "send_errors");
			pc_tw_i64(out, (long long)uf.send_errors);
			pc_tw_key(out, "acks");
			pc_tw_i64(out, (long long)uf.acks);
			pc_tw_ctr(out, "pubsub.pruned_no_ack", "pruned_no_ack", (unsigned long long)(uf.pruned_no_ack));
			pc_tw_ctr(out, "pubsub.pruned_no_progress", "pruned_no_progress", (unsigned long long)(uf.pruned_no_progress));
			pc_tw_end(out);
		}
		{
			/* PS11: the dedicated relay plane */
			int rport, rthreads, rdirect;
			unsigned long long sd, sc, rx;

			pc_cluster_pubsub_relay_figures(&rport, &rthreads, &rdirect,
				&sd, &sc, &rx);
			pc_tw_key(out, "relay_port");
			pc_tw_i64(out, rport);
			pc_tw_key(out, "relay_rx_threads");
			pc_tw_i64(out, rthreads);
			pc_tw_key(out, "relay_peers_direct");
			pc_tw_i64(out, rdirect);
			pc_tw_key(out, "relay_sent_direct");
			pc_tw_i64(out, (long long)sd);
			pc_tw_key(out, "relay_sent_cluster");
			pc_tw_i64(out, (long long)sc);
			pc_tw_key(out, "relay_rx_datagrams");
			pc_tw_i64(out, (long long)rx);
		}
		{
			/* PS12: relaying only what a peer wants */
			struct pc_psint_figures fi;

			pc_cluster_pubsub_interest_figures(&fi);
			if (fi.version) { pc_tw_key(out, "relay_mode"); pc_tw_str(out, "interested"); } else { pc_tw_key(out, "relay_mode"); pc_tw_str(out, "all"); }
			{
				/* hex: [epoch 4][rebuild 4][adds 8] read off
				 * directly, and no sign to lose */
				char hx[20];
				int hn = snprintf(hx, sizeof hx, "%016llx",
					(unsigned long long)fi.version);

				pc_tw_key(out, "interest_version");
				pc_tw_bulk(out, hx, (size_t)hn);
			}
			pc_tw_ctr(out, "pubsub.relay_skipped", "relay_skipped", (unsigned long long)(fi.skipped));
			pc_tw_key(out, "relay_peers_filtered");
			pc_tw_i64(out, fi.peers_filtered);
			pc_tw_key(out, "relay_peers_broadcast");
			pc_tw_i64(out, fi.peers_broadcast);
			pc_tw_key(out, "interest_updates_sent");
			pc_tw_i64(out, (long long)fi.updates_sent);
			pc_tw_key(out, "interest_updates_received");
			pc_tw_i64(out, (long long)fi.updates_received);
			pc_tw_key(out, "interest_resyncs_sent");
			pc_tw_i64(out, (long long)fi.resyncs_sent);
			pc_tw_key(out, "interest_resyncs_received");
			pc_tw_i64(out, (long long)fi.resyncs_received);
			pc_tw_key(out, "interest_requests_sent");
			pc_tw_i64(out, (long long)fi.requests_sent);
		}
		pc_tw_ctr(out, "pubsub.keyspace_events", "keyspace_events", (unsigned long long)(ps.keyspace));
		pc_tw_end(out);
	}
	/* S159: the command rows and the slow log, off the RESP door - what
	 * INFO commandstats / latencystats and SLOWLOG GET answer there */
	pc_tw_key(out, "commands");
	pc_obs_commands_tree(out);
	pc_tw_key(out, "slowlog");
	pc_obs_slowlog_tree(out, 32);
	unknown_tree(out);                     /* S165 */
	pc_alerts_stats_tree(out);             /* S352b: active + the last 100 */
	{                                      /* 12nw: config reloads */
		unsigned long long rok, rref;
		long long rlast;
		const char *rlm;

		pc_reload_stats(&rok, &rref, &rlast, &rlm);
		pc_tw_key(out, "reload");
		pc_tw_map(out);
		pc_tw_key(out, "applied");
		pc_tw_i64(out, (long long)rok);
		pc_tw_key(out, "refused");
		pc_tw_i64(out, (long long)rref);
		pc_tw_key(out, "last_unix");
		pc_tw_i64(out, rlast);
		pc_tw_key(out, "last");
		pc_tw_str(out, rlm);
		pc_tw_end(out);
	}
	/* 0.6.0 step 1 (DESIGN 12li): system calls against what they moved -
	 * the workers' epoll waits, events and client reads/writes, and the
	 * cluster plane's and the pub/sub relay's UDP syscalls against their
	 * datagrams (one each until S364 batches them) */
	{
		static const char *const plane[PC_UDP_N] = { "cluster_udp", "relay_udp" };
		static const char *const wk[PC_UDP_N][4] = {
			{ "io.cl.tx_syscalls", "io.cl.tx_datagrams",
			  "io.cl.rx_syscalls", "io.cl.rx_datagrams" },
			{ "io.ps.tx_syscalls", "io.ps.tx_datagrams",
			  "io.ps.rx_syscalls", "io.ps.rx_datagrams" } };
		struct pc_iostat io;
		int u;

		pc_iostat_get(&io, 1);
		pc_tw_key(out, "io");
		pc_tw_map(out);
		pc_tw_key(out, "backend");                 /* S361 */
		pc_tw_map(out);
		pc_tw_key(out, "configured");
		pc_tw_str(out, pc_io_backend_name(pc_io_backend.configured));
		pc_tw_key(out, "active");
		pc_tw_str(out, pc_io_backend_name(pc_io_backend.active));
		pc_tw_key(out, "buffers");
		pc_tw_str(out, pc_ur_buf_tier_name(pc_io_backend.buf_tier));
		pc_tw_key(out, "workers_uring");
		pc_tw_i64(out, __atomic_load_n(&pc_io_backend.workers_uring,
			__ATOMIC_RELAXED));
		pc_tw_key(out, "fallback");
		pc_tw_str(out, pc_io_backend.why);
		pc_tw_key(out, "counts_iowait");          /* 0.6.0.1 */
		pc_tw_bool(out, pc_io_backend.active == PC_IO_URING &&
			pc_io_backend.counts_iowait);
		pc_tw_end(out);
		{                                         /* 12np: the IPv6 guard */
			const char *why;
			int v6 = pc_ipv6_usable(&why);

			pc_tw_key(out, "ipv6");
			pc_tw_map(out);
			pc_tw_key(out, "usable");
			pc_tw_bool(out, v6);
			pc_tw_key(out, "why");
			pc_tw_str(out, why);
			pc_tw_end(out);
		}
		pc_tw_key(out, "busy_poll");               /* S363 */
		pc_tw_map(out);
		pc_tw_key(out, "us");
		pc_tw_i64(out, pc_io_backend.bp_us);
		pc_tw_key(out, "prefer");
		pc_tw_bool(out, pc_io_backend.bp_prefer);
		pc_tw_key(out, "workers");
		pc_tw_i64(out, __atomic_load_n(&pc_io_backend.bp_workers,
			__ATOMIC_RELAXED));
		pc_tw_key(out, "refused");
		pc_tw_str(out, pc_io_backend.bp_why);
		pc_tw_end(out);
		/* S379: under io = epoll, whether io_uring would run here */
		pc_tw_key(out, "uring_usable");
		if (pc_io_backend.configured == PC_IO_EPOLL)
			pc_tw_bool(out, pc_io_backend.uring_ok);
		else
			pc_tw_nil(out);
		/* S379: each worker's share of time outside its wait, over the
		 * last minute and five (null before two samples) */
		pc_tw_key(out, "workers");
		pc_tw_arr(out);
		for (u = 0; u < pc_io_backend.workers; u++) {
			int p1 = pc_iobusy_pct(u, 60), p5 = pc_iobusy_pct(u, 300);

			pc_tw_map(out);
			pc_tw_key(out, "busy_pct_1m");
			if (p1 >= 0)
				pc_tw_i64(out, p1);
			else
				pc_tw_nil(out);
			pc_tw_key(out, "busy_pct_5m");
			if (p5 >= 0)
				pc_tw_i64(out, p5);
			else
				pc_tw_nil(out);
			pc_tw_end(out);
		}
		pc_tw_end(out);
		pc_tw_ctr(out, "io.epoll_waits", "epoll_waits", io.ep_waits);
		pc_tw_ctr(out, "io.epoll_events", "epoll_events", io.ep_events);
		pc_tw_ctr(out, "io.reads", "reads", io.reads);
		pc_tw_ctr(out, "io.writes", "writes", io.writes);
		pc_tw_ctr(out, "io.uring_enters", "uring_enters", io.ur_enters);   /* S361 */
		pc_tw_ctr(out, "io.uring_completions", "uring_completions", io.ur_cqes);
		pc_tw_ctr(out, "io.uring_receives", "uring_receives", io.ur_recvs);
		pc_tw_ctr(out, "io.uring_sends", "uring_sends", io.ur_sends);
		for (u = 0; u < PC_UDP_N; u++) {
			pc_tw_key(out, plane[u]);
			pc_tw_map(out);
			pc_tw_ctr(out, wk[u][0], "tx_syscalls", io.tx_calls[u]);
			pc_tw_ctr(out, wk[u][1], "tx_datagrams", io.tx_dgrams[u]);
			pc_tw_ctr(out, wk[u][2], "rx_syscalls", io.rx_calls[u]);
			pc_tw_ctr(out, wk[u][3], "rx_datagrams", io.rx_dgrams[u]);
			pc_tw_end(out);
		}
		pc_tw_end(out);
	}
	/* S352: the alert mailer - whether this node can send, and how it
	 * went; never the server's credentials */
	{
		struct pc_mail_stats ms;

		pc_mail_get_stats(&ms);
		pc_tw_key(out, "mail");
		pc_tw_map(out);
		pc_tw_key(out, "built");
		pc_tw_bool(out, pc_mail_built());
		pc_tw_key(out, "ready");
		pc_tw_bool(out, pc_mail_ready());
		pc_tw_ctr(out, "mail.sent", "sent", ms.sent);
		pc_tw_ctr(out, "mail.failed", "failed", ms.failed);
		pc_tw_ctr(out, "mail.dropped", "dropped", ms.dropped);
		/* S352c: alerts relayed - mailed here for another node, handed
		 * from here to the elected mailer, and lost on the way */
		{
			unsigned long long ri, ro, rd;

			pc_alerts_relay_stats(&ri, &ro, &rd);
			pc_tw_ctr(out, "mail.relayed_in", "relayed_in", ri);
			pc_tw_ctr(out, "mail.relayed_out", "relayed_out", ro);
			pc_tw_ctr(out, "mail.relay_dropped", "relay_dropped", rd);
		}
		pc_tw_key(out, "queued");
		pc_tw_i64(out, (long long)ms.queued_now);
		pc_tw_key(out, "last_error");
		pc_tw_str(out, ms.last_error);
		pc_tw_key(out, "last_ok_ms");
		pc_tw_i64(out, ms.last_ok_ms);
		pc_tw_key(out, "last_error_ms");
		pc_tw_i64(out, ms.last_err_ms);
		pc_tw_end(out);
	}
	pc_tw_key(out, "resp");
	pc_tw_map(out);
	pc_tw_ctr(out, "resp.conns", "conns", (unsigned long long)(PC_DOOR_SINCE(resp_conns)));
	pc_tw_key(out, "open");
	pc_tw_i64(out, (long long)PC_RESP_READ(pc_resp_open));
	pc_tw_ctr(out, "resp.rejected", "rejected", (unsigned long long)(PC_DOOR_SINCE(resp_rejected)));
	pc_tw_ctr(out, "resp.authfail", "authfail", (unsigned long long)(PC_DOOR_SINCE(resp_authfail)));
	/* S129: failed `enable` attempts.  Beside authfail because it is
	 * the same question one layer up - somebody offering a secret that
	 * did not match - and an operator watching for a privilege secret
	 * being guessed looks here. */
	pc_tw_key(out, "enable_fails");
	pc_tw_i64(out, (long long)pc_verb_enable_fails());
	pc_tw_key(out, "enable_lockouts");                 /* S417 */
	pc_tw_i64(out, (long long)pc_verb_enable_lockouts());
	pc_tw_key(out, "enable_refused");
	pc_tw_i64(out, (long long)pc_verb_enable_refused());
	pc_tw_ctr(out, "resp.requests", "requests", (unsigned long long)(PC_DOOR_SINCE(resp_reqs)));
	pc_tw_key(out, "slots_hits");
	pc_tw_i64(out, PC_DOOR_SINCE(resp_slots_hits));
	pc_tw_key(out, "slots_builds");
	pc_tw_i64(out, PC_DOOR_SINCE(resp_slots_builds));
	/* S292: cooperative KEYS walks finished, and the turns they took -
	 * turns per walk is what a KEYS caller waits on under load */
	{
		unsigned long long kw, kt;

		pc_enum_figures(&kw, &kt);
		pc_tw_key(out, "keys_walks");
		pc_tw_i64(out, (long long)kw);
		pc_tw_key(out, "keys_turns");
		pc_tw_i64(out, (long long)kt);
	}
	/* S432: the DB-index map as it holds now, and how many indexes a
	 * runtime map (perfcli map) moved off what resp_collections says */
	pc_tw_key(out, "map");
	respmap_rows(out);
	pc_tw_key(out, "map_overrides");
	pc_tw_i64(out, pc_respmap_overrides());
	pc_tw_end(out);
	/* S76: the native door, per dialect.  A connection is counted once
	 * when its first byte settles the dialect; every consumed request
	 * after that counts against it.  resp here is RESP spoken to the
	 * native port - the dedicated RESP door is the block above.  (The
	 * json block left with the JSON-RPC dialect, S317.) */
	pc_tw_key(out, "native");
	pc_tw_map(out);
	/* S365: windowed too - the page's connects/s sums both doors */
	pc_tw_ctr(out, "native.conns", "conns", (unsigned long long)(PC_DOOR_SINCE(nat_conns)));
	pc_tw_key(out, "binary");
	pc_tw_map(out);
	pc_tw_ctr(out, "native.binary.conns", "conns", (unsigned long long)(PC_DOOR_SINCE(nat_bin_conns)));
	pc_tw_key(out, "open");
	pc_tw_i64(out, (long long)PC_RESP_READ(pc_nat_bin_open));
	pc_tw_ctr(out, "native.binary.requests", "requests", (unsigned long long)(PC_DOOR_SINCE(nat_bin_reqs)));
	pc_tw_end(out);
	pc_tw_key(out, "resp");
	pc_tw_map(out);
	pc_tw_ctr(out, "native.resp.conns", "conns", (unsigned long long)(PC_DOOR_SINCE(nat_resp_conns)));
	pc_tw_key(out, "open");
	pc_tw_i64(out, (long long)PC_RESP_READ(pc_nat_resp_open));
	pc_tw_ctr(out, "native.resp.requests", "requests", (unsigned long long)(PC_DOOR_SINCE(nat_resp_reqs)));
	pc_tw_end(out);
	pc_tw_end(out);
	pc_tw_key(out, "collections");
	pc_tw_arr(out);
	for (i = 0; i < pc_store_count(); i++) {
		if (!pc_store_live(i))
			continue;   /* S69: a dropped collection */
		pcache_ht_totals_t tot;

		if (only_col && strcmp(only_col, pc_store_name(i)))
			continue;
		pcache_ht_totals(pc_store_ht(i), &tot);
		pc_tw_map(out);
		pc_tw_key(out, "name");
		pc_tw_bulk(out, pc_store_name(i), strlen(pc_store_name(i)));
		{
			/* S163: the name's hash, which is how the map's per-member
			 * collection figures are joined to this row */
			char hx[24];

			snprintf(hx, sizeof hx, "%016llx", (unsigned long long)
				clmemb_col_hash(pc_store_name(i)));
			pc_tw_key(out, "hash");
			pc_tw_str(out, hx);
		}
		/* the mode is the first thing anyone needs to know
		 * about a collection - and until now the only way to
		 * find it was to read the node's config file */
		/* eager is legal only alongside store, so it is not a
		 * property hanging off the mode - it IS the mode, and
		 * reporting "store" for an eager collection drops the
		 * half an operator most needs. */
		pc_tw_key(out, "mode");
		/* S127: spread SETS the eager flag, because it reuses eager's
		 * push machinery aimed at K holders - so a spread collection
		 * reported "eager" here and the dashboard, which derives its
		 * label from this, told an operator "all in eager mode" about
		 * a fleet that is not.  Ask the cluster first. */
		pc_tw_str(out, pc_cluster_replicas() ? "spread"
			: pc_store_shard_enabled(pc_store_ht(i))
			? "shard" : pc_store_proxy_enabled(pc_store_ht(i))
			? "proxy" : pc_store_eager_enabled(pc_store_ht(i))
			? "eager" : "store");
		/* G5: false for a cache collection (durable = no) */
		pc_tw_key(out, "durable");
		pc_tw_bool(out, pc_store_durable_idx(i));
		pc_tw_key(out, "entries");
		pc_tw_i64(out, (long long)tot.entries);
		pc_tw_key(out, "buckets");
		pc_tw_i64(out, pcache_ht_nbuckets(pc_store_ht(i)));
		/* S131: records in the overflow leg, and what the last held
		 * walk cost.  A table at its target load factor keeps the leg
		 * near empty; one that has stopped growing puts everything
		 * there, on a chain per hash bucket under ONE lock - and the
		 * walk that crosses it blocks every other maintenance duty
		 * for its whole duration, the splitter included.  Neither
		 * figure was reported anywhere, so a table at 81 entries per
		 * bucket looked like a table at 4. */
		/* S180: slots per bucket, so a reader can work out the LOAD
		 * FACTOR - entries against buckets x slots.  The status page
		 * divided by buckets alone and reported a table at half its
		 * capacity as "300%", which is the opposite of the signal an
		 * operator needs from that column. */
		pc_tw_key(out, "slots");
		pc_tw_i64(out, PCACHE_SLOTS);
		pc_tw_key(out, "overflow");
		pc_tw_i64(out, pcache_ht_overflow(pc_store_ht(i)));
		/* S239: how it is sized, and what AUTO would size it to (log2
		 * buckets, 0 = leave it) - the page shows amber for a WARN
		 * collection auto would shrink, red for keys in the leg while
		 * growth is blocked */
		pc_tw_key(out, "autoscale");
		pc_tw_str(out, pc_autoscale_name(pc_store_autoscale(i)));
		/* S436: set at runtime (the verb's, which is the fleet's), and
		 * its generation - the same on every member once the resync has
		 * run; false: the config's mode.  0 with true: set by a build
		 * before 0.7.2, which stamped none */
		pc_tw_key(out, "autoscale_set");
		pc_tw_bool(out, pc_store_autoscale_isset(i));
		pc_tw_key(out, "autoscale_gen");
		pc_tw_i64(out, (long long)pc_store_autoscale_gen(i));
		pc_tw_key(out, "autoscale_target_log2");
		pc_tw_i64(out, pc_store_autoscale_target(i));
		/* S328: the size FLOOR mode keeps it at or above (published in
		 * every mode: it is what the operator gave it) */
		pc_tw_key(out, "autoscale_floor_log2");
		pc_tw_i64(out, pc_store_floor_log2(i));
		/* S172: what the maintenance tick has put BACK into the
		 * table - a leg that stays flat while this climbs is a
		 * table still filling it faster than the drain empties it */
		pc_tw_key(out, "leg_drained");
		pc_tw_i64(out, (long long)pcache_ht_drained(pc_store_ht(i)));
		/* S175: what the last COMPLETE drain pass could not put back
		 * - their buckets are full, so only a wider table can take
		 * them - and the splits that observation has asked for.
		 * leg_stuck falling to nothing while leg_splits climbs is
		 * the signal doing its job; leg_stuck holding steady with
		 * leg_splits flat means the floor has been reached and the
		 * remainder is the load factor the operator chose. */
		pc_tw_key(out, "leg_stuck");
		pc_tw_i64(out, pcache_ht_leg_stuck(pc_store_ht(i)));
		pc_tw_key(out, "leg_splits");
		pc_tw_i64(out, (long long)pcache_ht_leg_splits(pc_store_ht(i)));
		/* S331: the read-through upstream, for a collection with one */
		pc_upstream_stats_tree(out, pc_store_name(i),
			strlen(pc_store_name(i)));
		pc_tw_key(out, "held_walk_us");
		pc_tw_i64(out, (long long)pc_store_held_walk_us(i));
		{
			int rs_t = 0;
			unsigned long long rs_m = 0;

			if (pc_store_resizing(i, &rs_t, &rs_m)) {   /* S69 */
				pc_tw_key(out, "resizing_to");
				pc_tw_i64(out, 1LL << rs_t);
				pc_tw_key(out, "resize_moved");
				pc_tw_i64(out, (long long)rs_m);
			}
		}
		pc_tw_ctr_col(out, pc_store_name(i), strlen(pc_store_name(i)), "", "hits",
			(unsigned long long)(tot.hits));
		pc_tw_ctr_col(out, pc_store_name(i), strlen(pc_store_name(i)), "", "misses",
			(unsigned long long)(tot.misses));
		/* S148: hits/misses count LOOKUPS, so one hot key pulled hard
		 * reads as a perfect cache beside thousands of records nobody
		 * wants.  `reach` is how many DISTINCT keys were actually
		 * served in the last closed window - read against `entries`,
		 * it is the half the ratio cannot express.  Approximate by
		 * design (a 64-register HLL, ~13%); the window is the
		 * daemon's, because a sketch cannot be differenced the way
		 * the page differences counters. */
		{
			unsigned int rw = 0;
			unsigned long rk = pcache_ht_reach(pc_store_ht(i), &rw);

			pc_tw_key(out, "reach");
			pc_tw_i64(out, (long long)rk);
			pc_tw_key(out, "reach_window_s");
			pc_tw_i64(out, (long long)rw);
		}
		/* S151: the same three over CLIENT worker slots only.  hits,
		 * misses and reach above count every origin: a peer serving a
		 * pull lands there (12du), and so does recovery's probe of each
		 * replayed key.  These are what an operator means by the words,
		 * and what the status page now reads. */
		{
			pcache_ht_totals_t ct;
			unsigned int rw2 = 0;
			unsigned long rc2 = pcache_ht_reach_client(pc_store_ht(i), &rw2);

			pcache_ht_totals_client(pc_store_ht(i), &ct);
			pc_tw_ctr_col(out, pc_store_name(i), strlen(pc_store_name(i)), "", "hits_client",
				(unsigned long long)(ct.hits));
			pc_tw_ctr_col(out, pc_store_name(i), strlen(pc_store_name(i)), "", "misses_client",
				(unsigned long long)(ct.misses));
			pc_tw_key(out, "reach_client");
			pc_tw_i64(out, (long long)rc2);
			/* S164: a replicated apply, a kept pull and a recovery
			 * replay all store, so `stores` puts one eager client
			 * write on every member.  These are the writes clients
			 * made HERE - this node's share of the fleet figure. */
			pc_tw_ctr_col(out, pc_store_name(i), strlen(pc_store_name(i)), "", "stores_client",
				(unsigned long long)(ct.stores));
			pc_tw_ctr_col(out, pc_store_name(i), strlen(pc_store_name(i)), "", "removes_client",
				(unsigned long long)(ct.removes));
		}
		pc_tw_ctr_col(out, pc_store_name(i), strlen(pc_store_name(i)), "", "stores",
			(unsigned long long)(tot.stores));
		pc_tw_ctr_col(out, pc_store_name(i), strlen(pc_store_name(i)), "", "removes",
			(unsigned long long)(tot.removes));
		pc_tw_ctr_col(out, pc_store_name(i), strlen(pc_store_name(i)), "", "expired",
			(unsigned long long)(tot.expired));
		/* S164: the collection for the whole fleet, computed here so the
		 * page, perfcli and any scraper read one answer.  Held copies
		 * follow the mode (eager: the fullest member; spread: the copies
		 * over K; store: the fullest member as a lower bound; shard and
		 * proxy: the sum); client events sum.  Absent unclustered. */
		{
			struct pc_col_fleet fl;
			pcache_htable_t *fh = pc_store_ht(i);
			int basis = pc_cluster_replicas() ? PC_FLEET_PER_K
				: pc_store_shard_enabled(fh) || pc_store_proxy_enabled(fh)
				? PC_FLEET_SUM
				: pc_store_eager_enabled(fh) ? PC_FLEET_FULLEST
				: PC_FLEET_AT_LEAST;

			if (pc_cluster_col_fleet(i, basis, &fl)) {
				pc_tw_key(out, "fleet");
				pc_tw_map(out);
				pc_tw_key(out, "basis");
				pc_tw_str(out, pc_fleet_basis_name(fl.basis));
				pc_tw_key(out, "members");
				pc_tw_i64(out, fl.members);
				pc_tw_key(out, "reporting");
				pc_tw_i64(out, fl.reporting);
				pc_tw_key(out, "entries");
				pc_tw_i64(out, (long long)fl.entries);
				pc_tw_key(out, "copies");
				pc_tw_i64(out, (long long)fl.copies);
				pc_tw_key(out, "expired");
				pc_tw_i64(out, (long long)fl.expired);
				pc_tw_key(out, "win_expired");
				if (fl.w_ok)
					pc_tw_i64(out, (long long)fl.w_expired);
				else
					pc_tw_nil(out);
				pc_tw_key(out, "hits");
				pc_tw_i64(out, (long long)fl.hits);
				pc_tw_key(out, "win_hits");
				if (fl.w_ok)
					pc_tw_i64(out, (long long)fl.w_hits);
				else
					pc_tw_nil(out);
				pc_tw_key(out, "misses");
				pc_tw_i64(out, (long long)fl.misses);
				pc_tw_key(out, "win_misses");
				if (fl.w_ok)
					pc_tw_i64(out, (long long)fl.w_misses);
				else
					pc_tw_nil(out);
				pc_tw_key(out, "stores");
				pc_tw_i64(out, (long long)fl.stores);
				pc_tw_key(out, "win_stores");
				if (fl.w_ok)
					pc_tw_i64(out, (long long)fl.w_stores);
				else
					pc_tw_nil(out);
				pc_tw_key(out, "removes");
				pc_tw_i64(out, (long long)fl.removes);
				pc_tw_key(out, "win_removes");
				if (fl.w_ok)
					pc_tw_i64(out, (long long)fl.w_removes);
				else
					pc_tw_nil(out);
				pc_tw_end(out);
			}
		}
		/* S109: what the collection holds - key + value bytes of every
		 * record, from the maintenance thread's paced walk (store.c), so
		 * a node whose records all arrived by replication shows its real
		 * size.  held_age_s = how old the figure is; -1 before the first
		 * walk (a page shows a dash then, not a zero). */
		{
			unsigned int age = 0;
			unsigned long long hb = pc_store_held(i, &age);

			pc_tw_key(out, "held_bytes");
			pc_tw_i64(out, (long long)hb);
			pc_tw_key(out, "held_age_s");
			pc_tw_i64(out, age == (unsigned int)-1 ? -1 : (long long)age);
		}
		/* S116: the value sizes HELD, from the same walk - the same on
		 * every member whatever path stored the record, unlike size_hist
		 * below, which counts this node's own client writes.  Zeros
		 * before the first walk; the page keys on held_age_s. */
		{
			unsigned long long hh[8] = { 0 };
			int c;

			(void)pc_store_held_hist(i, hh);
			pc_tw_key(out, "held_hist");
			pc_tw_arr(out);
			for (c = 0; c < 8; c++)
				pc_tw_i64(out, (long long)hh[c]);
			pc_tw_end(out);
		}
		/* S120: what this collection costs - its index regions, exact,
		 * and its records as the cells they occupy, from the walk */
		pc_tw_key(out, "index_bytes");
		pc_tw_i64(out, (long long)pc_store_index_bytes(i));
		pc_tw_key(out, "held_cells");
		pc_tw_i64(out, (long long)pc_store_held_cells(i));
		/* S67: the write stream's sizes - a mean the page turns into an
		 * estimate of memory held, and a log2 distribution */
		{
			unsigned long long sb = 0, sn = 0, sh[8] = { 0 };
			int c;

			pc_store_size_stats(i, &sb, &sn, sh);
			pc_tw_key(out, "stored_bytes");
			pc_tw_i64(out, (long long)sb);
			pc_tw_key(out, "stored_n");
			pc_tw_i64(out, (long long)sn);
			pc_tw_key(out, "size_hist");
			pc_tw_arr(out);
			for (c = 0; c < 8; c++)
				pc_tw_i64(out, (long long)sh[c]);
			pc_tw_end(out);
		}
		/* S397 (DESIGN 12ng): the hot keys - 1 request in `rate` sampled,
		 * the counts halved each minute; `est` and `err` are samples
		 * (est >= true >= est - err), `share_pm` per mille of `sampled`.
		 * A key not surely seen twice (est - err < 2) is noise and not
		 * listed - space-saving gives a newcomer the count it replaced. */
		{
			struct pc_hot_row hr[32];
			unsigned long long hs = 0, htot = 0, hbusy = 0;
			int hn = pc_store_hot_get(i, hr, 32, &hs, &htot, &hbusy), h;

			pc_tw_key(out, "hot_keys");
			pc_tw_map(out);
			pc_tw_key(out, "rate");
			pc_tw_i64(out, pc_store_hot_rate());
			pc_tw_key(out, "sampled");
			pc_tw_i64(out, (long long)hs);
			pc_tw_key(out, "sampled_total");
			pc_tw_i64(out, (long long)htot);
			pc_tw_key(out, "busy");
			pc_tw_i64(out, (long long)hbusy);
			pc_tw_key(out, "top");
			pc_tw_arr(out);
			for (h = 0; h < hn; h++) {
				if (hr[h].est - hr[h].err < 2)    /* not surely seen twice */
					continue;
				pc_tw_map(out);
				pc_tw_key(out, "key");
				pc_tw_bulk(out, hr[h].key, hr[h].kept);
				pc_tw_key(out, "key_len");
				pc_tw_i64(out, (long long)hr[h].klen);
				pc_tw_key(out, "cut");            /* key holds the first 64 bytes */
				pc_tw_bool(out, hr[h].kept < hr[h].klen);
				pc_tw_key(out, "est");
				pc_tw_i64(out, (long long)hr[h].est);
				pc_tw_key(out, "err");
				pc_tw_i64(out, (long long)hr[h].err);
				pc_tw_key(out, "share_pm");
				pc_tw_i64(out, !hs ? 0 : hr[h].est >= hs ? 1000 :
					(long long)(hr[h].est * 1000ULL / hs));
				pc_tw_end(out);
			}
			pc_tw_end(out);
			pc_tw_end(out);
		}
		pc_tw_end(out);
	}
	pc_tw_end(out);
	pc_tw_key(out, "cluster");
	stats_tree_cluster(out);
	pc_tw_key(out, "rdb");
	{
		struct pc_rdb_stats rs;

		pc_rdb_get_stats(&rs);
		if (!rs.enabled) {
			pc_tw_nil(out);
		} else {
			pc_tw_map(out);
			pc_tw_ctr(out, "rdb.saves", "saves", (unsigned long long)(rs.saves));
			pc_tw_key(out, "running");
			pc_tw_bool(out, (rs.running));
			pc_tw_key(out, "last_bytes");
			pc_tw_i64(out, (long long)rs.last_bytes);
			pc_tw_key(out, "last_dur_ms");
			pc_tw_i64(out, rs.last_dur_ms);
			pc_tw_key(out, "last_marker");
			pc_tw_i64(out, (long long)rs.last_marker);
			pc_tw_key(out, "safe_marker");      /* S215 */
			pc_tw_i64(out, (long long)rs.safe_marker);
			pc_tw_key(out, "save_errors");
			pc_tw_i64(out, (long long)rs.save_errors);
			pc_tw_key(out, "last_unix");
			pc_tw_i64(out, rs.last_unix);
			pc_tw_key(out, "last_age_s");    /* RV-12 */
			pc_tw_i64(out, rs.last_age_s);
			pc_tw_end(out);
		}
	}
	pc_tw_key(out, "wal");
	{
		struct pc_wal_stats ws;

		pc_wal_get_stats(&ws);
		since_wal(&ws);
		if (!ws.enabled) {
			pc_tw_nil(out);
		} else {
			/* the storage identity resolved at startup: the
			 * class/chain belong NEXT TO the wal counters,
			 * not only in a boot log line */
			const struct pc_st_id *sid = pc_wal_identity();

			pc_tw_map(out);
			pc_tw_key(out, "fsync");
			pc_tw_str(out, ws.fsync_mode);
			if (sid) {
				pc_tw_key(out, "storage_class");
				pc_tw_bulk(out, pc_st_class_str(sid->cls), strlen(pc_st_class_str(sid->cls)));
				pc_tw_key(out, "fstype");
				pc_tw_bulk(out, sid->fstype, strlen(sid->fstype));
				pc_tw_key(out, "chain");
				pc_tw_bulk(out, sid->chain, strlen(sid->chain));
			}
			{
				/* the startup measurement policy
				 * followed: cached from the wal dir's
				 * .pc-walprobe unless re-probed */
				const struct pc_wal_probe *pr =
					pc_wal_probe_result();

				if (pr) {
					pc_tw_key(out, "probe");
					pc_tw_map(out);
					pc_tw_key(out, "cached");
					/* nothing is cached any more
					 * (DESIGN 12am); the key
					 * stays for consumers */
					pc_tw_bool(out, 0);
					pc_tw_key(out, "sync_bs");
					pc_tw_i64(out, PC_WPROBE_SYNC_BS);
					pc_tw_key(out, "qd");
					pc_tw_i64(out, PC_WPROBE_QD);
					pc_tw_key(out, "seq_bs");
					pc_tw_i64(out, PC_WPROBE_SEQ_BS);
					pc_tw_key(out, "seq_mb_s");
					pc_tw_i64(out, (long long)
						(pr->seq_mb_s + 0.5));
					pc_tw_key(out, "fsync_p50_us");
					pc_tw_i64(out, pr->fsync_p50_us);
					pc_tw_key(out, "fsync_p99_us");
					pc_tw_i64(out, pr->fsync_p99_us);
					pc_tw_key(out, "sync_iops");
					pc_tw_i64(out, pr->sync_iops);
					pc_tw_end(out);
				}
				{
					/* what the fsyncs ACTUALLY
					 * cost, beside what the probe
					 * predicted - the probe cannot
					 * see past a write-back cache,
					 * this is measured under real
					 * load (DESIGN 12am) */
					struct pc_wal_fsync_obs ob;

					pc_wal_fsync_observed(&ob);
					pc_tw_key(out, "observed");
					pc_tw_map(out);
					/* S351: the count and the sum, each with its
					 * window - a window's mean is their ratio - and
					 * the worst of the last five minutes beside the
					 * worst ever */
					pc_tw_ctr(out, "wal.fsync_n", "fsync_n", ob.fsync_n);
					pc_tw_ctr(out, "wal.fsync_sum_us", "fsync_sum_us", ob.sum_us);
					pc_tw_key(out, "fsync_avg_us");
					pc_tw_i64(out, (long long)ob.avg_us);
					pc_tw_key(out, "fsync_recent_us");
					pc_tw_i64(out, (long long)ob.recent_us);
					pc_tw_key(out, "fsync_max_us");
					pc_tw_i64(out, (long long)ob.max_us);
					pc_tw_key(out, "win_fsync_max_us");
					pc_tw_i64(out, (long long)ob.win_max_us);
					pc_tw_key(out, "probe_p50_us");
					pc_tw_i64(out, (long long)
						ob.probe_p50_us);
					pc_tw_key(out, "probe_underestimated");
					pc_tw_bool(out, (ob.probe_underestimated));
					pc_tw_end(out);
				}
			}
			pc_tw_ctr(out, "wal.appended", "appended", (unsigned long long)(ws.appended));
			pc_tw_ctr(out, "wal.bytes", "bytes", (unsigned long long)(ws.bytes));
			pc_tw_ctr(out, "wal.dropped", "dropped", (unsigned long long)(ws.dropped));
			pc_tw_ctr(out, "wal.late", "late", (unsigned long long)(ws.late));
			pc_tw_key(out, "recycles");
			pc_tw_i64(out, (long long)ws.recycles);
			pc_tw_key(out, "storage_failed");   /* S215 */
			pc_tw_bool(out, (ws.failed));
			pc_tw_key(out, "buffer_bytes");     /* S265 */
			pc_tw_i64(out, (long long)pc_wal_buffer_bytes());
			pc_tw_key(out, "buffers_locked");
			pc_tw_bool(out, (pc_wal_locked_bytes(NULL)));
			pc_tw_key(out, "ctrl_errors");
			pc_tw_i64(out, (long long)ws.ctrl_errors);
			pc_tw_ctr(out, "wal.overruns", "overruns", (unsigned long long)(ws.overruns));
			pc_tw_ctr(out, "wal.heals", "heals", (unsigned long long)(ws.heals));          /* S229 */
			if (ws.healing) { pc_tw_key(out, "healing"); pc_tw_bool(out, 1); } else { pc_tw_key(out, "healing"); pc_tw_bool(out, 0); }
			pc_tw_key(out, "last_heal_ms");
			pc_tw_i64(out, ws.last_heal_ms);
			/* S306: heals in the window, and whether that many
			 * means healing is routine (the log is undersized) */
			pc_tw_key(out, "heals_recent");
			pc_tw_i64(out, ws.heals_recent);
			if (ws.heal_routine) { pc_tw_key(out, "heal_routine"); pc_tw_bool(out, 1); } else { pc_tw_key(out, "heal_routine"); pc_tw_bool(out, 0); }
			pc_tw_key(out, "unhealed");
			pc_tw_i64(out, (long long)ws.unhealed);
			pc_tw_key(out, "free_segments");
			pc_tw_i64(out, ws.free_segments);
			if (ws.segments > 0) {
				/* S437: the ring's shape and how much of it holds
				 * records no completed snapshot covers - the space
				 * a snapshot has to free before the ring wraps */
				int used = ws.segments - ws.free_segments;

				if (used < 0)
					used = 0;
				pc_tw_key(out, "segments");
				pc_tw_i64(out, ws.segments);
				pc_tw_key(out, "segment_mb");
				pc_tw_i64(out, (long long)(ws.segment_bytes >> 20));
				pc_tw_key(out, "size_mb");
				pc_tw_i64(out, (long long)(((unsigned long long)ws.segments *
					ws.segment_bytes) >> 20));
				pc_tw_key(out, "used_segments");
				pc_tw_i64(out, used);
				pc_tw_key(out, "used_pct");
				pc_tw_i64(out, (long long)used * 100 / ws.segments);
			}
			{
				/* S224: tenths are plenty - each goes as a number's
				 * text ('d'), which the tree has for exactly this
				 * (RV-13: three figures now) */
				static const char *const fk[3] = { "fill_mb_s",
					"full_in_s", "ring_fill_s" };
				double fv[3];
				char fb[48];
				int fi, fl;

				fv[0] = ws.fill_mb_s;
				fv[1] = ws.full_in_s;
				fv[2] = ws.ring_fill_s;
				for (fi = 0; fi < 3; fi++) {
					fl = snprintf(fb, sizeof fb, "%.1f", fv[fi]);
					if (fl <= 0 || (size_t)fl >= sizeof fb)
						continue;
					pc_tw_key(out, fk[fi]);
					pc_tw_num(out, fb, (size_t)fl);
				}
			}
			pc_tw_key(out, "last_seq");
			pc_tw_i64(out, (long long)ws.last_seq);
			pc_tw_key(out, "synced_seq");
			pc_tw_i64(out, (long long)ws.synced_seq);
			/* S145: the two states one "unsynced" figure used to span */
			pc_tw_key(out, "appended_seq");
			pc_tw_i64(out, (long long)ws.pending_hi);
			pc_tw_key(out, "staged");
			pc_tw_i64(out, (long long)(ws.last_seq > ws.pending_hi ? ws.last_seq - ws.pending_hi : 0));
			pc_tw_key(out, "unsynced");
			pc_tw_i64(out, (long long)(ws.pending_hi > ws.synced_seq ? ws.pending_hi - ws.synced_seq : 0));
			pc_tw_end(out);
		}
	}
	/* S150 B: tables a resize left behind, waiting for every thread
	 * that could hold a pointer into them to park; the walks that ended
	 * because their table moved; and the quiescence lines themselves */
	{
		unsigned int rp, lines, inside;
		unsigned long long rc, rb;

		pc_store_retire_figures(&rp, &rc, &rb);
		pc_qs_figures(&lines, &inside);
		pc_tw_key(out, "retire");
		pc_tw_map(out);
		pc_tw_key(out, "pending");
		pc_tw_i64(out, (long long)rp);
		pc_tw_key(out, "cleared");
		pc_tw_i64(out, (long long)rc);
		pc_tw_key(out, "returned_bytes");
		pc_tw_i64(out, (long long)rb);
		pc_tw_key(out, "keys_ended_on_swap");
		pc_tw_i64(out, (long long)pc_enum_ended_on_swap());
		pc_tw_key(out, "lines");
		pc_tw_i64(out, (long long)lines);
		pc_tw_key(out, "inside");
		pc_tw_i64(out, (long long)inside);
		pc_tw_end(out);
	}
	pc_tw_end(out);
}

void pc_members_tree(struct pc_tw *out)
{
	struct pc_member *mem = members_scratch();
	int n = mem ? pc_cluster_members(mem, PC_CL_MAXMEMBERS) : 0, i;

	pc_tw_map(out);
	pc_tw_key(out, "members");
	pc_tw_arr(out);
	for (i = 0; i < n; i++) {
		char ab[24];
		const char *as = pc_inet_ntop4(mem[i].addr, ab,
			sizeof ab);

		pc_tw_map(out);
		pc_tw_key(out, "addr");
		pc_tw_bulk(out, as, strlen(as));
		pc_tw_key(out, "port");
		pc_tw_i64(out, mem[i].client_port);
		pc_tw_key(out, "node");
		pc_tw_i64(out, mem[i].node);
		pc_tw_key(out, "self");
		pc_tw_bool(out, (mem[i].is_self));
		pc_tw_key(out, "master");
		pc_tw_bool(out, (mem[i].is_master));
		/* who takes over.  The map has named a standby since
		 * C1 and nothing showed it, so "which node is the
		 * backup" was a question only the master could
		 * answer. */
		pc_tw_key(out, "backup");
		pc_tw_bool(out, (mem[i].is_backup));
		pc_tw_key(out, "role");
		pc_tw_str(out, mem[i].is_master ? "master" :
			mem[i].is_backup ? "backup" : "member");
		/* B1: whether this member's data can be trusted yet -
		 * a separate question from whether it is the master */
		pc_tw_key(out, "gone_s");
		pc_tw_i64(out, mem[i].gone_s);
		pc_tw_key(out, "held_s");
		pc_tw_i64(out, mem[i].held_s);
		/* S293: and, for a member that has left, WHY - goodbye,
		 * silent (purged) or forgotten (refused) - and for a silent
		 * one how long it had been quiet */
		if (mem[i].gone_s >= 0) {
			pc_tw_key(out, "gone_reason");
			pc_tw_str(out, pc_cluster_gone_reason(mem[i].gone_why));
			pc_tw_key(out, "gone_silent_s");
			pc_tw_i64(out, mem[i].gone_silent_s);
		}
		pc_tw_key(out, "state");
		/* S192: a member that has LEFT reports no state of its own -
		 * it is not answering.  Say "gone" rather than borrow a
		 * lifecycle word that means something else. */
		pc_tw_str(out, mem[i].gone_s >= 0 ? "gone"
			: pc_node_state_name(mem[i].state));
		/* S186: and WHY, when there is a reason to give.  The card
		 * showed FAILED with nothing beside it, which sends an
		 * operator to the logs of a node that may not be the one
		 * they are looking at - this is every node's view of every
		 * other. */
		pc_tw_key(out, "reason");
		pc_tw_str(out, pc_node_reason_text(mem[i].reason));
		/* 0.5.7.1: a reconcile open on it, and its only live witness
		 * (0 none - it waits for its own restart; 65535 two or more;
		 * -1 the member does not say) */
		pc_tw_key(out, "reconciling");
		pc_tw_bool(out, mem[i].reconciling);
		pc_tw_key(out, "sole_witness");
		pc_tw_i64(out, mem[i].sole_witness);
		/* 12ny AT-11: its marks of each origin's numbered deletes -
		 * confirmed, how far behind the head (the highest any member
		 * announced, or this node has seen), and how long the mark has
		 * stood still (null for this node's own: not kept) */
		{
			struct pc_ack_row ar[16];
			struct timespec ts;
			long long nowm;
			int na = 0, q;

			clock_gettime(CLOCK_MONOTONIC, &ts);
			nowm = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
			if (mem[i].has_ident)
				na = pc_ack_member_rows(fnv1a64(mem[i].ident,
					sizeof mem[i].ident), mem[i].is_self, ar, 16);
			pc_tw_key(out, "acks");
			pc_tw_arr(out);
			for (q = 0; q < na; q++) {
				char h[17];

				snprintf(h, sizeof h, "%016llx",
					(unsigned long long)ar[q].oid);
				pc_tw_map(out);
				pc_tw_key(out, "origin");
				pc_tw_str(out, h);
				pc_tw_key(out, "incarn");
				pc_tw_i64(out, (long long)ar[q].incarn);
				pc_tw_key(out, "confirmed");
				pc_tw_i64(out, (long long)ar[q].mark);
				pc_tw_key(out, "behind");
				pc_tw_i64(out, (long long)(ar[q].head - ar[q].mark));
				pc_tw_key(out, "still_s");
				if (ar[q].moved)
					pc_tw_i64(out, (nowm - ar[q].moved) / 1000);
				else
					pc_tw_nil(out);
				pc_tw_end(out);
			}
			pc_tw_end(out);
		}
		/* AR11: the settings the fleet must agree on that it holds
		 * differently from this node (none said: none compared) */
		{
			const struct pc_cfgsig *cs;
			int nc = pc_cluster_cfgsig(&cs), q, r;

			pc_tw_key(out, "config_differs");
			pc_tw_arr(out);
			for (q = 0; !mem[i].is_self && q < nc; q++)
				for (r = 0; r < mem[i].ncfg; r++)
					if (mem[i].cfg_name[r] == cs[q].name_h) {
						if (mem[i].cfg_val[r] != cs[q].val_h)
							pc_tw_str(out, cs[q].what);
						break;
					}
			pc_tw_end(out);
		}
		/* UP1: the member's build and the wire it speaks (null: a build
		 * before 0.6.5 says nothing) */
		{
			uint32_t fv;
			char vb[24];

			pc_tw_key(out, "version");
			if (pc_member_fact(&mem[i], PC_FACT_VERSION, &fv)) {
				snprintf(vb, sizeof vb, "%u.%u.%u", fv >> 16,
					(fv >> 8) & 0xff, fv & 0xff);
				pc_tw_str(out, vb);
			} else
				pc_tw_nil(out);
			pc_tw_key(out, "wire_epochs");
			if (pc_member_fact(&mem[i], PC_FACT_EPOCHS, &fv)) {
				pc_tw_arr(out);
				pc_tw_i64(out, (long long)(fv >> 8));
				pc_tw_i64(out, (long long)(fv & 0xff));
				pc_tw_end(out);
			} else
				pc_tw_nil(out);
			/* UPS: its helper, its upgrade state and the build it has
			 * staged (the sha256's first four hex digits) */
			pc_tw_key(out, "upgrade");
			if (pc_member_fact(&mem[i], PC_FACT_UPGRADE, &fv)) {
				pc_tw_map(out);
				pc_tw_key(out, "helper");
				pc_tw_str(out, pc_upg_fact_helper(fv));
				pc_tw_key(out, "state");
				pc_tw_str(out, pc_upg_fact_state(fv));
				pc_tw_key(out, "staged");
				if (fv & PC_UPG_FACT_HAS_STAGED) {
					snprintf(vb, sizeof vb, "%04x", fv >> 16);
					pc_tw_str(out, vb);
				} else
					pc_tw_nil(out);
				pc_tw_end(out);
			} else
				pc_tw_nil(out);
			/* S436: whether the autoscale modes it holds set at
			 * runtime differ from this node's (null: not reported - a
			 * build before 0.7.2, or a keepalive whose settings take
			 * every slot) */
			pc_tw_key(out, "autoscale_differs");
			if (mem[i].is_self)
				pc_tw_bool(out, 0);
			else if (pc_member_fact(&mem[i], PC_FACT_AUTOSCALE, &fv))
				pc_tw_bool(out, fv != pc_store_autoscale_sig(NULL));
			else
				pc_tw_nil(out);
		}
		pc_tw_key(out, "mem_tier");
		pc_tw_str(out, mem_tier_token(mem[i].mem_tier));
		pc_tw_key(out, "free_mb");
		pc_tw_i64(out, mem[i].free_mb);
		pc_tw_key(out, "total_mb");
		pc_tw_i64(out, mem[i].total_mb);
		/* S78: gossiped, so the grid links to the node's OWN door and
		 * shows its uptime; -1 = a build before the fields */
		pc_tw_key(out, "http");
		pc_tw_i64(out, mem[i].http_port);
		pc_tw_key(out, "uptime_s");
		pc_tw_i64(out, mem[i].uptime_s);
		/* S103: this node's VIEW of the member - what the sender election
		 * decides from */
		pc_tw_key(out, "entries");
		pc_tw_i64(out, mem[i].entries);
		pc_tw_key(out, "start");
		pc_tw_str(out, mem[i].start ? mem[i].start : "unknown");
		/* S160: its open clients as gossiped, absent for a build before
		 * the field - the page sums what is present and says how many
		 * members reported (the json gauge left with the dialect, S317) */
		if (mem[i].cl_reported) {
			pc_tw_key(out, "clients");
			pc_tw_map(out);
			pc_tw_key(out, "open");
			pc_tw_i64(out, mem[i].cl_open);
			pc_tw_key(out, "resp");
			pc_tw_i64(out, mem[i].cl_resp);
			pc_tw_key(out, "binary");
			pc_tw_i64(out, mem[i].cl_bin);
			pc_tw_key(out, "native_resp");
			pc_tw_i64(out, mem[i].cl_nresp);
			pc_tw_end(out);
		}
		/* PS11: its relay port and whether relays to it use it */
		pc_tw_key(out, "relay");
		pc_tw_map(out);
		pc_tw_key(out, "port");
		pc_tw_i64(out, mem[i].relay_port);
		pc_tw_key(out, "direct");
		pc_tw_bool(out, mem[i].relay_direct);
		/* PS12: what relays to it are: all, filtered by its interest, or
		 * everything because our copy of its interest is not current */
		pc_tw_key(out, "interest");
		pc_tw_str(out, mem[i].relay_interest == 1 ? "filtered"
			: mem[i].relay_interest == 2 ? "broadcast" : "all");
		pc_tw_end(out);
		/* S163: its per-collection figures, joined to /stats by hash */
		if (mem[i].cols_reported) {
			int k;

			pc_tw_key(out, "collections_total");
			pc_tw_i64(out, mem[i].ncols_total);
			pc_tw_key(out, "collections");
			pc_tw_arr(out);
			for (k = 0; k < mem[i].ncols; k++) {
				char hx[24];
				const struct clmemb_col *cl = &mem[i].cols[k];
				unsigned char hb[8];
				int q;

				/* %016llx by hand (see pc_inet_ntop4) */
				for (q = 0; q < 8; q++)
					hb[q] = (unsigned char)(cl->hash >> (56 - 8 * q));
				hex_bytes(hx, hb, 8);
				pc_tw_map(out);
				pc_tw_key(out, "hash");
				pc_tw_bulk(out, hx, 16);
				pc_tw_key(out, "entries");
				pc_tw_i64(out, (long long)cl->entries);
				pc_tw_key(out, "hits");
				pc_tw_i64(out, (long long)cl->hits);
				pc_tw_key(out, "misses");
				pc_tw_i64(out, (long long)cl->misses);
				pc_tw_key(out, "stores");
				pc_tw_i64(out, (long long)cl->stores);
				pc_tw_key(out, "removes");
				pc_tw_i64(out, (long long)cl->removes);
				pc_tw_key(out, "expired");
				pc_tw_i64(out, (long long)cl->expired);
				pc_tw_end(out);
			}
			pc_tw_end(out);
		}
		/* identity is stable across that node's restarts,
		 * incarnation is not - a client watching both can
		 * tell a restart from a missed heartbeat */
		if (mem[i].has_ident) {
			char hx[33];

			hex_bytes(hx, mem[i].ident, 16);   /* was 16 snprintf */
			hx[32] = 0;
			pc_tw_key(out, "identity");
			pc_tw_bulk(out, hx, 32);
			pc_tw_key(out, "incarnation");
			pc_tw_i64(out, (long long)mem[i].incarn);
		}
		pc_tw_end(out);
	}
	/* S221: the cluster wire epoch, advertised beside the routing algo
	 * and for the same reason - a client that does not recognise it
	 * stops ROUTING and lets the daemon forward, which costs a hop and
	 * never correctness. */
	pc_tw_end(out);
	/* S341: what the fleet should number, so the page's header can say
	 * "2/3" when a member is missing from the list altogether - a
	 * member that never joined, or whose id was released, is in no
	 * list.  `expect` is [cluster] expect (0 = not set); `seen_max` is
	 * the most members this node has listed since it started.  Every
	 * client's members call and every page poll comes through here,
	 * so the high-water mark follows the fleet closely. */
	{
		static int seen_max;
		int sm = __atomic_load_n(&seen_max, __ATOMIC_RELAXED);

		while (n > sm && !__atomic_compare_exchange_n(&seen_max, &sm, n,
		        0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
			;                     /* sm reloaded: try again while n is higher */
		pc_tw_key(out, "fleet");
		pc_tw_map(out);
		pc_tw_key(out, "expect");
		pc_tw_i64(out, pc_cluster_expect());
		pc_tw_key(out, "seen_max");
		pc_tw_i64(out, n > sm ? n : sm);
		pc_tw_end(out);
	}
	pc_tw_key(out, "routing");
	pc_tw_map(out);
	pc_tw_key(out, "epoch");
	pc_tw_i64(out, pc_cluster_epoch());   /* 12nu: the one spoken */
	pc_tw_key(out, "algo");
	pc_tw_str(out, PC_ROUTE_ALGO);
	pc_tw_key(out, "mode");
	{
		int md = pc_cluster_mode();

		pc_tw_str(out, md == PC_MODE_PROXY ? "proxy"
			: md == PC_MODE_SPREAD ? "spread"
			: md == PC_MODE_SHARD ? "shard" : "store");
	}
	pc_tw_key(out, "eager");
	pc_tw_bool(out, (pc_cluster_eager()));
	/* the single operational name; "mode" above stays as it was so
	 * an existing reader keeps working */
	pc_tw_key(out, "mode_name");
	pc_tw_str(out, pc_cluster_mode_name());
	/* S127: K under spread, 0 otherwise.  An operator confirming a
	 * spread fleet needs the number the WRITE PATH is using, not
	 * the one in the file - they differ if the config never
	 * reached the cluster. */
	pc_tw_key(out, "replicas");
	pc_tw_i64(out, pc_cluster_replicas());
	pc_tw_key(out, "authoritative");
	pc_tw_bool(out, (pc_cluster_authoritative()));
	/* the peer-plane port every member shares: owner selection
	 * mixes (advertise ip, CLUSTER port), so a client that wants
	 * to compute an owner needs it - the client port would give
	 * a different hash and route everything wrong (harmlessly,
	 * but pointlessly) */
	pc_tw_key(out, "cport");
	pc_tw_i64(out, pc_cluster_port());
	/* RV-10: the stamp of the map this list was read under.  A reply
	 * the daemon had to forward carries the stamp it routed by, and a
	 * client holding a different one knows it is behind. */
	{
		unsigned int mt = 0, ms = 0;

		if (pc_cluster_map_stamp(&mt, &ms)) {
			pc_tw_key(out, "map");
			pc_tw_map(out);
			pc_tw_key(out, "term");
			pc_tw_i64(out, mt);
			pc_tw_key(out, "seq");
			pc_tw_i64(out, ms);
			pc_tw_end(out);
		}
	}
	pc_tw_end(out);
	/* S70: the one thing a client cannot otherwise learn about the
	 * daemon it dialled, and the first question in a mixed fleet */
	pc_tw_key(out, "version");
	pc_tw_str(out, PC_VERSION);
	pc_tw_key(out, "rev");
	pc_tw_str(out, PC_BUILD_REV);
	pc_tw_end(out);
}
