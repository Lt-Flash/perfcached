/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clcid.h - S302: the cluster id, so two clusters never merge.
 *
 * A node used to persist only its own identity, and two fleets sharing a
 * multicast group, a secret and a config were indistinguishable: they
 * folded into one, data and all.  Now the node that FOUNDS a cluster
 * mints a 16-byte id (a UUID v7 - its founding time, then random) and
 * every member adopts it; every datagram carries it in its authenticated
 * header (clwire.h), and a node that holds an established id refuses
 * datagrams from any other.
 *
 * Lifecycle, per process:
 *   NONE         not yet in a cluster - seals with zeros, accepts any id;
 *                if it must found while it hears live members of a
 *                cluster, it founds into THAT id (pc_cid_adopt) rather
 *                than minting one their members would refuse
 *   PROVISIONAL  minted on founding, or adopted on joining, < 30 s ago.
 *                A cold start can have two founders before either hears
 *                the other; while provisional, a founder (or a node that
 *                is joining) still hears a foreign MASTER and yields to it
 *                (always to an established one), giving its id up before
 *                it joins that one.  A step-down or re-join inside its
 *                own cluster keeps the id.
 *   ESTABLISHED  held 30 s, or loaded from the state directory at start,
 *                or pinned by [cluster] cluster_id.  Never changes again
 *                in this process; datagrams of any other id are refused.
 *
 * The file is <state_dir>/cluster-id, written through a temp file and a
 * rename before the id is used (as the term is).  A provisional id that
 * is given up is removed from disk too, so a restart cannot resurrect it
 * as established.
 */
#ifndef PC_CLCID_H
#define PC_CLCID_H

#define PC_CID_FILE      "cluster-id"
#define PC_CID_GRACE_MS  30000

/* At start: load the persisted id (ESTABLISHED), apply @pin (a UUID, or
 * any other string, hashed into a v8 UUID) - a pin that disagrees with the
 * persisted id refuses the start.  @state_dir NULL = nothing persists
 * (said once).  0 ok, -1 refuse to start (logged). */
int pc_cid_init(const char *state_dir, const char *pin);
/* become a founder: mint a PROVISIONAL id if this node has none */
void pc_cid_mint(long long now_ms);
/* adopt the master's @cid on joining (PROVISIONAL), replacing a
 * provisional one; an established id is never replaced */
void pc_cid_adopt(const unsigned char cid[16], long long now_ms);
/* give a PROVISIONAL id up (a yield to another cluster) - no-op
 * otherwise */
void pc_cid_drop(void);
/* promote a provisional id that has lasted PC_CID_GRACE_MS */
void pc_cid_tick(long long now_ms);
/* CLWIRE_CID_*, and the id into @out */
int pc_cid_get(unsigned char out[16]);
/* "8-4-4-4-12" lowercase, or "" with no id */
void pc_cid_str(const unsigned char cid[16], char out[37]);
const char *pc_cid_state_name(int state);

#endif
