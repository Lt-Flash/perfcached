/*
 * clretain.h - S238: RETAINED TOMBSTONES.
 *
 * The negative cache holds a tombstone for tombstone_ms (2 s): long enough
 * for a set and its delete crossing on the wire (S209), nothing like long
 * enough for a partition.  Measured (DESIGN S238): after a heal, a
 * returning peer's repair sweep re-sent keys the other side had deleted,
 * and a read that missed locally pulled a stale copy back - 50 of 50
 * deletes made during the split came back, on every node.
 *
 * This table remembers every tombstone this node plants - its own
 * deletes and those applied from peers - for `retain_s` (a Cassandra
 * gc_grace), capped at `max` entries, oldest out first.  pc_neg_ver()
 * falls back to it, so every receive path that already refused a copy
 * older than a 2-s tombstone refuses one older than a retained one; and
 * the per-peer sweep replays it to a peer coming back (cluster.c).
 *
 * Memory is malloc'd OUTSIDE the arena (arena_mb stays the record
 * bound), counted in bytes and reported.  In memory only: a restart is
 * B4's reconcile's case, a heal is this one's.  Sharded, one mutex per
 * shard: planted on workers, looked up on the apply and peer threads.
 */
#ifndef PC_CLRETAIN_H
#define PC_CLRETAIN_H

#include <stddef.h>
#include <stdint.h>

struct clretain_stats {
	unsigned long long entries;        /* held now */
	unsigned long long bytes;          /* what they cost, headers in */
	unsigned long long dropped_early;  /* evicted by the cap before retain_s */
	unsigned long long noted;          /* tombstones ever planted here */
	unsigned int retain_s, max;
	unsigned int oldest_age_s;         /* 0 when empty */
};

/* 0 = off (retain_s 0), else the table exists - call once, before use */
void clretain_init(unsigned int retain_s, unsigned int max);
int clretain_enabled(void);

/* remember a tombstone at @now_s (the highest version wins, the time is
 * refreshed) */
void clretain_note(const char *col, size_t cn, const char *key, size_t kn,
		uint64_t ver, unsigned int now_s);

/* the retained version for the key, 0 when none (or older than retain_s) */
uint64_t clretain_get(const char *col, size_t cn, const char *key, size_t kn,
		unsigned int now_s);

/* call @cb for every retained tombstone planted in (@since_s, @upto_s];
 * returns how many.  @cb runs under a shard lock: it must not call back in */
unsigned int clretain_each(unsigned int since_s, unsigned int upto_s,
		void (*cb)(const char *col, size_t cn, const char *key, size_t kn,
			uint64_t ver, void *ctx), void *ctx);

/* drop what is older than retain_s (1 Hz) */
void clretain_expire(unsigned int now_s);

void clretain_get_stats(struct clretain_stats *out, unsigned int now_s);
unsigned long long clretain_dropped_early(void);

#endif
