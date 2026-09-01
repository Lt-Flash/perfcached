/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * daemon.c — S6: the threading frame.
 *
 * Threads (identity = pc_worker_id(), over struct pc_thread):
 *   0        main: init, signal handling, join, teardown
 *   1..N     workers: one epoll each, own SO_REUSEPORT TCP listeners
 *            (kernel balances accepts), the shared UNIX listener via
 *            EPOLLEXCLUSIVE; S6 behavior on accept is a polite immediate
 *            close - the protocol layer (S7) replaces the handler
 *   N+1      maintenance: 1/s arena reclaim tick + per-collection sweep
 *            and linear-hash growth (the single-splitter home)
 *   N+2      WAL stub: parked until M3 fills it in
 *
 * Every thread owns an eventfd used for two things: waking it out of its
 * wait, and delivering broadcast RPCs.  compat_set_broadcast() wires the
 * shim's ipc_send_rpc_all (the arena hoard flush) to run the callback on
 * EVERY live thread - inline on the caller, queued + eventfd-kicked on
 * the rest.  Shutdown: SIGTERM/SIGINT set the stop flag and kick every
 * eventfd (both async-signal-safe), main joins everything, tears down.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "compat/compat.h"
#include "compat/dprint.h"
#include "compat/timer.h"
#include "compat/ipc.h"
#include "core/pcache_mem.h"
#include "core/pcache_arena.h"
#include "core/pcache_htable.h"
#include "proto.h"
#include "store.h"
#include "storage.h"
#include "walprobe.h"
#include "wal.h"
#include "rdb.h"
#include "recover.h"
#include "cluster.h"
#include "obs.h"
#include "daemon.h"

/* the WAL directory's resolved storage identity, retained for the
 * stats verb (class + chain belong next to the wal counters, not only
 * in a startup log line) */
static struct pc_st_id wal_sid;
static int wal_id_valid;
static struct pc_wal_probe wal_probe;

const struct pc_st_id *pc_wal_identity(void)
{
	return wal_id_valid ? &wal_sid : NULL;
}

const struct pc_wal_probe *pc_wal_probe_result(void)
{
	return wal_probe.valid ? &wal_probe : NULL;
}



extern char *pcache_backing_policy;
extern int pcache_arena_hugepage_mb;
extern unsigned long pcache_arena_max_bytes;
extern int pcache_arena_hugepage_cap_mb;
extern int pcache_reclaim_keep, pcache_reclaim_quiet_s,
	pcache_reclaim_cooloff_s, pcache_reclaim_giveback;

#ifndef EPOLLEXCLUSIVE
#define EPOLLEXCLUSIVE (1u << 28)
#endif

#define PC_RPCQ 8

struct pc_thread {
	pthread_t tid;
	int efd;
	int idx;                            /* this worker's index; also
	                                     * handed to the vendored core
	                                     * as process_no, see
	                                     * compat_thread_register() */
	/* tiny locked RPC queue - broadcast is rare control-plane traffic */
	pthread_mutex_t mx;
	ipc_rpc_f fn[PC_RPCQ];
	void *arg[PC_RPCQ];
	int head, tail;
	int live;
};

struct pc_collection_rt {
	const struct pc_collection *conf;
	pcache_htable_t *ht;
};

static struct pc_thread threads[512 + 3];
static int n_threads;
static __thread struct pc_thread *self_slot;

static struct pc_collection_rt cols[PC_MAX_COLLECTIONS];
static int n_cols;

static const struct pc_config *CFG;

/* ---- RESP listener guards (task S33) ------------------------------------
 * A RESP listener has no handshake - stock Redis clients cannot speak
 * Noise - so its access control is entirely config: an allow-list
 * checked at accept, and an optional Redis AUTH password checked before
 * any data command.  Both live here because CFG does. */

int pc_resp_password_set(void)
{
	return CFG && CFG->resp_password != NULL;
}

/* constant-time: the password crosses the wire in the clear, which is
 * no reason to leak its prefix through timing as well */
int pc_resp_password_ok(const char *p, size_t n)
{
	const char *want = CFG ? CFG->resp_password : NULL;
	size_t wn, i;
	unsigned char diff = 0;

	if (!want)
		return 0;
	wn = strlen(want);
	if (n != wn)
		return 0;
	for (i = 0; i < n; i++)
		diff |= (unsigned char)(p[i] ^ want[i]);
	return diff == 0;
}

int pc_resp_peer_allowed(const struct sockaddr_in *sa, unsigned int len)
{
	int i;

	if (!CFG || !CFG->n_resp_allow)
		return 1;              /* a LAN listener without a list was
		                        * already refused at config validate */
	if (len < sizeof *sa || sa->sin_family != AF_INET)
		return 0;              /* v6 or unknown: not on the list */
	for (i = 0; i < CFG->n_resp_allow; i++)
		if ((sa->sin_addr.s_addr & CFG->resp_allow[i].mask) ==
		        CFG->resp_allow[i].net)
			return 1;
	/* A turned-away client is invisible to it (we close before a byte
	 * is read), so it has to be visible HERE or an operator debugging
	 * "my Redis client just hangs up" has nothing to go on.  Rate-
	 * limited: a port scan must not become a log flood. */
	{
		static __thread unsigned int shouted;

		if (shouted < 10) {
			shouted++;
			LM_WARN("RESP connection from %s refused: not in "
				"resp_allow%s\n", inet_ntoa(sa->sin_addr),
				shouted == 10 ? " (further ones silent)" : "");
		}
	}
	return 0;
}

/* the optional collection allow-list for RESP clients: NULL = all */
const char *pc_resp_collections(void)
{
	return CFG ? CFG->resp_collections : NULL;
}
static volatile sig_atomic_t stop_flag;
static int unix_fd = -1;
static struct pc_psk_ctx PSK;
static int have_psk;

/* the `probe` verb: re-measure on demand.  BLOCKS THE CALLER for the
 * probe's duration (like sync), and the measurement I/O shares the WAL
 * device with the live pump - expect perturbed fsync latency while it
 * runs; that is inherent to re-measuring.  Updates the retained result
 * (stats flips to cached:false) and the on-disk cache.  Concurrent
 * stats readers may see a torn number mid-update - counters only,
 * accepted like every other stats read. */
int pc_wal_reprobe(int secs, struct pc_wal_policy *pol)
{
	if (!CFG || !CFG->wal_dir)
		return -1;
	if (pc_wal_probe_run(CFG->wal_dir, secs, &wal_probe) != 0)
		return -1;
	pc_wal_probe_cache_store(CFG->wal_dir, &wal_probe);
	pc_wal_policy_from(&wal_probe, wal_id_valid ? &wal_sid : NULL, pol);
	return 0;
}

/* ---- registry + broadcast --------------------------------------------- */

static struct pc_thread *slot_init(int idx)
{
	struct pc_thread *t = &threads[n_threads++];

	t->idx = idx;
	t->efd = eventfd(0, EFD_NONBLOCK);
	pthread_mutex_init(&t->mx, NULL);
	return t;
}

static void slot_attach(struct pc_thread *t)
{
	self_slot = t;
	t->tid = pthread_self();
	compat_thread_register(t->idx);
	__atomic_store_n(&t->live, 1, __ATOMIC_RELEASE);
}

/* the worker index, from the context this thread already carries */
int pc_worker_id(void)
{
	return self_slot ? self_slot->idx : -1;
}

static void rpc_drain(struct pc_thread *t)
{
	ipc_rpc_f fn;
	void *arg;

	for (;;) {
		pthread_mutex_lock(&t->mx);
		if (t->head == t->tail) {
			pthread_mutex_unlock(&t->mx);
			return;
		}
		fn = t->fn[t->head % PC_RPCQ];
		arg = t->arg[t->head % PC_RPCQ];
		t->head++;
		pthread_mutex_unlock(&t->mx);
		fn(t->idx, arg);
	}
}

static void pc_broadcast(void (*fn)(int sender, void *param), void *param)
{
	uint64_t one = 1;
	int i;

	for (i = 0; i < n_threads; i++) {
		struct pc_thread *t = &threads[i];

		if (!__atomic_load_n(&t->live, __ATOMIC_ACQUIRE))
			continue;
		if (t == self_slot) {
			fn(t->idx, param);
			continue;
		}
		pthread_mutex_lock(&t->mx);
		if (t->tail - t->head < PC_RPCQ) {
			t->fn[t->tail % PC_RPCQ] = fn;
			t->arg[t->tail % PC_RPCQ] = param;
			t->tail++;
		}
		pthread_mutex_unlock(&t->mx);
		if (write(t->efd, &one, sizeof one) < 0) { /* wake is best-effort */ }
	}
}

/* ---- listeners --------------------------------------------------------- */

/* Is something ALREADY listening on this address:port?
 *
 * Every TCP listener sets SO_REUSEPORT, because each worker binds its
 * own socket and lets the kernel balance accepts across them.  The
 * consequence is that a second perfcached on the same address:port does
 * NOT get EADDRINUSE - it silently joins the load-balancing group and
 * answers a share of the connections, with its own dataset and its own
 * view of the cluster.  The symptom is intermittent wrong answers, or a
 * fleet that reports one member when it has four, and it is invisible
 * in the logs of either process.
 *
 * The only way to see an existing listener is to bind WITHOUT
 * SO_REUSEPORT once, before the workers start.  This socket is closed
 * immediately; the workers then bind normally.
 *
 * Only EADDRINUSE means "someone is already there".  EADDRNOTAVAIL
 * means the address is not on this host - a VIP with
 * net.ipv4.ip_nonlocal_bind=0 - which is a different fault with its own
 * message, so everything but EADDRINUSE is left to the generic probe.
 * (Checked both ways: ip_nonlocal_bind changes whether an absent
 * address binds at all, and changes nothing about EADDRINUSE.)
 */
static int tcp_port_taken(const struct pc_listener *l)
{
	struct addrinfo hints, *res = NULL, *ai;
	char port[16];
	int taken = 0;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	snprintf(port, sizeof port, "%d", l->port);
	if (getaddrinfo(strcmp(l->addr, "*") ? l->addr : NULL, port,
	        &hints, &res) != 0 || !res)
		return 0;                      /* the real bind will report it */
	for (ai = res; ai; ai = ai->ai_next) {
		int one = 1, fd = socket(ai->ai_family, ai->ai_socktype, 0);

		if (fd < 0)
			continue;
		/* REUSEADDR yes (TIME_WAIT is not a conflict), REUSEPORT no -
		 * omitting it is the entire point of this probe */
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		if (bind(fd, ai->ai_addr, ai->ai_addrlen) != 0 &&
		        errno == EADDRINUSE)
			taken = 1;
		close(fd);
		if (taken)
			break;
	}
	freeaddrinfo(res);
	return taken;
}

static int tcp_listener_fd(const struct pc_listener *l)
{
	struct addrinfo hints, *res = NULL, *ai;
	char port[16];
	int fd = -1, one = 1;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	snprintf(port, sizeof port, "%d", l->port);

	if (getaddrinfo(strcmp(l->addr, "*") ? l->addr : NULL, port,
	        &hints, &res) != 0 || !res) {
		LM_ERR("cannot resolve listener %s:%d\n", l->addr, l->port);
		return -1;
	}
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, 0);
		if (fd < 0)
			continue;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
		if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
		        listen(fd, 511) == 0) {
			fcntl(fd, F_SETFL, O_NONBLOCK);
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0)
		LM_ERR("cannot bind listener %s:%d (%s)\n", l->addr, l->port,
			strerror(errno));
	return fd;
}

static int unix_listener_fd(const char *path)
{
	struct sockaddr_un sa;
	int fd, probe;

	if (strlen(path) >= sizeof sa.sun_path) {
		LM_ERR("unix socket path too long\n");
		return -1;
	}
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, path);

	/* a stale socket file is normal after a crash; a LIVE one means
	 * another instance - probe before unlinking */
	probe = socket(AF_UNIX, SOCK_STREAM, 0);
	if (probe >= 0) {
		if (connect(probe, (struct sockaddr *)&sa, sizeof sa) == 0) {
			close(probe);
			LM_ERR("%s is accepting connections - another perfcached "
				"is running\n", path);
			return -1;
		}
		close(probe);
	}
	unlink(path);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 ||
	        listen(fd, 511) < 0) {
		LM_ERR("cannot bind unix listener %s (%s)\n", path,
			strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}

/* ---- worker ------------------------------------------------------------ */

struct worker_arg {
	struct pc_thread *slot;
	int wid;
};

/* Plaintext eligibility per listener: only loopback/unix listeners under
 * plaintext=loopback may speak the pre-Noise plaintext dialects.
 *
 * A RESP listener (S33) is the deliberate exception: it is plaintext BY
 * CONSTRUCTION - a stock Redis client cannot speak the Noise channel, so
 * requiring a handshake there would mean the listener could never serve
 * anyone.  Its safety comes from being narrow instead: RESP2 only (no
 * native dialects, no admin verbs), an allow-list the config REFUSES to
 * start without off-box, an optional AUTH password, and an optional
 * collection scope.  Without this, a LAN RESP listener silently fed
 * every client's first command into the Noise responder and dropped the
 * connection - which is exactly how it behaved before this line. */
static int listener_plaintext_ok(const struct pc_listener *l)
{
	if (l->resp)
		return 1;
	if (CFG->plaintext != PC_PLAINTEXT_LOOPBACK)
		return 0;
	return l->loopback;
}

static void *worker_main(void *p)
{
	struct worker_arg *wa = p;
	struct epoll_event ev, evs[32];
	struct pc_conn *conns = NULL;
	int ep, i, j, n, nlfd = 0;
	int lfds[PC_MAX_LISTEN + 1], plok[PC_MAX_LISTEN + 1];
	int lresp[PC_MAX_LISTEN + 1];
	uint64_t junk;

	slot_attach(wa->slot);
	pc_cluster_worker_register(wa->slot->idx, wa->slot->efd);
	ep = epoll_create1(0);

	/* epoll data tagging: fds (efd/listeners) ride data.u64 as small
	 * ints; connections ride data.ptr - heap pointers are never < 64K */
	ev.events = EPOLLIN;
	ev.data.u64 = (uint64_t)wa->slot->efd;
	epoll_ctl(ep, EPOLL_CTL_ADD, wa->slot->efd, &ev);

	/* own SO_REUSEPORT socket per TCP listener - the kernel balances */
	for (i = 0; i < CFG->n_listen; i++) {
		if (CFG->listen[i].type != PC_LISTEN_TCP)
			continue;
		lfds[nlfd] = tcp_listener_fd(&CFG->listen[i]);
		if (lfds[nlfd] < 0)
			continue;                  /* failure already logged */
		plok[nlfd] = listener_plaintext_ok(&CFG->listen[i]);
		lresp[nlfd] = CFG->listen[i].resp;
		ev.events = EPOLLIN;
		ev.data.u64 = (uint64_t)lfds[nlfd];
		epoll_ctl(ep, EPOLL_CTL_ADD, lfds[nlfd], &ev);
		nlfd++;
	}
	/* the single shared UNIX listener: EPOLLEXCLUSIVE avoids the herd */
	if (unix_fd >= 0) {
		lfds[nlfd] = unix_fd;
		plok[nlfd] = 0;    /* default-deny; the loop below sets the
		                    * configured value - the analyzer cannot see
		                    * that unix_fd >= 0 implies the loop matches */
		lresp[nlfd] = 0;   /* the RESP door is TCP-only (S33); this was
		                    * UNINITIALIZED - garbage nonzero would have
		                    * served RESP on the unix socket at random
		                    * (found by clang-analyzer CallAndMessage) */
		for (i = 0; i < CFG->n_listen; i++)
			if (CFG->listen[i].type == PC_LISTEN_UNIX)
				plok[nlfd] = listener_plaintext_ok(&CFG->listen[i]);
		ev.events = EPOLLIN | EPOLLEXCLUSIVE;
		ev.data.u64 = (uint64_t)unix_fd;
		epoll_ctl(ep, EPOLL_CTL_ADD, unix_fd, &ev);
		nlfd++;
	}

	while (!stop_flag) {
		/* an active cooperative walk (S40) must not sit behind a
		 * blocking wait: poll, step the walks, wait again */
		n = epoll_wait(ep, evs, 32, pc_enum_pending() ? 0 : -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (i = 0; i < n; i++) {
			if (evs[i].data.u64 >= 65536) {
				pc_conn_event(evs[i].data.ptr, evs[i].events);
				continue;
			}
			if ((int)evs[i].data.u64 == wa->slot->efd) {
				struct pc_pull_done done[16];
				int nd, k;

				while (read(wa->slot->efd, &junk, sizeof junk) > 0)
					;
				rpc_drain(wa->slot);
				while ((nd = pc_cluster_drain(wa->slot->idx,
				        done, 16)) > 0)
					for (k = 0; k < nd; k++) {
						pc_proto_pull_complete(&done[k]);
						free(done[k].val);
					}
				continue;
			}
			for (j = 0; j < nlfd; j++)
				if (lfds[j] == (int)evs[i].data.u64) {
					pc_conn_accept(ep, lfds[j], plok[j],
						lresp[j],
						have_psk ? &PSK : NULL, &conns);
					break;
				}
		}
		pc_enum_step();
	}
	pc_conn_destroy_all(&conns);
	for (i = 0; i < nlfd; i++)
		if (lfds[i] != unix_fd)
			close(lfds[i]);
	close(ep);
	return NULL;
}

/* ---- maintenance ------------------------------------------------------- */

static void *maint_main(void *p)
{
	unsigned int last = 0, now;
	int i;

	slot_attach(p);
	while (!stop_flag) {
		usleep(100 * 1000);
		rpc_drain(self_slot);
		now = get_ticks();
		if (now == last)
			continue;
		last = now;
		pc_obs_tick_1hz();
		/* fsync watchdog: a hung fsync on network-class storage is a
		 * HANG, not an error - flag it loudly, workers never block */
		{
			long long st = pc_wal_fsync_start_us;

			if (st) {
				struct timespec _ts;
				long long _now;

				clock_gettime(CLOCK_MONOTONIC, &_ts);
				_now = (long long)_ts.tv_sec * 1000000 +
					_ts.tv_nsec / 1000;
				if (_now - st > 5000000)
					LM_CRIT("wal: an fsync has been stuck for %llds - "
						"storage is stalled\n",
						(_now - st) / 1000000);
			}
		}
		/* the 1/s duties: reclaim, expiry, single-splitter growth */
		pcache_arena_reclaim_tick();
		for (i = 0; i < n_cols; i++) {
			pcache_ht_sweep(cols[i].ht, now, NULL, NULL);
			pcache_ht_grow(cols[i].ht, 4, 128);
		}
	}
	return NULL;
}

/* ---- peer thread (spawned only when clustered) -------------------------- */

static void *peer_main(void *p)
{
	slot_attach(p);
	pc_cluster_thread((volatile int *)&stop_flag);
	return NULL;
}

/* the bulk migration thread: TCP transfers of records too big for the
 * datagram plane (cluster principal Noise sessions, both directions) */
static void *bulk_main(void *p)
{
	slot_attach(p);
	pc_bulk_thread((volatile int *)&stop_flag);
	return NULL;
}

/* the heartbeat watchdog: keeps the node audible when the peer thread
 * is heads-down in a long duty (emission only - see cluster.h) */
static void *beat_main(void *p)
{
	slot_attach(p);
	pc_cluster_beat_thread((volatile int *)&stop_flag);
	return NULL;
}

/* ---- RDB thread (spawned only with persistence on) --------------------- */

static void *rdb_main(void *p)
{
	slot_attach(p);
	pc_rdb_thread((volatile int *)&stop_flag);
	return NULL;
}

/* ---- WAL thread --------------------------------------------------------- */

static void *wal_main(void *p)
{
	struct pollfd pf;
	int timeout = 200;

	slot_attach(p);
	pc_wal_set_wakeup(self_slot->efd);
	pf.fd = self_slot->efd;
	pf.events = POLLIN;
	while (!stop_flag) {
		pc_wal_mark_sleeping(1);
		poll(&pf, 1, timeout);
		pc_wal_mark_sleeping(0);
		if (pf.revents & POLLIN) {
			uint64_t junk;

			while (read(self_slot->efd, &junk, sizeof junk) > 0)
				;
		}
		rpc_drain(self_slot);
		timeout = pc_wal_pump();
	}
	pc_wal_shutdown();
	return NULL;
}

/* ---- signals ----------------------------------------------------------- */

static void on_signal(int sig)
{
	uint64_t one = 1;
	int i;

	(void)sig;
	stop_flag = 1;
	for (i = 0; i < n_threads; i++)
		if (threads[i].efd >= 0)
			if (write(threads[i].efd, &one, sizeof one) < 0) { /* best effort */ }
}

/* ---- the run ----------------------------------------------------------- */

int pc_daemon_run(struct pc_config *cfg)
{
	struct worker_arg wargs[512];
	struct pc_thread *mt, *wt, *rt = NULL, *ct = NULL, *bt = NULL;
	struct pc_thread *hb = NULL;
	struct sigaction sa;
	sigset_t blocked, old;
	pthread_attr_t tattr;
	int i, nw = cfg->workers;

	CFG = cfg;

	/* memory first: probe the ladder, size the arena from config */
	pcache_mem_probe();
	LM_NOTICE("memory: probed tier = %s (hugetlb %lukB, thp %lukB)\n",
		pcache_mem_tier_str(pcache_mem.tier), pcache_mem.hugetlb_kb,
		pcache_mem.thp_pmd_kb);
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = cfg->backing_heap ? 0 : cfg->arena_mb;
	pcache_arena_hugepage_cap_mb = cfg->arena_cap_mb;
	/* The HARD ceiling, which arena_mb alone never was: past the
	 * huge-page reservation the arena carves from shm_malloc - plain
	 * malloc here - so a node grew without any configured bound.
	 * config.h has always said arena_cap_mb "0 = fixed at arena_mb";
	 * this is what makes that true. */
	pcache_arena_max_bytes = (unsigned long)(cfg->arena_cap_mb
		? cfg->arena_cap_mb : cfg->arena_mb) << 20;
	pcache_reclaim_keep = cfg->reclaim_keep;
	pcache_reclaim_quiet_s = cfg->reclaim_quiet_s;
	pcache_reclaim_cooloff_s = cfg->reclaim_cooloff_s;
	pcache_reclaim_giveback = cfg->reclaim_giveback;
	if (pcache_arena_init() != 0) {
		LM_ERR("arena init failed\n");
		return 1;
	}

	for (i = 0; i < cfg->n_col; i++) {
		cols[i].conf = &cfg->col[i];
		cols[i].ht = pcache_htable_new((unsigned int)cfg->col[i].buckets_log2);
		if (!cols[i].ht) {
			LM_ERR("collection '%s': table creation failed\n",
				cfg->col[i].name);
			return 1;
		}
		pc_store_register(cfg->col[i].name, cols[i].ht,
			cfg->col[i].pull,
			cfg->col[i].mode == PC_MODE_PROXY,
			cfg->col[i].mode == PC_MODE_SHARD,
			cfg->col[i].eager);
	}
	n_cols = cfg->n_col;

	/* the shared UNIX listener (TCP ones are per-worker SO_REUSEPORT) */
	for (i = 0; i < cfg->n_listen; i++)
		if (cfg->listen[i].type == PC_LISTEN_UNIX) {
			unix_fd = unix_listener_fd(cfg->listen[i].addr);
			if (unix_fd < 0)
				return 1;
		}
	if (pc_obs_init(cfg->workers, cfg->slowlog_usec) != 0) {
		LM_ERR("observability init failed\n");
		return 1;
	}

	/* fail fast if a TCP listener cannot bind at all, and - because
	 * SO_REUSEPORT hides it - if one is already held by another
	 * process */
	for (i = 0; i < cfg->n_listen; i++)
		if (cfg->listen[i].type == PC_LISTEN_TCP) {
			int probe;

			if (tcp_port_taken(&cfg->listen[i])) {
				LM_ERR("%s:%d is already in use by another "
					"process.  TCP listeners set "
					"SO_REUSEPORT so the workers can share "
					"the port, which means a second "
					"perfcached would NOT be refused - it "
					"would join the port and answer a "
					"share of the connections with its own "
					"data and its own cluster view.  "
					"Refusing to start.  Stop the other "
					"process, or give this one its own "
					"address or port.\n",
					cfg->listen[i].addr,
					cfg->listen[i].port);
				return 1;
			}
			probe = tcp_listener_fd(&cfg->listen[i]);
			if (probe < 0)
				return 1;
			close(probe);
		}

	/* WAL storage: identity (the label) + probe (the measurement) at
	 * startup; S13 consumes the numbers, S12 reports them honestly */
	if (cfg->wal_dir) {
		struct pc_wal_policy pol;
		int ring_kb;
		char rep[3072];

		if (pc_storage_identity(cfg->wal_dir, &wal_sid) != 0) {
			LM_ERR("wal dir %s is unusable\n", cfg->wal_dir);
			return 1;
		}
		pc_storage_format(&wal_sid, rep, sizeof rep);
		LM_NOTICE("%s", rep);

		if (cfg->wal_probe == PC_WPROBE_ALWAYS) {
			if (pc_wal_probe_run(cfg->wal_dir, cfg->wal_probe_secs,
			        &wal_probe) == 0)
				pc_wal_probe_cache_store(cfg->wal_dir, &wal_probe);
		} else if (cfg->wal_probe == PC_WPROBE_AUTO) {
			if (pc_wal_probe_cache_load(cfg->wal_dir, &wal_probe) != 0 &&
			        pc_wal_probe_run(cfg->wal_dir, 0, &wal_probe) == 0)
				pc_wal_probe_cache_store(cfg->wal_dir, &wal_probe);
		}
		pc_wal_policy_from(wal_probe.valid ? &wal_probe : NULL, &wal_sid, &pol);
		wal_id_valid = 1;
		pc_wal_probe_format(wal_probe.valid ? &wal_probe : NULL, &pol, rep,
			sizeof rep);
		LM_NOTICE("%s", rep);

		/* S13: bring the WAL up - explicit knobs win, the probe's
		 * policy fills what the config left at 0 */
		if (cfg->wal_fsync == 1 &&
		        strcmp(pol.fsync_recommend, "always"))
			LM_WARN("wal: fsync=always configured but the probe "
				"recommends %s on this device\n", pol.fsync_recommend);
		{
			static const struct pc_rdb_rule defrules[] = {
				{ 900, 1 }, { 300, 10 }, { 60, 10000 } };
			const struct pc_rdb_rule *rr = cfg->n_rdb_rules < 0 ?
				defrules : cfg->rdb_rules;
			int nrr = cfg->n_rdb_rules < 0 ? 3 : cfg->n_rdb_rules;

			pc_rdb_init(cfg->wal_dir, rr, nrr, cfg->rdb_mb_s,
				wal_probe.valid ? wal_probe.seq_mb_s : 0);
		}
		/* S15: recovery BEFORE wal activation (activation claims the
		 * next generation slot), and a fresh checkpoint BEFORE it too
		 * (the clobbered oldest segment must already be covered) */
		{
			struct pc_recover_stats rst;

			/* A node with nothing of its own to bring back is
			 * STARTING; one that restores something is RECOVERING
			 * until the fleet settles it.  Which of the two it
			 * really is, is ultimately the master's call from the
			 * identity history - this is the node's own best
			 * guess, and the map is what settles it. */
			if (pc_recover(cfg->wal_dir, &rst) != 0) {
				LM_ERR("recovery failed\n");
				return 1;
			}
			if (rst.rdb_records + rst.wal_applied > 0)
				pc_node_state_set(PC_NST_RECOVERING);
			if (rst.rdb_records + rst.wal_applied > 0 &&
			        pc_rdb_save_sync() != 0) {
				LM_ERR("the post-recovery checkpoint failed - refusing "
					"to risk the replay window\n");
				return 1;
			}
		}
		/* fsync = always fsyncs once per drained batch, so the ring
		 * must absorb everything that arrives while the pump sits in
		 * fdatasync.  On ms-class storage the shipped 1 MB is not
		 * enough and the overflow path DROPS acknowledged writes, so
		 * take the depth the probe derived unless the operator pinned
		 * one.  Measured before this existed: ~13% of acknowledged
		 * writes absent after a restart, at 3 ms per fsync. */
		ring_kb = cfg->wal_ring_kb;
		if (cfg->wal_fsync == 1 && !cfg->wal_ring_kb_set &&
		        pol.ring_kb_always > ring_kb) {
			LM_NOTICE("wal: fsync = always on %lldus-p99 storage - "
				"ring %d -> %d KB per writer so the pump's "
				"fsync window cannot overflow it (set [wal] "
				"ring_kb to override)\n",
				pc_wal_probe_result() ?
				pc_wal_probe_result()->fsync_p99_us : 0,
				ring_kb, pol.ring_kb_always);
			ring_kb = pol.ring_kb_always;
		} else if (cfg->wal_fsync == 1 && !pol.ring_kb_always) {
			LM_WARN("wal: fsync = always with no probe - the ring "
				"depth CANNOT be sized for this device.  If "
				"fdatasync here takes milliseconds the ring "
				"will overflow under load and dropped records "
				"are acknowledged writes that will be missing "
				"after a restart.  Set [wal] probe = auto, or "
				"raise ring_kb.\n");
		}
		if (cfg->wal_fsync == 1 && pol.fsync_recommend &&
		        strcmp(pol.fsync_recommend, "always") != 0 &&
		        pc_wal_probe_result()) {
			LM_WARN("wal: fsync = always but this device measures "
				"%s-class (%s).  Every drained batch pays an "
				"fdatasync; sustained writes above ~%lld/s "
				"will outrun it.\n",
				pol.fsync_recommend, pol.note ? pol.note : "",
				pol.max_durable_wps);
		}
		if (pc_wal_init(cfg->wal_dir,
		        cfg->wal_fsync == 1 ? PC_WFSYNC_ALWAYS :
		        cfg->wal_fsync == 2 ? PC_WFSYNC_NO : PC_WFSYNC_EVERYSEC,
		        cfg->wal_segment_mb ? cfg->wal_segment_mb : pol.segment_mb,
		        cfg->wal_segments, ring_kb,
		        cfg->workers + 6) != 0) {
			LM_ERR("wal init failed\n");
			return 1;
		}
	}

	/* derive the PSKs once (Argon2id is never paid per connection).
	 * Needed whenever any listener is encryption-required. */
	{
		int need = 0;

		for (i = 0; i < cfg->n_listen; i++)
			if (!listener_plaintext_ok(&cfg->listen[i]))
				need = 1;
		if (cfg->cl_enabled)
			need = 1;                  /* the peer plane seals with it */
		if (need) {
			memset(&PSK, 0, sizeof PSK);
			for (i = 0; i < cfg->n_client_secrets; i++)
				if (pc_psk_derive(cfg->client_secret[i],
				        strlen(cfg->client_secret[i]), PC_PRIN_CLIENT,
				        PSK.client[i]) != 0)
					return 1;
			PSK.n_client = cfg->n_client_secrets;
			if (pc_psk_derive(cfg->cluster_secret,
			        strlen(cfg->cluster_secret), PC_PRIN_CLUSTER,
			        PSK.cluster) != 0)
				return 1;
			have_psk = 1;
			LM_NOTICE("Noise PSKs derived (%d client + 1 cluster)\n",
				PSK.n_client);
		}
	}

	if (!cfg->cl_enabled)
		pc_node_state_set(PC_NST_READY);   /* nothing to join */

	if (cfg->cl_enabled) {
		if (pc_cluster_init(cfg->cl_mcast, cfg->cl_port,
		        cfg->cl_advertise, PSK.cluster,
		        cfg->cl_pull_timeout_ms, cfg->cl_negative_ms,
		        cfg->cl_tombstone_ms, cfg->wal_dir,
		        cfg->cl_max_pending) != 0)
			return 1;
		/* S30: what this node believes the clustered collections
		 * ARE.  It rides every ALIVE, and a peer that disagrees is
		 * refused rather than fed (see config_digest()). */
		{
			int cport = 0, rport = 0, k;

			/* the first non-RESP TCP listener is where native
			 * clients dial us; the first RESP one is where Redis
			 * clients do.  Peers publish both, because the two
			 * audiences cannot use each other's door: RESP has no
			 * Noise handshake, and the native dialects are not
			 * spoken on the RESP listener. */
			for (k = 0; k < cfg->n_listen; k++)
				if (cfg->listen[k].type == PC_LISTEN_TCP &&
				        !cfg->listen[k].resp) {
					cport = cfg->listen[k].port;
					break;
				}
			for (k = 0; k < cfg->n_listen; k++)
				if (cfg->listen[k].type == PC_LISTEN_TCP &&
				        cfg->listen[k].resp) {
					rport = cfg->listen[k].port;
					break;
				}
			pc_cluster_set_config(cfg->n_cl_col ? (int)cfg->cl_mode
				: (cfg->n_col ? (int)cfg->col[0].mode : 0),
				cfg->cl_eager,
				cfg->n_cl_col != 0, cport, rport);
		}
	}

	compat_set_broadcast(pc_broadcast);

	/* spawn with signals blocked (inherited); main alone handles them.
	 * Explicit 1MB stacks: the cluster plane keeps ~65-130KB of
	 * datagram/seal buffers in stack frames, fine on glibc's 8MB
	 * default but PAST musl's 128KB - the first worker-thread forward
	 * segfaulted on Alpine until the size was pinned here. */
	pthread_attr_init(&tattr);
	pthread_attr_setstacksize(&tattr, 1024L * 1024);
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGTERM);
	sigaddset(&blocked, SIGINT);
	pthread_sigmask(SIG_BLOCK, &blocked, &old);

	for (i = 0; i < nw; i++) {
		wargs[i].slot = slot_init(1 + i);
		wargs[i].wid = i;
		pthread_create(&wargs[i].slot->tid, &tattr, worker_main,
			&wargs[i]);
	}
	mt = slot_init(nw + 1);
	pthread_create(&mt->tid, &tattr, maint_main, mt);
	wt = slot_init(nw + 2);
	pthread_create(&wt->tid, &tattr, wal_main, wt);
	if (cfg->wal_dir) {
		rt = slot_init(nw + 3);
		pthread_create(&rt->tid, &tattr, rdb_main, rt);
	}
	if (cfg->cl_enabled) {
		ct = slot_init(nw + 4);
		pthread_create(&ct->tid, &tattr, peer_main, ct);
		bt = slot_init(nw + 5);
		pthread_create(&bt->tid, &tattr, bulk_main, bt);
		hb = slot_init(nw + 6);
		pthread_create(&hb->tid, &tattr, beat_main, hb);
	}
	pthread_attr_destroy(&tattr);

	pthread_sigmask(SIG_SETMASK, &old, NULL);
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	LM_NOTICE("perfcached ready: %d workers, %d collections, %d "
		"listeners, arena %s\n", nw, n_cols, cfg->n_listen,
		cfg->backing_heap ? "heap" : "reserved");

	for (i = 0; i < nw; i++)
		pthread_join(wargs[i].slot->tid, NULL);
	pthread_join(mt->tid, NULL);
	pthread_join(wt->tid, NULL);
	if (rt)
		pthread_join(rt->tid, NULL);
	if (ct)
		pthread_join(ct->tid, NULL);
	if (hb)
		pthread_join(hb->tid, NULL);

	if (unix_fd >= 0) {
		close(unix_fd);
		for (i = 0; i < cfg->n_listen; i++)
			if (cfg->listen[i].type == PC_LISTEN_UNIX)
				unlink(cfg->listen[i].addr);
	}
	pcache_arena_destroy();
	LM_NOTICE("perfcached: clean shutdown\n");
	return 0;
}
