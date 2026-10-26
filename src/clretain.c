/*
 * clretain.c - S238: retained tombstones.  See clretain.h.
 *
 * NSHARD shards, each a chained hash plus a doubly linked list in PLANTING
 * order (a re-planted key moves to the tail with the new time), so expiry
 * and the cap both take from the head, and a replay from a time walks
 * forward.  Entries are one malloc each: header + col + '\0' + key.
 * S444: the shard's expiries are also on a second list, in the same
 * order, so the cap takes the oldest EXPIRY first - an expiry never
 * pushes out a delete.
 * 12ny AT-5: a numbered delete is on a third, its space's within the
 * shard (a small per-shard table of spaces, never shrunk: an entry names
 * its slot), in arrival order - what clretain_forget takes from the front.
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
	/* AT-5: 16 bits each (col <= 255, key <= 4096: 4,352 at most), so
	 * the space's slot fits the padding - 88 bytes a header, not 96 */
	unsigned short len;                /* col '\0' key */
	unsigned short cn;
	unsigned char local;               /* S405: never replayed to a peer */
	unsigned char expiry;              /* S443: noted for an expiry - AT-5
	                                    * (c): 1 on the xlist (may go early),
	                                    * 2 kept by the time rule */
	unsigned int spi;                  /* AT-5: its space's slot */
	struct ent *xprev, *xnext;         /* S444: the shard's expiries */
	uint64_t nseq;                     /* AT-5: its number, 0 = none */
	struct ent *sprev, *snext;         /* AT-5: the space's arrival order */
	char k[];
};

/* AT-5: one space's entries in a shard */
struct spl {
	uint64_t oid;
	uint32_t incarn;
	struct ent *head, *tail;
};
#define SPL_MAX 2052u                  /* two spaces a member, twice over */

static struct shard {
	pthread_mutex_t mx;
	struct ent **tab;
	unsigned int nb;                   /* power of two */
	unsigned int n, cap;
	struct ent *head, *tail;
	struct ent *xhead, *xtail;         /* S444: the expiries alone */
	struct spl *sp;                    /* AT-5 */
	unsigned int nsp, capsp;
} sh[NSHARD];

/* AT-5: how far each space was forgotten here */
static struct { uint64_t oid; uint32_t incarn; uint64_t through; unsigned int at_s;
	uint64_t lost; } FG[SPL_MAX];      /* AT-8: lost = dropped by time or cap */
static unsigned int nfg;
static pthread_mutex_t FG_mx = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long R_forgot, R_forgot_exp;              /* atomic */
static unsigned int R_num;              /* AT-5: numbered entries (atomic) */

static unsigned int R_retain_s, R_max;
static int R_on;
static unsigned long long R_bytes, R_dropped_early, R_noted;   /* atomic */
static unsigned long long R_dropped_early_exp, R_noted_exp;    /* S443 */
/* S392: the newest planting time of a delete this table LOST before
 * retain_s - evicted by the cap, or never stored for want of memory.
 * Every delete planted after it is still here (atomic, 0 = none lost) */
static unsigned int R_lost_upto_s;
/* 12ny AT-8: when the newest UNNUMBERED entry the time rule took was
 * noted - a member away since before then may lack it, and has no mark to
 * say so (numbered ones are judged by the marks: lost_note) */
static unsigned int R_time_hi_s;                               /* atomic */
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
	__atomic_store_n(&R_time_hi_s, 0, __ATOMIC_RELAXED);     /* AT-8 */
	__atomic_store_n(&R_num, 0, __ATOMIC_RELAXED);           /* AT-5 */
	pthread_mutex_lock(&FG_mx);
	nfg = 0;
	pthread_mutex_unlock(&FG_mx);
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
		sh[i].xhead = sh[i].xtail = NULL;           /* S444 */
		sh[i].nsp = 0;                              /* AT-5: kept, reused */
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

/* S444: the same two for the shard's expiry list */
static void xlist_unlink(struct shard *s, struct ent *e)
{
	if (e->xprev)
		e->xprev->xnext = e->xnext;
	else
		s->xhead = e->xnext;
	if (e->xnext)
		e->xnext->xprev = e->xprev;
	else
		s->xtail = e->xprev;
	e->xprev = e->xnext = NULL;
}

static void xlist_append(struct shard *s, struct ent *e)
{
	e->xprev = s->xtail;
	e->xnext = NULL;
	if (s->xtail)
		s->xtail->xnext = e;
	else
		s->xhead = e;
	s->xtail = e;
}

/* AT-5: a space's slot in the shard, made when @make; -1 when none (or no
 * room: the entry then stays unnumbered - kept by the time rule) */
static int spl_slot(struct shard *s, uint64_t oid, uint32_t incarn, int make)
{
	unsigned int i;
	struct spl *n;

	for (i = 0; i < s->nsp; i++)
		if (s->sp[i].oid == oid && s->sp[i].incarn == incarn)
			return (int)i;
	if (!make)
		return -1;
	for (i = 0; i < s->nsp; i++)
		if (!s->sp[i].head) {          /* an emptied slot: reused */
			s->sp[i].oid = oid;
			s->sp[i].incarn = incarn;
			return (int)i;
		}
	if (s->nsp == s->capsp) {
		unsigned int cap = s->capsp ? s->capsp * 2 : 4;

		if (cap > SPL_MAX)
			cap = SPL_MAX;
		if (s->nsp == cap || !(n = realloc(s->sp, cap * sizeof *n)))
			return -1;
		s->sp = n;
		s->capsp = cap;
	}
	s->sp[s->nsp].oid = oid;
	s->sp[s->nsp].incarn = incarn;
	s->sp[s->nsp].head = s->sp[s->nsp].tail = NULL;
	return (int)s->nsp++;
}

static void slist_unlink(struct shard *s, struct ent *e)
{
	struct spl *l = &s->sp[e->spi];

	if (e->sprev)
		e->sprev->snext = e->snext;
	else
		l->head = e->snext;
	if (e->snext)
		e->snext->sprev = e->sprev;
	else
		l->tail = e->sprev;
	e->sprev = e->snext = NULL;
	e->nseq = 0;
	__atomic_fetch_sub(&R_num, 1, __ATOMIC_RELAXED);
}

static void slist_append(struct shard *s, struct ent *e, int spi,
		uint64_t seq)
{
	struct spl *l = &s->sp[spi];

	e->spi = (unsigned int)spi;
	e->nseq = seq;
	e->sprev = l->tail;
	e->snext = NULL;
	if (l->tail)
		l->tail->snext = e;
	else
		l->head = e;
	l->tail = e;
	__atomic_fetch_add(&R_num, 1, __ATOMIC_RELAXED);
}

/* AT-5: the entry now stands for the delete noted at (oid, incarn, seq) -
 * seq 0 for one without a number */
static void ent_number(struct shard *s, struct ent *e, uint64_t oid,
		uint32_t incarn, uint64_t seq)
{
	int spi;

	if (e->nseq)
		slist_unlink(s, e);
	if (seq && (spi = spl_slot(s, oid, incarn, 1)) >= 0)
		slist_append(s, e, spi, seq);
}

/* 12ny AT-8: a numbered entry dropped without anyone's confirmation - the
 * time rule or the cap: the highest such seq, per space.  Under a shard
 * lock; FG_mx nests inside it here and nowhere the other way */
static void lost_note(const struct shard *s, const struct ent *e)
{
	uint64_t oid = s->sp[e->spi].oid;
	uint32_t incarn = s->sp[e->spi].incarn;
	unsigned int i;

	pthread_mutex_lock(&FG_mx);
	for (i = 0; i < nfg; i++)
		if (FG[i].oid == oid && FG[i].incarn == incarn)
			break;
	if (i == nfg && nfg < SPL_MAX) {
		FG[nfg].oid = oid;
		FG[nfg].incarn = incarn;
		FG[nfg].through = 0;
		FG[nfg].at_s = 0;
		FG[nfg++].lost = 0;
	}
	if (i < nfg && e->nseq > FG[i].lost)
		FG[i].lost = e->nseq;
	pthread_mutex_unlock(&FG_mx);
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
	if (e->expiry) {
		if (e->expiry == 1)
			xlist_unlink(s, e);            /* S444 */
		__atomic_fetch_sub(&R_exp, 1, __ATOMIC_RELAXED);
	}
	if (e->nseq)
		slist_unlink(s, e);                    /* AT-5 */
	__atomic_fetch_sub(&R_bytes, sizeof *e + e->len, __ATOMIC_RELAXED);
	free(e);
}

static unsigned int ent_hash(const struct ent *e)
{
	return hash_of(e->k, e->cn, e->k + e->cn + 1, e->len - e->cn - 1);
}

/* 12ny AT-8: R_time_hi_s, when the newest unnumbered entry the time rule
 * took was noted */
static void time_dropped(unsigned int t_s)
{
	unsigned int cur = __atomic_load_n(&R_time_hi_s, __ATOMIC_RELAXED);

	while (t_s > cur && !__atomic_compare_exchange_n(&R_time_hi_s, &cur,
	        t_s, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
}

static void lost_at(unsigned int t_s)
{
	unsigned int cur = __atomic_load_n(&R_lost_upto_s, __ATOMIC_RELAXED);

	while (t_s > cur && !__atomic_compare_exchange_n(&R_lost_upto_s, &cur,
	        t_s, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
}

static void note_in(const char *col, size_t cn, const char *key, size_t kn,
		uint64_t ver, unsigned int now_s, int local, int expiry,
		uint64_t oid, uint32_t incarn, uint64_t seq)
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
		/* AT-5: a newer delete takes the entry over, numbered or not;
		 * the same one again adds a number it lacked */
		if (ver > e->ver || (ver == e->ver && seq && !e->nseq))
			ent_number(s, e, oid, incarn, seq);
		if (ver > e->ver)
			e->ver = ver;
		e->t_s = now_s;
		if (!local && e->local) {
			e->local = 0;          /* a real delete: replayed from now */
			__atomic_fetch_sub(&R_local, 1, __ATOMIC_RELAXED);
		}
		if (!expiry && e->expiry) {
			if (e->expiry == 1)
				xlist_unlink(s, e);    /* S444 */
			e->expiry = 0;         /* S443: a delete outranks an expiry */
			__atomic_fetch_sub(&R_exp, 1, __ATOMIC_RELAXED);
		} else if (e->expiry == 1 && expiry == 2) {
			xlist_unlink(s, e);    /* AT-5 (c): kept from now on */
			e->expiry = 2;
		} else if (e->expiry == 1) {
			xlist_unlink(s, e);            /* S444: re-planted, to the tail */
			xlist_append(s, e);
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
		 * without a global lock.  Said when it had not expired.
		 * S444: its oldest EXPIRY first, and an expiry never pushes out
		 * a delete - a shard with none to give keeps its deletes and
		 * does not keep this expiry (counted as one dropped early).  A
		 * 60-s-TTL benchmark's 13 million expiries on 245-247 evicted
		 * every delete made meanwhile within seconds. */
		struct ent *o = s->xhead ? s->xhead : expiry == 1 ? NULL : s->head;

		if (!o) {
			__atomic_fetch_add(&R_dropped_early, 1, __ATOMIC_RELAXED);
			__atomic_fetch_add(&R_dropped_early_exp, 1,
				__ATOMIC_RELAXED);
			lost_at(now_s);
			pthread_mutex_unlock(&s->mx);
			return;
		}
		if (now_s - o->t_s < R_retain_s) {
			__atomic_fetch_add(&R_dropped_early, 1, __ATOMIC_RELAXED);
			if (o->expiry)                 /* S443 */
				__atomic_fetch_add(&R_dropped_early_exp, 1,
					__ATOMIC_RELAXED);
			lost_at(o->t_s);
		}
		if (o->nseq)
			lost_note(s, o);               /* AT-8 */
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
	e->xprev = e->xnext = NULL;
	e->nseq = 0;                       /* AT-5: numbered below */
	e->spi = 0;
	e->sprev = e->snext = NULL;
	e->cn = (unsigned short)cn;
	e->len = (unsigned short)(cn + 1 + kn);
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
	if (expiry) {
		if (expiry == 1)
			xlist_append(s, e);            /* S444; AT-5 (c): not a kept one */
		__atomic_fetch_add(&R_exp, 1, __ATOMIC_RELAXED);
	}
	if (seq)
		ent_number(s, e, oid, incarn, seq);    /* AT-5 */
	__atomic_fetch_add(&R_bytes, sizeof *e + e->len, __ATOMIC_RELAXED);
	pthread_mutex_unlock(&s->mx);
}

void clretain_note(const char *col, size_t cn, const char *key, size_t kn,
		uint64_t ver, unsigned int now_s)
{
	note_in(col, cn, key, kn, ver, now_s, 0, 0, 0, 0, 0);
}

void clretain_note_n(const char *col, size_t cn, const char *key, size_t kn,
		uint64_t ver, unsigned int now_s, uint64_t oid, uint32_t incarn,
		uint64_t seq)
{
	note_in(col, cn, key, kn, ver, now_s, 0, 0, oid, incarn, seq);  /* AT-5 */
}

void clretain_note_expiry(const char *col, size_t cn, const char *key,
		size_t kn, uint64_t ver, unsigned int now_s, int keep)
{
	note_in(col, cn, key, kn, ver, now_s, 0, keep ? 2 : 1, 0, 0, 0);  /* S443 */
}

void clretain_note_local(const char *col, size_t cn, const char *key,
		size_t kn, uint64_t ver, unsigned int now_s)
{
	note_in(col, cn, key, kn, ver, now_s, 1, 0, 0, 0, 0);
}

unsigned long clretain_forget(uint64_t oid, uint32_t incarn, uint64_t upto,
		unsigned int now_s)
{
	unsigned long n = 0;
	unsigned int i;
	int k;

	if (!R_on || !upto)
		return 0;
	for (i = 0; i < NSHARD; i++) {
		struct shard *s = &sh[i];

		pthread_mutex_lock(&s->mx);
		if ((k = spl_slot(s, oid, incarn, 0)) >= 0)
			while (s->sp[k].head && s->sp[k].head->nseq <= upto) {
				struct ent *e = s->sp[k].head;

				ent_drop(s, e, ent_hash(e) / NSHARD);
				n++;
			}
		pthread_mutex_unlock(&s->mx);
	}
	if (n) {
		__atomic_fetch_add(&R_forgot, n, __ATOMIC_RELAXED);
		pthread_mutex_lock(&FG_mx);
		for (i = 0; i < nfg; i++)
			if (FG[i].oid == oid && FG[i].incarn == incarn)
				break;
		if (i == nfg && nfg < SPL_MAX) {
			FG[nfg].oid = oid;
			FG[nfg].incarn = incarn;
			FG[nfg].lost = 0;
			FG[nfg++].through = 0;
		}
		if (i < nfg) {
			if (upto > FG[i].through)
				FG[i].through = upto;
			FG[i].at_s = now_s;
		}
		pthread_mutex_unlock(&FG_mx);
	}
	return n;
}

int clretain_forgot_each(int (*cb)(uint64_t oid, uint32_t incarn,
		uint64_t through, unsigned int at_s, void *ctx), void *ctx)
{
	unsigned int i;
	int rc = 0;

	pthread_mutex_lock(&FG_mx);
	for (i = 0; i < nfg && !rc; i++)
		if (FG[i].through)             /* AT-8: lost-only rows are not that */
			rc = cb(FG[i].oid, FG[i].incarn, FG[i].through, FG[i].at_s,
				ctx);
	pthread_mutex_unlock(&FG_mx);
	return rc;
}

int clretain_gone_each(int (*cb)(uint64_t oid, uint32_t incarn,
		uint64_t gone_hi, void *ctx), void *ctx)
{
	unsigned int i;
	int rc = 0;

	pthread_mutex_lock(&FG_mx);
	for (i = 0; i < nfg && !rc; i++)
		rc = cb(FG[i].oid, FG[i].incarn, FG[i].through > FG[i].lost ?
			FG[i].through : FG[i].lost, ctx);
	pthread_mutex_unlock(&FG_mx);
	return rc;
}

unsigned int clretain_space_each(uint64_t oid, uint32_t incarn, uint64_t from,
		uint64_t to, unsigned int max,
		void (*cb)(const char *col, size_t cn, const char *key, size_t kn,
			uint64_t ver, uint64_t seq, void *ctx), void *ctx)
{
	unsigned int i, n = 0;
	struct ent *e;
	int k;

	if (!R_on)
		return 0;
	for (i = 0; i < NSHARD && n < max; i++) {
		pthread_mutex_lock(&sh[i].mx);
		if ((k = spl_slot(&sh[i], oid, incarn, 0)) >= 0)
			for (e = sh[i].sp[k].head; e && n < max; e = e->snext)
				if (e->nseq >= from && e->nseq <= to) {
					cb(e->k, e->cn, e->k + e->cn + 1,
						e->len - e->cn - 1, e->ver, e->nseq, ctx);
					n++;
				}
		pthread_mutex_unlock(&sh[i].mx);
	}
	return n;
}

void clretain_expire_expiries(unsigned int now_s, unsigned int hold_s)
{
	unsigned long n = 0;
	unsigned int i;

	if (!R_on)
		return;
	for (i = 0; i < NSHARD; i++) {
		pthread_mutex_lock(&sh[i].mx);
		while (sh[i].xhead && now_s - sh[i].xhead->t_s >= hold_s) {
			ent_drop(&sh[i], sh[i].xhead, ent_hash(sh[i].xhead) / NSHARD);
			n++;
		}
		pthread_mutex_unlock(&sh[i].mx);
	}
	if (n)
		__atomic_fetch_add(&R_forgot_exp, n, __ATOMIC_RELAXED);
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
			uint64_t ver, int expiry, uint64_t oid, uint32_t incarn,
			uint64_t seq, void *ctx), void *ctx)
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
				e->ver, e->expiry,               /* S446: the kind */
				e->nseq ? sh[i].sp[e->spi].oid : 0,     /* AT-5 */
				e->nseq ? sh[i].sp[e->spi].incarn : 0, e->nseq, ctx);
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
		while (sh[i].head && now_s - sh[i].head->t_s >= R_retain_s) {
			if (sh[i].head->nseq)
				lost_note(&sh[i], sh[i].head);      /* AT-8 */
			else
				time_dropped(sh[i].head->t_s);      /* AT-8 */
			ent_drop(&sh[i], sh[i].head, ent_hash(sh[i].head) / NSHARD);
		}
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
	out->numbered = __atomic_load_n(&R_num, __ATOMIC_RELAXED);      /* AT-5 */
	out->forgotten = __atomic_load_n(&R_forgot, __ATOMIC_RELAXED);
	out->forgotten_expiries = __atomic_load_n(&R_forgot_exp,
		__ATOMIC_RELAXED);
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

int clretain_lost_since(unsigned int since_s)
{
	if (!R_on)
		return 0;
	return __atomic_load_n(&R_lost_upto_s, __ATOMIC_RELAXED) >= since_s ||
		__atomic_load_n(&R_time_hi_s, __ATOMIC_RELAXED) >= since_s;
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
