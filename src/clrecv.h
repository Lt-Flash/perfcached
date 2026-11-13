/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clrecv.h - replication receive/apply: what cluster.c's dispatch, the apply
 * rings and the bulk plane call in clrecv.c. */
#ifndef PC_CLRECV_H
#define PC_CLRECV_H

#include <stddef.h>

#include "compat/str.h"
#include "core/pcache_htable.h"      /* pcache_htable_t */
#include "pcaddr.h"

struct clapply_rec;
struct clapply_grp;

/* peer thread: dispatch_dgram's M_MIGRATE, M_MIGRATE_MANY, M_REPL_MANY */
void handle_migrate(const unsigned char *pt, size_t n,
		const pc_addr *from);
void handle_migrate_many(const unsigned char *pt, size_t n,
		const pc_addr *from, int passive, int here);
/* a relay receive thread (clrp_rx): a write-path group, applied there */
void cl_repl_apply_group(const unsigned char *pt, size_t n,
		const pc_addr *from);

/* clapply_init's apply and ack callbacks: whichever thread applies */
int apply_rec(const struct clapply_rec *r, void *ht_or_null);
void ack_group(const struct clapply_grp *g, void *ctx);

/* the bulk plane's receive (bulk_pull_from, bulk_rx) */
int dropfloor_refuses(const char *col, size_t cn, const char *key,
		size_t kn, unsigned long long ver);
void nomem_drop(pcache_htable_t *ht, const char *col, size_t cn,
		const str *k, unsigned long long ver);

#endif /* PC_CLRECV_H */
