/* clretain.h - retained tombstones, kept retain_s (capped at max, outside the arena) to refuse and
 * replay deletes after a partition; sharded: noted on workers, read on apply and peer threads. */
#ifndef PC_CLRETAIN_H
#define PC_CLRETAIN_H

#include <stddef.h>
#include <stdint.h>

struct clretain_stats {
	unsigned long long entries;        /* held now */
	unsigned long long bytes;          /* what they cost, headers in */
	unsigned long long dropped_early;  /* evicted by the cap before retain_s */
	unsigned long long noted;          /* tombstones planted here; one already held at
	                                    * that version or newer is not counted again */
	unsigned long long local;          /* of entries, this node's own
	                                    * drops - refused, never replayed */
	/* of the entries, early drops and notes: the EXPIRIES (keys an eager collection reaped), or
	 * a TTL workload reads as deletes */
	unsigned long long expiries;
	unsigned long long dropped_early_expiries;
	unsigned long long noted_expiries;
	/* of the entries, numbered deletes; gone because every member holds them (forgotten), or,
	 * expiries, because the fleet confirms and each member reaps its own */
	unsigned long long numbered, forgotten, forgotten_expiries;
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
/* the same for a drop this node made for ITSELF (an invalidation, a startup flush): it refuses
 * older copies but is never replayed; a real delete or an expiry of the key is held apart */
void clretain_note_local(const char *col, size_t cn, const char *key,
		size_t kn, uint64_t ver, unsigned int now_s);

/* the same for a key an eager collection reaped, counted apart; @keep (its TTL was shortened):
 * it keeps the time rule and the cap treats it as a delete, else it may go early and first */
void clretain_note_expiry(const char *col, size_t cn, const char *key,
		size_t kn, uint64_t ver, unsigned int now_s, int keep);

/* a delete NUMBERED at its origin: held as clretain_note holds one and indexed by its space in
 * arrival order, so the marks can forget it; a newer delete takes the entry over */
void clretain_note_n(const char *col, size_t cn, const char *key, size_t kn,
		uint64_t ver, unsigned int now_s, uint64_t oid, uint32_t incarn,
		uint64_t seq);
/* drop entries of space (oid, incarn) numbered at or below @upto, from the front of arrival
 * order: one late behind a higher seq waits for the mark to pass it.  Returns how many */
unsigned long clretain_forget(uint64_t oid, uint32_t incarn, uint64_t upto,
		unsigned int now_s);
/* per space, the highest @upto that forgot something and when: what a peer back from a restart
 * must hold before this node vouches for a window that began before @at_s */
int clretain_forgot_each(int (*cb)(uint64_t oid, uint32_t incarn,
		uint64_t through, unsigned int at_s, void *ctx), void *ctx);
/* the space's highest seq no longer held other than by a newer numbered delete of the same key
 * (forgotten, time rule, cap, no room, an unnumbered drop): UINT64_MAX when that is not known */
uint64_t clretain_space_gone(uint64_t oid, uint32_t incarn);
/* at most @max entries of space (oid, incarn) numbered in [@from, @to], a gap request's; @cb
 * runs under a shard lock (it must not call back in).  Returns how many */
unsigned int clretain_space_each(uint64_t oid, uint32_t incarn, uint64_t from,
		uint64_t to, unsigned int max,
		void (*cb)(const char *col, size_t cn, const char *key, size_t kn,
			uint64_t ver, uint64_t seq, void *ctx), void *ctx);

/* this node's own space (oid, incarn): its seqs a newer numbered delete takes over are remembered
 * (the last CLRETAIN_SUP_MAX of them) - not held, and not lost either */
#define CLRETAIN_SUP_MAX 131072u
void clretain_own_space(uint64_t oid, uint32_t incarn);
/* 1 when every seq in [@lo, @hi] of the own space was taken over so (at most 4096 of them) */
int clretain_superseded(uint64_t oid, uint32_t incarn, uint64_t lo, uint64_t hi);

/* per space, the highest seq no longer held (forgotten, or dropped by the time rule or the cap):
 * a member back with its marks below it lacks what nobody can replay */
int clretain_gone_each(int (*cb)(uint64_t oid, uint32_t incarn,
		uint64_t gone_hi, void *ctx), void *ctx);

/* the expiries noted more than @hold_s ago go - each member reaps
 * its own copy by its TTL; called only while the fleet confirms */
void clretain_expire_expiries(unsigned int now_s, unsigned int hold_s);

/* the retained version for the key, 0 when none (or older than retain_s) */
uint64_t clretain_get(const char *col, size_t cn, const char *key, size_t kn,
		unsigned int now_s);

/* @cb, under a shard lock (no calling back in), for each tombstone planted in (@since_s, @upto_s]:
 * @expiry 0 a delete, 1 an expiry, 2 a kept one; @seq 0 unnumbered.  Returns how many */
unsigned int clretain_each(unsigned int since_s, unsigned int upto_s,
		void (*cb)(const char *col, size_t cn, const char *key, size_t kn,
			uint64_t ver, int expiry, uint64_t oid, uint32_t incarn,
			uint64_t seq, void *ctx), void *ctx);

/* drop what is older than retain_s (1 Hz) */
void clretain_expire(unsigned int now_s);

void clretain_get_stats(struct clretain_stats *out, unsigned int now_s);
unsigned long long clretain_dropped_early(void);

/* every delete planted AFTER the returned second is still held (UINT_MAX when off); those from
 * before this process started are the caller's to bound */
unsigned int clretain_complete_since(unsigned int now_s);
/* 1 when an entry noted at or after @since_s may be gone other than by the marks: the cap or
 * memory took one, or the time rule an unnumbered one (expiries members reap do not count) */
int clretain_lost_since(unsigned int since_s);
unsigned int clretain_retain_s(void);    /* as configured */

#endif
