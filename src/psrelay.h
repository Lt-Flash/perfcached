/* SPDX-License-Identifier: GPL-2.0-or-later */
/* psrelay.h - sequence accounting for the pub/sub fleet relay, header-only and pure.  A sequence is
 * [epoch 16, random per process start][lane 8, per sending thread][counter 40, per lane, from 1] */
#ifndef PC_PSRELAY_H
#define PC_PSRELAY_H

#include <stdint.h>

#define PSR_WINDOW      64
#define PSR_COUNTER_MASK 0xFFFFFFFFFFULL

struct psr_lane {
	uint64_t high;                     /* highest counter seen */
	uint64_t bits;                     /* bit i: counter high - i arrived */
	uint64_t floor;                    /* first counter seen this epoch */
	uint16_t epoch;
	uint8_t used;
};

static inline uint64_t psr_seq(uint16_t epoch, uint8_t lane, uint64_t counter)
{
	return ((uint64_t)epoch << 48) | ((uint64_t)lane << 40) |
		(counter & PSR_COUNTER_MASK);
}

static inline uint16_t psr_epoch(uint64_t seq) { return (uint16_t)(seq >> 48); }
static inline uint8_t psr_lane(uint64_t seq) { return (uint8_t)(seq >> 40); }
static inline uint64_t psr_counter(uint64_t seq) { return seq & PSR_COUNTER_MASK; }

#define PSR_DUPLICATE 0
#define PSR_DELIVER   1

/* account one arrival: PSR_DELIVER or PSR_DUPLICATE; *lost grows by the counters pushed out of
 * the 64-wide window unseen, so a loss shows up to 64 later and an arrival past it is delivered */
static inline int psr_accept(struct psr_lane *l, uint64_t seq, uint64_t *lost)
{
	uint16_t epoch = psr_epoch(seq);
	uint64_t c = psr_counter(seq), i;

	if (!l->used || l->epoch != epoch) {
		/* first sighting, or the sender restarted: what came before is
		 * not a loss we can know about */
		l->used = 1;
		l->epoch = epoch;
		l->high = c;
		l->floor = c;
		l->bits = ~0ULL;
		return PSR_DELIVER;
	}
	if (c > l->high) {
		uint64_t d = c - l->high;

		if (d >= PSR_WINDOW) {
			/* the whole window leaves, and so does every counter
			 * between it and the new one */
			*lost += (uint64_t)(PSR_WINDOW - __builtin_popcountll(l->bits)) +
				(d - PSR_WINDOW);
			l->bits = 1;
		} else {
			uint64_t leaving = l->bits >> (PSR_WINDOW - d);

			*lost += d - (uint64_t)__builtin_popcountll(leaving);
			l->bits = (l->bits << d) | 1;
		}
		l->high = c;
		return PSR_DELIVER;
	}
	if (c < l->floor)
		return PSR_DELIVER;                /* older than our first sighting */
	i = l->high - c;
	if (i >= PSR_WINDOW)
		return PSR_DELIVER;                /* already counted lost; late */
	if (l->bits & (1ULL << i))
		return PSR_DUPLICATE;
	l->bits |= 1ULL << i;
	return PSR_DELIVER;
}

/* ---- which path a relay to one peer takes: its relay port only while the peer ANSWERS probes on
 * it, else the cluster socket.  Written by the cluster thread, read atomically by the workers */
#define PSR_PROBE_MS        1000        /* while not direct: probe this often */
#define PSR_REPROBE_MS      1000        /* while direct: re-confirm this often */
/* no answer for this long: fall back.  Three unanswered probes, not two: ticks run a little over a
 * second apart and probe before deciding, so 3 s would expire in the tick sending the third */
#define PSR_DIRECT_TTL_MS   3500
struct psr_path {
	uint64_t nonce;                    /* the outstanding probe's */
	long long ok_ms;                   /* last answer (atomic) */
	long long probe_ms;                /* last probe sent */
	uint16_t port;                     /* advertised, 0 = none (atomic) */
	uint8_t direct;                    /* the decision last reported */
};

/* the peer's advertisement: a new port starts unproven */
static inline void psr_path_port(struct psr_path *p, uint16_t port)
{
	if (__atomic_load_n(&p->port, __ATOMIC_ACQUIRE) == port)
		return;
	__atomic_store_n(&p->ok_ms, 0, __ATOMIC_RELEASE);
	p->probe_ms = 0;
	p->nonce = 0;
	__atomic_store_n(&p->port, port, __ATOMIC_RELEASE);
}

static inline int psr_path_direct(const struct psr_path *p, long long now)
{
	/* the port, its proof, the port again - psr_path_port clears the
	 * proof before it sets a new port, so an old proof never pairs with it */
	uint16_t port = __atomic_load_n(&p->port, __ATOMIC_ACQUIRE);
	long long ok = __atomic_load_n(&p->ok_ms, __ATOMIC_ACQUIRE);

	return port && ok && now - ok < PSR_DIRECT_TTL_MS &&
		__atomic_load_n(&p->port, __ATOMIC_ACQUIRE) == port;
}

/* time to send a probe?  The caller draws the nonce and sends it */
static inline int psr_path_probe_due(struct psr_path *p, long long now,
		uint64_t nonce)
{
	long long every;

	if (!__atomic_load_n(&p->port, __ATOMIC_ACQUIRE) || !nonce)
		return 0;
	every = psr_path_direct(p, now) ? PSR_REPROBE_MS : PSR_PROBE_MS;
	if (p->probe_ms && now - p->probe_ms < every)
		return 0;
	p->probe_ms = now;
	p->nonce = nonce;
	return 1;
}

/* an answer: 1 when it answers the outstanding probe */
static inline int psr_path_answer(struct psr_path *p, uint64_t nonce,
		long long now)
{
	if (!__atomic_load_n(&p->port, __ATOMIC_ACQUIRE) || !nonce ||
	        nonce != p->nonce)
		return 0;
	__atomic_store_n(&p->ok_ms, now ? now : 1, __ATOMIC_RELEASE);
	return 1;
}

/* 1 = became direct, -1 = fell back, 0 = unchanged: log only on a change */
static inline int psr_path_transition(struct psr_path *p, long long now)
{
	int d = psr_path_direct(p, now);

	if (d == p->direct)
		return 0;
	p->direct = (uint8_t)d;
	return d ? 1 : -1;
}

#endif /* PC_PSRELAY_H */
