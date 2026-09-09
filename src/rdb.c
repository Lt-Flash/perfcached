/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * rdb.c — the RDB snapshot (task S14).  See rdb.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

#include "compat/dprint.h"
#include "compat/timer.h"
#include "core/pcache_htable.h"
#include "store.h"
#include "wal.h"
#include "rdb.h"

static struct {
	int enabled;
	char dir[400];
	struct pc_rdb_rule rules[PC_RDB_MAX_RULES];
	int nrules;
	double budget_mb_s;
	volatile int save_req;
	volatile int running;
	unsigned long long saves, last_bytes, last_marker, seq_at_last;
	/* the marker of the last COMPLETED snapshot - last_marker is
	 * stamped at walk start and must not gate WAL recycling until the
	 * rename lands */
	unsigned long long safe_marker;
	long long last_dur_ms, last_unix, last_save_mono_s;
} R;

static long long mono_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ---- LE writers over a buffered, rate-limited, crc-tracked stream ------ */

struct wr {
	int fd;
	unsigned char buf[1 << 16];
	size_t n;
	uint32_t crc;
	unsigned long long total;
	long long t0;
	double budget;                     /* bytes per second, 0 = unlimited */
	int failed;
};

static void wr_flush(struct wr *w)
{
	size_t off = 0;

	while (off < w->n) {
		ssize_t r = write(w->fd, w->buf + off, w->n - off);

		if (r <= 0) {
			w->failed = 1;
			return;
		}
		off += (size_t)r;
	}
	w->n = 0;
	/* the rate limiter: sleep off any budget deficit.  Granularity is
	 * one buffer (64K); the walk callback may hold the overflow lock
	 * while we sleep - bounded by 64K/budget, accepted and documented */
	if (w->budget > 0) {
		double ahead = (double)w->total -
			w->budget * ((double)(mono_us() - w->t0) / 1e6);

		if (ahead > 0)
			usleep((useconds_t)(ahead / w->budget * 1e6));
	}
}

static void wr_put(struct wr *w, const void *p, size_t n)
{
	const unsigned char *b = p;

	w->crc = pc_crc32c(w->crc, b, n);
	w->total += n;
	while (n) {
		size_t room = sizeof w->buf - w->n, c = n < room ? n : room;

		memcpy(w->buf + w->n, b, c);
		w->n += c;
		b += c;
		n -= c;
		if (w->n == sizeof w->buf)
			wr_flush(w);
	}
}

static void put8(struct wr *w, uint8_t v)   { wr_put(w, &v, 1); }

static void put32(struct wr *w, uint32_t v)
{
	unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
		(unsigned char)(v >> 16), (unsigned char)(v >> 24) };

	wr_put(w, b, 4);
}

static void put64(struct wr *w, uint64_t v)
{
	put32(w, (uint32_t)v);
	put32(w, (uint32_t)(v >> 32));
}

/* ---- the walk ---------------------------------------------------------- */

struct walk_ctx {
	struct wr *w;
	unsigned int now_ticks;
	uint64_t now_wall;
	long written;
};

static int walk_cb(const str *key, const str *val, unsigned int exp,
		unsigned int wt, unsigned char fl, unsigned long long ver,
		void *p)
{
	struct walk_ctx *c = p;
	uint64_t wall = 0;

	(void)wt;
	(void)fl;

	if (c->w->failed)
		return -1;
	if (exp) {
		if (exp <= c->now_ticks)
			return 0;                  /* expired: skip at write time */
		wall = c->now_wall + (exp - c->now_ticks);
	}
	put8(c->w, 2);
	put32(c->w, (uint32_t)key->len);
	put32(c->w, (uint32_t)val->len);
	put64(c->w, wall);
	put64(c->w, ver);
	wr_put(c->w, key->s, (size_t)key->len);
	wr_put(c->w, val->s, (size_t)val->len);
	c->written++;
	return 0;
}

static int rdb_save(void)
{
	struct pc_wal_stats ws;
	struct wr w;
	struct walk_ctx ctx;
	char tmp[512], fin[512];
	long long t0 = mono_us();
	int i, dirfd;

	memset(&w, 0, sizeof w);
	snprintf(tmp, sizeof tmp, "%s/" PC_RDB_FILE ".tmp.%d", R.dir, getpid());
	snprintf(fin, sizeof fin, "%s/" PC_RDB_FILE, R.dir);
	w.fd = open(tmp, O_CREAT | O_EXCL | O_WRONLY, 0600);
	if (w.fd < 0) {
		LM_ERR("rdb: cannot create %s (%s)\n", tmp, strerror(errno));
		return -1;
	}
	w.t0 = t0;
	w.budget = R.budget_mb_s * 1e6;

	/* the consistency marker: the WAL sequence at WALK START */
	pc_wal_get_stats(&ws);
	R.last_marker = ws.last_seq;

	put32(&w, PC_RDB_MAGIC);
	put32(&w, PC_RDB_FMT);
	put64(&w, ws.last_seq);
	put64(&w, (uint64_t)time(NULL));

	ctx.w = &w;
	ctx.now_ticks = get_ticks();
	ctx.now_wall = (uint64_t)time(NULL);
	ctx.written = 0;
	for (i = 0; i < pc_store_count() && !w.failed; i++) {
		const char *name = pc_store_name(i);

		put8(&w, 1);
		put8(&w, (uint8_t)strlen(name));
		wr_put(&w, name, strlen(name));
		pcache_ht_iter_meta(pc_store_ht(i), walk_cb, &ctx);
	}

	put8(&w, 0xFF);
	{
		uint32_t crc = w.crc;          /* crc of everything incl. 0xFF */
		unsigned char b[4] = { (unsigned char)crc,
			(unsigned char)(crc >> 8), (unsigned char)(crc >> 16),
			(unsigned char)(crc >> 24) };

		/* append WITHOUT feeding the crc back into itself */
		if (w.n + 4 > sizeof w.buf)
			wr_flush(&w);
		w.total += 4;
		memcpy(w.buf + w.n, b, 4);
		w.n += 4;
	}
	wr_flush(&w);
	if (w.failed || fdatasync(w.fd) != 0) {
		LM_ERR("rdb: write failed (%s)\n", strerror(errno));
		close(w.fd);
		unlink(tmp);
		return -1;
	}
	close(w.fd);

	if (rename(tmp, fin) != 0) {
		LM_ERR("rdb: rename failed (%s)\n", strerror(errno));
		unlink(tmp);
		return -1;
	}
	/* the crash-safe half of rotation: the DIRECTORY entry must be
	 * durable too (NFS rename semantics included) */
	dirfd = open(R.dir, O_RDONLY | O_DIRECTORY);
	if (dirfd >= 0) {
		fsync(dirfd);
		close(dirfd);
	}

	R.saves++;
	R.safe_marker = R.last_marker;  /* coverage is real only now */
	R.last_bytes = w.total;
	R.last_dur_ms = (mono_us() - t0) / 1000;
	R.last_unix = (long long)time(NULL);
	R.seq_at_last = ws.last_seq;
	R.last_save_mono_s = mono_us() / 1000000;
	LM_NOTICE("rdb: snapshot %ld records, %llu bytes, %lld ms, wal "
		"marker %llu\n", ctx.written, w.total, R.last_dur_ms,
		R.last_marker);
	return 0;
}

/* ---- triggers + the thread body ---------------------------------------- */

/* the longest rule interval = the worst-case gap between snapshots,
 * i.e. how much WAL must fit between them (the sizing verb's basis);
 * 0 = no automatic snapshots configured */
/* the WAL-recycle boundary: everything at or below this seq is inside
 * the last COMPLETED snapshot; 0 = no snapshot has completed yet */
unsigned long long pc_rdb_safe_marker(void)
{
	return R.safe_marker;
}

int pc_rdb_max_interval_s(void)
{
	int i, mx = 0;

	for (i = 0; i < R.nrules; i++)
		if (R.rules[i].secs > mx)
			mx = R.rules[i].secs;
	return mx;
}

int pc_rdb_init(const char *dir, const struct pc_rdb_rule *rules,
		int nrules, int max_mb_s, double probe_mb_s)
{
	snprintf(R.dir, sizeof R.dir, "%s", dir);
	if (nrules > PC_RDB_MAX_RULES)
		nrules = PC_RDB_MAX_RULES;
	memcpy(R.rules, rules, (size_t)nrules * sizeof *rules);
	R.nrules = nrules;
	if (max_mb_s > 0)
		R.budget_mb_s = max_mb_s;
	else if (probe_mb_s > 0)
		R.budget_mb_s = probe_mb_s / 4;    /* leave the link to the WAL */
	else
		R.budget_mb_s = 64;
	R.last_save_mono_s = mono_us() / 1000000;
	R.enabled = 1;
	LM_NOTICE("rdb: enabled - %d trigger rule(s), writer budget %.0f "
		"MB/s\n", nrules, R.budget_mb_s);
	return 0;
}

void pc_rdb_thread(volatile int *stop)
{
	struct pc_wal_stats ws;

	while (!*stop) {
		int fire = 0, i;

		usleep(250 * 1000);
		if (!R.enabled)
			continue;
		if (R.save_req)
			fire = 1;
		if (!fire && R.nrules) {
			long long elapsed = mono_us() / 1000000 - R.last_save_mono_s;

			pc_wal_get_stats(&ws);
			for (i = 0; i < R.nrules; i++)
				if (elapsed >= R.rules[i].secs &&
				    (long long)(ws.last_seq - R.seq_at_last) >=
				        R.rules[i].changes) {
					fire = 1;
					break;
				}
		}
		if (fire) {
			R.save_req = 0;
			R.running = 1;
			rdb_save();
			R.running = 0;
		}
	}
}

int pc_rdb_save_sync(void)
{
	int rc;

	if (!R.enabled)
		return -1;
	R.running = 1;
	rc = rdb_save();
	R.running = 0;
	return rc;
}

int pc_rdb_request_save(void)
{
	if (R.running)
		return 1;
	R.save_req = 1;
	return 0;
}

const char *pc_rdb_dir(void)
{
	return R.dir;
}

void pc_rdb_get_stats(struct pc_rdb_stats *out)
{
	memset(out, 0, sizeof *out);
	out->enabled = R.enabled;
	out->running = R.running;
	out->saves = R.saves;
	out->last_bytes = R.last_bytes;
	out->last_marker = R.last_marker;
	out->last_dur_ms = R.last_dur_ms;
	out->last_unix = R.last_unix;
}

/* ---- validate (tests, -R; S15 builds the loader on this walk) ---------- */

static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

long pc_rdb_validate(const char *dir, unsigned long long *marker,
		long *ncols, const char **why)
{
	char path[512];
	unsigned char *data;
	struct stat st;
	long recs = 0, cols = 0;
	size_t off = 24, n;
	uint32_t crc, fmt, rhdr;
	int fd;

	*why = "ok";
	snprintf(path, sizeof path, "%s/" PC_RDB_FILE, dir);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		*why = "missing";
		return -1;
	}
	if (fstat(fd, &st) != 0 || st.st_size < 28) {
		close(fd);
		*why = "truncated";
		return -1;
	}
	n = (size_t)st.st_size;
	data = malloc(n);
	if (!data || read(fd, data, n) != (ssize_t)n) {
		free(data);
		close(fd);
		*why = "read";
		return -1;
	}
	close(fd);

	fmt = rd32(data + 4);
	if (rd32(data) != PC_RDB_MAGIC || fmt < 1 || fmt > PC_RDB_FMT) {
		*why = "bad-magic";
		goto bad;
	}
	rhdr = fmt >= 2 ? 24u : 16u;   /* format 2 records carry a version */
	crc = pc_crc32c(0, data, n - 4);
	if (crc != rd32(data + n - 4)) {
		*why = "crc";
		goto bad;
	}
	if (marker)
		*marker = (unsigned long long)rd32(data + 8) |
			((unsigned long long)rd32(data + 12) << 32);

	while (off < n - 4) {
		uint8_t tag = data[off++];

		if (tag == 0xFF)
			break;
		if (tag == 1) {
			if (off >= n - 4)
				goto torn;
			off += 1u + data[off];
			cols++;
		} else if (tag == 2) {
			uint32_t kl, vl;

			if (off + rhdr > n - 4)
				goto torn;
			kl = rd32(data + off);
			vl = rd32(data + off + 4);
			off += rhdr + kl + vl;
			if (off > n - 4)
				goto torn;
			recs++;
		} else {
			goto torn;
		}
	}
	free(data);
	if (ncols)
		*ncols = cols;
	return recs;
torn:
	*why = "torn";
bad:
	free(data);
	return -1;
}
