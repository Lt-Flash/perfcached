/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clcid.h - the cluster id, so two clusters never merge: minted by the founder,
 * sealed into every datagram; an established id refuses datagrams of any other. */
#ifndef PC_CLCID_H
#define PC_CLCID_H

#define PC_CID_FILE      "cluster-id"
#define PC_CID_GRACE_MS  30000

/* load the persisted id and apply @pin (a UUID, or a name hashed into a v8 UUID);
 * -1 = refuse to start (logged), e.g. a pin that disagrees with the disk */
int pc_cid_init(const char *state_dir, const char *pin);
/* become a founder: mint a PROVISIONAL id if this node has none */
void pc_cid_mint(long long now_ms);
/* adopt the master's @cid on joining (PROVISIONAL: it still yields to a foreign
 * master for PC_CID_GRACE_MS); an established id is never replaced */
void pc_cid_adopt(const unsigned char cid[16], long long now_ms);
/* give a PROVISIONAL id up (a yield to another cluster), on disk too so a
 * restart cannot resurrect it as established; no-op otherwise */
void pc_cid_drop(void);
/* promote a provisional id that has lasted PC_CID_GRACE_MS, once its write was made */
void pc_cid_tick(long long now_ms);
/* pc-maint: write (or remove) the file as mint, adopt and drop left it; 1 done, 0 nothing due,
 * -1 refused (said once, retried next call) */
int pc_cid_persist_tick(void);
/* CLWIRE_CID_*, and the id into @out */
int pc_cid_get(unsigned char out[16]);
/* "8-4-4-4-12" lowercase, or "" with no id */
void pc_cid_str(const unsigned char cid[16], char out[37]);
const char *pc_cid_state_name(int state);

#endif
