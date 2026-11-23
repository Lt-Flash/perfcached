/* addexptest.c - INCR, EXPIRE and DEL see what a read sees: a record past its deadline that
 * the sweep has not reaped yet is absent to them too; INCR of a counter needs no new cell. */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../src/compat/compat.h"
#include "../src/compat/mem/mem.h"
#include "../src/compat/str.h"
#include "../src/compat/timer.h"
#include "../src/core/pcache_htable.h"
#include "../src/core/pcache_arena.h"

extern int pcache_arena_hugepage_mb;
void pcache_mem_probe(void);

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static str S(const char *s)
{
	str k;

	k.s = (char *)s;
	k.len = (int)strlen(s);
	return k;
}

static const char *get(pcache_htable_t *ht, const char *k)
{
	static char buf[64];
	unsigned int vlen = 0, need = 0;
	str key = S(k);
	int rc = pcache_ht_fetch_buf(ht, &key, buf, sizeof buf - 1, &vlen, &need);

	if (rc < 0)
		return "";
	if (rc == 1)
		return "(counter)";
	buf[vlen] = 0;
	return buf;
}

int main(void)
{
	pcache_htable_t *ht;
	unsigned int now, eff = 1;
	long long nv = 0;
	str k, v;
	int i;

	compat_thread_register(0);
	pcache_mem_probe();
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = 16;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_ht_default_stat_slots(32);
	ht = pcache_htable_new(6);
	if (!ht) {
		printf("no table\n");
		return 2;
	}
	now = get_ticks();
	k = S("a-cnt");
	pcache_ht_add(ht, &k, 5, now + 1, &nv);
	k = S("a-str");
	v = S("7");
	pcache_ht_store(ht, &k, &v, now + 1);
	k = S("b-key");
	v = S("v");
	pcache_ht_store(ht, &k, &v, now + 1);
	k = S("d-key");
	pcache_ht_store(ht, &k, &v, now + 1);
	k = S("d-live");
	pcache_ht_store(ht, &k, &v, 0);
	for (i = 0; i < 40 && get_ticks() <= now + 1; i++)
		usleep(100000);         /* past the deadline; nothing sweeps here */
	CHK(!strcmp(get(ht, "a-cnt"), "") && !strcmp(get(ht, "a-str"), "") &&
		!strcmp(get(ht, "b-key"), ""), "0 all three read absent (control)");

	/* A: an accumulate starts from nothing */
	k = S("a-cnt");
	CHK(pcache_ht_add_ex(ht, &k, 1, PCACHE_EXP_PRESERVE, &nv, &eff) == 0 &&
		nv == 1 && eff == 0,
		"A1 INCR of an expired counter answers 1, a new immortal one (%lld, expiry %u; 6 is the dead value)",
		nv, eff);
	k = S("a-str");
	CHK(pcache_ht_add_ex(ht, &k, 1, PCACHE_EXP_PRESERVE, &nv, &eff) == 0 && nv == 1,
		"A2 so does INCR of an expired numeric string (%lld)", nv);
	CHK(pcache_ht_add_ex(ht, &k, 1, PCACHE_EXP_PRESERVE, &nv, &eff) == 0 && nv == 2,
		"A3 and the next one counts on from it (%lld)", nv);

	/* B: a TTL re-arm does not bring the value back */
	k = S("b-key");
	CHK(pcache_ht_touch(ht, &k, now + 1000) == 0,
		"B1 EXPIRE of an expired key answers 0");
	CHK(!strcmp(get(ht, "b-key"), ""), "B2 and the key still reads absent");

	/* D: a delete still removes it, and is told it was absent to a read */
	k = S("d-key");
	CHK(pcache_ht_remove(ht, &k) == 1 && pcache_last_del_dead == 1,
		"D1 DEL of an expired key removes it and is told it was past its deadline (dead %d)",
		pcache_last_del_dead);
	CHK(pcache_ht_remove(ht, &k) == 0, "D2 and the record is gone");
	k = S("d-live");
	CHK(pcache_ht_remove(ht, &k) == 1 && pcache_last_del_dead == 0,
		"D3 DEL of a live key is told it was live (dead %d)", pcache_last_del_dead);

	/* E: a sum past int64 is refused (-3) and the stored value kept, in place or on conversion */
	k = S("e-cnt");
	pcache_ht_add(ht, &k, LLONG_MAX, 0, &nv);
	CHK(pcache_ht_add_ex(ht, &k, 1, 0, &nv, &eff) == -3, "E1 LLONG_MAX + 1 is refused as an overflow");
	CHK(pcache_ht_add_ex(ht, &k, 0, 0, &nv, &eff) == 0 && nv == LLONG_MAX,
		"E2 and the counter kept (%lld)", nv);
	k = S("e-neg");
	pcache_ht_add(ht, &k, LLONG_MIN, 0, &nv);
	CHK(pcache_ht_add_ex(ht, &k, -1, 0, &nv, &eff) == -3, "E3 LLONG_MIN - 1 too");
	k = S("e-str");
	v = S("9223372036854775807");
	pcache_ht_store(ht, &k, &v, 0);
	CHK(pcache_ht_add_ex(ht, &k, 1, 0, &nv, &eff) == -3 &&
		!strcmp(get(ht, "e-str"), "9223372036854775807"),
		"E4 a numeric string at LLONG_MAX plus 1 - refused, the string kept");

	/* C: the arena at its ceiling - every cell of the counter's class taken */
	k = S("c-cnt");
	pcache_ht_add(ht, &k, 1, 0, &nv);
	pcache_arena_max_bytes = 1;
	for (i = 0; i < 4000000 && pcache_cell_alloc(PCACHE_REC_SIZE(k.len, 8)); i++)
		;
	k = S("c-new");
	CHK(pcache_ht_add(ht, &k, 1, 0, &nv) < 0, "C1 the arena is full: a new counter is refused");
	k = S("c-cnt");
	CHK(pcache_ht_add(ht, &k, 1, 0, &nv) == 0 && nv == 2,
		"C2 an existing counter still counts, in place (%lld)", nv);

	printf("addexptest: %s\n", fails ? "FAILED" : "all ok");
	return fails ? 1 : 0;
}
