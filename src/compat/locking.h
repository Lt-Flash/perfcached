/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Shim for OpenSIPS locking.h — a 4-byte futex mutex between threads.
 *
 * The vendored bucket layout statically asserts sizeof(gen_lock_t) == 4
 * (one cache line per bucket); this is the futex/fastlock-class backend
 * it demands.  States: 0 free, 1 locked, 2 locked-with-waiters (the
 * classic three-state futex mutex).  Fast paths inline; the syscall
 * paths live in compat.c.  FUTEX_PRIVATE is used throughout — every
 * contender is a thread of this process by design. */
#ifndef PC_COMPAT_LOCKING_H
#define PC_COMPAT_LOCKING_H

typedef int gen_lock_t;

void compat_lock_slow(gen_lock_t *l);
void compat_lock_wake(gen_lock_t *l);

static inline gen_lock_t *lock_init(gen_lock_t *l)
{
	__atomic_store_n(l, 0, __ATOMIC_RELAXED);
	return l;
}

static inline void lock_destroy(gen_lock_t *l)
{
	(void)l;
}

/* PC_COMPAT_BROKEN_LOCKS is TEST-ONLY (make selftest_broken): it turns
 * both operations into no-ops so the S4 stress can prove it DETECTS the
 * races the locks exist to prevent (fail-before-pass discipline).  It
 * must never appear in a real build - the daemon never defines it. */
static inline void lock_get(gen_lock_t *l)
{
#ifdef PC_COMPAT_BROKEN_LOCKS
	(void)l;
#else
	int free_ = 0;

	if (!__atomic_compare_exchange_n(l, &free_, 1, 0,
	        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		compat_lock_slow(l);
#endif
}

static inline void lock_release(gen_lock_t *l)
{
#ifdef PC_COMPAT_BROKEN_LOCKS
	(void)l;
#else
	if (__atomic_exchange_n(l, 0, __ATOMIC_RELEASE) == 2)
		compat_lock_wake(l);
#endif
}

#endif /* PC_COMPAT_LOCKING_H */
