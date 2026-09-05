/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * wal.c — the write-ahead log (task S13).  See wal.h for the contract
 * and the on-disk format.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <sys/statvfs.h>

#include "compat/dprint.h"
#include "compat/pt.h"
#include "compat/timer.h"
#include "walprobe.h"
#include "wal.h"
#include "daemon.h"                       /* pc_worker_id() */
#include "rdb.h"                        /* safe marker + save request */

#define MAX_THREADS 520
#define MAX_REC     (80L * 1024)
#define BATCH_MAX   4096

/* ---- CRC32C: slice-by-8 software + per-arch hardware dispatch ---------- */

static uint32_t crc_tab[8][256];

static void crc_init_tab(void)
{
	uint32_t i, j, c;

	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++)
			c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1)));
		crc_tab[0][i] = c;
	}
	for (i = 0; i < 256; i++)
		for (j = 1; j < 8; j++)
			crc_tab[j][i] = (crc_tab[j - 1][i] >> 8) ^
				crc_tab[0][crc_tab[j - 1][i] & 0xFF];
}

static uint32_t crc32c_sw(uint32_t crc, const unsigned char *p, size_t n)
{
	crc = ~crc;
	while (n >= 8) {
		uint32_t lo, hi;

		memcpy(&lo, p, 4);
		memcpy(&hi, p + 4, 4);
		lo ^= crc;
		crc = crc_tab[7][lo & 0xFF] ^ crc_tab[6][(lo >> 8) & 0xFF] ^
		      crc_tab[5][(lo >> 16) & 0xFF] ^ crc_tab[4][lo >> 24] ^
		      crc_tab[3][hi & 0xFF] ^ crc_tab[2][(hi >> 8) & 0xFF] ^
		      crc_tab[1][(hi >> 16) & 0xFF] ^ crc_tab[0][hi >> 24];
		p += 8;
		n -= 8;
	}
	while (n--)
		crc = (crc >> 8) ^ crc_tab[0][(crc ^ *p++) & 0xFF];
	return ~crc;
}

#if defined(__x86_64__)
__attribute__((target("sse4.2")))
static uint32_t crc32c_hw(uint32_t crc, const unsigned char *p, size_t n)
{
	crc = ~crc;
	while (n >= 8) {
		uint64_t v;

		memcpy(&v, p, 8);
		crc = (uint32_t)__builtin_ia32_crc32di(crc, v);
		p += 8;
		n -= 8;
	}
	while (n--)
		crc = __builtin_ia32_crc32qi(crc, *p++);
	return ~crc;
}
static int hw_ok(void) { return __builtin_cpu_supports("sse4.2"); }
#elif defined(__aarch64__)
#include <sys/auxv.h>
#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1 << 7)
#endif
__attribute__((target("+crc")))
static uint32_t crc32c_hw(uint32_t crc, const unsigned char *p, size_t n)
{
	crc = ~crc;
	while (n >= 8) {
		uint64_t v;

		memcpy(&v, p, 8);
		crc = __builtin_aarch64_crc32cx(crc, v);
		p += 8;
		n -= 8;
	}
	while (n--)
		crc = __builtin_aarch64_crc32cb(crc, *p++);
	return ~crc;
}
static int hw_ok(void) { return !!(getauxval(AT_HWCAP) & HWCAP_CRC32); }
#else
static uint32_t crc32c_hw(uint32_t crc, const unsigned char *p, size_t n)
{
	return crc32c_sw(crc, p, n);
}
static int hw_ok(void) { return 0; }
#endif

static uint32_t (*crc_fn)(uint32_t, const unsigned char *, size_t);

uint32_t pc_crc32c(uint32_t init, const void *buf, size_t len)
{
	if (!crc_fn) {                     /* lazy: CLI tools never wal_init */
		crc_init_tab();
		crc_fn = hw_ok() ? crc32c_hw : crc32c_sw;
	}
	return crc_fn(init, buf, len);
}

/* ---- LE helpers -------------------------------------------------------- */

static void w32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

static void w16(unsigned char *p, uint16_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
}

static void w64(unsigned char *p, uint64_t v)
{
	w32(p, (uint32_t)v);
	w32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t r32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t r16(const unsigned char *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint64_t r64(const unsigned char *p)
{
	return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32);
}

/* ---- state ------------------------------------------------------------- */

struct ring {
	unsigned char *buf;
	uint32_t size;
	volatile uint32_t head, tail;     /* monotonically increasing */
	unsigned long long dropped;
};

static struct {
	int enabled;
	char dir[400];
	enum pc_wal_fsync mode;
	long long seg_bytes;
	int nseg;
	struct ring rings[MAX_THREADS];
	int nrings;
	int wake_efd;                     /* daemon wires the WAL thread efd */
	volatile int sync_req;            /* a worker asked for a barrier */
	volatile int sleeping;

	int fd;                           /* active segment */
	uint32_t gen;                     /* active generation */
	long long off;
	long long last_fsync_us;
	unsigned long long seq;           /* the global stamp */
	unsigned long long appended, bytes, late, recycles, synced_seq,
		pending_hi;
	/* fixed-size ring accounting: the files ARE all the space there
	 * will ever be (no on-the-fly resize - a grow would stall the
	 * workers), so "free" = segments whose records sit at or below
	 * the last COMPLETED snapshot marker, and FULL = the pump must
	 * overwrite one that is not free (those records stop being
	 * replayable).  Warn near full, error+count at full; both kick a
	 * snapshot, which is what frees space. */
	unsigned long long *seg_first, *seg_last;  /* per segment, by idx */
	uint32_t *seg_gen;
	/* CONTROL: our pg_control.  The spans above used to live only here
	 * in RAM, so after a restart every one of them read zero, seg_hot()
	 * answered "cold" for every segment and the rule this comment
	 * describes was not enforced at all.  Both sides of the comparison
	 * have to survive a restart - PostgreSQL keeps the REDO point in
	 * pg_control, RocksDB keeps per-log bounds in the MANIFEST.  Written
	 * at activation and rotation only, which already pay an fdatasync;
	 * NEVER on the append path. */
	int ctrl_fd;
	/* observed fsync cost - the pump is the only writer, stats the only
	 * reader, so plain longs are enough (a torn read reports a slightly
	 * stale mean, never a wrong decision) */
	unsigned long long fsync_n, fsync_us_sum, fsync_us_max;
	unsigned long long fsync_ewma_us;   /* recent cost, 1/8 weight */
	int probe_warned;
	unsigned long long overruns;
	int free_segs;
	int pressure_warned;
	int dropped_any;               /* has this WAL ever dropped? */
	int shed;                      /* already taken out of service */
	/* a ring drop is a write the CLIENT was told succeeded that will
	 * not be in the WAL.  It was counted and nothing else - a counter
	 * nobody reads is not a signal, so the pump shouts as well.  The
	 * shout lives here and not at the drop site: that site is the
	 * worker's hot path, and logging per drop under the load that
	 * causes drops would be worse than the drops. */
	unsigned long long dropped_reported;
	long long drop_log_us;
} W;

volatile long long pc_wal_fsync_start_us;

static long long wall_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ---- segments ---------------------------------------------------------- */

static void seg_path(char *out, size_t cap, int idx)
{
	snprintf(out, cap, "%s/wal-%03d.seg", W.dir, idx);
}

static int seg_write_header(int fd, uint32_t gen)
{
	unsigned char h[PC_WAL_HDR];

	w32(h, PC_WAL_MAGIC);
	w32(h + 4, gen);
	if (pwrite(fd, h, sizeof h, 0) != (ssize_t)sizeof h)
		return -1;
	return 0;
}

/* provision one segment: REAL zero writes end to end - fallocate does
 * not provision thin/sparse/COW backends, a write does (DESIGN par 7) */
static int seg_provision(int idx)
{
	char path[512], *z;
	long long left = W.seg_bytes;
	int fd;

	seg_path(path, sizeof path, idx);
	fd = open(path, O_CREAT | O_WRONLY, 0600);
	if (fd < 0)
		return -1;
	z = calloc(1, 1 << 20);
	if (!z) {
		close(fd);
		return -1;
	}
	while (left > 0) {
		ssize_t n = write(fd, z, left > (1 << 20) ? (1 << 20) : (size_t)left);

		if (n <= 0) {
			/* Leave NOTHING behind.  The usual reason to be here
			 * is ENOSPC, and a partial segment would hold the
			 * space it already took after the daemon exits - so a
			 * refused start would leave the filesystem full for
			 * everything else on it, and a supervisor's retry
			 * would re-fill whatever had been freed. */
			int e = errno;

			free(z);
			close(fd);
			unlink(path);
			errno = e;
			return -1;
		}
		left -= n;
	}
	free(z);
	if (fdatasync(fd) != 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}
/* CONTROL is written by the activation below; defined with the rest
 * of the control-file handling further down. */
static void ctrl_write(void);

/* defined with the rest of the shed handling further down */
static void wal_shed_once(void);

/* Every fdatasync goes through here: it stamps the in-flight marker the
 * hang watchdog reads, and records what the call actually cost.  The
 * cost is the point - the startup probe cannot see past a write-back
 * cache, and this is the only measurement taken while the daemon is
 * under its real load. */
static void wal_fdatasync_timed(long long now)
{
	long long dur;

	pc_wal_fsync_start_us = now;
	fdatasync(W.fd);
	dur = wall_now_us() - now;
	pc_wal_fsync_start_us = 0;
	if (dur < 0)
		dur = 0;
	W.fsync_n++;
	W.fsync_us_sum += (unsigned long long)dur;
	if ((unsigned long long)dur > W.fsync_us_max)
		W.fsync_us_max = (unsigned long long)dur;
	/* EWMA, 1/8 weight: tracks what fsync costs NOW.  The lifetime
	 * mean above cannot notice a device degrading - a cache that
	 * fills after an hour of fast syncs is averaged into silence. */
	W.fsync_ewma_us = W.fsync_ewma_us ?
		(W.fsync_ewma_us * 7 + (unsigned long long)dur) / 8 :
		(unsigned long long)dur;
}


/* open segment @idx for a fresh generation (recycling reuses the file:
 * only the header changes, stale records are fenced by the gen echo) */
static int seg_activate(int idx, uint32_t gen)
{
	char path[512];
	int fd;

	seg_path(path, sizeof path, idx);
	fd = open(path, O_WRONLY, 0600);
	if (fd < 0)
		return -1;
	if (seg_write_header(fd, gen) != 0) {
		close(fd);
		return -1;
	}
	if (W.fd >= 0)
		close(W.fd);
	W.fd = fd;
	W.gen = gen;
	W.off = PC_WAL_HDR;
	if (W.seg_first) {
		W.seg_first[idx] = W.pending_hi + 1;
		W.seg_last[idx] = 0;           /* open */
		if (W.seg_gen)
			W.seg_gen[idx] = gen;
		ctrl_write();
	}
	return 0;
}

/* ---- CONTROL: the durable half of the recycling rule ------------------- */
/* Fixed-size, single-pwrite, CRC-checked.  A torn or absent CONTROL is
 * not an error: the spans read as unknown, every segment is then judged
 * hot, and that is the conservative direction - an old WAL directory
 * still starts.  Layout: magic, fmt, crc(of everything from nseg on),
 * nseg, next_seq, then {gen, pad, first, last} per segment. */
#define PC_WAL_CTRL_MAGIC 0x50435743u          /* "PCWC" */
#define PC_WAL_CTRL_FMT   1u
#define CTRL_HDR  24
#define CTRL_ENT  24
#define CTRL_MAX  (CTRL_HDR + CTRL_ENT * 256)

static void ctrl_path(char *out, size_t n, const char *dir)
{
	snprintf(out, n, "%s/CONTROL", dir);
}

static int ctrl_build(unsigned char *b, int nseg, unsigned long long seq,
		const uint32_t *gen, const unsigned long long *first,
		const unsigned long long *last)
{
	int i, len = CTRL_HDR + CTRL_ENT * nseg;

	memset(b, 0, (size_t)len);
	w32(b, PC_WAL_CTRL_MAGIC);
	w32(b + 4, PC_WAL_CTRL_FMT);
	w32(b + 12, (uint32_t)nseg);
	w64(b + 16, seq);
	for (i = 0; i < nseg; i++) {
		unsigned char *e = b + CTRL_HDR + CTRL_ENT * i;

		w32(e, gen ? gen[i] : 0);
		w64(e + 8, first ? first[i] : 0);
		w64(e + 16, last ? last[i] : 0);
	}
	w32(b + 8, pc_crc32c(0, b + 12, (size_t)(len - 12)));
	return len;
}

/* the WAL's own state, written where the rotation already syncs */
static void ctrl_write(void)
{
	unsigned char b[CTRL_MAX];
	int len;

	if (W.ctrl_fd < 0 || W.nseg > 256)
		return;
	len = ctrl_build(b, W.nseg, __atomic_load_n(&W.seq, __ATOMIC_ACQUIRE),
		W.seg_gen, W.seg_first, W.seg_last);
	if (pwrite(W.ctrl_fd, b, (size_t)len, 0) != (ssize_t)len) {
		LM_ERR("wal: CONTROL write failed (%s) - the recycling rule "
			"will not be enforced after the next restart\n",
			strerror(errno));
		return;
	}
	fdatasync(W.ctrl_fd);
}

/* @nseg entries are filled only when the file agrees about the count;
 * returns the recorded sequence, or 0 for absent/torn/mismatched */
static unsigned long long ctrl_read(const char *dir, int nseg, uint32_t *gen,
		unsigned long long *first, unsigned long long *last)
{
	unsigned char b[CTRL_MAX];
	char path[512];
	unsigned long long seq;
	int i, fd, len, want = CTRL_HDR + CTRL_ENT * (nseg > 0 ? nseg : 0);

	if (nseg <= 0 || nseg > 256)
		return 0;
	ctrl_path(path, sizeof path, dir);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	len = (int)read(fd, b, sizeof b);
	close(fd);
	if (len < want || r32(b) != PC_WAL_CTRL_MAGIC ||
	        r32(b + 4) != PC_WAL_CTRL_FMT || (int)r32(b + 12) != nseg)
		return 0;
	if (pc_crc32c(0, b + 12, (size_t)(want - 12)) != r32(b + 8)) {
		LM_WARN("wal: CONTROL is torn - treating every segment as "
			"un-snapshotted (the safe direction)\n");
		return 0;
	}
	seq = r64(b + 16);
	for (i = 0; i < nseg; i++) {
		const unsigned char *e = b + CTRL_HDR + CTRL_ENT * i;

		if (gen)   gen[i]   = r32(e);
		if (first) first[i] = r64(e + 8);
		if (last)  last[i]  = r64(e + 16);
	}
	return seq;
}

/* Read the sequence before the WAL exists.  The post-recovery
 * checkpoint records "the WAL sequence at walk start" as its marker,
 * and it runs BEFORE pc_wal_init(), so without this it stamps 0 - which
 * a slow-storage run showed in 11 of 12 snapshots, making all 12
 * restarts replay the whole WAL. */
void pc_wal_fsync_observed(struct pc_wal_fsync_obs *out)
{
	const struct pc_wal_probe *pp = pc_wal_probe_result();

	memset(out, 0, sizeof *out);
	out->fsync_n = W.fsync_n;
	out->max_us = W.fsync_us_max;
	out->avg_us = W.fsync_n ? W.fsync_us_sum / W.fsync_n : 0;
	out->recent_us = W.fsync_ewma_us;
	out->probe_p50_us = pp && pp->valid ?
		(unsigned long long)pp->fsync_p50_us : 0;
	out->probe_underestimated = out->probe_p50_us && out->fsync_n >= 20 &&
		out->recent_us >= out->probe_p50_us * 4;
}

void pc_wal_preload_seq(const char *dir, int segments)
{
	unsigned long long s = ctrl_read(dir, segments, NULL, NULL, NULL);

	if (s > W.seq)
		W.seq = s;
}

/* CONTROL is only rewritten where a sync is already being paid - at
 * activation, at rotation and at shutdown - so between those points it
 * is a LOWER BOUND, exactly like PostgreSQL's pg_control between
 * checkpoints.  The replay is what moves it forward: RocksDB likewise
 * recovers its sequence by reading the log, not from the MANIFEST.
 * Call with the highest sequence recovery saw, before the checkpoint. */
void pc_wal_seq_atleast(unsigned long long seq)
{
	if (seq > W.seq)
		W.seq = seq;
}

/* records in segment @i postdate the last completed snapshot (@last =
 * its last seq, or the live high-water for the open segment) */
static int seg_hot(int i, unsigned long long marker)
{
	unsigned long long last = W.seg_last[i] ? W.seg_last[i]
		: W.pending_hi;

	return W.seg_first[i] > 0 && last >= W.seg_first[i] &&
		last > marker;
}

static int seg_rotate(void)
{
	int idx;

	/* fsync what the old generation holds before moving on - counted
	 * like every other, a rotation sync costs what the device costs */
	wal_fdatasync_timed(wall_now_us());

	idx = (int)((W.gen + 1) % (uint32_t)W.nseg);
	W.seg_last[(int)(W.gen % (uint32_t)W.nseg)] = W.pending_hi;
	if (W.gen + 1 >= (uint32_t)W.nseg) {
		int have_rdb = pc_rdb_dir()[0] != 0;
		unsigned long long marker = pc_rdb_safe_marker();

		W.recycles++;
		if (have_rdb && seg_hot(idx, marker)) {
			/* FULL: every segment holds un-snapshotted records -
			 * this overwrite makes seqs %llu..%llu unreplayable */
			W.overruns++;
			LM_CRIT("wal: FULL - no free segment: overwriting "
				"segment %d (seqs %llu..%llu, NEWER than the "
				"last completed snapshot marker %llu); those "
				"records are no longer replayable, so this "
				"node is FAILED.  It stays a member and goes "
				"on answering reads, but refuses writes and "
				"no client will select it for new work, and "
				"it stays FAILED until restarted.  Increase "
				"segments/segment_mb, or snapshot more often "
				"(a snapshot was just requested)\n",
				idx, W.seg_first[idx], W.seg_last[idx], marker);
			/* an already-running save returns 1 - that IS the
			 * goal, so the return is honestly ignorable */
			pc_rdb_request_save();
			/* Same event as a ring drop: acknowledged writes that
			 * will not be there after a restart.  The decree in
			 * DESIGN section 7 - overwrite and count it - predates
			 * the rule that a drop FAILS the node, and kept the
			 * struggling node in service destroying data while
			 * healthy peers waited on it. */
			wal_shed_once();
		} else if (!have_rdb) {
			LM_INFO("wal: recycling segment %d for generation %u "
				"(wal-only mode: the replay window IS the "
				"ring)\n", idx, W.gen + 1);
		}
		if (have_rdb) {
			int i, hot = 0;

			for (i = 0; i < W.nseg; i++)
				if (seg_hot(i, marker))
					hot++;
			W.free_segs = W.nseg - hot;
			if (W.free_segs * 4 <= W.nseg) {
				if (!W.pressure_warned) {
					W.pressure_warned = 1;
					LM_WARN("wal: close to FULL - only %d "
						"of %d segments are free "
						"(records above snapshot marker "
						"%llu are not reclaimable); "
						"requesting a snapshot now\n",
						W.free_segs, W.nseg, marker);
					/* as above: already-running = done */
					pc_rdb_request_save();
				}
			} else if (W.free_segs * 2 >= W.nseg) {
				W.pressure_warned = 0;
			}
		}
	}
	return seg_activate(idx, W.gen + 1);
}

/* ---- init -------------------------------------------------------------- */

int pc_wal_init(const char *dir, enum pc_wal_fsync mode, int segment_mb,
		int segments, int ring_kb, int nthreads)
{
	struct stat st;
	char path[512];
	int i, start_idx = 0;
	uint32_t best_gen = 0;

	crc_init_tab();
	crc_fn = hw_ok() ? crc32c_hw : crc32c_sw;
	LM_INFO("wal: crc32c %s\n", crc_fn == crc32c_sw ? "software" :
		"hardware");

	memset(&W.rings, 0, sizeof W.rings);
	snprintf(W.dir, sizeof W.dir, "%s", dir);
	W.mode = mode;
	W.seg_bytes = (long long)segment_mb << 20;
	W.nseg = segments;
	W.seg_first = calloc((size_t)segments, sizeof *W.seg_first);
	W.seg_last = calloc((size_t)segments, sizeof *W.seg_last);
	W.seg_gen = calloc((size_t)segments, sizeof *W.seg_gen);
	if (!W.seg_first || !W.seg_last || !W.seg_gen)
		return -1;
	W.free_segs = segments;
	W.fd = -1;
	W.ctrl_fd = -1;
	/* resume, do not restart: the sequence is what every marker and
	 * every span is expressed in, so numbering from 1 again after a
	 * restart makes all of them incomparable */
	{
		unsigned long long c = ctrl_read(dir, segments, W.seg_gen,
			W.seg_first, W.seg_last);

		if (c > W.seq)          /* never below what the replay saw */
			W.seq = c;
	}
	W.pending_hi = W.synced_seq = W.seq;

	if (nthreads > MAX_THREADS)
		return -1;
	for (i = 0; i < nthreads; i++) {
		W.rings[i].size = (uint32_t)ring_kb << 10;
		W.rings[i].buf = malloc(W.rings[i].size);
		if (!W.rings[i].buf)
			return -1;
	}
	W.nrings = nthreads;

	{
		char cpath[512];

		ctrl_path(cpath, sizeof cpath, dir);
		W.ctrl_fd = open(cpath, O_RDWR | O_CREAT, 0640);
		if (W.ctrl_fd < 0)
			LM_WARN("wal: cannot open %s (%s) - the recycling rule "
				"will not survive a restart\n", cpath,
				strerror(errno));
	}

	/* Is there room, BEFORE writing a single byte?  Provisioning is
	 * real zero writes (fallocate does not commit thin/sparse/COW
	 * backends), so without this the check IS the write: the daemon
	 * fills the filesystem, then discovers it cannot finish.  Count
	 * only the segments actually missing - a restart re-provisions
	 * nothing - and ask for a little headroom, because the rdb shares
	 * this volume and a WAL that exactly fits leaves no room for the
	 * snapshot that makes it recyclable. */
	{
		struct statvfs vfs;
		long long need = 0;

		for (i = 0; i < segments; i++) {
			seg_path(path, sizeof path, i);
			if (stat(path, &st) != 0 || st.st_size != W.seg_bytes)
				need += W.seg_bytes;
		}
		need += need / 10;             /* 10% for the rdb beside it */
		if (need > 0 && statvfs(dir, &vfs) == 0) {
			long long avail = (long long)vfs.f_bavail *
				(long long)vfs.f_frsize;

			if (avail < need) {
				LM_ERR("wal: %s has %lld MB free but this "
					"configuration needs %lld MB (%d x %d MB "
					"of segments to provision, plus 10%% for "
					"the snapshot beside them).  Refusing to "
					"start rather than filling the "
					"filesystem: lower [wal] segments or "
					"segment_mb, or give the WAL its own "
					"volume.\n", dir, avail >> 20,
					need >> 20, segments, segment_mb);
				return -1;
			}
		}
	}

	/* provision missing segments; find the highest existing gen so a
	 * restart continues the generation sequence (replay is S15's) */
	for (i = 0; i < segments; i++) {
		seg_path(path, sizeof path, i);
		if (stat(path, &st) != 0 || st.st_size != W.seg_bytes) {
			LM_INFO("wal: provisioning segment %d (%d MB, zero-write)\n",
				i, segment_mb);
			if (seg_provision(i) != 0) {
				LM_ERR("wal: cannot provision %s (%s)\n", path,
					strerror(errno));
				return -1;
			}
		} else {
			unsigned char h[PC_WAL_HDR];
			int fd = open(path, O_RDONLY);

			if (fd >= 0) {
				if (read(fd, h, sizeof h) == (ssize_t)sizeof h &&
				        r32(h) == PC_WAL_MAGIC && r32(h + 4) >= best_gen) {
					best_gen = r32(h + 4);
					start_idx = i;
				}
				close(fd);
			}
		}
	}
	(void)start_idx;
	{
		int idx = (int)((best_gen + 1) % (uint32_t)segments);

		/* Until CONTROL existed this path could not ask: the spans
		 * were per-process, so on a restart it claimed the next slot
		 * round-robin and the fifth start overwrote what the first
		 * had written.  It was safe only because daemon.c checkpoints
		 * before calling us - an invariant held by call order rather
		 * than by the rule.  Now it is held by the rule. */
		if (pc_rdb_dir()[0] != 0 && seg_hot(idx, pc_rdb_safe_marker())) {
			W.overruns++;
			LM_CRIT("wal: FULL at startup - segment %d still holds "
				"seqs %llu..%llu, NEWER than the last completed "
				"snapshot marker %llu; activating it makes them "
				"unreplayable.  Increase segments/segment_mb, or "
				"snapshot more often\n", idx, W.seg_first[idx],
				W.seg_last[idx], pc_rdb_safe_marker());
			pc_rdb_request_save();
			/* before the cluster plane exists - pc_wal_on_shed()
			 * delivers it when cluster init installs the callback */
			wal_shed_once();
		}
		if (seg_activate(idx, best_gen + 1) != 0) {
			LM_ERR("wal: cannot activate a segment\n");
			return -1;
		}
	}
	W.enabled = 1;
	W.last_fsync_us = wall_now_us();
	LM_NOTICE("wal: active in %s - %d x %d MB segments, generation %u, "
		"fsync %s\n", dir, segments, segment_mb, W.gen,
		mode == PC_WFSYNC_ALWAYS ? "always" :
		mode == PC_WFSYNC_NO ? "no" : "everysec");
	return 0;
}

void pc_wal_set_wakeup(int efd)
{
	W.wake_efd = efd;
}

void pc_wal_mark_sleeping(int s)
{
	__atomic_store_n(&W.sleeping, s, __ATOMIC_RELEASE);
}

/* ---- producers --------------------------------------------------------- */

static __thread unsigned char *scratch;

static void produce(int type, const char *col, const char *key, int klen,
		const char *val, int vlen, unsigned int expires_ticks,
		unsigned long long ver)
{
	const int hasver = (type == PC_WAL_T_UPSERT_V ||
		type == PC_WAL_T_TOUCH_V);
	struct ring *r;
	uint64_t expires_wall = 0, one = 1;
	uint32_t need, pl, pos;
	int collen = (int)strlen(col);

	if (!W.enabled)
		return;
	{
		int w = pc_worker_id();

		if (w < 0 || w >= W.nrings)
			return;
		r = &W.rings[w];
	}

	if (expires_ticks) {
		unsigned int nowt = get_ticks();

		expires_wall = (uint64_t)time(NULL) +
			(expires_ticks > nowt ? expires_ticks - nowt : 0);
	}

	pl = 1 + 1 + 2 + 4 + 8 + 8 + (hasver ? 8u : 0u) +
		(uint32_t)collen + (uint32_t)klen +
		(uint32_t)(vlen > 0 ? vlen : 0);
	need = PC_WAL_RECHDR + pl;
	if (need > MAX_REC) {
		r->dropped++;
		return;
	}
	if (!scratch) {
		scratch = malloc(MAX_REC);
		if (!scratch)
			return;
	}

	{
		unsigned char *p = scratch;
		uint64_t seq = __atomic_fetch_add(&W.seq, 1,
			__ATOMIC_RELAXED) + 1;

		w32(p, pl);                        /* len */
		w32(p + 4, 0);                     /* crc: the WAL thread stamps */
		w32(p + 8, 0);                     /* gen: the WAL thread stamps */
		p += PC_WAL_RECHDR;
		*p++ = (unsigned char)type;
		*p++ = (unsigned char)collen;
		w16(p, (uint16_t)klen);
		p += 2;
		w32(p, (uint32_t)(vlen > 0 ? vlen : 0));
		p += 4;
		w64(p, expires_wall);
		p += 8;
		w64(p, seq);
		p += 8;
		if (hasver) {
			w64(p, ver);
			p += 8;
		}
		memcpy(p, col, (size_t)collen);
		p += collen;
		memcpy(p, key, (size_t)klen);
		p += klen;
		if (vlen > 0)
			memcpy(p, val, (size_t)vlen);
	}

	/* SPSC publish: fits? copy (with wrap), then release the tail */
	{
		uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
		uint32_t tail = r->tail;
		uint32_t free_ = r->size - (tail - head);

		if (free_ < need) {
			r->dropped++;              /* NEVER block a worker */
			return;
		}
		pos = tail % r->size;
		if (pos + need <= r->size) {
			memcpy(r->buf + pos, scratch, need);
		} else {
			uint32_t first = r->size - pos;

			memcpy(r->buf + pos, scratch, first);
			memcpy(r->buf, scratch + first, need - first);
		}
		__atomic_store_n(&r->tail, tail + need, __ATOMIC_RELEASE);
	}
	if (__atomic_load_n(&W.sleeping, __ATOMIC_ACQUIRE) && W.wake_efd > 0)
		if (write(W.wake_efd, &one, sizeof one) < 0) { /* best effort */ }
}

void pc_wal_upsert(const char *col, const char *key, int klen,
		const char *val, int vlen, unsigned int expires_ticks,
		unsigned long long ver)
{
	produce(PC_WAL_T_UPSERT_V, col, key, klen, val, vlen, expires_ticks,
		ver);
}

void pc_wal_del(const char *col, const char *key, int klen)
{
	produce(PC_WAL_T_DEL, col, key, klen, NULL, 0, 0, 0);
}

void pc_wal_touch(const char *col, const char *key, int klen,
		unsigned int expires_ticks, unsigned long long ver)
{
	produce(PC_WAL_T_TOUCH_V, col, key, klen, NULL, 0, expires_ticks,
		ver);
}

/* ---- the WAL thread pump ----------------------------------------------- */

struct batch_rec {
	unsigned char *bytes;             /* full record incl. header */
	uint32_t len;
	uint64_t seq;
};

static int cmp_seq(const void *a, const void *b)
{
	const struct batch_rec *x = a, *y = b;

	return x->seq < y->seq ? -1 : x->seq > y->seq;
}


/*
 * The limit IS the limit: the first acknowledged write this node drops
 * marks it FAILED.
 *
 * Two softer rules were tried and both were wrong.  "N seconds in a
 * row" is defeated by the shape it most needs to catch - a ring that
 * fills, drains and fills again never strings N together, so an
 * oscillating WAL sheds writes in bursts for ever and never trips.  A
 * window ("3 of the last 10") survives that, but it still spends two
 * seconds of acknowledged data buying tolerance for a fault that has
 * already happened.
 *
 * There is no acceptable rate of losing acknowledged writes.  One is
 * the contract broken.  A node that does it is finished until an
 * operator has looked at it - and with a probe-sized ring or
 * fsync = everysec it should never happen at all, so if it does, the
 * blip is the symptom and not the excuse.
 */

/* Set by the cluster layer so wal.c does not have to know about node
 * states; a build without a cluster simply keeps the default no-op and
 * still gets the CRIT. */
static void wal_shed_noop(void) { }
static void (*W_shed_cb)(void) = wal_shed_noop;
static void pc_wal_shed_cb(void) { W_shed_cb(); }

/* A ring drop and a segment overrun are the SAME event wearing two
 * costumes: an acknowledged write that will not be there after a
 * restart.  Both take the node out of service, once - the caller has
 * already said which one it was and why. */
static void wal_shed_once(void)
{
	if (W.shed)
		return;
	W.shed = 1;
	pc_wal_shed_cb();
}

void pc_wal_on_shed(void (*fn)(void))
{
	if (!fn)
		return;
	W_shed_cb = fn;
	/* The WAL can shed before the cluster plane exists: a startup
	 * overrun is found inside pc_wal_init(), and pc_cluster_init()
	 * installs this callback afterwards.  Deliver it now rather than
	 * letting the no-op swallow the one event that must not be. */
	if (W.shed)
		fn();
}

static int append_all(const unsigned char *p, size_t n)
{
	while (n) {
		ssize_t w = pwrite(W.fd, p, n, W.off);

		if (w <= 0)
			return -1;
		W.off += w;
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

int pc_wal_pump(void)
{
	static unsigned char *stage;
	static struct batch_rec recs[BATCH_MAX];
	size_t stage_len = 0, stage_cap = 4u << 20;
	int i, nrec = 0;
	long long now;

	if (!W.enabled)
		return 1000;
	if (!stage) {
		stage = malloc(stage_cap);
		if (!stage)
			return 1000;
	}

	/* drain every ring into the stage buffer */
	for (i = 0; i < W.nrings && nrec < BATCH_MAX; i++) {
		struct ring *r = &W.rings[i];
		uint32_t head = r->head;
		uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);

		while (head != tail && nrec < BATCH_MAX) {
			unsigned char hdr[PC_WAL_RECHDR];
			uint32_t pos = head % r->size, len, total, j;

			for (j = 0; j < PC_WAL_RECHDR; j++)
				hdr[j] = r->buf[(pos + j) % r->size];
			len = r32(hdr);
			total = PC_WAL_RECHDR + len;
			if (stage_len + total > stage_cap)
				break;
			for (j = 0; j < total; j++)
				stage[stage_len + j] = r->buf[(pos + j) % r->size];
			recs[nrec].bytes = stage + stage_len;
			recs[nrec].len = total;
			recs[nrec].seq = r64(stage + stage_len + PC_WAL_RECHDR +
				1 + 1 + 2 + 4 + 8);
			stage_len += total;
			nrec++;
			head += total;
		}
		__atomic_store_n(&r->head, head, __ATOMIC_RELEASE);
	}

	if (nrec) {
		qsort(recs, (size_t)nrec, sizeof recs[0], cmp_seq);
		for (i = 0; i < nrec; i++) {
			struct batch_rec *br = &recs[i];
			uint32_t crc;

			if (br->seq <= W.pending_hi && W.pending_hi)
				W.late++;              /* the documented inversion */
			else
				W.pending_hi = br->seq;

			/* rotate FIRST, stamp after: the gen echo must be the
			 * generation of the segment the record actually lands in
			 * (stamping before rotation fenced every rotated
			 * segment's first record as stale - caught by waltest) */
			if (W.off + br->len > W.seg_bytes) {
				if (seg_rotate() != 0) {
					LM_CRIT("wal: segment rotation failed - WAL "
						"stops\n");
					W.enabled = 0;
					return 1000;
				}
			}
			w32(br->bytes + 8, W.gen);
			crc = pc_crc32c(0, br->bytes + 8, br->len - 8);
			w32(br->bytes + 4, crc);
			if (append_all(br->bytes, br->len) != 0) {
				LM_CRIT("wal: append failed (%s) - WAL stops\n",
					strerror(errno));
				W.enabled = 0;
				return 1000;
			}
			W.appended++;
			W.bytes += br->len;
		}
	}

	now = wall_now_us();
	/* ~1 Hz: has the ring dropped anything since we last said so? */
	/* The probe promised a latency; the device is delivering another.
	 * Say it ONCE, with both numbers - an operator who sized fsync
	 * policy from the startup line has no other way to learn it was
	 * measuring a cache. */
	if (!W.probe_warned && W.fsync_n >= 20) {
		const struct pc_wal_probe *pp = pc_wal_probe_result();
		unsigned long long avg = W.fsync_ewma_us;

		if (pp && pp->valid && pp->fsync_p50_us > 0 &&
		        avg >= (unsigned long long)pp->fsync_p50_us * 4) {
			W.probe_warned = 1;
			LM_WARN("wal: fsync is costing %lluus recently over "
				"%llu calls (worst %lluus), but the startup "
				"probe measured p50 %lldus - it sampled a "
				"burst that a write-back cache absorbed, so "
				"any fsync policy chosen from it is optimistic "
				"by ~%llux.  Re-check with fio, or set the "
				"disk cache mode to none.\n",
				avg, W.fsync_n, W.fsync_us_max,
				pp->fsync_p50_us,
				avg / (unsigned long long)pp->fsync_p50_us);
		}
	}
	if (now - W.drop_log_us >= 1000000) {
		unsigned long long d = 0;

		for (i = 0; i < W.nrings; i++)
			d += W.rings[i].dropped;
		if (d > W.dropped_reported) {
			LM_ERR("wal: DROPPED %llu acknowledged write(s) "
				"(%llu total) - the ring filled faster than "
				"it could be drained, so these records were "
				"answered OK to the client and are NOT in "
				"the WAL.  They will be missing after a "
				"restart.  Raise [wal] ring_kb, or move to "
				"fsync = everysec.\n",
				d - W.dropped_reported, d);
			W.dropped_reported = d;
			W.dropped_any = 1;
		}
		W.drop_log_us = now;

		/*
		 * SUSTAINED dropping takes the node OUT OF SERVICE.
		 *
		 * A full ring is not a hiccup that drains itself.  If
		 * arrivals outpace the pump it stays full, every write is
		 * acknowledged and discarded, and the node goes on
		 * advertising itself as healthy - it will sit there losing
		 * data for as long as the load lasts.
		 *
		 * Refusing each write instead (-TRYAGAIN) does not fix that
		 * and can make it worse: the node stays in the client map,
		 * every refusal still costs a parse and a dispatch, and a
		 * retrying client raises the arrival rate exactly when it
		 * needs to fall.  That is an avalanche, not backpressure.
		 *
		 * Leaving the map is what actually sheds load:
		 * pc_clsel_eligible() selects READY nodes only, so peers and
		 * clients route to healthy nodes and the arrival rate here
		 * collapses to nothing.  Reads already served from memory
		 * stay correct; what stops is being handed NEW writes this
		 * node cannot honour.
		 *
		 * FAILED is terminal by design (pc_node_state_set refuses
		 * to leave it except to DRAINING), so this needs an operator:
		 * the device could not keep up, and coming back without
		 * changing ring_kb, the fsync mode or the offered load would
		 * only lose data again.  The FIRST dropped write trips it -
		 * the two softer rules that were tried are argued above.
		 */
		if (W.dropped_any && !W.shed) {
			LM_CRIT("wal: dropped %llu acknowledged write(s) - "
				"marking this node FAILED.  It stays a member "
				"and goes on answering reads, but refuses "
				"writes and no client will select it for new "
				"work, and it stays FAILED until restarted.  "
				"Raise [wal] ring_kb, or move to "
				"fsync = everysec.\n", d);
			wal_shed_once();
		}
	}
	if (W.mode == PC_WFSYNC_ALWAYS && nrec) { /* NOLINT(bugprone-branch-clone) */
		wal_fdatasync_timed(now);
		W.last_fsync_us = now;
		W.synced_seq = W.pending_hi;
	/* the arms differ by MODE, kept apart so each mode's comment
	 * sits on its own branch */
	/* NOLINTNEXTLINE(bugprone-branch-clone) */
	} else if (W.mode == PC_WFSYNC_EVERYSEC &&
	           now - W.last_fsync_us >= 1000000) {
		wal_fdatasync_timed(now);
		W.last_fsync_us = now;
		W.synced_seq = W.pending_hi;
	}

	/* a sync barrier: everything pumped so far goes to the platter
	 * NOW, whatever the fsync policy says */
	if (__atomic_load_n(&W.sync_req, __ATOMIC_ACQUIRE)) {
		wal_fdatasync_timed(now);
		W.last_fsync_us = now;
		W.synced_seq = W.pending_hi;
		__atomic_store_n(&W.sync_req, 0, __ATOMIC_RELEASE);
	}

	if (W.mode == PC_WFSYNC_ALWAYS)
		return nrec ? 0 : 20;          /* drain again promptly */
	return nrec ? 20 : 200;
}

static int rings_empty(void)
{
	int i;

	for (i = 0; i < W.nrings; i++)
		if (W.rings[i].head !=
		        __atomic_load_n(&W.rings[i].tail, __ATOMIC_ACQUIRE))
			return 0;
	return 1;
}

void pc_wal_shutdown(void)
{
	unsigned long long d = 0;
	int i, spins = 0;

	if (!W.enabled)
		return;
	while (!rings_empty() && spins++ < 1000)
		pc_wal_pump();
	pc_wal_pump();
	wal_fdatasync_timed(wall_now_us());
	W.synced_seq = W.pending_hi;
	/* AFTER the drain, or CONTROL records a sequence the shutdown was
	 * about to move: this is the one point where it can be exact
	 * rather than the lower bound it is between rotations.  Close the
	 * active segment's span first, for the same reason. */
	if (W.seg_last && W.nseg > 0)
		W.seg_last[(int)(W.gen % (uint32_t)W.nseg)] = W.pending_hi;
	ctrl_write();
	if (W.ctrl_fd >= 0) {
		close(W.ctrl_fd);
		W.ctrl_fd = -1;
	}
	for (i = 0; i < W.nrings; i++)
		d += W.rings[i].dropped;
	LM_NOTICE("wal: shutdown - %llu records, %llu bytes, %llu dropped, "
		"%llu late\n", W.appended, W.bytes, d, W.late);
}

/* the sync verb: block (bounded) until everything APPENDED so far is
 * fdatasync'd.  Worker-callable: sets the barrier flag, kicks the WAL
 * thread, and polls the synced watermark.  0 = synced, 1 = WAL off,
 * -1 = timeout. */
int pc_wal_sync(int timeout_ms)
{
	unsigned long long target;
	uint64_t one = 1;
	int waited = 0;

	if (!W.enabled)
		return 1;
	target = __atomic_load_n(&W.seq, __ATOMIC_ACQUIRE);
	if (timeout_ms <= 0)
		timeout_ms = 5000;
	while (waited < timeout_ms) {
		if (W.synced_seq >= target)
			return 0;
		__atomic_store_n(&W.sync_req, 1, __ATOMIC_RELEASE);
		if (__atomic_load_n(&W.sleeping, __ATOMIC_ACQUIRE) &&
		        W.wake_efd > 0)
			if (write(W.wake_efd, &one, sizeof one) < 0) { }
		usleep(2000);
		waited += 2;
	}
	return W.synced_seq >= target ? 0 : -1;
}

void pc_wal_get_stats(struct pc_wal_stats *out)
{
	unsigned long long d = 0;
	int i;

	memset(out, 0, sizeof *out);
	out->enabled = W.enabled;
	out->fsync_mode = W.mode == PC_WFSYNC_ALWAYS ? "always" :
		W.mode == PC_WFSYNC_NO ? "no" : "everysec";
	out->appended = W.appended;
	out->bytes = W.bytes;
	for (i = 0; i < W.nrings; i++)
		d += W.rings[i].dropped;
	out->dropped = d;
	out->late = W.late;
	out->overruns = W.overruns;
	out->free_segments = W.seg_first ? W.free_segs : 0;
	out->recycles = W.recycles;
	out->last_seq = W.seq;
	out->synced_seq = W.synced_seq;
}

/* ---- scan (tests, -W, S15 replay) -------------------------------------- */

struct seg_meta {
	int idx;
	uint32_t gen;
};

static int cmp_gen(const void *a, const void *b)
{
	const struct seg_meta *x = a, *y = b;

	return x->gen < y->gen ? -1 : x->gen > y->gen;
}

long pc_wal_scan(const char *dir, pc_wal_scan_cb cb, void *ctx,
		const char **why)
{
	struct seg_meta segs[256];
	char path[512];
	unsigned char h[PC_WAL_HDR], *buf = NULL;
	long count = 0;
	int i, n = 0, fd;

	crc_init_tab();
	if (!crc_fn)
		crc_fn = hw_ok() ? crc32c_hw : crc32c_sw;
	*why = "end";

	for (i = 0; i < 256; i++) {
		snprintf(path, sizeof path, "%s/wal-%03d.seg", dir, i);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		if (read(fd, h, sizeof h) == (ssize_t)sizeof h &&
		        r32(h) == PC_WAL_MAGIC) {
			segs[n].idx = i;
			segs[n].gen = r32(h + 4);
			n++;
		}
		close(fd);
	}
	if (!n)
		return 0;
	qsort(segs, (size_t)n, sizeof segs[0], cmp_gen);

	buf = malloc(MAX_REC + 16);
	if (!buf)
		return -1;
	for (i = 0; i < n; i++) {
		long long off = PC_WAL_HDR;

		snprintf(path, sizeof path, "%s/wal-%03d.seg", dir, segs[i].idx);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		for (;;) {
			unsigned char rh[PC_WAL_RECHDR];
			uint32_t len, crc, gen;

			if (pread(fd, rh, sizeof rh, off) != (ssize_t)sizeof rh)
				break;
			len = r32(rh);
			if (len == 0 || len > MAX_REC) {
				if (len != 0)
					*why = "torn";
				break;
			}
			crc = r32(rh + 4);
			gen = r32(rh + 8);
			if (gen != segs[i].gen) {
				*why = "stale-gen";
				break;
			}
			memcpy(buf, rh + 8, 4);        /* gen is crc-covered */
			if (pread(fd, buf + 4, len, off + PC_WAL_RECHDR) !=
			        (ssize_t)len) {
				*why = "torn";
				break;
			}
			if (pc_crc32c(0, buf, 4 + len) != crc) {
				*why = "torn";
				break;
			}
			{
				const unsigned char *p = buf + 4;
				int type = p[0], collen = p[1], klen = r16(p + 2);
				uint32_t vlen = r32(p + 4);
				uint64_t exp = r64(p + 8), seq = r64(p + 16);
				/* type 4 carries the version between seq and
				 * the strings; every older type does not, and
				 * still replays (numbered 0) */
				unsigned int hdr = (type == PC_WAL_T_UPSERT_V ||
					type == PC_WAL_T_TOUCH_V) ? 32u : 24u;
				uint64_t ver = hdr == 32u ? r64(p + 24) : 0;
				const char *col = (const char *)p + hdr;
				const char *key = col + collen;
				const char *val = key + klen;

				if (hdr + (uint32_t)(collen + klen) + vlen != len) {
					*why = "torn";
					break;
				}
				count++;
				if (cb && cb(type, col, collen, key, klen, val,
				        (int)vlen, exp, seq, ver, ctx) < 0) {
					close(fd);
					free(buf);
					return count;
				}
			}
			off += PC_WAL_RECHDR + len;
		}
		close(fd);
	}
	free(buf);
	return count;
}

/* ---- -W: human dump (also the test surface) ---------------------------- */

struct dump_ctx {
	long n;
	unsigned long long last_seq;
	int order_ok;
};

static int dump_cb(int type, const char *col, int collen, const char *key,
		int klen, const char *val, int vlen, unsigned long long exp,
		unsigned long long seq, unsigned long long ver, void *ctx)
{
	struct dump_ctx *d = ctx;
	static const char *tn[] = { "?", "upsert", "del", "touch", "upsert",
		"touch" };
	int isup = type == PC_WAL_T_UPSERT || type == PC_WAL_T_UPSERT_V;

	(void)val;
	if (seq <= d->last_seq && d->n)
		d->order_ok = 0;
	d->last_seq = seq;
	d->n++;
	/* ver= is printed only where the record actually carries one, so a
	 * pre-A1 log reads as silent about versions rather than as full of
	 * records versioned zero */
	printf("seq=%llu %s %.*s/%.*s vlen=%d exp=%llu", seq,
		tn[type >= 1 && type <= 5 ? type : 0], collen, col, klen, key,
		vlen, exp);
	if (type == PC_WAL_T_UPSERT_V || type == PC_WAL_T_TOUCH_V)
		printf(" ver=%llu", ver);
	printf("\n");
	if (isup && vlen <= 40)
		printf("   val=%.*s\n", vlen, val);
	return 0;
}

int pc_wal_dump(const char *dir)
{
	struct dump_ctx d = { 0, 0, 1 };
	const char *why = "?";
	long n = pc_wal_scan(dir, dump_cb, &d, &why);

	printf("wal-dump: %ld records, stop=%s, seq-order=%s\n", n, why,
		d.order_ok ? "ascending" : "VIOLATED");
	return n < 0 ? 1 : 0;
}
