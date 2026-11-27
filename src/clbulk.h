/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clbulk.h - the bulk TCP plane's framed record stream and batch handoff;
 * connecting, the handshake and applying records stay in cluster.c. */
#ifndef PC_CLBULK_H
#define PC_CLBULK_H

#include <netinet/in.h>
#include <pthread.h>
#include <stddef.h>

#include "pc_noise.h"                  /* struct pc_cipherstate */
#include "pcaddr.h"

/* a slot per batch: a sweep offers one per peer in the same tick; small
 * enough that a lagging bulk thread still applies back-pressure */
#define CLBULK_SLOTS  4

struct clbulk_batch {
	unsigned char *buf;            /* owned by the queue while queued */
	size_t len;
	unsigned int recs;
	int kind;
	pc_addr to;
};

struct clbulk {
	pthread_mutex_t mx;
	struct clbulk_batch q[CLBULK_SLOTS];
	unsigned int head;             /* next to take */
	unsigned int n;                /* queued */
	struct clbulk_batch cur;       /* the one the bulk thread holds */
};

/* a move makes the receiver the owner; a replica is stored PASSIVE, off the
 * WAL.  An older peer reads only the version byte and treats it as a move. */
#define CLBULK_KIND_MIGRATE  0
#define CLBULK_KIND_REPLICA  1
/* a staged build: an older receiver refuses CLBULK_STAGE_MAGIC as a count
 * and closes; offered only to a member advertising CLMEMB_FEAT_STAGE */
#define CLBULK_KIND_STAGE    2
#define CLBULK_STAGE_MAGIC   0xFFFFFFFEu

void clbulk_init(struct clbulk *b);
void clbulk_fini(struct clbulk *b);

/* the bulk plane's lock, lent to the planes that share it */
pthread_mutex_t *clbulk_lock(struct clbulk *b);

/* 1 = queued and OWNED by the queue; 0 = every slot full, the caller keeps
 * its buffer and the cycle did not deliver.  Peer thread. */
int clbulk_offer(struct clbulk *b, unsigned char *buf, size_t len,
		unsigned int recs, int kind, const pc_addr *to);

/* Any thread. */
int clbulk_pending(struct clbulk *b);
/* 1 = a slot is free now, so a large batch is worth building; any thread */
int clbulk_room(struct clbulk *b);

/* 0 when there is none; the caller frees the buffer, then calls
 * clbulk_done().  Bulk thread. */
int clbulk_take(struct clbulk *b, unsigned char **buf, size_t *len,
		unsigned int *recs, int *kind, pc_addr *to);
void clbulk_done(struct clbulk *b);

/* the framed record stream: fd and cipherstate already handshaked by the
 * caller; nothing here opens or closes a socket */

/* read or write exactly n bytes, retrying a short transfer.  0 or -1. */
int clbulk_io(int fd, int wr, void *buf, size_t n);

/* [len u16][ciphertext] chunks, each at most PC_NOISE_MAXPT of plaintext.
 * 0 or -1. */
int clbulk_send(int fd, struct pc_cipherstate *cs, const unsigned char *p,
		size_t n);

/* the remainder carries in `stash`; a chunk short of the tag, over a Noise
 * message or the stash is refused (truncated batch by exact length) */
int clbulk_recv_exact(int fd, struct pc_cipherstate *cs, unsigned char *stash,
		size_t *slen, size_t scap, unsigned char *out, size_t want);

/* boot request, the first bulk frame: [kind1][node2][pad1]; pad sent as 0,
 * not read, kept because an older peer sends four bytes */
#define CLBULK_BOOTREQ_LEN  4

size_t clbulk_bootreq_write(unsigned char *buf, size_t cap, int kind,
		int node);
/* Returns 0 unless four bytes are present. */
int clbulk_bootreq_parse(const unsigned char *p, size_t n, int *kind,
		int *node);

/* handshake frames: msg1 [len2][principal1][noise], len = 1 + noise length;
 * msg2 [len2][noise].  Payload written in place, length patched after. */
#define CLBULK_LEN_PREFIX  2       /* the LE length itself */
#define CLBULK_HS1_AT      3       /* msg1's payload: after len + principal */
#define CLBULK_HS2_AT      2       /* msg2's payload: after len */

/* patch the prefix once @paylen bytes sit at CLBULK_HS1_AT / CLBULK_HS2_AT;
 * both return the whole frame's length */
size_t clbulk_hs1_frame(unsigned char *buf, size_t cap, int principal,
		size_t paylen);
size_t clbulk_hs2_frame(unsigned char *buf, size_t cap, size_t paylen);

/* the 2-byte LE length off the front of a frame */
size_t clbulk_len_get(const unsigned char *p);

#endif /* PC_CLBULK_H */
