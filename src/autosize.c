/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * autosize.c - the tables' sizing, from the maintenance thread: S150 D's
 * shrink after a quiet cool-off, S239's advice (what AUTO would do,
 * published in every mode, said under WARN), S328's floor.  Moved from
 * daemon.c unchanged (the 0.7.2 split, wave 1 - doc/MODULARITY.md); the
 * splitter and the leg drain stay in the maintenance loop.
 */
#include <stdio.h>
#include <string.h>
#include "compat/dprint.h"
#include "core/pcache_htable.h"
#include "config.h"
#include "store.h"
#include "autosize.h"

/* S150 D: the other half of the splitter.  A table that sits below
 * shrink_at_pct of its slot capacity, and has neither split nor been
 * resized nor started within the cool-off, is resized DOWN - by the S69
 * migration, to the size that puts it midway between the two thresholds
 * so one copy lands it where neither fires - and never below one
 * segment, the floor a table always carves.  The old index then goes
 * back to the arena (steps B and C).  Local, like the splitter: each node
 * sizes what it holds. */
static unsigned int col_quiet_since[PC_MAX_COLLECTIONS];
static unsigned long long col_seen_pub[PC_MAX_COLLECTIONS];

/* S328: the modes that size the table by its keys - AUTO, and FLOOR,
 * which is AUTO that never shrinks below the operator's size */
int as_sizes(int i)
{
	int m = pc_store_autoscale(i);

	return m == PC_AUTOSCALE_AUTO || m == PC_AUTOSCALE_FLOOR;
}

/* S328: the smallest log2 size a shrink may pick: one segment, or under
 * FLOOR the size the operator gave the collection, whichever is larger */
static unsigned int shrink_lowest(int i)
{
	int fl = pc_store_autoscale(i) == PC_AUTOSCALE_FLOOR ?
		pc_store_floor_log2(i) : 0;

	return fl > PCACHE_SEG_BITS ? (unsigned int)fl : PCACHE_SEG_BITS;
}

void shrink_consider(int i, pcache_htable_t *cht, unsigned int now,
		int grew)
{
	pcache_ht_totals_t tot;
	unsigned long long pub = pc_store_pub_gen(i);
	unsigned int nb, cur = 0, t, lo = shrink_lowest(i);
	int rs_t, made = pc_store_buckets_log2(i);
	unsigned long long rs_m;
	const char *name;

	if (grew || !col_quiet_since[i] || col_seen_pub[i] != pub) {
		/* a split, a first sighting, a new incarnation or a resize just
		 * published: the cool-off starts (or restarts) here */
		col_quiet_since[i] = now;
		col_seen_pub[i] = pub;
		return;
	}
	if (pc_shrink_at_pct <= 0 ||
	    now - col_quiet_since[i] < (unsigned int)pc_shrink_cooloff_s ||
	    pc_store_resizing(i, &rs_t, &rs_m))
		return;
	nb = pcache_ht_nbuckets(cht);
	if (nb <= (1U << lo))
		return;                    /* one segment, or (S328) the floor */
	pcache_ht_totals(cht, &tot);
	if (tot.entries * 100 >=
	    (unsigned long long)pc_shrink_at_pct * PCACHE_SLOTS * nb)
		return;
	while ((1U << cur) < nb)
		cur++;                     /* what it has grown to, as a power */
	for (t = lo; t < cur; t++)
		if (tot.entries * 100 <= (unsigned long long)
		        ((pc_grow_at_pct + pc_shrink_at_pct) / 2) *
		        PCACHE_SLOTS * (1ULL << t))
			break;
	if (t >= cur || (1ULL << (t + 1)) > nb)
		return;                    /* less than a halving: not worth a copy */
	name = pc_store_name(i);
	if (pc_store_resize_start(name, strlen(name), (int)t) == 0) {
		LM_NOTICE("collection '%s': %llu records in %u buckets (%llu%% of "
			"the slots, made at 2^%d): shrinking to 2^%u%s\n", name,
			(unsigned long long)tot.entries, nb,
			(unsigned long long)tot.entries * 100 /
				((unsigned long long)PCACHE_SLOTS * nb),
			made, t, t == lo && lo > PCACHE_SEG_BITS ?
			" - its floor (autoscale = floor)" : "");
		col_quiet_since[i] = now;  /* the cool-off restarts on a shrink */
	}
}

/*
 * S239: what AUTO would do with table @i, as a log2 bucket count - 0 when
 * it would leave it.  Grow: entries past grow_at_pct of the slots.  Shrink:
 * below shrink_at_pct, and more than one segment.  Either way the target
 * is the size midway between the two thresholds, as shrink_consider picks
 * it.  Published for /stats, /metrics and the page, every mode.  Under
 * WARN it is also said in the log - the first time at once (the boot
 * clock starts at 0, S242's lesson), then at most every ten minutes, and
 * again whenever the target changes.
 */
static unsigned int as_warned_at[PC_MAX_COLLECTIONS];
static int as_warned_t[PC_MAX_COLLECTIONS];

void autoscale_advise(int i, pcache_htable_t *cht, unsigned int now)
{
	pcache_ht_totals_t tot;
	unsigned int nb = pcache_ht_nbuckets(cht), cur = 0, t = 0;
	unsigned long long mid = (unsigned long long)(pc_grow_at_pct + pc_shrink_at_pct) / 2;
	int grow;
	const char *name;

	pcache_ht_totals(cht, &tot);
	while ((1U << cur) < nb)
		cur++;
	grow = tot.entries * 100 > (unsigned long long)pc_grow_at_pct * PCACHE_SLOTS * nb;
	if (grow) {
		for (t = cur + 1; t < 30; t++)
			if (tot.entries * 100 <= mid * PCACHE_SLOTS * (1ULL << t))
				break;
	} else if (pc_shrink_at_pct > 0 && nb > (1U << shrink_lowest(i)) &&
	           tot.entries * 100 < (unsigned long long)pc_shrink_at_pct * PCACHE_SLOTS * nb) {
		for (t = shrink_lowest(i); t < cur; t++)
			if (tot.entries * 100 <= mid * PCACHE_SLOTS * (1ULL << t))
				break;
		if (t >= cur || (1ULL << (t + 1)) > nb)
			t = 0;             /* less than a halving: not worth a copy */
	}
	pc_store_set_autoscale_target(i, (int)t);
	if (!t || pc_store_autoscale(i) != PC_AUTOSCALE_WARN)
		return;
	if (as_warned_at[i] && as_warned_t[i] == (int)t &&
	    now - as_warned_at[i] < 600)
		return;
	as_warned_at[i] = now ? now : 1;
	as_warned_t[i] = (int)t;
	name = pc_store_name(i);
	LM_WARN("collection '%s' (autoscale = warn): %llu records in %u buckets "
		"(%llu%% of the slots%s) - auto would %s it to 2^%u.  Resize it "
		"(`perfcli resize %s %u`) or let it size itself (`perfcli "
		"autoscale %s auto`)\n", name,
		(unsigned long long)tot.entries, nb,
		(unsigned long long)tot.entries * 100 /
			((unsigned long long)PCACHE_SLOTS * nb),
		grow ? ", growth blocked: the rest is in the overflow leg" : "",
		grow ? "grow" : "shrink", t, name, t, name);
}
