/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * upstream.h - read-through from a RESP upstream (task S331, DESIGN 12ix).
 *
 * A collection may name an upstream - Redis, Valkey, another perfcached -
 * that this node READS on a local miss and never writes.  Fetched values
 * live in the collection's SHADOW table (pcache_ht_set_shadow): node-local,
 * never in the WAL, a snapshot or a peer, read by the table's fetch family
 * when its own table misses.  The gate below runs before a keyed command:
 * a key it must fetch parks the command (a RESP connection HOLDS, nothing
 * after it is read), an upstream thread fetches, and the command runs
 * again on completion.
 */
#ifndef PC_UPSTREAM_H
#define PC_UPSTREAM_H

#include <stddef.h>
#include "core/pcache_htable.h"

struct pc_config;
struct pc_jw;
struct pc_tw;

/* the gate's answer */
#define PC_UPG_PROCEED  0             /* run the command */
#define PC_UPG_HELD     1             /* parked: the command runs again later */
#define PC_UPG_ANSWERED 2             /* the gate wrote the reply (an error) */

/* a completion's verdict, carried to the re-run on the connection */
#define PC_UPV_NONE     0
#define PC_UPV_OK       1             /* every key was fetched (or found absent) */
#define PC_UPV_ERROR    2             /* the upstream failed: on_error decides */

/* after the store has its collections, before the workers start: a shadow
 * table and the connection threads for every collection with an upstream.
 * @nworkers sizes the completion queues (worker ids index them). */
int pc_upstream_init(const struct pc_config *cfg, int nworkers);
void pc_upstream_shutdown(void);

/* is any collection read-through?  (a cheap gate for the doors) */
int pc_upstream_any(void);

/* the worker side: register the wake fd, drain completions on its event */
void pc_upstream_worker_register(int worker, int efd);
void pc_upstream_drain(int worker);

/* the command classes the gate knows */
#define PC_UPC_NONE   0               /* never falls through */
#define PC_UPC_READ   1               /* reads: fetched on a miss */
#define PC_UPC_MUTATE 2               /* changes the value: fetched, then
                                       * PROMOTED before it runs */
#define PC_UPC_PLAIN  3               /* overwrites whatever is there */

/* what the gate answered with, for the door to render */
struct pc_upg {
	int wrongtype;                     /* the reply is WRONGTYPE */
	const char *err;                   /* else this error */
	unsigned int req;                  /* HELD: the id the door parks under */
};

/* the class of command @c (a RESP name, or an H command in a binary hcmd)
 * given its arguments (NULL: classed by name alone); *@del a delete or a
 * re-arm, *@all every argument from 1 is a key */
int pc_upstream_cmd_class(const char *c, size_t cl, char *const *argv,
		const size_t *argl, int nargs, int *del, int *all);

/* the door-neutral gate: @keys/@kl/@nkeys the command's keys in @ht's
 * collection, @verdict the completion's (PC_UPV_*) on a re-run.  HELD:
 * the door parks the request under g->req and runs it again on that
 * id's completion; ANSWERED: g says the reply. */
int pc_upstream_gate(pcache_htable_t *ht, const char *col, size_t collen,
		int cls, int del, const char *const *keys, const size_t *kl,
		int nkeys, int verdict, struct pc_upg *g);

/* the RESP gate, before a keyed command's handler.  @ht/@col the selected
 * collection; @argv/@argl/@nargs the command; @conn the connection (its
 * verdict is read and cleared here).  PC_UPG_*: on ANSWERED the reply is in
 * @out; on HELD nothing is written and the connection holds. */
int pc_upstream_resp_gate(pcache_htable_t *ht, const char *col, size_t collen,
		char *const *argv, const size_t *argl, int nargs,
		struct pc_jw *out, void *conn);

/* the drain sweep (an operator's privileged one-shot): read every key the
 * upstream holds under the allow-list and let the workers take over what
 * the fleet does not have yet.  @stop asks a running one to stop.  0 ok,
 * -1 refused; *@msg says which. */
int pc_upstream_drain_ctl(const char *col, size_t collen, int stop,
		const char **msg);

/* the collection's `upstream` block in stats (nothing for a collection
 * without one) */
void pc_upstream_stats_tree(struct pc_tw *out, const char *col, size_t collen);

#endif /* PC_UPSTREAM_H */
