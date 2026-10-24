/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * respcl.c - the RESP door's CLUSTER commands: the topology a stock Redis
 * client routes by (S44), the slot map serialised once per membership
 * snapshot (S75), and CLUSTER itself.  Moved from verbs.c unchanged (the
 * 0.7.2 split, wave 1 - doc/MODULARITY.md); it reaches verbs.c through
 * verbsint.h.
 */
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "cluster.h"
#include "pc_slot.h"
#include "daemon.h"                    /* PC_RESP_BUMP, pc_resp_slots_* */
#include "respout.h"
#include "verbsint.h"

/* ---- CLUSTER: the topology a stock Redis client routes by (S44) ------
 * Placement runs rendezvous over the SLOT, so ownership is derivable
 * here and a Redis client in any language can route without an extra
 * hop.  What this is NOT is Redis Cluster: there are no MOVED
 * redirects, and none are needed - a request landing on the wrong node
 * is FORWARDED, so a client with a stale map stays correct and merely
 * pays the hop it would have paid anyway.  That is why CLUSTER INFO
 * reports state ok whenever the fleet is up: there is no slot that
 * nobody can answer for.
 *
 * The slot space here is FRAGMENTED.  Rendezvous decides each slot
 * independently, so runs of a single owner average ~1.5 slots and a
 * three-node fleet yields ~11k ranges where Redis would have 3.  It is
 * truthful and clients cope (they expand any reply into a 16384-entry
 * table), but it makes CLUSTER SLOTS a large reply - measured and
 * discussed under S44 in doc/DESIGN.md.  CLUSTER SHARDS is ~6x smaller
 * for the same information because it does not repeat the node block
 * per range; prefer it where the client supports it.
 */

/* a 40-hex node name, the only shape Redis clients accept.  Built from
 * the PERSISTED identity when the peer has one, so a name survives a
 * restart the way a Redis node name does; a peer whose build predates
 * identities falls back to its node number, stable while it holds it. */
static void resp_cl_id(char *out, const struct pc_member *m)
{
	static const char hx[] = "0123456789abcdef";
	int i;

	if (m->has_ident) {
		for (i = 0; i < 16; i++) {
			out[(size_t)i * 2]     = hx[(m->ident[i] >> 4) & 0xF];
			out[(size_t)i * 2 + 1] = hx[m->ident[i] & 0xF];
		}
		for (i = 0; i < 8; i++)
			out[32 + i] = hx[(m->incarn >> ((7 - i) * 4)) & 0xF];
	} else {
		for (i = 0; i < 40; i++)
			out[i] = '0';
		out[38] = hx[(m->node >> 4) & 0xF];
		out[39] = hx[m->node & 0xF];
	}
}

/* the port the CLUSTER family advertises for a member: its RESP door.
 * A Redis client routed at client_port would be dialling the native
 * door, which off-box speaks only Noise - the client cannot even say
 * hello.  Falls back to client_port for a peer whose build predates the
 * gossiped field, which is exactly the old (broken off-box, workable
 * on-box) behaviour rather than a made-up number. */
static int resp_cl_port(const struct pc_member *m)
{
	return m->resp_port ? m->resp_port : m->client_port;
}

/* one member as [ip, port, id], the triple CLUSTER SLOTS wants */
static void resp_cl_node(struct pc_jw *w, const struct pc_member *m)
{
	char id[40], ip[INET_ADDRSTRLEN];

	resp_cl_id(id, m);
	if (!inet_ntop(AF_INET, &m->addr, ip, sizeof(ip)))
		ip[0] = 0;
	resp_arr(w, 3);
	resp_bulk(w, ip, strlen(ip));
	resp_int(w, resp_cl_port(m));
	resp_bulk(w, id, 40);
}

/* index of the member owning @slot, or -1.  pc_shard_owner_slot()
 * answers 0 for "this node", which is a node NUMBER everywhere else,
 * so the self case is resolved through the member list, not assumed. */
static int resp_cl_owner(const struct pc_member *mem, int nm, unsigned slot)
{
	int want = pc_shard_owner_slot(slot), i;

	for (i = 0; i < nm; i++) {
		if (want == 0 ? mem[i].is_self : mem[i].node == want)
			return i;
	}
	return -1;
}

/* fill @ownv with the owning member INDEX for every slot (0xFF: none).
 * Computed once per reply: both CLUSTER SLOTS and CLUSTER SHARDS walk
 * the same array, so the two replies cannot disagree, and neither pays
 * for 16384 rendezvous rounds twice. */
static void resp_cl_ownmap(const struct pc_member *mem, int nm,
		unsigned char *ownv)
{
	int s;

	for (s = 0; s < PC_SLOTS; s++) {
		int o = resp_cl_owner(mem, nm, (unsigned)s);

		ownv[s] = o < 0 ? 0xFF : (unsigned char)o;
	}
}

/* number of maximal equal-owner runs, counting only @who when it is
 * >= 0 (SHARDS asks per member) or every owner when it is -1 */
static int resp_cl_runs(const unsigned char *ownv, int who)
{
	int s, n = 0;

	for (s = 0; s < PC_SLOTS; s++) {
		if (ownv[s] == 0xFF || (who >= 0 && ownv[s] != who))
			continue;
		if (!s || ownv[s - 1] != ownv[s])
			n++;
	}
	return n;
}

/* ---- S75: the serialised slot map, cached per membership snapshot ----
 * CLUSTER SLOTS is a pure function of who the members are: building it
 * walks 16384 rendezvous rounds and writes ~750KB on a three-node fleet
 * (rendezvous scatters ownership, so ~11000 ranges).  Once S74 tells
 * clients the truth they all fetch it, on connect and on every topology
 * refresh, so the build is paid once per membership change instead of
 * once per client.  The client's own parsing cost is untouched - that is
 * the placement question (option 1 in the filing), measured, not
 * guessed at, before it is reopened.  Workers are threads: the copy out
 * happens under the lock. */
static pthread_mutex_t clmap_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { unsigned long long key; char *buf; size_t len; } clmap_cache[2];

/* FNV-1a over everything the reply depends on: the members, their
 * doors, their state, in snapshot order (self first - the reply differs
 * per node, so does the cache).  Placement reads the same peer table, so
 * a change that moves a slot changes this key. */
static unsigned long long resp_clmap_key(const struct pc_member *mem,
		int nm, int shards)
{
	unsigned long long h = FNV1A64_BASIS;
	int i;
#define CLMAP_MIX(_x) do { h = fnv1a64_u64(h, (uint64_t)(_x)); } while (0)
	CLMAP_MIX(nm);
	CLMAP_MIX(shards);
	for (i = 0; i < nm; i++) {
		CLMAP_MIX(mem[i].addr.s_addr);
		CLMAP_MIX(mem[i].node);
		CLMAP_MIX(mem[i].state);
		CLMAP_MIX(mem[i].client_port);
		CLMAP_MIX(mem[i].resp_port);
		CLMAP_MIX(mem[i].is_master);
	}
#undef CLMAP_MIX
	return h;
}

static int resp_clmap_cached(int shards, unsigned long long key,
		struct pc_jw *out)
{
	int hit = 0;

	pthread_mutex_lock(&clmap_mu);
	if (clmap_cache[shards].buf && clmap_cache[shards].key == key &&
	    pc_jw_init_heap(out, clmap_cache[shards].len + 16) == 0) {
		pc_jw_raw(out, clmap_cache[shards].buf, clmap_cache[shards].len);
		hit = 1;
	}
	pthread_mutex_unlock(&clmap_mu);
	return hit;
}

static void resp_clmap_store(int shards, unsigned long long key,
		const char *buf, size_t len)
{
	char *copy = malloc(len);

	if (!copy)
		return;                        /* answered, just not cached */
	memcpy(copy, buf, len);
	pthread_mutex_lock(&clmap_mu);
	free(clmap_cache[shards].buf);
	clmap_cache[shards].buf = copy;
	clmap_cache[shards].len = len;
	clmap_cache[shards].key = key;
	pthread_mutex_unlock(&clmap_mu);
}

int rh_cluster(struct resp_ctx *c)
{
	struct pc_member *mem = members_scratch();
	int nm = mem ? pc_cluster_members(mem, PC_CL_MAXMEMBERS) : 0;
	const char *sub = c->nargs >= 2 ? c->argv[1] : "";
	size_t subl = c->nargs >= 2 ? c->argl[1] : 0;

	/* KEYSLOT is answerable with no cluster at all, and is how
	 * an operator checks our slot maths against real Redis. */
	if (resp_is(sub, subl, "KEYSLOT")) {
		if (c->nargs != 3) {
			resp_err(c->out, "wrong number of arguments for "
				"'cluster|keyslot' command");
			return 0;
		}
		resp_int(c->out, pc_key_slot(c->argv[2], c->argl[2]));
		return 0;
	}
	if (resp_is(sub, subl, "MYID")) {
		char id[40];
		int i;

		for (i = 0; i < nm && !mem[i].is_self; i++)
			;
		if (i == nm) {
			resp_err(c->out, "this node is not in a cluster");
			return 0;
		}
		resp_cl_id(id, &mem[i]);
		resp_bulk(c->out, id, 40);
		return 0;
	}
	if (resp_is(sub, subl, "INFO")) {
		struct pc_jw b;
		int on = resp_cluster_on();   /* S74: one truth, INFO reads it too */

		pc_jw_init(&b, c->scratch, c->scratch_cap);
		pc_jw_lit(&b, "cluster_enabled:");
		pc_jw_i64(&b, on);
		/* ok whenever the fleet is up: every slot has an
		 * owner, and a miss is forwarded rather than
		 * refused, so there is no "slot not served" state */
		pc_jw_lit(&b, "\r\ncluster_state:");
		pc_jw_lit(&b, on ? "ok" : "fail");
		pc_jw_lit(&b, "\r\ncluster_slots_assigned:");
		pc_jw_i64(&b, on ? PC_SLOTS : 0);
		pc_jw_lit(&b, "\r\ncluster_slots_ok:");
		pc_jw_i64(&b, on ? PC_SLOTS : 0);
		pc_jw_lit(&b, "\r\ncluster_slots_pfail:0"
			"\r\ncluster_slots_fail:0"
			"\r\ncluster_known_nodes:");
		pc_jw_i64(&b, nm);
		pc_jw_lit(&b, "\r\ncluster_size:");
		pc_jw_i64(&b, nm);
		pc_jw_lit(&b, "\r\ncluster_current_epoch:0"
			"\r\ncluster_my_epoch:0\r\n");
		if (b.overflow)
			resp_err(c->out, "cluster info too large");
		else
			resp_bulk(c->out, b.buf, b.len);
		return 0;
	}

	if (!nm) {
		resp_err(c->out, "This instance has cluster support "
			"disabled");
		return 0;
	}

	if (resp_is(sub, subl, "NODES")) {
		/* The LEGACY text topology, and still the one the
		 * widest tooling reads: captured on the wire,
		 * redis-benchmark --cluster opens with exactly
		 * "CLUSTER NODES" (never INFO first), and Lettuce
		 * parses this format too.  Implemented S49, because
		 * without it the ecosystem's own load driver cannot
		 * route here and S44's payoff cannot even be measured.
		 *
		 * One line per node:
		 *   <id> <ip:port@cport> <flags> <master> <ping-sent>
		 *   <pong-recv> <config-epoch> <link-state> <slots...>
		 * There is no cluster bus, so cport repeats the client
		 * port - parsers split on the @ and use what precedes
		 * it.  No replicas exist, so flags are master (plus
		 * myself on the answering node) and the master field
		 * is "-". */
		struct pc_jw tw;
		unsigned char *ownv;
		int i, s;

		/* tw is a STACK local: pc_jw_init_heap() refuses a
		 * writer whose len is nonzero (its stranded-bytes
		 * guard), and uninitialised stack read as exactly
		 * that.  Found by the map test: NODES answered "out
		 * of memory" on any worker whose stack was dirty
		 * from earlier requests, and worked on a virgin
		 * daemon - which is why every smoke test passed. */
		memset(&tw, 0, sizeof tw);
		ownv = malloc(PC_SLOTS);
		if (!ownv) {
			resp_err(c->out, "out of memory building the "
				"slot map");
			return 0;
		}
		resp_cl_ownmap(mem, nm, ownv);
		/* worst case is one range per slot (~200KB of text);
		 * sized for the common case, grows if wrong */
		if (pc_jw_init_heap(&tw, (size_t)resp_cl_runs(ownv, -1)
		        * 13 + (size_t)nm * 128) != 0) {
			free(ownv);
			resp_err(c->out, "out of memory building the "
				"slot map");
			return 0;
		}
		for (i = 0; i < nm; i++) {
			char id[40], ip[INET_ADDRSTRLEN];

			resp_cl_id(id, &mem[i]);
			if (!inet_ntop(AF_INET, &mem[i].addr, ip,
			        sizeof(ip)))
				ip[0] = 0;
			pc_jw_raw(&tw, id, 40);
			pc_jw_lit(&tw, " ");
			pc_jw_lit(&tw, ip);
			pc_jw_lit(&tw, ":");
			pc_jw_i64(&tw, resp_cl_port(&mem[i]));
			pc_jw_lit(&tw, "@");
			pc_jw_i64(&tw, resp_cl_port(&mem[i]));
			pc_jw_lit(&tw, mem[i].is_self ?
				" myself,master" : " master");
			pc_jw_lit(&tw, " - 0 0 0 connected");
			for (s = 0; s < PC_SLOTS; s++) {
				int e;

				if (ownv[s] != i)
					continue;
				if (s && ownv[s - 1] == ownv[s])
					continue;
				for (e = s; e + 1 < PC_SLOTS &&
				        ownv[e + 1] == ownv[s]; e++)
					;
				pc_jw_lit(&tw, " ");
				pc_jw_i64(&tw, s);
				if (e > s) {
					pc_jw_lit(&tw, "-");
					pc_jw_i64(&tw, e);
				}
			}
			pc_jw_lit(&tw, "\n");
		}
		free(ownv);
		if (tw.overflow)
			resp_err(c->out, "cluster nodes reply too large");
		else
			resp_bulk(c->out, tw.buf, tw.len);
		pc_jw_free(&tw);
		return 0;
	}
	if (resp_is(sub, subl, "SLOTS") || resp_is(sub, subl, "SHARDS")) {
		int is_shards = resp_is(sub, subl, "SHARDS");
		unsigned long long key = resp_clmap_key(mem, nm, is_shards);
		struct pc_jw tw;
		unsigned char *ownv;
		int i, s, nr;

		/* S75: serve the last build while the membership
		 * snapshot's key still matches */
		if (resp_clmap_cached(is_shards, key, c->out)) {
			PC_RESP_BUMP(pc_resp_slots_hits);
			return 0;
		}
		/* tw is a STACK local: pc_jw_init_heap() refuses a
		 * writer whose len is nonzero, and dirty stack reads
		 * as exactly that (see CLUSTER NODES) */
		memset(&tw, 0, sizeof tw);
		/* 16KB, allocated per call rather than kept in a
		 * static: workers are THREADS, and a shared scratch
		 * buffer here would corrupt one reply with another's
		 * walk under any concurrency at all. */
		ownv = malloc(PC_SLOTS);
		if (!ownv) {
			resp_err(c->out, "out of memory building the "
				"slot map");
			return 0;
		}
		resp_cl_ownmap(mem, nm, ownv);
		nr = resp_cl_runs(ownv, -1);
		/* SLOTS is the one reply whose size a request does
		 * not bound: it enumerates the slot space, and
		 * rendezvous scatters ownership, so a 4-node fleet
		 * already exceeds the fixed per-worker scratch.  A
		 * heap buffer sized for the ranges about to be
		 * written - ~101 bytes each at the widest (5-digit
		 * slots, a 15-char address, a 40-char name) - so the
		 * common case never reallocs. */
		if (pc_jw_init_heap(&tw, (size_t)nr * 104 +
		        (size_t)nm * 256 + 64) != 0) {
			free(ownv);
			resp_err(c->out, "could not build the slot map");
			return 0;
		}
		if (!is_shards) {
			resp_arr(&tw, nr);
			for (s = 0; s < PC_SLOTS; s++) {
				int e;

				if (ownv[s] == 0xFF)
					continue;
				if (s && ownv[s - 1] == ownv[s])
					continue;
				for (e = s; e + 1 < PC_SLOTS &&
				        ownv[e + 1] == ownv[s]; e++)
					;
				resp_arr(&tw, 3);
				resp_int(&tw, s);
				resp_int(&tw, e);
				resp_cl_node(&tw, &mem[ownv[s]]);
			}
		} else {
			/* SHARDS groups the same ranges by owner,
			 * writing the node block once instead of once
			 * per range - the difference between a ~130KB
			 * and a ~770KB reply on a three-node fleet. */
			resp_arr(&tw, nm);
			for (i = 0; i < nm; i++) {
				char id[40], ip[INET_ADDRSTRLEN];

				nr = resp_cl_runs(ownv, i);
				resp_arr(&tw, 4);
				resp_bulk(&tw, "slots", 5);
				resp_arr(&tw, nr * 2);
				for (s = 0; s < PC_SLOTS; s++) {
					int e;

					if (ownv[s] != i)
						continue;
					if (s && ownv[s - 1] == ownv[s])
						continue;
					for (e = s; e + 1 < PC_SLOTS &&
					        ownv[e + 1] == ownv[s]; e++)
						;
					resp_int(&tw, s);
					resp_int(&tw, e);
				}
				resp_bulk(&tw, "nodes", 5);
				resp_arr(&tw, 1);
				resp_cl_id(id, &mem[i]);
				if (!inet_ntop(AF_INET, &mem[i].addr, ip,
				        sizeof(ip)))
					ip[0] = 0;
				resp_arr(&tw, 14);
				resp_bulk(&tw, "id", 2);
				resp_bulk(&tw, id, 40);
				resp_bulk(&tw, "port", 4);
				resp_int(&tw, resp_cl_port(&mem[i]));
				resp_bulk(&tw, "ip", 2);
				resp_bulk(&tw, ip, strlen(ip));
				resp_bulk(&tw, "endpoint", 8);
				resp_bulk(&tw, ip, strlen(ip));
				/* every node owns its slots outright -
				 * there are no replicas of a shard here -
				 * so the only honest role is master */
				resp_bulk(&tw, "role", 4);
				resp_bulk(&tw, "master", 6);
				resp_bulk(&tw, "replication-offset", 18);
				resp_int(&tw, 0);
				resp_bulk(&tw, "health", 6);
				resp_bulk(&tw, "online", 6);
			}
		}
		free(ownv);
		PC_RESP_BUMP(pc_resp_slots_builds);
		if (tw.overflow) {
			resp_err(c->out, "could not build the slot map");
		} else {
			resp_clmap_store(is_shards, key, tw.buf, tw.len);
			if (pc_jw_init_heap(c->out, tw.len + 16) != 0)
				resp_err(c->out, "could not build the "
					"slot map");
			else
				pc_jw_raw(c->out, tw.buf, tw.len);
		}
		pc_jw_free(&tw);
		return 0;
	}
	if (resp_is(sub, subl, "COUNTKEYSINSLOT")) {
		/* the count is not tracked per slot, and answering
		 * a made-up number would be worse than refusing */
		unk_sub(c);                    /* S165: refused on purpose, still a gap */
		resp_err(c->out, "unsupported CLUSTER subcommand "
			"(keys are not counted per slot)");
		return 0;
	}
	unk_sub(c);                            /* S165 */
	resp_err(c->out, "unsupported CLUSTER subcommand");
	return 0;
}
