/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Startup recovery (see recover.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
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

/* the wall-clock second the loaded snapshot began, 0 = none loaded */
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

/* when this node's data stopped being current: the newest mtime in its dir, ms.  It
 * errs EARLY (an idle node reports its last write), so the reconcile keeps. */
static long long last_write_ms;
static int last_write_noted;       /* sampled before the dir lock */

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

/* called before the dir lock, which rewrites perfcached.lock here */
void pc_recover_note_last_write(const char *dir)
{
	last_write_ms = 0;
	last_write_noted = 1;
	if (dir && *dir)
		note_last_write(dir);
}

/* ---- the WAL's renames and drops --------------------------------------- */

/* the collections file names each collection as of its last rename; the snapshot and the
 * replay may carry an older name, mapped forward by these (sorted by seq) */
struct ren {
	unsigned long long seq;
	unsigned char fl, tl;              /* tl 0: a drop */
	char from[PC_COL_NAME_MAX], to[PC_COL_NAME_MAX];
};
static struct ren *rens;
static int nren, cren;

static int ren_cb(int type, const char *col, int collen, const char *key,
		int klen, const char *val, int vlen, unsigned long long wall,
		unsigned long long seq, unsigned long long ver,
		unsigned char flags, void *p)
{
	struct ren *r;

	(void)key; (void)klen; (void)wall; (void)ver; (void)flags; (void)p;
	if (collen <= 0 || collen >= PC_COL_NAME_MAX ||
	    !(type == PC_WAL_T_COL_DROP || (type == PC_WAL_T_COL_RENAME &&
	      vlen > 0 && vlen < PC_COL_NAME_MAX)))
		return 0;
	if (nren == cren) {
		int nc = cren ? 2 * cren : 16;
		struct ren *n = realloc(rens, (size_t)nc * sizeof *n);

		if (!n)
			return 0;
		rens = n;
		cren = nc;
	}
	r = &rens[nren++];
	r->seq = seq;
	r->fl = (unsigned char)collen;
	memcpy(r->from, col, (size_t)collen);
	r->tl = type == PC_WAL_T_COL_RENAME ? (unsigned char)vlen : 0;
	if (r->tl)
		memcpy(r->to, val, (size_t)vlen);
	return 0;
}

static int ren_cmp(const void *a, const void *b)
{
	const struct ren *x = a, *y = b;

	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static void ren_free(void)
{
	free(rens);
	rens = NULL;
	nren = cren = 0;
}

static void ren_scan(const char *dir)
{
	const char *why;

	pc_wal_scan(dir, ren_cb, NULL, &why);
	if (nren > 1)
		qsort(rens, (size_t)nren, sizeof *rens, ren_cmp);
}

/* the table holding now what @nm named at @seq: the last name on its path through the later
 * renames that the store holds; NULL when a later drop ends that path */
static pcache_htable_t *ren_find(const char *nm, size_t nl, unsigned long long seq)
{
	pcache_htable_t *ht = pc_store_find(nm, nl), *h;
	int i;

	for (i = 0; i < nren; i++) {
		const struct ren *r = &rens[i];

		if (r->seq <= seq || r->fl != nl || memcmp(r->from, nm, nl))
			continue;
		if (!r->tl)
			return NULL;
		nm = r->to;
		nl = r->tl;
		if ((h = pc_store_find(nm, nl)))
			ht = h;
	}
	return ht;
}

/* is @nm renamed onto after @seq?  Then the store's @nm is that collection, not the one before */
static int ren_onto_after(const char *nm, size_t nl, unsigned long long seq)
{
	int i;

	for (i = 0; i < nren; i++)
		if (rens[i].seq > seq && rens[i].tl == nl &&
		    !memcmp(rens[i].to, nm, nl))
			return 1;
	return 0;
}

static int rdb_load(const char *dir, struct pc_recover_stats *st)
{
	char path[512];
	unsigned char *data;
	struct stat fst;
	pcache_htable_t *ht = NULL;
	int cache = 0;                     /* ht's collection is durable = no */
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
	if (!data || pc_rdb_read_all(fd, data, n) != (ssize_t)n) {
		free(data);
		close(fd);
		return -1;
	}
	close(fd);

	fmt = rd32(data + 4);
	/* a NEWER format is a downgrade, not corruption: the WAL alone would lose every
	 * replica the snapshot holds, so pc_recover refuses */
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
	st->rdb_wall = rd64(data + 16);    /* stamped when the save began */

	while (off < n - 4) {
		unsigned char tag = data[off++];

		if (tag == 0xFF)
			break;
		if (tag == 1) {
			unsigned char cl;

			if (!pc_rdb_col_fits(data, n, off))
				goto torn;
			cl = data[off];
			ht = ren_find((const char *)data + off + 1, cl, st->marker);
			if (!ht)
				st->rdb_skipped_nocol++;
			/* a cache collection starts empty - a snapshot taken
			 * before it was made one is not loaded into it */
			cache = ht && !pc_store_durable(ht);
			if (cache)
				ht = NULL;
			off += 1u + cl;
		} else if (tag == 2) {
			uint32_t kl, vl;
			unsigned long long wall, ver;
			unsigned char fl;
			str k, v;
			unsigned int exp;

			if (!pc_rdb_rec_fits(data, n, off, fmt, &kl, &vl))
				goto torn;
			wall = rd64(data + off + 8);
			/* format 1 predates the field: those records come back
			 * unnumbered rather than being refused */
			ver = fmt >= 2 ? rd64(data + off + 16) : 0;
			/* the record's type */
			fl = fmt >= 3 ? (data[off + 24] & PCACHE_F_CARRY) : 0;
			off += PC_RDB_RHDR(fmt);
			k.s = (char *)data + off;
			k.len = (int)kl;
			v.s = (char *)data + off + kl;
			v.len = (int)vl;
			off += kl + vl;
			if (!ht) {
				if (cache)
					st->rdb_skipped_cache++;
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
			goto torn;
		}
	}
	free(data);
	return 0;
torn:
	free(data);
	return -1;
}

/* ---- WAL replay -------------------------------------------------------- */

/* Replay by VERSION, not file order: per-worker rings put one key's records out of
 * order.  Deletes are remembered here for one replay, so an older upsert cannot revive. */
#define TOMB_BUCKETS 65536u
struct tomb {
	struct tomb *next;
	unsigned long long ver;
	unsigned int exp;                  /* a pending touch's expiry */
	int len;                           /* col '\0' key */
	char k[];
};
static struct tomb **tombs;
/* a touch read before its key's upsert (another ring): applied when an older upsert lands */
static struct tomb **touches;

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

static unsigned long long tab_get(struct tomb **tab, const char *k, int n,
		unsigned int *exp)
{
	struct tomb *t;

	if (!tab)
		return 0;
	for (t = tab[tomb_hash(k, n)]; t; t = t->next)
		if (t->len == n && !memcmp(t->k, k, (size_t)n)) {
			if (exp)
				*exp = t->exp;
			return t->ver;
		}
	return 0;
}

static void tab_note(struct tomb ***tabp, const char *k, int n,
		unsigned long long ver, unsigned int exp)
{
	struct tomb *t;
	unsigned int h;

	if (!*tabp && !(*tabp = calloc(TOMB_BUCKETS, sizeof **tabp)))
		return;                        /* no memory: file order it is */
	h = tomb_hash(k, n);
	for (t = (*tabp)[h]; t; t = t->next)
		if (t->len == n && !memcmp(t->k, k, (size_t)n)) {
			if (ver > t->ver) {
				t->ver = ver;
				t->exp = exp;
			}
			return;
		}
	t = malloc(sizeof *t + (size_t)n);
	if (!t)
		return;
	t->ver = ver;
	t->exp = exp;
	t->len = n;
	memcpy(t->k, k, (size_t)n);
	t->next = (*tabp)[h];
	(*tabp)[h] = t;
}

static unsigned long long tomb_get(const char *k, int n)
{
	return tab_get(tombs, k, n, NULL);
}

static void tomb_note(const char *k, int n, unsigned long long ver)
{
	tab_note(&tombs, k, n, ver, 0);
}

/* the tombstone key's buffer, grown to fit: the store takes keys to 64 KB */
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

static void tab_free(struct tomb ***tabp)
{
	unsigned int i;
	struct tomb *t, *n;

	if (!*tabp)
		return;
	for (i = 0; i < TOMB_BUCKETS; i++)
		for (t = (*tabp)[i]; t; t = n) {
			n = t->next;
			free(t);
		}
	free(*tabp);
	*tabp = NULL;
}

static void tomb_free(void)
{
	tab_free(&tombs);
	tab_free(&touches);
	free(tk_buf);
	tk_buf = NULL;
	tk_cap = 0;
}

struct replay_ctx {
	struct pc_recover_stats *st;
	time_t now_wall;
	long unk_pending;       /* unknown records not yet bounded */
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
	unsigned long long tv = 0;          /* a touch meets this */
	int trc = 0;

	/* a newer build's record, bounded by the next known one: at or under the marker
	 * the snapshot holds what it did; above, or last in the log, pc_recover refuses */
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
	/* a numbered delete counts wherever it lies and whatever its collection became:
	 * below the marker the snapshot holds it, above it this replay does */
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
	/* the collection set replays in order with the records: a create precedes the
	 * records written into it, a drop undoes an earlier create */
	if (type == PC_WAL_T_COL_RENAME) {
		/* only for a file written before renames were persisted: one that holds the new
		 * name, or a later one on its path, is past this record */
		if (vlen > 0 && pc_store_find(col, (size_t)collen) &&
		    !ren_find(val, (size_t)vlen, seq))
			(void)pc_store_rename(col, (size_t)collen, val,
				(size_t)vlen, pc_lamport_now());
		return 0;
	}
	if (type == PC_WAL_T_COL_CREATE || type == PC_WAL_T_COL_DROP) {
		if (type == PC_WAL_T_COL_DROP) {
			/* the store's name is the collection renamed onto it later */
			if (ren_onto_after(col, (size_t)collen, seq))
				return 0;
			if (pc_store_find(col, (size_t)collen))
				pc_store_drop(col, (size_t)collen);
			else
				pc_store_note_drop(col, (size_t)collen,
					pc_lamport_now());
		} else if (!pc_store_find(col, (size_t)collen) && vlen == 1) {
			if (!pc_col_name_held_ok(col, (size_t)collen)) {
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
	/* a write racing its rename may be logged under the old name after it */
	ht = ren_find(col, (size_t)collen, seq);
	if (!ht)
		ht = ren_find(col, (size_t)collen, 0);
	if (!ht) {
		c->st->wal_skipped++;
		return 0;
	}
	/* tombstones by table, not by the name a record was logged under */
	col = (const char *)&ht;
	collen = (int)sizeof ht;
	/* a cache collection starts empty - records logged before it was
	 * made one are not replayed into it */
	if (!pc_store_durable(ht)) {
		c->st->wal_skipped_cache++;
		return 0;
	}
	k.s = (char *)key;
	k.len = klen;
	exp = wall_to_ticks(wall, c->now_wall);

	switch (type) {
	case PC_WAL_T_UPSERT:              /* unversioned: arrives with ver 0 */
	case PC_WAL_T_UPSERT_V:
		if (exp == 1) {
			c->st->wal_skipped++;      /* born dead: expired already */
			return 0;
		}
		v.s = (char *)val;
		v.len = vlen;
		if (ver) {                     /* by version */
			char *tk = tk_get((size_t)collen + 1 + (size_t)klen);
			int rc;

			if (tk &&
			    tomb_get(tk, tomb_key(tk, col, collen, key, klen)) >= ver) {
				c->st->wal_older++;    /* deleted by a newer record */
				return 0;
			}
			/* the record's type comes back with it */
			rc = pcache_ht_store_ver(ht, &k, &v, exp,
				(unsigned char)(flags & PCACHE_F_CARRY), ver);
			if (rc == PCACHE_E_OLDER) {
				c->st->wal_older++;    /* a newer one already here */
				return 0;
			}
			/* no room for this newer record: the older copy left behind
			 * must not be what the node restores */
			if (rc == -2)
				pcache_ht_drop_older(ht, &k, ver);
			if (rc == 0) {
				unsigned int texp = 0;
				unsigned long long ptv = tk ? tab_get(touches, tk,
					tomb_key(tk, col, collen, key, klen), &texp) : 0;

				if (ver > c->st->ver_high)
					c->st->ver_high = ver;
				c->st->wal_applied++;
				/* a newer touch read before this upsert: its expiry wins */
				if (ptv > ver && pcache_ht_touch(ht, &k, texp) == 1)
					pcache_ht_setver(ht, &k, ptv);
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
	case PC_WAL_T_DEL_N:               /* a DEL, numbered */
	case PC_WAL_T_DEL: {
		char *tk = tk_get((size_t)collen + 1 + (size_t)klen);
		unsigned long long have = 0;

		if (vlen != PC_WAL_DEL_VERLEN &&    /* unversioned: file order */
		    !(type == PC_WAL_T_DEL_N && vlen == PC_WAL_DELN_VLEN)) {
			pcache_ht_remove(ht, &k);
			c->st->wal_applied++;
			return 0;
		}
		/* the version rides as the value (wal.h) */
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
	case PC_WAL_T_TOUCH:               /* unversioned: arrives with ver 0 */
	case PC_WAL_T_TOUCH_V:
		if (exp == 1 && ver) {
			/* re-armed into the past = a DELETE at this version: it honours
			 * versions and is remembered, so an older write cannot revive it */
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
			c->st->wal_older++;        /* an older touch */
			return 0;
		} else if ((trc = pcache_ht_touch(ht, &k, exp)) == 1 && ver) {
			/* put back the version the touch ticked when logged,
			 * not the fresh tick the replay just made */
			pcache_ht_setver(ht, &k, ver);
			if (ver > c->st->ver_high)
				c->st->ver_high = ver;
		} else if (trc == 0 && ver) {
			/* no key yet: its upsert may follow from another ring */
			char *tk = tk_get((size_t)collen + 1 + (size_t)klen);

			if (tk)
				tab_note(&touches, tk, tomb_key(tk, col, collen, key, klen),
					ver, exp);
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

static int skip_unknown;                   /* [wal] unknown_records */

void pc_recover_set_skip_unknown(int on)
{
	skip_unknown = on ? 1 : 0;
}

/* ---- entry ------------------------------------------------------------- */

int pc_recover(const char *dir, struct pc_recover_stats *st)
{
	/* before anything below writes in this directory, or the newest mtime
	 * would be "now" rather than "when I died" */
	if (!last_write_noted)             /* the daemon sampled it first */
		note_last_write(dir);
	struct replay_ctx ctx;
	const char *why = "end";
	int rc;

	memset(st, 0, sizeof *st);
	ren_scan(dir);                     /* before either name lookup */
	rc = rdb_load(dir, st);
	if (rc == -2) {
		ren_free();
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
		char bad[512], aside[560];

		LM_CRIT("recover: the snapshot in %s is missing pieces or "
			"corrupt - starting from the WAL alone\n", dir);
		st->marker = 0;
		/* kept for a look: the checkpoint after recovery would overwrite it */
		snprintf(bad, sizeof bad, "%s/" PC_RDB_FILE, dir);
		snprintf(aside, sizeof aside, "%s.bad.%lld", bad, (long long)time(NULL));
		if (rename(bad, aside) == 0)
			LM_CRIT("recover: the unreadable snapshot is kept as %s\n", aside);
		else
			LM_WARN("recover: could not keep the unreadable snapshot aside "
				"(%s)\n", strerror(errno));
	}

	/* the marks that go with the snapshot just loaded, before
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
	tomb_free();                       /* one replay's worth */
	ren_free();
	st->wal_unknown += ctx.unk_pending;   /* the log's last word */
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
	/* resume the version clock above everything restored: restore, not observe(),
	 * which at boot (clock 0) would refuse our own dataset as absurd */
	recovered_n = st->rdb_records + st->wal_applied;
	recovered_wal = st->wal_applied;
	recovered_snap_wall = st->rdb_ok ? st->rdb_wall : 0;
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

/* the load verb: store only the snapshot records whose key is ABSENT (the live value
 * wins); rdb_load's format, none of its boot-time assumptions */
/* a test holds the import between a key's probe and its store (PERFCACHED_TEST_IMPORT_GAP_MS) */
static long import_gap_ms(void)
{
	static long m = -1;

	if (m < 0) {
		const char *e = getenv("PERFCACHED_TEST_IMPORT_GAP_MS");
		long v = e ? atol(e) : 0;

		m = v > 0 ? v : 0;
	}
	return m;
}

static int rdb_import(const char *dir, long *loaded, long *skipped_existing,
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
	if (!data || pc_rdb_read_all(fd, data, n) != (ssize_t)n) {
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
			unsigned char cl;

			if (!pc_rdb_col_fits(data, n, off))
				goto torn;
			cl = data[off];
			ht = pc_store_find((const char *)data + off + 1, cl);
			off += 1u + cl;
		} else if (tag == 2) {
			uint32_t kl, vl;
			unsigned long long wall, ver;
			unsigned char fl;
			str k, v;
			unsigned int exp;
			int rc;

			if (!pc_rdb_rec_fits(data, n, off, fmt, &kl, &vl))
				goto torn;
			wall = rd64(data + off + 8);
			ver = fmt >= 2 ? rd64(data + off + 16) : 0;
			/* the record's type */
			fl = fmt >= 3 ? (data[off + 24] & PCACHE_F_CARRY) : 0;
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
			/* the clock first: a client write from here on outranks this copy */
			pc_lamport_restore(ver);
			if (pcache_ht_probe(ht, &k, NULL, NULL, NULL) == 0) {
				(*skipped_existing)++;
				continue;
			}
			if (import_gap_ms()) {
				struct timespec gt = { import_gap_ms() / 1000,
					(import_gap_ms() % 1000) * 1000000L };

				nanosleep(&gt, NULL);
			}
			/* under the bucket lock with its version: a write since the probe wins */
			rc = pcache_ht_store_ver(ht, &k, &v, exp, fl, ver);
			if (rc == 0) {
				if (ver > high)
					high = ver;
				(*loaded)++;
			} else if (rc == PCACHE_E_OLDER) {
				(*skipped_existing)++;
			}
		} else {
			break;                     /* unknown tag: stop cleanly */
		}
	}
	free(data);
	/* imported records keep their versions: the clock must clear them, or the
	 * next local write is numbered below data this node just took in */
	pc_lamport_restore(high);
	return 0;
torn:
	free(data);
	pc_lamport_restore(high);
	return -1;
}

/* one import at a time: each holds a snapshot-sized buffer and its worker for the whole file */
int pc_rdb_import(const char *dir, long *loaded, long *skipped_existing,
		long *skipped_expired)
{
	static int busy;
	int rc;

	*loaded = *skipped_existing = *skipped_expired = 0;
	if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQ_REL))
		return -2;
	rc = rdb_import(dir, loaded, skipped_existing, skipped_expired);
	__atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
	return rc;
}
