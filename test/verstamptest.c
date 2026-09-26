/* verstamptest.c - S234: a local write's version is stamped under the
 * bucket lock, where the record is published.
 *
 * The replacement record is built before lock_get (3.5b rule 3), and so
 * was its version: a tick taken while building could reach the bucket
 * after a later one.  Two workers on one key, A ticking 100 and B 101, B
 * storing first - A stored 100 over 101, this node kept A's value, and
 * every peer refused A's push as older and kept B's.  The same gap sat
 * between a delete and its tombstone, ticked after the lock was dropped.
 *
 * A thread race cannot be pinned, so the test pins it: it includes the
 * table and holds the bucket's lock itself.  The writer starts, reaches
 * the lock and waits (the pre-S234 code has ticked by then); the test,
 * still holding the lock, commits "the other worker's" version to the
 * key - exactly what B did - and lets go.  The writer's version must land
 * above it.  Deterministic, milliseconds.
 *
 *   1. replace: a value that outgrows its cell (the path that kept the
 *      early tick)
 *   2. insert: an absent key, against a delete stamped under the lock
 *   3. delete then set: the tombstone's version is below a set that lands
 *      after the delete (-DOLD_REMOVE builds the pre-S234 delete verb:
 *      remove, then tick - the fail-first for this arm)
 *   4. an accumulate on a counter in the overflow leg moves its version
 *      (that path handed out the discarded prebuilt record's tick and
 *      left the record's own number where it was)
 *   5. every write path leaves a non-zero, rising version that matches
 *      pcache_last_ver - the control that moving the stamps lost none
 *
 * FAIL-FIRST: arms 1-4 fail against the pre-S234 table. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../src/core/pcache_htable.c"

extern int pcache_arena_hugepage_mb;
void pcache_mem_probe(void);

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

struct writer {
	pcache_htable_t *ht;
	str key, val;
	int rc, done;
	unsigned long long ver;           /* pcache_last_ver, in its thread */
};

static void *write_one(void *arg)
{
	struct writer *w = arg;

	w->rc = pcache_ht_store(w->ht, &w->key, &w->val, 0);
	w->ver = pcache_last_ver;
	__atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
	return NULL;
}

static unsigned long long ver_of(pcache_htable_t *ht, const str *k)
{
	unsigned long long v = 0;

	return pcache_ht_getver(ht, k, &v) == 0 ? v : 0;
}

/* hold @k's bucket lock while a writer stores @val to it; under the lock,
 * @commit stands in for the other worker.  Returns the version it put
 * there; *@w reports the writer's. */
static unsigned long long race(pcache_htable_t *ht, const str *k,
		const str *val, int existing, struct writer *w, int *waited)
{
	unsigned int hash = pcache_key_hash(k), idx;
	uint64_t route;
	pcache_bucket_t *b;
	pthread_t th;
	unsigned long long other;
	int i;

	idx = route_idx(ht, hash, &route);
	b = bucket_at(ht, idx);
	memset(w, 0, sizeof *w);
	w->ht = ht; w->key = *k; w->val = *val;

	lock_get(&b->lock);
	pthread_create(&th, NULL, write_one, w);
	usleep(100000);                    /* it hashes, builds, and waits */
	*waited = !__atomic_load_n(&w->done, __ATOMIC_ACQUIRE);
	other = pc_lamport_tick();
	if (existing) {
		/* B's write to the same key, committed under this lock first */
		i = find_slot(b, k, hash, tag_of(hash));
		if (i >= 0)
			b->slot[i]->ver = other;
		else
			other = 0;
	}
	/* else: a delete of the absent key, stamped under this lock */
	lock_release(&b->lock);
	pthread_join(th, NULL);
	return other;
}

int main(void)
{
	pcache_htable_t *ht;
	struct writer w;
	char big[900], kb[24];
	unsigned long long other, v, v0, vt, prev;
	long long nv;
	unsigned int ee;
	int waited, i, zero = 0, back = 0, mism = 0, found = 0;
	str k, val;

	pcache_mem_probe();
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("arena init failed\n");
		return 2;
	}
	pcache_ht_default_stat_slots(32);
	ht = pcache_htable_new(4);
	if (!ht) {
		printf("create failed\n");
		return 2;
	}
	memset(big, 'x', sizeof big);

	/* 1. replace */
	k.s = "k1"; k.len = 2;
	val.s = "a"; val.len = 1;
	if (pcache_ht_store(ht, &k, &val, 0) != 0)
		return 2;
	val.s = big; val.len = (int)sizeof big;
	other = race(ht, &k, &val, 1, &w, &waited);
	v = ver_of(ht, &k);
	CHK(waited && other && w.rc == 0,
		"1. the writer waited on the held lock while another write "
		"to the key committed version %llu", other);
	CHK(v > other,
		"   a value that outgrew its cell was stamped above it: "
		"stored %llu over %llu", v, other);
	CHK(w.ver == v, "   and the writer reports the version it stored "
		"(%llu)", w.ver);

	/* 2. insert */
	k.s = "k2";
	val.s = "b"; val.len = 1;
	other = race(ht, &k, &val, 0, &w, &waited);
	v = ver_of(ht, &k);
	CHK(waited && w.rc == 0,
		"2. an insert waited while a delete of its key was stamped "
		"%llu under the lock", other);
	CHK(v > other, "   the insert landed above the delete: %llu over "
		"%llu", v, other);

	/* 3. delete, then a set that lands after it */
	k.s = "k3";
	val.s = "c"; val.len = 1;
	if (pcache_ht_store(ht, &k, &val, 0) != 0)
		return 2;
	v0 = ver_of(ht, &k);
#ifdef OLD_REMOVE
	(void)pcache_ht_remove(ht, &k);
	(void)pcache_ht_store(ht, &k, &val, 0);   /* lands in between */
	vt = pc_lamport_tick();                   /* the verb's tombstone */
#else
	(void)pcache_ht_remove_ver(ht, &k, &vt);
	(void)pcache_ht_store(ht, &k, &val, 0);   /* lands after it */
#endif
	v = ver_of(ht, &k);
	CHK(vt > v0, "3. the tombstone (%llu) is newer than the record it "
		"removed (%llu)", vt, v0);
	CHK(v > vt, "   and older than a set that landed after the delete "
		"(%llu) - a peer applies both and keeps the key", v);

	/* 4. a counter in the overflow leg: fill a 16-bucket table */
	for (i = 0; i < 400; i++) {
		k.len = snprintf(kb, sizeof kb, "c%d", i);
		k.s = kb;
		if (pcache_ht_add_ex(ht, &k, 1, 0, &nv, &ee) != 0)
			return 2;
	}
	for (i = 0; i < 400 && !found; i++) {
		k.len = snprintf(kb, sizeof kb, "c%d", i);
		k.s = kb;
		lock_get(&ht->ovf_lock);
		found = ovf_find(ht, &k, pcache_key_hash(&k), NULL) != NULL;
		lock_release(&ht->ovf_lock);
	}
	CHK(found, "4. counter %.*s lives in the overflow leg (%u records "
		"there)", k.len, k.s, ht->ovf_count);
	v0 = ver_of(ht, &k);
	(void)pcache_ht_add_ex(ht, &k, 5, 0, &nv, &ee);
	v = ver_of(ht, &k);
	CHK(v > v0 && v == pcache_last_ver,
		"   an accumulate on it moved its version %llu -> %llu, the "
		"version it reported (%llu)", v0, v, pcache_last_ver);

	/* 5. every path: insert, in-place, TTL-only, replace, incr create,
	 * incr in place, string conversion */
	prev = 0;
	for (i = 0; i < 7; i++) {
		k.s = "p"; k.len = 1;
		switch (i) {
		case 0: val.s = "1"; val.len = 1; break;       /* insert */
		case 1: val.s = "2"; val.len = 1; break;       /* in place */
		case 2: break;                                  /* same bytes */
		case 3: val.s = big; val.len = (int)sizeof big; break;
		}
		if (i < 4)
			(void)pcache_ht_store(ht, &k, &val, 0);
		else if (i == 4) {
			k.s = "q";
			(void)pcache_ht_add_ex(ht, &k, 1, 0, &nv, &ee);
		} else if (i == 5) {
			k.s = "q";
			(void)pcache_ht_add_ex(ht, &k, 1, 0, &nv, &ee);
		} else {
			val.s = "7"; val.len = 1;
			(void)pcache_ht_store(ht, &k, &val, 0);
			(void)pcache_ht_add_ex(ht, &k, 1, 0, &nv, &ee);
		}
		v = ver_of(ht, &k);
		if (!v)
			zero++;
		if (v <= prev)
			back++;
		if (v != pcache_last_ver)
			mism++;
		prev = v;
	}
	CHK(!zero && !back && !mism,
		"5. seven write paths each left a non-zero, rising version "
		"matching pcache_last_ver (%d zero, %d not rising, %d "
		"mismatched)", zero, back, mism);

	printf("verstamptest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
