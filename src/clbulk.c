/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clbulk.c - the bulk plane's framing and batch handoff (M7).  See
 * clbulk.h.  The bodies came out of bulk_io(), bulk_send_stream(),
 * bulk_recv_exact() and run_migration()'s handoff unchanged; what
 * differs is that the queue is reached through a pointer and the
 * "channel busy" case is REPORTED rather than logged here.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "clcodec.h"
#include "clbulk.h"

void clbulk_init(struct clbulk *b)
{
	memset(b, 0, sizeof *b);
	pthread_mutex_init(&b->mx, NULL);
}

void clbulk_fini(struct clbulk *b)
{
	free(b->tx);
	b->tx = NULL;
	pthread_mutex_destroy(&b->mx);
}

pthread_mutex_t *clbulk_lock(struct clbulk *b)
{
	return &b->mx;
}

int clbulk_offer(struct clbulk *b, unsigned char *buf, size_t len,
		unsigned int recs, const struct sockaddr_in *to)
{
	int queued = 0;

	pthread_mutex_lock(&b->mx);
	if (!b->tx) {
		b->tx = buf;
		b->tx_len = len;
		b->tx_recs = recs;
		b->tx_to = *to;
		queued = 1;
	}
	pthread_mutex_unlock(&b->mx);
	return queued;
}

int clbulk_pending(struct clbulk *b)
{
	int p;

	pthread_mutex_lock(&b->mx);
	p = b->tx != NULL;
	pthread_mutex_unlock(&b->mx);
	return p;
}

int clbulk_take(struct clbulk *b, unsigned char **buf, size_t *len,
		unsigned int *recs, struct sockaddr_in *to)
{
	pthread_mutex_lock(&b->mx);
	*buf = b->tx;
	*len = b->tx_len;
	*recs = b->tx_recs;
	*to = b->tx_to;
	pthread_mutex_unlock(&b->mx);
	return *buf != NULL;
}

void clbulk_done(struct clbulk *b)
{
	pthread_mutex_lock(&b->mx);
	b->tx = NULL;
	pthread_mutex_unlock(&b->mx);
}

int clbulk_io(int fd, int wr, void *buf, size_t n)
{
	size_t off = 0;

	while (off < n) {
		ssize_t r = wr ? write(fd, (char *)buf + off, n - off)
		               : read(fd, (char *)buf + off, n - off);

		if (r <= 0) {
			if (r < 0 && errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)r;
	}
	return 0;
}

int clbulk_send(int fd, struct pc_cipherstate *cs, const unsigned char *p,
		size_t n)
{
	while (n) {
		unsigned char rec[2 + PC_NOISE_MAXPT + PC_NOISE_TAGLEN];
		size_t chunk = n > PC_NOISE_MAXPT ? PC_NOISE_MAXPT : n;
		int cl = pc_transport_encrypt(cs, p, chunk, rec + 2);

		if (cl < 0)
			return -1;
		pc_p16(rec, (uint16_t)cl);
		if (clbulk_io(fd, 1, rec, 2 + (size_t)cl) != 0)
			return -1;
		p += chunk;
		n -= chunk;
	}
	return 0;
}

int clbulk_recv_exact(int fd, struct pc_cipherstate *cs, unsigned char *stash,
		size_t *slen, size_t scap, unsigned char *out, size_t want)
{
	size_t got = 0;

	for (;;) {
		size_t take = *slen < want - got ? *slen : want - got;

		memcpy(out + got, stash, take);
		memmove(stash, stash + take, *slen - take);
		*slen -= take;
		got += take;
		if (got == want)
			return 0;
		{
			unsigned char hdr[2], ct[PC_NOISE_MAXMSG];
			size_t cl;
			int pl;

			if (clbulk_io(fd, 0, hdr, 2) != 0)
				return -1;
			cl = pc_g16(hdr);
			if (cl < PC_NOISE_TAGLEN || cl > PC_NOISE_MAXMSG ||
			        *slen + cl > scap ||
			        clbulk_io(fd, 0, ct, cl) != 0)
				return -1;
			pl = pc_transport_decrypt(cs, ct, cl, stash + *slen);
			if (pl < 0)
				return -1;
			*slen += (size_t)pl;
		}
	}
}

/* ---- the boot request.  See clbulk.h. ----------------------------- */

size_t clbulk_bootreq_write(unsigned char *buf, size_t cap, int kind,
		int node)
{
	struct pc_wcur c;

	pc_wcur_init(&c, buf, cap);
	pc_w8(&c, (unsigned)kind);
	pc_w16(&c, (uint16_t)node);
	pc_w8(&c, 0);                      /* pad: sent, never read */
	return c.ok ? pc_wcur_len(&c, buf) : 0;
}

int clbulk_bootreq_parse(const unsigned char *p, size_t n, int *kind,
		int *node)
{
	struct pc_rcur c;

	pc_rcur_init(&c, p, n);
	if (!pc_rfits(&c, CLBULK_BOOTREQ_LEN))
		return 0;
	*kind = pc_ru8(&c);
	*node = pc_ru16(&c);
	return 1;
}

/* ---- the handshake framing.  See clbulk.h. ------------------------ */

size_t clbulk_hs1_frame(unsigned char *buf, size_t cap, int principal,
		size_t paylen)
{
	struct pc_wcur c;

	if (CLBULK_HS1_AT + paylen > cap)
		return 0;
	pc_wcur_init(&c, buf, CLBULK_HS1_AT);
	pc_w16(&c, (uint16_t)(1 + paylen));   /* the principal is inside it */
	pc_w8(&c, (unsigned)principal);
	return c.ok ? CLBULK_HS1_AT + paylen : 0;
}

size_t clbulk_hs2_frame(unsigned char *buf, size_t cap, size_t paylen)
{
	struct pc_wcur c;

	if (CLBULK_HS2_AT + paylen > cap)
		return 0;
	pc_wcur_init(&c, buf, CLBULK_HS2_AT);
	pc_w16(&c, (uint16_t)paylen);
	return c.ok ? CLBULK_HS2_AT + paylen : 0;
}

size_t clbulk_len_get(const unsigned char *p)
{
	return pc_g16(p);
}
