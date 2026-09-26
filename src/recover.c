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
#include "wal.h"
#include "rdb.h"
#include "recover.h"

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

static int rdb_load(const char *dir, struct pc_recover_stats *st)
{
	char path[512];
	unsigned char *data;
	struct stat fst;
	pcache_htable_t *ht = NULL;
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
	if (rd32(data) != PC_RDB_MAGIC || fmt < 1 || fmt > PC_RDB_FMT ||
	        pc_crc32c(0, data, n - 4) != rd32(data + n - 4)) {
		free(data);
		return -1;
	}
	st->marker = rd64(data + 8);

	while (off < n - 4) {
		unsigned char tag = data[off++];

		if (tag == 0xFF)
			break;
		if (tag == 1) {
			unsigned char cl = data[off];

			ht = pc_store_find((const char *)data + off + 1, cl);
			if (!ht)
				st->rdb_skipped_nocol++;
			off += 1u + cl;
		} else if (tag == 2) {
			uint32_t kl = rd32(data + off), vl = rd32(data + off + 4);
			unsigned long long wall = rd64(data + off + 8);
			/* format 1 predates the field: those records come back
			 * unnumbered rather than being refused */
			unsigned long long ver = fmt >= 2 ? rd64(data + off + 16) : 0;
			str k, v;
			unsigned int exp;

			off += fmt >= 2 ? 24 : 16;
			k.s = (char *)data + off;
			k.len = (int)kl;
			v.s = (char *)data + off + kl;
			v.len = (int)vl;
			off += kl + vl;
			if (!ht)
				continue;              /* its collection went away */
			exp = wall_to_ticks(wall, now_wall);
			if (exp == 1) {
				st->rdb_skipped_expired++;
				continue;
			}
			if (pcache_ht_store(ht, &k, &v, exp) == 0) {
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
}

struct replay_ctx {
	struct pc_recover_stats *st;
	time_t now_wall;
};

static int replay_cb(int type, const char *col, int collen, const char *key,
		int klen, const char *val, int vlen, unsigned long long wall,
		unsigned long long seq, unsigned long long ver, void *p)
{
	struct replay_ctx *c = p;
	pcache_htable_t *ht;
	str k, v;
	unsigned int exp;
	unsigned long long tv = 0;          /* S240: a touch meets this */

	if (seq > c->st->last_seq)
		c->st->last_seq = seq;
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
			char tk[512];
			int rc;

			if (collen + 1 + klen <= (int)sizeof tk &&
			    tomb_get(tk, tomb_key(tk, col, collen, key, klen)) >= ver) {
				c->st->wal_older++;    /* deleted by a newer record */
				return 0;
			}
			rc = pcache_ht_store_ver(ht, &k, &v, exp, 0, ver);
			if (rc == PCACHE_E_OLDER) {
				c->st->wal_older++;    /* a newer one already here */
				return 0;
			}
			if (rc == 0) {
				if (ver > c->st->ver_high)
					c->st->ver_high = ver;
				c->st->wal_applied++;
			}
			return 0;
		}
		if (pcache_ht_store(ht, &k, &v, exp) == 0) {
			pcache_ht_setver(ht, &k, ver);
			if (ver > c->st->ver_high)
				c->st->ver_high = ver;
			c->st->wal_applied++;
		}
		return 0;
	case PC_WAL_T_DEL: {
		char tk[512];
		unsigned long long have = 0;

		if (vlen != PC_WAL_DEL_VERLEN) {  /* pre-S240: file order */
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

		if (collen + 1 + klen <= (int)sizeof tk)
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
		if (exp == 1) {
			pcache_ht_remove(ht, &k);  /* re-armed into the past */
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

/* ---- entry ------------------------------------------------------------- */

int pc_recover(const char *dir, struct pc_recover_stats *st)
{
	/* S223: BEFORE anything below opens a file for writing - the replay
	 * and the snapshot machinery both touch this directory, and after
	 * that the newest mtime would be "now" rather than "when I died" */
	note_last_write(dir);
	struct replay_ctx ctx;
	const char *why = "end";
	int rc;

	memset(st, 0, sizeof *st);
	rc = rdb_load(dir, st);
	if (rc == 0) {
		st->rdb_ok = 1;
	} else if (rc < 0) {
		/* a cache starts cold rather than refusing: replay everything
		 * the WAL still holds (marker 0) and say so LOUDLY */
		LM_CRIT("recover: the snapshot in %s is missing pieces or "
			"corrupt - starting from the WAL alone\n", dir);
		st->marker = 0;
	}

	ctx.st = st;
	ctx.now_wall = time(NULL);
	pc_wal_scan(dir, replay_cb, &ctx, &why);
	tomb_free();                       /* S240: one replay's worth */
	/* Resume the version clock above everything that came back.  Not
	 * observe(): that exists to refuse a peer claiming an absurd value,
	 * and at boot - clock 0 against a fleet long past a billion writes -
	 * it would refuse our own dataset and count it as a defect. */
	recovered_n = st->rdb_records + st->wal_applied;
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
			str k, v;
			unsigned int exp;

			off += fmt >= 2 ? 24 : 16;
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
			if (pcache_ht_store(ht, &k, &v, exp) == 0) {
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
