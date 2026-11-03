/* clretaintest.c - S238: the retained-tombstone table (clretain.c).
 *
 *   A  get: a noted tombstone answers with the highest version noted, and
 *      nothing after retain_s
 *   B  each: replays exactly the entries planted in (since, upto]
 *   C  the cap: past `max`, the OLDEST goes; one evicted before retain_s
 *      is counted as dropped early, one past it is not
 *   D  expire: drops what is older than retain_s, keeps the rest
 *   F  12ny AT-5: numbered deletes - forgotten through a mark, in their
 *      space's arrival order (a late lower one waits for the head), a
 *      newer delete taking an entry over (numbered or not), two spaces
 *      apart, the replay walk carrying the numbering, how far each space
 *      was forgotten and when, the expiry hold
 *   G  12ny AT-8: what this table no longer holds, for a member back from
 *      an absence - per space, the highest seq forgotten by the marks or
 *      dropped unconfirmed (the time rule, the cap); and whether anything
 *      unnumbered noted since a time is gone
 *   E  memory at the default cap (250,000 x a realistic key): the bytes
 *      the table reports, and what the process actually grew by (RSS) -
 *      the figure the README quotes
 * Milliseconds, and a quarter-million allocations. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/clretain.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static long rss_kb(void)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long v = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof line, f))
		if (!strncmp(line, "VmRSS:", 6))
			v = strtol(line + 6, NULL, 10);
	fclose(f);
	return v;
}

struct acc { unsigned int n, x, num; unsigned long long vsum, seqsum; };
static void count_cb(const char *col, size_t cn, const char *key, size_t kn,
		uint64_t ver, int expiry, uint64_t oid, uint32_t incarn,
		uint64_t seq, void *ctx)
{
	struct acc *a = ctx;

	(void)col; (void)cn; (void)key; (void)kn; (void)oid; (void)incarn;
	a->n++;
	a->x += expiry != 0;               /* S446: the kind, for the replay */
	a->vsum += ver;
	a->num += seq != 0;                /* AT-5: the numbering, for the replay */
	a->seqsum += seq;
}

/* clretain.c's shard of a key - the arrival order is per shard, so the
 * late-arrival case needs keys that share one (hash_of, NSHARD 64) */
static unsigned int shard_of(const char *col, const char *key)
{
	unsigned int h = 2166136261u;

	for (; *col; col++)
		h = (h ^ (unsigned char)*col) * 16777619u;
	h = (h ^ 0u) * 16777619u;
	for (; *key; key++)
		h = (h ^ (unsigned char)*key) * 16777619u;
	return h % 64u;
}

/* AT-5 (c): counts the kept expiries a replay walk hands out */
static void kind_cb(const char *col, size_t cn, const char *key, size_t kn,
		uint64_t ver, int expiry, uint64_t oid, uint32_t incarn,
		uint64_t seq, void *ctx)
{
	(void)col; (void)cn; (void)key; (void)kn; (void)ver; (void)oid;
	(void)incarn; (void)seq;
	((struct acc *)ctx)->x += expiry == 2;
}

struct fg { unsigned int n; uint64_t through; unsigned int at; };
static int fg_cb(uint64_t oid, uint32_t incarn, uint64_t through,
		unsigned int at_s, void *ctx)
{
	struct fg *f = ctx;

	if (oid == 0x77 && incarn == 1) {
		f->through = through;
		f->at = at_s;
	}
	f->n++;
	return 0;
}

struct gn { unsigned int n; uint64_t hi; };
static int gn_cb(uint64_t oid, uint32_t incarn, uint64_t gone_hi, void *ctx)
{
	struct gn *g = ctx;

	(void)oid; (void)incarn;
	g->n++;
	if (gone_hi > g->hi)
		g->hi = gone_hi;
	return 0;
}

int main(void)
{
	struct clretain_stats st;
	struct acc a;
	char k[32];
	long r0, r1;
	int i;

	/* ---- A..D on a small table: retain 100 s, 640 entries (10 a shard) */
	clretain_init(100, 640);
	clretain_note("c", 1, "k1", 2, 5, 1000);
	clretain_note("c", 1, "k1", 2, 3, 1001);     /* lower: the version stays */
	CHK(clretain_get("c", 1, "k1", 2, 1050) == 5, "A: the highest noted version answers (5)");
	CHK(clretain_get("c", 1, "k2", 2, 1050) == 0, "A: an unknown key answers 0");
	CHK(clretain_get("d", 1, "k1", 2, 1050) == 0, "A: the collection is part of the key");
	CHK(clretain_get("c", 1, "k1", 2, 1101) == 0, "A: past retain_s it answers nothing (planted 1001, asked 1101)");

	clretain_init(100, 640);
	for (i = 0; i < 300; i++) {
		snprintf(k, sizeof k, "b%03d", i);
		clretain_note("c", 1, k, strlen(k), (uint64_t)i + 1, 2000 + (unsigned)i / 10);
	}
	memset(&a, 0, sizeof a);
	CHK(clretain_each(2009, 2019, count_cb, &a) == 100 && a.n == 100,
		"B: (2009, 2019] replays exactly the 100 planted at 2010..2019 (%u)", a.n);
	memset(&a, 0, sizeof a);
	clretain_each(0, 5000, count_cb, &a);
	CHK(a.n == 300, "B: the whole table replays once (%u of 300)", a.n);

	/* C: 640 slots, 64 shards of 10; plant 2,000 over 20 s - most are
	 * evicted by the cap inside retain_s */
	clretain_init(100, 640);
	for (i = 0; i < 2000; i++) {
		snprintf(k, sizeof k, "c%05d", i);
		clretain_note("c", 1, k, strlen(k), (uint64_t)i + 1, 3000 + (unsigned)i / 100);
	}
	clretain_get_stats(&st, 3020);
	CHK(st.entries <= 640 && st.dropped_early >= 2000 - 640,
		"C: the cap holds (%llu of max 640) and early drops are counted (%llu)",
		st.entries, st.dropped_early);
	CHK(clretain_get("c", 1, "c01999", 6, 3020) == 2000 && clretain_get("c", 1, "c00000", 6, 3020) == 0,
		"C: the newest survives, the oldest went first");
	/* S392: a witness vouches only after the newest delete the cap lost -
	 * past retain_s's floor (2920), at the planting second of the newest
	 * entry evicted early (the table holds ~640 of 100 a second: ~3013) */
	{
		unsigned int cs = clretain_complete_since(3020);

		CHK(cs > 2920 && cs >= 3010 && cs < 3020,
			"C: complete-since moved to the newest early drop (%u, floor 2920)", cs);
	}

	/* C2: S443 - expiries counted apart from deletes.  Five of each on a
	 * fresh table; a delete noted for an expired key turns it into a
	 * delete; then expiries past the cap - their early drops are
	 * expiries, and a delete evicted early is not */
	{
		struct clretain_stats s0, s1;
		char kk[16];

		clretain_init(100, 640);
		clretain_get_stats(&s0, 4000);
		for (i = 0; i < 5; i++) {
			snprintf(kk, sizeof kk, "x%d", i);
			clretain_note_expiry("c", 1, kk, strlen(kk), 10, 4000, 0);
			snprintf(kk, sizeof kk, "d%d", i);
			clretain_note("c", 1, kk, strlen(kk), 10, 4000);
		}
		clretain_get_stats(&s1, 4000);
		CHK(s1.entries == 10 && s1.expiries == 5 &&
		    s1.noted_expiries - s0.noted_expiries == 5 &&
		    s1.noted - s0.noted == 10,
			"C2: 10 kept, 5 of them expiries; 10 noted, 5 expiries (%llu, %llu)",
			s1.entries, s1.expiries);
		clretain_note("c", 1, "x0", 2, 11, 4001);
		clretain_note_expiry("c", 1, "d0", 2, 11, 4001, 0);
		clretain_get_stats(&s1, 4001);
		CHK(s1.entries == 10 && s1.expiries == 4,
			"C2: a delete outranks an expiry, an expiry never a delete (%llu expiries)",
			s1.expiries);
		clretain_get_stats(&s0, 4001);
		for (i = 0; i < 2000; i++) {
			snprintf(k, sizeof k, "e%05d", i);
			clretain_note_expiry("c", 1, k, strlen(k), (uint64_t)i + 20, 4002 + (unsigned)i / 100, 0);
		}
		clretain_get_stats(&s1, 4022);
		/* the seeded ten are now 6 deletes (d0..d4, and x0) and 4
		 * expiries; whichever of the deletes the cap pushed out are the
		 * only early drops that are not expiries - since S444, none: an
		 * expiry never pushes out a delete (C3) */
		{
			unsigned long long dr = s1.dropped_early - s0.dropped_early,
				dx = s1.dropped_early_expiries - s0.dropped_early_expiries,
				dels_left = s1.entries - s1.expiries;

			CHK(dr >= 2000 - 640 && dels_left <= 6 &&
			    dx == dr - (6 - dels_left),
				"C2: 2,000 expiries past the cap - %llu dropped early, %llu "
				"of them expiries: the rest are the %llu seeded deletes it "
				"pushed out", dr, dx, 6 - dels_left);
		}
	}

	/* C3: S444 - an expiry never pushes out a delete.  100 deletes, then
	 * 2,000 expiries past the cap: every delete is still held, and every
	 * early drop is an expiry */
	{
		struct clretain_stats s0, s1;
		int held = 0;

		clretain_init(100, 640);
		clretain_get_stats(&s0, 5000);
		for (i = 0; i < 100; i++) {
			snprintf(k, sizeof k, "del%03d", i);
			clretain_note("c", 1, k, strlen(k), (uint64_t)i + 1, 5000);
		}
		for (i = 0; i < 2000; i++) {
			snprintf(k, sizeof k, "exp%05d", i);
			clretain_note_expiry("c", 1, k, strlen(k), (uint64_t)i + 200, 5001 + (unsigned)i / 100, 0);
		}
		for (i = 0; i < 100; i++) {
			snprintf(k, sizeof k, "del%03d", i);
			held += clretain_get("c", 1, k, strlen(k), 5021) == (uint64_t)i + 1;
		}
		clretain_get_stats(&s1, 5021);
		CHK(held == 100, "C3: 100 deletes, then 2,000 expiries past the cap - every delete still held (%d)", held);
		CHK(s1.dropped_early - s0.dropped_early >= 2000 + 100 - 640 &&
		    s1.dropped_early_expiries - s0.dropped_early_expiries ==
		    s1.dropped_early - s0.dropped_early,
			"C3: and every early drop was an expiry (%llu of %llu)",
			s1.dropped_early_expiries - s0.dropped_early_expiries,
			s1.dropped_early - s0.dropped_early);
		CHK(s1.entries <= 640 && s1.entries - s1.expiries == 100,
			"C3: the cap holds (%llu), 100 of them deletes", s1.entries);
		{
			struct acc a3;

			memset(&a3, 0, sizeof a3);
			clretain_each(0, 6000, count_cb, &a3);
			CHK(a3.n == s1.entries && a3.x == s1.expiries,
				"C3 S446: a replay walk says which are expiries (%u of %u)", a3.x, a3.n);
		}
		/* a delete still pushes out an expiry, and a delete when the
		 * shard has nothing else */
		for (i = 0; i < 1400; i++) {      /* ~22 a shard against ~8 expiries */
			snprintf(k, sizeof k, "d2_%04d", i);
			clretain_note("c", 1, k, strlen(k), (uint64_t)i + 5000, 5022);
		}
		clretain_get_stats(&s1, 5022);
		CHK(s1.entries <= 640 && s1.expiries < 10,
			"C3: 1,400 more deletes take the expiries' room first (%llu kept, %llu expiries)",
			s1.entries, s1.expiries);
	}

	/* ---- F: AT-5 */
	{
		struct acc af;
		struct fg f;
		unsigned long gone;

		clretain_init(100, 640);
		for (i = 1; i <= 10; i++) {
			snprintf(k, sizeof k, "n%02d", i);
			clretain_note_n("c", 1, k, strlen(k), (uint64_t)i + 100, 6000,
				0x77, 1, (uint64_t)i);
		}
		clretain_note("c", 1, "plain", 5, 99, 6000);   /* unnumbered */
		clretain_get_stats(&st, 6000);
		CHK(st.entries == 11 && st.numbered == 10, "F: 10 numbered of 11 held (%llu, %llu)",
			st.numbered, st.entries);
		gone = clretain_forget(0x77, 1, 6, 6001);
		clretain_get_stats(&st, 6001);
		CHK(gone == 6 && st.entries == 5 && st.numbered == 4 && st.forgotten == 6 &&
			clretain_get("c", 1, "n06", 3, 6001) == 0 && clretain_get("c", 1, "n07", 3, 6001) == 107 &&
			clretain_get("c", 1, "plain", 5, 6001) == 99,
			"F: forgotten through 6 - 1..6 gone, 7..10 and the unnumbered one kept (gone %lu)", gone);
		CHK(clretain_forget(0x77, 1, 6, 6002) == 0 && clretain_forget(0x77, 2, 99, 6002) == 0 &&
			clretain_forget(0x88, 1, 99, 6002) == 0,
			"F: again through 6, another incarnation, another origin: nothing");
		memset(&f, 0, sizeof f);
		clretain_forgot_each(fg_cb, &f);
		CHK(f.n == 1 && f.through == 6 && f.at == 6001,
			"F: how far: space 77/1 through 6, at 6001 (%u spaces)", f.n);
		/* the replay walk carries the numbering */
		memset(&af, 0, sizeof af);
		clretain_each(0, 7000, count_cb, &af);
		CHK(af.n == 5 && af.num == 4 && af.seqsum == 7 + 8 + 9 + 10,
			"F: the replay walk hands 4 numbered (seqs 7..10) and 1 without");
		CHK(clretain_forget(0x77, 1, 10, 6003) == 4, "F: through 10: 7..10 go");
		/* arrival order, in one shard: a late lower one waits for the head */
		{
			char ks[4][16];
			unsigned int nk = 1, tgt = shard_of("c", "o0");

			strcpy(ks[0], "o0");
			for (i = 1; nk < 4; i++) {
				snprintf(k, sizeof k, "o%d", i);
				if (shard_of("c", k) == tgt)
					strcpy(ks[nk++], k);
			}
			clretain_note_n("c", 1, ks[0], strlen(ks[0]), 400, 6003, 0x55, 1, 50);
			clretain_note_n("c", 1, ks[1], strlen(ks[1]), 401, 6003, 0x55, 1, 51);
			clretain_note_n("c", 1, ks[2], strlen(ks[2]), 402, 6003, 0x55, 1, 52);
			clretain_note_n("c", 1, ks[3], strlen(ks[3]), 403, 6003, 0x55, 1, 49);  /* late */
			CHK(clretain_forget(0x55, 1, 50, 6004) == 1 &&
				clretain_get("c", 1, ks[3], strlen(ks[3]), 6004) == 403,
				"F: seq 49 arrived after 50..52 in one shard: through 50 takes 50 and stops at 51");
			CHK(clretain_forget(0x55, 1, 52, 6005) == 3 &&
				clretain_get("c", 1, ks[3], strlen(ks[3]), 6005) == 0,
				"F: through 52: 51, 52 and the late 49 behind them");
		}
		/* a newer delete takes an entry over */
		clretain_note_n("c", 1, "take", 4, 600, 6006, 0x77, 1, 20);
		clretain_note("c", 1, "take", 4, 601, 6007);           /* newer, unnumbered */
		CHK(clretain_forget(0x77, 1, 99, 6008) == 0 && clretain_get("c", 1, "take", 4, 6008) == 601,
			"F: a newer unnumbered delete took the entry over - the mark does not forget it");
		clretain_note_n("c", 1, "move", 4, 700, 6009, 0x77, 1, 30);
		clretain_note_n("c", 1, "move", 4, 701, 6010, 0x99, 5, 3); /* newer, another space */
		CHK(clretain_forget(0x77, 1, 99, 6011) == 0 && clretain_get("c", 1, "move", 4, 6011) == 701 &&
			clretain_forget(0x99, 5, 3, 6011) == 1,
			"F: a newer numbered one moves the entry to its space - forgotten there, not in the old");
		clretain_note_n("c", 1, "older", 5, 800, 6012, 0x77, 1, 40);
		clretain_note_n("c", 1, "older", 5, 799, 6013, 0x99, 5, 4); /* OLDER: keeps the first */
		CHK(clretain_forget(0x99, 5, 99, 6014) == 0 && clretain_forget(0x77, 1, 40, 6014) == 1,
			"F: an older delete noted after a newer one changes nothing");
		/* the expiry hold */
		clretain_init(100, 640);
		clretain_note_expiry("c", 1, "x1", 2, 1, 7000, 0);
		clretain_note_expiry("c", 1, "x2", 2, 2, 7008, 0);
		clretain_note("c", 1, "d1", 2, 3, 7000);
		clretain_note_expiry("c", 1, "xk", 2, 4, 7000, 1);   /* (c): shortened */
		clretain_expire_expiries(7010, 10);
		clretain_get_stats(&st, 7010);
		CHK(st.entries == 3 && st.expiries == 2 && st.forgotten_expiries == 1 &&
			clretain_get("c", 1, "d1", 2, 7010) == 3 && clretain_get("c", 1, "x2", 2, 7010) == 2 &&
			clretain_get("c", 1, "xk", 2, 7010) == 4,
			"F: an expiry held 10 s goes at 7010; the younger one, the delete and a KEPT expiry (shortened TTL) stay");
		/* a kept note over a plain one keeps it; a plain note over a kept one changes nothing */
		clretain_note_expiry("c", 1, "x2", 2, 5, 7011, 1);
		clretain_note_expiry("c", 1, "xk", 2, 6, 7011, 0);
		clretain_expire_expiries(7100, 10);
		CHK(clretain_get("c", 1, "x2", 2, 7100) == 5 && clretain_get("c", 1, "xk", 2, 7100) == 6,
			"F: once kept, kept - by a later note either way");
		{
			struct acc ak;

			memset(&ak, 0, sizeof ak);
			clretain_each(0, 8000, kind_cb, &ak);
			CHK(ak.x == 2, "F: the replay walk says kept (2) for both kept expiries (%u)", ak.x);
		}
	}

	/* D: expire */
	clretain_init(100, 640);
	clretain_note("c", 1, "old", 3, 1, 4000);
	clretain_note("c", 1, "new", 3, 2, 4090);
	clretain_expire(4100);
	clretain_get_stats(&st, 4100);
	CHK(st.entries == 1 && clretain_get("c", 1, "new", 3, 4100) == 2,
		"D: expire at 4100 dropped the 4000 entry and kept the 4090 one");
	CHK(clretain_complete_since(4100) == 4000,
		"D: S392 - nothing lost: complete since retain_s's floor (%u, want 4000)",
		clretain_complete_since(4100));

	/* ---- G: AT-8 */
	{
		struct gn g;
		char ks[11][16];
		unsigned int nk = 1, tgt = shard_of("c", "q0");

		clretain_init(100, 640);
		clretain_note_n("c", 1, "g1", 2, 11, 9000, 0x66, 1, 1);
		clretain_note_n("c", 1, "g2", 2, 12, 9005, 0x66, 1, 2);
		clretain_note("c", 1, "u1", 2, 13, 9010);              /* unnumbered */
		memset(&g, 0, sizeof g);
		clretain_gone_each(gn_cb, &g);
		CHK(g.n == 0 && !clretain_lost_since(8000),
			"G: nothing dropped yet - no space gone, nothing lost since 8000");
		clretain_forget(0x66, 1, 1, 9050);
		memset(&g, 0, sizeof g);
		clretain_gone_each(gn_cb, &g);
		CHK(g.n == 1 && g.hi == 1, "G: forgotten through 1 by the marks: the space is gone through 1 (%u, %llu)",
			g.n, (unsigned long long)g.hi);
		clretain_expire(9106);         /* g2 (9005) goes, u1 (9010) not yet */
		memset(&g, 0, sizeof g);
		clretain_gone_each(gn_cb, &g);
		CHK(g.n == 1 && g.hi == 2 && !clretain_lost_since(9000),
			"G: seq 2 taken by the time rule unconfirmed: gone through 2; a numbered one is the marks' to judge, not lost_since's");
		clretain_expire(9111);         /* u1 */
		CHK(clretain_lost_since(9010) && !clretain_lost_since(9011),
			"G: the unnumbered one noted at 9010 taken by the time rule: lost since 9010, nothing since 9011");
		/* the cap (the whole table's, 10): an 11th numbered into one shard
		 * evicts that shard's oldest */
		clretain_init(100, 10);
		strcpy(ks[0], "q0");
		for (i = 1; nk < 11; i++) {
			snprintf(k, sizeof k, "q%d", i);
			if (shard_of("c", k) == tgt)
				strcpy(ks[nk++], k);
		}
		for (i = 0; i < 11; i++)
			clretain_note_n("c", 1, ks[i], strlen(ks[i]), (uint64_t)i + 1, 9200 + i, 0x44, 3,
				(uint64_t)i + 1);
		memset(&g, 0, sizeof g);
		clretain_gone_each(gn_cb, &g);
		CHK(g.n == 1 && g.hi == 1 && clretain_lost_since(9200) && !clretain_lost_since(9201),
			"G: the cap evicted seq 1 (noted at 9200) unconfirmed: gone through 1, lost since 9200, nothing since 9201");
	}

	/* ---- E: memory at the default cap ------------------------------- */
	r0 = rss_kb();
	clretain_init(900, 250000);
	for (i = 0; i < 250000; i++) {
		snprintf(k, sizeof k, "call:%08d:branch", i);  /* 20 bytes */
		clretain_note("0", 1, k, strlen(k), (uint64_t)i + 1, 10000);
	}
	r1 = rss_kb();
	clretain_get_stats(&st, 10000);
	printf("  --   E: 250,000 retained, 20-byte keys: the table reports %.1f MB; "
		"the process grew %.1f MB (malloc headers and the hash included)\n",
		st.bytes / 1048576.0, (r1 - r0) / 1024.0);
#if defined(__SANITIZE_ADDRESS__)
#define UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UNDER_ASAN 1
#endif
#endif
#ifdef UNDER_ASAN
	/* ASan's redzones and its quarantine of the entries C evicted make
	 * the process's growth no measure of the table (v0.7.2-rc4, GitHub
	 * check-asan core1: 40.8 MB for the table's 20.7) - the bound is the
	 * release build's, where the figure is the one the GUIDE quotes */
	CHK(st.entries == 250000, "E: 250,000 held (the RSS bound is the release build's, not ASan's)");
#else
	CHK(st.entries == 250000 && (r1 - r0) / 1024 < 40,
		"E: 250,000 held, and under 40 MB of process memory at the default cap");
#endif
	printf("clretaintest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
