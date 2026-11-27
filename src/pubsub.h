/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* pubsub.h - pub/sub engine: subscriptions live on their connection and every
 * delivery runs on the worker that owns it, so no connection crosses a thread */
#ifndef PC_PUBSUB_H
#define PC_PUBSUB_H
#include <stddef.h>
#include <stdint.h>
int  pc_pubsub_init(int nworkers);
void pc_pubsub_worker_register(int worker, int efd);

/* ---- the subscription side (on the owning worker) -------------------- */
/* returns the connection's subscription count after the call (channels
 * and patterns together, as redis counts them), or -1 on no memory */
int pc_pubsub_subscribe(void *conn, int worker, const char *name,
		size_t nlen, int pattern);
int pc_pubsub_unsubscribe(void *conn, const char *name, size_t nlen,
		int pattern);
/* remove ONE subscription of @pattern kind and copy its name out; 0 when
 * none is left - the no-argument (P)UNSUBSCRIBE walks with this */
int pc_pubsub_pop(void *conn, int pattern, char *name, size_t cap,
		size_t *nlen, int *left);
int pc_pubsub_count(void *conn);                /* 0 = not in subscribed mode */
void pc_pubsub_conn_closed(void *conn);         /* proto.c, on teardown */

/* ---- publish and delivery ------------------------------------------- */
/* @origin: 0 a local client's publish (relayed to the fleet), 1 from a peer
 * (never relayed again), 2 a local-only keyspace notification (never relayed) */
int pc_pubsub_publish(const char *chan, size_t clen, const char *data,
		size_t dlen, int origin);
void pc_pubsub_drain(int worker);               /* the worker's eventfd turn */
/* the most bytes queued for one worker (0 = no cap): past it a message for
 * that worker is dropped and counted, never queued */
void pc_pubsub_set_queue_cap(size_t bytes);
/* ---- introspection (PUBSUB CHANNELS / NUMSUB / NUMPAT) ------------------ */
typedef int (*pc_pubsub_name_f)(const char *name, size_t nlen, int nsubs,
		void *arg);
int  pc_pubsub_channels(const char *glob, size_t glen, pc_pubsub_name_f cb,
		void *arg);
int  pc_pubsub_numsub(const char *name, size_t nlen);
int  pc_pubsub_numpat(void);
/* glob matching with PSUBSCRIBE's semantics: * ? [abc] [^a] [a-z] \x -
 * this engine's own linear matcher; exposed for the test */
int  pc_pubsub_match(const char *pat, size_t plen, const char *s, size_t slen);

/* ---- figures ----------------------------------------------------------- */
struct pc_pubsub_stats {
	int channels, patterns, subscribers;
	unsigned long long published, delivered, slow_kills, queued, keyspace;
	unsigned long long relay_sent, relay_recv, relay_lost, relay_dropped;
	/* deliveries not made because an allocation failed on OUR side - never
	 * counted as a slow subscriber, never a reason to close one */
	unsigned long long alloc_failed;
	unsigned long long relay_duplicates;   /* relayed datagrams seen twice, dropped */
	/* messages dropped because a worker's queue was at its cap, and the
	 * bytes queued right now across all workers */
	unsigned long long queue_dropped;
	unsigned long long queue_bytes;
	/* publishing connections paused because a queue they feed passed
	 * half its cap */
	unsigned long long publish_paused;
};
void pc_pubsub_stats(struct pc_pubsub_stats *out);

/* the reserved prefix: internal events publish under it; clients may subscribe
 * to it but not publish into it */
#define PC_PUBSUB_RESERVED "__pc."

/* cluster.c: relay a publish to every live peer; returns peers sent to, 0 with no
 * cluster, -1 when the frame does not fit a datagram (delivered locally only) */
int  pc_cluster_pubsub_relay(const char *chan, size_t clen, const char *data,
		size_t dlen);
/* cluster.c: tell every live peer this node gained its first subscription to a
 * name; @h1/@h2 are an exact name's filter hashes, a pattern travels as text */
void pc_cluster_pubsub_interest_add(uint64_t version, int pattern,
		const char *name, size_t nlen, uint32_t h1, uint32_t h2);
/* relay interest advertised to peers (psinterest.h): set once before the workers
 * start; version 0 = wants everything; full returns 0 when @cap is too small */
void pc_pubsub_set_interest(int on);
uint64_t pc_pubsub_interest_version(void);
size_t pc_pubsub_interest_full(unsigned char *out, size_t cap);
void pc_pubsub_interest_tick(long long now_ms);   /* rebuilds at most every 30 s */
/* the relay's own bookkeeping, from cluster.c */
void pc_pubsub_relay_account(unsigned long long sent, unsigned long long lost,
		unsigned long long dropped, unsigned long long duplicates);

/* the native door's NOTIFY tree for one message: built by the first native
 * subscriber, reused by the rest; the caller zeroes it and frees buf after */
struct pc_ps_note {
	char *buf;
	size_t len;
	int state;                          /* 0 not built, 1 built, -1 failed */
};

/* provided by proto.c: write a frame to a connection this thread owns,
 * and close one that cannot keep up */
int  pc_conn_pubsub_push(void *conn, const char *pat, size_t plen,
		const char *chan, size_t clen, const char *data, size_t dlen,
		const char *resp_frame, size_t flen, struct pc_ps_note *note);
/* push result: SLOW = output at its cap (close, count a slow kill); NOMEM = our
 * allocation failed (skip, keep the conn); BROKEN = socket failed (close only) */
#define PC_PS_PUSH_SLOW   (-1)
#define PC_PS_PUSH_NOMEM  (-2)
#define PC_PS_PUSH_BROKEN (-3)
/* closes from the owner's thread; 1 when THIS call started the close, 0 when
 * already closing, so a dying subscriber is killed once, not per message */
int pc_conn_kill(void *conn, const char *why);
/* a staged push's flush failed (SLOW or BROKEN): close and count it as the
 * push itself would have been */
void pc_pubsub_push_failed(void *conn, int rc);
/* a client publish leaving a target queue at >= half its cap sets pc_ps_pressed;
 * pressure_wait answers 1 once every target is below a quarter, else arms a wake */
#define PC_PS_MASK_WORDS 8
extern __thread int pc_ps_pressed;
int  pc_pubsub_pressure_take(uint64_t *mask);   /* mask may be NULL */
int  pc_pubsub_pressure_wait(const uint64_t *mask);
void pc_pubsub_pause_note(void);

/* a worker defers its cross-worker wakes to the end of its turn: defer
 * once as it starts, flush before it waits */
void pc_pubsub_defer_wakes(void);
void pc_pubsub_flush_wakes(void);
void pc_conn_subs_note(void *conn, int n);   /* for /clients */
#endif /* PC_PUBSUB_H */
