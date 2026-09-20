/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * psrelay.h - sequence accounting for the pub/sub fleet relay (PS3),
 * rebuilt after the PS7 bench.  Header-only and pure, so the unit test
 * drives exactly what the receiver runs.
 *
 * WHY IT WAS REBUILT.  The relay stamped every M_PUBLISH with one global
 * counter, taken by whichever worker handled the publish, and the
 * receiver counted a gap whenever a number arrived more than one above the
 * last.  Two workers publishing at once take N and N+1 and may send N+1
 * first, so the receiver counted N lost - and never took it back when N
 * arrived.  On the two-host bench every relayed message arrived and every
 * delivery was made, yet relay_lost read 411 at 2k/s and 142,710 at 100k/s
 * where 77,914 were truly missing.  It was also blind after a restart: the
 * counter began again at 1, below the old high-water mark, and real gaps
 * went uncounted until it caught up.
 *
 * THE SEQUENCE (the same 8 bytes on the wire):
 *     [ epoch 16 ][ lane 8 ][ counter 40 ]
 * epoch   drawn at random when the process starts: a new epoch is a restart
 * lane    one per sending thread, so a lane's datagrams leave in order
 * counter per lane, from 1 (2^40 messages a lane a process lifetime)
 *
 * THE RECEIVER keeps, per sender node and lane, the highest counter seen and
 * a 64-bit window of which of the 64 counters below it arrived.  A late
 * datagram inside the window fills its bit: no loss.  A counter is lost only
 * when it falls out of the window unseen, so a loss is reported up to 64
 * messages later on that lane - and a lane that falls silent never reports
 * its last gap.  A bit already set is a duplicate and is dropped, which is
 * what at-most-once promises; beyond the window a late datagram is delivered
 * (it was already counted lost), so the promise holds within 64 messages.
 */
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

/* account one arrival.  PSR_DELIVER or PSR_DUPLICATE; *lost grows by the
 * counters this arrival pushed out of the window unseen. */
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

/* ---- which path a relay to one peer takes (PS11) --------------------------
 * A node may receive relays on a dedicated port, read by several threads,
 * instead of the one cluster socket that also carries membership, pulls and
 * replication.  A sender uses that port only once the peer has ANSWERED a
 * probe on it, and only while the answers stay fresh: a firewall that drops
 * the port, or a peer that stopped listening, must never turn into relays
 * sent into a hole.  Until then, and after the answers stop, relays go to
 * the cluster socket as they always did.
 *
 * Written by the cluster thread (port, probe, answer); read by the sending
 * workers (psr_path_direct), which is why port and ok_ms are stored and
 * loaded atomically - one relay taking the old path during a change is
 * harmless. */
#define PSR_PROBE_MS        1000        /* while not direct: probe this often */
#define PSR_REPROBE_MS      1000        /* while direct: re-confirm this often */
/* no answer for this long: fall back.  Three unanswered probes, not two:
 * the cluster thread's ticks run a little over a second apart and a tick
 * probes before it decides, so 3 s would expire in the tick that sends the
 * third probe.  At 10 s / 30 s a firewall took 30 s of relays on the rig
 * (59,602 at 2,000/s); this bounds that to ~3.5 s for one small datagram a
 * second per peer. */
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
	long long ok = __atomic_load_n(&p->ok_ms, __ATOMIC_ACQUIRE);

	return __atomic_load_n(&p->port, __ATOMIC_ACQUIRE) && ok &&
		now - ok < PSR_DIRECT_TTL_MS;
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
