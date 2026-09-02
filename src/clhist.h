/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clhist.h — the identity history (control plane, step 3b).
 *
 * Part of what the master syncs to the backup, and the piece that is
 * easy to leave out.  It answers one question: HAVE WE SEEN THIS NODE
 * BEFORE?  That is what separates STARTING (a genuinely new node) from
 * RECOVERING (one that is coming back), and it is not a judgement the
 * node itself can make - a node whose state directory was wiped, or one
 * running an ephemeral identity, sincerely believes it is new.
 *
 * A backup promoted without this would call every returning node new,
 * which is why it belongs in the synced payload rather than being
 * rebuilt from whatever the new master happens to remember.
 *
 * IT IS BOUNDED, AND FORGETTING IS SAFE.  A cluster can see an unbounded
 * number of identities over its life - every container restart with no
 * volume mints a fresh one - so this cannot grow without limit.  When
 * full, the least recently seen entry is dropped.
 *
 * The cost of forgetting is WORK, not correctness: a returning node whose
 * identity was evicted is called STARTING, so peers may backfill it when
 * they did not need to.  Its data is not at risk, because a receiver
 * refuses a copy that is not newer than the one it holds - the version
 * comparison is what makes this bound affordable.  Without that, evicting
 * an identity could mean clobbering a rejoiner's newer data, and the
 * history would have to be exact.
 */
#ifndef PC_CLHIST_H
#define PC_CLHIST_H

#include <stdint.h>

/* 1024 x 24 bytes = 24KB, and it rides the sync payload rather than a
 * datagram, so the bound is about memory and churn rather than MTU. */
#define PC_CLHIST_MAX   1024
#define PC_CLHIST_ENTSZ 24

struct pc_clhist_ent {
	unsigned char ident[16];
	uint16_t node_id;              /* the id it last held */
	uint16_t _pad;
	uint32_t seen;                 /* monotonic stamp, for eviction order */
};

struct pc_clhist {
	uint16_t n;
	uint32_t clock;                /* bumped per note(); NOT wall time */
	struct pc_clhist_ent ent[PC_CLHIST_MAX];
};

void pc_clhist_init(struct pc_clhist *h);

/* Record or refresh an identity.  Returns 1 if it was already known
 * (the node is RECOVERING), 0 if this is the first sight of it (it is
 * STARTING).  Evicts the least recently seen entry when full. */
int pc_clhist_note(struct pc_clhist *h, const unsigned char ident[16],
		uint16_t node_id);

/* 1 = seen before, without recording a sighting. */
int pc_clhist_seen(const struct pc_clhist *h, const unsigned char ident[16]);

/* The id this identity last held, or 0.  A rejoiner keeping its id is
 * what lets peers' replication marks stay meaningful across a restart. */
uint16_t pc_clhist_last_id(const struct pc_clhist *h,
		const unsigned char ident[16]);

/* Wire form for the sync payload: u16 count then count x 24 bytes.
 * Returns bytes written / 0, or -1 on a bad buffer. */
long pc_clhist_encode(const struct pc_clhist *h, unsigned char *buf, size_t cap);
int pc_clhist_decode(const unsigned char *buf, size_t n, struct pc_clhist *out,
		const char **why);

#endif /* PC_CLHIST_H */
