/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* compat.c - compatibility shim for the vendored core: logging, futex mutex slow
 * paths, the ticks clock, the broadcast hook and the per-thread identity */
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
	/* the analyzer misfires on the ternary in the size argument */
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

/* the write clock alone in a 128-aligned line pair: clock statics read on every
 * request must not miss on each write, nor share its adjacent-line prefetch */
static struct {
	unsigned long long v;
	char pad[128 - sizeof(unsigned long long)];
} pc_lamport_line __attribute__((aligned(128)));
#define pc_lamport (pc_lamport_line.v)

#ifdef PC_EDITION_STANDALONE
/* per-thread clock floored by the shared one (WAL replay needs rising versions);
 * a delete publishes its version before unlocking, so a rewrite stamps above it */
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

/* far beyond a real peer lead (~2M/s over a 1 Hz heartbeat), so one bad heartbeat
 * cannot jump every node's clock for good; refused jumps are counted */
#define PC_LAMPORT_MAX_JUMP 1000000000ULL

unsigned long long pc_lamport_rejected;

/* forward only, and a CAS loop so two workers observing different peers cannot
 * lose the larger value */
void pc_lamport_restore(unsigned long long high)
{
	unsigned long long cur = __atomic_load_n(&pc_lamport, __ATOMIC_ACQUIRE);

	while (high > cur) {
		if (__atomic_compare_exchange_n(&pc_lamport, &cur, high, 0,
		        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			lamport_moved();
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
			lamport_moved();
			return;
		}
		/* cur was reloaded with the current value; retry */
	}
}

unsigned int compat_ticks(void)
{
	struct timespec now;

	/* read several times a request, and pthread_once on each read was 8% of a
	 * SET's worker CPU: only the first call takes it */
	if (!__atomic_load_n(&pc_t0_ready, __ATOMIC_ACQUIRE))
		pthread_once(&pc_t0_once, pc_t0_init);
	clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
	return (unsigned int)(now.tv_sec - pc_t0.tv_sec) + compat_ticks_offset;
}

unsigned int compat_ticks_deadline(long long ms)
{
	struct timespec now;
	long long f;

	if (!__atomic_load_n(&pc_t0_ready, __ATOMIC_ACQUIRE))
		pthread_once(&pc_t0_once, pc_t0_init);
	clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
	/* a tick starts where the clock's seconds roll over: the deadline's
	 * whole seconds past this one are the ticks it reaches */
	f = (long long)(now.tv_nsec / 1000000) + (ms > 0 ? ms : 0);
	if (f / 1000 > 0x7fffffffLL)
		f = 0x7fffffffLL * 1000;
	return (unsigned int)(now.tv_sec - pc_t0.tv_sec) + compat_ticks_offset +
		(unsigned int)(f / 1000);
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

