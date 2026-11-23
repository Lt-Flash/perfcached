/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* daemon.c - the threading frame: main, the workers (one epoll or io_uring each), maintenance and
 * the rest; every thread owns an eventfd for wakes and broadcast RPCs. */
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
#include <time.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <netdb.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "iostat.h"
#include "uring.h"
#include "busypoll.h"
#include "compat/compat.h"
#include "quiesce.h"
#include "compat/dprint.h"
#include "compat/timer.h"
#include "compat/ipc.h"
#include "core/pcache_mem.h"
#include "core/pcache_arena.h"
#include "core/pcache_htable.h"
#include "proto.h"
#include "reload.h"
#include <sodium.h>
#include "memguard.h"
#include "pubsub.h"
#include "store.h"
#include "events.h"
#include "storage.h"
#include "walprobe.h"
#include "wal.h"
#include "rdb.h"
#include "recover.h"
#include "ackstate.h"
#include "verbs.h"
#include "cluster.h"
#include "clcid.h"
#include "clterm.h"
#include "obs.h"
#include "metrics.h"
#include "respmap.h"
#include "daemon.h"
#include "rpcq.h"
#include "thrtab.h"
#include "upstream.h"
#include "mail.h"
#include "alerts.h"
#include "addrwait.h"
#include "dirlock.h"
#include "upgrade.h"
#include "pcaddr.h"
#include "doorlive.h"
#include "listenfd.h"
#include "cfgsig.h"
#include "autosize.h"
#include "keeps.h"

/* the WAL directory's resolved storage identity, kept for the stats verb */
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

struct pc_thread {
	pthread_t tid;
	int efd;
	int idx;                            /* this worker's index; also the vendored core's process_no */
	pthread_mutex_t mx;                 /* guards rq */
	struct pc_rpcq rq;
	int live;
	int up;                             /* created: joined if start-up gives up */
	struct pc_conn **conns;             /* this worker's client list, read on this worker only */
};

struct pc_collection_rt {
	const struct pc_collection *conf;
	pcache_htable_t *ht;
};

static struct pc_thread threads[PC_THREAD_SLOTS];
static int n_threads;
static __thread struct pc_thread *self_slot;

static struct pc_collection_rt cols[PC_MAX_COLLECTIONS];
static int n_cols;

static const struct pc_config *CFG;

int pc_http_rate(void)
{
	return CFG ? CFG->http_rate : 0;
}

int pc_http_cache_ms(void)
{
	return CFG ? CFG->http_cache_ms : 0;
}

/* ---- the listeners, as facts stats can carry --------------------------- */
static int listener_plaintext_ok(const struct pc_listener *l);

int pc_listener_count(void)
{
	return CFG ? CFG->n_listen : 0;
}

const struct pc_listener *pc_listener_at(int i)
{
	return CFG && i >= 0 && i < CFG->n_listen ? &CFG->listen[i] : NULL;
}

int pc_listener_plaintext(const struct pc_listener *l)
{
	return l ? listener_plaintext_ok(l) : 0;
}

int pc_listener_allow(const struct pc_listener *l)
{
	if (!CFG || !l)
		return 0;
	const struct pc_live *L = live();

	if (!L)
		return 0;
	return l->http ? L->n_http_allow : l->resp ? L->n_resp_allow : 0;
}

/* the first RESP door, for the wrong-port notice on the native one */
const char *pc_resp_door(void)
{
	static char buf[128];
	int i;

	for (i = 0; CFG && i < CFG->n_listen; i++)
		if (CFG->listen[i].resp && CFG->listen[i].type == PC_LISTEN_TCP) {
			snprintf(buf, sizeof buf, "%s:%d", CFG->listen[i].addr,
				CFG->listen[i].port);
			return buf;
		}
	return NULL;
}

/* the optional collection allow-list for RESP clients: NULL = all; once respmap is up, its
 * effective list (the file's with the runtime overrides applied) */
const char *pc_resp_collections(void)
{
	if (pc_respmap_ready())
		return pc_respmap_effective();
	return CFG ? CFG->resp_collections : NULL;
}
static volatile sig_atomic_t stop_flag;
static volatile int wal_stop;           /* the WAL pump's own: set once its writers are joined */
static volatile int stop_for_files;    /* pc_daemon_restart_from_files */
static volatile int stop_for_worker;   /* a worker left its loop unasked */
static void worker_lost(void);
/* workers whose listeners are open - "ready" waits for all of them */
static int workers_listening;
static int unix_fd = -1;
static struct pc_psk_ctx PSK;
static int have_psk;
/* the PSK table a NEW connection takes (proto.c, at accept); a reload publishes a fresh one and
 * never frees the old - a connection mid-handshake still holds it */
static const struct pc_psk_ctx *PSK_CUR;

const struct pc_psk_ctx *pc_psk_current(void)
{
	return __atomic_load_n(&PSK_CUR, __ATOMIC_ACQUIRE);
}

/* apply a reload: reload.c refused it unless every changed key is a PC_RL_* one, so the new
 * snapshot is simply @nc's.  0, or -1 with @msg set. */
int pc_live_reload(const struct pc_config *nc, unsigned int what,
		char *msg, size_t ml)
{
	struct pc_live *nl;

	if (what & PC_RL_CLIENT) {
		if (!have_psk) {
			snprintf(msg, ml, "client secrets: no encrypted door here, nothing to derive");
		} else {
			struct pc_psk_ctx *t = malloc(sizeof *t);
			int i;

			if (!t) {
				snprintf(msg, ml, "out of memory for the new client keys");
				return -1;
			}
			*t = *pc_psk_current();            /* the cluster key stays */
			memset(t->client, 0, sizeof t->client);
			for (i = 0; i < nc->n_client_secrets; i++)
				if (pc_psk_derive(nc->client_secret[i],
				        strlen(nc->client_secret[i]), PC_PRIN_CLIENT,
				        t->client[i]) != 0) {
					sodium_memzero(t, sizeof *t);
					free(t);
					snprintf(msg, ml, "client secret %d: the key derivation failed", i + 1);
					return -1;
				}
			t->n_client = nc->n_client_secrets;
			__atomic_store_n(&PSK_CUR, t, __ATOMIC_RELEASE);
		}
	}
	if (what & (PC_RL_RESP_ALLOW | PC_RL_HTTP_ALLOW | PC_RL_HTTP_TOKEN |
	            PC_RL_RESP_PASSWORD | PC_RL_ENABLE | PC_RL_AUTH_TLS)) {
		nl = live_from(nc);
		if (!nl) {
			snprintf(msg, ml, "out of memory for the new settings");
			return -1;
		}
		live_install(nl);
	}
	return 0;
}

/* the probe's I/O shares the WAL device with the live pump: fsync latency is perturbed while it
 * runs, and a stats reader may see a torn number mid-update */
int pc_wal_reprobe(int secs, struct pc_wal_policy *pol)
{
	if (!CFG || !CFG->wal_dir)
		return -1;
	if (pc_wal_probe_run(CFG->wal_dir, secs, &wal_probe) != 0)
		return -1;
	pc_wal_policy_from(&wal_probe, wal_id_valid ? &wal_sid : NULL, pol);
	return 0;
}

/* ---- registry + broadcast --------------------------------------------- */

static struct pc_thread *slot_init(int idx)
{
	int k = pc_thread_slot_take(&n_threads, PC_THREAD_SLOTS);
	struct pc_thread *t;

	if (k < 0)
		return NULL;
	t = &threads[k];
	t->idx = idx;
	t->efd = eventfd(0, EFD_NONBLOCK);
	if (t->efd < 0)
		return NULL;
	pthread_mutex_init(&t->mx, NULL);
	return t;
}

/* the thread's role in its name, for top -H, perf and cores (15 bytes + NUL); prctl, not
 * pthread_setname_np, which would need _GNU_SOURCE */
static void thread_name(const char *name)
{
#ifdef __linux__
	char buf[16];

	snprintf(buf, sizeof buf, "%s", name);
	prctl(PR_SET_NAME, (unsigned long)buf, 0, 0, 0);
#else
	(void)name;
#endif
}

static void slot_attach(struct pc_thread *t)
{
	self_slot = t;
	t->tid = pthread_self();
	compat_thread_register(t->idx);
	pc_qs_attach();                     /* a quiescence line */
	__atomic_store_n(&t->live, 1, __ATOMIC_RELEASE);
}

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
		if (!pc_rpcq_take(&t->rq, &fn, &arg)) {
			pthread_mutex_unlock(&t->mx);
			return;
		}
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
		(void)pc_rpcq_put(&t->rq, fn, param);
		pthread_mutex_unlock(&t->mx);
		if (write(t->efd, &one, sizeof one) < 0) { /* wake is best-effort */ }
	}
}

/* ---- membership notifications to every client -------------------------- */

struct notify_msg {
	int refs;                           /* one per queued worker */
	size_t n;
	char buf[];
};

static void notify_release(struct notify_msg *m)
{
	if (__atomic_sub_fetch(&m->refs, 1, __ATOMIC_ACQ_REL) == 0)
		free(m);
}

/* the caller holds a reference for as long as this runs */
static void notify_deliver(const struct notify_msg *m)
{
	/* self_slot, NOT threads[idx]: a worker's public idx is 1 + its slot, so threads[idx] is the
	 * NEXT worker's connection list, written from the wrong thread */
	struct pc_thread *t = self_slot;

	if (t && t->conns)
		pc_conn_notify_all(*t->conns, m->buf, m->n);
}

/* a queued delivery owns the reference it was queued with */
static void notify_rpc(int idx, void *arg)
{
	(void)idx;
	notify_deliver(arg);
	notify_release(arg);
}

void pc_clients_notify(const char *payload, size_t n)
{
	struct notify_msg *m = malloc(sizeof *m + n);
	uint64_t one = 1;
	int i;

	if (!m)
		return;
	memcpy(m->buf, payload, n);
	m->n = n;
	m->refs = 1;                        /* the caller's, released below */
	for (i = 0; i < n_threads; i++) {
		struct pc_thread *t = &threads[i];

		if (!__atomic_load_n(&t->live, __ATOMIC_ACQUIRE) || !t->conns)
			continue;
		if (t == self_slot) {
			/* synchronous: the caller's reference covers it, so
			 * no count moves and nothing can free it mid-loop */
			notify_deliver(m);
			continue;
		}
		pthread_mutex_lock(&t->mx);
		__atomic_add_fetch(&m->refs, 1, __ATOMIC_ACQ_REL);
		if (!pc_rpcq_put(&t->rq, notify_rpc, m))
			__atomic_sub_fetch(&m->refs, 1, __ATOMIC_ACQ_REL);
		pthread_mutex_unlock(&t->mx);
		if (write(t->efd, &one, sizeof one) < 0) { /* best effort */ }
	}
	notify_release(m);
}

/* ---- worker ------------------------------------------------------------ */

struct worker_arg {
	struct pc_thread *slot;
	int wid;
};

/* only loopback/unix listeners under plaintext=loopback skip the Noise channel - and RESP and
 * HTTP, plaintext by construction (their clients cannot speak Noise), kept narrow instead */
static int listener_plaintext_ok(const struct pc_listener *l)
{
	if (l->resp)
		return 1;
	if (l->http)
		return 1;
	if (CFG->plaintext != PC_PLAINTEXT_LOOPBACK)
		return 0;
	return l->loopback;
}

/* what pc_conn_accept's plaintext_ok gets for @l */
static int listener_door(const struct pc_listener *l)
{
	if (listener_plaintext_ok(l))
		return PC_DOOR_PLAIN;
	return (l->encrypt >= 0 ? l->encrypt : CFG->listen_encrypt) ? 0 : PC_DOOR_AUTH;
}

/* the worker's eventfd fired: RPCs, pub/sub, upstream completions, the
 * cluster's pulls - whichever backend woke it */
static void worker_efd(struct worker_arg *wa)
{
	struct pc_pull_done done[16];
	uint64_t junk;
	int nd, k;

	while (read(wa->slot->efd, &junk, sizeof junk) > 0)
		;
	rpc_drain(wa->slot);
	pc_pubsub_drain(wa->slot->idx);
	pc_upstream_drain(wa->slot->idx);
	while ((nd = pc_cluster_drain(wa->slot->idx, done, 16)) > 0)
		for (k = 0; k < nd; k++) {
			pc_proto_pull_complete(&done[k]);
			free(done[k].val);
		}
}

/* the clock either side of a worker's wait - the vDSO, no system
 * call; what lies between two waits is the worker's busy time */
static unsigned long long wclock(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ULL +
		(unsigned long long)ts.tv_nsec;
}

/* the io_uring worker loop: the wait is an io_uring_enter() that also submits the last turn's
 * requests; a listener its accept budget stopped is retried next turn without waiting */
static void worker_loop_uring(struct worker_arg *wa, struct pc_ur *ur,
		struct pc_conn **conns, int nlfd, const int *lfds,
		const int *plok, const int *lresp)
{
	unsigned char larmed[PC_MAX_LISTEN + 1] = { 0 };
	unsigned char lmore[PC_MAX_LISTEN + 1] = { 0 };
	unsigned char ldone[PC_MAX_LISTEN + 1];
	struct pc_ur_cqe *e;
	struct pc_ur_sqe *s;
	unsigned int hs_deferred;
	int efd_armed = 0, any_more, tmo, rc, hs, j, ncq;
	unsigned long long t_wake = wclock(), t_wait;

	pc_conn_uring_attach(ur);
	while (!stop_flag) {
		pc_conn_resume_paused();
		pc_conn_resume_out();
		pc_conn_resume_wal();
		pc_pubsub_flush_wakes();
		pc_conn_flush_pushes();
		pc_qs_exit();
		pc_conn_reap();
		pc_conn_ur_retry();
		if (!efd_armed && (s = pc_ur_sqe(ur)) != NULL) {
			pc_ur_prep_poll_multi(s, wa->slot->efd, POLLIN,
				PC_UR_UD_EFD);
			efd_armed = 1;
		}
		any_more = 0;
		for (j = 0; j < nlfd; j++) {
			if (!larmed[j] && (s = pc_ur_sqe(ur)) != NULL) {
				pc_ur_prep_poll_multi(s, lfds[j], POLLIN,
					PC_UR_UD_LISTEN(j));
				larmed[j] = 1;
			}
			any_more |= lmore[j];
		}
		tmo = pc_enum_pending() || pc_conn_hs_parked() || any_more ? 0
			: pc_conn_wal_paused_open() ? 1
			: pc_conn_paused_open() || pc_conn_deny_open() ? 50
			: (pc_conn_http_open() || pc_conn_udp_open() ||
			   pc_conn_bufs_held() || pc_conn_out_paused_open() ? 1000 : -1);
		t_wait = wclock();             /* busy until here */
		PC_IOBUSY_ADD(busy_ns, t_wait - t_wake);
		rc = pc_ur_submit(ur, tmo != 0, tmo);
		t_wake = wclock();
		PC_IOBUSY_ADD(wait_ns, t_wake - t_wait);
		PC_IOSTAT_ADD(ur_enters, 1);
		pc_qs_enter();
		pc_conn_sweep_idle(conns);
		pc_conn_sweep_http(conns, CFG->http_timeout_s);
		pc_conn_sweep_udp(conns);
		pc_conn_sweep_deny();          /* refused HTTP peers */
		if (rc < 0 && rc != -ETIME && rc != -EINTR && rc != -EBUSY &&
		    rc != -EAGAIN) {
			LM_CRIT("worker %d: io_uring_enter: %s - the worker "
				"stops, and the daemon with it\n", wa->wid, strerror(-rc));
			worker_lost();
			break;
		}
		hs = pc_conn_hs_run(PC_HS_PER_TURN);
		hs_deferred = 0;
		memset(ldone, 0, sizeof ldone);
		/* a bounded batch: what completes while it runs waits for the
		 * next turn, behind the turn's own housekeeping */
		for (ncq = 0; ncq < 4096 && (e = pc_ur_cqe_peek(ur)) != NULL;
		     ncq++) {
			uint64_t ud = e->user_data;
			int res = e->res;
			uint32_t fl = e->flags;

			pc_ur_cqe_seen(ur);    /* before: a handler may submit */
			if (ud >= 65536 && ud != PC_UR_UD_PROVIDE) {
				pc_conn_ur_complete(ud, res, fl, &hs, &hs_deferred);
				continue;
			}
			if (ud == PC_UR_UD_EFD) {
				if (!(fl & PC_UR_CQE_F_MORE))
					efd_armed = 0;
				worker_efd(wa);
				continue;
			}
			if (ud >= PC_UR_UD_LISTEN(0) &&
			    ud < PC_UR_UD_LISTEN(nlfd)) {
				j = (int)(ud - PC_UR_UD_LISTEN(0));
				if (!(fl & PC_UR_CQE_F_MORE))
					larmed[j] = 0;
				if (!ldone[j]) {
					ldone[j] = 1;
					lmore[j] = (unsigned char)pc_conn_accept(-1,
						lfds[j], plok[j], lresp[j],
						have_psk ? &PSK : NULL, conns);
				} else {
					/* posted after this turn's accept: edge-triggered, ask again */
					lmore[j] = 1;
				}
				continue;
			}
			/* PC_UR_UD_IGNORE, PC_UR_UD_PROVIDE: cancels, buffers */
		}
		pc_ur_cqe_flush(ur);
		PC_IOSTAT_ADD(ur_cqes, (unsigned)ncq);
		for (j = 0; j < nlfd; j++)
			if (lmore[j] && !ldone[j])
				lmore[j] = (unsigned char)pc_conn_accept(-1,
					lfds[j], plok[j], lresp[j],
					have_psk ? &PSK : NULL, conns);
		if (hs_deferred)
			pc_conn_hs_deferred(hs_deferred);
		pc_enum_step();
	}
	pc_conn_destroy_all(conns);
	pc_ur_submit(ur, 0, 0);                /* their cancels */
}

static void *worker_main(void *p)
{
	struct worker_arg *wa = p;
	struct epoll_event ev, evs[32];
	struct pc_conn *conns = NULL;
	int ep, i, j, n, nlfd = 0, hs, ewait;
	unsigned int hs_deferred;
	int lfds[PC_MAX_LISTEN + 1], plok[PC_MAX_LISTEN + 1];
	int lresp[PC_MAX_LISTEN + 1];
	struct pc_ur ur;
	int use_uring = 0;
	unsigned long long t_wake, t_wait;

	slot_attach(wa->slot);
	{
		char wn[16];

		snprintf(wn, sizeof wn, "pc-w%d", wa->wid);
		thread_name(wn);
	}
	wa->slot->conns = &conns;           /* read on this thread only */
	pc_cluster_worker_register(wa->slot->idx, wa->slot->efd);
	pc_pubsub_worker_register(wa->slot->idx, wa->slot->efd);
	pc_upstream_worker_register(wa->slot->idx, wa->slot->efd);
	pcache_ht_shadow_reads(1);          /* client reads only */
	pc_conn_push_batching();
	pc_pubsub_defer_wakes();
	/* this worker's backend - io_uring where the start's check
	 * passed and this worker's own ring comes up, epoll otherwise */
	if (pc_io_backend.active == PC_IO_URING) {
		char err[200];

		if (pc_ur_init(&ur, 1024, 16384, err, sizeof err) == 0 &&
		    pc_ur_bufs_init(&ur, PC_UR_BGID, PC_UR_WORKER_BUFS,
			PC_UR_WORKER_BUF_SIZE, err, sizeof err) == 0)
			use_uring = 1;
		else {
			if (ur.fd >= 0)
				pc_ur_free(&ur);
			LM_WARN("worker %d: its io_uring ring did not come up "
				"(%s) - this worker uses epoll\n", wa->wid, err);
		}
	}
	ep = use_uring ? -1 : epoll_create1(0);
	if (CFG->busy_poll_us > 0) {
		char err[200];
		int rc = use_uring
			? pc_bp_ring(&ur, (unsigned)CFG->busy_poll_us,
				CFG->busy_poll_prefer, err, sizeof err)
			: pc_bp_epoll(ep, (unsigned)CFG->busy_poll_us,
				CFG->busy_poll_prefer, err, sizeof err);

		if (rc == 0) {
			if (__atomic_add_fetch(&pc_io_backend.bp_workers, 1,
			        __ATOMIC_RELAXED) == 1)
				LM_NOTICE("busy poll: %d us%s on the workers (%s)\n",
					CFG->busy_poll_us, CFG->busy_poll_prefer
					? ", preferred over the device's interrupts"
					: "", use_uring ? "io_uring NAPI" : "epoll");
		} else if (!__atomic_exchange_n(&pc_io_backend.bp_refused, 1,
		        __ATOMIC_ACQ_REL)) {
			snprintf(pc_io_backend.bp_why, sizeof pc_io_backend.bp_why,
				"%s", err);
			LM_WARN("busy_poll_us = %d, but this host refuses it - the "
				"workers run without busy polling: %s\n",
				CFG->busy_poll_us, err);
		}
	}

	/* epoll data tagging: fds (efd/listeners) ride data.u64 as small
	 * ints; connections ride data.ptr - heap pointers are never < 64K */
	ev.events = EPOLLIN;
	ev.data.u64 = (uint64_t)wa->slot->efd;
	if (ep >= 0)
		epoll_ctl(ep, EPOLL_CTL_ADD, wa->slot->efd, &ev);

	/* own SO_REUSEPORT socket per TCP listener - the kernel balances */
	for (i = 0; i < CFG->n_listen; i++) {
		if (CFG->listen[i].type != PC_LISTEN_TCP)
			continue;
		lfds[nlfd] = tcp_listener_fd(&CFG->listen[i]);
		if (lfds[nlfd] < 0)
			continue;                  /* failure already logged */
		plok[nlfd] = listener_door(&CFG->listen[i]);
		lresp[nlfd] = (CFG->listen[i].http ? PC_LK_HTTP :
			CFG->listen[i].resp ? PC_LK_RESP : PC_LK_NATIVE) |
			(CFG->listen[i].tls ? PC_LK_TLS : 0);
		ev.events = EPOLLIN;
		ev.data.u64 = (uint64_t)lfds[nlfd];
		if (ep >= 0)
			epoll_ctl(ep, EPOLL_CTL_ADD, lfds[nlfd], &ev);
		nlfd++;
	}
	/* the single shared UNIX listener: EPOLLEXCLUSIVE avoids the herd */
	if (unix_fd >= 0) {
		lfds[nlfd] = unix_fd;
		plok[nlfd] = 0;    /* default-deny; the loop below sets the configured value */
		lresp[nlfd] = PC_LK_NATIVE;
		                   /* the RESP door is TCP-only */
		for (i = 0; i < CFG->n_listen; i++)
			if (CFG->listen[i].type == PC_LISTEN_UNIX)
				plok[nlfd] = listener_door(&CFG->listen[i]);
		ev.events = EPOLLIN | EPOLLEXCLUSIVE;
		ev.data.u64 = (uint64_t)unix_fd;
		if (ep >= 0)
			epoll_ctl(ep, EPOLL_CTL_ADD, unix_fd, &ev);
		nlfd++;
	}
	/* this worker's doors accept from here on (a listener that failed
	 * to open was logged above and counts as done) */
	__atomic_add_fetch(&workers_listening, 1, __ATOMIC_RELEASE);

	if (use_uring) {
		__atomic_add_fetch(&pc_io_backend.workers_uring, 1,
			__ATOMIC_RELAXED);
		worker_loop_uring(wa, &ur, &conns, nlfd, lfds, plok, lresp);
		pc_qs_exit();
		for (i = 0; i < nlfd; i++)
			if (lfds[i] != unix_fd)
				close(lfds[i]);
		/* the ring goes; its buffers stay - a receive cancelled by
		 * the ring's own teardown could still land in them */
		ur.bufs = NULL;
		pc_ur_free(&ur);
		return NULL;
	}

	t_wake = wclock();
	while (!stop_flag) {
		/* the last turn's pub/sub work before it parks: its wakes to other workers, then its staged
		 * frames - every path back to the wait passes here */
		pc_conn_resume_paused();
		pc_conn_resume_out();
		pc_conn_resume_wal();
		pc_pubsub_flush_wakes();
		pc_conn_flush_pushes();
		pc_qs_exit();                  /* parked across the wait */
		pc_conn_reap();                /* no batch in flight here */
		/* an active walk polls; an open HTTP connection (its head may never arrive) or UDP stream bounds
		 * the wait - read HERE, so one opened by this turn's events shortens it */
		t_wait = wclock();             /* busy until here */
		PC_IOBUSY_ADD(busy_ns, t_wait - t_wake);
		n = epoll_wait(ep, evs, 32,
			pc_enum_pending() || pc_conn_hs_parked() ? 0
			: pc_conn_wal_paused_open() ? 1
			: pc_conn_paused_open() || pc_conn_deny_open() ? 50
			: (pc_conn_http_open() || pc_conn_udp_open() ||
			   pc_conn_bufs_held() || pc_conn_out_paused_open() ? 1000 : -1));
		ewait = errno;                 /* the sweeps below make system calls */
		t_wake = wclock();
		PC_IOBUSY_ADD(wait_ns, t_wake - t_wait);
		if (n >= 0) {                  /* waits vs events */
			PC_IOSTAT_ADD(ep_waits, 1);
			PC_IOSTAT_ADD(ep_events, (unsigned)n);
		}
		pc_qs_enter();                 /* the turn: events, drains, walks */
		pc_conn_sweep_idle(&conns);    /* first - it stamps the tick */
		pc_conn_sweep_http(&conns, CFG->http_timeout_s);
		pc_conn_sweep_udp(&conns);
		pc_conn_sweep_deny();          /* refused HTTP peers */
		if (n < 0) {
			if (ewait == EINTR)
				continue;
			LM_CRIT("worker %d: epoll_wait: %s - the worker stops, and the "
				"daemon with it\n", wa->wid, strerror(ewait));
			worker_lost();
			break;
		}
		/* the handshakes put off earlier first, oldest first;
		 * past the turn's budget a new one is parked behind them */
		hs = pc_conn_hs_run(PC_HS_PER_TURN);
		hs_deferred = 0;
		for (i = 0; i < n; i++) {
			if (evs[i].data.u64 >= 65536) {
				if (pc_conn_hs_pending(evs[i].data.ptr, evs[i].events) &&
				    hs++ >= PC_HS_PER_TURN) {
					pc_conn_hs_park(evs[i].data.ptr);
					hs_deferred++;
					continue;
				}
				pc_conn_event(evs[i].data.ptr, evs[i].events);
				continue;
			}
			if ((int)evs[i].data.u64 == wa->slot->efd) {
				worker_efd(wa);
				continue;
			}
			for (j = 0; j < nlfd; j++)
				if (lfds[j] == (int)evs[i].data.u64) {
					(void)pc_conn_accept(ep, lfds[j], plok[j],
						lresp[j],
						have_psk ? &PSK : NULL, &conns);
					break;
				}
		}
		if (hs_deferred)
			pc_conn_hs_deferred(hs_deferred);
		pc_enum_step();
	}
	pc_conn_destroy_all(&conns);
	pc_qs_exit();
	for (i = 0; i < nlfd; i++)
		if (lfds[i] != unix_fd)
			close(lfds[i]);
	close(ep);
	return NULL;
}

/* the splitter's per-tick bound: a CHUNK is one call into the core's grow loop, the SLICE how long
 * one collection may split, since expiry, reclaim and the resize tick queue behind it */
int pc_grow_at_pct = 75;                 /* [daemon] grow_at_pct */
int pc_shrink_at_pct = 18;               /* [daemon] shrink_at_pct, resolved */
int pc_shrink_cooloff_s = 60;            /* [daemon] shrink_cooloff_s */
/* a table holding more than leg_stuck_pct of its records in chains grows past grow_at_pct, as far
 * as grow_floor_pct: only a wider table empties full buckets' chains */
int pc_grow_floor_pct = 50;              /* [daemon] grow_floor_pct */
int pc_leg_stuck_pct = 1;                /* [daemon] leg_stuck_pct */
#define PC_GROW_CHUNK     512
#define PC_EVAC_BUCKETS   4096         /* buckets per second */
#define PC_EVAC_BUDGET    512          /* records moved per second, at most */
#define PC_GROW_SLICE_US  20000

/* ---- maintenance ------------------------------------------------------- */

/* the sweep names each reaped record, for the event log; the cluster keeps it as a
 * delete at that version */
static void ev_expired_cb(const str *key, unsigned long long ver,
		unsigned char rflags, void *ctx)
{
	/* the copy a running resize already took would come back at the swap */
	pcache_htable_t *sh = pc_store_resize_shadow(pc_store_ht((int)(long)ctx));

	if (sh)
		pcache_ht_remove_below(sh, key, ver + 1);
	pc_ev_expired((int)(long)ctx, key);
	pc_cluster_note_expired((int)(long)ctx, key->s, key->len, ver,
		(rflags & PCACHE_F_SHORTENED) != 0);
}

/* a peer's fleet-wide stats reset (M_STATS_RESET), as of its time */
static void stats_reset_peer(long long at)
{
	(void)pc_stats_reset_at(at);
}

/* split in chunks until the table has caught up (a short chunk) or the slice is spent; only AUTO
 * and FLOOR size the table - under WARN and OFF keys past its slots go to chains */
static long long grow_slice(pcache_htable_t *cht, long long budget_us)
{
	struct timespec g0, g1;
	unsigned int did;
	long long us = 0;

	clock_gettime(CLOCK_MONOTONIC, &g0);
	do {
		did = pcache_ht_grow_at_leg(cht, (unsigned int)pc_grow_at_pct,
			(unsigned int)pc_grow_floor_pct,
			(unsigned int)pc_leg_stuck_pct, PC_GROW_CHUNK);
		clock_gettime(CLOCK_MONOTONIC, &g1);
		us = (long long)(g1.tv_sec - g0.tv_sec) * 1000000LL +
			(g1.tv_nsec - g0.tv_nsec) / 1000;
	} while (did == PC_GROW_CHUNK && us < budget_us);
	return us;
}

/* a split re-lays its bucket's whole chain, so a table past the leg_stuck_pct line also splits
 * at the loop's 100 ms cadence until it has caught up */
static long long grow_behind(int i, long long budget_us)
{
	pcache_htable_t *cht = pc_store_ht(i);
	pcache_ht_totals_t t;
	unsigned long rg0, ch;
	long long us;

	if (!as_sizes(i) || !(ch = pcache_ht_overflow(cht)))
		return 0;
	pcache_ht_totals(cht, &t);
	if (ch * 100UL <= (unsigned long)pc_leg_stuck_pct * t.entries)
		return 0;
	rg0 = pcache_arena_regions_bytes();
	us = grow_slice(cht, budget_us);
	if (pcache_arena_regions_bytes() > rg0)
		pc_store_note_index(cht, pcache_arena_regions_bytes() - rg0);
	return us;
}

static void *maint_main(void *p)
{
	unsigned int last = 0, now, win_built = 0, grow_rr = 0;
	long long gleft;
	int i, k, n;

	slot_attach(p);
	thread_name("pc-maint");
	while (!stop_flag) {
		pc_qs_exit();                  /* parked across the sleep */
		usleep(100 * 1000);
		pc_qs_enter();
		rpc_drain(self_slot);
		/* the commit-ahead window at THIS loop's 100 ms cadence: a bulk load crosses a 2 MB group
		 * every ~50 ms, and a 1 Hz refill left the crossings to the worker holding the write */
		pcache_arena_ahead_tick();
		/* the cluster thread's term and cluster id: a promotion waits on the term's write */
		(void)pc_term_persist_tick();
		(void)pc_cid_persist_tick();
		/* one slice a pass for every collection, the first in turn */
		gleft = PC_GROW_SLICE_US;
		n = pc_store_count();
		for (k = 0; k < n && gleft > 0; k++) {
			i = (int)((grow_rr + (unsigned int)k) % (unsigned int)n);
			if (pc_store_live(i))
				gleft -= grow_behind(i, gleft);
		}
		grow_rr++;
		now = get_ticks();
		if (now == last)
			continue;
		last = now;
		pc_obs_tick_1hz();
		pc_stats_reset_tick();         /* the daily (N-day) reset */
		pc_doors_widen();              /* no door total wraps unseen */
		pc_memguard_tick();            /* the client guard, the trim */
		pc_iobusy_tick();              /* before the alerts read it */
		pc_alerts_tick();              /* the node's own alerts */
		pc_upg_node_tick();            /* the helper's READY */
		/* build /stats once a minute and throw it away: it takes each five-minute window's snapshot
		 * when nobody reads the page (winctr.h) */
		if (now - win_built >= 60) {
			struct pc_tw wt;

			win_built = now;
			pc_tw_init(&wt, NULL, 0);
			pc_stats_tree(&wt, NULL);
			pc_tw_free(&wt);
		}
		pc_store_held_tick();          /* the paced walk */
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
		pc_store_resize_tick();        /* a migration in flight */
		pc_store_persist_tick();       /* what the cluster thread applied for the fleet */
		pc_respmap_persist_tick();
		pc_store_retire_tick();        /* what a resize left, once no thread can still hold it */
		pc_ev_tick();                  /* refill, report suppression */
		pc_store_hot_tick();           /* the hot-key counts halve each minute */
		pc_reload_tick();              /* a SIGHUP's reload */
		pc_live_retire_tick();         /* free what a reload replaced */
		/* the REGISTRY, not the startup array: a runtime collection needs its sweep and splitter
		 * too, and a dropped one must not be swept */
		gleft = PC_GROW_SLICE_US;
		for (i = 0; i < pc_store_count(); i++) {
			unsigned long rg0;
			pcache_htable_t *cht;

			if (!pc_store_live(i))
				continue;
			rg0 = pcache_arena_regions_bytes();
			cht = pc_store_ht(i);
			pcache_ht_sweep(cht, now, ev_expired_cb, (void *)(long)i);
			if (as_sizes(i) && gleft > 0)
				gleft -= grow_slice(cht, gleft);
			/* move a mass expiry's survivors out of the sparse chunks they pin, so those chunks can
			 * retire; nothing to do when no chunk is sparse (the ordinary case) */
			if (pcache_arena_sparse_chunks())
				pcache_ht_evacuate(cht, PC_EVAC_BUCKETS, PC_EVAC_BUDGET);
			if (pcache_arena_regions_bytes() > rg0)
				pc_store_note_index(cht,
					pcache_arena_regions_bytes() - rg0);
			if (as_sizes(i))
				shrink_consider(i, cht, now,
					pcache_arena_regions_bytes() > rg0);
			autoscale_advise(i, cht, now);
		}
	}
	pc_store_persist_tick();               /* a fleet change applied just before the stop */
	pc_respmap_persist_tick();
	(void)pc_term_persist_tick();
	(void)pc_cid_persist_tick();
	return NULL;
}

/* ---- peer thread (spawned only when clustered) -------------------------- */

static void *peer_main(void *p)
{
	slot_attach(p);
	thread_name("pc-cluster");
	pc_cluster_thread((volatile int *)&stop_flag);
	return NULL;
}

/* the bulk migration thread: TCP transfers of records too big for the
 * datagram plane (cluster principal Noise sessions, both directions) */
static void *bulk_main(void *p)
{
	slot_attach(p);
	thread_name("pc-bulk");
	pc_bulk_thread((volatile int *)&stop_flag);
	return NULL;
}

/* the heartbeat watchdog: keeps the node audible when the peer thread
 * is heads-down in a long duty (emission only - see cluster.h) */
static void *beat_main(void *p)
{
	slot_attach(p);
	thread_name("pc-beat");
	pc_cluster_beat_thread((volatile int *)&stop_flag);
	return NULL;
}
/* a pub/sub relay receive thread */
struct psrx_arg {
	struct pc_thread *slot;
	int idx;
};
static void *psrx_main(void *p)
{
	struct psrx_arg *a = p;

	slot_attach(a->slot);
	thread_name("pc-psrx");
	pc_cluster_pubsub_rx_thread((volatile int *)&stop_flag, a->idx);
	return NULL;
}
/* a replica-apply thread */
static void *apply_main(void *p)
{
	struct psrx_arg *a = p;

	slot_attach(a->slot);
	thread_name("pc-apply");
	pc_cluster_apply_thread((volatile int *)&stop_flag, a->idx);
	return NULL;
}
/* ---- RDB thread (spawned only with persistence on) --------------------- */

static void *rdb_main(void *p)
{
	slot_attach(p);
	thread_name("pc-rdb");
	pc_rdb_thread((volatile int *)&stop_flag);
	return NULL;
}

/* ---- WAL thread --------------------------------------------------------- */

static void *wal_main(void *p)
{
	struct pollfd pf;
	int timeout = 200;

	slot_attach(p);
	thread_name("pc-wal");
	pc_wal_set_wakeup(self_slot->efd);
	pf.fd = self_slot->efd;
	pf.events = POLLIN;
	while (!__atomic_load_n(&wal_stop, __ATOMIC_ACQUIRE)) {
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

/* the pump's last drain, once nothing can append any more */
static void wal_stop_join(struct pc_thread *t)
{
	uint64_t one = 1;

	__atomic_store_n(&wal_stop, 1, __ATOMIC_RELEASE);
	if (write(t->efd, &one, sizeof one) < 0) { /* best effort: the poll times out */ }
	pthread_join(t->tid, NULL);
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

void pc_daemon_restart_from_files(void)
{
	stop_for_files = 1;
	on_signal(SIGTERM);
}

/* a worker out of its loop leaves its door shut and its registrations undrained: the daemon stops */
static void worker_lost(void)
{
	stop_for_worker = 1;
	on_signal(SIGTERM);
}

/* SIGHUP asks for a config reload, which the maintenance thread runs; the signal's
 * default action would end the daemon */
static void on_hup(int sig)
{
	(void)sig;
	pc_reload_ask();
}

/* -1 when @t has no slot or eventfd, or the thread cannot be created: said, by its role */
static int spawn(struct pc_thread *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg,
		const char *what)
{
	int e = t ? pthread_create(&t->tid, a, fn, arg) : 0;

	if (!t || e) {
		LM_CRIT("could not start the %s thread: %s - refusing to start\n", what,
			t ? strerror(e) : "no thread slot or eventfd");
		return -1;
	}
	t->up = 1;
	return 0;
}

/* a start given up: the threads already running see the stop and are joined */
static void spawn_unwind(void)
{
	int i;

	__atomic_store_n(&wal_stop, 1, __ATOMIC_RELEASE);
	on_signal(SIGTERM);
	for (i = 0; i < n_threads; i++)
		if (threads[i].up)
			pthread_join(threads[i].tid, NULL);
}

/* ---- the run ----------------------------------------------------------- */

/* io = io_uring (the default) runs only where uring.c's functional check passes; a default
 * that falls back to epoll is a NOTICE, a configured io_uring that does is a WARNING */
static void io_decide(const struct pc_config *cfg)
{
	struct pc_io_backend *b = &pc_io_backend;
	struct pc_uring_verdict v;

	memset(b, 0, sizeof *b);
	b->configured = cfg->io_backend;
	b->explicit_set = cfg->io_explicit;
	b->workers = cfg->workers;
	b->active = PC_IO_EPOLL;
	b->bp_us = cfg->busy_poll_us;
	b->bp_prefer = cfg->busy_poll_prefer;
	pc_uring_check(&v);
	b->uring_ok = v.ok;
	b->uring_tier = v.buf_tier;
	snprintf(b->uring_why, sizeof b->uring_why, "%s", v.ok ? "" : v.why);
	if (cfg->io_backend == PC_IO_EPOLL) {
		/* the lever this node has, said once - its epoll-busy
		 * alert repeats it when the workers run out of time */
		if (v.ok)
			LM_NOTICE("io: epoll (io = epoll) - io_uring is usable on "
				"this host (%s)\n", pc_ur_buf_tier_name(v.buf_tier));
		else
			LM_NOTICE("io: epoll (io = epoll) - io_uring is not usable "
				"on this host either: %s\n", v.why);
		return;
	}
	if (!v.ok) {
		snprintf(b->why, sizeof b->why, "%s", v.why);
		if (cfg->io_explicit)
			LM_WARN("io = io_uring, but this host cannot run it - "
				"the workers use epoll: %s\n", v.why);
		else
			LM_NOTICE("io: epoll - io_uring (the default) is not "
				"usable here: %s\n", v.why);
		return;
	}
	b->buf_tier = v.buf_tier;
	b->counts_iowait = v.counts_iowait;
	b->active = PC_IO_URING;
	LM_NOTICE("io: io_uring (%s; %u KB of receive buffers a worker; "
		"ring flags 0x%x)\n", pc_ur_buf_tier_name(v.buf_tier),
		PC_UR_WORKER_BUFS * PC_UR_WORKER_BUF_SIZE / 1024, v.setup_flags);
	if (v.counts_iowait)
		LM_NOTICE("io: this kernel counts the workers' waits for the "
			"network as iowait (no opt-out before 6.15) - iowait and "
			"PSI io pressure read high; it is not disk I/O\n");
}

int pc_daemon_run(struct pc_config *cfg)
{
	struct worker_arg wargs[512];
	struct pc_thread *mt = NULL, *wt = NULL, *rt = NULL, *ct = NULL, *bt = NULL;
	struct pc_thread *hb = NULL;
	struct psrx_arg psrx[16];
	struct psrx_arg apth[16];
	int npsrx = 0, napply = 0;
	struct sigaction sa;
	sigset_t blocked, old;
	pthread_attr_t tattr;
	int i, nw = cfg->workers, bad = 0;
	long fd_limit = 0, fd_reserve = 0;      /* logged with the memory guard's figure */

	/* from the first line: a reload asked during recovery waits for maint, a reset peer is an
	 * EPIPE - either signal's default action would end the daemon; recovery's reads restart */
	memset(&sa, 0, sizeof sa);
	sa.sa_flags = SA_RESTART;
	sa.sa_handler = on_hup;
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	CFG = cfg;
	__atomic_store_n(&LIVE, live_from(cfg), __ATOMIC_RELEASE);
	if (!LIVE) {
		LM_CRIT("out of memory for the reloadable settings\n");
		return 1;
	}
	io_decide(cfg);
	{                                       /* the IPv6 guard, said once */
		const char *why;

		if (!pc_ipv6_usable(&why))
			LM_NOTICE("net: IPv6 is not usable on this host - %s: the "
				"listeners and every name resolved are IPv4 only (no "
				"AAAA query goes out)\n", why);
	}

	/* memory first: probe the ladder, size the arena from config */
	if (cfg->huge_pages_off) {
		pcache_mem_no_huge = 1;
		/* the advice is not enough on `enabled = always`: the kernel folds anonymous pages
		 * unasked, so the process says no once, before anything is mapped */
		if (prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0) != 0)
			LM_WARN("huge_pages = off: the kernel refused "
				"PR_SET_THP_DISABLE (%s) - the arena asks for "
				"no huge pages, but THP may still fold it\n",
				strerror(errno));
	}
	pcache_mem_probe();
	LM_NOTICE("memory: probed tier = %s (hugetlb %lukB, thp %lukB)\n",
		pcache_mem_tier_str(pcache_mem.tier), pcache_mem.hugetlb_kb,
		pcache_mem.thp_pmd_kb);
	pcache_backing_policy = "own";
	pcache_arena_hugepage_mb = cfg->backing_heap ? 0 : cfg->arena_mb;
	pcache_arena_hugepage_cap_mb = cfg->arena_cap_mb;
	/* the HARD ceiling: past the huge-page reservation the arena carves from plain malloc, so
	 * without it a node grows unbounded (arena_cap_mb 0 = fixed at arena_mb) */
	pcache_arena_max_bytes = (unsigned long)(cfg->arena_cap_mb
		? cfg->arena_cap_mb : cfg->arena_mb) << 20;
	pcache_reclaim_keep = cfg->reclaim_keep;
	pcache_reclaim_quiet_s = cfg->reclaim_quiet_s;
	pcache_reclaim_cooloff_s = cfg->reclaim_cooloff_s;
	pcache_reclaim_giveback = cfg->reclaim_giveback;
	pcache_arena_floor_bytes = (unsigned long)cfg->reclaim_floor_mb << 20;
	pcache_arena_shrink_step_bytes = (unsigned long)cfg->shrink_step_mb << 20;
	if (pcache_arena_init() != 0) {
		LM_ERR("arena init failed\n");
		return 1;
	}
	if (cfg->pin_off) {                     /* memory given back over held */
		if (pcache_arena_unpin())
			LM_NOTICE("[memory] pin = no: the arena is not locked - the kernel may "
				"swap it, and idle groups go back to it (reclaim_*)\n");
		else
			LM_NOTICE("[memory] pin = no: the arena is on reserved huge pages, which "
				"are never swapped or given back - huge_pages = off for an arena "
				"that gives memory back\n");
	}
	/* pinning degrades to a warning, so a node can believe it is resident while swappable;
	 * `pin = require` refuses instead, naming RLIMIT_MEMLOCK (a container's cap beats the unit's) */
	if (cfg->pin_require) {
		struct pcache_arena_pressure pr;
		struct rlimit rl;

		pcache_arena_pressure(&pr);
		if (!pr.locked_bytes) {
			getrlimit(RLIMIT_MEMLOCK, &rl);
			LM_ERR("[memory] pin = require, but the arena is not "
				"locked: RLIMIT_MEMLOCK is %lu MB and the arena "
				"asks for %d MB. Raise it (systemd "
				"LimitMEMLOCK=infinity; in an unprivileged "
				"container the CONTAINER's limit is the one "
				"that refuses), or set pin = auto to run "
				"swappable.\n",
				rl.rlim_cur == RLIM_INFINITY ? 0UL :
				(unsigned long)(rl.rlim_cur >> 20),
				cfg->arena_mb);
			return 1;
		}
		LM_NOTICE("memory: %lu MB of the arena locked in RAM "
			"(pin = require)\n", pr.locked_bytes >> 20);
	}

	/* what a runtime-created collection inherits: a fleet is ONE mode over ONE collection set,
	 * so it takes the cluster's mode */
	pc_col_default_proxy = cfg->cl_mode == PC_MODE_PROXY;
	pc_col_default_shard = cfg->cl_mode == PC_MODE_SHARD;
	pc_col_default_eager = cfg->cl_eager;
	pc_grow_at_pct = cfg->grow_at_pct;
	pc_output_stall_s = cfg->output_stall_s;
	pc_autoscale_default = cfg->autoscale;
	pc_types_default = cfg->redis_types;
	/* the shrink threshold: a quarter of the growth one unless pinned, never at or above it -
	 * the two would chase each other across every boundary */
	pc_shrink_at_pct = cfg->shrink_at_pct ? cfg->shrink_at_pct
		: cfg->grow_at_pct / 4;
	if (pc_shrink_at_pct >= pc_grow_at_pct) {
		LM_WARN("shrink_at_pct %d is not below grow_at_pct %d: shrinking "
			"at half of it instead\n", pc_shrink_at_pct, pc_grow_at_pct);
		pc_shrink_at_pct = pc_grow_at_pct / 2;
	}
	pc_shrink_cooloff_s = cfg->shrink_cooloff_s;
	{
		extern int pc_prefetch_min_keys;

		pc_prefetch_min_keys = cfg->prefetch_min_keys;
	}
	/* a floor at or above the ordinary target asks for nothing; one at or below the shrink
	 * threshold would have the leg and the shrinker pull the same table both ways */
	pc_grow_floor_pct = cfg->grow_floor_pct;
	pc_leg_stuck_pct = cfg->leg_stuck_pct;
	if (pc_grow_floor_pct && pc_grow_floor_pct >= pc_grow_at_pct) {
		LM_WARN("grow_floor_pct %d is not below grow_at_pct %d: the "
			"overflow leg will not widen a table\n",
			pc_grow_floor_pct, pc_grow_at_pct);
		pc_grow_floor_pct = 0;
	}
	if (pc_grow_floor_pct && pc_grow_floor_pct <= pc_shrink_at_pct) {
		LM_WARN("grow_floor_pct %d is not above shrink_at_pct %d: a "
			"table widened for its leg would be shrunk again, so "
			"the leg will not widen one\n",
			pc_grow_floor_pct, pc_shrink_at_pct);
		pc_grow_floor_pct = 0;
	}
	pc_col_default_pull = pc_col_default_proxy || pc_col_default_shard;

	/* main is slot 0, workers 1..n, then maint, wal, rdb, peer, bulk and beat at n+1..n+6, sixteen
	 * spare; the client range is set before the first table, so every table inherits it */
	pcache_ht_default_stat_slots((unsigned int)cfg->workers + 7 + 16);
	pcache_ht_default_client_slots(1, 1 + (unsigned int)cfg->workers);
	pc_ev_set_default(cfg->log_events, cfg->log_events_rate, cfg->log_events_hash);
	pc_ev_set_notify_default(cfg->notify_events);

	for (i = 0; i < cfg->n_col; i++) {
		unsigned long rg0 = pcache_arena_regions_bytes();

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
			cfg->col[i].eager, cfg->col[i].buckets_log2);
		if (cfg->col[i].has_autoscale)
			pc_store_set_autoscale_idx(i, cfg->col[i].autoscale);
		if (cfg->col[i].has_redis_types)
			pc_store_set_types_idx(i, cfg->col[i].redis_types);
		if (cfg->col[i].nondurable)
			pc_store_set_nondurable_idx(i, 1);
		pc_store_note_index(cols[i].ht, pcache_arena_regions_bytes() - rg0);
		pc_ev_configure(i, cfg->col[i].has_log_events ? cfg->col[i].log_events
			: PC_EV_INHERIT);
		pc_ev_configure_notify(i, cfg->col[i].has_notify_events
			? cfg->col[i].notify_events : PC_EV_INHERIT);
	}
	n_cols = cfg->n_col;
	/* one daemon per state_dir and per [wal] dir, locked before anything in either is opened
	 * (fcntl locks are per process: the same directory twice is one lock) */
	pc_recover_note_last_write(cfg->wal_dir);  /* before the lock writes there */
	if (pc_dir_lock(cfg->state_dir, "state_dir", 1) != 0 ||
	        pc_dir_lock(cfg->wal_dir, "wal dir", 0) != 0)
		return 1;
	/* the collections clients created, BEFORE the replay and the snapshot import: both drop
	 * records for a collection name they cannot resolve */
	if (pc_store_load(cfg->state_dir) != 0)
		return 1;
	/* the RESP map's runtime overrides, before any worker reads it */
	if (pc_respmap_init(cfg->resp_collections, cfg->state_dir) != 0)
		return 1;
	pc_upg_set_conf(&cfg->upgrade);
	pc_verb_set_allow_create(cfg->allow_create);
	pc_verb_set_resp_redirect(cfg->resp_redirect);
	pc_stats_reset_schedule(cfg->stats_reset_days,
		cfg->stats_reset_at_s);
	{
		const char *e = getenv("PERFCACHED_TEST_DOOR_START");

		if (e && *e) {
			pc_doors_test_start((unsigned int)strtoul(e, NULL, 10));
			LM_NOTICE("test: the door counters start at %s "
				"(PERFCACHED_TEST_DOOR_START)\n", e);
		}
	}
	pc_keys_set_turn_us(cfg->keys_turn_us);

	/* the shared UNIX listener (TCP ones are per-worker SO_REUSEPORT) */
	for (i = 0; i < cfg->n_listen; i++)
		if (cfg->listen[i].type == PC_LISTEN_UNIX) {
			unix_fd = unix_listener_fd(cfg->listen[i].addr);
			if (unix_fd < 0)
				return 1;
		}
	pc_obs_qlog_config(cfg->query_log, cfg->query_log_keys);
	if (cfg->query_log && cfg->log_level < L_INFO)
		LM_WARN("query_log is on but log_level is below info: the lines "
			"go through LM_INFO and will not appear - set log_level = "
			"info while it is on\n");
	if (pc_obs_init(cfg->workers, cfg->slowlog_usec) != 0) {
		LM_ERR("observability init failed\n");
		return 1;
	}
	pc_pubsub_init(cfg->workers + 8);             /* worker ids are 1-based */
	/* read-through: the collections exist, the workers do not */
	if (pc_upstream_init(cfg, cfg->workers + 8) != 0)
		return 1;
	/* the alert mailer's sender thread - its own copy of [mail],
	 * so the config's secrets can be wiped below without touching it */
	if (cfg->mail_on && pc_mail_start(&cfg->mail) != 0)
		return 1;
	pc_alerts_init(cfg);                          /* [mail]'s rules */
	pc_keepalive_s = cfg->keepalive_s;
	pc_pubsub_set_queue_cap((size_t)cfg->pubsub_queue_mb << 20);
	/* the client limit, configured or derived from the fd ceiling (raised toward the hard limit);
	 * the reserve covers listeners, threads, cluster, WAL/RDB and a few HTTP doors for /stats */
	{
		struct rlimit rl;
		long reserve = 64 + 2L * (cfg->workers + 8) + cfg->n_listen
			+ (cfg->cl_enabled ? 16 : 0) + 16;
		long want = cfg->max_clients ? cfg->max_clients + reserve : 0;
		long avail;

		if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
			rl.rlim_cur = 1024;
			rl.rlim_max = 1024;
		}
		if (want && (long)rl.rlim_cur < want) {
			struct rlimit up = rl;

			up.rlim_cur = (rlim_t)want > rl.rlim_max ? rl.rlim_max
				: (rlim_t)want;
			if (setrlimit(RLIMIT_NOFILE, &up) == 0 &&
			    getrlimit(RLIMIT_NOFILE, &rl) != 0)
				rl = up;
		}
		avail = (long)rl.rlim_cur - reserve;
		if (avail < 16)
			avail = 16;
		if (cfg->max_clients && cfg->max_clients > avail) {
			LM_WARN("max_clients %d needs %ld descriptors and the limit "
				"is %ld: running with max_clients %ld\n",
				cfg->max_clients, want, (long)rl.rlim_cur, avail);
			pc_max_clients = (int)avail;
		} else if (cfg->max_clients) {
			pc_max_clients = cfg->max_clients;
		} else {
			pc_max_clients = (int)avail;
		}
		pc_enable_lockout_after = cfg->enable_lockout_after;
		pc_enable_lockout_s = cfg->enable_lockout_s;
		/* the line waits for the memory figure (pc_memguard_init,
		 * once the arena and the WAL exist) */
		fd_limit = (long)rl.rlim_cur;
		fd_reserve = reserve;
		pc_max_clients_basis = cfg->max_clients ? "configured" : "descriptors";
	}

	/* fail fast if a TCP listener cannot bind at all, and - since SO_REUSEPORT hides it - if
	 * another process already holds it */
	for (i = 0; i < cfg->n_listen; i++)
		if (cfg->listen[i].type == PC_LISTEN_TCP) {
			int probe;

			/* an address not configured YET is waited
			 * for, not failed on - see addrwait.h */
			if (pc_addr_wait(cfg->listen[i].addr,
			        cfg->address_wait_s, "listener") != 0)
				return 1;
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
	/* each native TCP door sends UDP pushes from its own address and port (one firewall rule);
	 * a port held elsewhere only turns the feature off for that door */
	for (i = 0; cfg->pubsub_udp && i < cfg->n_listen; i++) {
		const struct pc_listener *l = &cfg->listen[i];
		struct addrinfo hints, *res = NULL;
		char port[16];
		int fd, small = 4096;

		if (l->type != PC_LISTEN_TCP || l->resp || l->http)
			continue;
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_flags = AI_PASSIVE;
		snprintf(port, sizeof port, "%d", l->port);
		if (getaddrinfo(strcmp(l->addr, "*") ? l->addr : NULL, port,
		        &hints, &res) != 0 || !res)
			continue;                  /* IPv6-only door: no UDP pushes */
		fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd >= 0 && bind(fd, res->ai_addr, res->ai_addrlen) == 0) {
			pc_addr la;

			setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
			pc_addr_from_sa(&la, res->ai_addr, res->ai_addrlen);
			pc_udp_door_add(pc_addr_v4(&la), (uint16_t)l->port, fd);
			LM_NOTICE("pub/sub: UDP pushes leave from %s:%d/udp\n",
				l->addr, l->port);
		} else {
			LM_WARN("pub/sub: cannot bind %s:%d/udp (%s) - no UDP pushes "
				"from this door\n", l->addr, l->port, strerror(errno));
			if (fd >= 0)
				close(fd);
		}
		freeaddrinfo(res);
	}
	/* the watermarks, before the recovery counts into them: without a WAL that syncs, a delete
	 * is as durable as it will ever be once applied */
	if (pc_ack_init(!cfg->wal_dir || cfg->wal_fsync == 2) != 0) {
		LM_ERR("no memory for the watermarks\n");
		return 1;
	}
	/* WAL storage: identity (the label) and probe (the measurement) at startup */
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

		/* no probe cache: storage moves under a stable device number (a hypervisor migration), so a
		 * cached entry could describe storage that is gone; `probe = no` pays nothing */
		if (cfg->wal_probe == PC_WPROBE_ALWAYS)
			pc_wal_probe_run(cfg->wal_dir, cfg->wal_probe_secs,
				&wal_probe);
		else if (cfg->wal_probe == PC_WPROBE_AUTO)
			pc_wal_probe_run(cfg->wal_dir, 0, &wal_probe);
		pc_wal_policy_from(wal_probe.valid ? &wal_probe : NULL, &wal_sid, &pol);
		wal_id_valid = 1;
		pc_wal_probe_format(wal_probe.valid ? &wal_probe : NULL, &pol, rep,
			sizeof rep);
		LM_NOTICE("%s", rep);

		/* bring the WAL up - explicit knobs win, the probe's
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

			if (pc_rdb_init(cfg->wal_dir, rr, nrr,
			        cfg->rdb_mb_s,
			        wal_probe.valid ? wal_probe.seq_mb_s : 0)
			        != 0) {
				LM_ERR("rdb: checkpoint engine failed to "
					"init - refusing to run with the "
					"durability the config promised "
					"silently absent\n");
				return 1;
			}
		}
		/* recovery and a fresh checkpoint BEFORE wal activation (it claims the next generation slot);
		 * the preload gives the checkpoint's marker the WAL sequence before the walk */
		pc_wal_preload_seq(cfg->wal_dir, cfg->wal_segments);

		{
			struct pc_recover_stats rst;

			/* nothing of its own to bring back: STARTING; something restored: RECOVERING until the
			 * fleet settles it - the master decides from the identity history */
			pc_recover_set_skip_unknown(cfg->wal_unknown_skip);
			if (pc_recover(cfg->wal_dir, &rst) != 0) {
				LM_ERR("recovery failed\n");
				return 1;
			}
			/* before the checkpoint below, whose marker is
			 * stamped with this sequence */
			pc_wal_seq_atleast(rst.last_seq);
			/* the loaded snapshot covers what it covers,
			 * checkpoint or not - pc_wal_init asks below */
			if (rst.rdb_ok)
				pc_rdb_seed_marker(rst.marker);
			if (rst.rdb_records + rst.wal_applied > 0)
				pc_node_state_set(PC_NST_RECOVERING);
			if (rst.rdb_records + rst.wal_applied > 0 &&
			        pc_rdb_save_sync() != 0) {
				LM_ERR("the post-recovery checkpoint failed - refusing "
					"to risk the replay window\n");
				return 1;
			}
		}
		/* fsync = always fsyncs once per drained batch, so the ring must absorb the fdatasync window:
		 * on ms-class storage 1 MB overflows and DROPS acknowledged writes - take the probe's depth */
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
		/* the WAL's buffers are locked when the arena is -
		 * both or neither, settled just below */
		{
			struct pcache_arena_pressure apr;

			pcache_arena_pressure(&apr);
			pc_wal_set_pin(apr.locked_bytes > 0);
		}
		/* the apply threads sit at nw+7.. and a migration they apply lands in THEIR ring, so
		 * the WAL needs one per thread; 1 (the default) adds none */
		int wnr = cfg->workers + 6 + (cfg->cl_apply_threads >= 2
		        ? 1 + cfg->cl_apply_threads : 0);
		/* slots: 0 main, 1..nw workers, nw+1 maint, +2 pump, +3 RDB, +4 peer, +5 bulk, +6 beat, +7.. apply;
		 * only workers, peer, bulk and apply write - the rest get min(ring_kb, 512 KB) */
		int small_kb = ring_kb < 512 ? ring_kb : 512;
		int *wkb = calloc((size_t)wnr, sizeof *wkb);
		int wi, nbig = 0;

		if (!wkb) {
			LM_ERR("wal init failed: no memory for the ring table\n");
			return 1;
		}
		for (wi = 0; wi < wnr; wi++) {
			int nw = cfg->workers, big =
				(wi >= 1 && wi <= nw) ||
				(cfg->cl_enabled && (wi == nw + 4 || wi == nw + 5 ||
				                     wi >= nw + 7));

			wkb[wi] = big ? ring_kb : small_kb;
			nbig += big;
		}
		LM_NOTICE("wal: %d ring(s) of %d KB for the threads that write, "
			"%d of %d KB for the rest\n", nbig, ring_kb, wnr - nbig,
			small_kb);
		pc_wal_set_segments_max(cfg->wal_segments_max);
		if (pc_wal_init(cfg->wal_dir,
		        cfg->wal_fsync == 1 ? PC_WFSYNC_ALWAYS :
		        cfg->wal_fsync == 2 ? PC_WFSYNC_NO : PC_WFSYNC_EVERYSEC,
		        cfg->wal_segment_mb ? cfg->wal_segment_mb : pol.segment_mb,
		        cfg->wal_segments, ring_kb, wnr, wkb) != 0) {
			free(wkb);
			LM_ERR("wal init failed\n");
			return 1;
		}
		free(wkb);
		pc_wal_set_on_full(cfg->wal_on_full);
		pc_wal_set_workers(1, cfg->workers);
		/* never half pinned: a locked arena beside swappable WAL buffers stalls the write path on
		 * a swapped ring page while claiming to be resident */
		{
			struct pcache_arena_pressure apr;
			struct rlimit rl;
			int lerr = 0;
			unsigned long long wl = pc_wal_locked_bytes(&lerr);

			pcache_arena_pressure(&apr);
			getrlimit(RLIMIT_MEMLOCK, &rl);
			if (apr.locked_bytes && !wl) {
				unsigned long lim = rl.rlim_cur == RLIM_INFINITY
					? 0UL : (unsigned long)(rl.rlim_cur >> 20);

				if (cfg->pin_require) {
					LM_ERR("[memory] pin = require: the arena is "
						"locked (%lu MB) but the WAL buffers "
						"(%llu MB) could not be (%s) - "
						"RLIMIT_MEMLOCK is %lu MB.  The two are "
						"locked together or not at all: raise the "
						"limit (systemd LimitMEMLOCK=infinity; in "
						"an unprivileged container the "
						"CONTAINER's limit is the one that "
						"refuses), or set pin = auto to run "
						"swappable.\n", apr.locked_bytes >> 20,
						pc_wal_buffer_bytes() >> 20,
						strerror(lerr), lim);
					return 1;
				}
				if (pcache_arena_unpin())
					LM_WARN("memory: the WAL buffers (%llu MB) "
						"could not be locked (%s; "
						"RLIMIT_MEMLOCK %lu MB), so the arena is "
						"unlocked too - the node runs swappable "
						"as a whole rather than half pinned.  "
						"Raise the limit to lock both, or set "
						"pin = require to refuse instead.\n",
						pc_wal_buffer_bytes() >> 20,
						strerror(lerr), lim);
				else
					LM_WARN("memory: the WAL buffers (%llu MB) "
						"could not be locked (%s; "
						"RLIMIT_MEMLOCK %lu MB) and the arena is "
						"on hugetlb pages, which cannot be "
						"unlocked: the arena is unswappable and "
						"the WAL buffers are not.  Raise the "
						"limit, or set pin = require to refuse "
						"instead.\n",
						pc_wal_buffer_bytes() >> 20,
						strerror(lerr), lim);
			} else if (wl) {
				LM_NOTICE("memory: the WAL buffers (%llu MB) are "
					"locked with the arena\n", wl >> 20);
			}
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
			/* optional standalone; absent, the door
			 * refuses the cluster principal (have_cluster) */
			if (cfg->cluster_secret) {
				if (pc_psk_derive(cfg->cluster_secret,
				        strlen(cfg->cluster_secret),
				        PC_PRIN_CLUSTER, PSK.cluster) != 0)
					return 1;
				PSK.have_cluster = 1;
			}
			if (cfg->cluster_accept_secret) {
				if (pc_psk_derive(cfg->cluster_accept_secret,
				        strlen(cfg->cluster_accept_secret),
				        PC_PRIN_CLUSTER, PSK.cluster_accept) != 0)
					return 1;
				PSK.have_cluster_accept = 1;
			}
			have_psk = 1;
			__atomic_store_n(&PSK_CUR, &PSK, __ATOMIC_RELEASE);
			LM_NOTICE("Noise PSKs derived (%d client + %d cluster)\n",
				PSK.n_client, PSK.have_cluster);
		}
		for (i = 0; i < cfg->n_listen; i++) {
			const struct pc_listener *l = &cfg->listen[i];

			if (listener_door(l) != PC_DOOR_AUTH)
				continue;
			if (l->type == PC_LISTEN_UNIX)
				LM_NOTICE("native door %s: authenticated, not encrypted "
					"([listen] encrypt = yes encrypts it)\n", l->addr);
			else
				LM_NOTICE("native door %s:%d: authenticated, not encrypted - "
					"its traffic can be read and altered on the network "
					"([listen] encrypt = yes encrypts it)\n", l->addr, l->port);
		}
	}

	/* the PSKs are cached, so the raw secrets' plaintext must not live for the process lifetime;
	 * exit paths that never get here are covered by pc_config_free wiping again */
	pc_config_wipe_secrets(cfg);
	pc_keeps_log(cfg);

	if (!cfg->cl_enabled) {
		pc_node_state_set(PC_NST_READY);   /* nothing to join */
		/* installed here too, not only in pc_cluster_init(); after READY, so a failure the WAL
		 * found at startup (delivered by this call) is what stands */
		pc_wal_on_shed(pc_node_wal_shed);
		pc_wal_on_heal(pc_node_wal_heal);
	}

	if (cfg->cl_enabled) {
		/* identity and the mastership term live in state_dir (else the [wal] dir, carried across
		 * once); neither set is legal but LOUD - the node presents as new after every restart */
		const char *sd = cfg->state_dir ? cfg->state_dir : cfg->wal_dir;
		const char *legacy = (cfg->state_dir && cfg->wal_dir &&
			strcmp(cfg->state_dir, cfg->wal_dir) != 0)
			? cfg->wal_dir : NULL;

		if (!sd)
			LM_WARN("cluster: state_dir is not set - identity and "
				"mastership term are EPHEMERAL, so this node "
				"presents as NEW after every restart.  Set "
				"[daemon] state_dir (the prefix's var/ is the "
				"place)\n");
		else if (!cfg->state_dir)
			LM_NOTICE("cluster: state_dir not set - using the WAL "
				"directory %s for identity and term (set "
				"[daemon] state_dir to keep them apart)\n", sd);
		/* before init: the identity is derived in there, and it has
		 * to be settled before this node joins anything */
		pc_cluster_set_site_salt(cfg->cl_site_salt);
		pc_cluster_set_apply_stall_ms(cfg->cl_apply_stall_ms);
		pc_cluster_set_apply_threads(cfg->cl_apply_threads);
		pc_cluster_set_sweep_digests(cfg->cl_sweep_digests);
		pc_cluster_set_reconcile_fast(cfg->cl_reconcile_fast);
		pc_cluster_set_reconcile_rate_max(cfg->cl_reconcile_rate_max);
		pc_cluster_set_multicast_ttl(cfg->cl_mcast_ttl);
		pc_cluster_set_max_members(cfg->cl_max_members);
		pc_cluster_set_cluster_id(cfg->cl_cluster_id);
		pc_cluster_set_discovery(cfg->cl_unicast, cfg->cl_seeds);
		pc_cluster_set_lane(cfg->cl_max_dgram, cfg->cl_lane_port);
		pc_cluster_set_lane_retry(cfg->cl_lane_retry_s);
		pc_cluster_set_expect(cfg->cl_expect);
		pc_cluster_set_identity_source(cfg->cl_id_source);
		pc_cluster_set_retain(cfg->cl_tomb_retain_s,
			cfg->cl_tomb_retain_max);
		pc_cluster_set_acks(cfg->cl_tomb_acks);
		pc_cluster_set_resync_after(cfg->cl_resync_after_s);
		pc_cluster_set_data_multicast(cfg->cl_data_mcast);
		cfgsig_build(cfg);
		/* the cluster binds its sockets to the advertise
		 * address - wait for it like a listener's */
		if (pc_addr_wait(cfg->cl_advertise, cfg->address_wait_s,
		        "cluster advertise") != 0)
			return 1;
		if (PSK.have_cluster_accept)
			pc_cluster_set_accept_psk(PSK.cluster_accept);
		if (pc_cluster_init(cfg->cl_mcast, cfg->cl_port,
		        cfg->cl_advertise, PSK.cluster,
		        cfg->cl_pull_timeout_ms, cfg->cl_negative_ms,
		        cfg->cl_tombstone_ms, sd, legacy,
		        cfg->cl_max_pending) != 0)
			return 1;
		pc_cluster_set_stats_reset_cb(stats_reset_peer);
		pc_cluster_set_alert_relay_cb(pc_alerts_relayed);
		/* relays on a port of their own, or stay on the cluster socket */
		napply = pc_cluster_apply_threads();
		npsrx = pc_cluster_pubsub_rx_open(cfg->cl_ps_port, cfg->cl_ps_rx_threads);
		pc_pubsub_set_interest(cfg->cl_ps_interested);
		if (npsrx > 16)
			npsrx = 16;
		/* what this node believes the clustered collections ARE: it rides every ALIVE, and a peer
		 * that disagrees is refused rather than fed (config_digest()) */
		{
			int cport = 0, rport = 0, k;
			int hport = 0;             /* the first HTTP door's port */

			/* peers publish the first native TCP door and the first RESP one: the two audiences
			 * cannot use each other's door (RESP has no Noise handshake) */
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
			for (k = 0; k < cfg->n_listen; k++)
				if (cfg->listen[k].type == PC_LISTEN_TCP &&
				        cfg->listen[k].http) {
					hport = cfg->listen[k].port;
					break;
				}
			pc_cluster_set_config(cfg->n_cl_col ? (int)cfg->cl_mode
				: (cfg->n_col ? (int)cfg->col[0].mode : 0),
				cfg->cl_eager,
				cfg->n_cl_col != 0, cport, rport, hport,
				cfg->wal_dir != NULL,
				cfg->cl_replicas);
		}
	}

	compat_set_broadcast(pc_broadcast);

	/* the memory guard's first sample: the arena and WAL rings exist now, so the base is
	 * measured; a configured limit stands (the guard refuses at the floor) but not silently */
	{
		long mem = pc_memguard_init(cfg);
		char why[96] = "";

		if (mem == PC_MEMGUARD_NOFIT)
			return 1;                  /* said why */

		if (mem >= 0 && !cfg->max_clients && mem < pc_max_clients) {
			pc_max_clients = (int)mem;
			pc_max_clients_basis = "memory";
		} else if (mem >= 0 && cfg->max_clients && mem < cfg->max_clients) {
			LM_WARN("max_clients %d is more idle connections than this "
				"node's memory holds (~%ld): the memory guard "
				"(memory_floor) will refuse before the limit is "
				"reached\n", cfg->max_clients, mem);
		}
		if (mem >= 0)
			snprintf(why, sizeof why, ", memory for ~%ld idle", mem);
		LM_NOTICE("clients: max_clients %d (%s%s), descriptor limit %ld, "
			"reserve %ld%s\n", pc_max_clients,
			strcmp(pc_max_clients_basis, "configured") ? "derived from " : "",
			pc_max_clients_basis, fd_limit, fd_reserve, why);
	}

	/* the node file the upgrade helper reads (state_dir only), written only now: recovery and
	 * the cluster identity accepted what is on disk, so a refused start has written nothing */
	pc_upg_set_install_mode(cfg->install_mode);
	pc_upg_node_init(cfg->state_dir);
	{
		const char *e = getenv("PERFCACHED_TEST_START_HOLD_MS");

		if (e && *e) {
			unsigned long ms = strtoul(e, NULL, 10);
			struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };

			LM_NOTICE("test: start-up holds %lu ms before the threads "
				"(PERFCACHED_TEST_START_HOLD_MS)\n", ms);
			nanosleep(&ts, NULL);
		}
	}
	/* threads spawn with signals blocked (main alone handles them) and 1 MB stacks: the cluster
	 * plane keeps ~65-130 KB of buffers on the stack, past some libcs' 128 KB default */
	pthread_attr_init(&tattr);
	pthread_attr_setstacksize(&tattr, 1024L * 1024);
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGTERM);
	sigaddset(&blocked, SIGINT);
	sigaddset(&blocked, SIGHUP);            /* the threads leave it to this one */
	pthread_sigmask(SIG_BLOCK, &blocked, &old);

	if (pc_thread_slots(nw, cfg->wal_dir != NULL, cfg->cl_enabled, napply, npsrx) >
	        PC_THREAD_SLOTS) {
		LM_CRIT("%d workers and the helper threads need more than the %d thread "
			"slots this build has\n", nw, PC_THREAD_SLOTS);
		return 1;
	}
	for (i = 0; i < nw && !bad; i++) {
		wargs[i].slot = slot_init(1 + i);
		wargs[i].wid = i;
		bad = spawn(wargs[i].slot, &tattr, worker_main, &wargs[i], "worker");
	}
	if (!bad) {
		mt = slot_init(nw + 1);
		bad = spawn(mt, &tattr, maint_main, mt, "maintenance");
	}
	if (!bad) {
		wt = slot_init(nw + 2);
		bad = spawn(wt, &tattr, wal_main, wt, "wal");
	}
	if (!bad && cfg->wal_dir) {
		rt = slot_init(nw + 3);
		bad = spawn(rt, &tattr, rdb_main, rt, "rdb");
	}
	if (!bad && cfg->cl_enabled) {
		ct = slot_init(nw + 4);
		bad = spawn(ct, &tattr, peer_main, ct, "cluster");
		if (!bad) {
			bt = slot_init(nw + 5);
			bad = spawn(bt, &tattr, bulk_main, bt, "bulk");
		}
		if (!bad) {
			hb = slot_init(nw + 6);
			bad = spawn(hb, &tattr, beat_main, hb, "beat");
		}
		/* the apply threads take nw+7.., the relay threads
		 * follow them - the WAL ring count above depends on it */
		for (i = 0; i < napply && !bad; i++) {
			apth[i].slot = slot_init(nw + 7 + i);
			apth[i].idx = i;
			bad = spawn(apth[i].slot, &tattr, apply_main, &apth[i], "apply");
		}
		for (i = 0; i < npsrx && !bad; i++) {
			psrx[i].slot = slot_init(nw + 7 + napply + i);
			psrx[i].idx = i;
			bad = spawn(psrx[i].slot, &tattr, psrx_main, &psrx[i], "relay");
		}
	}
	if (bad) {
		pthread_attr_destroy(&tattr);
		spawn_unwind();
		pthread_sigmask(SIG_SETMASK, &old, NULL);
		return 1;
	}
	pthread_attr_destroy(&tattr);

	/* the handlers before the mask lifts: a TERM taken in thread creation is a clean stop */
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	pthread_sigmask(SIG_SETMASK, &old, NULL);

	/* "ready" promises that the doors accept, and each worker opens its OWN listener: wait for
	 * every worker, bounded - one that never gets there is said, not hidden */
	{
		struct timespec ms1 = { 0, 1000000 };
		int w = 0;

		while (__atomic_load_n(&workers_listening, __ATOMIC_ACQUIRE) < nw &&
		       w < 10000 && !stop_flag) {
			nanosleep(&ms1, NULL);
			w++;
		}
		if (__atomic_load_n(&workers_listening, __ATOMIC_ACQUIRE) < nw)
			LM_WARN("only %d of %d workers have opened their "
				"listeners after %d ms - going on\n",
				__atomic_load_n(&workers_listening, __ATOMIC_ACQUIRE),
				nw, w);
	}
	pc_metrics_mark_start();       /* uptime counts from HERE: a slow recovery is not uptime */
	LM_NOTICE("perfcached ready: %d workers, %d collections, %d "
		"listeners, arena %s\n", nw, n_cols, cfg->n_listen,
		cfg->backing_heap ? "heap" : "reserved");

	for (i = 0; i < nw; i++)
		pthread_join(wargs[i].slot->tid, NULL);
	pc_upstream_shutdown();                       /* no gate runs now */
	pthread_join(mt->tid, NULL);
	pc_mail_stop();                               /* after the alert source */
	if (rt)
		pthread_join(rt->tid, NULL);
	if (ct)
		pthread_join(ct->tid, NULL);
	if (hb)
		pthread_join(hb->tid, NULL);
	if (bt)                                       /* it stores: before the snapshot and the arena go */
		pthread_join(bt->tid, NULL);
	/* after the cluster thread (ct) - nothing pushes any more,
	 * and a thread drains what it holds before it sees the flag */
	for (i = 0; i < napply; i++)
		pthread_join(apth[i].slot->tid, NULL);
	for (i = 0; i < npsrx; i++)
		pthread_join(psrx[i].slot->tid, NULL);
	wal_stop_join(wt);                            /* last: every writer has stopped */
	if (unix_fd >= 0) {
		close(unix_fd);
		for (i = 0; i < cfg->n_listen; i++)
			if (cfg->listen[i].type == PC_LISTEN_UNIX)
				unlink(cfg->listen[i].addr);
	}
	/* a clean stop leaves a snapshot: the WAL holds only what this node WROTE (received copies
	 * are not logged), and the snapshot walk stores the copies too */
	if (rt && pc_rdb_held()) {
		LM_WARN("perfcached: stopping WITHOUT the shutdown snapshot - a copy of the "
			"fleet's state did not finish, so the files keep what this node held "
			"before it\n");
	} else if (rt) {
		LM_NOTICE("perfcached: stopping - taking the shutdown snapshot\n");
		if (pc_rdb_save_on_stop() != 0)
			LM_ERR("perfcached: the shutdown snapshot FAILED - the WAL "
				"still holds everything this node wrote, but it will "
				"come back without the copies it held for its "
				"peers\n");
	}
	pcache_arena_destroy();
	if (stop_for_worker) {
		LM_CRIT("perfcached: stopped because a worker left its loop (exit 1: a "
			"supervisor restarts it)\n");
		return 1;
	}
	if (stop_for_files) {
		LM_WARN("perfcached: stopped to start again from its own files (exit 75: a "
			"supervisor restarts it)\n");
		return 75;
	}
	LM_NOTICE("perfcached: clean shutdown\n");
	return 0;
}
