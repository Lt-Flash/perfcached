/* clordertest.c - S461: a replicated set and delete of one key on two receive
 * threads; the hook runs the other side inside each side's gap, deterministically */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/compat/compat.h"
#include "../src/compat/mem/mem.h"
#include "../src/compat/str.h"
#include "../src/core/pcache_htable.h"
#include "../src/core/pcache_arena.h"
#include "../src/clorder.h"

extern int pcache_arena_hugepage_mb;
void pcache_mem_probe(void);

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static pcache_htable_t *ht, *sh;
static str K = { "s461-key", 8 };
static unsigned long long tomb;        /* the key's tombstone, as the neg cache holds it */
static unsigned long long gap_ver;     /* what the other side applies in the gap */
static int fired;

static int refuse(void *arg, unsigned long long ver)
{
	(void)arg;
	return tomb && ver <= tomb;
}

static unsigned long long tomb_ver(void *arg)
{
	(void)arg;
	return tomb;
}

static const struct clorder_tomb T = { refuse, tomb_ver, NULL };

static int set(unsigned long long ver)
{
	str v = { "v", 1 };

	return clorder_set(ht, &K, &v, 0, 0, ver, &T);
}

/* planted before the remove, as handle_tombstone does */
static int del(unsigned long long ver)
{
	if (ver > tomb)
		tomb = ver;
	return clorder_del(ht, sh, &K, ver);
}

static unsigned long long held(pcache_htable_t *t)
{
	str v;
	unsigned int e;
	unsigned long long ver = 0;

	if (pcache_ht_fetch_ver(t, &K, &v, &e, &ver) != 0)
		return 0;
	free(v.s);
	return ver;
}

static void other_side(int side, const str *k)
{
	(void)k;
	clorder_test_hook = NULL;
	fired++;
	if (side == CLORDER_SET)
		(void)del(gap_ver);
	else
		(void)set(gap_ver);
}

static void fresh(int with_shadow)
{
	ht = pcache_htable_new(4);
	sh = with_shadow ? pcache_htable_new(4) : NULL;
	tomb = 0;
	fired = 0;
	clorder_test_hook = NULL;
	if (!ht || (with_shadow && !sh)) {
		printf("table create failed\n");
		exit(2);
	}
}

static void in_gap(unsigned long long ver)
{
	gap_ver = ver;
	clorder_test_hook = other_side;
}

static void put_copy(pcache_htable_t *t, unsigned long long ver)
{
	str v = { "v", 1 };

	if (pcache_ht_store_ver(t, &K, &v, 0, 0, ver) != 0) {
		printf("setup store failed\n");
		exit(2);
	}
}

int main(void)
{
	int rc;

	pcache_mem_probe();
	pcache_arena_hugepage_mb = 16;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}

	printf("control - one after the other:\n");
	fresh(0);
	set(5);
	del(6);
	CHK(held(ht) == 0, "set 5 then delete 6: gone (holds %llu)", held(ht));
	fresh(0);
	del(6);
	rc = set(5);
	CHK(rc == CLORDER_REFUSED && held(ht) == 0,
		"delete 6 then set 5: refused (rc %d, holds %llu)", rc, held(ht));
	fresh(0);
	set(7);
	del(6);
	CHK(held(ht) == 7, "set 7 then delete 6: 7 stays (holds %llu)", held(ht));
	fresh(1);
	put_copy(ht, 5);
	put_copy(sh, 5);
	del(6);
	CHK(held(ht) == 0 && held(sh) == 0,
		"delete 6 over 5: gone from the table and the resize shadow "
		"(%llu, %llu)", held(ht), held(sh));

	printf("in the gap - what two receive threads can do:\n");
	fresh(0);
	in_gap(6);
	rc = set(5);
	CHK(fired == 1, "the delete ran inside the set's gap");
	CHK(held(ht) == 0, "set 5 with delete 6 in its gap: gone, not back "
		"(rc %d, holds %llu)", rc, held(ht));
	fresh(0);
	put_copy(ht, 5);
	in_gap(7);
	del(6);
	CHK(fired == 1, "the set ran inside the delete's gap");
	CHK(held(ht) == 7, "delete 6 with set 7 in its gap: 7 stays "
		"(holds %llu)", held(ht));
	fresh(0);
	in_gap(6);
	rc = set(7);
	CHK(fired == 1 && held(ht) == 7, "set 7 with delete 6 in its gap: 7 "
		"stays (rc %d, holds %llu)", rc, held(ht));

	printf("%s\n", fails ? "FAILED" : "PASSED");
	return fails ? 1 : 0;
}
