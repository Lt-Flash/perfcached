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
static int pc_t0_ready;

static void pc_t0_init(void)
{
	clock_gettime(CLOCK_MONOTONIC_COARSE, &pc_t0);
	__atomic_store_n(&pc_t0_ready, 1, __ATOMIC_RELEASE);
}

/* S231: the write clock alone on its lines.  Every write increments it;
 * the statics declared beside it (pc_t0, pc_t0_ready) are read by
 * compat_ticks() several times a request, and sharing its line they
 * missed on every write another worker made - compat_ticks was 23-28% of
 * a saturated SET at 8-12 workers, doing nothing but a vDSO call
 * (DESIGN 12ga).  128 bytes, 64-aligned: two whole lines, so nothing
 * shares either, nor the adjacent-line prefetch pair. */
static struct {
	unsigned long long v;
	char pad[128 - sizeof(unsigned long long)];
} pc_lamport_line __attribute__((aligned(128)));
/* S251: 128, not 64.  "Two whole lines" only keeps the adjacent-line
 * prefetch pair to itself when the PAIR is aligned: at 64 the standalone
 * link put the counter's first line in the same 128-byte pair as
 * compat_ticks_offset, which every request reads - the write on every SET
 * then cost every clock read, 8.4% of a SET at 8 workers
 * (compat_ticks, 95.7% of it on that one load) against under 1.2% in the
 * clustered link, where the same object happened to land on a 128
 * boundary.  Measured on 223, both editions, same tree (DESIGN 12gj). */
#define pc_lamport (pc_lamport_line.v)

#ifdef PC_EDITION_STANDALONE
/*
 * S252: THE STANDALONE CLOCK IS PER THREAD, AND READS THE SHARED ONE.
 *
 * One atomic add per write on one line shared by every worker was 5-7% of
 * a saturated SET at 8-12 workers even with the line isolated (S231,
 * S251).  A standalone node needs a key's versions to rise with time - the
 * WAL is replayed BY VERSION (S240) - and nothing more: no peer compares
 * them.  While the key's record exists the store floors the version at the
 * old one + 1 under the bucket lock (S234).  What the floor cannot see is a
 * key DELETED and written again: there is no old record.  So:
 *
 *   tick     = max(this thread's clock, the shared one) + 1   - a READ of
 *              the shared line, which only deletes write
 *   publish  = a delete raises the shared clock to its own version
 *              BEFORE its bucket lock is released (pcache_ht remove path),
 *              so a later write of that key - under the same lock, on any
 *              thread - reads it and stamps above the delete
 *
 * The first cut handed out blocks of 1,024 from the shared counter
 * instead, and walordertest caught it: 192 of 120,000 keys replayed OLDER
 * than they had served - deleted on one thread, written again on another
 * whose block was lower.
 *
 * NOT for the clustered build: there a version must also beat what this
 * node has observed from a peer, which the RMW does by construction. */
static __thread unsigned long long lt_clock;

unsigned long long pc_lamport_tick(void)
{
	unsigned long long g = __atomic_load_n(&pc_lamport, __ATOMIC_ACQUIRE);

	lt_clock = (lt_clock > g ? lt_clock : g) + 1;
	return lt_clock;
}

void pc_lamport_publish(unsigned long long v)
{
	unsigned long long cur = __atomic_load_n(&pc_lamport, __ATOMIC_ACQUIRE);

	while (v > cur &&
	       !__atomic_compare_exchange_n(&pc_lamport, &cur, v, 0,
		        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		;
}

static void lamport_moved(void)
{
}
#else
unsigned long long pc_lamport_tick(void)
{
	return __atomic_add_fetch(&pc_lamport, 1, __ATOMIC_ACQ_REL);
}

/* the RMW already keeps the shared clock above every version issued */
void pc_lamport_publish(unsigned long long v)
{
	(void)v;
}

static void lamport_moved(void)
{
}
#endif

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
		        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			lamport_moved();       /* S252: blocks below it are void */
			return;
		}
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
		        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			lamport_moved();       /* S252 */
			return;
		}
		/* cur was reloaded with the current value; retry */
	}
}

unsigned int compat_ticks(void)
{
	struct timespec now;

	/* the write path reads the clock several times a request, and a
	 * pthread_once call on each was 8% of a SET's worker CPU (measured
	 * with perf at 1.65M SET/s): the once only for the first call */
	if (!__atomic_load_n(&pc_t0_ready, __ATOMIC_ACQUIRE))
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

