/*
 * clretain.c - S238: retained tombstones.  See clretain.h.
 *
 * NSHARD shards, each a chained hash plus a doubly linked list in PLANTING
 * order (a re-planted key moves to the tail with the new time), so expiry
 * and the cap both take from the head, and a replay from a time walks
 * forward.  Entries are one malloc each: header + col + '\0' + key.
 */
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "clretain.h"
#include "resident.h"                   /* S265 */

#define NSHARD 64u

struct ent {
	struct ent *hnext;                 /* hash chain */
	struct ent *prev, *next;           /* planting order */
	uint64_t ver;
	unsigned int t_s;
	unsigned int len;                  /* col '\0' key */
	unsigned int cn;
	unsigned char local;               /* S405: never replayed to a peer */
	unsigned char expiry;              /* S443: noted for an expiry */
	char k[];
};

static struct shard {
	pthread_mutex_t mx;
	struct ent **tab;
	unsigned int nb;                   /* power of two */
	unsigned int n, cap;
	struct ent *head, *tail;
} sh[NSHARD];

static unsigned int R_retain_s, R_max;
static int R_on;
static unsigned long long R_bytes, R_dropped_early, R_noted;   /* atomic */
static unsigned long long R_dropped_early_exp, R_noted_exp;    /* S443 */
/* S392: the newest planting time of a delete this table LOST before
 * retain_s - evicted by the cap, or never stored for want of memory.
 * Every delete planted after it is still here (atomic, 0 = none lost) */
static unsigned int R_lost_upto_s;
static unsigned int R_n;                /* entries, all shards (atomic) */
static unsigned int R_local;            /* S405: of them, local (atomic) */
static unsigned int R_exp;              /* S443: of them, expiries (atomic) */

static unsigned int hash_of(const char *col, size_t cn, const char *key,
		size_t kn)
{
	unsigned int h = 2166136261u;
	size_t i;

	for (i = 0; i < cn; i++)
		h = (h ^ (unsigned char)col[i]) * 16777619u;
	h = (h ^ 0u) * 16777619u;
	for (i = 0; i < kn; i++)
		h = (h ^ (unsigned char)key[i]) * 16777619u;
	return h;
}

void clretain_init(unsigned int retain_s, unsigned int max)
{
	unsigned int i, per;

	R_retain_s = retain_s;
	R_max = max;
	__atomic_store_n(&R_n, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&R_local, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&R_exp, 0, __ATOMIC_RELAXED);           /* S443 */
	__atomic_store_n(&R_lost_upto_s, 0, __ATOMIC_RELAXED);   /* S392 */
	R_on = retain_s > 0 && max > 0;
	if (!R_on)
		return;
	per = (max + NSHARD - 1) / NSHARD;
	for (i = 0; i < NSHARD; i++) {
		unsigned int nb = 64;

		while (nb < per && nb < (1u << 20))
			nb <<= 1;
		pthread_mutex_init(&sh[i].mx, NULL);
		sh[i].tab = calloc(nb, sizeof *sh[i].tab);
		pc_touch_pages(sh[i].tab, nb * sizeof *sh[i].tab);   /* S265 */
		sh[i].nb = sh[i].tab ? nb : 0;
		sh[i].cap = per;
		sh[i].n = 0;
		sh[i].head = sh[i].tail = NULL;
		if (!sh[i].tab)
			R_on = 0;                  /* no memory: stay off, honestly */
	}
}

int clretain_enabled(void)
{
	return R_on;
}

static int ent_is(const struct ent *e, const char *col, size_t cn,
		const char *key, size_t kn)
{
	return e->cn == cn && e->len == cn + 1 + kn &&
		!memcmp(e->k, col, cn) && !memcmp(e->k + cn + 1, key, kn);
}

static void list_unlink(struct shard *s, struct ent *e)
{
	if (e->prev)
		e->prev->next = e->next;
	else
		s->head = e->next;
	if (e->next)
		e->next->prev = e->prev;
	else
		s->tail = e->prev;
	e->prev = e->next = NULL;
}

static void list_append(struct shard *s, struct ent *e)
{
	e->prev = s->tail;
	e->next = NULL;
	if (s->tail)
		s->tail->next = e;
	else
		s->head = e;
	s->tail = e;
}

/* under the shard lock: take @e out of the table and free it */
static void ent_drop(struct shard *s, struct ent *e, unsigned int h)
{
	struct ent **pp = &s->tab[h & (s->nb - 1)];

	while (*pp && *pp != e)
		pp = &(*pp)->hnext;
	if (*pp)
		*pp = e->hnext;
	list_unlink(s, e);
	s->n--;
	__atomic_fetch_sub(&R_n, 1, __ATOMIC_RELAXED);
	if (e->local)
		__atomic_fetch_sub(&R_local, 1, __ATOMIC_RELAXED);
	if (e->expiry)
		__atomic_fetch_sub(&R_exp, 1, __ATOMIC_RELAXED);
	__atomic_fetch_sub(&R_bytes, sizeof *e + e->len, __ATOMIC_RELAXED);
	free(e);
}

static unsigned int ent_hash(const struct ent *e)
{
	return hash_of(e->k, e->cn, e->k + e->cn + 1, e->len - e->cn - 1);
}

static void lost_at(unsigned int t_s)
{
	unsigned int cur = __atomic_load_n(&R_lost_upto_s, __ATOMIC_RELAXED);

	while (t_s > cur && !__atomic_compare_exchange_n(&R_lost_upto_s, &cur,
	        t_s, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
}

static void note_in(const char *col, size_t cn, const char *key, size_t kn,
		uint64_t ver, unsigned int now_s, int local, int expiry)
{
	unsigned int h;
	struct shard *s;
	struct ent *e;

	if (!R_on || !ver || cn > 255 || kn > 4096)
		return;                        /* an unversioned entry orders nothing */
	h = hash_of(col, cn, key, kn);
	s = &sh[h % NSHARD];
	__atomic_fetch_add(&R_noted, 1, __ATOMIC_RELAXED);
	if (expiry)
		__atomic_fetch_add(&R_noted_exp, 1, __ATOMIC_RELAXED);
	pthread_mutex_lock(&s->mx);
	for (e = s->tab[(h / NSHARD) & (s->nb - 1)]; e; e = e->hnext)
		if (ent_is(e, col, cn, key, kn))
			break;
	if (e) {
		if (ver > e->ver)
			e->ver = ver;
		e->t_s = now_s;
		if (!local && e->local) {
			e->local = 0;          /* a real delete: replayed from now */
			__atomic_fetch_sub(&R_local, 1, __ATOMIC_RELAXED);
		}
		if (!expiry && e->expiry) {
			e->expiry = 0;         /* S443: a delete outranks an expiry */
			__atomic_fetch_sub(&R_exp, 1, __ATOMIC_RELAXED);
		}
		list_unlink(s, e);
		list_append(s, e);
		pthread_mutex_unlock(&s->mx);
		return;
	}
	if (__atomic_load_n(&R_n, __ATOMIC_RELAXED) >= R_max && s->head) {
		/* the cap, counted over the WHOLE table - a per-shard share
		 * evicted in the busy shards while others had room.  The
		 * victim is this shard's oldest: close to the oldest overall,
		 * without a global lock.  Said when it had not expired. */
		struct ent *o = s->head;

		if (now_s - o->t_s < R_retain_s) {
			__atomic_fetch_add(&R_dropped_early, 1, __ATOMIC_RELAXED);
			if (o->expiry)                 /* S443 */
				__atomic_fetch_add(&R_dropped_early_exp, 1,
					__ATOMIC_RELAXED);
			lost_at(o->t_s);
		}
		ent_drop(s, o, ent_hash(o) / NSHARD);
	}
	e = malloc(sizeof *e + cn + 1 + kn);
	if (!e) {
		/* S392: a delete not kept is a delete lost, as an early
		 * eviction is - the table cannot vouch past it */
		__atomic_fetch_add(&R_dropped_early, 1, __ATOMIC_RELAXED);
		if (expiry)                            /* S443 */
			__atomic_fetch_add(&R_dropped_early_exp, 1,
				__ATOMIC_RELAXED);
		lost_at(now_s);
		pthread_mutex_unlock(&s->mx);
		return;
	}
	e->ver = ver;
	e->t_s = now_s;
	e->local = (unsigned char)local;
	e->expiry = (unsigned char)expiry;
	e->cn = (unsigned int)cn;
	e->len = (unsigned int)(cn + 1 + kn);
	memcpy(e->k, col, cn);
	e->k[cn] = 0;
	memcpy(e->k + cn + 1, key, kn);
	e->hnext = s->tab[(h / NSHARD) & (s->nb - 1)];
	s->tab[(h / NSHARD) & (s->nb - 1)] = e;
	list_append(s, e);
	s->n++;
	__atomic_fetch_add(&R_n, 1, __ATOMIC_RELAXED);
	if (local)
		__atomic_fetch_add(&R_local, 1, __ATOMIC_RELAXED);
	if (expiry)
		__atomic_fetch_add(&R_exp, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&R_bytes, sizeof *e + e->len, __ATOMIC_RELAXED);
	pthread_mutex_unlock(&s->mx);
}

void clretain_note(const char *col, size_t cn, const char *key, size_t kn,
		uint64_t ver, unsigned int now_s)
{
	note_in(col, cn, key, kn, ver, now_s, 0, 0);
}

void clretain_note_expiry(const char *col, size_t cn, const char *key,
		size_t kn, uint64_t ver, unsigned int now_s)
{
	note_in(col, cn, key, kn, ver, now_s, 0, 1);       /* S443 */
}

void clretain_note_local(const char *col, size_t cn, const char *key,
		size_t kn, uint64_t ver, unsigned int now_s)
{
	note_in(col, cn, key, kn, ver, now_s, 1, 0);
}

uint64_t clretain_get(const char *col, size_t cn, const char *key, size_t kn,
		unsigned int now_s)
{
	unsigned int h;
	struct shard *s;
	struct ent *e;
	uint64_t v = 0;

	if (!R_on)
		return 0;
	h = hash_of(col, cn, key, kn);
	s = &sh[h % NSHARD];
	pthread_mutex_lock(&s->mx);
	if (!s->n) {
		pthread_mutex_unlock(&s->mx);
		return 0;
	}
	for (e = s->tab[(h / NSHARD) & (s->nb - 1)]; e; e = e->hnext)
		if (ent_is(e, col, cn, key, kn)) {
			if (now_s - e->t_s < R_retain_s)
				v = e->ver;
			break;
		}
	pthread_mutex_unlock(&s->mx);
	return v;
}

unsigned int clretain_each(unsigned int since_s, unsigned int upto_s,
		void (*cb)(const char *col, size_t cn, const char *key, size_t kn,
			uint64_t ver, void *ctx), void *ctx)
{
	unsigned int i, n = 0;
	struct ent *e;

	if (!R_on)
		return 0;
	for (i = 0; i < NSHARD; i++) {
		pthread_mutex_lock(&sh[i].mx);
		for (e = sh[i].head; e; e = e->next) {
			if (e->t_s <= since_s)
				continue;
			if (e->t_s > upto_s)
				break;             /* planting order: the rest are later */
			if (e->local)
				continue;          /* S405: this node's own drop */
			cb(e->k, e->cn, e->k + e->cn + 1, e->len - e->cn - 1,
				e->ver, ctx);
			n++;
		}
		pthread_mutex_unlock(&sh[i].mx);
	}
	return n;
}

void clretain_expire(unsigned int now_s)
{
	unsigned int i;

	if (!R_on)
		return;
	for (i = 0; i < NSHARD; i++) {
		pthread_mutex_lock(&sh[i].mx);
		while (sh[i].head && now_s - sh[i].head->t_s >= R_retain_s)
			ent_drop(&sh[i], sh[i].head, ent_hash(sh[i].head) / NSHARD);
		pthread_mutex_unlock(&sh[i].mx);
	}
}

void clretain_get_stats(struct clretain_stats *out, unsigned int now_s)
{
	unsigned int i, oldest = 0;
	unsigned long long n = 0;

	memset(out, 0, sizeof *out);
	out->retain_s = R_retain_s;
	out->max = R_max;
	if (!R_on)
		return;
	for (i = 0; i < NSHARD; i++) {
		pthread_mutex_lock(&sh[i].mx);
		n += sh[i].n;
		if (sh[i].head && now_s - sh[i].head->t_s > oldest)
			oldest = now_s - sh[i].head->t_s;
		pthread_mutex_unlock(&sh[i].mx);
	}
	out->entries = n;
	out->bytes = __atomic_load_n(&R_bytes, __ATOMIC_RELAXED);
	out->dropped_early = __atomic_load_n(&R_dropped_early, __ATOMIC_RELAXED);
	out->noted = __atomic_load_n(&R_noted, __ATOMIC_RELAXED);
	out->local = __atomic_load_n(&R_local, __ATOMIC_RELAXED);
	out->expiries = __atomic_load_n(&R_exp, __ATOMIC_RELAXED);       /* S443 */
	out->dropped_early_expiries = __atomic_load_n(&R_dropped_early_exp,
		__ATOMIC_RELAXED);
	out->noted_expiries = __atomic_load_n(&R_noted_exp, __ATOMIC_RELAXED);
	out->oldest_age_s = oldest;
}

unsigned long long clretain_dropped_early(void)
{
	return __atomic_load_n(&R_dropped_early, __ATOMIC_RELAXED);
}

unsigned int clretain_retain_s(void)
{
	return R_retain_s;
}

unsigned int clretain_complete_since(unsigned int now_s)
{
	unsigned int c, lost;

	if (!R_on)
		return UINT_MAX;
	/* clretain_expire keeps what is younger than retain_s */
	c = now_s > R_retain_s ? now_s - R_retain_s : 0;
	lost = __atomic_load_n(&R_lost_upto_s, __ATOMIC_RELAXED);
	return lost > c ? lost : c;
}
