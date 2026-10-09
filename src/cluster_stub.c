/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * cluster_stub.c - S250: the STANDALONE build's side of the cluster seam.
 *
 * A standalone perfcached links this instead of cluster.c and the cl*.c
 * modules (clunk.c and clretain.c stay: they are standalone features).
 * Every call the core makes into the cluster answers as a daemon with no
 * [cluster] section already does at runtime - not enabled, no replicas,
 * nothing forwarded, nothing pulled, no negative cache - so the core's
 * behaviour is the clustered build's with the cluster off, and the
 * difference is only what is not compiled in.
 *
 * Generated from the linker's view of the seam (symbols the core objects
 * leave undefined that the cluster objects define), then written out by
 * hand where "zero" would be the wrong answer: pc_repl_push() must say
 * KEEP (0 tells the caller to delete its copy), and names say what this
 * build is.  The node's lifecycle is not here - it is node.c, in both.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cluster.h"
#include "clterm.h"
#include "clunk.h"
#include "clfleet.h"
#include "clpeers.h"

unsigned long long pc_term_rejected;

void pc_bulk_thread(volatile int *stop)
{
	(void)stop;
}

void pc_cluster_apply_thread(volatile int *stop, int idx)
{
	(void)stop;
	(void)idx;
}

int pc_cluster_apply_threads(void)
{
	return 0;
}

int pc_cluster_authoritative(void)
{
	return 0;
}

void pc_cluster_beat_thread(volatile int *stop)
{
	(void)stop;
}

unsigned long long pc_cluster_col_announce(const char *name, size_t nlen, int buckets_log2, int op)
{
	(void)name;
	(void)nlen;
	(void)buckets_log2;
	(void)op;
	return 0;
}

void pc_cluster_col_announce2(const char *from, size_t flen, const char *to, size_t tlen)
{
	(void)from;
	(void)flen;
	(void)to;
	(void)tlen;
}

int pc_cluster_col_fleet(int store, int basis, struct pc_col_fleet *out)
{
	(void)store;
	(void)basis;
	(void)out;
	return 0;
}

int pc_cluster_drain(int worker, struct pc_pull_done *out, int max)
{
	(void)worker;
	(void)out;
	(void)max;
	return 0;
}

int pc_cluster_eager(void)
{
	return 0;
}

int pc_cluster_enabled(void)
{
	return 0;
}

int pc_cluster_stats_reset_send(long long at) { (void)at; return 0; }
void pc_cluster_set_stats_reset_cb(void (*cb)(long long at)) { (void)cb; }
int pc_cluster_fleet_stop(long long timeout_ms, int *gone, int *ngone, int *still, int *nstill, int *lost, int *nlost, int cap)
{
	(void)timeout_ms;
	(void)cap;
	(void)gone;
	(void)still;
	(void)lost;
	if (ngone)
		*ngone = 0;
	if (nstill)
		*nstill = 0;
	if (nlost)
		*nlost = 0;
	return -1;             /* there is no fleet to stop */
}

void pc_cluster_get_stats(struct pc_cl_stats *out)
{
	memset(out, 0, sizeof *out);
}

const char *pc_cluster_identity(void)
{
	return "";
}

int pc_cluster_identity_durable(void)
{
	return 0;
}

void pc_cluster_identity_uuid(char out[37], int *ver)
{
	out[0] = 0;
	if (ver)
		*ver = 0;
}

unsigned int pc_cluster_incarnation(void)
{
	return 0;
}

int pc_cluster_init(const char *mcast_addr, int mcast_port, const char *advertise, const uint8_t psk[PC_NOISE_KEYLEN], int pull_timeout_ms, int negative_ms, int tombstone_ms, const char *state_dir, const char *legacy_dir, int max_pending)
{
	(void)mcast_addr;
	(void)mcast_port;
	(void)advertise;
	(void)psk;
	(void)pull_timeout_ms;
	(void)negative_ms;
	(void)tombstone_ms;
	(void)state_dir;
	(void)legacy_dir;
	(void)max_pending;
	return 0;
}

int pc_cluster_last_fail(void)
{
	return 0;
}

int pc_cluster_map_stamp(unsigned int *term, unsigned int *seq)
{
	if (term)
		*term = 0;
	if (seq)
		*seq = 0;
	return 0;
}

const char *pc_cluster_gone_reason(int why)
{
	(void)why;
	return "unknown";
}

int pc_cluster_members(struct pc_member *out, int max)
{
	(void)out;
	(void)max;
	return 0;
}

int pc_cluster_mode(void)
{
	return 0;
}

const char *pc_cluster_mode_name(void)
{
	/* what clients parse: the clustered build says "store" with the
	 * cluster off, and so does this one - the edition is -V's and
	 * /stats' "edition", not a new mode a client has never seen */
	return "store";
}

int pc_cluster_neg_ms(void)
{
	return 0;
}

int pc_cluster_peers(struct pc_cl_peer_info *out, int max)
{
	(void)out;
	(void)max;
	return 0;
}

int pc_cluster_pend_cap(void)
{
	return 0;
}

int pc_cluster_port(void)
{
	return 0;
}

void pc_cluster_pubsub_interest_add(uint64_t version, int pattern, const char *name, size_t nlen, uint32_t h1, uint32_t h2)
{
	(void)version;
	(void)pattern;
	(void)name;
	(void)nlen;
	(void)h1;
	(void)h2;
}

void pc_cluster_pubsub_interest_figures(struct pc_psint_figures *f)
{
	memset(f, 0, sizeof *f);
}

int pc_cluster_pubsub_relay(const char *chan, size_t clen, const char *data, size_t dlen)
{
	(void)chan;
	(void)clen;
	(void)data;
	(void)dlen;
	return 0;
}

void pc_cluster_pubsub_relay_figures(int *port, int *threads, int *peers_direct, unsigned long long *sent_direct, unsigned long long *sent_cluster, unsigned long long *rx_dgrams)
{
	if (port)
		*port = 0;
	if (threads)
		*threads = 0;
	if (peers_direct)
		*peers_direct = 0;
	if (sent_direct)
		*sent_direct = 0;
	if (sent_cluster)
		*sent_cluster = 0;
	if (rx_dgrams)
		*rx_dgrams = 0;
}

int pc_cluster_pubsub_rx_open(int port, int threads)
{
	(void)port;
	(void)threads;
	return 0;
}

void pc_cluster_pubsub_rx_thread(volatile int *stop, int idx)
{
	(void)stop;
	(void)idx;
}

int pc_cluster_rcvbuf(void)
{
	return 0;
}

int pc_cluster_fleet_has(int feat, int *lacking)       /* S314 */
{
	(void)feat;
	if (lacking)
		*lacking = 0;
	return 1;                      /* no cluster: nothing to gate */
}

int pc_cluster_replicas(void)
{
	return 0;
}

int pc_cluster_replicas_short(void)
{
	return 0;
}

void pc_cluster_set_apply_stall_ms(int ms)
{
	(void)ms;
}

void pc_cluster_set_sweep_digests(int on)
{
	(void)on;
}

void pc_cluster_set_apply_threads(int n)
{
	(void)n;
}

void pc_cluster_set_config(int mode, int eager, int authoritative, int client_port, int resp_port, int http_port, int wal, int replicas)
{
	(void)mode;
	(void)eager;
	(void)authoritative;
	(void)client_port;
	(void)resp_port;
	(void)http_port;
	(void)wal;
	(void)replicas;
}

void pc_cluster_set_retain(int retain_s, int max)
{
	(void)retain_s;
	(void)max;
}

void pc_cluster_set_site_salt(const char *salt)
{
	(void)salt;
}

const char *pc_cluster_state_dir(void)
{
	return NULL;
}

int pc_cluster_state_tmpfs(void)
{
	return 0;
}

void pc_cluster_thread(volatile int *stop)
{
	(void)stop;
}

unsigned long long pc_cluster_tomb_catchup_sent(void)
{
	return 0;
}

void pc_cluster_unknown_figures(unsigned long long *recv, unsigned long long *bad)
{
	if (recv)
		*recv = 0;
	if (bad)
		*bad = 0;
}

int pc_cluster_unknown_fleet(const struct clunk_table *self, struct clunk_table *out, int *members, int *reporting)
{
	/* the node's own table IS the fleet's - what the clustered build
	 * answers with the cluster off (S165) */
	memset(out, 0, sizeof *out);
	clunk_fold(out, self, 0);
	*members = *reporting = 1;
	clunk_sort(out);
	return 0;
}

void pc_cluster_worker_register(int worker, int efd)
{
	(void)worker;
	(void)efd;
}

const char *pc_fleet_basis_name(int basis)
{
	(void)basis;
	return "none";
}

uint32_t pc_fwd_begin(int node, int op, const char *col, size_t collen, const char *key, size_t klen, const char *val, size_t vlen, unsigned int ttl_rel, long long delta)
{
	(void)node;
	(void)op;
	(void)col;
	(void)collen;
	(void)key;
	(void)klen;
	(void)val;
	(void)vlen;
	(void)ttl_rel;
	(void)delta;
	return 0;
}

uint32_t pc_fwd_json_begin(int node, int jop, const char *col, size_t collen, const char *key, size_t klen, const char *path, size_t plen, const char *val, size_t vlen, long long by, int have_ttl, long long ttl, int nx, int xx, int mkpath)
{
	(void)node;
	(void)jop;
	(void)col;
	(void)collen;
	(void)key;
	(void)klen;
	(void)path;
	(void)plen;
	(void)val;
	(void)vlen;
	(void)by;
	(void)have_ttl;
	(void)ttl;
	(void)nx;
	(void)xx;
	(void)mkpath;
	return 0;
}

void pc_loc_clear(const char *col, size_t collen, const char *key, size_t klen)
{
	(void)col;
	(void)collen;
	(void)key;
	(void)klen;
}

int pc_loc_get(const char *col, size_t collen, const char *key, size_t klen)
{
	(void)col;
	(void)collen;
	(void)key;
	(void)klen;
	return 0;
}

void pc_loc_set(const char *col, size_t collen, const char *key, size_t klen, int node)
{
	(void)col;
	(void)collen;
	(void)key;
	(void)klen;
	(void)node;
}

void pc_neg_clear(const char *col, size_t collen, const char *key, size_t klen)
{
	(void)col;
	(void)collen;
	(void)key;
	(void)klen;
}

int pc_neg_hit(const char *col, size_t collen, const char *key, size_t klen)
{
	(void)col;
	(void)collen;
	(void)key;
	(void)klen;
	return 0;
}

void pc_neg_set(const char *col, size_t collen, const char *key, size_t klen, int ms)
{
	(void)col;
	(void)collen;
	(void)key;
	(void)klen;
	(void)ms;
}

int pc_neg_ver(const char *col, size_t cn, const char *key, size_t kn, unsigned long long *ver)
{
	(void)col;
	(void)cn;
	(void)key;
	(void)kn;
	if (ver)
		*ver = 0;
	return 0;
}

int pc_place(void)
{
	return 0;
}

uint32_t pc_probe_fwd_begin(int op, const char *col, size_t collen, const char *key, size_t klen, const char *val, size_t vlen, unsigned int ttl_rel, long long by)
{
	(void)op;
	(void)col;
	(void)collen;
	(void)key;
	(void)klen;
	(void)val;
	(void)vlen;
	(void)ttl_rel;
	(void)by;
	return 0;
}

void pc_proxy_get_stats(struct pc_proxy_stats *out)
{
	memset(out, 0, sizeof *out);
}

uint32_t pc_pull_begin(const char *col, size_t collen, const char *key, size_t klen)
{
	(void)col;
	(void)collen;
	(void)key;
	(void)klen;
	return 0;
}

uint32_t pc_pull_begin_at(const char *col, size_t collen, const char *key, size_t klen, int holder_node)
{
	(void)col;
	(void)collen;
	(void)key;
	(void)klen;
	(void)holder_node;
	return 0;
}

int pc_repl_push(const char *col, size_t collen, const char *key, size_t klen, const char *val, size_t vlen, unsigned int exp, unsigned long long ver, unsigned char type)
{
	(void)type;
	(void)col;
	(void)collen;
	(void)key;
	(void)klen;
	(void)val;
	(void)vlen;
	(void)exp;
	(void)ver;
	return 1;             /* keep it: there is nobody to hand it to */
}

int pc_shard_grace(void)
{
	return 0;
}

int pc_shard_owner(const char *col, size_t collen, const char *key, size_t klen)
{
	(void)col;
	(void)collen;
	(void)key;
	(void)klen;
	return 0;
}

int pc_shard_owner_slot(unsigned slot)
{
	(void)slot;
	return 0;
}

int pc_spread_in_set(unsigned slot, int node_id, int k)
{
	(void)slot;
	(void)node_id;
	(void)k;
	return 1;
}

int pc_spread_pick_peer(unsigned slot, int k)
{
	(void)slot;
	(void)k;
	return 0;
}

int pc_spread_pick_peer_except(unsigned slot, int k, int except)
{
	(void)slot;
	(void)k;
	(void)except;
	return 0;
}

uint32_t pc_term_current(void)
{
	return 0;
}

void pc_tombstone_send_ver(const char *col, size_t collen, const char *key, size_t klen, unsigned long long ver)
{
	(void)col;
	(void)collen;
	(void)key;
	(void)klen;
	(void)ver;
}

void pc_cluster_rebalance(struct pc_rebalance *out)
{
	memset(out, 0, sizeof *out);
	out->settled = 1;
	snprintf(out->reason, sizeof out->reason, "standalone: no fleet");
}

void pc_cluster_set_multicast_ttl(int ttl)
{
	(void)ttl;
}

void pc_cluster_set_max_members(int n)
{
	(void)n;
}

void pc_cluster_set_cluster_id(const char *pin)
{
	(void)pin;
}

void pc_cluster_cid_info(char out[37], const char **state,
		unsigned long long *foreign)
{
	out[0] = 0;
	*state = "none";
	*foreign = 0;
}

int pc_cluster_multicast_ttl(void)
{
	return 0;
}

void pc_cluster_lane_info(struct clane_figs *f, int *max_dgram,
		int *lane_port)
{
	(void)f;
	*max_dgram = *lane_port = 0;
}

int pc_cluster_foreign_list(char *ids, int cap_ids, char addrs[][24],
		long long *ago_ms, unsigned long long *frames, int cap)
{
	(void)ids; (void)cap_ids; (void)addrs; (void)ago_ms; (void)frames;
	(void)cap;
	return 0;
}

unsigned long long pc_cluster_self_late(void)
{
	return 0;
}

unsigned long long pc_cluster_lane_saves(void)
{
	return 0;
}

unsigned long long pc_cluster_mprobe_holds(void)
{
	return 0;
}

void pc_cluster_set_identity_source(int src)
{
	(void)src;
}

void pc_cluster_set_expect(int n)
{
	(void)n;
}

/* S341: no fleet, nothing expected */
int pc_cluster_expect(void)
{
	return 0;
}

/* S343: no peers, nothing refused */
unsigned long long pc_cluster_bad_col_names(void)
{
	return 0;
}

void pc_cluster_set_lane(int max_dgram, int lane_port)
{
	(void)max_dgram;
	(void)lane_port;
}

void pc_cluster_set_lane_retry(int s)
{
	(void)s;
}

unsigned long long pc_cluster_lane_restored(void)
{
	return 0;
}

void pc_cluster_set_discovery(int unicast, const char *seeds)
{
	(void)unicast;
	(void)seeds;
}

void pc_cluster_discovery_info(const char **mode, int *seeds,
		int *seeds_resolved, int *known, unsigned long long *resolve_fail,
		unsigned long long *hints_sent, unsigned long long *hints_recv)
{
	*mode = "none";
	*seeds = *seeds_resolved = *known = 0;
	*resolve_fail = *hints_sent = *hints_recv = 0;
}

int pc_cluster_eager_miss_final(void)
{
	return 0;
}

/* S295/S321: nothing is forwarded or pulled here, so no request has a
 * target - the park holds no hint and no steered miss to retry. */
int pc_fwd_target_of(uint32_t req, unsigned int *ttl)
{
	(void)req;
	(void)ttl;
	return 0;
}
