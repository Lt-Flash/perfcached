/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clpend.h - parked requests and the per-worker completion queues, sized together so a completion
 * is never dropped for want of room.  The clock is a parameter; nothing logs (*reason instead). */
#ifndef PC_CLPEND_H
#define PC_CLPEND_H

#include <pthread.h>
#include "winctr.h"
#include <stdint.h>

#include "cluster.h"                 /* struct pc_pull_done, PC_CLFAIL_* */

/* the CQ array's bound; only the completion queues use it */
#define WORKERS_MAX   520

/* the parked-request table: sized at startup from max_pending and never resized, as pend_find()
 * hands out RAW POINTERS into it.  PEND_LIMIT is HARD: the slot index rides in the request id. */
#define PEND_DEFAULT  8192
#define PEND_LIMIT    (1 << PEND_SLOT_BITS)
#define PEND_MIN      64

/* the parked-request id is a HANDLE: bit 31 tags it, the low PEND_SLOT_BITS index the slot, a
 * per-slot generation between, so a reply after its slot was recycled fails the compare */
#define PEND_TAG        0x80000000u     /* migration ids, masked to 31 bits, never match */
#define PEND_SLOT_BITS  16
#define PEND_SLOT_MASK  ((1u << PEND_SLOT_BITS) - 1)
#define PEND_GEN_SHIFT  PEND_SLOT_BITS
#define PEND_GEN_MASK   (PEND_TAG - 1 - PEND_SLOT_MASK)

struct pending {
	uint32_t req;                  /* 0 = free slot; else a handle,
	                                * see PEND_TAG */
	uint32_t gen;                  /* bumped per alloc, rides in req so
	                                * a recycled slot is a NEW id */
	int worker;                    /* pc_worker_id() to deliver to */
	int expect;                    /* answers still awaited */
	int kind;                      /* PC_DONE_* */
	int answered;                  /* completion already posted */
	int first_node;                /* birth-race: the first positive */
	unsigned long long first_ver;  /* and the VERSION it answered with: two holders
	                                * are normal, only a DIFFERENT version is a race */
	uint32_t first_sum;            /* and its VALUE's checksum (0 = none): equal versions
	                                * with different bytes ARE a race (a partition) */
	long long deadline_ms;
	char col[40];
	unsigned char collen;
	char key[256];
	unsigned short klen;           /* 0 = too big for demote tracking */
	unsigned char jop;             /* PC_DONE_JSON: the PC_JOP_* code */
	/* probe-before-place (proxy set/add, holder unknown): the deferred
	 * write, forwarded or replayed when the probe answers */
	unsigned char probe_op;        /* 0 none, 1 set, 2 add, 3 touch, 4 del */
	unsigned char probe_fwd;       /* probe turned forward: the ack completes it,
	                                * stray positives go to the demoter */
	char *stash;                   /* set: the malloc'd value */
	int stash_len;
	long long by;
	unsigned int ttl_rel;
};

struct cqueue {
	pthread_mutex_t mx;
	/* pend_cap entries, malloc'd at worker registration: it MUST match the parked table, as
	 * post_done_ex() silently DROPS a completion into a full queue */
	struct pc_pull_done *q;          /* qmask + 1 entries, a power of two */
	unsigned int head, tail;       /* free-running; the slot is & qmask */
	int efd;
};

/* the parked-request plane's state, embedded in `C`; next_req is NOT here: the migration allocator
 * draws from it and pend_alloc never touches it */
struct clpend {
	pthread_mutex_t mx;
	struct pending *pend;          /* cap entries, allocated once */
	int cap;
	unsigned int qmask;            /* each completion ring: cap rounded up to 2^k, less 1 */
	int used;                      /* EXACT occupancy; see clpend_release */
	int peak;
	struct pc_winmax wpeak;        /* the peak of the last 5 min */
	/* occupancy BY KIND (PC_DONE_*, 0..15): reconcile probes not yet answered are kind
	 * PC_DONE_RECONCILE's, whatever the answer turns out to be */
	int used_kind[16];
	unsigned long long exhausted;
	/* PC_DONE_RECONCILE slots gone with no verdict; clpend_expire's
	 * thread reads and zeroes it */
	unsigned long long reconcile_expired;
	struct cqueue cq[WORKERS_MAX];
};

/* clamps cap into [PEND_MIN, PEND_LIMIT] and returns the value used (the caller logs a clamp), or
 * 0 if the allocation failed.  Startup. */
int clpend_init(struct clpend *p, int cap);
void clpend_fini(struct clpend *p);

/* the lock, for callers holding it across a decision - the ack path
 * still does.  Any thread. */
void clpend_lock(struct clpend *p);
void clpend_unlock(struct clpend *p);

/* Park a request.  Returns the handle, or 0 with *reason set to
 * PC_CLFAIL_BUSY when the table is full.  Takes the lock.  Worker. */
uint32_t clpend_alloc(struct clpend *p, int kind, int expect, int extra_ms,
		int worker, long long now, int pull_timeout_ms,
		struct pending **out, int *reason);

/* O(1) by slot; needs the tag AND an exact req match, so a reply for a
 * recycled slot fails the compare.  Caller holds the lock. */
struct pending *clpend_find(struct clpend *p, uint32_t req);

/* The ONLY free path, so occupancy stays exact.  Idempotent. */
void clpend_release_locked(struct clpend *p, struct pending *slot);
void clpend_release(struct clpend *p, struct pending *slot);

int clpend_used(const struct clpend *p);
int clpend_used_kind(const struct clpend *p, int kind);
int clpend_peak(const struct clpend *p);
/* the peak of the last five minutes */
int clpend_peak_win(const struct clpend *p);
unsigned long long clpend_exhausted(const struct clpend *p);

/* completion queues: sized to cap, so a post cannot find one full */
void clpend_cq_register(struct clpend *p, int worker, int efd);
void clpend_cq_post(struct clpend *p, int worker, uint32_t req, int kind,
		int found, int ok, long long newval, int from_node,
		unsigned int ttl_left, const unsigned char *val, int vlen,
		int jop, int count, unsigned long long ver, int timedout);
int clpend_cq_drain(struct clpend *p, int worker, struct pc_pull_done *out,
		int max);

/* retire a parked request a FORWARD acked: 1 with *worker and *kind, 0 when it names nothing held;
 * @only_unanswered (the FWD ack, not JSON's) refuses one answered.  Locks itself; peer thread. */
int clpend_retire(struct clpend *p, uint32_t req, int only_unanswered,
		int *worker, int *kind);

/* ---- the answer path: what a pull reply MEANS, under one lock; the caller does every side effect.
 * All it needs comes back COPIED in `out`: a released slot may be reused.  Peer thread. */
enum clpend_ans {
	CLPEND_ANS_IGNORE = 0,  /* no such slot, a stale generation, or the
	                         * request was answered and is now freed */
	CLPEND_ANS_WAIT,        /* a negative, and answers are still due */
	CLPEND_ANS_HIT,         /* the first positive: complete as a hit */
	CLPEND_ANS_MISS,        /* every node said no */
	CLPEND_ANS_RACE,        /* a second positive at a DIFFERENT version:
	                         * the holders genuinely disagree */
	CLPEND_ANS_SAME,        /* a second positive at the SAME version: the
	                         * same record (normal in eager mode) */
	CLPEND_ANS_PROBE_FWD,   /* probe-before-place hit: forward to it */
	CLPEND_ANS_RESUME,      /* probe missed fleet-wide: replay the write */
	CLPEND_ANS_RECONCILE,   /* nobody holds it: drop the replayed record */
	CLPEND_ANS_POSSESS,     /* a designated holder CONFIRMED it has the record:
	                         * our surplus copy may go (RECONCILE's mirror) */
};

struct clpend_ans_out {
	int worker;
	int kind;                      /* PROBE_FWD: the transformed kind */
	int from_node;                 /* HIT, PROBE_FWD: who answered */
	int winner, loser;             /* RACE */
	unsigned long long loser_ver;  /* RACE: the loser's copy, 0 = unknown */
	char col[40];
	unsigned int collen;
	char key[256];
	unsigned int klen;
	unsigned char probe_op;        /* PROBE_FWD, RESUME: raw, as stored */
	char *stash;                   /* ownership PASSES to the caller */
	int stash_len;
	long long by;
	unsigned int ttl_rel;
};

/* @ver, the version answered (0: cannot prove sameness), tells "same record" from "disagree";
 * @vsum the value's checksum, 0 = none (equal versions then the same record) */
int clpend_answer(struct clpend *p, uint32_t req, int positive, int from_node,
		unsigned long long ver, uint32_t vsum, long long fwd_deadline_ms,
		struct clpend_ans_out *out);

/* sweep deadlines at or before `now`: release each slot, free its probe stash, post a timeout
 * completion for any not already answered; returns how many timed out.  Peer thread. */
int clpend_expire(struct clpend *p, long long now);

#endif /* PC_CLPEND_H */
