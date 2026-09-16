/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * pubsub.c - the pub/sub engine.  See pubsub.h for the contract and
 * doc/DESIGN.md 12eb (PS1) for why it is shaped this way.
 *
 * TABLES.  One global lock guards two hash tables - channels and
 * patterns - whose entries hold the subscriptions (connection + owning
 * worker) hanging off them, and a per-connection list of the same
 * subscriptions so a close or a bare UNSUBSCRIBE can walk them.  NUMSUB
 * is the channel entry's count, O(1) by name, because the socket.io
 * adapter calls it per emit (S52).
 *
 * DELIVERY.  A publish resolves its receivers under the lock - the
 * channel's subscribers plus every pattern that matches - and notes
 * which workers own one.  For its own worker it delivers inline; for
 * every other it queues the message on that worker's queue and writes
 * the worker's eventfd.  A draining worker resolves its OWN receivers
 * again under the lock, so a connection that closed in between is
 * simply not there, and writes the frames itself.  The message buffer
 * is reference-counted across the workers it was queued to.
 *
 * SLOW SUBSCRIBERS die at the output cap (proto.c refuses the append),
 * which is redis's client-output-buffer-limit behaviour.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "pubsub.h"

#define PS_CH_BUCKETS   4096
#define PS_PAT_BUCKETS  256
#define PS_MAX_WORKERS  512
#define PS_NAME_MAX     4096

struct ps_entry;
struct ps_conn;

struct ps_sub {
	struct ps_sub *next_in_entry;      /* the entry's subscriber list */
	struct ps_sub *next_in_conn;       /* the connection's list */
	struct ps_entry *entry;
	struct ps_conn *owner;
};

struct ps_entry {                      /* a channel or a pattern */
	struct ps_entry *next;
	char *name;
	size_t nlen;
	unsigned hash;
	int nsubs;
	int pattern;
	struct ps_sub *subs;
};

struct ps_conn {
	void *conn;
	int worker;
	int nchan, npat;
	struct ps_sub *subs;
	struct ps_conn *next;              /* the registry of subscribed conns */
};

struct ps_msg {
	int refs;                          /* workers still to deliver it */
	int relayed;
	size_t clen, dlen;
	char *frame;                       /* the "message" frame, built once */
	size_t flen;
	char *chan;
	char *data;
};

struct ps_qnode {
	struct ps_qnode *next;
	struct ps_msg *m;
};

struct ps_wq {
	pthread_mutex_t lk;
	struct ps_qnode *head, *tail;
	int efd;
	int n;
};

static pthread_mutex_t ps_lk = PTHREAD_MUTEX_INITIALIZER;
static struct ps_entry *ps_ch[PS_CH_BUCKETS];
static struct ps_entry *ps_pat[PS_PAT_BUCKETS];
static struct ps_conn *ps_conns;       /* every connection holding a subscription */
static int ps_nch, ps_npat, ps_nsubs;
static struct ps_wq ps_wq[PS_MAX_WORKERS];
static int ps_nworkers;
static unsigned long long ps_published, ps_delivered, ps_slow_kills, ps_queued;
static unsigned long long ps_relay_sent, ps_relay_recv, ps_relay_lost,
	ps_relay_dropped;

/* ---- the connection's own worker: who owns which connection ------------
 * pc_worker_id() lives in daemon.c; the engine only needs the number the
 * subscribe call passes in, so nothing here reaches into the daemon. */

static unsigned ps_hash(const char *s, size_t n)
{
	unsigned h = 2166136261u;
	size_t i;

	for (i = 0; i < n; i++)
		h = (h ^ (unsigned char)s[i]) * 16777619u;
	return h;
}

/* redis's stringmatchlen, ported for the same answers on the same
 * patterns: * ? [set] [^set] [a-z] and \ as the escape.  Recursive on
 * '*' like the original, which is bounded by the pattern length. */
int pc_pubsub_match(const char *pat, size_t plen, const char *s, size_t slen)
{
	while (plen && slen) {
		switch (pat[0]) {
		case '*':
			while (plen > 1 && pat[1] == '*') { pat++; plen--; }
			if (plen == 1)
				return 1;
			while (slen) {
				if (pc_pubsub_match(pat + 1, plen - 1, s, slen))
					return 1;
				s++; slen--;
			}
			return 0;
		case '?':
			s++; slen--;
			break;
		case '[': {
			int not, match = 0;

			pat++; plen--;
			not = plen && pat[0] == '^';
			if (not) { pat++; plen--; }
			for (;;) {
				if (plen && pat[0] == '\\' && plen >= 2) {
					pat++; plen--;
					if (pat[0] == s[0])
						match = 1;
				} else if (!plen || pat[0] == ']') {
					break;
				} else if (plen >= 3 && pat[1] == '-') {
					int start = (unsigned char)pat[0];
					int end = (unsigned char)pat[2];
					int c = (unsigned char)s[0];

					if (start > end) { int t = start; start = end; end = t; }
					pat += 2; plen -= 2;
					if (c >= start && c <= end)
						match = 1;
				} else if (pat[0] == s[0]) {
					match = 1;
				}
				pat++; plen--;
			}
			if (not)
				match = !match;
			if (!match)
				return 0;
			s++; slen--;
			break;
		}
		case '\\':
			if (plen >= 2) { pat++; plen--; }
			/* fall through */
		default:
			if (pat[0] != s[0])
				return 0;
			s++; slen--;
			break;
		}
		pat++; plen--;
		if (!slen) {
			while (plen && pat[0] == '*') { pat++; plen--; }
			break;
		}
	}
	return plen == 0 && slen == 0;
}

/* ---- entries ------------------------------------------------------------ */

static struct ps_entry *entry_find(int pattern, const char *name, size_t nlen,
		unsigned h, int create)
{
	struct ps_entry **tab = pattern ? ps_pat : ps_ch;
	unsigned nb = pattern ? PS_PAT_BUCKETS : PS_CH_BUCKETS;
	struct ps_entry *e;

	for (e = tab[h % nb]; e; e = e->next)
		if (e->hash == h && e->nlen == nlen && !memcmp(e->name, name, nlen))
			return e;
	if (!create)
		return NULL;
	e = calloc(1, sizeof *e);
	if (!e)
		return NULL;
	e->name = malloc(nlen + 1);
	if (!e->name) {
		free(e);
		return NULL;
	}
	memcpy(e->name, name, nlen);
	e->name[nlen] = 0;
	e->nlen = nlen;
	e->hash = h;
	e->pattern = pattern;
	e->next = tab[h % nb];
	tab[h % nb] = e;
	if (pattern)
		ps_npat++;
	else
		ps_nch++;
	return e;
}

static void entry_drop_if_empty(struct ps_entry *e)
{
	struct ps_entry **tab = e->pattern ? ps_pat : ps_ch;
	unsigned nb = e->pattern ? PS_PAT_BUCKETS : PS_CH_BUCKETS;
	struct ps_entry **pp;

	if (e->nsubs)
		return;
	for (pp = &tab[e->hash % nb]; *pp; pp = &(*pp)->next)
		if (*pp == e) {
			*pp = e->next;
			break;
		}
	if (e->pattern)
		ps_npat--;
	else
		ps_nch--;
	free(e->name);
	free(e);
}

/* ---- per-connection state ------------------------------------------------
 * Found by the opaque pointer: a scan of the subscribed connections, which
 * are few relative to connections (a subscriber holds one connection for
 * its lifetime) and only on subscribe / unsubscribe / close. */

static struct ps_conn *conn_find(void *conn, int worker, int create)
{
	struct ps_conn *pc;

	for (pc = ps_conns; pc; pc = pc->next)
		if (pc->conn == conn)
			return pc;
	if (!create)
		return NULL;
	pc = calloc(1, sizeof *pc);
	if (!pc)
		return NULL;
	pc->conn = conn;
	pc->worker = worker;
	pc->next = ps_conns;
	ps_conns = pc;
	return pc;
}

static void conn_drop_if_empty(struct ps_conn *pc)
{
	struct ps_conn **pp;

	if (pc->subs)
		return;
	for (pp = &ps_conns; *pp; pp = &(*pp)->next)
		if (*pp == pc) {
			*pp = pc->next;
			break;
		}
	free(pc);
}

static void sub_unlink(struct ps_sub *s)
{
	struct ps_sub **pp;

	for (pp = &s->entry->subs; *pp; pp = &(*pp)->next_in_entry)
		if (*pp == s) { *pp = s->next_in_entry; break; }
	for (pp = &s->owner->subs; *pp; pp = &(*pp)->next_in_conn)
		if (*pp == s) { *pp = s->next_in_conn; break; }
	s->entry->nsubs--;
	if (s->entry->pattern)
		s->owner->npat--;
	else
		s->owner->nchan--;
	ps_nsubs--;
	entry_drop_if_empty(s->entry);
	free(s);
}

int pc_pubsub_init(int nworkers)
{
	int i;

	if (nworkers < 1)
		nworkers = 1;
	if (nworkers > PS_MAX_WORKERS)
		nworkers = PS_MAX_WORKERS;
	ps_nworkers = nworkers;
	for (i = 0; i < PS_MAX_WORKERS; i++) {
		pthread_mutex_init(&ps_wq[i].lk, NULL);
		ps_wq[i].efd = -1;
	}
	return 0;
}

void pc_pubsub_worker_register(int worker, int efd)
{
	if (worker < 0 || worker >= PS_MAX_WORKERS)
		return;
	ps_wq[worker].efd = efd;
	if (worker >= ps_nworkers)
		ps_nworkers = worker + 1;
}

int pc_pubsub_subscribe(void *conn, int worker, const char *name,
		size_t nlen, int pattern)
{
	struct ps_conn *pc;
	struct ps_entry *e;
	struct ps_sub *s;
	unsigned h;
	int n;

	if (nlen == 0 || nlen > PS_NAME_MAX)
		return -1;
	h = ps_hash(name, nlen);
	pthread_mutex_lock(&ps_lk);
	pc = conn_find(conn, worker, 1);
	if (!pc) {
		pthread_mutex_unlock(&ps_lk);
		return -1;
	}
	/* already subscribed: redis answers with the unchanged count */
	for (s = pc->subs; s; s = s->next_in_conn)
		if (s->entry->pattern == pattern && s->entry->hash == h &&
		    s->entry->nlen == nlen && !memcmp(s->entry->name, name, nlen)) {
			n = pc->nchan + pc->npat;
			pthread_mutex_unlock(&ps_lk);
			return n;
		}
	e = entry_find(pattern, name, nlen, h, 1);
	s = e ? calloc(1, sizeof *s) : NULL;
	if (!s) {
		if (e)
			entry_drop_if_empty(e);
		conn_drop_if_empty(pc);
		pthread_mutex_unlock(&ps_lk);
		return -1;
	}
	s->entry = e;
	s->owner = pc;
	s->next_in_entry = e->subs;
	e->subs = s;
	s->next_in_conn = pc->subs;
	pc->subs = s;
	e->nsubs++;
	if (pattern)
		pc->npat++;
	else
		pc->nchan++;
	ps_nsubs++;
	n = pc->nchan + pc->npat;
	pthread_mutex_unlock(&ps_lk);
	return n;
}

int pc_pubsub_unsubscribe(void *conn, const char *name, size_t nlen,
		int pattern)
{
	struct ps_conn *pc;
	struct ps_sub *s;
	unsigned h = ps_hash(name, nlen);
	int n = 0;

	pthread_mutex_lock(&ps_lk);
	pc = conn_find(conn, 0, 0);
	if (pc) {
		for (s = pc->subs; s; s = s->next_in_conn)
			if (s->entry->pattern == pattern && s->entry->hash == h &&
			    s->entry->nlen == nlen &&
			    !memcmp(s->entry->name, name, nlen)) {
				sub_unlink(s);
				break;
			}
		n = pc->nchan + pc->npat;
		conn_drop_if_empty(pc);
	}
	pthread_mutex_unlock(&ps_lk);
	return n;
}

int pc_pubsub_pop(void *conn, int pattern, char *name, size_t cap,
		size_t *nlen, int *left)
{
	struct ps_conn *pc;
	struct ps_sub *s;

	pthread_mutex_lock(&ps_lk);
	pc = conn_find(conn, 0, 0);
	if (pc)
		for (s = pc->subs; s; s = s->next_in_conn)
			if (s->entry->pattern == pattern) {
				size_t n = s->entry->nlen < cap ? s->entry->nlen : cap;

				memcpy(name, s->entry->name, n);
				*nlen = n;
				sub_unlink(s);
				*left = pc->nchan + pc->npat;
				conn_drop_if_empty(pc);
				pthread_mutex_unlock(&ps_lk);
				return 1;
			}
	pthread_mutex_unlock(&ps_lk);
	return 0;
}

int pc_pubsub_count(void *conn)
{
	struct ps_conn *pc;
	int n = 0;

	pthread_mutex_lock(&ps_lk);
	pc = conn_find(conn, 0, 0);
	if (pc)
		n = pc->nchan + pc->npat;
	pthread_mutex_unlock(&ps_lk);
	return n;
}

void pc_pubsub_conn_closed(void *conn)
{
	struct ps_conn *pc;

	pthread_mutex_lock(&ps_lk);
	pc = conn_find(conn, 0, 0);
	if (pc) {
		while (pc->subs)
			sub_unlink(pc->subs);
		conn_drop_if_empty(pc);
	}
	pthread_mutex_unlock(&ps_lk);
}

/* ---- frames --------------------------------------------------------------
 * RESP2 arrays, the one wire every subscriber speaks today:
 *   ["message", channel, payload]
 *   ["pmessage", pattern, channel, payload] */

static size_t bulk_len(size_t n)
{
	size_t d = 1, v = n;

	while (v >= 10) { v /= 10; d++; }
	return 1 + d + 2 + n + 2;          /* $<d>\r\n<n bytes>\r\n */
}

static char *put_bulk(char *p, const char *s, size_t n)
{
	int k = 0;
	char tmp[24];
	size_t v = n;

	*p++ = '$';
	do { tmp[k++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (k)
		*p++ = tmp[--k];
	*p++ = '\r'; *p++ = '\n';
	memcpy(p, s, n); p += n;
	*p++ = '\r'; *p++ = '\n';
	return p;
}

static char *frame_message(const char *chan, size_t clen, const char *data,
		size_t dlen, size_t *flen)
{
	size_t n = 4 + bulk_len(7) + bulk_len(clen) + bulk_len(dlen);
	char *f = malloc(n), *p = f;

	if (!f)
		return NULL;
	memcpy(p, "*3\r\n", 4); p += 4;
	p = put_bulk(p, "message", 7);
	p = put_bulk(p, chan, clen);
	p = put_bulk(p, data, dlen);
	*flen = (size_t)(p - f);
	return f;
}

static char *frame_pmessage(const char *pat, size_t plen, const char *chan,
		size_t clen, const char *data, size_t dlen, size_t *flen)
{
	size_t n = 4 + bulk_len(8) + bulk_len(plen) + bulk_len(clen) + bulk_len(dlen);
	char *f = malloc(n), *p = f;

	if (!f)
		return NULL;
	memcpy(p, "*4\r\n", 4); p += 4;
	p = put_bulk(p, "pmessage", 8);
	p = put_bulk(p, pat, plen);
	p = put_bulk(p, chan, clen);
	p = put_bulk(p, data, dlen);
	*flen = (size_t)(p - f);
	return f;
}

/* ---- delivery ----------------------------------------------------------- */

static void msg_release(struct ps_msg *m)
{
	if (__atomic_sub_fetch(&m->refs, 1, __ATOMIC_ACQ_REL) > 0)
		return;
	free(m->frame);
	free(m->chan);
	free(m->data);
	free(m);
}

struct target {
	void *conn;
	struct ps_entry *pat;              /* NULL for an exact subscription */
};

/* resolve THIS worker's receivers for @m under the lock, then write.  The
 * pattern entry pointer is only used to build the frame; a pattern that
 * vanished between the resolve and the write still has its name here
 * because entries are freed only under the lock we release after copying
 * the name lengths - so copy the pattern text too. */
static void deliver_local(int worker, struct ps_msg *m)
{
	struct target stack[64], *t = stack;
	int cap = 64, n = 0, i, b;
	struct ps_entry *e;
	struct ps_sub *s;
	unsigned h = ps_hash(m->chan, m->clen);
	char *pnames = NULL; size_t *plens = NULL;

	pthread_mutex_lock(&ps_lk);
	e = entry_find(0, m->chan, m->clen, h, 0);
	if (e)
		for (s = e->subs; s; s = s->next_in_entry)
			if (s->owner->worker == worker) {
				if (n == cap) {
					struct target *nt = malloc(sizeof *nt * cap * 2);

					if (!nt) break;
					memcpy(nt, t, sizeof *nt * (size_t)n);
					if (t != stack) free(t);
					t = nt; cap *= 2;
				}
				t[n].conn = s->owner->conn; t[n].pat = NULL; n++;
			}
	if (ps_npat)
		for (b = 0; b < PS_PAT_BUCKETS; b++)
			for (e = ps_pat[b]; e; e = e->next)
				if (pc_pubsub_match(e->name, e->nlen, m->chan, m->clen))
					for (s = e->subs; s; s = s->next_in_entry)
						if (s->owner->worker == worker) {
							if (n == cap) {
								struct target *nt = malloc(sizeof *nt * cap * 2);

								if (!nt) break;
								memcpy(nt, t, sizeof *nt * (size_t)n);
								if (t != stack) free(t);
								t = nt; cap *= 2;
							}
							t[n].conn = s->owner->conn; t[n].pat = e; n++;
						}
	/* copy the pattern names out before letting go of the lock */
	if (n) {
		size_t tot = 0;

		for (i = 0; i < n; i++)
			if (t[i].pat)
				tot += t[i].pat->nlen;
		if (tot) {
			pnames = malloc(tot);
			plens = malloc(sizeof *plens * (size_t)n);
		}
		if (pnames && plens) {
			size_t off = 0;

			for (i = 0; i < n; i++) {
				plens[i] = t[i].pat ? t[i].pat->nlen : 0;
				if (t[i].pat) {
					memcpy(pnames + off, t[i].pat->name, plens[i]);
					off += plens[i];
				}
			}
		}
	}
	pthread_mutex_unlock(&ps_lk);
	{
		size_t off = 0;

		for (i = 0; i < n; i++) {
			int rc;

			if (!t[i].pat) {
				rc = pc_conn_pubsub_push(t[i].conn, NULL, 0, m->chan,
					m->clen, m->data, m->dlen, m->frame, m->flen);
			} else if (pnames && plens) {
				size_t flen;
				char *f = frame_pmessage(pnames + off, plens[i],
					m->chan, m->clen, m->data, m->dlen, &flen);

				rc = f ? pc_conn_pubsub_push(t[i].conn, pnames + off,
					plens[i], m->chan, m->clen, m->data, m->dlen,
					f, flen) : -1;
				off += plens[i];
				free(f);
			} else {
				rc = -1;
			}
			if (rc == 0) {
				__atomic_fetch_add(&ps_delivered, 1, __ATOMIC_RELAXED);
			} else {
				__atomic_fetch_add(&ps_slow_kills, 1, __ATOMIC_RELAXED);
				pc_conn_kill(t[i].conn, "slow subscriber");
			}
		}
	}
	free(pnames);
	free(plens);
	if (t != stack)
		free(t);
}

/* the number of a worker the calling thread runs on: daemon.c's accessor,
 * declared here rather than through daemon.h to keep this file's includes
 * to its own contract */
int pc_worker_id(void);

static unsigned long long ps_keyspace;

int pc_pubsub_publish(const char *chan, size_t clen, const char *data,
		size_t dlen, int relayed)
{
	unsigned char hit[PS_MAX_WORKERS];
	struct ps_entry *e;
	struct ps_sub *s;
	struct ps_msg *m;
	unsigned h;
	int count = 0, nw = 0, w, me = pc_worker_id(), b;

	if (clen == 0 || clen > PS_NAME_MAX)
		return 0;
	__atomic_fetch_add(&ps_published, 1, __ATOMIC_RELAXED);
	if (relayed == 2) {
		__atomic_fetch_add(&ps_keyspace, 1, __ATOMIC_RELAXED);
	} else if (relayed == 1) {
		__atomic_fetch_add(&ps_relay_recv, 1, __ATOMIC_RELAXED);
	} else {
		/* PS3: the fleet first, before "nobody here" can cut this
		 * short - the subscribers may all sit on other nodes.  A
		 * message that arrived from a peer is never relayed again. */
		int rs = pc_cluster_pubsub_relay(chan, clen, data, dlen);

		if (rs > 0)
			__atomic_fetch_add(&ps_relay_sent, (unsigned long long)rs,
				__ATOMIC_RELAXED);
		else if (rs < 0)
			__atomic_fetch_add(&ps_relay_dropped, 1, __ATOMIC_RELAXED);
	}
	memset(hit, 0, sizeof hit);
	h = ps_hash(chan, clen);
	pthread_mutex_lock(&ps_lk);
	e = entry_find(0, chan, clen, h, 0);
	if (e)
		for (s = e->subs; s; s = s->next_in_entry) {
			count++;
			hit[s->owner->worker] = 1;
		}
	if (ps_npat)
		for (b = 0; b < PS_PAT_BUCKETS; b++)
			for (e = ps_pat[b]; e; e = e->next)
				if (pc_pubsub_match(e->name, e->nlen, chan, clen))
					for (s = e->subs; s; s = s->next_in_entry) {
						count++;
						hit[s->owner->worker] = 1;
					}
	pthread_mutex_unlock(&ps_lk);
	if (!count)
		return 0;
	for (w = 0; w < ps_nworkers; w++)
		nw += hit[w];
	m = calloc(1, sizeof *m);
	if (!m)
		return count;
	m->chan = malloc(clen);
	m->data = malloc(dlen ? dlen : 1);
	m->frame = frame_message(chan, clen, data, dlen, &m->flen);
	if (!m->chan || !m->data || !m->frame) {
		free(m->chan); free(m->data); free(m->frame); free(m);
		return count;
	}
	memcpy(m->chan, chan, clen);
	memcpy(m->data, data, dlen);
	m->clen = clen;
	m->dlen = dlen;
	m->relayed = relayed;
	m->refs = nw;
	for (w = 0; w < ps_nworkers; w++) {
		if (!hit[w])
			continue;
		if (w == me) {
			deliver_local(w, m);
			msg_release(m);
			continue;
		}
		{
			struct ps_qnode *q = malloc(sizeof *q);
			struct ps_wq *wq = &ps_wq[w];
			uint64_t one = 1;

			if (!q) {
				__atomic_fetch_add(&ps_relay_dropped, 1, __ATOMIC_RELAXED);
				msg_release(m);
				continue;
			}
			q->m = m;
			q->next = NULL;
			pthread_mutex_lock(&wq->lk);
			if (wq->tail)
				wq->tail->next = q;
			else
				wq->head = q;
			wq->tail = q;
			wq->n++;
			pthread_mutex_unlock(&wq->lk);
			__atomic_fetch_add(&ps_queued, 1, __ATOMIC_RELAXED);
			if (wq->efd >= 0 && write(wq->efd, &one, sizeof one) < 0)
				{ /* best effort: the next event drains it */ }
		}
	}
	return count;
}

void pc_pubsub_drain(int worker)
{
	struct ps_wq *wq;
	struct ps_qnode *q, *next;

	if (worker < 0 || worker >= PS_MAX_WORKERS)
		return;
	wq = &ps_wq[worker];
	pthread_mutex_lock(&wq->lk);
	q = wq->head;
	wq->head = wq->tail = NULL;
	wq->n = 0;
	pthread_mutex_unlock(&wq->lk);
	for (; q; q = next) {
		next = q->next;
		deliver_local(worker, q->m);
		msg_release(q->m);
		free(q);
	}
}

/* ---- introspection ------------------------------------------------------ */

int pc_pubsub_channels(const char *glob, size_t glen, pc_pubsub_name_f cb,
		void *arg)
{
	int b, n = 0;
	struct ps_entry *e;

	pthread_mutex_lock(&ps_lk);
	for (b = 0; b < PS_CH_BUCKETS; b++)
		for (e = ps_ch[b]; e; e = e->next) {
			if (glob && !pc_pubsub_match(glob, glen, e->name, e->nlen))
				continue;
			if (cb(e->name, e->nlen, e->nsubs, arg) != 0)
				goto out;
			n++;
		}
out:
	pthread_mutex_unlock(&ps_lk);
	return n;
}

int pc_pubsub_numsub(const char *name, size_t nlen)
{
	struct ps_entry *e;
	int n = 0;

	pthread_mutex_lock(&ps_lk);
	e = entry_find(0, name, nlen, ps_hash(name, nlen), 0);
	if (e)
		n = e->nsubs;
	pthread_mutex_unlock(&ps_lk);
	return n;
}

int pc_pubsub_numpat(void)
{
	int n;

	pthread_mutex_lock(&ps_lk);
	n = ps_npat;
	pthread_mutex_unlock(&ps_lk);
	return n;
}

void pc_pubsub_relay_account(unsigned long long sent, unsigned long long lost,
		unsigned long long dropped)
{
	if (sent)
		__atomic_fetch_add(&ps_relay_sent, sent, __ATOMIC_RELAXED);
	if (lost)
		__atomic_fetch_add(&ps_relay_lost, lost, __ATOMIC_RELAXED);
	if (dropped)
		__atomic_fetch_add(&ps_relay_dropped, dropped, __ATOMIC_RELAXED);
}

void pc_pubsub_stats(struct pc_pubsub_stats *out)
{
	pthread_mutex_lock(&ps_lk);
	out->channels = ps_nch;
	out->patterns = ps_npat;
	out->subscribers = ps_nsubs;
	pthread_mutex_unlock(&ps_lk);
	out->published = __atomic_load_n(&ps_published, __ATOMIC_RELAXED);
	out->delivered = __atomic_load_n(&ps_delivered, __ATOMIC_RELAXED);
	out->slow_kills = __atomic_load_n(&ps_slow_kills, __ATOMIC_RELAXED);
	out->queued = __atomic_load_n(&ps_queued, __ATOMIC_RELAXED);
	out->keyspace = __atomic_load_n(&ps_keyspace, __ATOMIC_RELAXED);
	out->relay_sent = __atomic_load_n(&ps_relay_sent, __ATOMIC_RELAXED);
	out->relay_recv = __atomic_load_n(&ps_relay_recv, __ATOMIC_RELAXED);
	out->relay_lost = __atomic_load_n(&ps_relay_lost, __ATOMIC_RELAXED);
	out->relay_dropped = __atomic_load_n(&ps_relay_dropped, __ATOMIC_RELAXED);
}
