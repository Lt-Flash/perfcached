/*
 * selftest — S4: the M1 gate.
 *
 * Phase 1 runs the vendored startup selftests exactly as the module ran
 * them at mod_init: pcache_arena_selftest() on an own-backing 64M arena,
 * then pcache_htable_selftest().
 *
 * Phase 2 is the daemon's own addition: a cross-THREAD storm proving the
 * core's invariants hold between threads (the module only ever proved
 * them between processes):
 *   - 4 writers churn 2048 keys (store/remove/re-store) with values that
 *     carry their key and a generation number at BOTH ends - a reader
 *     that ever sees a mixed value has caught a torn read through the
 *     seqlock;
 *   - 4 readers hammer random keys and validate every hit;
 *   - 4+2 counter threads prove add/sub atomicity: the final value must
 *     be EXACTLY adds-subs;
 *   - a maintenance thread runs linear-hash growth and the expiry sweep
 *     concurrently (single-splitter, as designed) - the table starts at
 *     256 buckets and must grow under the storm;
 *   - a final single-threaded audit fetches every key (100% presence,
 *     all consistent) and iterates the table (exact entry count).
 *
 * Exit 0 = pass.  Any inconsistency prints and exits nonzero; a
 * watchdog alarm turns a livelock into a failure too.
 * `make selftest_broken` builds this same storm with the bucket locks
 * compiled out (PC_COMPAT_BROKEN_LOCKS) and must FAIL - proof the storm
 * detects what the locks prevent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "../src/compat/compat.h"
#include "../src/compat/str.h"
#include "../src/compat/timer.h"
#include "../src/core/pcache_mem.h"
#include "../src/core/pcache_arena.h"
#include "../src/core/pcache_htable.h"

extern char *pcache_backing_policy;
extern int pcache_arena_hugepage_mb;

#define NWRITE   4
#define NREAD    4
#define NCTR     4
#define NSUB     2
#define KEYS_PER_W 512
/* scale knobs, not macros: PC_SELFTEST_LIGHT=1 shrinks the storm for
 * qemu-user emulation in the S27 arch matrix (8-16x slower than metal;
 * the full storm would trip the 120s livelock alarm there).  The
 * counter-exactness check derives from these, so it stays exact. */
static unsigned int ROUNDS = 120;  /* vs the writers' unsigned r */
static int READ_ITER = 400000;
static int CTR_ADDS = 200000;
static int SUB_SUBS = 100000;

static pcache_htable_t *ht;
static int nfail;
static volatile int maint_stop;

#define FAIL(...) do { \
	__atomic_fetch_add(&nfail, 1, __ATOMIC_RELAXED); \
	printf("FAIL: " __VA_ARGS__); \
} while (0)

static int key_of(char *buf, int w, int j)
{
	return sprintf(buf, "k-%d-%03d", w, j);
}

/* value: V:<key>:<gen8>:<pad>:<gen8> - key + generation echoed at both
 * ends, torn reads cannot forge both */
static int val_of(char *buf, const char *key, int klen, unsigned gen, int j)
{
	int pad = (j % 7) * 40 + 8, n;

	n = sprintf(buf, "V:%.*s:%08u:", klen, key, gen);
	memset(buf + n, 'a' + (j % 23), pad);
	n += pad;
	n += sprintf(buf + n, ":%08u", gen);
	return n;
}

static int val_check(const str *key, const str *val)
{
	char head[64];
	int n;

	n = sprintf(head, "V:%.*s:", key->len, key->s);
	if (val->len < n + 8 + 1 + 8 || memcmp(val->s, head, n))
		return -1;
	if (memcmp(val->s + n, val->s + val->len - 8, 8))
		return -1;                     /* generation mismatch head vs tail */
	return 0;
}

static void *writer(void *arg)
{
	long w = (long)arg;
	char kb[32], vb[512];
	str k, v;
	unsigned r;
	int j;

	compat_thread_register(1 + (int)w);
	for (r = 0; r < ROUNDS; r++) {
		for (j = 0; j < KEYS_PER_W; j++) {
			k.s = kb; k.len = key_of(kb, (int)w, j);
			if (r + 1 < ROUNDS && (r % 8) == 7 && (j % 16) == (int)w) {
				pcache_ht_remove(ht, &k);
				continue;              /* churn: absent until next round */
			}
			v.s = vb; v.len = val_of(vb, kb, k.len, r, j);
			if (pcache_ht_store(ht, &k, &v, 0) != 0)
				FAIL("writer %ld: store %s round %u\n", w, kb, r);
		}
	}
	return NULL;
}

static void *reader(void *arg)
{
	long id = (long)arg;
	unsigned s = (unsigned)(id * 2654435761u + 12345);
	char kb[32];
	str k, out;
	int i, rc;

	compat_thread_register(100 + (int)id);
	for (i = 0; i < READ_ITER; i++) {
		s ^= s << 13; s ^= s >> 17; s ^= s << 5;
		k.s = kb;
		k.len = key_of(kb, (int)(s % NWRITE), (int)((s >> 8) % KEYS_PER_W));
		rc = pcache_ht_fetch(ht, &k, &out);
		if (rc == 0) {
			if (val_check(&k, &out))
				FAIL("reader %ld: inconsistent value for %s "
					"(len %d)\n", id, kb, out.len);
			free(out.s);
		} else if (rc != -2) {
			FAIL("reader %ld: fetch %s rc %d\n", id, kb, rc);
		}
	}
	return NULL;
}

static void *ctr_add(void *arg)
{
	str k = str_init("ctr");
	long long nv;
	int i;

	compat_thread_register(200 + (int)(long)arg);
	for (i = 0; i < CTR_ADDS; i++)
		if (pcache_ht_add(ht, &k, 1, 0, &nv) != 0)
			FAIL("ctr add\n");
	return NULL;
}

static void *ctr_sub(void *arg)
{
	str k = str_init("ctr");
	long long nv;
	int i;

	compat_thread_register(220 + (int)(long)arg);
	for (i = 0; i < SUB_SUBS; i++)
		if (pcache_ht_add(ht, &k, -1, 0, &nv) != 0)
			FAIL("ctr sub\n");
	return NULL;
}

static void *maint(void *arg)
{
	(void)arg;
	compat_thread_register(300);
	while (!maint_stop) {
		pcache_ht_grow(ht, 4, 64);
		pcache_ht_sweep(ht, get_ticks(), NULL, NULL);
		usleep(20000);
	}
	return NULL;
}

struct audit { unsigned entries, bad; };

static int audit_cb(const str *key, const str *val, unsigned int exp,
		void *ctx)
{
	struct audit *a = ctx;

	(void)exp;
	a->entries++;
	if (key->len == 3 && !memcmp(key->s, "ctr", 3))
		return 0;
	if (val_check(key, val))
		a->bad++;
	return 0;
}

int main(void)
{
	pthread_t tw[NWRITE], tr[NREAD], tc[NCTR], ts[NSUB], tm;
	struct audit au = { 0, 0 };
	long long nv = 0;
	char kb[32];
	str k, out;
	unsigned nb0, nb1;
	long i;
	int w, j;

	alarm(120);                        /* livelock = failure, not a hang */

	if (getenv("PC_SELFTEST_LIGHT")) {
		ROUNDS = 24;
		READ_ITER = 50000;
		CTR_ADDS = 25000;
		SUB_SUBS = 12500;
		printf("selftest: LIGHT scale (qemu emulation)\n");
	}

	/* ---- phase 1: the vendored startup selftests ------------------- */
	pcache_mem_probe();
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = 64;
	if (pcache_arena_init() != 0) {
		printf("FAIL: arena init\n");
		return 1;
	}
	if (pcache_arena_selftest() != 0) {
		printf("FAIL: arena selftest\n");
		return 1;
	}
	if (pcache_htable_selftest() != 0) {
		printf("FAIL: htable selftest\n");
		return 1;
	}
	printf("phase 1: vendored arena + htable selftests PASS\n");

	/* ---- phase 2: cross-thread storm ------------------------------- */
	ht = pcache_htable_new(8);         /* 256 buckets: must grow under load */
	if (!ht) {
		printf("FAIL: table\n");
		return 1;
	}
	nb0 = pcache_ht_nbuckets(ht);

	pthread_create(&tm, NULL, maint, NULL);
	for (i = 0; i < NWRITE; i++) pthread_create(&tw[i], NULL, writer, (void *)i);
	for (i = 0; i < NREAD; i++)  pthread_create(&tr[i], NULL, reader, (void *)i);
	for (i = 0; i < NCTR; i++)   pthread_create(&tc[i], NULL, ctr_add, (void *)i);
	for (i = 0; i < NSUB; i++)   pthread_create(&ts[i], NULL, ctr_sub, (void *)i);

	for (i = 0; i < NWRITE; i++) pthread_join(tw[i], NULL);
	for (i = 0; i < NREAD; i++)  pthread_join(tr[i], NULL);
	for (i = 0; i < NCTR; i++)   pthread_join(tc[i], NULL);
	for (i = 0; i < NSUB; i++)   pthread_join(ts[i], NULL);
	maint_stop = 1;
	pthread_join(tm, NULL);
	nb1 = pcache_ht_nbuckets(ht);

	/* counter exactness: the futex-lock atomicity proof */
	k = (str)str_init("ctr");
	pcache_ht_add(ht, &k, 0, 0, &nv);
	if (nv != (long long)NCTR * CTR_ADDS - (long long)NSUB * SUB_SUBS)
		FAIL("counter: %lld != %lld (lost updates)\n", nv,
			(long long)NCTR * CTR_ADDS - (long long)NSUB * SUB_SUBS);

	/* every key present and consistent after the storm */
	for (w = 0; w < NWRITE; w++)
		for (j = 0; j < KEYS_PER_W; j++) {
			k.s = kb; k.len = key_of(kb, w, j);
			if (pcache_ht_fetch(ht, &k, &out) != 0) {
				FAIL("final: %s absent\n", kb);
				continue;
			}
			if (val_check(&k, &out))
				FAIL("final: %s inconsistent\n", kb);
			free(out.s);
		}

	/* exact census */
	pcache_ht_iter(ht, audit_cb, &au);
	if (au.entries != NWRITE * KEYS_PER_W + 1 || au.bad)
		FAIL("census: %u entries (want %u), %u bad\n",
			au.entries, NWRITE * KEYS_PER_W + 1, au.bad);

	if (nb1 <= nb0)
		FAIL("growth never ran: %u -> %u buckets\n", nb0, nb1);

	printf("phase 2: storm done - buckets %u -> %u, counter %lld, "
		"census %u entries\n", nb0, nb1, nv, au.entries);
	if (nfail) {
		printf("SELFTEST FAILED: %d failure(s)\n", nfail);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
