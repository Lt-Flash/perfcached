/* autosizetest.c - the tables' sizing (autosize.c; the 0.7.2 split, wave
 * 1): shrink_consider and autoscale_advise against stubbed tables (a
 * bucket count and a record count) and a stubbed store; the log kept.
 * Each case its own collection index, so the per-table clocks start clean.
 * With 6 slots a bucket, grow at 75%, shrink at 18%, the target is the
 * size that puts the table at the midpoint, 46%.
 *
 *   A  the shrink: a first sighting starts the cool-off; a second short
 *      of it, nothing; at it, a 2^16 table holding 10,000 records goes to
 *      2^12 - the smallest at or under the midpoint - said, the cool-off
 *      restarted; a split, a new incarnation, a resize under way each
 *      restart or hold it
 *   B  no shrink: above shrink_at_pct; one segment already; shrink off;
 *      less than a halving to gain (shrink_at_pct close to the
 *      midpoint); a resize the store refused is tried again at once
 *   C  FLOOR: the shrink stops at the operator's size, and says so; only
 *      AUTO and FLOOR size by keys
 *   D  the advice, every mode: grow past grow_at_pct to the midpoint
 *      size, shrink below shrink_at_pct, 0 in between
 *   E  under WARN it is said: at once (clock 0 included), not again for
 *      ten minutes at the same target, again when the target changes,
 *      and at ten minutes; nothing when the advice is 0
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "compat/dprint.h"
#include "core/pcache_htable.h"
#include "config.h"
#include "store.h"
#include "autosize.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs: daemon.c's knobs, the store, the tables, the log ------- */
int pc_grow_at_pct = 75, pc_shrink_at_pct = 18, pc_shrink_cooloff_s = 60;

#define N 64
static int mode[N], floor_l2[N], made[N] = { [0 ... N - 1] = 16 }, resizing[N], target[N] = { [0 ... N - 1] = -1 };
static unsigned long long pub[N];
static unsigned int nb[N];
static unsigned long entries[N];
static char names[N][8];
static int rs_rc, rs_calls, rs_to;

int pc_store_autoscale(int i) { return mode[i]; }
int pc_store_floor_log2(int i) { return floor_l2[i]; }
unsigned long long pc_store_pub_gen(int i) { return pub[i]; }
int pc_store_buckets_log2(int i) { return made[i]; }
void pc_store_set_autoscale_target(int i, int t) { target[i] = t; }

int pc_store_resizing(int i, int *t, unsigned long long *m)
{
	*t = 0;
	*m = 0;
	return resizing[i];
}

const char *pc_store_name(int i)
{
	snprintf(names[i], sizeof names[i], "c%d", i);
	return names[i];
}

int pc_store_resize_start(const char *name, size_t nlen, int t)
{
	(void)name; (void)nlen;
	rs_calls++;
	rs_to = t;
	return rs_rc;
}

/* a table is its collection's index, as a pointer */
static pcache_htable_t *tab(int i)
{
	return (pcache_htable_t *)(names[i]);
}

static int idx(pcache_htable_t *ht)
{
	return (int)(((char (*)[8])ht) - names);
}

unsigned int pcache_ht_nbuckets(pcache_htable_t *ht) { return nb[idx(ht)]; }

void pcache_ht_totals(pcache_htable_t *ht, pcache_ht_totals_t *out)
{
	memset(out, 0, sizeof *out);
	out->entries = entries[idx(ht)];
}

int compat_log_level = L_NOTICE;
static char logbuf[4096];
static int warns, notices;

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	size_t n = strlen(logbuf);
	va_list ap;

	(void)tag;
	va_start(ap, fmt);
	vsnprintf(logbuf + n, sizeof logbuf - n, fmt, ap);
	va_end(ap);
	if (level == L_WARN)
		warns++;
	else if (level == L_NOTICE)
		notices++;
}

/* ---- the harness ------------------------------------------------------- */
static void quiet(void)
{
	logbuf[0] = 0;
	warns = notices = 0;
	rs_calls = 0;
	rs_to = -1;
}

/* table @i: @n records in 2^@l2 buckets */
static void table(int i, unsigned long n, int l2)
{
	entries[i] = n;
	nb[i] = 1u << l2;
}

/* a first sighting at @t0, then the cool-off's end: did it shrink? */
static void settle(int i, unsigned int t0)
{
	shrink_consider(i, tab(i), t0, 0);
	shrink_consider(i, tab(i), t0 + (unsigned int)pc_shrink_cooloff_s, 0);
}

int main(void)
{
	/* A */
	table(1, 10000, 16);
	quiet();
	shrink_consider(1, tab(1), 100, 0);
	CHK(rs_calls == 0, "A: a first sighting - the cool-off starts, nothing done");
	shrink_consider(1, tab(1), 159, 0);
	CHK(rs_calls == 0, "A: 59 s on - inside the cool-off");
	shrink_consider(1, tab(1), 160, 0);
	CHK(rs_calls == 1 && rs_to == 12 && notices == 1 &&
		strstr(logbuf, "collection 'c1': 10000 records in 65536 buckets (2% of the slots, made at 2^16): shrinking to 2^12\n"),
		"A: 60 s on, 10,000 records in 2^16 - shrunk to 2^12, said");
	shrink_consider(1, tab(1), 161, 0);
	CHK(rs_calls == 1, "A: the cool-off restarted by the shrink");
	quiet();
	shrink_consider(1, tab(1), 300, 1);
	shrink_consider(1, tab(1), 330, 0);
	CHK(rs_calls == 0, "A: a split at 300 - the cool-off restarts, nothing at 330");
	pub[1]++;
	shrink_consider(1, tab(1), 400, 0);
	CHK(rs_calls == 0, "A: a new incarnation published - restarts it too");
	resizing[1] = 1;
	shrink_consider(1, tab(1), 460, 0);
	CHK(rs_calls == 0, "A: a resize under way - held");
	resizing[1] = 0;
	shrink_consider(1, tab(1), 461, 0);
	CHK(rs_calls == 1 && rs_to == 12, "A: the resize done - shrunk");

	/* B */
	quiet();
	table(2, 80000, 16);                  /* 20.3% of 393,216 slots */
	settle(2, 100);
	CHK(rs_calls == 0, "B: 80,000 records in 2^16 (20%%) - above shrink_at_pct, kept");
	table(3, 10, 12);
	settle(3, 100);
	CHK(rs_calls == 0, "B: one segment (2^12) - never below it");
	pc_shrink_at_pct = 0;
	table(4, 10000, 16);
	settle(4, 100);
	CHK(rs_calls == 0, "B: shrink_at_pct 0 - off");
	pc_shrink_at_pct = 60;                /* the midpoint becomes 67% */
	table(5, 157286, 16);                 /* 40% */
	settle(5, 100);
	CHK(rs_calls == 0, "B: 40%% under a 60%% shrink, the midpoint 67%% - less than a halving to gain, kept");
	pc_shrink_at_pct = 18;
	rs_rc = -1;
	table(6, 10000, 16);
	settle(6, 100);
	CHK(rs_calls == 1 && notices == 0, "B: the store refuses the resize - nothing said");
	rs_rc = 0;
	shrink_consider(6, tab(6), 160, 0);
	CHK(rs_calls == 2 && notices == 1, "B: and the next tick tries again - the cool-off was not restarted");

	/* C */
	quiet();
	mode[7] = PC_AUTOSCALE_FLOOR;
	floor_l2[7] = 14;
	table(7, 10000, 16);
	settle(7, 100);
	CHK(rs_calls == 1 && rs_to == 14 && strstr(logbuf, "shrinking to 2^14 - its floor (autoscale = floor)"),
		"C: FLOOR at 2^14 - the shrink stops there, and says why");
	mode[9] = PC_AUTOSCALE_WARN;
	mode[10] = PC_AUTOSCALE_OFF;
	CHK(as_sizes(7) && as_sizes(8) && !as_sizes(9) && !as_sizes(10),
		"C: FLOOR and AUTO size by keys, WARN and OFF do not");

	/* D */
	quiet();
	table(11, 20000, 12);                 /* 81% of 24,576 slots */
	autoscale_advise(11, tab(11), 50);
	CHK(target[11] == 13 && !warns, "D: 20,000 records in 2^12 - grow to 2^13 advised (AUTO: not said)");
	table(12, 100000, 16);                /* 25% */
	autoscale_advise(12, tab(12), 50);
	CHK(target[12] == 0, "D: 100,000 in 2^16, between the two - 0");
	table(13, 10000, 16);
	autoscale_advise(13, tab(13), 50);
	CHK(target[13] == 12, "D: 10,000 in 2^16 - shrink to 2^12 advised");

	/* E */
	quiet();
	mode[14] = PC_AUTOSCALE_WARN;
	table(14, 20000, 12);
	autoscale_advise(14, tab(14), 0);
	CHK(warns == 1 && strstr(logbuf, "collection 'c14' (autoscale = warn): 20000 records in 4096 buckets "
		"(81% of the slots, growth blocked: the rest is in the overflow leg) - auto would grow it to 2^13.  "
		"Resize it (`perfcli resize c14 13`) or let it size itself (`perfcli autoscale c14 auto`)"),
		"E: WARN, at clock 0 - said at once, with the commands");
	autoscale_advise(14, tab(14), 300);
	CHK(warns == 1, "E: 300 s on, the same target - not again");
	table(14, 50000, 12);
	autoscale_advise(14, tab(14), 400);
	CHK(warns == 2 && target[14] == 15 && strstr(logbuf, "grow it to 2^15"), "E: the target now 2^15 - said again");
	autoscale_advise(14, tab(14), 999);
	CHK(warns == 2, "E: 599 s after that - not again");
	autoscale_advise(14, tab(14), 1000);
	CHK(warns == 3, "E: 600 s - said again");
	table(14, 100000, 16);
	autoscale_advise(14, tab(14), 5000);
	CHK(warns == 3 && target[14] == 0, "E: nothing to advise - nothing said");

	printf("autosizetest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
