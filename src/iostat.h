/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * iostat.h - 0.6.0 step 1 (DESIGN 12li): what the network costs in
 * system calls, the instrument S361-S364 are judged with.
 *
 *  - per thread (a worker's own slot, written only by that thread - no
 *    lock, no atomic RMW on the hot path): epoll waits and the events
 *    they returned, read() and write() calls on client sockets;
 *  - the cluster plane's UDP: send and receive SYSCALLS against the
 *    DATAGRAMS they moved - one each today, so datagrams per syscall is
 *    1.0 and S364's batching is what moves it;
 *  - the pub/sub relay's UDP, the same way.
 *
 * Counted where the calls are made; read by /stats (`io`, windowed, reset
 * with the other counters) and /metrics (never reset).
 */
#ifndef PC_IOSTAT_H
#define PC_IOSTAT_H

#define PC_IOSTAT_SLOTS 520                /* daemon.c: threads[512 + 3] */

struct pc_iostat_w {                        /* one thread's, one cache line */
	unsigned long long ep_waits, ep_events, reads, writes;
	/* S361: an io_uring worker's io_uring_enter() calls, the completions
	 * it reaped, and the receives among them that brought input (its
	 * read() equivalent - no system call); and the sends that carried
	 * its output (step 3: its write() equivalent) */
	unsigned long long ur_enters, ur_cqes, ur_recvs, ur_sends;
} __attribute__((aligned(64)));

extern struct pc_iostat_w pc_iostat_slots[PC_IOSTAT_SLOTS];
int pc_worker_id(void);                     /* daemon.c */

/* single writer per slot: a plain load and store, never a locked add */
#define PC_IOSTAT_ADD(field, v) do {                                         \
	int i_ = pc_worker_id();                                             \
	if (i_ >= 0 && i_ < PC_IOSTAT_SLOTS)                                  \
		__atomic_store_n(&pc_iostat_slots[i_].field,                  \
			__atomic_load_n(&pc_iostat_slots[i_].field,           \
				__ATOMIC_RELAXED) + (v), __ATOMIC_RELAXED);   \
} while (0)

/* UDP: any thread sends, so these are shared and atomic */
enum { PC_UDP_CLUSTER, PC_UDP_RELAY, PC_UDP_N };
void pc_iostat_udp_tx(int which, unsigned long long calls,
		unsigned long long dgrams);
void pc_iostat_udp_rx(int which, unsigned long long calls,
		unsigned long long dgrams);

struct pc_iostat {
	unsigned long long ep_waits, ep_events, reads, writes;
	unsigned long long ur_enters, ur_cqes, ur_recvs, ur_sends;   /* S361 */
	unsigned long long tx_calls[PC_UDP_N], tx_dgrams[PC_UDP_N];
	unsigned long long rx_calls[PC_UDP_N], rx_dgrams[PC_UDP_N];
};
/* S361: the workers' I/O backend - what was configured, what runs, and
 * why not, when they differ.  Written once by the main thread before the
 * workers start (workers_uring: by each worker as its ring comes up). */
struct pc_io_backend {
	int configured, explicit_set;       /* PC_IO_* (config.h) */
	int active;
	int buf_tier;                       /* PC_UR_BUF_* (uring.h) */
	int workers, workers_uring;
	char why[200];                      /* when active != configured */
	/* S363: busy poll - what was asked, how many workers have it, and the
	 * first refusal's reason */
	int bp_us, bp_prefer, bp_workers, bp_refused;
	char bp_why[200];
};
extern struct pc_io_backend pc_io_backend;
const char *pc_io_backend_name(int b);

/* @since_reset: 1 for /stats (since the last stats reset), 0 for /metrics */
void pc_iostat_get(struct pc_iostat *out, int since_reset);
/* S361: one thread's own counters, never reset (/metrics, per worker - a
 * worker's slot is 1 + its id, daemon.c) */
void pc_iostat_slot(int slot, struct pc_iostat *out);
void pc_iostat_reset(void);                 /* S358 */

#endif /* PC_IOSTAT_H */
