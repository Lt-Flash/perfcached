/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * compat.c — implementation half of the OpenSIPS compatibility shim:
 * logging, the futex mutex slow paths, the ticks clock, the broadcast
 * hook, and the per-thread identity registry.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#include "compat.h"
#include "dprint.h"
#include "locking.h"
#include "pt.h"
#include "ipc.h"

/* ---- identity ---------------------------------------------------------- */

__thread int process_no = 0;

void compat_thread_register(int thread_idx)
{
	/* the bucket owner tag packs process_no+1 into 12 bits */
	if (thread_idx < 0 || thread_idx >= 0xFFF) {
		compat_log(L_CRIT, "BUG", "thread index %d outside the 12-bit "
			"owner-tag range\n", thread_idx);
		thread_idx = 0;
	}
	process_no = thread_idx;
}

/* ---- logging ----------------------------------------------------------- */

int compat_log_level = L_INFO;

void compat_log(int level, const char *tag, const char *fmt, ...)
{
	char line[1024];
	struct tm tm;
	time_t now;
	size_t off;
	va_list ap;

	(void)level;
	now = time(NULL);
	localtime_r(&now, &tm);
	off = strftime(line, sizeof line, "%b %e %H:%M:%S ", &tm);
	off += (size_t)snprintf(line + off, sizeof line - off, "[%d] %s: ",
		process_no, tag);
	va_start(ap, fmt);
	/* textbook va_start/vsnprintf/va_end; the checker misfires on the
	 * ternary in the size argument */
	/* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) */
	off += (size_t)vsnprintf(line + off, off < sizeof line ?
		sizeof line - off : 0, fmt, ap);
	va_end(ap);
	if (off >= sizeof line) {
		off = sizeof line - 1;
		line[off - 1] = '\n';
	}
	fwrite(line, 1, off, stderr);
}

/* ---- futex mutex slow paths (fast paths inline in locking.h) ----------- */

static void pc_futex(gen_lock_t *addr, int op, int val)
{
	syscall(SYS_futex, addr, op, val, NULL, NULL, 0);
}

void compat_lock_slow(gen_lock_t *l)
{
	/* three-state futex mutex: mark contended, sleep while not free */
	int prev = __atomic_exchange_n(l, 2, __ATOMIC_ACQUIRE);

	while (prev != 0) {
		pc_futex(l, FUTEX_WAIT_PRIVATE, 2);
		prev = __atomic_exchange_n(l, 2, __ATOMIC_ACQUIRE);
	}
}

void compat_lock_wake(gen_lock_t *l)
{
	pc_futex(l, FUTEX_WAKE_PRIVATE, 1);
}

/* ---- ticks clock ------------------------------------------------------- */

unsigned int compat_ticks_offset = 0;

static struct timespec pc_t0;
static pthread_once_t pc_t0_once = PTHREAD_ONCE_INIT;

static void pc_t0_init(void)
{
	clock_gettime(CLOCK_MONOTONIC_COARSE, &pc_t0);
}

static unsigned long long pc_lamport;

unsigned long long pc_lamport_tick(void)
{
	return __atomic_add_fetch(&pc_lamport, 1, __ATOMIC_ACQ_REL);
}

unsigned long long pc_lamport_now(void)
{
	return __atomic_load_n(&pc_lamport, __ATOMIC_ACQUIRE);
}

/* How far ahead of us a peer may legitimately be.  The fleet converges
 * over a 1 Hz heartbeat, so a peer can only have out-run us by roughly
 * (its write rate x the gap since we last heard it) - about 2M per
 * second at the measured peak.  A billion is orders of magnitude beyond
 * any real gap while still leaving a 64-bit counter unreachable.
 *
 * This is the bound that matters, not wrap.  Counting writes to 2^64
 * takes ~300,000 years at peak, but observe() accepting whatever a peer
 * advertises would let ONE bad heartbeat - a bug, a corrupted field,
 * uninitialised memory, a hostile peer inside the cluster PSK - jump
 * every node to an absurd value in a single round, permanently.  A
 * refused jump is counted, never silent: a real one means a defect. */
#define PC_LAMPORT_MAX_JUMP 1000000000ULL

unsigned long long pc_lamport_rejected;

/* fold in a value seen from a peer: the clock only ever moves forward,
 * and a CAS loop rather than a plain store so two workers observing
 * different peers cannot lose the larger one */
void pc_lamport_restore(unsigned long long high)
{
	unsigned long long cur = __atomic_load_n(&pc_lamport, __ATOMIC_ACQUIRE);

	while (high > cur) {
		if (__atomic_compare_exchange_n(&pc_lamport, &cur, high, 0,
		        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			return;
	}
}

void pc_lamport_observe(unsigned long long seen)
{
	unsigned long long cur = __atomic_load_n(&pc_lamport,
		__ATOMIC_ACQUIRE);

	if (seen > cur + PC_LAMPORT_MAX_JUMP) {
		__atomic_add_fetch(&pc_lamport_rejected, 1, __ATOMIC_RELAXED);
		return;
	}
	while (seen > cur) {
		if (__atomic_compare_exchange_n(&pc_lamport, &cur, seen, 0,
		        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			return;
		/* cur was reloaded with the current value; retry */
	}
}

unsigned int compat_ticks(void)
{
	struct timespec now;

	pthread_once(&pc_t0_once, pc_t0_init);
	clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
	return (unsigned int)(now.tv_sec - pc_t0.tv_sec) + compat_ticks_offset;
}

/* ---- hoard-flush broadcast --------------------------------------------- */

static void (*pc_bcast)(void (*fn)(int, void *), void *param);

void compat_set_broadcast(void (*bcast)(void (*fn)(int sender, void *param),
		void *param))
{
	pc_bcast = bcast;
}

int ipc_send_rpc_all(ipc_rpc_f fn, void *param)
{
	if (pc_bcast)
		pc_bcast(fn, param);
	else
		fn(process_no, param);   /* single-threaded fallback: just us */
	return 0;
}

