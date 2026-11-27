/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clepoch.c - the cluster wire epoch (clepoch.h): owns the committed epoch and the newest this
 * build speaks.  test/clepochtest.c. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "compat/dprint.h"
#include "cluster.h"
#include "clpeers.h"
#include "clstate.h"
#include "clepoch.h"
#include "logrl.h"

/* the epoch this node speaks (the fleet's committed one, from the master's keepalive, persisted
 * as node-epoch so a new master continues with it) and the newest it can */
static int cl_committed = PC_CL_EPOCH;
static int cl_epoch_max = PC_CL_EPOCH_MAX;

/* may we talk to a peer announcing @theirs?  About what the BYTES mean, so checked before the
 * config digest and nothing rescues it; a peer sending no epoch reads as 0 and is refused */
int epoch_ok(int theirs, const pc_addr *from, const char *what)
{
	/* any epoch this build carries a codec for is read; what this
	 * node SPEAKS is the committed one (cl_committed) */
	if (theirs >= PC_CL_EPOCH_MIN && theirs <= cl_epoch_max)
		return 1;
	if (pc_logrl_ok(!strcmp(what, "join") ? LRL_EPOCH_JOIN :
	        !strcmp(what, "keepalive") ? LRL_EPOCH_ALIVE :
	        !strcmp(what, "master keepalive") ? LRL_EPOCH_MASTER :
	        LRL_EPOCH_OTHER, from))
		LM_ERR("cluster: REFUSING %s from %s - it speaks cluster wire epoch "
			"%d and we speak %d..%d.  The epoch changes only when a frame "
			"means something an older build would MIS-EXECUTE rather than "
			"ignore, so these two daemons cannot safely share a cluster: run "
			"one release on every node (one that also speaks the old epoch "
			"rolls, then commits the new; one that does not moves the whole "
			"fleet at once).  Neither node is harmed by this refusal.\n",
			what, pc_addr_str(from), theirs, PC_CL_EPOCH_MIN,
			cl_epoch_max);
	return 0;
}

/* a peer's fact from the settings its keepalive carried */
int peer_fact(const struct peer *p, uint32_t key, uint32_t *val)
{
	int i;

	for (i = 0; i < p->ncfg; i++)
		if (p->cfg_name[i] == key) {
			*val = p->cfg_val[i];
			return 1;
		}
	return 0;
}

/* can @p speak @epoch?  1 yes, 0 no (its range is known and excludes it), -1 unknown (an older
 * build says nothing) */
int peer_speaks(const struct peer *p, int epoch, int *lo, int *hi)
{
	uint32_t fv;

	if (!peer_fact(p, PC_FACT_EPOCHS, &fv))
		return -1;
	*lo = (int)(fv >> 8) & 0xff;
	*hi = (int)fv & 0xff;
	return epoch >= *lo && epoch <= *hi;
}

void epoch_save(void)
{
	char path[600], tmp[620];
	FILE *f;

	if (!cl_state_dir()[0])
		return;
	snprintf(path, sizeof path, "%.520s/node-epoch", cl_state_dir());
	snprintf(tmp, sizeof tmp, "%.600s.tmp", path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "%d\n", cl_committed);
	if (fclose(f) != 0 || rename(tmp, path) != 0)
		unlink(tmp);
}

void epoch_load(void)
{
	char path[600];
	FILE *f;
	int e = 0, older = 0;

	if (!cl_state_dir()[0])
		return;
	snprintf(path, sizeof path, "%.520s/node-epoch", cl_state_dir());
	f = fopen(path, "r");
	if (!f)
		return;
	if (fscanf(f, "%d", &e) == 1 && e >= PC_CL_EPOCH_MIN && e <= cl_epoch_max)
		cl_committed = e;
	else if (e > cl_epoch_max)
		LM_ERR("cluster: %s says the fleet is committed to wire epoch %d, "
			"which this build does not speak (%d..%d) - it cannot join "
			"that fleet; it starts speaking %d\n", path, e,
			PC_CL_EPOCH_MIN, cl_epoch_max, cl_committed);
	else if (e) {                      /* an earlier release's fleet: the whole fleet moves */
		LM_NOTICE("cluster: %s names wire epoch %d, an earlier release's; this build "
			"speaks %d..%d, so every node of the fleet must run it\n", path, e,
			PC_CL_EPOCH_MIN, cl_epoch_max);
		older = 1;
	}
	fclose(f);
	if (older)
		epoch_save();                  /* the notice is said once */
}

int pc_cluster_epoch(void)
{
	return __atomic_load_n(&cl_committed, __ATOMIC_RELAXED);
}

int pc_cluster_epoch_min(void)
{
	return PC_CL_EPOCH_MIN;
}

int pc_cluster_epoch_max(void)
{
	return cl_epoch_max;
}

int pc_cluster_epoch_commit(int epoch, char *why, size_t n)
{
	long long now = cl_now_ms();
	int i, lo, hi;

	if (!cl_enabled()) {
		snprintf(why, n, "no cluster here");
		return -1;
	}
	if (cl_role() != PC_ROLE_MASTER) {
		snprintf(why, n, "not the master - ask node %d", cl_master_id());
		return -1;
	}
	if (epoch < PC_CL_EPOCH_MIN || epoch > cl_epoch_max) {
		snprintf(why, n, "this node speaks wire epochs %d..%d, not %d",
			PC_CL_EPOCH_MIN, cl_epoch_max, epoch);
		return -1;
	}
	for (i = 0; i < cl_peers()->n_peers; i++) {
		const struct peer *p = &cl_peers()->peers[i];
		int id = __atomic_load_n(&p->node, __ATOMIC_ACQUIRE), r;

		if (!id || id == cl_node_id() || !clpeers_live(p, now))
			continue;
		r = peer_speaks(p, epoch, &lo, &hi);
		if (r < 0) {
			snprintf(why, n, "node %d says nothing about the epochs it "
				"speaks (a build before 0.6.5) - upgrade it first", id);
			return -1;
		}
		if (!r) {
			snprintf(why, n, "node %d speaks wire epochs %d..%d, not %d - "
				"upgrade it first", id, lo, hi, epoch);
			return -1;
		}
	}
	if (epoch == cl_committed) {
		snprintf(why, n, "the fleet speaks wire epoch %d already", epoch);
		return -1;
	}
	LM_NOTICE("cluster: wire epoch %d -> %d committed here (node %d), every "
		"live member speaks it; the keepalive carries it\n", cl_committed,
		epoch, cl_node_id());
	__atomic_store_n(&cl_committed, epoch, __ATOMIC_RELAXED);
	epoch_save();
	cl_st_epoch_commits();
	return 0;
}

void epoch_adopt(int epoch)
{
	__atomic_store_n(&cl_committed, epoch, __ATOMIC_RELAXED);
	epoch_save();
}

void epoch_set_max(int epoch)
{
	cl_epoch_max = epoch;
}
