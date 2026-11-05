/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * psudp.h - a connection's pub/sub push stream over UDP (PS5), the part
 * that decides: when to probe, when a probe has gone unanswered too long,
 * and when a confirmed stream has stopped being acknowledged.  Header-only
 * and pure, so the unit test drives what the worker runs.  The datagram is
 * lib/perfd_push.h; proto.c does the sockets.
 *
 * THE LIFE OF A STREAM
 *   probing   PSU_PROBES probes, PSU_PROBE_MS apart; deliveries stay on TCP.
 *             Unconfirmed after PSU_EXPIRE_MS, the stream is dropped without
 *             a word - the client's own wait for a probe has long ended.
 *   active    deliveries that fit go over UDP.  Pruned when no ACK has come
 *             for PFP_PRUNE_MS (a client that stopped), or when the daemon
 *             has been sending past an acknowledgement that stopped moving
 *             for PFP_PRUNE_MS (UDP broke while TCP still carries the ACKs).
 *             An idle stream is not stalled: nothing unacknowledged.
 */
#ifndef PC_PSUDP_H
#define PC_PSUDP_H

#include <stdint.h>

#include "../lib/perfd_push.h"

#define PSU_PROBES      3
#define PSU_PROBE_MS    1000
#define PSU_EXPIRE_MS   10000

struct psu_stream {
	uint64_t id;
	uint64_t seq;                      /* the last sequence used */
	uint64_t acked;                    /* the highest the client reported */
	long long started_ms;
	long long probe_ms;                /* the last probe */
	long long ack_ms;                  /* the last acknowledgement */
	long long pending_ms;              /* since when something sent is
	                                    * unacknowledged and the ack has not
	                                    * moved; 0 = nothing pending */
	int probes;
	uint8_t active;
};

#define PSU_NOTHING         0
#define PSU_PROBE           1
#define PSU_EXPIRE          2
#define PSU_PRUNE_SILENT    3
#define PSU_PRUNE_STALLED   4

static inline void psu_start(struct psu_stream *s, uint64_t id, long long now)
{
	s->id = id;
	s->seq = 0;
	s->acked = 0;
	s->started_ms = now;
	s->probe_ms = 0;
	s->ack_ms = 0;
	s->pending_ms = 0;
	s->probes = 0;
	s->active = 0;
}

/* what the sweep should do now */
static inline int psu_due(const struct psu_stream *s, long long now)
{
	if (!s->active) {
		if (s->probes < PSU_PROBES &&
		    (!s->probes || now - s->probe_ms >= PSU_PROBE_MS))
			return PSU_PROBE;
		return now - s->started_ms >= PSU_EXPIRE_MS ? PSU_EXPIRE
			: PSU_NOTHING;
	}
	if (now - s->ack_ms >= PFP_PRUNE_MS)
		return PSU_PRUNE_SILENT;
	if (s->pending_ms && now - s->pending_ms >= PFP_PRUNE_MS)
		return PSU_PRUNE_STALLED;
	return PSU_NOTHING;
}

/* a probe is going out: its sequence */
static inline uint64_t psu_probe_sent(struct psu_stream *s, long long now)
{
	s->probes++;
	s->probe_ms = now;
	return ++s->seq;
}

/* the client echoed the cookie: what it has seen so far counts as seen */
static inline void psu_confirm(struct psu_stream *s, long long now)
{
	s->active = 1;
	s->ack_ms = now;
	s->acked = s->seq;
	s->pending_ms = 0;
}

/* a message is going out: its sequence */
static inline uint64_t psu_message_sent(struct psu_stream *s, long long now)
{
	if (!s->pending_ms)
		s->pending_ms = now;               /* something is unacknowledged */
	return ++s->seq;
}

/* an acknowledgement: 0, or -1 when it claims more than was sent */
static inline int psu_ack(struct psu_stream *s, uint64_t seq, long long now)
{
	if (seq > s->seq)
		return -1;
	s->ack_ms = now;
	if (seq > s->acked) {
		s->acked = seq;
		/* progress: the stall clock starts again from here, if
		 * anything is still unacknowledged */
		s->pending_ms = s->acked >= s->seq ? 0 : now;
	}
	return 0;
}

#endif /* PC_PSUDP_H */
