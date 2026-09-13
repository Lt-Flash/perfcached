/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clpend.h - parked requests and the per-worker completion queues (M2).
 *
 * One mechanism, not two: a request parks in the table, its answer is
 * posted to the queue of the worker that parked it, and the two are
 * sized together so a completion can never be dropped for want of room.
 *
 * What is deliberately NOT here, and stays in cluster.c: the ANSWER
 * path.  Deciding what a peer's reply means reaches the locator, the
 * peer table, the wire and the store in one breath - it sends demotes,
 * writes locator entries and logs - so under rule 4 it cannot live in a
 * module that must link alone.  doc/MODULARITY.md's appendix specifies
 * clpend_answer(); it waits on M3 (clloc) and M6 (clwire) naming those
 * primitives, and case group H of that catalogue waits with it.
 *
 * The clock is a parameter to alloc and to expire, so deadlines are
 * exercised without sleeping, and the module never logs: alloc reports
 * a refusal through *reason and the caller shouts, rate-limited.
 */
#ifndef PC_CLPEND_H
#define PC_CLPEND_H

#include <pthread.h>
#include <stdint.h>

#include "cluster.h"                 /* struct pc_pull_done, PC_CLFAIL_* */

/* the CQ array's bound; only the completion queues use it */
#define WORKERS_MAX   520

/* Parked-request table.  Sized at startup from [cluster] max_pending
 * and never resized: pend_find() hands out RAW POINTERS into it and
 * about 117 uses hang off them, so a realloc under load would be a
 * use-after-free.
 *
 * The default was 1024 and that refused a single deeply-pipelined RESP
 * client: 50 connections at pipeline 64 is 3200 requests in flight, and
 * a client that cannot compute the owner forwards nearly all of them.
 * Measured, that peaked at 711 slots on a 16-core host and exhausted
 * 1024 outright on an 8-core one - every shard row at pipeline >= 16
 * was reporting the ceiling rather than the mode.  Raising it was only
 * affordable once pend_find() stopped being a walk of the whole table.
 *
 * PEND_LIMIT is a HARD ceiling, not a preference: the slot index rides
 * in the low PEND_SLOT_BITS of the request id. */
#define PEND_DEFAULT  8192
#define PEND_LIMIT    (1 << PEND_SLOT_BITS)
#define PEND_MIN      64

/* The parked-request id is a HANDLE, not a ticket: bit 31 marks it as
 * one, the low PEND_SLOT_BITS index the slot, and the bits between are
 * a per-slot generation, so a REUSED slot yields a different id and a
 * reply that arrives after its slot was recycled fails the compare
 * instead of completing somebody else's request.
 *
 * pend_find() runs once per reply and used to walk the whole table.
 * That is affordable at 1024 slots and is exactly what stops the table
 * growing: raising MAX_PENDING to relieve the backpressure would have
 * made every reply proportionally more expensive, so the ceiling could
 * not be lifted without this first.  S38.
 *
 * Migration group ids (C.mig_out) are drawn from the same counter but
 * are matched in their own table under M_MIGRATE_ACK, so the two id
 * spaces never meet in a lookup.  They are masked to 31 bits anyway, so
 * PEND_TAG makes the two provably disjoint rather than merely
 * unreachable. */
#define PEND_TAG        0x80000000u
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
	long long deadline_ms;
	char col[40];
	unsigned char collen;
	char key[256];
	unsigned short klen;           /* 0 = too big for demote tracking */
	unsigned char jop;             /* PC_DONE_JSON: the PC_JOP_* code */
	/* probe-before-place (proxy set/add, holder unknown): the deferred
	 * write, forwarded or replayed when the probe answers */
	unsigned char probe_op;        /* 0 none, 1 set, 2 add */
	unsigned char probe_fwd;       /* probe transformed into a forward:
	                                * the ack completes it; stray
	                                * positives go to the demoter */
	char *stash;                   /* set: the malloc'd value */
	int stash_len;
	long long by;
	unsigned int ttl_rel;
};

struct cqueue {
	pthread_mutex_t mx;
	/* pend_cap entries, malloc'd at worker registration.  It MUST
	 * match the parked-request table: every parked request could
	 * belong to one worker, and post_done_ex() silently DROPS a
	 * completion into a full queue - the request would then only
	 * resolve by timing out.  Sizing them together is what makes that
	 * unreachable. */
	struct pc_pull_done *q;
	int head, tail;
	int efd;
};

/* The parked-request plane's state.  `C` embeds one; every function
 * below takes the pointer.  next_req is NOT here: the migration
 * allocator draws from it and pend_alloc never touches it, whatever the
 * ownership table in doc/MODULARITY.md says. */
struct clpend {
	pthread_mutex_t mx;            /* was C.pmx */
	struct pending *pend;          /* cap entries, allocated once */
	int cap;
	int used;                      /* EXACT occupancy; see clpend_release */
	int peak;
	unsigned long long exhausted;
	struct cqueue cq[WORKERS_MAX];
};

/* Clamps cap into [PEND_MIN, PEND_LIMIT] and returns the value used, or
 * 0 if the allocation failed.  The caller compares the returned cap with
 * what it asked for and logs the clamp; this does not.  Startup. */
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
int clpend_peak(const struct clpend *p);
unsigned long long clpend_exhausted(const struct clpend *p);

/* completion queues: sized to cap, so a post cannot find one full */
void clpend_cq_register(struct clpend *p, int worker, int efd);
void clpend_cq_post(struct clpend *p, int worker, uint32_t req, int kind,
		int found, int ok, long long newval, int from_node,
		unsigned int ttl_left, const unsigned char *val, int vlen,
		int jop, int count, unsigned long long ver, int timedout);
int clpend_cq_drain(struct clpend *p, int worker, struct pc_pull_done *out,
		int max);

/* Retire a parked request that a FORWARD has acked, and say who to wake.
 *
 * Returns 1 with *worker and *kind set when a slot was retired, 0 when
 * the request names nothing we hold - a duplicate ack, or one for a
 * request already timed out.
 *
 * `only_unanswered` is the difference between the two ack paths, and
 * making it a parameter is the point of having one function: the FWD
 * ack refuses a request already answered (a probe transformed into a
 * forward completes on the holder's ack, and a stray second one must
 * not complete it twice), while the JSON ack does not check.  That
 * asymmetry was previously split across two handlers three thousand
 * lines apart, where nothing showed they differed.
 *
 * Takes the lock and returns with it released.  Peer thread.
 */
int clpend_retire(struct clpend *p, uint32_t req, int only_unanswered,
		int *worker, int *kind);

/* ---- the answer path ---------------------------------------------------
 *
 * What a pull reply MEANS, decided in one place under one lock.  The
 * caller performs every side effect - the demote, the locator write, the
 * completion, the store delete, the log - because rule 4 bars this
 * module from logging and the store is not this plane's to touch.
 *
 * Everything the caller needs comes back COPIED in `out`.  That is not
 * tidiness.  The code this replaced kept `rec_col = p->col`, a bare
 * pointer into the slot, and read it after the slot was released and the
 * lock dropped - so a worker reusing that slot changed which collection
 * a reconcile deleted from.  Copying makes the whole class unreachable.
 *
 * `fwd_deadline_ms` is used only on the PROBE_FWD path, and is a
 * parameter for the same reason `now` is elsewhere: no stored clock, so
 * the test is deterministic.
 *
 * Takes the lock and returns with it released.  Peer thread.
 */
enum clpend_ans {
	CLPEND_ANS_IGNORE = 0,  /* no such slot, a stale generation, or the
	                         * request was answered and is now freed */
	CLPEND_ANS_WAIT,        /* a negative, and answers are still due */
	CLPEND_ANS_HIT,         /* the first positive: complete as a hit */
	CLPEND_ANS_MISS,        /* every node said no */
	CLPEND_ANS_RACE,        /* a second positive: two holders */
	CLPEND_ANS_PROBE_FWD,   /* probe-before-place hit: forward to it */
	CLPEND_ANS_RESUME,      /* probe missed fleet-wide: replay the write */
	CLPEND_ANS_RECONCILE,   /* nobody holds it: drop the replayed record */
};

struct clpend_ans_out {
	int worker;
	int kind;                      /* PROBE_FWD: the transformed kind */
	int from_node;                 /* HIT, PROBE_FWD: who answered */
	int winner, loser;             /* RACE */
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

int clpend_answer(struct clpend *p, uint32_t req, int positive, int from_node,
		long long fwd_deadline_ms, struct clpend_ans_out *out);

/* Sweep for deadlines at or before `now`: release each slot, free its
 * probe stash, and post a timeout completion for any not already
 * answered.  Returns how many timed out, for the caller's counter.
 * Peer thread. */
int clpend_expire(struct clpend *p, long long now);

#endif /* PC_CLPEND_H */
