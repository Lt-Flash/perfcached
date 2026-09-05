/* PROVENANCE: vendored from the OpenSIPS cachedb_perf module
 * upstream: /dn/wt-pullshare-hg
 * branch: scratch/pullshare-t1
 * commit: 17bc82132e971cfd03ea0d9e1c150a83c5b42d59
 * path: modules/cachedb_perf/pcache_htable.h   synced: 2026-08-24
 * Local modifications ARE expected (compat-shim rewiring).
 * Check upstream drift with tools/sync-core.sh status|diff. */
/*
 * cachedb_perf - high-performance local memory cache
 *
 * Copyright (C) 2026 Yury Kirsanov
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef PCACHE_HTABLE_H
#define PCACHE_HTABLE_H

#include <stdint.h>
#include <stddef.h>

#include "../compat/str.h"
#include "../compat/locking.h"

#define PCACHE_SLOTS        6
#define PCACHE_SEG_BITS     12
#define PCACHE_SEG_SIZE     (1U << PCACHE_SEG_BITS)          /* 4096 buckets */
#define PCACHE_NSEGS        (1U << (24 - PCACHE_SEG_BITS))   /* for 2^24 max */
#define PCACHE_SEQ_RETRIES  64
#define PCACHE_OVF_BUCKETS  1024
/* per-process stat shards: sized to a fixed cap, not counted_max_processes
 * (not yet final when the table is built in mod_init, pre-fork).  The
 * owner:12 bucket field already caps the system at 4096 processes. */
#define PCACHE_MAX_PROCS    1024

/*
 * The record (DESIGN 3.3).  Byte 0 is the arena class id, stamped by the
 * arena and read-only here (pcache_arena.h).  vlen and expires are
 * naturally aligned so their loads/stores are single-copy-atomic: expires
 * is the versionless-TTL-bump target (DESIGN 2.7) and vlen may be read by
 * an optimistic reader mid-update.  While a cell sits on a free list the
 * link overlays bytes 8-15 (expires/hash) - never klen/vlen, so even a
 * freed cell keeps a bounded length at the vlen offset.
 */
typedef struct pcache_rec {
	unsigned char         cls;      /* arena class - read-only */
	unsigned char         rflags;   /* PCACHE_F_INT etc. (CP-04) */
	unsigned short        klen;
	unsigned int          vlen;
	volatile unsigned int expires;  /* absolute ticks, 0 = never */
	unsigned int          hash;     /* full hash: split relink + fast reject */
	unsigned long long    ver;      /* Lamport version (perfcached
	                                 * addition): which of two copies of
	                                 * this key is newer, comparably
	                                 * ACROSS nodes and without any clock
	                                 * agreement.  wtick cannot serve - it
	                                 * is seconds since THIS process
	                                 * started, so it means nothing on
	                                 * another node and restarts at 0.
	                                 *
	                                 * 64 bits because it counts WRITES,
	                                 * not seconds: at the measured 1.9M
	                                 * SET/s a 32-bit counter wraps in 38
	                                 * minutes, and on wrap every new
	                                 * write would compare OLDER than
	                                 * everything already stored.  At 8
	                                 * bytes it sits at offset 16, which
	                                 * is a real 8-alignment because cells
	                                 * are multiples of 32. */
	unsigned int          wtick;    /* write tick (perfcached addition):
	                                 * stamped at creation/overwrite, the
	                                 * rebalancer's coldest-first key -
	                                 * OUTSIDE the free-list link overlay
	                                 * (bytes 8-15), advisory only */
	char                  data[];   /* key, then value, contiguous */
} pcache_rec_t;

/* 28 since the 8-byte Lamport `ver` was added (was 20).  The free-list
 * link overlays bytes 8-15 only, so ver (16) and wtick (24) both sit
 * clear of it; the static assert below enforces that this constant, the
 * struct and ver's 8-alignment never drift apart. */
#define PCACHE_REC_HDR              28

/* the key hash (MurmurHash3 x86_32), local to this node */
unsigned int pcache_key_hash(const str *key);
#define PCACHE_REC_SIZE(_kl, _vl)   (PCACHE_REC_HDR + (_kl) + (_vl))

/* rflags: native int64 counter (CP-04) - the value payload is 8 raw
 * bytes, arithmetic is fixed-width under the bucket lock, and every
 * user-facing read (fetch, walker) formats it as a decimal string */
#define PCACHE_F_INT                0x01
/* The record arrived through a cluster pull, not through a local consumer
 * write - a passive copy.  Provenance doubles as authority: the serve side
 * can answer "held, not authoritative" for these instead of shipping the
 * value, so in a converged cluster only the writer answers with bytes.
 * A later local write over the key clears it (the writer IS the authority);
 * a pull landing on identical bytes keeps whatever the record had, which
 * keeps an owner's copy authoritative. */
#define PCACHE_F_PASSIVE            0x02

/* strict bounded decimal parse; no overflow guard - counter territory */
static inline int pcache_str2ll(const char *p, int len, long long *out)
{
	long long v = 0;
	int i = 0, neg = 0;

	if (len <= 0)
		return -1;
	if (p[0] == '-' || p[0] == '+') {
		neg = p[0] == '-';
		if (++i == len)
			return -1;
	}
	for (; i < len; i++) {
		if (p[i] < '0' || p[i] > '9')
			return -1;
		v = v * 10 + (p[i] - '0');
	}
	*out = neg ? -v : v;
	return 0;
}

_Static_assert(offsetof(pcache_rec_t, vlen) == 4 &&
               offsetof(pcache_rec_t, expires) == 8 &&
               offsetof(pcache_rec_t, ver) % 8 == 0 &&
               offsetof(pcache_rec_t, data) == PCACHE_REC_HDR,
               "pcache_rec field alignment broken");

/*
 * The bucket (DESIGN 3.1): exactly one cache line.  meta packs
 * used:4 (low bits) | owner:12 (process_no+1 of the lock holder, 0 =
 * none) - the owner exists so the maintenance worker can detect a dead
 * holder (3.5b).  tags[] plus meta form one aligned 8-byte word at offset
 * 8, which the SWAR tag scan loads whole.
 */
typedef struct pcache_bucket {
	volatile unsigned int   version;  /* seqlock: odd = writer inside */
	gen_lock_t              lock;     /* writers (+ reader fallback) */
	unsigned char           tags[PCACHE_SLOTS];  /* hash>>24, never 0 */
	volatile unsigned short meta;     /* used:4 | owner:12 */
	pcache_rec_t           *slot[PCACHE_SLOTS];
} __attribute__((aligned(64))) pcache_bucket_t;

_Static_assert(sizeof(pcache_bucket_t) == 64,
	"cachedb_perf requires a 4-byte lock backend (futex/fastlock): "
	"gen_lock_t made pcache_bucket exceed one cache line");
_Static_assert(offsetof(pcache_bucket_t, tags) == 8,
	"tags+meta must form the aligned 8-byte word at offset 8");

struct povf;

/*
 * Per-process op counters (CP-06): one cache line per process per table,
 * plain increments on the owner's own line, summed only at read time.
 * NEVER update_stat() per operation - that is one shared atomic line,
 * the measured 0.72x collapse (DESIGN 2.5) installed by observability.
 */
typedef struct pcache_pstat {
	unsigned long hits, misses, stores, removes,
	              created, destroyed, expired, retries, fallbacks,
	/*
	 * Stores made with expires == 0, i.e. "never expires".  A COUNT OF
	 * STORE OPERATIONS, not a population: an immortal entry can still be
	 * dropped by an explicit remove (which lands in `removes`, never in
	 * `expired`), so this must not be read as "immortals currently live".
	 * It exists so `expired` can be measured against the stores that were
	 * ever ELIGIBLE to expire - dividing by every store understates the
	 * share on a collection that mixes the two.
	 * Inexact under overwrite, exactly as `stores` already is: re-storing
	 * a key flips its eligibility without unwinding the earlier count.
	 * Free to carry - the struct is 128 bytes with or without it.
	 */
	              stores_immortal;
} __attribute__((aligned(64))) pcache_pstat_t;

typedef struct pcache_ht_totals {
	unsigned long hits, misses, stores, removes,
	              created, destroyed, expired, retries, fallbacks, entries,
	              stores_immortal;
} pcache_ht_totals_t;

typedef struct pcache_htable {
	/* the 3.4 routing word: (level << 32) | split, published whole.
	 * On its own line - everything else here mutates */
	/* (level << 32) | split - genuinely 64 bits, so NOT unsigned long:
	 * that is 32 bits on every ILP32 target (arm32, i386) and the packing
	 * would collapse silently. */
	volatile uint64_t       route;
	char                    _pad0[56];

	unsigned int            nbuckets;
	volatile unsigned int   ovf_count;   /* readers' overflow gate */
	gen_lock_t              ovf_lock;
	struct povf           **ovf_tab;     /* PCACHE_OVF_BUCKETS heads */

	pcache_bucket_t        *seg[PCACHE_NSEGS];

	/* per-bucket min-expires hints (CP-05), parallel to seg[]: the 64B
	 * bucket is full, and a separate array sweeps better anyway - 16
	 * hints per cache line, no bucket touched unless due.  Written under
	 * the bucket lock, only when a LOWER expiry arrives (a TTL bump only
	 * raises, so the hot bump path never writes here); a stale-low hint
	 * just costs one wasted bucket visit.  0 = nothing expiring */
	unsigned int           *hint_seg[PCACHE_NSEGS];

	/* CP-06 counters, indexed by process_no */
	pcache_pstat_t         *pstats;
	unsigned int            pstats_n;

	/* Baseline for perf_stats_reset: the shard sums as of the last reset.
	 * The counters themselves are never rewound - the hot paths own their
	 * own cache lines and must not be written from another process - so a
	 * reset just records where to count from, and pcache_ht_totals()
	 * reports the difference.  'entries' is a live gauge computed from the
	 * raw created/destroyed, so it survives a reset untouched. */
	pcache_ht_totals_t      base;
} pcache_htable_t;

/* sum the per-process shards, less the reset baseline; entries is absolute */
void pcache_ht_totals(pcache_htable_t *ht, pcache_ht_totals_t *out);

/* re-baseline the cumulative counters: everything perf_stats reports as a
 * running total starts from zero again.  Live gauges (entries, buckets,
 * overflow, arena) are unaffected. */
void pcache_ht_stats_reset(pcache_htable_t *ht);

/* current live bucket count (grows at runtime, CP-09) - for the CP-11
 * growth event, which reports the before/after span */
unsigned int pcache_ht_nbuckets(pcache_htable_t *ht);

pcache_htable_t *pcache_htable_new(unsigned int size_log2);

/* 0 = stored; -1 = error; -2 = out of memory (the arena could not allocate
 * a cell - the cache is full and the write was dropped).  @expires is
 * absolute ticks, 0 = never */
int pcache_ht_store(pcache_htable_t *ht, const str *key, const str *val,
		unsigned int expires);

/* as pcache_ht_store, stamping @rflags on the record (PCACHE_F_PASSIVE for
 * a value that arrived through a cluster pull).  pcache_ht_store() is this
 * with rflags 0 - a local consumer write, the authoritative kind. */
int pcache_ht_store_ex(pcache_htable_t *ht, const str *key, const str *val,
		unsigned int expires, unsigned char rflags);

/* A copy that arrived from a peer, carrying the version the SENDER's
 * table committed (task A2).  The record is stored with that version
 * rather than a fresh local tick - a version only means something across
 * nodes if it travels with the bytes - and a copy we already hold at an
 * equal or higher version is refused.
 *
 * The comparison happens under the bucket lock, beside the install,
 * because a probe-then-store in the caller leaves a gap a local write
 * can land in: the write would then be silently clobbered by the older
 * copy it had just beaten.
 *
 * 0 = stored; PCACHE_E_OLDER = refused, ours is newer or the same age;
 * -1 = error; -2 = arena full. */
#define PCACHE_E_OLDER      (-4)

int pcache_ht_store_ver(pcache_htable_t *ht, const str *key, const str *val,
		unsigned int expires, unsigned char rflags,
		unsigned long long ver);

/* get_buf(): @buf was too small.  *vlen stays 0 and *needed carries the
 * size the value would have needed - never a length the caller could
 * mistake for "bytes written into buf". */
#define PCACHE_E_TOOSMALL   (-3)

/* Smallest buffer get_buf() will accept.  A native counter is 8 raw bytes
 * formatted as decimal on read, so anything shorter could not represent
 * every legal hit; enforced rather than assumed. */
#define PCACHE_GETBUF_MIN   24

/*
 * Allocation-free read into a caller-owned buffer: the value is copied
 * once, straight from the record to @buf, instead of being copied to the
 * internal scratch and then into a freshly pkg_malloc'd str.
 *
 * @buf MUST be private to the calling process (its own stack or pkg).  The
 * lock-free read path writes into it SPECULATIVELY - a retried optimistic
 * section may leave a partial value behind - so on any return other than 0
 * the contents are undefined and must not be used.  The value is not
 * NUL-terminated.
 *
 * 0 = hit, *vlen bytes written (always <= @buflen); -2 = miss or expired;
 * -1 = error or malformed request; PCACHE_E_TOOSMALL = value does not fit,
 * *needed holds the required size.  *vlen and *needed are zeroed first.
 * @needed may be NULL.
 */
/* Existence probe: is @key present and live, how long is its value and
 * when does it expire - without copying the value anywhere.  Shares the
 * whole read path with the fetches, stopping before the copy-out, so it
 * can never disagree with a read about whether a key is there.
 *
 * Cheaper than any fetch by construction: no pkg_malloc (the ~70 ns the
 * profile attributes to the allocator), no copy, and the record's payload
 * is never touched - a miss is usually settled on the bucket's tag word
 * alone.  Intended for answering "do I have this key?" - a cross-node
 * lookup asking peers, or a script/MI existence test.
 *
 * @vlen, @expires and @is_counter are optional; @expires is absolute ticks
 * (0 = never); @is_counter reports a native counter, whose value is a
 * per-node quantity rather than a portable one.
 * @return 0 = present and live, -2 = absent or expired, -1 = bad args. */
int pcache_ht_probe(pcache_htable_t *ht, const str *key, unsigned int *vlen,
		unsigned int *expires, int *is_counter);

int pcache_ht_fetch_buf(pcache_htable_t *ht, const str *key, char *buf,
		unsigned int buflen, unsigned int *vlen, unsigned int *needed);

/* as pcache_ht_fetch_buf, plus *@expires - the read equivalent of
 * pcache_ht_fetch_ex, and the reason the verb layer can serve a GET
 * without allocating: the value is already copied out into the
 * caller's buffer, so the malloc+memcpy+free that fetch_ex adds on top
 * is pure overhead on the hottest path there is. */
int pcache_ht_fetch_buf_ex(pcache_htable_t *ht, const str *key, char *buf,
		unsigned int buflen, unsigned int *vlen, unsigned int *needed,
		unsigned int *expires);

/* 0 = hit (val->s pkg-allocated, caller frees); -2 = miss or expired;
 * -1 = error */
int pcache_ht_fetch(pcache_htable_t *ht, const str *key, str *val);

/* as pcache_ht_fetch, plus *@expires = the record's absolute expiry (0 =
 * never) on a hit - the MI perf_get reports the TTL with the value - and
 * *@rflags = the record's flags (either out pointer may be NULL) */
int pcache_ht_fetch_ex(pcache_htable_t *ht, const str *key, str *val,
		unsigned int *expires, unsigned char *rflags);

/* as pcache_ht_fetch_ex, but reporting the record's version instead of
 * its flags (A2).  A separate entry point rather than a second call
 * after a fetch: value and version must come out of the SAME read, or a
 * write landing between them would pair old bytes with a new version -
 * and a receiver told that pairing would then refuse the real update as
 * older.  Either out pointer may be NULL. */
int pcache_ht_fetch_ver(pcache_htable_t *ht, const str *key, str *val,
		unsigned int *expires, unsigned long long *ver);
/* as pcache_ht_fetch_ver, plus *@rflags: value, expiry, flags and version
 * out of the SAME read (perfcached S73).  A write path that re-sends a
 * record it just touched must know both that it AUTHORED it and which
 * version it is sending, and pairing them across two reads would let a
 * write land between.  Any out pointer may be NULL. */
int pcache_ht_fetch_full(pcache_htable_t *ht, const str *key, str *val,
		unsigned int *expires, unsigned char *rflags,
		unsigned long long *ver);

/*
 * The version of the record this thread last wrote (perfcached A1).
 *
 * The WAL has to log the version the table actually committed, and the
 * committing store is not the caller's to inspect: pcache_ht_store_ex
 * ticks the clock on a prebuilt record, then MAY discard that record and
 * tick again for an in-place update, so the version that survives is
 * only known inside the bucket lock.  Rather than widen the store and
 * add signatures for every caller that does not care - upstream
 * cachedb_perf calls all of them - each ticking path leaves its result
 * here, and the two callers that log to the WAL pass it explicitly:
 *
 *      pcache_ht_store(ht, &k, &v, exp);
 *      pc_wal_upsert(col, k.s, k.len, v.s, v.len, exp, pcache_last_ver);
 *
 * Read it in the SAME thread, immediately after the store that set it,
 * and pass it at the call site rather than reaching for it deeper down -
 * the coupling is only safe while it stays visible.  Zero means no write
 * has committed on this thread yet.
 */
extern __thread unsigned long long pcache_last_ver;

/* The record's version without copying its value (A1's read surface, and
 * what a cross-node comparison will need).  Kept separate from probe()
 * rather than added to it: probe has a dozen callers in two trees and
 * none of the others want this.
 * 0 = present, *ver written; -2 = absent or expired; -1 = bad args. */
int pcache_ht_getver(pcache_htable_t *ht, const str *key,
		unsigned long long *ver);

/* Restore a record's version to one read back off the WAL or a snapshot
 * (A1's replay).  A store always stamps a FRESH tick - correct for a
 * write, wrong for a record being put back exactly as it was - so replay
 * stores and then re-stamps.  Boot-time only: it takes the bucket lock
 * per record, and lying about a live record's version would make a newer
 * copy look older to every peer that asks.
 * 1 = re-stamped; 0 = absent; -1 = bad args. */
int pcache_ht_setver(pcache_htable_t *ht, const str *key,
		unsigned long long ver);

/* 1 = removed; 0 = was absent; -1 = error */
int pcache_ht_remove(pcache_htable_t *ht, const str *key);

/* re-arm an existing key's TTL without rewriting the value (MI perf_ttl):
 * one aligned store of expires under the bucket lock, the versionless bump
 * of 2.7.  @expires is absolute ticks (0 = never).  1 = re-armed, 0 = absent */
int pcache_ht_touch(pcache_htable_t *ht, const str *key, unsigned int expires);

/* atomic counter add (CP-04): creates a native counter on an absent key,
 * accumulates fixed-width on an existing one, converts a numeric string
 * record on first touch.  0 = ok (*new_val = the result); -1 = error or
 * the existing value is not an integer.  @expires re-arms the TTL,
 * absolute ticks, 0 = never */
int pcache_ht_add(pcache_htable_t *ht, const str *key, long long delta,
		unsigned int expires, long long *new_val);

/* @expires == PCACHE_EXP_PRESERVE keeps an existing record's expiry
 * untouched (the Redis INCR contract - task S29); a CREATED counter is
 * then immortal.  *@eff_expires (may be NULL) returns the record's
 * effective absolute expiry after the op - the WAL logs THAT, keeping
 * replay idempotent under preserve. */
#define PCACHE_EXP_PRESERVE 0xFFFFFFFFu
int pcache_ht_add_ex(pcache_htable_t *ht, const str *key, long long delta,
		unsigned int expires, long long *new_val,
		unsigned int *eff_expires);

/*
 * Key/value walker: per-slot optimistic snapshots over every bucket, then
 * the overflow chains under the overflow lock.  @key/@val given to the
 * callback are stable NUL-terminated copies in walker-owned buffers,
 * valid only for the duration of the call; @expires is raw (0 = never) -
 * filtering is the callback's choice.  Return <0 from the callback to
 * stop the walk (returned through).
 *
 * Guarantees are the Redis SCAN class: an entry mutated concurrently may
 * be seen once, twice or not at all.  The overflow leg runs under the
 * overflow lock, so the callback must not re-enter this cache.
 */
typedef int (*pcache_iter_cb)(const str *key, const str *val,
		unsigned int expires, void *ctx);

/* the metadata walk variant: adds each record's write tick (advisory
 * coldness) - the rebalancer's victim-selection feed - and its version.
 * The version rides HERE rather than on the plain walk because a walk
 * callback may hold the overflow lock and so cannot ask the table for it
 * afterwards; the snapshot has to carry everything the caller needs. */
typedef int (*pcache_iter_meta_cb)(const str *key, const str *val,
		unsigned int expires, unsigned int wtick,
		unsigned char rflags, unsigned long long ver, void *ctx);
/* PERFCACHED LOCAL MOD: resumable metadata walk.  *@cursor in = the
 * bucket to start at, out = where to resume, or 0 when a full cycle
 * (all buckets plus the overflow leg) finished.  A budgeted repeating
 * sweep MUST use this: the plain walk restarts at bucket 0, so a budget
 * cut means the tail is never reached. */
int pcache_ht_iter_meta_from(pcache_htable_t *ht, unsigned int *cursor,
		pcache_iter_meta_cb cb, void *ctx);

int pcache_ht_iter_meta(pcache_htable_t *ht, pcache_iter_meta_cb cb,
		void *ctx);
int pcache_ht_iter(pcache_htable_t *ht, pcache_iter_cb cb, void *ctx);

/* default buckets visited per perf_scan call when count is unset */
#define PCACHE_SCAN_BUCKETS 100

/*
 * Cursored, bounded walk (MI perf_scan, Redis SCAN semantics).  Starts at
 * bucket *@cursor, visits up to @max_buckets buckets calling @cb per live
 * entry, and updates *@cursor to the bucket to resume from - 0 once the walk
 * (overflow leg included) is complete.  Pass *@cursor = 0 to begin.  The
 * ascending cursor is stable across a concurrent resize (3.4 / 5.2) and gives
 * the >=-once guarantee; it advances a whole bucket at a time.  0, or <0 on
 * error / callback stop.
 */
int pcache_ht_scan(pcache_htable_t *ht, unsigned int *cursor,
		unsigned int max_buckets, pcache_iter_cb cb, void *ctx);

/* S40: the same cursor walk with flags.  PCACHE_SCAN_NOVAL skips the
 * value copy-out - keys-only enumeration (KEYS/SCAN) was paying a
 * memcpy AND the value's cold cachelines for bytes it never read; the
 * callback then receives val = {"", 0}.  pcache_ht_scan() is this with
 * flags 0, unchanged. */
#define PCACHE_SCAN_NOVAL  0x1
int pcache_ht_scan_ex(pcache_htable_t *ht, unsigned int *cursor,
		unsigned int max_buckets, unsigned int flags,
		pcache_iter_cb cb, void *ctx);

/* CP-11: invoked for each record the sweep reaps, after the bucket lock is
 * released and while the key is still valid, so the caller can raise an
 * expiry event.  @key points into the about-to-be-freed record; do not
 * retain it past the call. */
typedef void (*pcache_expired_cb)(const str *key, void *ctx);

/* expiry sweep (CP-05): visits only buckets whose hint is due, removes
 * expired records (overflow chains too whenever any overflow exists) and
 * reclaims their cells through the global pool - the sweeping process is
 * not an allocator, so private-stack frees would never drain.  If @cb is
 * non-NULL it is called once per reaped record (CP-11).  Returns the number
 * of records reclaimed. */
unsigned int pcache_ht_sweep(pcache_htable_t *ht, unsigned int now,
		pcache_expired_cb cb, void *cb_ctx);

/* linear-hash growth (CP-09): split buckets while entries/nbuckets exceeds
 * @target_lf, up to @budget splits.  Single-splitter (maintenance timer). */
unsigned int pcache_ht_grow(pcache_htable_t *ht, unsigned int target_lf,
		unsigned int budget);

/* modparam-triggered startup selftest; -1 on any mismatch */
int pcache_htable_selftest(void);

#endif /* _PCACHE_HTABLE_H_ */
