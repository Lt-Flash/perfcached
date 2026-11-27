/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* cladmit.c - admission: the master's joiner queue, the link check that moves a joiner from
 * CONNECTING to ONLINE, and the member-list repair.  Peer thread for every entry point. */
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "compat/dprint.h"
#include "cluster.h"
#include "clcodec.h"                 /* the LE wire codec */
#include "clmemb.h"                  /* JOIN_WAIT, ASSIGN layout */
#include "clmesh.h"                  /* discovery = unicast */
#include "clmsg.h"
#include "clpeers.h"
#include "clane.h"                  /* the lane's port */
#include "clconn.h"                 /* the TCP paths */
#include "clsend.h"
#include "clterm.h"
#include "clwire.h"                  /* HDR_LEN, CLWIRE_TAG */
#include "clstate.h"
#include "cladmit.h"

#define ELECTING_LATE_MS 3000  /* a member answers ELECTING once its master missed 3 beats: a
                                * joiner's window (3 s) is shorter than MASTER_DEAD_MS */
#define ELECT_SAY_MS     2000  /* ELECTING to one joiner at most this often - it holds it 3 s */
#define ADMIT_IDLE_MS    6000          /* a queued joiner silent this long has gone */
#define ADMIT_BATCH_ABOVE 16           /* 1 in flight up to this many members, 4 above */
enum { AQ_QUEUED = 1, AQ_ADMITTED, AQ_STUCK };   /* STUCK: held CONNECTING, out of the batch */
static struct admitq {
	pc_addr a;
	int state;
	long long first_ms, last_ms, admit_ms;
	size_t flen;
	unsigned char frame[128];
} aq[PC_CL_MAXPEER];
static int aq_n;
static int admitting;                  /* a queued JOIN_REQ is being replayed */
static int admit_said;                 /* the gate state said last: 1 held, 2 open */
static long long master_since_ms;      /* this mastership began - the admission gate */

#define LINK_TICK_MS      500
#define CONNECT_MS        10000
#define LINK_CS_OK        0x01         /* the cluster socket answered since the check began */
#define LINK_RELAY_NEED   0x02         /* the peer advertises a relay port */
#define LINK_RELAY_OK     0x04         /* and answered the relay probe since the check began */
#define LINK_LANE_MISS    0x08         /* the frame lane is on; its port has not taken a connect */
#define LINK_RPT_MISS     0x10         /* replication rides clrpt; its relay port has not either */
#define LINK_BULK_MISS    0x20         /* this joiner pulls a bootstrap; that member's bulk listener has not */
#define LINK_SEEN         0x80         /* the master's mark: a report said it */
#define LINK_F_CONNECTING 0x01
#define LCHK_MAX          16
_Static_assert(sizeof ((struct peer *)0)->link_tcp_ms == CLCONN_PATHS * sizeof (long long),
	"a time for each of clconn's paths");
static struct lchk {
	pc_addr a;
	int node, stuck, npr, again;       /* again: begun on its own CONNECTING report */
	int seen;                          /* its own report said CONNECTING: it took the ASSIGN */
	long long start_ms;
	struct { int node; unsigned char jb, mb; } pr[PC_CL_MAXPEER + 1];   /* its view, theirs */
} lc[LCHK_MAX];
static int lc_n;
static char stuck_why[200 + 64];     /* "node %d (%s): " and a joiner's why */
static pthread_mutex_t stuck_mx = PTHREAD_MUTEX_INITIALIZER;   /* the alerts thread reads it */
static long long connect_start_ms;     /* CONNECTING since; its link checks count from here */
static uint64_t assign_tok;            /* its ASSIGN's token, for the later parts */
static long long online_ms;            /* moved ONLINE at - no election of its own for a while */

#define MEMBERS_MISS      3            /* beacons running whose digest is not this view - ask */
#define MEMBERS_ASK_MS    5000
#define MEMBERS_REVIVE_MS 30000
static int digest_miss;
static long long members_asked_ms;

static const unsigned char noident[16];
#define PC_START_UNKNOWN 0             /* cluster.c's: how the member started, not said */

static int is_master(void)
{
	return cl_role() == PC_ROLE_MASTER;
}

/* a JOIN_WAIT that says why - QUEUED from the master, ELECTING from a member of a fleet with
 * no master.  Either holds the joiner off founding; an older joiner holds too */
static void send_join_why(const pc_addr *to, uint64_t tok, int reason, int count)
{
	unsigned char msg[CLMEMB_WAIT_LEN];
	struct clmemb_wait w;

	w.reason = reason;
	w.term = pc_term_current();
	w.node = cl_node_id();
	w.count = count;
	clsend_seal(to, msg, clmemb_wait_write(msg, sizeof msg, M_JOIN_WAIT, tok, &w));
}

static int electing_due(const pc_addr *to, long long now)
{
	static struct { pc_addr a; long long ms; } said[64];
	int i, old = 0;

	for (i = 0; i < 64; i++) {
		if (pc_addr_eq(&said[i].a, to)) {
			if (now - said[i].ms < ELECT_SAY_MS)
				return 0;
			said[i].ms = now;
			return 1;
		}
		if (said[i].ms < said[old].ms)
			old = i;
	}
	said[old].a = *to;
	said[old].ms = now;
	return 1;
}

/* a fleet without a master is still a fleet - the joiner is told, not left to found beside it
 * while the members elect */
void cladmit_electing(const pc_addr *from, uint64_t tok, long long master_seen_ms)
{
	long long now = cl_now_ms();

	if ((!master_seen_ms || now - master_seen_ms >= ELECTING_LATE_MS) &&
	        electing_due(from, now))
		send_join_why(from, tok, CLMEMB_WAIT_ELECTING, clpeers_count_live(cl_peers(), now) + 1);
}

static struct admitq *aq_find(const pc_addr *a)
{
	int i;

	for (i = 0; i < aq_n; i++)
		if (pc_addr_eq(&aq[i].a, a))
			return &aq[i];
	return NULL;
}

/* its place among the queued, 1 first */
static int aq_place(const struct admitq *q)
{
	int i, n = 1;

	for (i = 0; i < aq_n; i++)
		n += aq[i].state == AQ_QUEUED && aq[i].first_ms < q->first_ms;
	return n;
}

/* ---- the link check: LANE_PROBE every 500 ms until answered; each side reports what it proved,
 * the master moves the node ONLINE once every pair holds both ways or holds it past CONNECT_MS */

static struct lchk *lchk_find(int node)
{
	int i;

	for (i = 0; i < lc_n; i++)
		if (lc[i].node == node)
			return &lc[i];
	return NULL;
}

static struct lchk *lchk_new(const pc_addr *a, int node, long long now)
{
	struct lchk *l = lchk_find(node);

	if (l || lc_n == LCHK_MAX)
		return l;
	l = &lc[lc_n++];
	memset(l, 0, sizeof *l);
	l->a = *a;
	l->node = node;
	l->start_ms = now;
	return l;
}

/* the pair with @node: &jb (@its view) or &mb */
static unsigned char *lchk_pair(struct lchk *l, int node, int its)
{
	int i;

	for (i = 0; i < l->npr && l->pr[i].node != node; i++)
		;
	if (i == l->npr) {
		if (l->npr == PC_CL_MAXPEER + 1)
			return NULL;
		l->npr++;
		l->pr[i].node = node;
		l->pr[i].jb = l->pr[i].mb = 0;
	}
	return its ? &l->pr[i].jb : &l->pr[i].mb;
}

/* 1 = this side uses TCP path @k to @p, at @to: the lane when it is on, clrpt when replication
 * rides it, the bulk listener when this CONNECTING node will pull a bootstrap */
static int tcp_need(const struct peer *p, int k, int me, pc_addr *to)
{
	int port = pc_addr_port(&p->addr);

	if (k == CLCONN_LANE && clane_off())
		port += clane_off();
	else if (k == CLCONN_RPT && cl_max_datagram() > 0)
		port = __atomic_load_n(&p->rp.port, __ATOMIC_ACQUIRE);
	else if (k != CLCONN_BULK || !me || !cl_will_bootstrap())
		return 0;
	if (port <= 0 || port > 65535)
		return 0;
	*to = p->addr;
	pc_addr_set_port(to, (uint16_t)port);
	return 1;
}

static int tcp_proved(const struct peer *p, int k)
{
	return p->link_tcp_ms[k] && p->link_tcp_ms[k] >= p->link_since_ms;
}

static unsigned link_bits(const struct peer *p, int me)
{
	static const unsigned miss[CLCONN_PATHS] = { LINK_LANE_MISS, LINK_RPT_MISS, LINK_BULK_MISS };
	uint16_t port = __atomic_load_n(&p->ps.port, __ATOMIC_ACQUIRE);
	long long ok = __atomic_load_n(&p->ps.ok_ms, __ATOMIC_ACQUIRE);
	unsigned b = 0;
	pc_addr to;
	int k;

	for (k = 0; k < CLCONN_PATHS; k++)
		if (tcp_need(p, k, me, &to) && !tcp_proved(p, k))
			b |= miss[k];

	if (p->link_ok_ms && p->link_ok_ms >= p->link_since_ms)
		b |= LINK_CS_OK;
	if (port) {
		b |= LINK_RELAY_NEED;
		if (ok && ok >= p->link_since_ms)
			b |= LINK_RELAY_OK;
	}
	return b;
}

/* 1 = proved: both views reported, the cluster socket both ways, a relay port, the lane, clrpt
 * and the bulk listener where each is in use.  Else what is missing goes on @why */
static int link_pair_ok(struct lchk *l, int x, char *why, size_t cap)
{
	unsigned char *jb = lchk_pair(l, x, 1), *mb = lchk_pair(l, x, 0);
	const char *miss = NULL;
	size_t w = strlen(why);

	if (!jb || !mb)
		return 0;
	if (!(*jb & LINK_SEEN))
		miss = "no report from it on that member yet";
	else if (!(*mb & LINK_SEEN))
		miss = "no report from that member on it yet";
	else if (!(*jb & LINK_CS_OK) || !(*mb & LINK_CS_OK))
		miss = "the cluster socket did not answer";
	else if (((*jb & LINK_RELAY_NEED) && !(*jb & LINK_RELAY_OK)) ||
	         ((*mb & LINK_RELAY_NEED) && !(*mb & LINK_RELAY_OK)))
		miss = "the relay port did not answer";
	else if ((*jb | *mb) & LINK_LANE_MISS)
		miss = "the frame lane did not answer";
	else if ((*jb | *mb) & LINK_RPT_MISS)
		miss = "clrpt did not answer";
	else if (*jb & LINK_BULK_MISS)
		miss = "the bulk listener did not answer";
	if (!miss)
		return 1;
	if (w + 48 < cap)
		snprintf(why + w, cap - w, "%snode %d: %s", w ? "; " : "", x, miss);
	return 0;
}

static void link_eval(long long now)
{
	struct clpeers *pt = cl_peers();
	int i, k, stuck = 0;

	for (i = 0; i < lc_n; ) {
		struct lchk *l = &lc[i];
		struct peer *jp = clpeers_by_id_live(pt, l->node, now);
		struct admitq *q = aq_find(&l->a);
		char why[sizeof stuck_why - 64] = "";
		int bad = 0, pairs = 1;

		if (!jp || !clpeers_live(jp, now) || !pc_addr_eq(&jp->addr, &l->a)) {
			if (q)
				*q = aq[--aq_n];       /* gone while CONNECTING */
			*l = lc[--lc_n];
			continue;
		}
		/* out of CONNECTING with no ONLINE from here: a report older than its ONLINE, or a
		 * restart's copy (RECOVERING, then READY) - it is no longer this check's */
		if ((l->again || l->seen) && jp->nstate != PC_NST_CONNECTING &&
		        jp->nstate != PC_NST_STARTING) {
			if (!l->again)
				LM_NOTICE("cluster: node %d (%s) left CONNECTING without an ONLINE, now %s - "
					"its link check ends%s\n", l->node, pc_addr_str(&l->a),
					pc_node_state_name(jp->nstate), l->stuck ? ", and its hold" : "");
			if (q && !l->again)
				*q = aq[--aq_n];
			*l = lc[--lc_n];
			continue;
		}
		bad += !link_pair_ok(l, cl_node_id(), why, sizeof why);
		for (k = 0; k < pt->n_peers; k++) {
			const struct peer *x = &pt->peers[k];

			if (!x->node || x->node == l->node || !clpeers_live(x, now) ||
			        x->nstate == PC_NST_STALLED)
				continue;              /* a stalled member's cluster thread cannot report */
			pairs++;
			bad += !link_pair_ok(l, x->node, why, sizeof why);
		}
		if (!bad) {
			unsigned char m[M_ONLINE_LEN];
			struct pc_wcur c;

			pc_wcur_init(&c, m, sizeof m);
			pc_w8(&c, M_ONLINE);
			pc_w32(&c, pc_term_current());
			clsend_seal(&l->a, m, pc_wcur_len(&c, m));
			LM_NOTICE("cluster: node %d (%s) is ONLINE - all %d pair(s) with it held both "
				"ways, %lld ms%s\n", l->node, pc_addr_str(&l->a), pairs, now - l->start_ms,
				l->stuck ? ", after being held" : "");
			if (q)
				*q = aq[--aq_n];
			*l = lc[--lc_n];
			continue;
		}
		if (!l->stuck && now - l->start_ms >= CONNECT_MS) {
			l->stuck = 1;
			if (q && q->state == AQ_ADMITTED)
				q->state = AQ_STUCK;
			LM_WARN("cluster: admission stuck - node %d (%s) has been CONNECTING %lld s, "
				"%d of %d pair(s) not proved both ways: %s.  Held CONNECTING (re-probing); "
				"the queue moves on\n", l->node, pc_addr_str(&l->a),
				(now - l->start_ms) / 1000, bad, pairs, why);
		}
		if (l->stuck && !stuck++) {
			pthread_mutex_lock(&stuck_mx);
			snprintf(stuck_why, sizeof stuck_why, "node %d (%s): %s", l->node,
				pc_addr_str(&l->a), why);
			pthread_mutex_unlock(&stuck_mx);
		}
		i++;
	}
	cl_st_admission_stuck((unsigned long long)stuck);
	if (!stuck) {
		pthread_mutex_lock(&stuck_mx);
		stuck_why[0] = 0;
		pthread_mutex_unlock(&stuck_mx);
	}
}

/* @count records of [node2][bits1] from node @rnode (@rconn: its own check, every peer) */
static void link_apply(int rnode, int rconn, const unsigned char *rec, size_t n, int count,
		long long now)
{
	struct pc_rcur c;
	struct lchk *l;
	int i;

	if (rconn && !lchk_find(rnode)) {
		struct peer *p = clpeers_by_id_live(cl_peers(), rnode, now);

		/* CONNECTING, and not this master's to have admitted - checked again */
		if (p && clpeers_live(p, now) && (l = lchk_new(&p->addr, rnode, now)) != NULL)
			l->again = 1;
	}
	if (rconn && (l = lchk_find(rnode)) != NULL)
		l->seen = 1;
	pc_rcur_init(&c, rec, n);
	for (i = 0; i < count && pc_rfits(&c, 3); i++) {
		int x = pc_ru16(&c);
		unsigned char b = (unsigned char)(pc_ru8(&c) | LINK_SEEN), *s;

		if (rconn && (l = lchk_find(rnode)) && (s = lchk_pair(l, x, 1)))
			*s = b;
		if ((l = lchk_find(x)) && (s = lchk_pair(l, rnode, 0)))
			*s = b;
	}
	link_eval(now);
	cladmit_tick(now);                     /* an ONLINE frees a place */
}

/* a side's first probe to a pair waits 0-50 ms a member, at most 3 s (corosync's send_join);
 * the pair's hash picks the delay, so the members spread without keeping a time each */
#define PROBE_SPREAD_MS     50
#define PROBE_SPREAD_MAX_MS 3000
static long long probe_delay(int peer, int members)
{
	uint32_t h = (uint32_t)cl_node_id() * 2654435761u ^ (uint32_t)peer * 2246822519u;
	int span = members * PROBE_SPREAD_MS;

	if (span > PROBE_SPREAD_MAX_MS)
		span = PROBE_SPREAD_MAX_MS;
	h ^= h >> 15;
	return span > 0 ? (long long)(h % (uint32_t)span) : 0;
}

/* one connect in flight a path; one that completes proves it for this check */
static void tcp_probe(struct peer *p, int me, long long now)
{
	pc_addr to, self = cl_self_addr();
	int k;

	for (k = 0; k < CLCONN_PATHS; k++)
		if (tcp_need(p, k, me, &to) && !tcp_proved(p, k) &&
		        clconn_probe(p->node, k, &self, &to, now) == 1)
			p->link_tcp_ms[k] = now;
}

void cladmit_link_tick(long long now)
{
	static long long next;
	unsigned char msg[4 + (PC_CL_MAXPEER + 1) * 3], m = M_LANE_PROBE;
	struct clpeers *pt = cl_peers();
	struct pc_wcur c;
	int i, n = 0, me = pc_node_state() == PC_NST_CONNECTING, members;

	if (!is_master() && lc_n) {
		lc_n = 0;                          /* no longer the master's to check */
		cl_st_admission_stuck(0);
		pthread_mutex_lock(&stuck_mx);
		stuck_why[0] = 0;
		pthread_mutex_unlock(&stuck_mx);
	}
	if (now < next || cl_role() == PC_ROLE_JOINING || !cl_node_id())
		return;
	next = now + LINK_TICK_MS;
	members = clpeers_count_live(pt, now) + 1;
	pc_wcur_init(&c, msg, sizeof msg);
	pc_w8(&c, M_LINK_REPORT);
	pc_w8(&c, me ? LINK_F_CONNECTING : 0);
	pc_w16(&c, 0);
	for (i = 0; i < pt->n_peers; i++) {
		struct peer *p = &pt->peers[i];

		if (!p->node || !clpeers_live(p, now) || (!me && p->nstate != PC_NST_CONNECTING)) {
			p->link_since_ms = 0;
			continue;
		}
		if (!p->link_since_ms)
			p->link_since_ms = me ? connect_start_ms : now;
		if (now >= p->link_since_ms + probe_delay(p->node, members)) {
			if (!p->link_ok_ms || p->link_ok_ms < p->link_since_ms) {
				p->link_probe_ms = now;    /* before the send: a fast answer counts */
				(void)clsend_seal_udp(&p->addr, &m, 1);
			}
			tcp_probe(p, me, now);
		}
		pc_w16(&c, (uint16_t)p->node);
		pc_w8(&c, link_bits(p, me));
		n++;
	}
	clconn_reap(now);                      /* a check that ended, a peer gone */
	if (is_master() && lc_n)
		link_eval(now);                    /* a CONNECTING one gone, or past CONNECT_MS */
	if (!n || !c.ok)
		return;
	pc_p16(msg + 2, (uint16_t)n);
	if (is_master()) {
		link_apply(cl_node_id(), me, msg + 4, pc_wcur_len(&c, msg) - 4, n, now);
	} else {
		pc_addr ma = cl_master_addr();

		clsend_seal(&ma, msg, pc_wcur_len(&c, msg));
	}
}

void cladmit_link_report(const unsigned char *pt, size_t n, const pc_addr *from)
{
	struct peer *r;
	long long now = cl_now_ms();

	if (!is_master() || n < 4 ||
	        !(r = clpeers_by_addr(cl_peers(), from)) || !r->node || !clpeers_live(r, now))
		return;
	link_apply(r->node, pt[1] & LINK_F_CONNECTING, pt + 4, n - 4, pc_g16(pt + 2), now);
}

void cladmit_probe_answered(struct peer *p, long long now)
{
	if (p && p->link_probe_ms && p->link_probe_ms >= p->link_since_ms)
		p->link_ok_ms = now;               /* the cluster socket, both ways */
}

void cladmit_connect_begin(long long now, uint64_t tok)
{
	connect_start_ms = now;
	assign_tok = tok;
}

static void publish_members(const unsigned char *pt, size_t n, int count, long long now);

/* a later part of this node's ASSIGN - from its master, under its join's token, while it
 * is CONNECTING.  Holding every part is the link check's: it reports on each member it knows */
void cladmit_assign_part(const unsigned char *pt, size_t n, const struct clmemb_assign *as,
		const pc_addr *from)
{
	struct clmemb_trailer t;
	pc_addr ma = cl_master_addr();

	if (cl_role() != PC_ROLE_MEMBER || pc_node_state() != PC_NST_CONNECTING || !assign_tok ||
	        as->tok != assign_tok || !pc_addr_eq(from, &ma) ||
	        !clmemb_trailer_parse(pt, n, as->count, &t) || t.parts < 2 || t.part >= t.parts)
		return;                        /* any part: the first to arrive may not be the first */
	publish_members(pt, n, as->count, cl_now_ms());
	LM_NOTICE("cluster: ASSIGN part %d of %d - %d member(s)\n", t.part + 1, t.parts, as->count);
}

/* the master says every pair held both ways */
void cladmit_online(const unsigned char *pt, size_t n, const pc_addr *from)
{
	long long now = cl_now_ms();
	pc_addr ma = cl_master_addr();

	(void)pt;
	if (n < M_ONLINE_LEN || cl_role() != PC_ROLE_MEMBER || !pc_addr_eq(from, &ma) ||
	        pc_node_state() != PC_NST_CONNECTING)
		return;
	online_ms = now;
	assign_tok = 0;
	LM_NOTICE("cluster: ONLINE - every link to every member answered both ways (%lld ms "
		"CONNECTING)\n", now - connect_start_ms);
	joined_state(clpeers_count_live(cl_peers(), now) + 1);
	send_alive();
}

long long cladmit_online_ms(void)
{
	return online_ms;
}

/* for the alert (alerts.c): the first node held CONNECTING and what it lacks, "" when none */
void pc_cluster_admission_stuck_text(char *out, size_t cap)
{
	pthread_mutex_lock(&stuck_mx);
	snprintf(out, cap, "%s", stuck_why);
	pthread_mutex_unlock(&stuck_mx);
}

/* a new master admits nobody until every live member's ALIVE is newer than its mastership,
 * nor while a CONNECTING member an earlier master admitted is being checked again */
static int admit_gate_open(long long now, int *heard, int *live)
{
	struct clpeers *pt = cl_peers();
	int i, again = 0;

	*heard = *live = 0;
	for (i = 0; i < pt->n_peers; i++) {
		const struct peer *p = &pt->peers[i];
		const struct admitq *q = aq_find(&p->addr);

		if (!clpeers_live(p, now) || (q && q->state != AQ_QUEUED))
			continue;              /* one in flight or held is the batch's to count */
		again += p->nstate == PC_NST_CONNECTING;
		(*live)++;
		*heard += p->alive_ms >= master_since_ms;
	}
	return *heard == *live && !again;
}

void cladmit_master_begin(long long now)
{
	master_since_ms = now;
	aq_n = 0;
	admit_said = 0;
	lc_n = 0;                              /* a CONNECTING member is checked again (link_apply) */
}

void cladmit_tick(long long now)
{
	struct clpeers *pt = cl_peers();
	int i, inflight = 0, batch, heard, live;

	if (!is_master()) {
		aq_n = 0;
		return;
	}
	for (i = 0; i < aq_n; ) {
		if (aq[i].state == AQ_QUEUED && now - aq[i].last_ms >= ADMIT_IDLE_MS) {
			aq[i] = aq[--aq_n];
			continue;
		}
		inflight += aq[i].state == AQ_ADMITTED;
		i++;
	}
	if (!aq_n)
		return;
	if (!admit_gate_open(now, &heard, &live)) {
		if (admit_said != 1)
			LM_NOTICE("cluster: admission held - %d of %d live member(s) heard since "
				"this mastership began; %d joiner(s) queued\n", heard, live, aq_n);
		admit_said = 1;
		return;
	}
	if (admit_said != 2)
		LM_NOTICE("cluster: admission open - every live member heard since this "
			"mastership began\n");
	admit_said = 2;
	batch = live + 1 > ADMIT_BATCH_ABOVE ? 4 : 1;   /* members: neither in flight nor held */
	while (inflight < batch && lc_n < LCHK_MAX) {
		struct admitq *h = NULL;
		unsigned char f[sizeof aq[0].frame];
		struct peer *p;
		pc_addr a;
		size_t fl;

		for (i = 0; i < aq_n; i++)
			if (aq[i].state == AQ_QUEUED && (!h || aq[i].first_ms < h->first_ms))
				h = &aq[i];
		if (!h)
			break;
		h->state = AQ_ADMITTED;
		h->admit_ms = now;
		inflight++;
		a = h->a;
		fl = h->flen;
		memcpy(f, h->frame, fl);
		LM_NOTICE("cluster: admitting %s from the queue (%d in flight, %d queued behind)\n",
			pc_addr_str(&a), inflight, aq_n - inflight);
		admitting = 1;
		handle_join_req(f, fl, &a);
		admitting = 0;
		/* refused or held on the way in: it is not in flight */
		if (!(p = clpeers_by_addr(pt, &a)) || !clpeers_live(p, now)) {
			if ((h = aq_find(&a)) != NULL)
				*h = aq[--aq_n];
			inflight--;
			continue;
		}
		p->nstate = PC_NST_CONNECTING;     /* its ASSIGN said connect first */
		lchk_new(&a, p->node, now);
	}
}

int cladmit_hold(const unsigned char *pt, size_t n, const pc_addr *from, uint64_t tok)
{
	struct admitq *q;
	long long now = cl_now_ms();

	if (admitting || n > sizeof aq[0].frame)
		return 0;
	q = aq_find(from);
	if (q && q->state == AQ_ADMITTED)
		return 0;
	if (!q) {
		if (aq_n == PC_CL_MAXPEER)
			return 1;                  /* full: it asks again */
		q = &aq[aq_n++];
		memset(q, 0, sizeof *q);
		q->a = *from;
		q->state = AQ_QUEUED;
		q->first_ms = now;
	}
	q->last_ms = now;
	memcpy(q->frame, pt, n);
	q->flen = n;
	cladmit_tick(now);
	if ((q = aq_find(from)) != NULL && q->state == AQ_QUEUED)
		send_join_why(from, tok, CLMEMB_WAIT_QUEUED, aq_place(q));
	return 1;
}

/* a member's ALIVE may open the gate; an admission ends at its ONLINE (link_eval) */
void cladmit_alive(long long now)
{
	if (aq_n)
		cladmit_tick(now);
}

/* ---- the member-list repair ------------------------------------------------------------- */

/* an ASSIGN (echoing @tok) or M_MEMBERS: self first, then every live peer except the addressee;
 * a list past one datagram goes in parts, each numbered of the whole */
void send_member_list(unsigned char type, const pc_addr *to, uint64_t tok,
		int your_id)
{
	static struct clmemb_member all[PC_CL_MAXPEER + 1];
	unsigned char msg[CLMEMB_ASSIGN_HDR + (PC_CL_MAXPEER + 1) * CLMEMB_ASSIGN_REC + CLMEMB_ASSIGN_TRAILER];
	struct clpeers *pt = cl_peers();
	pc_addr self = cl_self_addr();
	long long now = cl_now_ms();
	int i, k, n = 0, per, parts;

	memset(&all[0], 0, sizeof all[0]);
	all[0].node = cl_node_id();
	all[0].addr = self.in4.sin_addr;
	all[0].port = self.in4.sin_port;
	all[0].free_mb = cl_self_free_mb();
	all[0].total_mb = cl_self_total_mb();
	all[0].live_kb = cl_self_live_kb();
	n = 1;
	for (i = 0; i < pt->n_peers && n <= PC_CL_MAXPEER; i++) {
		struct clmemb_member *m = &all[n];

		if (!clpeers_live(&pt->peers[i], now) ||
		        pc_addr_eq(&pt->peers[i].addr, to))
			continue;
		memset(m, 0, sizeof *m);
		m->node = pt->peers[i].node;
		m->addr = pt->peers[i].addr.in4.sin_addr;
		m->port = pt->peers[i].addr.in4.sin_port;
		m->free_mb = pt->peers[i].free_mb;
		m->total_mb = pt->peers[i].total_mb;
		m->live_kb = pt->peers[i].live_kb;
		n++;
	}
	{
		int room = (cl_max_datagram() > 0 ? cl_max_datagram() : 1400) - HDR_LEN - CLWIRE_TAG -
			CLMEMB_ASSIGN_HDR - CLMEMB_ASSIGN_TRAILER;

		per = room >= CLMEMB_ASSIGN_REC ? room / CLMEMB_ASSIGN_REC : 1;
		if (per < (n + 254) / 255)
			per = (n + 254) / 255;     /* the trailer counts parts in a byte */
	}
	parts = (n + per - 1) / per;
	for (k = 0; k < parts; k++) {
		struct clmemb_assign h;
		int c = n - k * per < per ? n - k * per : per;
		size_t off;

		h.tok = tok;
		h.your_id = your_id;
		h.master_id = cl_node_id();
		h.count = c;
		off = clmemb_assign_hdr_write(msg, sizeof msg, type, &h);
		for (i = 0; i < c; i++)
			off += clmemb_member_write(msg + off, sizeof msg - off, &all[k * per + i]);
		if (type == M_ASSIGN) {
			struct clmemb_trailer t = { CLMEMB_ASSIGN_CONNECT, k, parts };   /* connect first */

			off += clmemb_trailer_write(msg + off, sizeof msg - off, &t);
		}
		clsend_seal(to, msg, off);
	}
	if (parts > 1 && type == M_ASSIGN)
		LM_NOTICE("cluster: the member list for node %d (%s) goes in %d parts - %d member(s), "
			"%d a datagram\n", your_id, pc_addr_str(to), parts, n, per);
}


void cladmit_members_check(uint64_t theirs, const pc_addr *master)
{
	unsigned char msg[M_MEMBERS_REQ_LEN];
	struct pc_wcur c;
	uint64_t ours = member_digest();
	long long now = cl_now_ms();

	if (theirs == ours) {
		digest_miss = 0;
		return;
	}
	if (++digest_miss < MEMBERS_MISS || now - members_asked_ms < MEMBERS_ASK_MS)
		return;
	members_asked_ms = now;
	cl_st_members_asked();
	pc_wcur_init(&c, msg, sizeof msg);
	pc_w8(&c, M_MEMBERS_REQ);
	pc_w64(&c, ours);
	clsend_seal(master, msg, pc_wcur_len(&c, msg));
}

void cladmit_members_req(const unsigned char *pt, size_t n, const pc_addr *from)
{
	static long long win_ms;
	static int win_n;
	struct peer *p;
	long long now = cl_now_ms();

	(void)pt;
	if (!is_master() || n < M_MEMBERS_REQ_LEN ||
	        !(p = clpeers_by_addr(cl_peers(), from)) || !clpeers_live(p, now))
		return;
	if (now - win_ms >= 1000) {
		win_ms = now;
		win_n = 0;
	}
	if (++win_n > 20)
		return;                        /* it asks again in 5 s */
	send_member_list(M_MEMBERS, from, 0, p->node);
	cl_st_members_sent();
}

/* a member this node did not know is known now; one it knows is left to its own beats, and one
 * dropped for silence in the last 30 s is not revived on the master's word */
void cladmit_members(const unsigned char *pt, size_t n, const pc_addr *from)
{
	struct clmemb_assign as;
	pc_addr ma = cl_master_addr();

	if (cl_role() != PC_ROLE_MEMBER || !pc_addr_eq(from, &ma) ||
	        !clmemb_assign_parse(pt, n, &as))
		return;
	publish_members(pt, n, as.count, cl_now_ms());
	digest_miss = 0;
}

static void publish_members(const unsigned char *pt, size_t n, int count, long long now)
{
	struct clmemb_member mr;
	pc_addr self = cl_self_addr();
	int i;

	for (i = 0; i < count && clmemb_member_at(pt, n, i, &mr); i++) {
		pc_addr pa;
		struct peer *p;

		memset(&pa, 0, sizeof pa);
		pc_addr_from_v4(&pa, mr.addr.s_addr, ntohs(mr.port));
		if (pc_addr_eq(&pa, &self))
			continue;
		if ((p = clpeers_by_addr(cl_peers(), &pa)) != NULL &&
		        (clpeers_live(p, now) || (p->gone_ms && now - p->gone_ms < MEMBERS_REVIVE_MS)))
			continue;
		if (cl_unicast_on())
			clmesh_heard(&pa, now, 0);
		p = peer_upsert(&pa);
		if (p)
			peer_publish(p, mr.node, mr.free_mb, mr.total_mb, mr.live_kb,
				noident, 0, 0, PC_START_UNKNOWN);
	}
}
