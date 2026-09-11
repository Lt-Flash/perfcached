/*
 * slottest.c — src/pc_slot.h, the key -> slot function placement rests on.
 *
 * Checked here rather than through a live cluster because everything
 * else about placement is downstream of this: if the slot is wrong the
 * owner is wrong, and a cluster test would report that as "the nodes
 * disagree" without ever pointing here.
 *
 * The reference values are Redis's own.  They are hard-coded rather
 * than recomputed, because a test that recomputes the value the same
 * way the code does proves only that the code is self-consistent.
 */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "pc_slot.h"
#include "pc_mix.h"

static int pass, fail;
static void ok(const char *m) { pass++; printf("  ok   %s\n", m); }
static void bad(const char *m) { fail++; printf("  FAIL %s\n", m); }

static void eq(const char *what, unsigned got, unsigned want)
{
	char b[160];

	snprintf(b, sizeof(b), "%s = %u", what, want);
	if (got == want)
		ok(b);
	else {
		snprintf(b, sizeof(b), "%s: got %u, want %u", what, got, want);
		bad(b);
	}
}

/* The tables are compile-time constants, so the typo they could hide
 * has to be caught HERE.  All 2048 entries are re-derived - T[0] from
 * the polynomial bitwise, T[k] from T[k-1] - and compared.  This is the
 * check that used to run on every call, moved to build time.  A single
 * wrong digit would still look like a hash and still spread keys, and
 * would disagree with every Redis client for one byte value in 256. */
static void t_table(void)
{
	unsigned k, n, j, wrong = 0, fk = 0, fn = 0;
	uint16_t ref[8][256];

	for (n = 0; n < 256; n++) {
		uint16_t c = (uint16_t)(n << 8);

		for (j = 0; j < 8; j++)
			c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021)
			                 : (uint16_t)(c << 1);
		ref[0][n] = c;
	}
	for (k = 1; k < 8; k++)
		for (n = 0; n < 256; n++)
			ref[k][n] = (uint16_t)((ref[k - 1][n] << 8) ^
				ref[0][ref[k - 1][n] >> 8]);

	for (k = 0; k < 8; k++) {
		for (n = 0; n < 256; n++) {
			if (ref[k][n] == pc_crc16_tab[k][n])
				continue;
			if (!wrong) { fk = k; fn = n; }
			wrong++;
		}
	}
	if (!wrong)
		ok("all 2048 slice-table entries match the 0x1021 polynomial");
	else {
		char b[144];

		snprintf(b, sizeof(b), "%u of 2048 table entries wrong, first "
			"at [%u][%u]: have 0x%04X want 0x%04X - hand-edited",
			wrong, fk, fn, pc_crc16_tab[fk][fn], ref[fk][fn]);
		bad(b);
	}
}

/* Slicing must equal the plain byte-at-a-time form for EVERY length,
 * not just the aligned ones: the 8-byte step and the tail loop are
 * different code, and a key of 25 bytes uses both.  Lengths 0..64 plus
 * pseudo-random content, compared against an independent
 * implementation written here. */
static void t_slice(void)
{
	unsigned char buf[80];
	unsigned seed = 2463534242u;
	size_t n;
	int bad_len = -1;

	for (n = 0; n < sizeof(buf); n++) {
		seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
		buf[n] = (unsigned char)(seed >> 16);
	}
	for (n = 0; n <= 64 && bad_len < 0; n++) {
		uint16_t want = 0;
		size_t i;
		unsigned j;

		for (i = 0; i < n; i++) {          /* bitwise, no tables */
			want ^= (uint16_t)(buf[i] << 8);
			for (j = 0; j < 8; j++)
				want = (want & 0x8000) ?
					(uint16_t)((want << 1) ^ 0x1021) :
					(uint16_t)(want << 1);
		}
		if (pc_crc16((const char *)buf, n) != want)
			bad_len = (int)n;
	}
	if (bad_len < 0)
		ok("slice-by-8 equals the bitwise CRC at every length 0..64");
	else {
		char b[128];

		snprintf(b, sizeof(b), "slice-by-8 differs from the bitwise "
			"CRC at length %d", bad_len);
		bad(b);
	}
}

/* the CRC16 check value every CCITT/XMODEM implementation publishes */
static void t_crc(void)
{
	eq("crc16(\"123456789\")", pc_crc16("123456789", 9), 0x31C3);
	eq("crc16(\"\")", pc_crc16("", 0), 0);
}

/* Redis's own slots.  Verified against redis-cli CLUSTER KEYSLOT. */
static void t_vectors(void)
{
	struct { const char *k; unsigned s; } v[] = {
		{ "foo", 12182 }, { "bar", 5061 }, { "hello", 866 },
		{ "somekey", 11058 }, { "user:1000", 1649 },
		{ "", 0 },
	};
	size_t i;

	for (i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		char b[64];

		snprintf(b, sizeof(b), "slot(\"%s\")", v[i].k);
		eq(b, pc_key_slot(v[i].k, strlen(v[i].k)), v[i].s);
	}
}

/* the hash-tag rule, including the edges Redis specifies but that are
 * easy to get subtly wrong - each one is a key someone deliberately
 * grouped, so disagreeing here breaks exactly the intent */
static void t_tags(void)
{
	unsigned a = pc_key_slot("{42}", 4);

	eq("slot(\"user:{42}:name\") groups with \"{42}\"",
		pc_key_slot("user:{42}:name", 14), a);
	eq("slot(\"{42}:other\") groups too", pc_key_slot("{42}:other", 10), a);
	/* an EMPTY tag is not a tag: the whole key is hashed */
	eq("slot(\"{}\") hashes the whole key",
		pc_key_slot("{}", 2), pc_crc16("{}", 2) % PC_SLOTS);
	eq("slot(\"a{}b\") hashes the whole key",
		pc_key_slot("a{}b", 4), pc_crc16("a{}b", 4) % PC_SLOTS);
	/* an unclosed brace is not a tag either */
	eq("slot(\"x{\") hashes the whole key",
		pc_key_slot("x{", 2), pc_crc16("x{", 2) % PC_SLOTS);
	/* only the FIRST tag counts */
	eq("slot(\"{a}{b}\") uses the first tag",
		pc_key_slot("{a}{b}", 6), pc_crc16("a", 1) % PC_SLOTS);
	/* keys are bytes, not C strings: an embedded NUL must be hashed */
	eq("slot() hashes past an embedded NUL",
		pc_key_slot("a\0b", 3), pc_crc16("a\0b", 3) % PC_SLOTS);
	if (pc_key_slot("a\0b", 3) != pc_key_slot("a", 1))
		ok("an embedded NUL does not truncate the key");
	else
		bad("key hashing stopped at an embedded NUL");
}

/* The table is built lazily and workers are THREADS, so this checks
 * that concurrent first-use is correct.
 *
 * Be clear about what it does NOT prove.  The reason pc_slot.h gives
 * the table per-thread storage is a weak-memory hazard: one thread
 * seeing `built` set before the table's stores are visible.  x86 does
 * not reorder store-store, so a SHARED table passes this test here -
 * verified by putting one back, 5 runs, 18/18 each time.  Only a
 * weakly-ordered target (ARM) could fail it.  The per-thread table
 * removes the hazard by construction; this test guards the ordinary
 * correctness of lazy init, and the ARM case rests on the argument in
 * pc_slot.h, not on this assertion passing. */
#define NTHREAD 8
#define NITER   20000
static void *hammer(void *arg)
{
	long bad_here = 0;
	int i;

	(void)arg;
	for (i = 0; i < NITER; i++) {
		char k[32];
		int n = snprintf(k, sizeof(k), "key:%d", i);

		if (pc_key_slot(k, (size_t)n) !=
		        (uint16_t)(pc_crc16(k, (size_t)n) % PC_SLOTS))
			bad_here++;
		if (pc_key_slot("foo", 3) != 12182)
			bad_here++;
	}
	return (void *)bad_here;
}

static void t_threads(void)
{
	pthread_t th[NTHREAD];
	long total = 0;
	int i;

	for (i = 0; i < NTHREAD; i++) {
		if (pthread_create(&th[i], NULL, hammer, NULL) != 0) {
			bad("could not start the thread race");
			return;
		}
	}
	for (i = 0; i < NTHREAD; i++) {
		void *r;

		pthread_join(th[i], &r);
		total += (long)r;
	}
	if (total == 0)
		ok("8 threads x 20000 keys: every slot correct under "
			"concurrent lazy init");
	else {
		char b[96];

		snprintf(b, sizeof(b), "%ld wrong slot(s) under thread race",
			total);
		bad(b);
	}
}


/* pc_hrw_mix(): the rendezvous weight, and the second half of the
 * placement contract.  These values were captured from the THREE
 * byte-identical copies that lived in cluster.c, lib/perfd.c and
 * clplace.c BEFORE they were collapsed into pc_mix.h, so they prove the
 * collapse changed nothing.  (Exception: the 198.51.100.224 row's
 * address was swapped to a documentation address after the collapse and
 * its value re-derived from pc_mix.h; the other four rows still carry
 * the pre-collapse capture.)  They are also the guard against anyone
 * "improving" the mix later: a better hash here would move every key in
 * every deployment, silently, because both sides would change together
 * and agree perfectly on the new answer. */
static void t_mix(void)
{
	struct { uint64_t kh; const char *ip; uint16_t port; uint64_t want; } v[] = {
		{ 1ull,     "127.0.0.1",       6379, 0x0fbaa8994a259be2ull },
		{ 12182ull, "198.51.100.224", 11211, 0x2f3c79ed262c1069ull },
		{ 16383ull, "192.168.1.1",    65535, 0xa884305c3fdec348ull },
		{ 5061ull,  "10.0.0.1",           1, 0x1d8a0b71b50d3f89ull },
		{ 0xFFFFFFFFFFFFFFFFull, "255.255.255.255", 65535,
			0x1cff94498dab36a2ull },
	};
	size_t i;

	for (i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		uint64_t got = pc_hrw_mix(v[i].kh, inet_addr(v[i].ip),
			htons(v[i].port));
		char b[160];

		if (got == v[i].want) {
			snprintf(b, sizeof(b), "mix(%llu, %s:%u)",
				(unsigned long long)v[i].kh, v[i].ip, v[i].port);
			ok(b);
		} else {
			snprintf(b, sizeof(b), "mix(%llu, %s:%u): got %016llx,"
				" want %016llx - PLACEMENT HAS MOVED",
				(unsigned long long)v[i].kh, v[i].ip, v[i].port,
				(unsigned long long)got,
				(unsigned long long)v[i].want);
			bad(b);
		}
	}
	/* the address decides the weight: two nodes must not tie */
	if (pc_hrw_mix(42, inet_addr("10.0.0.1"), htons(11211)) !=
	        pc_hrw_mix(42, inet_addr("10.0.0.2"), htons(11211)))
		ok("one slot weighs differently on two addresses");
	else
		bad("two addresses give the same weight - argmax would tie");
}

/* placement quality: the slot must spread, or balance claims are void */
static void t_spread(void)
{
	int hist[16] = { 0 }, i, lo = NITER, hi = 0;
	char k[32];

	for (i = 0; i < 100000; i++) {
		int n = snprintf(k, sizeof(k), "user:%d:session", i);

		hist[pc_key_slot(k, (size_t)n) * 16 / PC_SLOTS]++;
	}
	for (i = 0; i < 16; i++) {
		if (hist[i] < lo) lo = hist[i];
		if (hist[i] > hi) hi = hist[i];
	}
	/* 100k keys over 16 buckets: 6250 each, +-5% is generous */
	if (lo > 5900 && hi < 6600)
		ok("100k keys spread evenly over the slot space");
	else {
		char b[96];

		snprintf(b, sizeof(b), "slot spread is skewed: %d..%d per "
			"16th (want ~6250)", lo, hi);
		bad(b);
	}
}

int main(void)
{
	t_table();
	t_slice();
	t_crc();
	t_vectors();
	t_tags();
	t_threads();
	t_mix();
	t_spread();
	printf("slottest: %d passed, %d failed\n", pass, fail);
	return fail != 0;
}
