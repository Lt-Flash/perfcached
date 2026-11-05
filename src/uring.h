/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * uring.h - S361 (0.6.0): the io_uring layer the worker backend is built
 * on, and the startup check that decides whether it is used at all.
 *
 * Raw system calls - no liburing, no <linux/io_uring.h> - so it builds on
 * every target the releases do (old kernel headers, musl) and adds no
 * dependency.  What a host can actually run is decided at RUN time by
 * pc_uring_check(): it does the real thing on a loopback TCP pair (a
 * multishot receive into provided buffers, a cancel, a multishot poll, a
 * send) rather than trusting a kernel version - Ubuntu's 6.8.0-136
 * refuses buffer rings that 5.19 introduced, a container's seccomp
 * profile can refuse io_uring_setup itself, kernel.io_uring_disabled can
 * turn it off.  Whatever fails is the reason the daemon falls back to
 * epoll, and it is reported as such.
 *
 * Buffer tiers, best first:
 *   PC_UR_BUF_RING     a registered provided-buffer ring (5.19+)
 *   PC_UR_BUF_CLASSIC  IORING_OP_PROVIDE_BUFFERS (5.7+), where the ring
 *                      is refused
 * The backend needs multishot receive either way (6.0+).
 */
#ifndef PC_URING_H
#define PC_URING_H

#include <stddef.h>
#include <stdint.h>

/* the kernel ABI, as published in linux/io_uring.h */
struct pc_ur_sqe {
	uint8_t opcode, flags;
	uint16_t ioprio;
	int32_t fd;
	uint64_t off, addr;
	uint32_t len, op_flags;
	uint64_t user_data;
	uint16_t buf_group, personality;
	int32_t file_index;
	uint64_t addr3, pad;
};
struct pc_ur_cqe {
	uint64_t user_data;
	int32_t res;
	uint32_t flags;
};

#define PC_UR_CQE_F_BUFFER  (1u << 0)
#define PC_UR_CQE_F_MORE    (1u << 1)
#define PC_UR_CQE_BID(f)    ((uint16_t)((f) >> 16))

enum { PC_UR_BUF_NONE = 0, PC_UR_BUF_RING, PC_UR_BUF_CLASSIC };

/* the worker's tags (a connection's are its address | an op, >= 65536):
 * what the backend never dispatches - cancels, classic buffer
 * hand-backs - and the worker's own eventfd and listeners */
#define PC_UR_UD_IGNORE     1ULL
#define PC_UR_UD_EFD        2ULL
#define PC_UR_UD_LISTEN(j)  (16ULL + (uint64_t)(j))
#define PC_UR_UD_PROVIDE    (~0ULL)
#define PC_UR_BGID          0          /* the worker's buffer group */
/* a worker's receive buffers: 512 x 4 KB, 2 MB.  A receive takes one per
 * completion and the worker hands it back as it copies it out; when all
 * are out the receive ends with ENOBUFS and is armed again - the bytes
 * wait in the socket meanwhile.  Many small buffers suit many connections
 * each sending small requests (a large value just arrives in more
 * pieces).  The pool is touched at start: a buffer RING hands buffers out
 * in turn, so under load every page of it becomes resident anyway - first
 * 256 x 16 KB crept to 4 MB a worker during idlebuftest on 224 - and a
 * fixed cost paid at start is one an operator can see and plan for. */
#define PC_UR_WORKER_BUFS     512
#define PC_UR_WORKER_BUF_SIZE 4096

struct pc_ur {
	int fd;
	unsigned features, setup_flags;
	unsigned sq_entries, cq_entries;
	unsigned *sq_tail, *sq_mask, *sq_array, *sq_head;
	unsigned *cq_head, *cq_tail, *cq_mask;
	struct pc_ur_cqe *cqes;
	struct pc_ur_sqe *sqes;
	unsigned sq_local;                  /* prepared, not yet published */
	unsigned cq_local;                  /* consumed, not yet published */
	void *sq_map, *cq_map;
	size_t sq_map_sz, cq_map_sz, sqes_sz;
	/* provided buffers: one group per ring */
	int buf_tier;
	uint16_t bgid;
	unsigned nbufs, buf_size;
	char *bufs;
	void *br;                           /* PC_UR_BUF_RING: the ring */
	size_t br_sz;
	uint16_t br_tail;
};

/* Set up a ring of @entries submissions (completions: @cq_entries, 0 for
 * the kernel's default).  0, or -1 with @err filled - the reason in words
 * an operator can act on. */
int pc_ur_init(struct pc_ur *r, unsigned entries, unsigned cq_entries,
		char *err, size_t errsz);
void pc_ur_free(struct pc_ur *r);

/* @nbufs buffers of @size bytes as buffer group @bgid: a ring where the
 * kernel takes one, classic provided buffers where it refuses.  0 or -1. */
int pc_ur_bufs_init(struct pc_ur *r, uint16_t bgid, unsigned nbufs,
		unsigned size, char *err, size_t errsz);
static inline char *pc_ur_buf(const struct pc_ur *r, uint16_t bid)
{
	return r->bufs + (size_t)bid * r->buf_size;
}
/* hand buffer @bid back to the kernel (ring: at once; classic: an SQE
 * that goes out with the next submit) */
void pc_ur_buf_return(struct pc_ur *r, uint16_t bid);

/* A zeroed SQE to fill, or NULL when the queue is full (submit first). */
struct pc_ur_sqe *pc_ur_sqe(struct pc_ur *r);
void pc_ur_prep_recv_multi(struct pc_ur_sqe *s, int fd, uint16_t bgid,
		uint64_t ud);
void pc_ur_prep_poll_multi(struct pc_ur_sqe *s, int fd, uint32_t mask,
		uint64_t ud);
void pc_ur_prep_poll(struct pc_ur_sqe *s, int fd, uint32_t mask, uint64_t ud);
void pc_ur_prep_send(struct pc_ur_sqe *s, int fd, const void *buf,
		size_t len, uint64_t ud);
void pc_ur_prep_cancel(struct pc_ur_sqe *s, uint64_t target, uint64_t ud);

/* Publish what was prepared and, with @wait, block until a completion
 * arrives or @timeout_ms passes (-1: no limit).  The number submitted,
 * or -errno (-ETIME on a timeout with nothing submitted, -EINTR). */
int pc_ur_submit(struct pc_ur *r, int wait, int timeout_ms);

/* Completions: peek the next, then mark it seen.  The CQ head is
 * published by pc_ur_cqe_flush() (pc_ur_submit does it too). */
static inline struct pc_ur_cqe *pc_ur_cqe_peek(struct pc_ur *r)
{
	if (r->cq_local == __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE))
		return NULL;
	return &r->cqes[r->cq_local & *r->cq_mask];
}
static inline void pc_ur_cqe_seen(struct pc_ur *r)
{
	r->cq_local++;
}
static inline void pc_ur_cqe_flush(struct pc_ur *r)
{
	__atomic_store_n(r->cq_head, r->cq_local, __ATOMIC_RELEASE);
}

/* ---- the startup check ---------------------------------------------- */
struct pc_uring_verdict {
	int ok;                             /* 1: the backend can run here */
	int buf_tier;                       /* PC_UR_BUF_* when ok */
	unsigned setup_flags;               /* the ring flags the kernel took */
	int counts_iowait;                  /* 0.6.0.1: 1 = this kernel (before
	                                     * 6.15) counts a ring wait as iowait */
	char why[200];                      /* when !ok: the reason */
};
/* The functional check (see the top of this file).  Never fails the
 * caller: the verdict says what to do.  PERFCACHED_URING_DENY=1 in the
 * environment makes it refuse - for the fallback's tests. */
void pc_uring_check(struct pc_uring_verdict *v);
const char *pc_ur_buf_tier_name(int tier);

#endif /* PC_URING_H */
