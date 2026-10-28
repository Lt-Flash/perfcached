/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * recover.c — startup recovery (task S15).  See recover.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "compat/dprint.h"
#include "compat/timer.h"
#include "core/pcache_htable.h"
#include "store.h"
#include "colname.h"
#include "wal.h"
#include "rdb.h"
#include "recover.h"
#include "ackstate.h"

/* wall-clock seconds -> absolute ticks; 1 = already expired, 0 = never */
static unsigned int wall_to_ticks(unsigned long long wall, time_t now_wall)
{
	if (!wall)
		return 0;
	if ((time_t)wall <= now_wall)
		return 1;                      /* expired marker (never-0) */
	return get_ticks() + (unsigned int)((time_t)wall - now_wall);
}

/* ---- RDB load ---------------------------------------------------------- */

static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char *p)
{
	return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* what the last recovery restored - see pc_recovered_records() */
static long recovered_n;

static long recovered_wal;

/* S392: the wall-clock second the loaded snapshot began, 0 = none loaded */
static unsigned long long recovered_snap_wall;

unsigned long long pc_recovered_snapshot_wall(void)
{
	return recovered_snap_wall;
}

long pc_recovered_wal_applied(void)
{
	return recovered_wal;
}

long pc_recovered_records(void)
{
	return recovered_n;
}

/* S223: when this node's data STOPPED being current - the newest write
 * in its storage directory, wall-clock ms, 0 when nothing was found.
 * The kernel updates a file's mtime on every write(), so the newest one
 * is the last thing this node wrote before it went down.
 *
 * It errs EARLY, never late: a node that sat idle before dying reports
 * its last write rather than its death, which makes its outage look
 * longer, which finds FEWER peers that were watching throughout, which
 * makes the reconcile keep rather than delete.  That is the direction
 * the whole task exists to choose. */
static long long last_write_ms;
static int last_write_noted;       /* S455: sampled before the dir lock */

long long pc_recovered_last_write_ms(void)
{
	return last_write_ms;
}

static void note_last_write(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	char path[1024];
	struct stat sb;

	last_write_ms = 0;
	if (!d)
		return;
	while ((de = readdir(d))) {
		long long ms;

		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
		if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode))
			continue;
		ms = (long long)sb.st_mtim.tv_sec * 1000 +
			sb.st_mtim.tv_nsec / 1000000;
		if (ms > last_write_ms)
			last_write_ms = ms;
	}
	closedir(d);
}

/* S455: called before S290's dir lock, which rewrites perfcached.lock here */
void pc_recover_note_last_write(const char *dir)
{
	last_write_ms = 0;
	last_write_noted = 1;
	if (dir && *dir)
		note_last_write(dir);
}

static int rdb_load(const char *dir, struct pc_recover_stats *st)
{
	char path[512];
	unsigned char *data;
	struct stat fst;
	pcache_htable_t *ht = NULL;
	int cache = 0;                     /* G5: ht's collection is durable = no */
	time_t now_wall = time(NULL);
	size_t off = 24, n;
	uint32_t fmt;
	int fd;

	snprintf(path, sizeof path, "%s/" PC_RDB_FILE, dir);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 1;                      /* no snapshot: normal first boot */
	if (fstat(fd, &fst) != 0 || fst.st_size < 28) {
		close(fd);
		return -1;
	}
	n = (size_t)fst.st_size;
	data = malloc(n);
	if (!data || read(fd, data, n) != (ssize_t)n) {
		free(data);
		close(fd);
		return -1;
	}
	close(fd);

	fmt = rd32(data + 4);
	/* UP3: a format NEWER than this build reads is not corruption - it is
	 * a downgrade, and starting from the WAL alone would lose every
	 * replica the snapshot holds.  Said apart, and refused (pc_recover) */
	if (rd32(data) == PC_RDB_MAGIC && fmt > PC_RDB_FMT) {
		st->rdb_fmt_newer = fmt;
		free(data);
		return -2;
	}
	if (rd32(data) != PC_RDB_MAGIC || fmt < 1 ||
	        pc_crc32c(0, data, n - 4) != rd32(data + n - 4)) {
		free(data);
		return -1;
	}
	st->marker = rd64(data + 8);
	st->rdb_wall = rd64(data + 16);    /* S392: stamped when the save began */

	while (off < n - 4) {
		unsigned char tag = data[off++];

		if (tag == 0xFF)
			break;
		if (tag == 1) {
			unsigned char cl = data[off];

			ht = pc_store_find((const char *)data + off + 1, cl);
			if (!ht)
				st->rdb_skipped_nocol++;
			/* G5: a cache collection starts empty - a snapshot taken
			 * before it was made one is not loaded into it */
			cache = ht && !pc_store_durable(ht);
			if (cache)
				ht = NULL;
			off += 1u + cl;
		} else if (tag == 2) {
			uint32_t kl = rd32(data + off), vl = rd32(data + off + 4);
			unsigned long long wall = rd64(data + off + 8);
			/* format 1 predates the field: those records come back
			 * unnumbered rather than being refused */
			unsigned long long ver = fmt >= 2 ? rd64(data + off + 16) : 0;
			/* S280: the record's type */
			unsigned char fl = fmt >= 3 ? (data[off + 24] & PCACHE_F_CARRY) : 0;
			str k, v;
			unsigned int exp;

			off += PC_RDB_RHDR(fmt);
			k.s = (char *)data + off;
			k.len = (int)kl;
			v.s = (char *)data + off + kl;
			v.len = (int)vl;
			off += kl + vl;
			if (!ht) {
				if (cache)
					st->rdb_skipped_cache++;   /* G5 */
				continue;              /* its collection went away */
			}
			exp = wall_to_ticks(wall, now_wall);
			if (exp == 1) {
				st->rdb_skipped_expired++;
				continue;
			}
			if (pcache_ht_store_ex(ht, &k, &v, exp, fl) == 0) {
				/* the store stamped a fresh tick; put the
				 * record's own version back over it */
				pcache_ht_setver(ht, &k, ver);
				if (ver > st->ver_high)
					st->ver_high = ver;
				st->rdb_records++;
			}
		} else {
			free(data);
			return -1;
		}
	}
	free(data);
	return 0;
}

/* ---- WAL replay -------------------------------------------------------- */

/*
 * S240: REPLAY BY VERSION, NOT BY FILE ORDER.
 *
 * Each worker has its own ring and the WAL thread drains them in turn,
 * sorting only within one pass, so two workers' records for ONE key can
 * sit in the file in the opposite order to their versions (wal_late
 * counts those passes).  Applied in file order, the later of the two in
 * the FILE won: walordertest measured 10,496-18,272 of 20,000 raced keys
 * restored with an older value than they were serving at kill -9.  Since
 * S234 every record's version is stamped under the key's lock and rises
 * per key, so: an upsert or touch lands only over an OLDER version, a
 * delete removes only an older record - and remembers its version here,
 * so an older upsert later in the file cannot resurrect the key.
 * Unversioned records (pre-A1 upserts, pre-S240 deletes) replay in file
 * order as before.  The table lives for one replay.
 */
#define TOMB_BUCKETS 65536u
struct tomb {
	struct tomb *next;
	unsigned long long ver;
	int len;                           /* col '\0' key */
	char k[];
};
static struct tomb **tombs;

static unsigned int tomb_hash(const char *s, int n)
{
	unsigned int h = 2166136261u;
	int i;

	for (i = 0; i < n; i++)
		h = (h ^ (unsigned char)s[i]) * 16777619u;
	return h & (TOMB_BUCKETS - 1);
}

/* the tombstone key: col, a NUL, the key - into @out (>= collen+klen+1) */
static int tomb_key(char *out, const char *col, int collen, const char *key,
		int klen)
{
	memcpy(out, col, (size_t)collen);
	out[collen] = 0;
	memcpy(out + collen + 1, key, (size_t)klen);
	return collen + 1 + klen;
}

static unsigned long long tomb_get(const char *k, int n)
{
	struct tomb *t;

	if (!tombs)
		return 0;
	for (t = tombs[tomb_hash(k, n)]; t; t = t->next)
		if (t->len == n && !memcmp(t->k, k, (size_t)n))
			return t->ver;
	return 0;
}

static void tomb_note(const char *k, int n, unsigned long long ver)
{
	struct tomb *t;
	unsigned int h;

	if (!tombs && !(tombs = calloc(TOMB_BUCKETS, sizeof *tombs)))
		return;                        /* no memory: file order it is */
	h = tomb_hash(k, n);
	for (t = tombs[h]; t; t = t->next)
		if (t->len == n && !memcmp(t->k, k, (size_t)n)) {
			if (ver > t->ver)
				t->ver = ver;
			return;
		}
	t = malloc(sizeof *t + (size_t)n);
	if (!t)
		return;
	t->ver = ver;
	t->len = n;
	memcpy(t->k, k, (size_t)n);
	t->next = tombs[h];
	tombs[h] = t;
}

/* the tombstone key's buffer, grown to fit: a fixed 512 bytes skipped
 * the delete memory for any key past ~510 bytes, so an older write later
 * in the file resurrected it (the store takes keys to 64 KB) */
static char *tk_buf;
static size_t tk_cap;

static char *tk_get(size_t need)
{
	if (need > tk_cap) {
		char *n = realloc(tk_buf, need);

		if (!n)
			return NULL;
		tk_buf = n;
		tk_cap = need;
	}
	return tk_buf;
}

static void tomb_free(void)
{
	unsigned int i;
	struct tomb *t, *n;

	if (!tombs)
		return;
	for (i = 0; i < TOMB_BUCKETS; i++)
		for (t = tombs[i]; t; t = n) {
			n = t->next;
			free(t);
		}
	free(tombs);
	tombs = NULL;
	free(tk_buf);
	tk_buf = NULL;
	tk_cap = 0;
}

struct replay_ctx {
	struct pc_recover_stats *st;
	time_t now_wall;
	long unk_pending;       /* UP3: unknown records not yet bounded */
};

static int replay_cb(int type, const char *col, int collen, const char *key,
		int klen, const char *val, int vlen, unsigned long long wall,
		unsigned long long seq, unsigned long long ver,
		unsigned char flags, void *p)
{
	struct replay_ctx *c = p;
	pcache_htable_t *ht;
	str k, v;
	unsigned int exp;
	unsigned long long tv = 0;          /* S240: a touch meets this */

	/* UP3: a newer build's record, unparsed - its seq unknown too.  The
	 * next known record bounds it: at or under the snapshot's marker,
	 * the snapshot (a format this build reads, or rdb_load refused it)
	 * already holds what it did.  Above, or with nothing after it, it
	 * is a write this start would drop - pc_recover refuses */
	if (!PC_WAL_T_KNOWN(type)) {
		if (!c->unk_pending++ && !c->st->wal_unknown &&
		        !c->st->wal_unknown_covered)
			c->st->wal_unknown_type = type;
		c->st->wal_skipped++;
		return 0;
	}
	if (c->unk_pending) {
		if (c->st->rdb_ok && seq <= c->st->marker)
			c->st->wal_unknown_covered += c->unk_pending;
		else
			c->st->wal_unknown += c->unk_pending;
		c->unk_pending = 0;
	}
	if (seq > c->st->last_seq)
		c->st->last_seq = seq;
	/* 12ny AT-3b: a numbered delete is counted wherever it lies and
	 * whatever its collection became - its record was logged after the
	 * delete was applied, so below the marker the snapshot holds it and
	 * above it this replay does.  On disk: durable, nothing to wait for */
	if (type == PC_WAL_T_DEL_N && vlen == PC_WAL_DELN_VLEN) {
		const unsigned char *u = (const unsigned char *)val;

		if (pc_ack_applied(rd64(u + 8), rd32(u + 16), rd64(u + 20), 0) ==
		        CLACK_NEW)
			c->st->ack_replayed++;
	}
	if (seq <= c->st->marker) {
		c->st->wal_below_marker++;     /* the snapshot already holds it */
		return 0;
	}
	/* S69: the collection set itself, replayed in order with the records
	 * - a create must exist before the records written into it, and a
	 * drop must undo a create that came earlier in the same window. */
	if (type == PC_WAL_T_COL_RENAME) {
		/* the collections file names this collection as the window
		 * STARTS, so the rename is applied here, between the records
		 * written under each name */
		if (vlen > 0 && pc_store_find(col, (size_t)collen))
			(void)pc_store_rename(col, (size_t)collen, val,
				(size_t)vlen, pc_lamport_now());
		return 0;
	}
	if (type == PC_WAL_T_COL_CREATE || type == PC_WAL_T_COL_DROP) {
		if (type == PC_WAL_T_COL_DROP) {
			if (pc_store_find(col, (size_t)collen))
				pc_store_drop(col, (size_t)collen);
			else
				pc_store_note_drop(col, (size_t)collen,
					pc_lamport_now());
		} else if (!pc_store_find(col, (size_t)collen) && vlen == 1) {
			if (!pc_col_name_held_ok(col, (size_t)collen)) {   /* S343 */
				char pb[4 * PC_COL_NAME_MAX + 1];

				LM_WARN("WAL replay: collection '%s' breaks the "
					"collection-name rule - kept as it is; rename it\n",
					pc_col_name_print(pb, sizeof pb, col, (size_t)collen));
			}
			(void)pc_store_create(col, (size_t)collen,
				(unsigned char)val[0]);
		}
		return 0;
	}
	ht = pc_store_find(col, (size_t)collen);
	if (!ht) {
		c->st->wal_skipped++;
		return 0;
	}
	/* G5: a cache collection starts empty - records logged before it was
	 * made one are not replayed into it */
	if (!pc_store_durable(ht)) {
		c->st->wal_skipped_cache++;
		return 0;
	}
	k.s = (char *)key;
	k.len = klen;
	exp = wall_to_ticks(wall, c->now_wall);

	switch (type) {
	case PC_WAL_T_UPSERT:              /* pre-A1: arrives with ver 0 */
	case PC_WAL_T_UPSERT_V:
		if (exp == 1) {
			c->st->wal_skipped++;      /* born dead: expired already */
			return 0;
		}
		v.s = (char *)val;
		v.len = vlen;
		if (ver) {                     /* S240: by version */
			char *tk = tk_get((size_t)collen + 1 + (size_t)klen);
			int rc;

			if (tk &&
			    tomb_get(tk, tomb_key(tk, col, collen, key, klen)) >= ver) {
				c->st->wal_older++;    /* deleted by a newer record */
				return 0;
			}
			/* S280: the record's type comes back with it */
			rc = pcache_ht_store_ver(ht, &k, &v, exp,
				(unsigned char)(flags & PCACHE_F_CARRY), ver);
			if (rc == PCACHE_E_OLDER) {
				c->st->wal_older++;    /* a newer one already here */
				return 0;
			}
			/* S244: no room for this newer record - the older copy
			 * an earlier record (or the snapshot) left must not be
			 * what the node restores */
			if (rc == -2)
				pcache_ht_drop_older(ht, &k, ver);
			if (rc == 0) {
				if (ver > c->st->ver_high)
					c->st->ver_high = ver;
				c->st->wal_applied++;
			}
			return 0;
		}
		if (pcache_ht_store_ex(ht, &k, &v, exp,
		        (unsigned char)(flags & PCACHE_F_CARRY)) == 0) {
			pcache_ht_setver(ht, &k, ver);
			if (ver > c->st->ver_high)
				c->st->ver_high = ver;
			c->st->wal_applied++;
		}
		return 0;
	case PC_WAL_T_DEL_N:               /* AT-3b: a DEL, numbered */
	case PC_WAL_T_DEL: {
		char *tk = tk_get((size_t)collen + 1 + (size_t)klen);
		unsigned long long have = 0;

		if (vlen != PC_WAL_DEL_VERLEN &&    /* pre-S240: file order */
		    !(type == PC_WAL_T_DEL_N && vlen == PC_WAL_DELN_VLEN)) {
			pcache_ht_remove(ht, &k);
			c->st->wal_applied++;
			return 0;
		}
		/* S240: the version rides as the value (wal.h) */
		ver = (unsigned long long)(unsigned char)val[0] |
			(unsigned long long)(unsigned char)val[1] << 8 |
			(unsigned long long)(unsigned char)val[2] << 16 |
			(unsigned long long)(unsigned char)val[3] << 24 |
			(unsigned long long)(unsigned char)val[4] << 32 |
			(unsigned long long)(unsigned char)val[5] << 40 |
			(unsigned long long)(unsigned char)val[6] << 48 |
			(unsigned long long)(unsigned char)val[7] << 56;

		if (tk)
			tomb_note(tk, tomb_key(tk, col, collen, key, klen), ver);
		if (pcache_ht_getver(ht, &k, &have) == 0 && have >= ver) {
			c->st->wal_older++;        /* written again after it */
			return 0;
		}
		pcache_ht_remove(ht, &k);
		if (ver > c->st->ver_high)
			c->st->ver_high = ver;
		c->st->wal_applied++;
		return 0;
	}
	case PC_WAL_T_TOUCH:               /* pre-A2: arrives with ver 0 */
	case PC_WAL_T_TOUCH_V:
		if (exp == 1 && ver) {
			/* re-armed into the past: a DELETE at this version, and
			 * treated as one - it removed an unconditionally, so an
			 * EXPIRE later in the file than a newer SET took the
			 * newer value with it, and it is remembered, so an
			 * older write after it cannot bring the key back */
			char *tk = tk_get((size_t)collen + 1 + (size_t)klen);
			unsigned long long have = 0;

			if (tk)
				tomb_note(tk, tomb_key(tk, col, collen, key, klen),
					ver);
			if (pcache_ht_getver(ht, &k, &have) == 0 && have >= ver) {
				c->st->wal_older++;
				return 0;
			}
			pcache_ht_remove(ht, &k);
		} else if (exp == 1) {
			pcache_ht_remove(ht, &k);  /* unversioned: file order */
		} else if (ver && pcache_ht_getver(ht, &k, &tv) == 0 &&
		           tv >= ver) {
			c->st->wal_older++;        /* S240: an older touch */
			return 0;
		} else if (pcache_ht_touch(ht, &k, exp) == 1 && ver) {
			/* the touch ticked a version of its own when it was
			 * logged; put that one back, not the fresh tick the
			 * replayed touch just made */
			pcache_ht_setver(ht, &k, ver);
			if (ver > c->st->ver_high)
				c->st->ver_high = ver;
		}
		c->st->wal_applied++;
		return 0;
	default:
		break;
	}
	c->st->wal_skipped++;
	return 0;
}

static int skip_unknown;                   /* UP3: [wal] unknown_records */

void pc_recover_set_skip_unknown(int on)
{
	skip_unknown = on ? 1 : 0;
}

/* ---- entry ------------------------------------------------------------- */

int pc_recover(const char *dir, struct pc_recover_stats *st)
{
	/* S223: BEFORE anything below opens a file for writing - the replay
	 * and the snapshot machinery both touch this directory, and after
	 * that the newest mtime would be "now" rather than "when I died" */
	if (!last_write_noted)             /* S455: the daemon sampled it first */
		note_last_write(dir);
	struct replay_ctx ctx;
	const char *why = "end";
	int rc;

	memset(st, 0, sizeof *st);
	rc = rdb_load(dir, st);
	if (rc == -2) {
		LM_CRIT("recover: the snapshot in %s is format %u - written by a "
			"NEWER build (this one reads formats 1 to %u).  Refusing to "
			"start: from the WAL alone this node would lose every replica "
			"the snapshot holds.  Run the newer build, or move the "
			"snapshot aside if losing them is meant\n", dir,
			st->rdb_fmt_newer, (unsigned)PC_RDB_FMT);
		return -1;
	}
	if (rc == 0) {
		st->rdb_ok = 1;
	} else if (rc < 0) {
		/* a cache starts cold rather than refusing: replay everything
		 * the WAL still holds (marker 0) and say so LOUDLY */
		LM_CRIT("recover: the snapshot in %s is missing pieces or "
			"corrupt - starting from the WAL alone\n", dir);
		st->marker = 0;
	}

	/* 12ny AT-3b: the marks that go with the snapshot just loaded, before
	 * the replay counts what came after it */
	if (st->rdb_ok) {
		st->ack_restored = pc_ack_load(dir, st->marker);
		if (st->ack_restored < 0)
			LM_WARN("recover: %s/" PC_ACK_FILE " is not a watermarks "
				"file this build reads, or is torn - the marks start "
				"from the WAL alone\n", dir);
	}

	ctx.st = st;
	ctx.now_wall = time(NULL);
	ctx.unk_pending = 0;
	pc_wal_scan(dir, replay_cb, &ctx, &why);
	tomb_free();                       /* S240: one replay's worth */
	st->wal_unknown += ctx.unk_pending;   /* UP3: the log's last word */
	if (st->wal_unknown_covered)
		LM_NOTICE("recover: %ld WAL record(s) of a type this build does "
			"not know (first: %d) lie under the snapshot's marker - the "
			"snapshot holds what they did\n", st->wal_unknown_covered,
			st->wal_unknown_type);
	if (st->wal_unknown) {
		if (!skip_unknown) {
			LM_CRIT("recover: the WAL in %s holds %ld record(s) of type "
				"%d, which this build does not know - written by a "
				"NEWER build.  Refusing to start rather than drop them: "
				"run the newer build, or set [wal] unknown_records = "
				"skip to start without them\n", dir, st->wal_unknown,
				st->wal_unknown_type);
			return -1;
		}
		LM_WARN("recover: %ld WAL record(s) of type %d, unknown to this "
			"build, were SKIPPED ([wal] unknown_records = skip)\n",
			st->wal_unknown, st->wal_unknown_type);
	}
	/* Resume the version clock above everything that came back.  Not
	 * observe(): that exists to refuse a peer claiming an absurd value,
	 * and at boot - clock 0 against a fleet long past a billion writes -
	 * it would refuse our own dataset and count it as a defect. */
	recovered_n = st->rdb_records + st->wal_applied;
	recovered_wal = st->wal_applied;              /* S352 */
	recovered_snap_wall = st->rdb_ok ? st->rdb_wall : 0;   /* S392 */
	pc_lamport_restore(st->ver_high);
	LM_NOTICE("recover: rdb %s (%ld records, %ld expired, %ld orphaned), "
		"wal replay %ld applied / %ld below marker / %ld skipped / "
		"%ld older than what they met (stop: %s), lamport resumed at "
		"%llu\n",
		st->rdb_ok ? "loaded" : "absent/cold",
		st->rdb_records, st->rdb_skipped_expired, st->rdb_skipped_nocol,
		st->wal_applied, st->wal_below_marker, st->wal_skipped,
		st->wal_older, why,
		pc_lamport_now());
	if (st->ack_restored > 0 || st->ack_replayed)
		LM_NOTICE("recover: watermarks - %d numbering space(s) restored "
			"from the snapshot's, %ld numbered delete(s) counted from "
			"the WAL\n", st->ack_restored > 0 ? st->ack_restored : 0,
			st->ack_replayed);
	if (st->rdb_skipped_cache || st->wal_skipped_cache)
		LM_NOTICE("recover: %ld snapshot and %ld WAL record(s) of cache "
			"collections (durable = no) not loaded - they start empty\n",
			st->rdb_skipped_cache, st->wal_skipped_cache);
	return 0;
}

/* the load verb (runtime, additive): read the current snapshot and
 * store only the records whose key is ABSENT - the live value always
 * wins.  Shares the wire format with rdb_load above but none of its
 * boot-time assumptions (the store is hot and serving). */
int pc_rdb_import(const char *dir, long *loaded, long *skipped_existing,
		long *skipped_expired)
{
	char path[512];
	unsigned char *data;
	struct stat fst;
	pcache_htable_t *ht = NULL;
	time_t now_wall = time(NULL);
	size_t off = 24, n;
	unsigned long long high = 0;
	uint32_t fmt;
	int fd;

	*loaded = *skipped_existing = *skipped_expired = 0;
	snprintf(path, sizeof path, "%s/" PC_RDB_FILE, dir);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	if (fstat(fd, &fst) != 0 || fst.st_size < 28) {
		close(fd);
		return -1;
	}
	n = (size_t)fst.st_size;
	data = malloc(n);
	if (!data || read(fd, data, n) != (ssize_t)n) {
		free(data);
		close(fd);
		return -1;
	}
	close(fd);
	fmt = rd32(data + 4);
	if (rd32(data) != PC_RDB_MAGIC || fmt < 1 || fmt > PC_RDB_FMT ||
	        pc_crc32c(0, data, n - 4) != rd32(data + n - 4)) {
		free(data);
		return -1;
	}
	while (off < n - 4) {
		unsigned char tag = data[off++];

		if (tag == 0xFF)
			break;
		if (tag == 1) {
			unsigned char cl = data[off];

			ht = pc_store_find((const char *)data + off + 1, cl);
			off += 1u + cl;
		} else if (tag == 2) {
			uint32_t kl = rd32(data + off), vl = rd32(data + off + 4);
			unsigned long long wall = rd64(data + off + 8);
			unsigned long long ver = fmt >= 2 ? rd64(data + off + 16) : 0;
			/* S280: the record's type */
			unsigned char fl = fmt >= 3 ? (data[off + 24] & PCACHE_F_CARRY) : 0;
			str k, v;
			unsigned int exp;

			off += PC_RDB_RHDR(fmt);
			k.s = (char *)data + off;
			k.len = (int)kl;
			v.s = (char *)data + off + kl;
			v.len = (int)vl;
			off += kl + vl;
			if (!ht)
				continue;
			exp = wall_to_ticks(wall, now_wall);
			if (exp == 1) {
				(*skipped_expired)++;
				continue;
			}
			if (pcache_ht_probe(ht, &k, NULL, NULL, NULL) == 0) {
				(*skipped_existing)++;
				continue;
			}
			if (pcache_ht_store_ex(ht, &k, &v, exp, fl) == 0) {
				pcache_ht_setver(ht, &k, ver);
				if (ver > high)
					high = ver;
				(*loaded)++;
			}
		} else {
			break;                     /* unknown tag: stop cleanly */
		}
	}
	free(data);
	/* an imported record keeps the version it was saved with, so the
	 * clock has to clear it too - otherwise the next local write would
	 * be numbered below data this node just took in */
	pc_lamport_restore(high);
	return 0;
}
