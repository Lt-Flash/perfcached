/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * iostat.c - 0.6.0 step 1: system calls against the work they moved
 * (iostat.h).  The per-thread counters are summed when read; a read races
 * the writers by design and sees each counter either before or after an
 * increment, never torn (one aligned 64-bit load each).
 */
#include <pthread.h>
#include <string.h>

#include "config.h"
#include "iostat.h"

struct pc_iostat_w pc_iostat_slots[PC_IOSTAT_SLOTS];
_Static_assert(sizeof(struct pc_iostat_w) == 64, "one thread's counters, one cache line");
struct pc_io_backend pc_io_backend;         /* S361 */

const char *pc_io_backend_name(int b)
{
	return b == PC_IO_URING ? "io_uring" : "epoll";
}

static unsigned long long tx_calls[PC_UDP_N], tx_dgrams[PC_UDP_N];
static unsigned long long rx_calls[PC_UDP_N], rx_dgrams[PC_UDP_N];

static pthread_mutex_t base_mx = PTHREAD_MUTEX_INITIALIZER;
static struct pc_iostat base;               /* S358: at the last reset */

void pc_iostat_udp_tx(int which, unsigned long long calls,
		unsigned long long dgrams)
{
	__atomic_add_fetch(&tx_calls[which], calls, __ATOMIC_RELAXED);
	__atomic_add_fetch(&tx_dgrams[which], dgrams, __ATOMIC_RELAXED);
}

void pc_iostat_udp_rx(int which, unsigned long long calls,
		unsigned long long dgrams)
{
	__atomic_add_fetch(&rx_calls[which], calls, __ATOMIC_RELAXED);
	__atomic_add_fetch(&rx_dgrams[which], dgrams, __ATOMIC_RELAXED);
}

static void raw(struct pc_iostat *o)
{
	int i;

	memset(o, 0, sizeof *o);
	for (i = 0; i < PC_IOSTAT_SLOTS; i++) {
		const struct pc_iostat_w *w = &pc_iostat_slots[i];

		o->ep_waits += __atomic_load_n(&w->ep_waits, __ATOMIC_RELAXED);
		o->ep_events += __atomic_load_n(&w->ep_events, __ATOMIC_RELAXED);
		o->reads += __atomic_load_n(&w->reads, __ATOMIC_RELAXED);
		o->writes += __atomic_load_n(&w->writes, __ATOMIC_RELAXED);
		o->ur_enters += __atomic_load_n(&w->ur_enters, __ATOMIC_RELAXED);
		o->ur_cqes += __atomic_load_n(&w->ur_cqes, __ATOMIC_RELAXED);
		o->ur_recvs += __atomic_load_n(&w->ur_recvs, __ATOMIC_RELAXED);
		o->ur_sends += __atomic_load_n(&w->ur_sends, __ATOMIC_RELAXED);
	}
	for (i = 0; i < PC_UDP_N; i++) {
		o->tx_calls[i] = __atomic_load_n(&tx_calls[i], __ATOMIC_RELAXED);
		o->tx_dgrams[i] = __atomic_load_n(&tx_dgrams[i], __ATOMIC_RELAXED);
		o->rx_calls[i] = __atomic_load_n(&rx_calls[i], __ATOMIC_RELAXED);
		o->rx_dgrams[i] = __atomic_load_n(&rx_dgrams[i], __ATOMIC_RELAXED);
	}
}

void pc_iostat_slot(int slot, struct pc_iostat *out)
{
	const struct pc_iostat_w *w;

	memset(out, 0, sizeof *out);
	if (slot < 0 || slot >= PC_IOSTAT_SLOTS)
		return;
	w = &pc_iostat_slots[slot];
	out->ep_waits = __atomic_load_n(&w->ep_waits, __ATOMIC_RELAXED);
	out->ep_events = __atomic_load_n(&w->ep_events, __ATOMIC_RELAXED);
	out->reads = __atomic_load_n(&w->reads, __ATOMIC_RELAXED);
	out->writes = __atomic_load_n(&w->writes, __ATOMIC_RELAXED);
	out->ur_enters = __atomic_load_n(&w->ur_enters, __ATOMIC_RELAXED);
	out->ur_cqes = __atomic_load_n(&w->ur_cqes, __ATOMIC_RELAXED);
	out->ur_recvs = __atomic_load_n(&w->ur_recvs, __ATOMIC_RELAXED);
	out->ur_sends = __atomic_load_n(&w->ur_sends, __ATOMIC_RELAXED);
}

void pc_iostat_get(struct pc_iostat *out, int since_reset)
{
	int i;

	raw(out);
	if (!since_reset)
		return;
	pthread_mutex_lock(&base_mx);
	out->ep_waits -= base.ep_waits;
	out->ep_events -= base.ep_events;
	out->reads -= base.reads;
	out->writes -= base.writes;
	out->ur_enters -= base.ur_enters;
	out->ur_cqes -= base.ur_cqes;
	out->ur_recvs -= base.ur_recvs;
	out->ur_sends -= base.ur_sends;
	for (i = 0; i < PC_UDP_N; i++) {
		out->tx_calls[i] -= base.tx_calls[i];
		out->tx_dgrams[i] -= base.tx_dgrams[i];
		out->rx_calls[i] -= base.rx_calls[i];
		out->rx_dgrams[i] -= base.rx_dgrams[i];
	}
	pthread_mutex_unlock(&base_mx);
}

void pc_iostat_reset(void)
{
	struct pc_iostat now;

	raw(&now);
	pthread_mutex_lock(&base_mx);
	base = now;
	pthread_mutex_unlock(&base_mx);
}
