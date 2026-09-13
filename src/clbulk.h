/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clbulk.h - the bulk TCP plane's framed record stream and the batch
 * handoff (M7).
 *
 * Records above the datagram ceiling travel over a cluster-principal
 * Noise session on advertise:port/TCP.  This module owns the two things
 * that are the same for every consumer of that session: the framing -
 * [len u16][ciphertext] chunks carrying a plaintext byte stream - and
 * the queue by which the peer thread hands a batch to the bulk thread.
 *
 * What is deliberately NOT here: connecting, the handshake, and
 * applying records.  bulk_pull_from, bulk_tx, bulk_rx and
 * bulk_serve_boot each open a socket, drive pc_hs_*, and write into the
 * store and the WAL; under rule 4 none of that can live in a module
 * that links alone for its test.  They keep the framing calls and stay
 * in cluster.c as orchestration.  That makes M7 smaller than
 * doc/MODULARITY.md sketches - about 110 lines rather than 480 - and
 * the residue is orchestration, not unfinished work.
 *
 * The lock here IS the bulk plane's: C.bmx guarded this handoff and the
 * migration producer alike, so clbulk owns it and lends it out.  clboot
 * borrowed it as an unowned pointer in M4; it now borrows it from here,
 * and clmig will do the same in wave 3.
 */
#ifndef PC_CLBULK_H
#define PC_CLBULK_H

#include <netinet/in.h>
#include <pthread.h>
#include <stddef.h>

#include "pc_noise.h"                  /* struct pc_cipherstate */

/* The batch handoff: the peer thread OFFERS a stubbed batch, the bulk
 * thread TAKES it, sends it, and says DONE.  One batch in flight. */
struct clbulk {
	pthread_mutex_t mx;
	unsigned char *tx;             /* owned by the queue while queued */
	size_t tx_len;
	unsigned int tx_recs;
	struct sockaddr_in tx_to;
};

void clbulk_init(struct clbulk *b);
void clbulk_fini(struct clbulk *b);

/* The lock, for the planes that share it - clboot's handoff today,
 * clmig's producer in wave 3.  Owning it here is what lets those stop
 * passing an unowned mutex around. */
pthread_mutex_t *clbulk_lock(struct clbulk *b);

/* Offer a batch.  1 = queued and OWNED by the queue from now on; 0 =
 * the bulk thread is still on the last one, so the caller keeps its
 * buffer and counts the records as lost.  Peer thread. */
int clbulk_offer(struct clbulk *b, unsigned char *buf, size_t len,
		unsigned int recs, const struct sockaddr_in *to);

/* Is a batch waiting?  Any thread. */
int clbulk_pending(struct clbulk *b);

/* Take the queued batch; 0 when there is none.  The caller frees the
 * buffer and then calls clbulk_done().  Bulk thread. */
int clbulk_take(struct clbulk *b, unsigned char **buf, size_t *len,
		unsigned int *recs, struct sockaddr_in *to);
void clbulk_done(struct clbulk *b);

/* ---- the framed record stream ---------------------------------------
 * Every entry point works on an fd and a cipherstate the caller has
 * already handshaked; nothing here opens or closes a socket. */

/* read or write exactly n bytes, retrying a short transfer.  0 or -1. */
int clbulk_io(int fd, int wr, void *buf, size_t n);

/* Send a plaintext byte stream as [len u16][ciphertext] chunks, each at
 * most PC_NOISE_MAXPT of plaintext.  0 or -1. */
int clbulk_send(int fd, struct pc_cipherstate *cs, const unsigned char *p,
		size_t n);

/* Read exactly `want` plaintext bytes out of the chunk stream, carrying
 * the remainder in `stash` for the next call.  A chunk whose declared
 * length is short of the tag, longer than a Noise message, or would
 * overrun the stash is refused - this is the "truncated batch refused
 * by exact length" the specification asks for.  0 or -1. */
int clbulk_recv_exact(int fd, struct pc_cipherstate *cs, unsigned char *stash,
		size_t *slen, size_t scap, unsigned char *out, size_t want);

/* ---- the boot request -----------------------------------------------
 *
 * The first frame over an established bulk channel: which kind of
 * transfer, and who is asking.
 *
 *     [kind1][node2][pad1]
 *
 * Four bytes, built at one end of cluster.c and read at the other, with
 * the node's offset written out in both.  The pad byte is sent as 0 and
 * not read; it is kept because a peer on an older build sends four.
 */
#define CLBULK_BOOTREQ_LEN  4

size_t clbulk_bootreq_write(unsigned char *buf, size_t cap, int kind,
		int node);
/* Returns 0 unless four bytes are present. */
int clbulk_bootreq_parse(const unsigned char *p, size_t n, int *kind,
		int *node);

/* ---- the handshake framing -------------------------------------------
 *
 * Before the cipher is up, the bulk channel frames each handshake
 * message with a 2-byte little-endian length.  The INITIATOR's first
 * message also carries a principal byte, and the length COVERS it:
 *
 *     msg1   [len2][principal1][noise msg1]     len = 1 + noise length
 *     msg2   [len2][noise msg2]                 len = noise length
 *
 * The noise library writes its payload in place, so these reserve the
 * prefix and patch the length afterwards - the same shape as clsync's
 * envelope, and the reason this is offsets-and-lengths rather than a
 * cursor walk.
 *
 * It was spelled out three times - bulk_pull_from, bulk_tx and bulk_rx -
 * and the difference between the two shapes (whether the principal is
 * inside the length) is exactly the sort of thing that survives being
 * copied and then diverges.
 */
#define CLBULK_LEN_PREFIX  2       /* the LE length itself */
#define CLBULK_HS1_AT      3       /* msg1's payload: after len + principal */
#define CLBULK_HS2_AT      2       /* msg2's payload: after len */

/* Patch the prefix once the payload of @paylen bytes is in place at
 * CLBULK_HS1_AT / CLBULK_HS2_AT.  Both return the whole frame's length,
 * which is what goes on the wire. */
size_t clbulk_hs1_frame(unsigned char *buf, size_t cap, int principal,
		size_t paylen);
size_t clbulk_hs2_frame(unsigned char *buf, size_t cap, size_t paylen);

/* the 2-byte LE length off the front of a frame */
size_t clbulk_len_get(const unsigned char *p);

#endif /* PC_CLBULK_H */
