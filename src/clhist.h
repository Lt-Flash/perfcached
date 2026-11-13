/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clhist.h - the identity history the master syncs to the backup: HAVE WE SEEN THIS NODE BEFORE
 * (RECOVERING, not STARTING) - a node with a wiped state directory sincerely believes it is new */
#ifndef PC_CLHIST_H
#define PC_CLHIST_H

#include <stdint.h>

/* 24KB in the sync payload; when full the least recently seen goes - a forgotten node is called
 * STARTING and costs a needless backfill, never data (receivers refuse copies that are not newer) */
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

/* record or refresh an identity: 1 already known (RECOVERING), 0 first sight (STARTING) */
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
