/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * uring.c - S361 (0.6.0): the io_uring layer and its startup check
 * (uring.h).  Raw system calls throughout; the numbers and layouts are
 * the kernel's published ABI, the same on every architecture we build.
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/utsname.h>

#include "uring.h"

#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup     425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter     426
#endif
#ifndef __NR_io_uring_register
#define __NR_io_uring_register  427
#endif

/* setup flags */
#define UR_SETUP_CQSIZE         (1u << 3)
#define UR_SETUP_SUBMIT_ALL     (1u << 7)
#define UR_SETUP_COOP_TASKRUN   (1u << 8)
#define UR_SETUP_SINGLE_ISSUER  (1u << 12)
#define UR_SETUP_DEFER_TASKRUN  (1u << 13)
/* features */
#define UR_FEAT_SINGLE_MMAP     (1u << 0)
#define UR_FEAT_NODROP          (1u << 1)
#define UR_FEAT_SUBMIT_STABLE   (1u << 2)
#define UR_FEAT_FAST_POLL       (1u << 5)
#define UR_FEAT_EXT_ARG         (1u << 8)
#define UR_FEAT_NO_IOWAIT       (1u << 17)  /* 6.15: IORING_FEAT_NO_IOWAIT */
/* opcodes */
#define UR_OP_POLL_ADD          6
#define UR_OP_ASYNC_CANCEL      14
#define UR_OP_SEND              26
#define UR_OP_RECV              27
#define UR_OP_PROVIDE_BUFFERS   31
/* register opcodes */
#define UR_REG_PROBE            8
#define UR_REG_PBUF_RING        22
/* enter flags */
#define UR_ENTER_GETEVENTS      (1u << 0)
#define UR_ENTER_EXT_ARG        (1u << 3)
#define UR_ENTER_NO_IOWAIT      (1u << 7)   /* 6.15: IORING_ENTER_NO_IOWAIT */
/* sqe bits */
#define UR_SQE_BUFFER_SELECT    (1u << 5)
#define UR_RECV_MULTISHOT       (1u << 1)
#define UR_POLL_ADD_MULTI       (1u << 0)
/* mmap offsets */
#define UR_OFF_SQ_RING          0ULL
#define UR_OFF_CQ_RING          0x8000000ULL
#define UR_OFF_SQES             0x10000000ULL

/* the user_data of a classic buffer hand-back */
#define UR_UD_PROVIDE           PC_UR_UD_PROVIDE

struct ur_sq_off { uint32_t head, tail, ring_mask, ring_entries, flags,
	dropped, array, resv1; uint64_t user_addr; };
struct ur_cq_off { uint32_t head, tail, ring_mask, ring_entries,
	overflow, cqes, flags, resv1; uint64_t user_addr; };
struct ur_params { uint32_t sq_entries, cq_entries, flags, sq_thread_cpu,
	sq_thread_idle, features, wq_fd, resv[3];
	struct ur_sq_off sq_off; struct ur_cq_off cq_off; };
struct ur_buf { uint64_t addr; uint32_t len; uint16_t bid, resv; };
struct ur_buf_reg { uint64_t ring_addr; uint32_t ring_entries;
	uint16_t bgid, flags; uint64_t resv[3]; };
struct ur_probe_op { uint8_t op, resv; uint16_t flags; uint32_t resv2; };
struct ur_probe { uint8_t last_op, ops_len; uint16_t resv;
	uint32_t resv2[3]; struct ur_probe_op ops[64]; };
struct ur_getevents_arg { uint64_t sigmask; uint32_t sigmask_sz,
	min_wait_usec; uint64_t ts; };
struct ur_timespec { int64_t tv_sec; long long tv_nsec; };

static int ur_setup(unsigned entries, struct ur_params *p)
{
	return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int ur_enter(int fd, unsigned submit, unsigned wait, unsigned flags,
		const void *arg, size_t argsz)
{
	return (int)syscall(__NR_io_uring_enter, fd, submit, wait, flags, arg,
		argsz);
}

static int ur_register(int fd, unsigned op, void *arg, unsigned n)
{
	return (int)syscall(__NR_io_uring_register, fd, op, arg, n);
}

static const char *kernel_release(void)
{
	static char rel[65];
	struct utsname u;

	if (!rel[0]) {
		if (uname(&u) == 0)
			snprintf(rel, sizeof rel, "%s", u.release);
		else
			snprintf(rel, sizeof rel, "?");
	}
	return rel;
}

static void setup_failed(int e, char *err, size_t errsz)
{
	char d[16] = "?";
	FILE *f;

	switch (e) {
	case ENOSYS:
		snprintf(err, errsz, "io_uring_setup: not implemented - kernel "
			"%s has no io_uring, or a seccomp filter hides it",
			kernel_release());
		break;
	case EPERM:
	case EACCES:
		if ((f = fopen("/proc/sys/kernel/io_uring_disabled", "r"))) {
			if (fscanf(f, "%15s", d) != 1)
				snprintf(d, sizeof d, "?");
			fclose(f);
		}
		snprintf(err, errsz, "io_uring_setup: %s - kernel."
			"io_uring_disabled is %s here, or a container's seccomp "
			"profile or an LSM refuses io_uring", strerror(e), d);
		break;
	case ENOMEM: {
		struct rlimit rl;

		getrlimit(RLIMIT_MEMLOCK, &rl);
		snprintf(err, errsz, "io_uring_setup: %s - before kernel 5.12 "
			"the rings count against RLIMIT_MEMLOCK (%llu bytes "
			"here; kernel %s)", strerror(e),
			(unsigned long long)rl.rlim_cur, kernel_release());
		break;
	}
	case EINVAL:
		snprintf(err, errsz, "io_uring_setup: %s - kernel %s is too "
			"old for this backend (it needs 6.0+)", strerror(e),
			kernel_release());
		break;
	default:
		snprintf(err, errsz, "io_uring_setup: %s (kernel %s)",
			strerror(e), kernel_release());
	}
}

int pc_ur_init(struct pc_ur *r, unsigned entries, unsigned cq_entries,
		char *err, size_t errsz)
{
	/* best first: the flags that let a single-thread ring skip the
	 * kernel's cross-task signalling; each step drops what an older
	 * kernel rejects with EINVAL */
	static const unsigned ladder[] = {
		UR_SETUP_SUBMIT_ALL | UR_SETUP_COOP_TASKRUN |
			UR_SETUP_SINGLE_ISSUER | UR_SETUP_DEFER_TASKRUN,
		UR_SETUP_SUBMIT_ALL | UR_SETUP_COOP_TASKRUN,
		0,
	};
	static const struct { int op; const char *name; } need[] = {
		{ UR_OP_POLL_ADD, "POLL_ADD" },
		{ UR_OP_ASYNC_CANCEL, "ASYNC_CANCEL" },
		{ UR_OP_SEND, "SEND" },
		{ UR_OP_RECV, "RECV" },
		{ UR_OP_PROVIDE_BUFFERS, "PROVIDE_BUFFERS" },
	};
	struct ur_params p;
	struct ur_probe pr;
	unsigned i, want = UR_FEAT_SINGLE_MMAP | UR_FEAT_NODROP |
		UR_FEAT_SUBMIT_STABLE | UR_FEAT_FAST_POLL | UR_FEAT_EXT_ARG;
	int fd = -1, e = 0;

	memset(r, 0, sizeof *r);
	r->fd = -1;
	for (i = 0; i < sizeof ladder / sizeof ladder[0]; i++) {
		memset(&p, 0, sizeof p);
		p.flags = ladder[i] | (cq_entries ? UR_SETUP_CQSIZE : 0);
		p.cq_entries = cq_entries;
		fd = ur_setup(entries, &p);
		if (fd >= 0)
			break;
		e = errno;
		if (e != EINVAL)
			break;
	}
	if (fd < 0) {
		setup_failed(e, err, errsz);
		return -1;
	}
	r->fd = fd;
	r->features = p.features;
	r->setup_flags = p.flags;
	if ((p.features & want) != want) {
		snprintf(err, errsz, "kernel %s lacks io_uring features "
			"0x%x (has 0x%x) - this backend needs 6.0+",
			kernel_release(), want & ~p.features, p.features);
		goto fail;
	}
	memset(&pr, 0, sizeof pr);
	if (ur_register(fd, UR_REG_PROBE, &pr, 64) < 0) {
		snprintf(err, errsz, "IORING_REGISTER_PROBE: %s - kernel %s is "
			"older than 5.6; this backend needs 6.0+",
			strerror(errno), kernel_release());
		goto fail;
	}
	for (i = 0; i < sizeof need / sizeof need[0]; i++)
		if (need[i].op > pr.last_op || !(pr.ops[need[i].op].flags & 1)) {
			snprintf(err, errsz, "kernel %s lacks IORING_OP_%s",
				kernel_release(), need[i].name);
			goto fail;
		}

	r->sq_entries = p.sq_entries;
	r->cq_entries = p.cq_entries;
	r->sq_map_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
	r->cq_map_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct pc_ur_cqe);
	if (r->cq_map_sz > r->sq_map_sz)    /* FEAT_SINGLE_MMAP: one map */
		r->sq_map_sz = r->cq_map_sz;
	r->sq_map = mmap(NULL, r->sq_map_sz, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_POPULATE, fd, UR_OFF_SQ_RING);
	if (r->sq_map == MAP_FAILED) {
		r->sq_map = NULL;
		snprintf(err, errsz, "mmap of the io_uring rings: %s",
			strerror(errno));
		goto fail;
	}
	r->cq_map = r->sq_map;
	r->sqes_sz = p.sq_entries * sizeof(struct pc_ur_sqe);
	r->sqes = mmap(NULL, r->sqes_sz, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_POPULATE, fd, UR_OFF_SQES);
	if (r->sqes == MAP_FAILED) {
		r->sqes = NULL;
		snprintf(err, errsz, "mmap of the io_uring SQEs: %s",
			strerror(errno));
		goto fail;
	}
	r->sq_head = (unsigned *)((char *)r->sq_map + p.sq_off.head);
	r->sq_tail = (unsigned *)((char *)r->sq_map + p.sq_off.tail);
	r->sq_mask = (unsigned *)((char *)r->sq_map + p.sq_off.ring_mask);
	r->sq_array = (unsigned *)((char *)r->sq_map + p.sq_off.array);
	r->cq_head = (unsigned *)((char *)r->cq_map + p.cq_off.head);
	r->cq_tail = (unsigned *)((char *)r->cq_map + p.cq_off.tail);
	r->cq_mask = (unsigned *)((char *)r->cq_map + p.cq_off.ring_mask);
	r->cqes = (struct pc_ur_cqe *)((char *)r->cq_map + p.cq_off.cqes);
	for (i = 0; i < p.sq_entries; i++)  /* slot i is always SQE i */
		r->sq_array[i] = i;
	r->sq_local = *r->sq_tail;
	r->cq_local = *r->cq_head;
	return 0;
fail:
	pc_ur_free(r);
	return -1;
}

void pc_ur_free(struct pc_ur *r)
{
	if (r->fd >= 0)
		close(r->fd);               /* the kernel drops its side */
	if (r->sqes)
		munmap(r->sqes, r->sqes_sz);
	if (r->sq_map)
		munmap(r->sq_map, r->sq_map_sz);
	if (r->br)
		munmap(r->br, r->br_sz);
	free(r->bufs);
	memset(r, 0, sizeof *r);
	r->fd = -1;
}

struct pc_ur_sqe *pc_ur_sqe(struct pc_ur *r)
{
	struct pc_ur_sqe *s;

	if (r->sq_local - __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE) >=
	    r->sq_entries)
		return NULL;
	s = &r->sqes[r->sq_local & *r->sq_mask];
	memset(s, 0, sizeof *s);
	r->sq_local++;
	return s;
}

void pc_ur_prep_recv_multi(struct pc_ur_sqe *s, int fd, uint16_t bgid,
		uint64_t ud)
{
	s->opcode = UR_OP_RECV;
	s->fd = fd;
	s->flags = UR_SQE_BUFFER_SELECT;
	s->ioprio = UR_RECV_MULTISHOT;
	s->buf_group = bgid;
	s->user_data = ud;
}

void pc_ur_prep_poll_multi(struct pc_ur_sqe *s, int fd, uint32_t mask,
		uint64_t ud)
{
	s->opcode = UR_OP_POLL_ADD;
	s->fd = fd;
	s->len = UR_POLL_ADD_MULTI;
	s->op_flags = mask;
	s->user_data = ud;
}

void pc_ur_prep_poll(struct pc_ur_sqe *s, int fd, uint32_t mask, uint64_t ud)
{
	s->opcode = UR_OP_POLL_ADD;
	s->fd = fd;
	s->op_flags = mask;
	s->user_data = ud;
}

void pc_ur_prep_send(struct pc_ur_sqe *s, int fd, const void *buf,
		size_t len, uint64_t ud)
{
	s->opcode = UR_OP_SEND;
	s->fd = fd;
	s->addr = (uint64_t)(uintptr_t)buf;
	s->len = (uint32_t)len;
	s->op_flags = MSG_NOSIGNAL;
	s->user_data = ud;
}

void pc_ur_prep_cancel(struct pc_ur_sqe *s, uint64_t target, uint64_t ud)
{
	s->opcode = UR_OP_ASYNC_CANCEL;
	s->fd = -1;
	s->addr = target;
	s->user_data = ud;
}

int pc_ur_submit(struct pc_ur *r, int wait, int timeout_ms)
{
	struct ur_getevents_arg arg;
	struct ur_timespec ts;
	unsigned n = r->sq_local - *r->sq_tail;
	unsigned flags = UR_ENTER_GETEVENTS;  /* runs deferred task work too */
	int rc;

	/* 0.6.0.1: a waiter with requests pending is counted as in iowait,
	 * and a worker always has some - every connection's multishot
	 * receive - so an idle node read ~97% PSI io pressure and high
	 * iowait on 245-247 (10-12).  It waits for the network, not a disk;
	 * a kernel that can be told so (6.15+) is. */
	if (wait && (r->features & UR_FEAT_NO_IOWAIT))
		flags |= UR_ENTER_NO_IOWAIT;

	pc_ur_cqe_flush(r);
	__atomic_store_n(r->sq_tail, r->sq_local, __ATOMIC_RELEASE);
	if (wait && timeout_ms >= 0) {
		memset(&arg, 0, sizeof arg);
		ts.tv_sec = timeout_ms / 1000;
		ts.tv_nsec = (long long)(timeout_ms % 1000) * 1000000LL;
		arg.ts = (uint64_t)(uintptr_t)&ts;
		rc = ur_enter(r->fd, n, 1, flags | UR_ENTER_EXT_ARG, &arg,
			sizeof arg);
	} else
		rc = ur_enter(r->fd, n, wait ? 1 : 0, flags, NULL, 0);
	return rc < 0 ? -errno : rc;
}

/* ---- provided buffers --------------------------------------------- */

static void ring_add(struct pc_ur *r, uint16_t bid)
{
	struct ur_buf *b = (struct ur_buf *)r->br +
		(r->br_tail & (r->nbufs - 1));

	b->addr = (uint64_t)(uintptr_t)pc_ur_buf(r, bid);
	b->len = r->buf_size;
	b->bid = bid;
	r->br_tail++;
}

static void ring_publish(struct pc_ur *r)
{
	/* the tail shares the first entry's last 16 bits (resv) */
	__atomic_store_n((uint16_t *)((char *)r->br + 14), r->br_tail,
		__ATOMIC_RELEASE);
}

static void classic_provide(struct pc_ur_sqe *s, struct pc_ur *r,
		uint16_t bid, unsigned count)
{
	s->opcode = UR_OP_PROVIDE_BUFFERS;
	s->fd = (int32_t)count;
	s->addr = (uint64_t)(uintptr_t)pc_ur_buf(r, bid);
	s->len = r->buf_size;
	s->off = bid;
	s->buf_group = r->bgid;
	s->user_data = UR_UD_PROVIDE;
}

int pc_ur_bufs_init(struct pc_ur *r, uint16_t bgid, unsigned nbufs,
		unsigned size, char *err, size_t errsz)
{
	struct ur_buf_reg reg;
	struct pc_ur_sqe *s;
	struct pc_ur_cqe *c;
	char ring_why[96];
	unsigned i;
	long pg = sysconf(_SC_PAGESIZE);
	int rc;

	if (nbufs < 1 || nbufs > 32768 || (nbufs & (nbufs - 1))) {
		snprintf(err, errsz, "provided buffers: %u is not a power of "
			"two up to 32768", nbufs);
		return -1;
	}
	r->bgid = bgid;
	r->nbufs = nbufs;
	r->buf_size = size;
	r->bufs = malloc((size_t)nbufs * size);
	if (!r->bufs) {
		snprintf(err, errsz, "provided buffers: out of memory");
		return -1;
	}
	/* resident from the start: one store a page, through volatile - a
	 * memset(0) right after malloc() is folded into calloc(), which on
	 * fresh pages touches nothing (seen on 224: the pool became resident
	 * later, as traffic cycled through the ring) */
	{
		volatile char *b = r->bufs;
		size_t i, len = (size_t)nbufs * size;

		for (i = 0; i < len; i += (size_t)pg)
			b[i] = 0;
	}
	r->br_sz = ((size_t)nbufs * sizeof(struct ur_buf) + (size_t)pg - 1) &
		~((size_t)pg - 1);
	r->br = mmap(NULL, r->br_sz, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (r->br == MAP_FAILED) {
		r->br = NULL;
		snprintf(ring_why, sizeof ring_why, "mmap: %s", strerror(errno));
	} else {
		memset(&reg, 0, sizeof reg);
		reg.ring_addr = (uint64_t)(uintptr_t)r->br;
		reg.ring_entries = nbufs;
		reg.bgid = bgid;
		if (ur_register(r->fd, UR_REG_PBUF_RING, &reg, 1) == 0) {
			for (i = 0; i < nbufs; i++)
				ring_add(r, (uint16_t)i);
			ring_publish(r);
			r->buf_tier = PC_UR_BUF_RING;
			return 0;
		}
		snprintf(ring_why, sizeof ring_why, "%s", strerror(errno));
		munmap(r->br, r->br_sz);
		r->br = NULL;
	}
	/* classic: one PROVIDE_BUFFERS for the lot, and its answer */
	s = pc_ur_sqe(r);
	if (!s) {
		snprintf(err, errsz, "provided buffers: the queue is full");
		return -1;
	}
	classic_provide(s, r, 0, nbufs);
	rc = pc_ur_submit(r, 1, 2000);
	if (rc < 0 && rc != -ETIME) {
		snprintf(err, errsz, "provided buffers: the ring refused "
			"(%s), and so did IORING_OP_PROVIDE_BUFFERS (%s)",
			ring_why, strerror(-rc));
		return -1;
	}
	c = pc_ur_cqe_peek(r);
	if (!c || c->user_data != UR_UD_PROVIDE || c->res < 0) {
		snprintf(err, errsz, "provided buffers: the ring refused "
			"(%s), and so did IORING_OP_PROVIDE_BUFFERS (%s)",
			ring_why, c ? strerror(-c->res) : "no answer");
		return -1;
	}
	pc_ur_cqe_seen(r);
	pc_ur_cqe_flush(r);
	r->buf_tier = PC_UR_BUF_CLASSIC;
	return 0;
}

void pc_ur_buf_return(struct pc_ur *r, uint16_t bid)
{
	struct pc_ur_sqe *s;

	if (r->buf_tier == PC_UR_BUF_RING) {
		ring_add(r, bid);
		ring_publish(r);
		return;
	}
	s = pc_ur_sqe(r);
	if (!s) {                           /* full: send what is queued */
		pc_ur_submit(r, 0, 0);
		s = pc_ur_sqe(r);
		if (!s)
			return;             /* lost to this group until reset */
	}
	classic_provide(s, r, bid, 1);
}

const char *pc_ur_buf_tier_name(int tier)
{
	return tier == PC_UR_BUF_RING ? "buffer ring" :
		tier == PC_UR_BUF_CLASSIC ? "provided buffers" : "none";
}

/* ---- the startup check ------------------------------------------------ */

enum { CK_RECV = 1, CK_POLL, CK_CANCEL_RECV, CK_CANCEL_POLL, CK_SEND };

struct ck {
	char got[32];
	size_t got_len;
	int recv_more, recv_end, recv_err;
	int poll_n, poll_more, poll_end;
	int cancels, cancel_err;
	int send_res, send_done;
};

/* reap what has come back; buffers go straight home */
static void ck_reap(struct pc_ur *r, struct ck *k)
{
	struct pc_ur_cqe *c;

	while ((c = pc_ur_cqe_peek(r)) != NULL) {
		switch (c->user_data) {
		case CK_RECV:
			if (c->res > 0 && (c->flags & PC_UR_CQE_F_BUFFER)) {
				uint16_t bid = PC_UR_CQE_BID(c->flags);
				size_t n = (size_t)c->res;

				if (k->got_len + n <= sizeof k->got) {
					memcpy(k->got + k->got_len,
						pc_ur_buf(r, bid), n);
					k->got_len += n;
				}
				pc_ur_buf_return(r, bid);
			} else if (c->res < 0 && c->res != -ECANCELED)
				k->recv_err = -c->res;
			if (c->flags & PC_UR_CQE_F_MORE)
				k->recv_more = 1;
			else
				k->recv_end = 1;
			break;
		case CK_POLL:
			if (c->res > 0)
				k->poll_n++;
			if (c->flags & PC_UR_CQE_F_MORE)
				k->poll_more = 1;
			else
				k->poll_end = 1;
			break;
		case CK_CANCEL_RECV:
		case CK_CANCEL_POLL:
			k->cancels++;
			if (c->res < 0 && c->res != -EALREADY)
				k->cancel_err = -c->res;
			break;
		case CK_SEND:
			k->send_res = c->res;
			k->send_done = 1;
			break;
		default:                    /* a classic buffer hand-back */
			break;
		}
		pc_ur_cqe_seen(r);
	}
	pc_ur_cqe_flush(r);
}

/* pump until @stage holds or 2 s pass */
static int ck_pump(struct pc_ur *r, struct ck *k, int stage)
{
	struct timespec t0, t;
	int rc;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		ck_reap(r, k);
		switch (stage) {
		case 1: if (k->got_len >= 4 && k->poll_n >= 1) return 0; break;
		case 2: if (k->got_len >= 8) return 0; break;
		case 3: if (k->recv_end && k->poll_end && k->cancels >= 2)
				return 0;
			break;
		case 4: if (k->send_done) return 0; break;
		}
		if (k->recv_err || k->recv_end || k->poll_end)
			if (stage < 3)
				return -1;
		clock_gettime(CLOCK_MONOTONIC, &t);
		if ((t.tv_sec - t0.tv_sec) * 1000 +
		    (t.tv_nsec - t0.tv_nsec) / 1000000 > 2000)
			return -1;
		rc = pc_ur_submit(r, 1, 200);
		if (rc < 0 && rc != -ETIME && rc != -EINTR)
			return -1;
	}
}

static int loopback_pair(int *cfd, int *sfd, char *err, size_t errsz)
{
	struct sockaddr_in a;
	socklen_t al = sizeof a;
	int ls, c = -1, s = -1;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) < 0 ||
	    listen(ls, 1) < 0 || getsockname(ls, (struct sockaddr *)&a, &al) < 0)
		goto fail;
	c = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (c < 0 || connect(c, (struct sockaddr *)&a, sizeof a) < 0)
		goto fail;
	s = accept(ls, NULL, NULL);
	if (s < 0)
		goto fail;
	close(ls);
	*cfd = c;
	*sfd = s;
	return 0;
fail:
	snprintf(err, errsz, "the check's loopback TCP pair: %s",
		strerror(errno));
	if (ls >= 0)
		close(ls);
	if (c >= 0)
		close(c);
	return -1;
}

void pc_uring_check(struct pc_uring_verdict *v)
{
	struct pc_ur r;
	struct pc_ur_sqe *s1, *s2;
	struct ck k;
	const char *deny = getenv("PERFCACHED_URING_DENY");
	char out[8];
	uint64_t one = 1;
	int cfd = -1, sfd = -1, efd = -1, armed = 0;

	memset(v, 0, sizeof *v);
	if (deny && *deny == '1') {
		snprintf(v->why, sizeof v->why, "refused by "
			"PERFCACHED_URING_DENY=1 in the environment (the "
			"fallback's tests)");
		return;
	}
	if (pc_ur_init(&r, 16, 64, v->why, sizeof v->why) < 0)
		return;
	if (pc_ur_bufs_init(&r, 1, 8, 4096, v->why, sizeof v->why) < 0)
		goto out;
	if (loopback_pair(&cfd, &sfd, v->why, sizeof v->why) < 0)
		goto out;
	efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (efd < 0) {
		snprintf(v->why, sizeof v->why, "the check's eventfd: %s",
			strerror(errno));
		goto out;
	}
	memset(&k, 0, sizeof k);

	/* 1: a multishot receive and a multishot poll, both still armed
	 *    after their first completion */
	s1 = pc_ur_sqe(&r);
	s2 = pc_ur_sqe(&r);
	pc_ur_prep_recv_multi(s1, sfd, 1, CK_RECV);
	pc_ur_prep_poll_multi(s2, efd, POLLIN, CK_POLL);
	armed = 1;
	if (write(cfd, "ping", 4) != 4 ||
	    write(efd, &one, sizeof one) != (ssize_t)sizeof one ||
	    ck_pump(&r, &k, 1) < 0 || !k.recv_more || !k.poll_more) {
		snprintf(v->why, sizeof v->why, "multishot receive/poll did "
			"not work (%s) - kernel %s; this backend needs 6.0+",
			k.recv_err ? strerror(k.recv_err) : k.recv_end ?
			"the receive ended" : "no completion within 2 s",
			kernel_release());
		goto out;
	}
	/* 2: the same receive delivers again */
	if (write(cfd, "pong", 4) != 4 || ck_pump(&r, &k, 2) < 0 ||
	    k.got_len != 8 || memcmp(k.got, "pingpong", 8)) {
		snprintf(v->why, sizeof v->why, "the multishot receive did not "
			"deliver a second time (kernel %s)", kernel_release());
		goto out;
	}
	/* 3: cancel both - the backend's pause, park and close rely on it */
	s1 = pc_ur_sqe(&r);
	s2 = pc_ur_sqe(&r);
	pc_ur_prep_cancel(s1, CK_RECV, CK_CANCEL_RECV);
	pc_ur_prep_cancel(s2, CK_POLL, CK_CANCEL_POLL);
	if (ck_pump(&r, &k, 3) < 0 || k.cancel_err) {
		snprintf(v->why, sizeof v->why, "cancelling a multishot request "
			"failed (%s, kernel %s)", k.cancel_err ?
			strerror(k.cancel_err) : "no completion within 2 s",
			kernel_release());
		goto out;
	}
	armed = 0;
	/* 4: a send */
	s1 = pc_ur_sqe(&r);
	pc_ur_prep_send(s1, sfd, "hello", 5, CK_SEND);
	if (ck_pump(&r, &k, 4) < 0 || k.send_res != 5 ||
	    read(cfd, out, sizeof out) != 5 || memcmp(out, "hello", 5)) {
		snprintf(v->why, sizeof v->why, "an io_uring send did not "
			"arrive (%s, kernel %s)", k.send_res < 0 ?
			strerror(-k.send_res) : "short or missing",
			kernel_release());
		goto out;
	}
	v->ok = 1;
	v->buf_tier = r.buf_tier;
	v->setup_flags = r.setup_flags;
	v->counts_iowait = !(r.features & UR_FEAT_NO_IOWAIT);
out:
	/* a receive still armed could land in the buffers after the ring's
	 * teardown, which is asynchronous: on that path they are left be
	 * (32 KB, once, on a host that is falling back anyway) */
	if (armed)
		r.bufs = NULL;
	if (cfd >= 0)
		close(cfd);
	if (sfd >= 0)
		close(sfd);
	if (efd >= 0)
		close(efd);
	pc_ur_free(&r);
}
