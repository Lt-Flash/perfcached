/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * pubsub.h - the pub/sub engine (PS1): subscriptions live on the
 * connection that made them, a publish is delivered to every local
 * subscriber by the worker that OWNS each connection, and the fleet
 * relay (PS3) hands a node the messages published elsewhere.
 *
 * Every entry point here takes the connection as an opaque pointer -
 * the engine never touches a connection itself.  Delivery goes through
 * proto.c (pc_conn_pubsub_push / pc_conn_kill) on the owner's thread:
 * a publish on worker A hands worker B a message through B's queue and
 * eventfd, and B resolves its own subscribers when it drains, so no
 * connection pointer ever crosses a thread.  Subscribe, unsubscribe and
 * the close hook take the global lock; publish takes it once to find
 * out who is subscribed and then lets go.
 */
#ifndef PC_PUBSUB_H
#define PC_PUBSUB_H
#include <stddef.h>

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
/* local receivers.  @origin: 0 a client's publish here (relayed to the
 * fleet), 1 a message that arrived from a peer (delivered, never relayed
 * again), 2 a local-only event - a keyspace notification (PS8), which
 * every node emits for what it applies and none relays. */
int pc_pubsub_publish(const char *chan, size_t clen, const char *data,
		size_t dlen, int origin);
void pc_pubsub_drain(int worker);               /* the worker's eventfd turn */

/* ---- introspection (PUBSUB CHANNELS / NUMSUB / NUMPAT) ------------------ */
typedef int (*pc_pubsub_name_f)(const char *name, size_t nlen, int nsubs,
		void *arg);
int  pc_pubsub_channels(const char *glob, size_t glen, pc_pubsub_name_f cb,
		void *arg);
int  pc_pubsub_numsub(const char *name, size_t nlen);
int  pc_pubsub_numpat(void);
/* redis's stringmatchlen: * ? [abc] [^a] [a-z] \x; exposed for the test */
int  pc_pubsub_match(const char *pat, size_t plen, const char *s, size_t slen);

/* ---- figures ----------------------------------------------------------- */
struct pc_pubsub_stats {
	int channels, patterns, subscribers;
	unsigned long long published, delivered, slow_kills, queued, keyspace;
	unsigned long long relay_sent, relay_recv, relay_lost, relay_dropped;
};
void pc_pubsub_stats(struct pc_pubsub_stats *out);

/* the reserved prefix: internal events (PS6) publish under it, clients
 * may subscribe to it and may not publish into it */
#define PC_PUBSUB_RESERVED "__pc."

/* provided by cluster.c (PS3): relay a publish to every live peer on the
 * sealed unicast plane.  Returns peers sent to, 0 with no cluster, -1
 * when the frame does not fit a datagram (delivered locally, not relayed). */
int  pc_cluster_pubsub_relay(const char *chan, size_t clen, const char *data,
		size_t dlen);
/* the relay's own bookkeeping, from cluster.c */
void pc_pubsub_relay_account(unsigned long long sent, unsigned long long lost,
		unsigned long long dropped);

/* provided by proto.c: write a frame to a connection this thread owns,
 * and close one that cannot keep up */
int  pc_conn_pubsub_push(void *conn, const char *pat, size_t plen,
		const char *chan, size_t clen, const char *data, size_t dlen,
		const char *resp_frame, size_t flen);
void pc_conn_kill(void *conn, const char *why);
#endif /* PC_PUBSUB_H */
