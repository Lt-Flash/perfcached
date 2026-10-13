/*
 * clsendtest.c - the cluster plane's send path (M14, wave 4).
 *
 * What goes wrong here goes wrong silently: a datagram sealed under the
 * wrong key is dropped by every peer without a word, one sent to a
 * departed node's address is a write that never lands, and a broadcast
 * that skips a live peer or reaches a dead one is only ever visible as a
 * member that "sometimes" lags.  So every case opens what was sent.
 *
 * No socket: clsend_init() takes the transmit as a function, and this
 * one records what would have gone out - rule 7 of doc/MODULARITY.md.
 *
 * Build: cc -o clsendtest test/clsendtest.c src/clsend.o src/clpeers.o
 *        src/clwire.o -lsodium
 */
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <sodium.h>

#include "../src/clsend.h"
#include "../src/clpeers.h"
#include "../src/clwire.h"

static int pass, fail;
static void chk(int cond, const char *what)
{
	if (cond) { pass++; return; }
	fail++;
	printf("  FAIL %s\n", what);
}

static const uint8_t K[32] = { 9, 8, 7, 6, 5 };
static const uint8_t K2[32] = { 9, 8, 7, 6, 4 };   /* another fleet */

/* the capture: what would have gone out, and to whom */
static struct { struct sockaddr_in to; unsigned char buf[512]; size_t n; } out[16];
static int nout;
static unsigned char refuse_last = 0;   /* this address's last octet fails */

static long capture(int fd, const void *buf, size_t n,
		const struct sockaddr_in *to)
{
	(void)fd;
	if (refuse_last &&
	    (ntohl(to->sin_addr.s_addr) & 0xff) == refuse_last)
		return -1;
	if (nout < 16 && n <= sizeof out[0].buf) {
		out[nout].to = *to;
		memcpy(out[nout].buf, buf, n);
		out[nout].n = n;
	}
	nout++;
	return (long)n;
}

static struct sockaddr_in A(unsigned char last)
{
	struct sockaddr_in a;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(17163);
	a.sin_addr.s_addr = htonl(0x7f001000u | last);   /* 127.0.16.<last> */
	return a;
}

static unsigned last_octet(int i)
{
	return ntohl(out[i].to.sin_addr.s_addr) & 0xff;
}

/* open capture i with key k: 1 when it opens to exactly msg */
static int opens_to(int i, const uint8_t *k, const char *msg)
{
	unsigned char pt[512];
	unsigned long long ptlen = 0;

	return clwire_open(k, out[i].buf, out[i].n, pt, &ptlen) == 0 &&
		ptlen == strlen(msg) && memcmp(pt, msg, ptlen) == 0;
}

static struct clpeers PT;

static void add(unsigned char last, int node, long long seen)
{
	struct peer *p = &PT.peers[PT.n_peers++];

	memset(p, 0, sizeof *p);
	p->addr = A(last);
	p->node = node;
	p->last_seen_ms = seen;
}

int main(void)
{
	const long long now = 1000000;
	const char *msg = "a frame for the peer plane";
	unsigned char big[MAX_DGRAM + 8];
	struct sockaddr_in to = A(42);
	int r;

	if (sodium_init() < 0) {
		printf("clsendtest: libsodium would not initialise\n");
		return 2;
	}
	clsend_init(7, K, capture);
	/* the table: live 5 at .5, live 7 at .7, 9 at .9 unheard for longer
	 * than PEER_UP_MS, and a departed slot (node 0) at .11 */
	add(5, 5, now - 100);
	add(7, 7, now - 2000);
	add(9, 9, now - PEER_UP_MS - 1);
	add(11, 0, now - 100);

	/* ---- seal ------------------------------------------------------ */
	nout = 0;
	r = clsend_seal(&to, (const unsigned char *)msg, strlen(msg));
	chk(r == 0 && nout == 1 && last_octet(0) == 42, "seal: one datagram, to the address given");
	chk(opens_to(0, K, msg), "seal: it opens, under the cluster key, to the plaintext");
	chk(!opens_to(0, K2, msg), "seal: another fleet's key cannot open it");
	chk(out[0].n == HDR_LEN + strlen(msg) + CLWIRE_TAG, "seal: header + plaintext + tag, nothing else");
	nout = 0;
	chk(clsend_seal(&to, big, MAX_DGRAM + 1) == -1 && nout == 0, "seal: past MAX_DGRAM is refused, nothing sent");

	/* ---- to a node id ----------------------------------------------- */
	nout = 0;
	r = clsend_to_node(&PT, 7, (const unsigned char *)msg, strlen(msg), now);
	chk(r == 0 && nout == 1 && last_octet(0) == 7 && opens_to(0, K, msg),
		"to_node: node 7's address and no other, and it opens");
	nout = 0;
	chk(clsend_to_node(&PT, 9, (const unsigned char *)msg, strlen(msg), now) == -1 && nout == 0,
		"to_node: a node not heard for PEER_UP_MS gets nothing and -1");
	nout = 0;
	chk(clsend_to_node(&PT, 0, (const unsigned char *)msg, strlen(msg), now) == -1 && nout == 0,
		"to_node: node 0 - a departed slot - is nobody");
	nout = 0;
	chk(clsend_to_node(&PT, 42, (const unsigned char *)msg, strlen(msg), now) == -1 && nout == 0,
		"to_node: an unknown node id gets nothing and -1");

	/* ---- every live peer -------------------------------------------- */
	nout = 0;
	r = clsend_live(&PT, (const unsigned char *)msg, strlen(msg), now);
	chk(r == 2 && nout == 2 && last_octet(0) == 5 && last_octet(1) == 7,
		"live: exactly the two live peers, in table order");
	chk(opens_to(0, K, msg) && opens_to(1, K, msg), "live: each opens to the plaintext");

	/* ---- a transmit that fails -------------------------------------- */
	refuse_last = 7;
	nout = 0;
	chk(clsend_to_node(&PT, 7, (const unsigned char *)msg, strlen(msg), now) == -1,
		"a failed transmit is -1, as seal_send's was");
	nout = 0;
	r = clsend_live(&PT, (const unsigned char *)msg, strlen(msg), now);
	chk(r == 1 && nout == 1 && last_octet(0) == 5,
		"live: one peer failing does not stop the others, and is not counted");
	refuse_last = 0;

	printf("clsendtest: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
