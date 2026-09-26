/*
 * clpstest.c - the pub/sub relay plane with nothing of cluster.c linked
 * (M16, wave 4).
 *
 * Until this, the plane's only checks were daemons: relaytest,
 * relayinteresttest and the PS11 port tests, each a fleet on one host.
 * The failure that cost the most was a restarted node answered as a
 * stale slot, which then FILTERED relays it had never been told about -
 * a subscription made in that window was never delivered, 6 runs in 9 on
 * two CPUs, and nothing below a fleet could see it.
 *
 * This links the REAL clps.o, clsend.o, clpeers.o, clwire.o, psindex.o
 * and quiesce.o; the transmit is captured (clsend's hook) and every
 * datagram is opened and parsed back.  Only cluster.c's accessors and
 * pubsub.c's callbacks are stand-ins.  What it pins, per peer: who gets
 * a relay and on which sequence space (everything / filtered / broadcast
 * until current), what the interest handlers accept and from whom, and
 * what the relay port's probe step reports.  Nothing binds a socket: the
 * probes are checked up to the decision, and a peer is never direct
 * while a relay is sent.
 *
 * Build: see the Makefile's clpstest target.
 */
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <sodium.h>

#include "../src/cluster.h"
#include "../src/clstate.h"
#include "../src/clpeers.h"
#include "../src/clsend.h"
#include "../src/clwire.h"
#include "../src/clmsg.h"
#include "../src/clps.h"
#include "../src/psinterest.h"
#include "../src/psrelay.h"
#include "../src/quiesce.h"
#include "../src/pubsub.h"            /* pc_cluster_pubsub_relay, the callbacks */

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

/* ---- the stand-ins: cluster.c's accessors ------------------------------- */
static struct clpeers PT;
static long long now = 5000000;
static int enabled = 1;

struct clpeers *cl_peers(void) { return &PT; }
int cl_enabled(void) { return enabled; }
int cl_node_id(void) { return 42; }
long long cl_now_ms(void) { return now; }
struct sockaddr_in cl_self_addr(void)
{
	struct sockaddr_in a;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(0x7f001001u);
	return a;
}

/* ---- and pubsub.c's callbacks --------------------------------------------- */
static uint64_t my_ver;             /* our interest version; 0 = none */
static unsigned long long delivered, dups, lost;
static char got_chan[64], got_data[64];
static int got_origin;

int pc_pubsub_publish(const char *chan, size_t clen, const char *data,
		size_t dlen, int origin)
{
	delivered++;
	snprintf(got_chan, sizeof got_chan, "%.*s", (int)clen, chan);
	snprintf(got_data, sizeof got_data, "%.*s", (int)dlen, data);
	got_origin = origin;
	return 0;
}
void pc_pubsub_relay_account(unsigned long long sent, unsigned long long l,
		unsigned long long dropped, unsigned long long d)
{
	(void)sent; (void)dropped;
	lost += l;
	dups += d;
}
uint64_t pc_pubsub_interest_version(void) { return my_ver; }
size_t pc_pubsub_interest_full(unsigned char *out, size_t cap)
{
	if (cap < PSF_HDR)
		return 0;
	memset(out, 0, PSF_HDR);           /* no names, no patterns */
	return PSF_HDR;
}
void pc_pubsub_interest_tick(long long t) { (void)t; }

/* ---- the capture ------------------------------------------------------- */
static const uint8_t KEY[32] = { 7, 1 };
#define NOUT 8
static struct {
	unsigned last, port;
	unsigned char raw[256];            /* the sealed bytes, when short */
	size_t rawn;
	unsigned char pt[65536];
	unsigned long long n;
} out[NOUT];
static int nout;

static long capture(int fd, const void *buf, size_t n,
		const struct sockaddr_in *to)
{
	(void)fd;
	if (nout < NOUT) {
		out[nout].last = ntohl(to->sin_addr.s_addr) & 0xff;
		out[nout].port = ntohs(to->sin_port);
		out[nout].rawn = n < sizeof out[nout].raw ? n : 0;
		if (out[nout].rawn)
			memcpy(out[nout].raw, buf, n);
		if (clwire_open(KEY, buf, n, out[nout].pt, &out[nout].n) != 0)
			out[nout].n = 0;
	}
	nout++;
	return (long)n;
}

/* big-endian, as the plane writes its frames */
static unsigned g16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint64_t g64(const unsigned char *p)
{
	uint64_t v = 0;
	int i;

	for (i = 0; i < 8; i++)
		v = v << 8 | p[i];
	return v;
}
static void p16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static void p32(unsigned char *p, uint32_t v) { p16(p, v >> 16); p16(p + 2, v & 0xffff); }
static void p64(unsigned char *p, uint64_t v) { p32(p, (uint32_t)(v >> 32)); p32(p + 4, (uint32_t)v); }

static struct peer *add(unsigned char last, int node, long long seen)
{
	struct peer *p = &PT.peers[PT.n_peers++];

	memset(p, 0, sizeof *p);
	p->addr.sin_family = AF_INET;
	p->addr.sin_port = htons(17163);
	p->addr.sin_addr.s_addr = htonl(0x7f001000u | last);
	p->node = node;
	p->last_seen_ms = seen;
	return p;
}

static struct peer *P5, *P7;

/* time passes; the live peers keep talking */
static void advance(long long ms)
{
	now += ms;
	P5->last_seen_ms = P7->last_seen_ms = now - 10;
}

static int relay(const char *chan)
{
	nout = 0;
	return pc_cluster_pubsub_relay(chan, strlen(chan), "hi", 2);
}

/* the datagram that went to peer `last`, or -1 */
static int to(unsigned last)
{
	int i;

	for (i = 0; i < nout && i < NOUT; i++)
		if (out[i].last == last)
			return i;
	return -1;
}

static uint64_t seq_of(int i) { return g64(out[i].pt + 3); }

static struct pc_psint_figures fig(void)
{
	struct pc_psint_figures f;

	pc_cluster_pubsub_interest_figures(&f);
	return f;
}

static struct sockaddr_in from_addr(unsigned char last)
{
	struct sockaddr_in a;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(17163);
	a.sin_addr.s_addr = htonl(0x7f001000u | last);
	return a;
}

/* a full interest state: [M_PSINT_FULL][node][ver][flags][bloom][npat]
 * then the patterns */
static unsigned char fullf[3 + PSF_HDR + 64];
static size_t full_frame(unsigned node, uint64_t ver, const char *name,
		const char *pattern)
{
	unsigned char *b = fullf + 3;
	size_t n = PSF_HDR;
	uint32_t h1, h2;

	memset(fullf, 0, sizeof fullf);
	fullf[0] = M_PSINT_FULL;
	p16(fullf + 1, node);
	p64(b, ver);
	if (name) {
		psb_hash(name, strlen(name), &h1, &h2);
		psb_add(b + 9, h1, h2);
	}
	if (pattern) {
		p16(b + PSF_HDR - 2, 1);
		p16(b + n, (unsigned)strlen(pattern));
		memcpy(b + n + 2, pattern, strlen(pattern));
		n += 2 + strlen(pattern);
	}
	return 3 + n;
}

/* one update: kind 0 a name (its hashes), kind 1 a pattern */
static size_t add_frame(unsigned char *m, unsigned node, uint64_t ver,
		int pattern, const char *s)
{
	uint32_t h1, h2;

	m[0] = M_PSINT_ADD;
	p16(m + 1, node);
	p64(m + 3, ver);
	if (!pattern) {
		m[11] = 0;
		psb_hash(s, strlen(s), &h1, &h2);
		p32(m + 12, h1);
		p32(m + 16, h2);
		return 20;
	}
	m[11] = 1;
	p16(m + 12, (unsigned)strlen(s));
	memcpy(m + 14, s, strlen(s));
	return 14 + strlen(s);
}

int main(void)
{
	struct sockaddr_in f5 = from_addr(5), f7 = from_addr(7), f8 = from_addr(8);
	unsigned char m[128];
	uint64_t s5, s7, iv;
	int i5, i7, why, err;
	size_t n;

	if (sodium_init() < 0) {
		printf("clpstest: sodium_init failed\n");
		return 2;
	}
	clsend_init(3, KEY, capture);
	clps_init();
	clps_new_epoch();
	pc_qs_attach();                    /* a worker, inside its turn: */
	pc_qs_enter();                     /* the filters may be read */
	P5 = add(5, 5, now - 10);
	P7 = add(7, 7, now - 10);
	add(9, 9, now - PEER_UP_MS - PEER_SLOT_GRACE_MS - 1);   /* long gone */
	add(8, 0, now - 10);               /* a departed peer's slot: peer_gone
	                                    * zeroes the id, keeps the address */

	/* ---- nobody filters: one seal, the same bytes to every live peer --- */
	chk(relay("news") == 2 && nout == 2 && to(5) >= 0 && to(7) >= 0 && to(9) < 0,
		"relay: every LIVE peer, not the gone one");
	i5 = to(5); i7 = to(7);
	chk(out[i5].rawn && out[i5].rawn == out[i7].rawn && !memcmp(out[i5].raw, out[i7].raw, out[i5].rawn),
		"relay, everyone wants all: sealed ONCE, the same bytes to each");
	chk(out[i5].pt[0] == M_PUBLISH && g16(out[i5].pt + 1) == 42 && g16(out[i5].pt + 11) == 4 &&
	    !memcmp(out[i5].pt + 13, "news", 4) && out[i5].n == 17 + 4 + 2 && !memcmp(out[i5].pt + 17 + 4, "hi", 2),
		"relay: M_PUBLISH from node 42, the channel and the message");
	chk(out[i5].port == 17163, "relay: on the cluster socket's port (no relay port proven)");
	s5 = seq_of(i5);
	relay("news");
	chk(psr_counter(seq_of(to(5))) == psr_counter(s5) + 1 && psr_lane(seq_of(to(5))) == psr_lane(s5) &&
	    psr_epoch(seq_of(to(5))) == psr_epoch(s5),
		"relay: this thread's lane, the next counter, the same epoch");

	/* ---- what the relay refuses --------------------------------------- */
	{
		static char big[MAX_DGRAM];

		memset(big, 'x', sizeof big);
		nout = 0;
		chk(pc_cluster_pubsub_relay("c", 1, big, sizeof big) == -1 && nout == 0,
			"relay over the datagram: -1 (delivered locally only), nothing sent");
	}
	enabled = 0;
	chk(relay("news") == 0 && nout == 0, "relay, cluster off: 0, nothing sent");
	enabled = 1;

	/* ---- the receiving side ---------------------------------------------- */
	relay("rx");
	i5 = to(5);
	delivered = dups = 0;
	clps_handle_publish(out[i5].pt, out[i5].n);
	chk(delivered == 1 && !strcmp(got_chan, "rx") && !strcmp(got_data, "hi") && got_origin == 1,
		"receive: delivered here as from a peer (origin 1: never relayed again)");
	clps_handle_publish(out[i5].pt, out[i5].n);
	chk(delivered == 1 && dups == 1, "receive the same sequence again: a duplicate, not delivered twice");
	clps_handle_publish(out[i5].pt, out[i5].n - 1);
	chk(delivered == 1, "receive a frame one byte short: dropped");

	/* ---- a peer that filters, before we have its state ------------------ */
	iv = psv_make(3, 0, 0);
	nout = 0;
	chk(clps_heard(P7, 0, iv, now) == 0 && P7->pv_mode == PV_BROADCAST,
		"interest heartbeat, no state yet: BROADCAST (everything until current)");
	chk(nout == 1 && out[0].last == 7 && out[0].pt[0] == M_PSINT_REQ && g16(out[0].pt + 1) == 42 &&
	    fig().requests_sent == 1 && fig().peers_broadcast == 1,
		"  and it is asked for its full state, once");
	nout = 0;
	advance(500);
	clps_heard(P7, 0, iv, now);
	chk(nout == 0, "  not again within the second");
	advance(600);
	clps_heard(P7, 0, iv, now);
	chk(nout == 1 && out[0].pt[0] == M_PSINT_REQ, "  again after it");

	relay("news");
	i5 = to(5); i7 = to(7);
	s5 = seq_of(i5); s7 = seq_of(i7);
	chk(i5 >= 0 && i7 >= 0 && psr_epoch(s7) == (psr_epoch(s5) ^ 0x8000),
		"relay to a BROADCAST peer: its own sequence space (epoch ^ 0x8000)");
	chk(out[i5].rawn != out[i7].rawn || memcmp(out[i5].raw, out[i7].raw, out[i5].rawn),
		"  sealed for it alone");
	relay("news");
	chk(psr_counter(seq_of(to(7))) == psr_counter(s7) + 1,
		"  counted on its own counter, so what it is not sent is not a loss to it");

	/* ---- its full state: it filters -------------------------------------- */
	n = full_frame(7, iv, "news", NULL);
	clps_handle_int_full(fullf, n, &f8);
	chk(P7->pv_mode == PV_BROADCAST && fig().resyncs_received == 0,
		"full state from the right node at the WRONG address: ignored");
	full_frame(0, iv, "news", NULL);
	clps_handle_int_full(fullf, n, &f8);
	chk(fig().resyncs_received == 0,
		"full state from node 0 at a departed peer's address: ignored (the stale slot is nobody)");
	n = full_frame(7, iv, "news", NULL);
	clps_handle_int_full(fullf, n - 1, &f7);
	chk(P7->pv_mode == PV_BROADCAST, "full state one byte short: ignored");
	clps_handle_int_full(fullf, n, &f7);
	chk(P7->pv_mode == PV_FILTER && fig().resyncs_received == 1 && fig().peers_filtered == 1 &&
	    fig().peers_broadcast == 0,
		"full state from node 7 at its address: FILTER");

	{
		unsigned long long sk = fig().skipped;

		chk(relay("sports") == 1 && to(7) < 0 && to(5) >= 0 && fig().skipped == sk + 1,
			"filtering: a channel it cannot want is not sent to it, and is counted");
		chk(relay("news") == 2 && to(7) >= 0, "filtering: a channel it subscribed is");
	}

	/* ---- updates ------------------------------------------------------------ */
	n = add_frame(m, 7, psv_make(3, 0, 1), 0, "sports");
	clps_handle_int_add(m, n, &f8);
	chk(relay("sports") == 1 && fig().updates_received == 0, "update from the wrong address: ignored");
	clps_handle_int_add(m, n, &f7);
	chk(fig().updates_received == 1 && P7->pv_mode == PV_FILTER && relay("sports") == 2 && to(7) >= 0,
		"update (a name): applied, still FILTER, and the channel now reaches it");
	n = add_frame(m, 7, psv_make(3, 0, 2), 1, "weather.*");
	clps_handle_int_add(m, n, &f7);
	chk(P7->pv_mode == PV_FILTER && relay("weather.today") == 2 && to(7) >= 0,
		"update (a pattern): a matching channel reaches it");
	chk(relay("weatherman") == 1 && to(7) < 0, "  a channel the pattern does not match does not");
	m[11] = 2;
	clps_handle_int_add(m, n, &f7);
	chk(fig().updates_received == 2, "update of an unknown kind: ignored");

	/* an update goes missing: 3 is lost, 4 arrives */
	n = add_frame(m, 7, psv_make(3, 0, 4), 0, "misc");
	nout = 0;
	clps_handle_int_add(m, n, &f7);
	chk(P7->pv_mode == PV_BROADCAST && relay("weatherman") == 2 && to(7) >= 0,
		"an update missing: BROADCAST - everything goes to it until it is current");
	nout = 0;
	advance(1000);
	clps_interest_tick(now);
	chk(nout == 1 && out[0].last == 7 && out[0].pt[0] == M_PSINT_REQ,
		"  and a second later the tick asks for its full state");
	n = full_frame(7, psv_make(3, 0, 4), "news", NULL);
	clps_handle_int_full(fullf, n, &f7);
	chk(P7->pv_mode == PV_FILTER, "  which puts it back to FILTER");

	/* ---- it asks for ours ---------------------------------------------------- */
	m[0] = M_PSINT_REQ;
	p16(m + 1, 5);
	nout = 0;
	my_ver = 0;
	clps_handle_int_req(m, 3, &f5);
	chk(nout == 0, "a request while we filter nothing (version 0): not answered");
	my_ver = psv_make(1, 0, 0);
	clps_handle_int_req(m, 3, &f8);
	chk(nout == 0, "a request from the wrong address: not answered");
	p16(m + 1, 0);
	clps_handle_int_req(m, 3, &f8);
	chk(nout == 0, "a request from node 0 at a departed peer's address: not answered (3fb73fa)");
	p16(m + 1, 5);
	clps_handle_int_req(m, 3, &f5);
	chk(nout == 1 && out[0].last == 5 && out[0].pt[0] == M_PSINT_FULL && g16(out[0].pt + 1) == 42 &&
	    out[0].n == 3 + PSF_HDR && fig().resyncs_sent == 1,
		"a request from node 5 at its address: our full state, to it");

	/* ---- it wants everything again ------------------------------------------ */
	chk(clps_heard(P7, 0, 0, now) == 0 && P7->pv_mode == PV_ALL && fig().peers_filtered == 0,
		"a heartbeat with no interest: ALL again");
	relay("weatherman");
	i5 = to(5); i7 = to(7);
	chk(i5 >= 0 && i7 >= 0 && out[i5].rawn == out[i7].rawn && !memcmp(out[i5].raw, out[i7].raw, out[i5].rawn),
		"  and it shares the one seal again");

	/* ---- the relay port's probe step (no relays from here on) ------------- */
	nout = 0;
	chk(clps_heard(P5, 17999, 0, now) == 0 && clps_probe_peer(P5, now) == CLPS_EV_NONE,
		"probe: a new port is probed, nothing to say yet");
	chk(nout == 1 && out[0].last == 5 && out[0].port == 17999 && out[0].pt[0] == M_PSPROBE &&
	    g16(out[0].pt + 1) == 42,
		"  M_PSPROBE from node 42 to its RELAY port");
	memcpy(m + 3, out[0].pt + 3, 8);   /* the nonce, echoed */
	m[0] = M_PSPROBE_ACK;
	p16(m + 1, 5);
	m[3] ^= 1;
	clps_handle_probe_ack(m, 11, &f5);
	chk(clps_probe_peer(P5, now + 1) == CLPS_EV_NONE, "an answer with the wrong nonce: not direct");
	m[3] ^= 1;
	clps_handle_probe_ack(m, 11, &f7);
	chk(clps_probe_peer(P5, now + 1) == CLPS_EV_NONE, "the right nonce from another address: not direct");
	clps_handle_probe_ack(m, 11, &f5);
	chk(clps_probe_peer(P5, now + 1) == CLPS_EV_DIRECT, "the right answer: DIRECT, said once");
	chk(clps_probe_peer(P5, now + 2) == CLPS_EV_NONE, "  and not said again");
	{
		int port, thr, direct;
		unsigned long long sd, sc, rx;

		pc_cluster_pubsub_relay_figures(&port, &thr, &direct, &sd, &sc, &rx);
		chk(direct == 1 && port == 0 && thr == 0, "figures: one peer direct, no port of ours bound");
	}
	{
		int ev = CLPS_EV_NONE, k;

		for (k = 0; k < 5 && ev == CLPS_EV_NONE; k++) {
			advance(1000);
			ev = clps_probe_peer(P5, now);
		}
		chk(ev == CLPS_EV_STOPPED && k >= 3, "no answers: STOPPED after the direct window, not before");
		advance(1000);
		chk(clps_probe_peer(P5, now) == CLPS_EV_NONE, "  said once");
		advance(10000);
		chk(clps_probe_peer(P5, now) == CLPS_EV_NONE,
			"  and the never-answered warning does not repeat it for the same outage");
	}
	chk(clps_heard(P5, 0, 0, now) == 0, "withdrawing a port that is not direct: nothing to say");

	/* a port that never answers */
	{
		int ev, silent = 0, k;

		clps_heard(P7, 17999, 0, now);
		for (k = 0; k < 12; k++) {
			ev = clps_probe_peer(P7, now);
			silent += ev == CLPS_EV_SILENT;
			advance(1000);
		}
		chk(silent == 1, "a port that never answers: SILENT once, after 10 s");
	}
	/* a direct peer withdraws its port */
	clps_heard(P5, 17998, 0, now);
	clps_probe_peer(P5, now);
	nout = 0;
	clps_probe_peer(P5, now + 1000);   /* a re-probe carries a new nonce */
	for (n = 0; (int)n < nout && out[n].pt[0] != M_PSPROBE; n++)
		;
	memcpy(m + 3, out[n].pt + 3, 8);
	clps_handle_probe_ack(m, 11, &f5);
	clps_probe_peer(P5, now + 1001);
	chk(P5->ps.direct && clps_heard(P5, 0, 0, now + 1002) == 1 && !P5->ps.direct,
		"a DIRECT peer withdraws its port: said (as configuration, not a fallback)");

	/* ---- the relay port, without binding anything ---------------------- */
	enabled = 0;
	chk(clps_rx_open(17999, 2, &why, &err) == 0 && why == CLPS_RX_OFF, "rx_open, cluster off: 0, nothing to say");
	enabled = 1;
	chk(clps_rx_open(17999, 0, &why, &err) == 0 && why == CLPS_RX_OFF, "rx_open, no threads asked: 0, nothing to say");
	chk(clps_rx_open(0, 2, &why, &err) == 0 && why == CLPS_RX_OFF, "rx_open, no port: 0, nothing to say");
	chk(clps_rx_port() == 0, "no relay port bound: 0 advertised");

	pc_qs_exit();
	printf("clpstest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
