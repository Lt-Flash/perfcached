/* clinject.c - test tool: sends one cluster frame, sealed the way a node seals it, from an address
 * of the suite's choosing, so a suite can play a sender the fleet does not know. */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sodium.h>
#include "pcaddr.h"
#include "pc_noise.h"
#include "cluster.h"
#include "clmsg.h"
#include "clwire.h"
#include "clmemb.h"
#include "clpull.h"
#include "clfwd.h"

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int usage(void)
{
	fprintf(stderr, "usage: clinject -s SECRET -c CLUSTER_ID -f FROM_IP:PORT "
		"-t TO_IP:PORT [-n COUNT] [-i GAP_MS] [-w WAIT_MS] raw HEX | pullreq COL "
		"KEY | joinreq | alive NODE [IDENT] | malive NODE MEMBERS TERM | fwdadd COL KEY "
		"DELTA\n");
	return 2;
}

static int hostport(const char *s, pc_addr *a)
{
	char h[64];
	const char *c = strrchr(s, ':');

	if (!c || (size_t)(c - s) >= sizeof h)
		return -1;
	memcpy(h, s, (size_t)(c - s));
	h[c - s] = 0;
	return pc_addr_parse(h, (uint16_t)atoi(c + 1), a);
}

static int unhex(const char *s, unsigned char *out, size_t cap)
{
	size_t n = 0;

	while (*s) {
		unsigned v;

		if (*s == '-') {
			s++;
			continue;
		}
		if (n >= cap || sscanf(s, "%2x", &v) != 1)
			return -1;
		out[n++] = (unsigned char)v;
		s += 2;
	}
	return (int)n;
}

int main(int argc, char **argv)
{
	const char *secret = NULL, *cidhex = NULL, *froms = NULL, *tos = NULL;
	unsigned char cid[CLWIRE_CID_LEN], pt[2048], sealed[HDR_LEN + 2048 + CLWIRE_TAG];
	unsigned char psk[32], ident[16];
	pc_addr from, to;
	size_t n = 0, sl;
	int count = 1, gap_ms = 0, wait_ms = 0, i, fd, opt, got = 0;

	while ((opt = getopt(argc, argv, "s:c:f:t:n:i:w:")) != -1)
		switch (opt) {
		case 's': secret = optarg; break;
		case 'c': cidhex = optarg; break;
		case 'f': froms = optarg; break;
		case 't': tos = optarg; break;
		case 'n': count = atoi(optarg); break;
		case 'i': gap_ms = atoi(optarg); break;
		case 'w': wait_ms = atoi(optarg); break;
		default: return usage();
		}
	if (!secret || !cidhex || !froms || !tos || optind >= argc ||
	    hostport(froms, &from) != 0 || hostport(tos, &to) != 0 ||
	    unhex(cidhex, cid, sizeof cid) != CLWIRE_CID_LEN)
		return usage();
	if (sodium_init() < 0 ||
	    pc_psk_derive(secret, strlen(secret), PC_PRIN_CLUSTER, psk) != 0)
		return 2;
	randombytes_buf(ident, sizeof ident);

	if (!strcmp(argv[optind], "raw") && optind + 1 < argc) {
		int r = unhex(argv[optind + 1], pt, sizeof pt);

		if (r <= 0)
			return usage();
		n = (size_t)r;
	} else if (!strcmp(argv[optind], "pullreq") && optind + 2 < argc) {
		const char *col = argv[optind + 1], *key = argv[optind + 2];

		if (clpull_req_size(strlen(col), strlen(key)) > sizeof pt)
			return usage();
		n = clpull_req_frame(pt, 0x5eed, 0, col, strlen(col), key, strlen(key));
	} else if (!strcmp(argv[optind], "joinreq")) {
		struct clmemb_joinreq j;

		memset(&j, 0, sizeof j);
		randombytes_buf(&j.tok, sizeof j.tok);
		j.ident = ident;
		randombytes_buf(&j.incarn, sizeof j.incarn);
		j.incarn |= 1;
		j.wal = 1;
		j.epoch = PC_CL_EPOCH;
		n = clmemb_joinreq_write(pt, sizeof pt, M_JOIN_REQ, &j);
	} else if (!strcmp(argv[optind], "alive") && optind + 1 < argc) {
		struct clmemb_alive a;

		memset(&a, 0, sizeof a);
		a.node = atoi(argv[optind + 1]);
		if (optind + 2 < argc && unhex(argv[optind + 2], ident, sizeof ident) != 16)
			return usage();
		a.ident = ident;
		randombytes_buf(&a.incarn, sizeof a.incarn);
		a.incarn |= 1;
		a.nstate = PC_NST_READY;
		a.epoch = PC_CL_EPOCH;
		n = clmemb_alive_write(pt, sizeof pt, M_ALIVE, &a);
	} else if (!strcmp(argv[optind], "malive") && optind + 3 < argc) {
		struct clmemb_malive m;

		memset(&m, 0, sizeof m);
		m.node = atoi(argv[optind + 1]);
		m.members = atoi(argv[optind + 2]);
		m.term = (uint32_t)strtoul(argv[optind + 3], NULL, 10);
		m.epoch = PC_CL_EPOCH;
		m.committed = PC_CL_EPOCH;
		n = clmemb_malive_write(pt, sizeof pt, M_MASTER_ALIVE, &m);
	} else if (!strcmp(argv[optind], "fwdadd") && optind + 3 < argc) {
		const char *col = argv[optind + 1], *key = argv[optind + 2];

		n = clfwd_op_frame(pt, 0x5eed, 0, 2, 0, atoll(argv[optind + 3]),
			col, strlen(col), key, strlen(key), NULL, 0);
	} else
		return usage();
	if (!n)
		return usage();

	clwire_set_cid(cid, CLWIRE_CID_ESTABLISHED);
	clwire_set_self(from.in4.sin_addr.s_addr, ntohs(from.in4.sin_port));
	sl = clwire_seal(psk, pt, n, sealed);
	fd = socket(pc_addr_family(&from), SOCK_DGRAM, 0);
	if (fd < 0 || bind(fd, &from.sa, pc_addr_len(&from)) != 0) {
		fprintf(stderr, "clinject: bind %s: %s\n", froms, strerror(errno));
		return 1;
	}
	for (i = 0; i < count; i++) {
		if (i && gap_ms) {
			poll(NULL, 0, gap_ms);
			sl = clwire_seal(psk, pt, n, sealed);
		}
		if (sendto(fd, sealed, sl, 0, &to.sa, pc_addr_len(&to)) < 0)
			fprintf(stderr, "clinject: send: %s\n", strerror(errno));
	}
	printf("sent %d\n", count);
	if (wait_ms > 0) {
		struct pollfd p = { fd, POLLIN, 0 };
		long long end = mono_ms() + wait_ms, left;

		/* WAIT_MS in all, not per reply: closely spaced replies would hold it open */
		while ((left = end - mono_ms()) > 0 && poll(&p, 1, (int)left) == 1) {
			unsigned char buf[HDR_LEN + MAX_DGRAM + 64], rp[MAX_DGRAM + 16];
			unsigned long long rl = 0;
			ssize_t r = recv(fd, buf, sizeof buf, 0);

			if (r <= 0)
				break;
			if (clwire_open(psk, buf, (size_t)r, rp, &rl) == 0 && rl) {
				printf("reply %u %llu\n", rp[0], rl);
				got++;
			}
		}
		printf("replies %d\n", got);
	}
	close(fd);
	return 0;
}
