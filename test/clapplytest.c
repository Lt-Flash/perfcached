/* clapplytest.c - the sharded apply rings: a key's records reach one thread in push order, none lost
 * or doubled across wraps, a slow consumer makes the producer wait, a group acked once. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/clapply.h"

#define NT     4
#define KEYS   97
#define RECS   60000

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static struct clapply A, *CUR = &A;          /* the one the consumers serve */
static volatile int stop;
static unsigned long long last_ver[KEYS];    /* per key, written by ONE thread */
static int owner[KEYS];                      /* which thread saw it */
static unsigned long long applied, backwards, wrongthread, acks, ack_stored;
static int slow;
static __thread int me;

static int apply_cb(const struct clapply_rec *r, void *ctx)
{
	char kb[16];
	int k;
	unsigned long long v;

	(void)ctx;
	/* the key is not NUL-terminated in the ring: the value follows it */
	memcpy(kb, r->key, r->klen < 15 ? r->klen : 15);
	kb[r->klen < 15 ? r->klen : 15] = 0;
	k = atoi(kb + 1);
	if (r->collen != 1 || r->col[0] != 'c' || r->klen < 2 || k < 0 || k >= KEYS) {
		__atomic_fetch_add(&wrongthread, 1, __ATOMIC_RELAXED);
		return 0;
	}
	if (owner[k] < 0)
		owner[k] = me;
	else if (owner[k] != me)
		__atomic_fetch_add(&wrongthread, 1, __ATOMIC_RELAXED);
	v = last_ver[k];
	if (r->ver <= v)
		__atomic_fetch_add(&backwards, 1, __ATOMIC_RELAXED);
	last_ver[k] = r->ver;
	/* the value carries the version too: the bytes crossed intact */
	if (r->vlen < 8 || memcmp(r->val, &r->ver, 8) != 0)
		__atomic_fetch_add(&wrongthread, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&applied, 1, __ATOMIC_RELAXED);
	if (slow) {
		struct timespec ts = { 0, 20000L };
		nanosleep(&ts, NULL);
	}
	return (r->ver & 1) ? 1 : 0;   /* odd versions "stored" */
}

static void ack_cb(const struct clapply_grp *g, void *ctx)
{
	(void)ctx;
	__atomic_fetch_add(&acks, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&ack_stored, g->stored, __ATOMIC_RELAXED);
}

static long long fake_now;
static long long fake_clock(void) { return fake_now; }

static void *consumer(void *arg)
{
	int idx = (int)(long)arg;
	struct clapply_item it;

	me = idx;
	while (clapply_take(CUR, idx, &stop, &it, 1)) {
		int st = CUR->apply(&it.r, CUR->ctx);

		if (it.grp && st)
			__atomic_fetch_add(&it.grp->stored, 1, __ATOMIC_RELAXED);
		clapply_done(CUR, idx, &it);
	}
	return NULL;
}

static struct clapply *P;                    /* the pusher's */
static int pushed, refused, push_done;
static const struct clapply_rec *prec;

static void *pusher(void *arg)
{
	int i;

	(void)arg;
	for (i = 0; i < 40; i++) {
		if (clapply_push(P, prec, NULL) < 0)
			refused++;
		else
			pushed++;
	}
	__atomic_store_n(&push_done, 1, __ATOMIC_RELEASE);
	return NULL;
}

int main(void)
{
	pthread_t th[NT];
	pc_addr from;
	struct clapply_rec r;
	unsigned char key[16], val[70000];
	unsigned long long ver = 0, sum_blocks = 0;
	unsigned int seed = 12345;
	int i, k, n;

	memset(&from, 0, sizeof from);
	memset(val, 'v', sizeof val);
	for (k = 0; k < KEYS; k++)
		owner[k] = -1;
	n = clapply_init(&A, NT, apply_cb, ack_cb, NULL);
	A.stop = &stop;
	CHK(n == NT, "init: %d threads", n);
	CHK(clapply_init(&A, 1, apply_cb, ack_cb, NULL) == 0 && A.n == 0,
		"apply_threads = 1 means OFF: no rings, no threads");
	n = clapply_init(&A, NT, apply_cb, ack_cb, NULL);
	A.stop = &stop;
	for (i = 0; i < NT; i++)
		pthread_create(&th[i], NULL, consumer, (void *)(long)i);

	/* the storm: sizes from 8 bytes to 60 KB, so the wrap marker lands
	 * everywhere and the ring turns over ~thousands of times */
	for (i = 0; i < RECS; i++) {
		unsigned int rnd = rand_r(&seed);

		k = (int)(rnd % KEYS);
		ver++;
		memcpy(val, &ver, 8);
		r.col = (const unsigned char *)"c";
		r.collen = 1;
		r.klen = (unsigned)snprintf((char *)key, sizeof key, "k%d", k);
		r.key = key;
		r.val = val;
		r.vlen = (rnd >> 8) % 3 == 0 ? 8 + (rnd >> 10) % 60000 : 8 + (rnd >> 10) % 200;
		r.ttl_left = 0;
		r.ver = ver;
		r.passive = 1;
		if (i == RECS / 2)
			slow = 1;              /* the second half against a slow consumer */
		if (clapply_push(&A, &r, NULL) < 0) {
			printf("  FAIL push refused record %d\n", i);
			fails++;
			break;
		}
	}
	/* a group: 10 records with odd versions 5 of them, across threads */
	{
		struct clapply_grp *g = clapply_open(&A, &from, 77);
		int odd = 0;

		for (i = 0; i < 10; i++) {
			ver++;
			odd += ver & 1;
			memcpy(val, &ver, 8);
			r.klen = (unsigned)snprintf((char *)key, sizeof key, "k%d", i * 9);
			r.vlen = 8;
			r.ver = ver;
			clapply_push(&A, &r, g);
		}
		CHK(__atomic_load_n(&acks, __ATOMIC_RELAXED) == 0,
			"the group is not acked while the receiver still holds it");
		clapply_close(&A, g);
		for (i = 0; i < 400 && __atomic_load_n(&acks, __ATOMIC_RELAXED) == 0; i++) {
			struct timespec ts = { 0, 10000000L };
			nanosleep(&ts, NULL);
		}
		CHK(__atomic_load_n(&acks, __ATOMIC_RELAXED) == 1 &&
			__atomic_load_n(&ack_stored, __ATOMIC_RELAXED) == (unsigned)odd,
			"acked exactly once after its last record, stored = %d of 10 (got %llu ack(s), stored %llu)",
			odd, acks, ack_stored);
	}
	/* drain */
	for (i = 0; i < 3000 && __atomic_load_n(&applied, __ATOMIC_RELAXED) < (unsigned)RECS + 10; i++) {
		struct timespec ts = { 0, 10000000L };
		nanosleep(&ts, NULL);
	}
	stop = 1;
	for (i = 0; i < NT; i++)
		pthread_join(th[i], NULL);
	for (i = 0; i < NT; i++)
		sum_blocks += A.ring[i].blocks;
	CHK(applied == (unsigned)RECS + 10, "every record applied exactly once: %llu of %d", applied, RECS + 10);
	CHK(backwards == 0, "no key ever saw a version go backwards or repeat (%llu)", backwards);
	CHK(wrongthread == 0, "every record of a key on ONE thread, bytes intact (%llu wrong)", wrongthread);
	{
		int used[NT] = { 0 }, nused = 0;

		for (k = 0; k < KEYS; k++)
			if (owner[k] >= 0 && !used[owner[k]]++)
				nused++;
		CHK(nused == NT, "%d keys spread over all %d threads", KEYS, nused);
	}
	CHK(sum_blocks > 0 && A.dispatched == (unsigned)RECS + 10,
		"a slow consumer made the producer wait %llu time(s) - and drop nothing", sum_blocks);
	r.vlen = CLAPPLY_RING_SZ;
	CHK(clapply_push(&A, &r, NULL) == -1, "a record the ring can never hold is refused up front");
	/* an EMPTY ring whose tail sits past the middle and a record bigger than what is left before
	 * the end AND bigger than tail must wrap, not wait for ever */
	{
		struct clapply B;
		pthread_t t;
		struct clapply_item it;
		int j, ok = 0;

		unsigned char *big = malloc(CLAPPLY_RING_SZ);

		memset(big, 'b', CLAPPLY_RING_SZ);
		r.val = big;
		stop = 0;
		CUR = &B;
		clapply_init(&B, 2, apply_cb, ack_cb, NULL);
		B.stop = &stop;
		r.klen = (unsigned)snprintf((char *)key, sizeof key, "k1");
		r.vlen = CLAPPLY_RING_SZ / 2 + 1000;
		r.ver = 1;
		memcpy(big, &r.ver, 8);
		clapply_push(&B, &r, NULL);            /* tail lands past the middle */
		j = clapply_shard(&B, r.col, r.collen, r.key, r.klen);
		if (clapply_take(&B, j, &stop, &it, 1))
			clapply_done(&B, j, &it);          /* consumed: the ring is empty, tail > cap/2 */
		pthread_create(&t, NULL, consumer, (void *)(long)j);
		r.ver = 3;
		memcpy(big, &r.ver, 8);
		ok = clapply_push(&B, &r, NULL) == j;  /* must wrap, not wait for ever */
		for (i = 0; i < 300 && last_ver[1] != 3; i++) {
			struct timespec ts = { 0, 10000000L };
			nanosleep(&ts, NULL);
		}
		stop = 1;
		pthread_join(t, NULL);
		CHK(ok && last_ver[1] == 3, "an empty ring takes a record larger than the space before its end and larger than tail: it wraps (last version %llu)", last_ver[1]);
		clapply_free(&B);
		free(big);
	}
	clapply_free(&A);
	/* busy_since_ms is the owner's clock at the take, not the caller's now_ms read before the take
	 * blocked, which would stamp the first item after an idle gap as old as the gap */
	{
		struct clapply S;
		struct clapply_item it;
		long long b;
		int j;

		stop = 0;
		clapply_init(&S, 2, apply_cb, ack_cb, NULL);
		S.stop = &stop;
		S.clock = fake_clock;
		r.col = (const unsigned char *)"c"; r.collen = 1;
		r.klen = (unsigned)snprintf((char *)key, sizeof key, "stamp");
		r.key = key;
		r.val = (const unsigned char *)"v"; r.vlen = 1; r.ver = 7;
		j = clapply_shard(&S, r.col, r.collen, r.key, r.klen);
		CHK(clapply_push(&S, &r, NULL) >= 0, "stamp: one record pushed");
		fake_now = 5000;                        /* the clock at the take */
		CHK(clapply_take(&S, j, &stop, &it, 1000) == 1,
		    "stamp: taken (the caller's argument says 1000, the clock says 5000)");
		b = __atomic_load_n(&S.ring[j].busy_since_ms, __ATOMIC_RELAXED);
		CHK(b == 5000, "stamp: busy_since_ms is the clock at the take (%lld), not the pre-wait argument", b);
		clapply_done(&S, j, &it);
		clapply_free(&S);
	}

	/* the owner sets no stop (the daemon does not): the consumer's stop is the push's, so a push
	 * into a full ring whose consumer has left at shutdown returns for the caller to apply */
	{
		static struct clapply E;
		struct clapply_item it;
		pthread_t t;
		int j;

		stop = 1;
		clapply_init(&E, 2, apply_cb, ack_cb, NULL);
		r.col = (const unsigned char *)"c"; r.collen = 1;
		r.klen = (unsigned)snprintf((char *)key, sizeof key, "k5");
		r.key = key;
		r.val = val; r.vlen = 60000; r.ver = 9;
		j = clapply_shard(&E, r.col, r.collen, r.key, r.klen);
		CHK(clapply_take(&E, j, &stop, &it, 1) == 0, "stop: the consumer leaves an empty ring at shutdown");
		P = &E;
		prec = &r;
		pthread_create(&t, NULL, pusher, NULL);
		for (i = 0; i < 300 && !__atomic_load_n(&push_done, __ATOMIC_ACQUIRE); i++) {
			struct timespec ts = { 0, 10000000L };
			nanosleep(&ts, NULL);
		}
		CHK(__atomic_load_n(&push_done, __ATOMIC_ACQUIRE) && refused > 0,
			"stop: 40 x 60 KB into its 1 MiB ring returned within 3 s: %d queued, %d handed back", pushed, refused);
		if (push_done) {
			pthread_join(t, NULL);
			clapply_free(&E);
		}
	}

	printf("clapplytest: %s\n", fails ? "FAILED" : "passed");
	return fails ? 1 : 0;
}
