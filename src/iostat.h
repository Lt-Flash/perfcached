/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* iostat.h - what the network costs in system calls: per-thread epoll waits, events, reads and writes
 * (lock-free slots) and UDP syscalls against datagrams moved; /stats windowed, /metrics never reset */
#ifndef PC_IOSTAT_H
#define PC_IOSTAT_H

#define PC_IOSTAT_SLOTS 520                /* daemon.c: threads[512 + 3] */

struct pc_iostat_w {                        /* one thread's, one cache line */
	unsigned long long ep_waits, ep_events, reads, writes;
	/* an io_uring worker's io_uring_enter() calls, completions reaped, receives that brought input
	 * (its read()) and sends that carried output (its write()) */
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

/* a worker's time outside its wait (busy) and in it, from the vDSO clock around epoll_wait /
 * io_uring_enter; on io_uring the enter submits the turn's sends, so they count as waiting */
struct pc_iobusy_w {
	unsigned long long busy_ns, wait_ns;
} __attribute__((aligned(64)));
extern struct pc_iobusy_w pc_iobusy_slots[PC_IOSTAT_SLOTS];
#define PC_IOBUSY_ADD(field, v) do {                                         \
	int i_ = pc_worker_id();                                             \
	if (i_ >= 0 && i_ < PC_IOSTAT_SLOTS)                                  \
		__atomic_store_n(&pc_iobusy_slots[i_].field,                  \
			__atomic_load_n(&pc_iobusy_slots[i_].field,           \
				__ATOMIC_RELAXED) + (v), __ATOMIC_RELAXED);   \
} while (0)
/* once a second (daemon.c's 1 Hz loop): a sample of every worker's busy
 * time, kept five minutes */
void pc_iobusy_tick(void);
/* worker @w's (0..workers-1) busy share of the last @secs seconds, 0-100,
 * or -1 before two samples; over the samples there are when fewer */
int pc_iobusy_pct(int w, int secs);
/* the busiest worker's share over @secs, -1 as above */
int pc_iobusy_max_pct(int secs);
/* worker @w's busy and waiting time since it started, in ns (/metrics) */
void pc_iobusy_ns(int w, unsigned long long *busy, unsigned long long *wait);

/* UDP: any thread sends, so these are shared and atomic */
enum { PC_UDP_CLUSTER, PC_UDP_RELAY, PC_UDP_N };
void pc_iostat_udp_tx(int which, unsigned long long calls,
		unsigned long long dgrams);
void pc_iostat_udp_rx(int which, unsigned long long calls,
		unsigned long long dgrams);

struct pc_iostat {
	unsigned long long ep_waits, ep_events, reads, writes;
	unsigned long long ur_enters, ur_cqes, ur_recvs, ur_sends;
	unsigned long long tx_calls[PC_UDP_N], tx_dgrams[PC_UDP_N];
	unsigned long long rx_calls[PC_UDP_N], rx_dgrams[PC_UDP_N];
};
/* the workers' I/O backend - configured, running, and why not when they differ; written by the
 * main thread before the workers start (workers_uring: by each worker as its ring comes up) */
struct pc_io_backend {
	int configured, explicit_set;       /* PC_IO_* (config.h) */
	int active;
	int buf_tier;                       /* PC_UR_BUF_* (uring.h) */
	int counts_iowait;                  /* the kernel counts a ring wait as
	                                     * iowait (no opt-out before 6.15) */
	int workers, workers_uring;
	char why[200];                      /* when active != configured */
	/* under io = epoll, whether io_uring would run here (the
	 * start-up check is made anyway), its buffer tier, or why not */
	int uring_ok, uring_tier;
	char uring_why[200];
	/* busy poll - what was asked, how many workers have it, and the
	 * first refusal's reason */
	int bp_us, bp_prefer, bp_workers, bp_refused;
	char bp_why[200];
};
extern struct pc_io_backend pc_io_backend;
const char *pc_io_backend_name(int b);

/* @since_reset: 1 for /stats (since the last stats reset), 0 for /metrics */
void pc_iostat_get(struct pc_iostat *out, int since_reset);
/* one thread's own counters, never reset (/metrics, per worker - a
 * worker's slot is 1 + its id, daemon.c) */
void pc_iostat_slot(int slot, struct pc_iostat *out);
void pc_iostat_reset(void);

#endif /* PC_IOSTAT_H */
