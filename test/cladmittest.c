/* cladmittest.c - admission (cladmit.c): the master's queue and gate, the link check, ELECTING,
 * the member list in parts and its repair, the TCP paths; stubbed clstate.h, a real peer table. */
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "compat/dprint.h"
#include "cluster.h"
#include "clcodec.h"
#include "clmemb.h"
#include "clmsg.h"
#include "clpeers.h"
#include "clane.h"
#include "clconn.h"
#include "clstate.h"
#include "cladmit.h"

static int fails;
#define CHK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
	else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

/* ---- the stubs ------------------------------------------------------------------------------ */
static struct clpeers PT;
static long long now = 1000000;
static int role = PC_ROLE_MASTER, nst = PC_NST_READY, maxdg;
static int next_id = 9, joins, refuse, published, joined, alives;
static unsigned long long asked, answered, stuck_gauge;
static uint64_t digest = 0x1111;
static char logbuf[1 << 16];

static pc_addr addr(int last)
{
	pc_addr a;

	memset(&a, 0, sizeof a);
	pc_addr_from_v4(&a, htonl(0x0a000000u | (unsigned)last), 6480);
	return a;
}

static pc_addr master_a;

struct clpeers *cl_peers(void) { return &PT; }
long long cl_now_ms(void) { return now; }
int cl_role(void) { return role; }
int cl_node_id(void) { return 1; }
pc_addr cl_self_addr(void) { return addr(5); }
pc_addr cl_master_addr(void) { return master_a; }
int cl_unicast_on(void) { return 0; }
void cl_st_members_asked(void) { asked++; }
void cl_st_members_sent(void) { answered++; }
void cl_st_admission_stuck(unsigned long long n) { stuck_gauge = n; }
unsigned int cl_self_free_mb(void) { return 100; }
unsigned int cl_self_total_mb(void) { return 200; }
unsigned int cl_self_live_kb(void) { return 300; }
int cl_max_datagram(void) { return maxdg; }
int pc_node_state(void) { return nst; }
const char *pc_node_state_name(int st) { return st == PC_NST_RECOVERING ? "recovering" : st == PC_NST_READY ? "ready" : "?"; }
uint32_t pc_term_current(void) { return 7; }
int clmesh_heard(const pc_addr *a, long long ms, int trial) { (void)a; (void)ms; (void)trial; return 0; }
uint64_t member_digest(void) { return digest; }
void joined_state(int members) { joined = members; }
void send_alive(void) { alives++; }
/* clconn's probes - what each (node, path) answers, who was asked, at which port */
static int lane_off, wboot, conn_ans[64][CLCONN_PATHS], conn_asked[64][CLCONN_PATHS];
static int conn_port[64][CLCONN_PATHS], reaps;
int clane_off(void) { return lane_off; }
int cl_will_bootstrap(void) { return wboot; }
void clconn_reap(long long since) { (void)since; reaps++; }

int clconn_probe(int node, int path, const pc_addr *src, const pc_addr *to, long long t)
{
	(void)src;
	(void)t;
	if (node < 0 || node >= 64 || path < 0 || path >= CLCONN_PATHS)
		return 0;
	conn_asked[node][path]++;
	conn_port[node][path] = pc_addr_port(to);
	return conn_ans[node][path];
}
int compat_log_level = L_NOTICE;

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	size_t w = strlen(logbuf);
	va_list ap;

	(void)level;
	(void)tag;
	if (w + 1024 > sizeof logbuf)
		w = logbuf[0] = 0;
	va_start(ap, fmt);
	vsnprintf(logbuf + w, sizeof logbuf - w, fmt, ap);
	va_end(ap);
}

static struct peer *add_peer(const pc_addr *a, int node)
{
	struct peer *p = &PT.peers[PT.n_peers++];

	memset(p, 0, sizeof *p);
	p->addr = *a;
	p->node = node;
	p->last_seen_ms = p->alive_ms = now;
	p->nstate = PC_NST_READY;
	return p;
}

struct peer *peer_upsert(const pc_addr *a)
{
	struct peer *p = clpeers_by_addr(&PT, a);

	return p ? p : add_peer(a, 0);
}

void peer_publish(struct peer *p, int node, unsigned int free_mb, unsigned int total_mb,
		unsigned int live_kb, const unsigned char *ident, unsigned int incarn,
		unsigned int entries, int start)
{
	(void)free_mb; (void)total_mb; (void)live_kb; (void)ident; (void)incarn; (void)entries;
	(void)start;
	p->node = node;
	p->last_seen_ms = now;
	p->gone_ms = 0;
	published++;
}

/* the master's handle_join_req: the joiner admitted, a live peer with the next id */
void handle_join_req(const unsigned char *pt, size_t n, const pc_addr *from)
{
	(void)pt; (void)n;
	joins++;
	if (!refuse && !clpeers_by_addr(&PT, from))
		add_peer(from, next_id++);
}

#define SENT_MAX 600
static struct { pc_addr to; unsigned char b[5400]; size_t n; int udp; } sent[SENT_MAX];
static int nsent;

static int capture(const pc_addr *to, const unsigned char *pt, size_t n, int udp)
{
	if (nsent < SENT_MAX && n <= sizeof sent[0].b) {
		sent[nsent].to = *to;
		memcpy(sent[nsent].b, pt, n);
		sent[nsent].n = n;
		sent[nsent++].udp = udp;
	}
	return 0;
}
int clsend_seal(const pc_addr *to, const unsigned char *pt, size_t n) { return capture(to, pt, n, 0); }
int clsend_seal_udp(const pc_addr *to, const unsigned char *pt, size_t n) { return capture(to, pt, n, 1); }

/* frames of @type to @to (NULL: anyone); *last the index of the last one */
static int count_sent(unsigned char type, const pc_addr *to, int *last)
{
	int i, n = 0;

	for (i = 0; i < nsent; i++)
		if (sent[i].n && sent[i].b[0] == type && (!to || pc_addr_eq(&sent[i].to, to))) {
			n++;
			if (last)
				*last = i;
		}
	return n;
}

static int wait_to(const pc_addr *to, uint64_t *tok, struct clmemb_wait *w)
{
	int i = -1;

	memset(w, 0, sizeof *w);
	return count_sent(M_JOIN_WAIT, to, &i) && clmemb_wait_parse(sent[i].b, sent[i].n, tok, w);
}

static void beat(void)
{
	int i;

	for (i = 0; i < PT.n_peers; i++)
		if (PT.peers[i].node)
			PT.peers[i].last_seen_ms = PT.peers[i].alive_ms = now;
}

/* self (node 1) and @members - 1 peers: nodes 2.., at 10.0.0.10.. */
static void fleet(int members)
{
	int i;

	memset(&PT, 0, sizeof PT);
	for (i = 0; i < members - 1; i++) {
		pc_addr a = addr(10 + i);

		add_peer(&a, 2 + i);
	}
}

static const unsigned char jf[10] = { M_JOIN_REQ };

/* a member's M_LINK_REPORT: [type][flags][count LE][node LE][bits] per record */
static void report(const pc_addr *from, int connecting, int count, const int *node,
		const unsigned *bits, int recs)
{
	unsigned char m[4 + 3 * 8];
	int i;

	m[0] = M_LINK_REPORT;
	m[1] = connecting ? 0x01 : 0;
	pc_p16(m + 2, (uint16_t)count);
	for (i = 0; i < recs; i++) {
		pc_p16(m + 4 + 3 * i, (uint16_t)node[i]);
		m[6 + 3 * i] = (unsigned char)bits[i];
	}
	cladmit_link_report(m, 4 + 3 * (size_t)recs, from);
}

#define CS 0x01
#define RN 0x02
#define RO 0x04
#define LM 0x08                                /* the lane, clrpt, the bulk listener missing */
#define PM 0x10
#define BM 0x20

/* the bits for @node in the last M_LINK_REPORT sent to @to; -1 none */
static int report_bits(const pc_addr *to, int node)
{
	int i = -1, k;

	if (!count_sent(M_LINK_REPORT, to, &i))
		return -1;
	for (k = 0; k < pc_g16(sent[i].b + 2) && 4 + 3 * (size_t)k + 3 <= sent[i].n; k++)
		if (pc_g16(sent[i].b + 4 + 3 * k) == node)
			return sent[i].b[6 + 3 * k];
	return -1;
}

static void ticks(int n)
{
	while (n-- > 0) {
		now += 500;
		beat();
		cladmit_link_tick(now);
	}
}

/* ---- the frames a member list went in ------------------------------------------------------- */
static int parts_check(unsigned char type, const pc_addr *to, int want_parts, int per,
		int members, int trailer)
{
	int i, k = 0, total = 0, ok = 1;
	unsigned char seen[300] = { 0 };

	for (i = 0; i < nsent; i++) {
		struct clmemb_assign as;
		struct clmemb_trailer t;
		struct clmemb_member m;
		int r, has;

		if (sent[i].b[0] != type || !pc_addr_eq(&sent[i].to, to))
			continue;
		if (!clmemb_assign_parse(sent[i].b, sent[i].n, &as))
			return 0;
		has = clmemb_trailer_parse(sent[i].b, sent[i].n, as.count, &t);
		if (trailer && (!has || t.flags != CLMEMB_ASSIGN_CONNECT || t.part != k ||
		        t.parts != want_parts))
			ok = 0;
		if (!trailer && has)
			ok = 0;
		if (as.count != (k < want_parts - 1 ? per : members - per * (want_parts - 1)))
			ok = 0;
		for (r = 0; r < as.count && clmemb_member_at(sent[i].b, sent[i].n, r, &m); r++)
			if (m.node > 0 && m.node < 300 && !seen[m.node]++)
				total++;
		k++;
	}
	return ok && k == want_parts && total == members;
}

int main(void)
{
	pc_addr j1 = addr(100), j2 = addr(101), j3 = addr(102), j4 = addr(103), j5 = addr(104);
	struct clmemb_wait w;
	uint64_t tok;
	struct peer *p;
	char txt[300];
	int i, n;

	setvbuf(stdout, NULL, _IONBF, 0);

	printf("--- A: ELECTING from a member whose master is silent\n");
	role = PC_ROLE_MEMBER;
	fleet(4);
	cladmit_electing(&j1, 0x77, now - 3000);
	CHK(wait_to(&j1, &tok, &w) && tok == 0x77 && w.reason == CLMEMB_WAIT_ELECTING && w.term == 7 &&
		w.node == 1 && w.count == 4,
		"A. master silent 3 s: ELECTING (reason %d, term %u, node %d, %d member(s))",
		w.reason, w.term, w.node, w.count);
	now += 1000;
	cladmit_electing(&j1, 0x77, now - 4000);
	CHK(count_sent(M_JOIN_WAIT, &j1, NULL) == 1, "A. not again within 2 s");
	now += 1000;
	cladmit_electing(&j1, 0x77, now - 5000);
	CHK(count_sent(M_JOIN_WAIT, &j1, NULL) == 2, "A. again after 2 s");
	cladmit_electing(&j2, 0x78, now - 2999);
	CHK(!count_sent(M_JOIN_WAIT, &j2, NULL), "A. none while the master was heard 2.999 s ago");
	cladmit_electing(&j2, 0x78, 0);
	CHK(count_sent(M_JOIN_WAIT, &j2, NULL) == 1, "A. one when no master was ever heard");

	printf("--- B: the queue\n");
	role = PC_ROLE_MASTER;
	now += 10000;
	fleet(4);
	cladmit_master_begin(now - 1);
	nsent = joins = 0;
	CHK(cladmit_hold(jf, sizeof jf, &j1, 1) == 1 && joins == 1 && !count_sent(M_JOIN_WAIT, &j1, NULL),
		"B. the first joiner admitted at once (%d admission(s))", joins);
	p = clpeers_by_addr(&PT, &j1);
	CHK(p && p->nstate == PC_NST_CONNECTING, "B. and CONNECTING in the master's table");
	now += 10;
	cladmit_hold(jf, sizeof jf, &j2, 2);
	CHK(joins == 1 && wait_to(&j2, &tok, &w) && w.reason == CLMEMB_WAIT_QUEUED && w.count == 1 &&
		tok == 2, "B. the second told QUEUED, place %d, with one in flight below 16", w.count);
	now += 10;
	cladmit_hold(jf, sizeof jf, &j3, 3);
	CHK(joins == 1 && wait_to(&j3, &tok, &w) && w.count == 2, "B. the third, place %d", w.count);
	now += 3000;
	cladmit_hold(jf, sizeof jf, &j2, 2);
	now += 3001;
	beat();
	cladmit_tick(now);
	cladmit_hold(jf, sizeof jf, &j4, 4);
	CHK(wait_to(&j4, &tok, &w) && w.count == 2,
		"B. one silent 6 s left the queue: the next is place %d behind the one that asked again",
		w.count);

	now += 10000;
	fleet(16);
	cladmit_master_begin(now - 1);
	joins = 0;
	next_id = 100;
	for (i = 0; i < 3; i++) {
		pc_addr a = addr(120 + i);

		now += 10;
		cladmit_hold(jf, sizeof jf, &a, 10 + i);
	}
	CHK(joins == 1, "B. 16 members: 1 in flight (%d) - the joiner in flight is not a member", joins);
	now += 10000;
	fleet(17);
	cladmit_master_begin(now - 1);
	joins = nsent = 0;
	for (i = 0; i < 5; i++) {
		pc_addr a = addr(130 + i);

		now += 10;
		cladmit_hold(jf, sizeof jf, &a, 20 + i);
	}
	j5 = addr(134);
	CHK(joins == 4 && wait_to(&j5, &tok, &w) && w.count == 1,
		"B. 17 members: 4 in flight (%d), the fifth place %d", joins, w.count);

	printf("--- C: a new master's gate\n");
	now += 10000;
	fleet(4);
	p = add_peer(&j3, 30);
	p->nstate = PC_NST_CONNECTING;             /* admitted by a master before this one */
	now += 100;
	cladmit_master_begin(now);
	joins = 0;
	logbuf[0] = 0;
	cladmit_hold(jf, sizeof jf, &j1, 1);
	CHK(!joins && strstr(logbuf, "admission held - 0 of 4 live member(s) heard"),
		"C. no member heard since the mastership began: held");
	beat();
	cladmit_alive(now);
	CHK(!joins, "C. every member heard, one CONNECTING it did not admit: still held");
	p->nstate = PC_NST_READY;
	cladmit_alive(now);
	CHK(joins == 1 && strstr(logbuf, "admission open"), "C. that one READY: open, admitted");

	printf("--- D: the link check\n");
	now += 10000;
	fleet(3);                                  /* nodes 2 (10.0.0.10) and 3 (10.0.0.11) */
	cladmit_master_begin(now - 1);
	next_id = 9;
	nsent = joins = 0;
	stuck_gauge = 0;
	logbuf[0] = 0;
	cladmit_hold(jf, sizeof jf, &j1, 1);       /* node 9 */
	{
		pc_addr m2 = addr(10), m3 = addr(11);
		int x9[] = { 9 }, xj[] = { 1, 2, 3 };
		unsigned ok1[] = { CS }, okj[] = { CS, CS, CS };

		p = clpeers_by_addr(&PT, &j1);
		cladmit_link_tick(now);
		CHK(!count_sent(M_LANE_PROBE, &j1, NULL), "D. the master's first probe waits its spread");
		now += 500;
		beat();
		cladmit_link_tick(now);
		n = count_sent(M_LANE_PROBE, &j1, &i);
		CHK(n == 1 && sent[i].udp, "D. and is sent by UDP within 500 ms");
		cladmit_probe_answered(p, now);
		report(&m2, 0, 1, x9, ok1, 1);
		report(&m3, 0, 1, x9, ok1, 1);
		report(&j1, 1, 3, xj, okj, 3);
		CHK(!count_sent(M_ONLINE, &j1, NULL),
			"D. not ONLINE before the master's own view says the cluster socket answered");
		now += 500;
		cladmit_link_tick(now);
		n = count_sent(M_ONLINE, &j1, &i);
		CHK(n == 1 && sent[i].n == M_ONLINE_LEN && pc_g32(sent[i].b + 1) == 7 &&
			strstr(logbuf, "node 9 (10.0.0.100) is ONLINE - all 3 pair(s)"),
			"D. every pair both ways: ONLINE (%d)", n);
		p->nstate = PC_NST_READY;              /* its ALIVE says so */

		/* node 10: node 3 advertises a relay port that does not answer */
		cladmit_hold(jf, sizeof jf, &j2, 2);
		cladmit_hold(jf, sizeof jf, &j3, 3);
		CHK(joins == 2 && wait_to(&j3, &tok, &w) && w.count == 1, "D. the next in flight, one queued");
		p = clpeers_by_addr(&PT, &j2);
		{
			int x10[] = { 10 }, xk[] = { 1, 2, 3, 9 };     /* node 9 is a member now */
			unsigned rn[] = { CS | RN }, okk[] = { CS, CS, CS, CS };
			long long t0 = now;

			report(&m2, 0, 1, x10, ok1, 1);
			report(&m3, 0, 1, x10, rn, 1);
			report(&j1, 0, 1, x10, ok1, 1);
			report(&j2, 1, 4, xk, okk, 4);
			for (now += 500; now < t0 + 2000; now += 500) {
				cladmit_link_tick(now);
				cladmit_probe_answered(p, now);
			}
			CHK(!count_sent(M_ONLINE, &j2, NULL) && !stuck_gauge,
				"D. a relay port not answering: not ONLINE, not yet held");
			for (; now <= t0 + 10000; now += 500) {
				beat();
				cladmit_hold(jf, sizeof jf, &j3, 3);   /* it announces again, queued */
				cladmit_link_tick(now);
			}
			pc_cluster_admission_stuck_text(txt, sizeof txt);
			CHK(stuck_gauge == 1 && strstr(logbuf, "admission stuck - node 10 (10.0.0.101)") &&
				!strcmp(txt, "node 10 (10.0.0.101): node 3: the relay port did not answer"),
				"D. 10 s: held, counted (%llu) and named: '%s'", stuck_gauge, txt);
			cladmit_tick(now);
			CHK(joins == 3, "D. and the queue moved on (%d admissions)", joins);
			{
				int x11[] = { 11 };            /* admitted meanwhile: a pair with it too */

				report(&j2, 1, 1, x11, ok1, 1);
				report(&j3, 1, 1, x10, ok1, 1);
			}
			rn[0] = CS | RN | RO;
			report(&m3, 0, 1, x10, rn, 1);
			pc_cluster_admission_stuck_text(txt, sizeof txt);
			CHK(count_sent(M_ONLINE, &j2, NULL) == 1 && strstr(logbuf, "after being held"),
				"D. its relay port answered: ONLINE after being held");
			p->nstate = PC_NST_READY;
			now += 500;
			cladmit_link_tick(now);
			pc_cluster_admission_stuck_text(txt, sizeof txt);
			CHK(stuck_gauge == 0 && !txt[0], "D. none held: the gauge 0, the text empty");
		}
		/* node 11 (j3): a report whose count says 1 carries five records - only the first counts */
		p = clpeers_by_addr(&PT, &j3);
		{
			int x11[] = { 11 }, xl[] = { 1, 2, 3, 9, 10 };
			unsigned okl[] = { CS, CS, CS, CS, CS };

			report(&m2, 0, 1, x11, ok1, 1);
			report(&m3, 0, 1, x11, ok1, 1);
			report(&j1, 0, 1, x11, ok1, 1);
			report(&j2, 0, 1, x11, ok1, 1);
			report(&j3, 1, 1, xl, okl, 5);
			for (i = 0; i < 3; i++) {
				now += 500;
				beat();
				cladmit_link_tick(now);
				cladmit_probe_answered(p, now);
			}
			CHK(!count_sent(M_ONLINE, &j3, NULL),
				"D. a count of 1 (little-endian) reads one record of five: not ONLINE");
			report(&j3, 1, 5, xl, okl, 5);
			CHK(count_sent(M_ONLINE, &j3, NULL) == 1, "D. the count of 5: ONLINE");
			/* a report it sent before the ONLINE reached it, read after */
			report(&j3, 1, 1, xl, okl, 1);
			p->nstate = PC_NST_RECOVERING;     /* its ALIVE after the ONLINE */
			for (i = 0; i < 24; i++) {
				now += 500;
				beat();
				cladmit_link_tick(now);
			}
			pc_cluster_admission_stuck_text(txt, sizeof txt);
			CHK(stuck_gauge == 0 && !txt[0] && !strstr(logbuf, "admission stuck - node 11") &&
				count_sent(M_ONLINE, &j3, NULL) == 1,
				"D. a CONNECTING report older than its ONLINE: let go once its ALIVE says "
				"RECOVERING, never held 12 s on (%llu, '%s')", stuck_gauge, txt);
		}
	}

	printf("--- E: the probe spread\n");
	role = PC_ROLE_MEMBER;
	nst = PC_NST_CONNECTING;
	now += 10000;
	fleet(64);
	master_a = addr(10);
	cladmit_connect_begin(now, 0x99);
	{
		long long t0 = now;
		int at[7];

		nsent = 0;
		for (i = 0; i <= 6; i++) {
			unsigned char got[80] = { 0 };
			int k;

			now = t0 + 500 * i;
			beat();
			cladmit_link_tick(now);
			for (n = 0, k = 0; k < nsent; k++)
				if (sent[k].udp && sent[k].b[0] == M_LANE_PROBE) {
					p = clpeers_by_addr(&PT, &sent[k].to);
					if (p && p->node < 80 && !got[p->node]++)
						n++;
				}
			at[i] = n;
		}
		CHK(at[0] == 0 && at[3] > 0 && at[3] < 63 && at[6] == 63,
			"E. 63 peers probed: %d at once, %d by 1.5 s, %d by 3 s", at[0], at[3], at[6]);
		CHK(count_sent(M_LINK_REPORT, &master_a, NULL) == 7, "E. a report to the master each tick");
	}
	nst = PC_NST_READY;

	printf("--- F: the member list in parts\n");
	role = PC_ROLE_MASTER;
	now += 10000;
	fleet(8);
	{
		pc_addr to = addr(1000);                /* outside the 257 below */

		maxdg = 200;
		nsent = 0;
		logbuf[0] = 0;
		send_member_list(M_ASSIGN, &to, 0x55, 42);
		CHK(parts_check(M_ASSIGN, &to, 2, 5, 8, 1) &&
			strstr(logbuf, "goes in 2 parts - 8 member(s), 5 a datagram"),
			"F. max_datagram 200, 8 members: an ASSIGN in 2 parts (5, 3), numbered");
		n = count_sent(M_ASSIGN, &to, &i);
		{
			struct clmemb_assign as;
			struct clmemb_member m;

			CHK(clmemb_assign_parse(sent[i].b, sent[i].n, &as) && as.tok == 0x55 &&
				as.your_id == 42 && as.master_id == 1 &&
				clmemb_member_at(sent[0].b, sent[0].n, 0, &m) && m.node == 1,
				"F. its token, the joiner's id, the master's, the master first");
		}
		nsent = 0;
		send_member_list(M_MEMBERS, &to, 0, 42);
		CHK(parts_check(M_MEMBERS, &to, 2, 5, 8, 0), "F. M_MEMBERS: 2 parts, no trailer");

		fleet(PC_CL_MAXPEER + 1);
		maxdg = 0;
		nsent = 0;
		send_member_list(M_ASSIGN, &to, 0x55, 42);
		CHK(parts_check(M_ASSIGN, &to, 4, 65, PC_CL_MAXPEER + 1, 1),
			"F. %d members, max_datagram 0 (1400): 4 parts of 65 at most", PC_CL_MAXPEER + 1);
		maxdg = 65000;
		nsent = 0;
		send_member_list(M_ASSIGN, &to, 0x55, 42);
		CHK(parts_check(M_ASSIGN, &to, 1, PC_CL_MAXPEER + 1, PC_CL_MAXPEER + 1, 1),
			"F. max_datagram 65000: all %d in one frame, its trailer behind them",
			PC_CL_MAXPEER + 1);
		maxdg = 100;
		nsent = 0;
		send_member_list(M_ASSIGN, &to, 0x55, 42);
		CHK(parts_check(M_ASSIGN, &to, 129, 2, PC_CL_MAXPEER + 1, 1),
			"F. max_datagram 100 (no room for one): 2 a part, 129 parts - the byte's 255 kept");
		maxdg = 200;
	}

	printf("--- G: the member-list repair\n");
	role = PC_ROLE_MEMBER;
	now += 10000;
	fleet(8);
	master_a = addr(10);
	nsent = 0;
	asked = 0;
	for (i = 0; i < 2; i++)
		cladmit_members_check(0x2222, &master_a);
	CHK(!count_sent(M_MEMBERS_REQ, NULL, NULL), "G. two beacons off its view: not yet");
	cladmit_members_check(0x2222, &master_a);
	n = count_sent(M_MEMBERS_REQ, &master_a, &i);
	CHK(n == 1 && sent[i].n == M_MEMBERS_REQ_LEN && pc_g64(sent[i].b + 1) == 0x1111 && asked == 1,
		"G. the third: it asks the master, with its own digest");
	now += 4999;
	cladmit_members_check(0x2222, &master_a);
	CHK(count_sent(M_MEMBERS_REQ, NULL, NULL) == 1, "G. not again within 5 s");
	now += 1;
	cladmit_members_check(0x2222, &master_a);
	CHK(count_sent(M_MEMBERS_REQ, NULL, NULL) == 2 && asked == 2, "G. again at 5 s");
	cladmit_members_check(0x1111, &master_a);
	now += 10000;
	beat();
	cladmit_members_check(0x2222, &master_a);
	CHK(count_sent(M_MEMBERS_REQ, NULL, NULL) == 2, "G. its view matched once: the count starts over");

	role = PC_ROLE_MASTER;
	{
		unsigned char rq[M_MEMBERS_REQ_LEN] = { M_MEMBERS_REQ };
		pc_addr m2 = addr(10), stranger = addr(250);

		nsent = 0;
		answered = 0;
		now += 2000;
		for (i = 0; i < 25; i++)
			cladmit_members_req(rq, sizeof rq, &m2);
		CHK(answered == 20 && count_sent(M_MEMBERS, &m2, NULL) == 40,
			"G. the master answers 20 in a second (%llu), each list in 2 parts", answered);
		now += 1000;
		cladmit_members_req(rq, sizeof rq, &m2);
		CHK(answered == 21, "G. and more in the next second");
		cladmit_members_req(rq, sizeof rq, &stranger);
		CHK(answered == 21, "G. not to a node it does not know");
	}

	role = PC_ROLE_MEMBER;
	{
		unsigned char f[256];
		struct clmemb_assign h = { 0, 0, 2, 5 };
		struct clmemb_member m[5];
		pc_addr gone10 = addr(12), gone31 = addr(13), self = addr(5);
		size_t off;
		int k;

		p = clpeers_by_addr(&PT, &gone10);
		p->last_seen_ms = now - 20000;
		p->gone_ms = now - 10000;
		p = clpeers_by_addr(&PT, &gone31);
		p->last_seen_ms = now - 40000;
		p->gone_ms = now - 31000;
		memset(m, 0, sizeof m);
		m[0].addr = self.in4.sin_addr;      m[0].port = self.in4.sin_port;      m[0].node = 1;
		m[1].addr = PT.peers[0].addr.in4.sin_addr; m[1].port = PT.peers[0].addr.in4.sin_port;
		m[1].node = 2;
		m[2].addr = gone10.in4.sin_addr;    m[2].port = gone10.in4.sin_port;    m[2].node = 4;
		m[3].addr = gone31.in4.sin_addr;    m[3].port = gone31.in4.sin_port;    m[3].node = 5;
		m[4].addr = j4.in4.sin_addr;        m[4].port = j4.in4.sin_port;        m[4].node = 40;
		off = clmemb_assign_hdr_write(f, sizeof f, M_MEMBERS, &h);
		for (k = 0; k < 5; k++)
			off += clmemb_member_write(f + off, sizeof f - off, &m[k]);
		published = 0;
		cladmit_members(f, off, &PT.peers[1].addr);
		CHK(!published, "G. a list from a node not its master: ignored");
		cladmit_members(f, off, &master_a);
		p = clpeers_by_addr(&PT, &j4);
		CHK(published == 2 && p && p->node == 40 && clpeers_by_addr(&PT, &gone10)->gone_ms != 0,
			"G. from its master: the one it lacked added and the one gone 31 s revived (%d); "
			"itself, one live and one gone 10 s left alone", published);
	}

	printf("--- H: the joiner's later parts and its ONLINE\n");
	role = PC_ROLE_MEMBER;
	nst = PC_NST_CONNECTING;
	now += 10000;
	fleet(3);
	master_a = addr(10);
	cladmit_connect_begin(now, 0x99);
	{
		unsigned char f[128];
		struct clmemb_assign h = { 0x99, 9, 2, 2 }, as;
		struct clmemb_member m[2];
		struct clmemb_trailer t = { CLMEMB_ASSIGN_CONNECT, 1, 2 };
		pc_addr a30 = addr(30), a31 = addr(31), m3 = addr(11);
		size_t off;
		int k;

		memset(m, 0, sizeof m);
		m[0].addr = a30.in4.sin_addr; m[0].port = a30.in4.sin_port; m[0].node = 30;
		m[1].addr = a31.in4.sin_addr; m[1].port = a31.in4.sin_port; m[1].node = 31;
		off = clmemb_assign_hdr_write(f, sizeof f, M_ASSIGN, &h);
		for (k = 0; k < 2; k++)
			off += clmemb_member_write(f + off, sizeof f - off, &m[k]);
		off += clmemb_trailer_write(f + off, sizeof f - off, &t);
		clmemb_assign_parse(f, off, &as);
		published = 0;
		logbuf[0] = 0;
		cladmit_assign_part(f, off, &as, &m3);
		CHK(!published, "H. a part from a node not its master: ignored");
		as.tok = 0x98;
		cladmit_assign_part(f, off, &as, &master_a);
		CHK(!published, "H. a part under another token: ignored");
		as.tok = 0x99;
		nst = PC_NST_READY;
		cladmit_assign_part(f, off, &as, &master_a);
		CHK(!published, "H. a part when not CONNECTING: ignored");
		nst = PC_NST_CONNECTING;
		cladmit_assign_part(f, off, &as, &master_a);
		CHK(published == 2 && strstr(logbuf, "ASSIGN part 2 of 2 - 2 member(s)"),
			"H. part 2 of 2 from its master under its token: its 2 members known");
		t.part = 0;
		m[0].node = 32;
		m[0].addr = addr(32).in4.sin_addr;
		m[0].port = addr(32).in4.sin_port;
		off = clmemb_assign_hdr_write(f, sizeof f, M_ASSIGN, &h);
		for (k = 0; k < 2; k++)
			off += clmemb_member_write(f + off, sizeof f - off, &m[k]);
		off += clmemb_trailer_write(f + off, sizeof f - off, &t);
		cladmit_assign_part(f, off, &as, &master_a);
		CHK(published == 3, "H. part 1 arriving after part 2: taken too (one new member)");
		{
			unsigned char on[M_ONLINE_LEN] = { M_ONLINE };

			cladmit_online(on, sizeof on, &m3);
			CHK(!joined && !alives, "H. ONLINE from a node not its master: ignored");
			cladmit_online(on, sizeof on, &master_a);
			CHK(joined == clpeers_count_live(&PT, now) + 1 && alives == 1 &&
				cladmit_online_ms() == now && strstr(logbuf, "ONLINE - every link"),
				"H. ONLINE from its master: joined with %d member(s), its ALIVE sent", joined);
			published = 0;
			cladmit_assign_part(f, off, &as, &master_a);
			CHK(!published, "H. a part after ONLINE: ignored (the token is spent)");
		}
	}

	printf("--- I: 3c's TCP paths\n");
	role = PC_ROLE_MASTER;
	nst = PC_NST_READY;
	maxdg = 0;
	lane_off = 2;
	now += 10000;
	fleet(3);
	cladmit_master_begin(now - 1);
	next_id = 9;
	nsent = joins = reaps = 0;
	stuck_gauge = 0;
	logbuf[0] = 0;
	cladmit_hold(jf, sizeof jf, &j1, 1);       /* node 9 */
	{
		pc_addr m2 = addr(10), m3 = addr(11);
		int x9[] = { 9 }, x10[] = { 10 }, x11[] = { 11 }, xj[] = { 1, 2, 3 };
		int xk[] = { 1, 2, 3, 9 }, xl[] = { 1, 2, 3, 9, 10 };
		unsigned ok1[] = { CS }, okj[] = { CS, CS, CS }, okk[] = { CS, CS, CS, CS };
		unsigned okl[] = { CS, CS, CS, CS, CS }, lm1[] = { CS | LM }, bm1[] = { CS | BM };
		unsigned bml[] = { CS, CS, CS | BM, CS, CS };
		long long t0;

		p = clpeers_by_addr(&PT, &j1);
		for (i = 0; i < 2; i++) {
			ticks(1);
			cladmit_probe_answered(p, now);
		}
		CHK(conn_asked[9][CLCONN_LANE] && conn_port[9][CLCONN_LANE] == 6482 &&
			!conn_asked[9][CLCONN_BULK] && !conn_asked[9][CLCONN_RPT] && reaps >= 2,
			"I. the master connects to the joiner's lane (port %d), not its bulk listener nor clrpt",
			conn_port[9][CLCONN_LANE]);
		report(&m2, 0, 1, x9, ok1, 1);
		report(&m3, 0, 1, x9, ok1, 1);
		report(&j1, 1, 3, xj, okj, 3);
		ticks(1);
		CHK(!count_sent(M_ONLINE, &j1, NULL), "I. the lane's connect not taken: not ONLINE");
		conn_ans[9][CLCONN_LANE] = 1;
		ticks(1);
		CHK(count_sent(M_ONLINE, &j1, NULL) == 1 && p->link_tcp_ms[CLCONN_LANE] == now,
			"I. the connect taken: ONLINE");
		p->nstate = PC_NST_READY;

		cladmit_hold(jf, sizeof jf, &j2, 2);   /* node 10: member 3's lane connect not taken */
		p = clpeers_by_addr(&PT, &j2);
		conn_ans[10][CLCONN_LANE] = 1;
		report(&m2, 0, 1, x10, ok1, 1);
		report(&m3, 0, 1, x10, lm1, 1);
		report(&j1, 0, 1, x10, ok1, 1);
		report(&j2, 1, 4, xk, okk, 4);
		for (t0 = now; now <= t0 + 10000; ) {
			ticks(1);
			cladmit_probe_answered(p, now);
		}
		pc_cluster_admission_stuck_text(txt, sizeof txt);
		CHK(stuck_gauge == 1 && !count_sent(M_ONLINE, &j2, NULL) &&
			!strcmp(txt, "node 10 (10.0.0.101): node 3: the frame lane did not answer"),
			"I. a member's lane connect not taken: held, named '%s'", txt);
		report(&m3, 0, 1, x10, ok1, 1);
		CHK(count_sent(M_ONLINE, &j2, NULL) == 1, "I. taken: ONLINE");
		p->nstate = PC_NST_READY;

		cladmit_hold(jf, sizeof jf, &j3, 3);   /* node 11: the bulk listener, on its view only */
		p = clpeers_by_addr(&PT, &j3);
		conn_ans[11][CLCONN_LANE] = 1;
		report(&m2, 0, 1, x11, bm1, 1);        /* a member's view: not its to need */
		report(&m3, 0, 1, x11, ok1, 1);
		report(&j1, 0, 1, x11, ok1, 1);
		report(&j2, 0, 1, x11, ok1, 1);
		report(&j3, 1, 5, xl, bml, 5);
		for (t0 = now; now <= t0 + 10000; ) {
			ticks(1);
			cladmit_probe_answered(p, now);
		}
		pc_cluster_admission_stuck_text(txt, sizeof txt);
		CHK(stuck_gauge == 1 &&
			!strcmp(txt, "node 11 (10.0.0.102): node 3: the bulk listener did not answer"),
			"I. member 3's bulk listener on the joiner's view: held, named '%s' - and member 2's "
			"own bulk bit counts for nothing", txt);
		report(&j3, 1, 5, xl, okl, 5);
		CHK(count_sent(M_ONLINE, &j3, NULL) == 1, "I. its connect taken: ONLINE");
	}
	/* the joiner's side: a bootstrap ahead, replication over max_datagram to node 3 */
	role = PC_ROLE_MEMBER;
	nst = PC_NST_CONNECTING;
	wboot = 1;
	maxdg = 1400;
	now += 10000;
	fleet(3);
	master_a = addr(10);
	PT.peers[1].rp.port = 7000;                /* node 3 */
	memset(conn_ans, 0, sizeof conn_ans);
	memset(conn_asked, 0, sizeof conn_asked);
	cladmit_connect_begin(now, 0x77);
	nsent = 0;
	ticks(2);
	CHK(conn_asked[2][CLCONN_LANE] && conn_asked[2][CLCONN_BULK] && conn_port[2][CLCONN_BULK] == 6480 &&
		!conn_asked[2][CLCONN_RPT] && conn_asked[3][CLCONN_RPT] && conn_port[3][CLCONN_RPT] == 7000,
		"I. the joiner connects to each member's lane and bulk listener, and to clrpt where "
		"replication rides it (node 3, port %d)", conn_port[3][CLCONN_RPT]);
	CHK(report_bits(&master_a, 2) == (LM | BM) && report_bits(&master_a, 3) == (LM | PM | BM),
		"I. and reports each path missing: node 2 0x%x, node 3 0x%x", report_bits(&master_a, 2),
		report_bits(&master_a, 3));
	for (i = 0; i < CLCONN_PATHS; i++)
		conn_ans[2][i] = conn_ans[3][i] = 1;
	cladmit_probe_answered(&PT.peers[0], now);
	cladmit_probe_answered(&PT.peers[1], now);
	ticks(1);
	CHK(report_bits(&master_a, 2) == CS && report_bits(&master_a, 3) == CS,
		"I. each connect taken: nothing missing (0x%x, 0x%x)", report_bits(&master_a, 2),
		report_bits(&master_a, 3));

	printf("--- J: a restart's copy takes the joiner out of CONNECTING with no ONLINE\n");
	role = PC_ROLE_MASTER;
	nst = PC_NST_READY;
	wboot = maxdg = lane_off = 0;
	now += 10000;
	fleet(3);
	cladmit_master_begin(now - 1);
	next_id = 9;
	nsent = joins = 0;
	stuck_gauge = 0;
	logbuf[0] = 0;
	cladmit_hold(jf, sizeof jf, &j1, 1);       /* node 9: node 3's relay port never answers */
	{
		pc_addr m2 = addr(10), m3 = addr(11);
		int x9[] = { 9 }, xj[] = { 1, 2, 3 };
		unsigned ok1[] = { CS }, rn[] = { CS | RN }, okj[] = { CS, CS, CS };
		long long t0;

		p = clpeers_by_addr(&PT, &j1);
		report(&m2, 0, 1, x9, ok1, 1);
		report(&m3, 0, 1, x9, rn, 1);
		report(&j1, 1, 3, xj, okj, 3);
		for (t0 = now; now <= t0 + 10000; ) {
			ticks(1);
			cladmit_probe_answered(p, now);
		}
		CHK(stuck_gauge == 1 && !count_sent(M_ONLINE, &j1, NULL), "J. node 9 held at 10 s (%llu)",
			stuck_gauge);
		p->nstate = PC_NST_RECOVERING;         /* its ALIVE: a copy began */
		ticks(1);
		pc_cluster_admission_stuck_text(txt, sizeof txt);
		CHK(stuck_gauge == 0 && !txt[0] && !count_sent(M_ONLINE, &j1, NULL) &&
			strstr(logbuf, "node 9 (10.0.0.100) left CONNECTING without an ONLINE, now recovering - "
			"its link check ends, and its hold"),
			"J. its ALIVE says RECOVERING: the check and the hold end, said, no ONLINE (%llu, '%s')",
			stuck_gauge, txt);
		p->nstate = PC_NST_READY;
		ticks(24);
		CHK(stuck_gauge == 0 && !count_sent(M_ONLINE, &j1, NULL), "J. READY 12 s on: none held (%llu)",
			stuck_gauge);

		cladmit_hold(jf, sizeof jf, &j2, 2);   /* node 10: admitted, never says CONNECTING */
		CHK(joins == 2, "J. the next admitted (%d)", joins);
		p = clpeers_by_addr(&PT, &j2);
		p->nstate = PC_NST_RECOVERING;
		for (t0 = now; now <= t0 + 10000; )
			ticks(1);
		CHK(stuck_gauge == 1 && !strstr(logbuf, "node 10 (10.0.0.101) left CONNECTING"),
			"J. one that never reported CONNECTING is not let go on the master's view alone (%llu)",
			stuck_gauge);
	}

	printf("--- K: a CONNECTING member an earlier master admitted, held past CONNECT_MS\n");
	role = PC_ROLE_MASTER;
	nst = PC_NST_READY;
	wboot = maxdg = lane_off = 0;
	now += 10000;
	fleet(3);
	p = add_peer(&j3, 30);
	p->nstate = PC_NST_CONNECTING;
	now += 100;
	cladmit_master_begin(now);
	nsent = joins = 0;
	stuck_gauge = 0;
	logbuf[0] = 0;
	{
		pc_addr m2 = addr(10), m3 = addr(11);
		int x30[] = { 30 }, xk[] = { 1, 2, 3 };
		unsigned ok1[] = { CS }, rn[] = { CS | RN }, okk[] = { CS, CS, CS };
		long long t0;

		report(&j3, 1, 3, xk, okk, 3);         /* its own report: checked again */
		report(&m2, 0, 1, x30, ok1, 1);
		report(&m3, 0, 1, x30, rn, 1);         /* node 3's relay port never answers it */
		now += 500;
		beat();
		cladmit_hold(jf, sizeof jf, &j1, 1);
		CHK(!joins && strstr(logbuf, "admission held") &&
			strstr(logbuf, "1 CONNECTING member(s) checked again"),
			"K. while node 30 is checked again: held, and the line says that is why");
		for (t0 = now; now <= t0 + 10000; ) {
			ticks(1);
			cladmit_probe_answered(p, now);
		}
		CHK(stuck_gauge == 1 && strstr(logbuf, "admission stuck - node 30 (10.0.0.102)"),
			"K. node 30 held at 10 s (%llu)", stuck_gauge);
		beat();
		cladmit_hold(jf, sizeof jf, &j1, 1);
		CHK(joins == 1, "K. once it is held the queue moves on, as the log says: the next "
			"joiner admitted (%d)", joins);
	}

	printf("cladmittest: %s\n", fails ? "FAILED" : "all passed");
	return fails != 0;
}
