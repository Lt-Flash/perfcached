/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* wal.c - the write-ahead log; the contract and on-disk format are in wal.h */
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
#include <pthread.h>                    /* the segment grower thread */

#include "compat/dprint.h"
#include "compat/pt.h"
#include "compat/timer.h"
#include "winctr.h"
#include "walprobe.h"
#include "wal.h"
#include "store.h"                      /* pc_store_durable_name */
#include "walring.h"
#include "daemon.h"                       /* pc_worker_id() */
#include "rdb.h"                        /* safe marker + save request */
#include "core/pcache_arena.h"          /* PCACHE_CELL_MAX: the record ceiling */
#include "core/pcache_htable.h"         /* pcache_last_del_ver */
#include "resident.h"
#include <sys/mman.h>                   /* mlock */
#include "walfmt.h"                     /* the record ceiling, LE fields */

#define MAX_THREADS 520
#define BATCH_MAX   4096

/* ---- state ------------------------------------------------------------- */

struct ring {
	unsigned char *buf;
	uint32_t size;
	volatile uint32_t head, tail;     /* monotonically increasing */
	unsigned long long dropped;
	/* at most the seq its producer holds and has not yet put in it (0 = none) */
	unsigned long long busy;
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
	/* every seq at or below it was written, then synced, or dropped: what a sync waits for */
	unsigned long long synced_low;
	/* the lowest and highest seq a full ring dropped - never cleared, read
	 * against the last snapshot's marker */
	unsigned long long drop_lo, drop_hi;
	/* a segment is free once its records sit at or below the last COMPLETED
	 * snapshot marker; FULL = the pump must overwrite one that is not */
	unsigned long long *seg_first, *seg_last;  /* per segment, by idx */
	uint32_t *seg_gen;
	/* CONTROL keeps the spans above across a restart (like pg_control),
	 * written only at activation and rotation - never on the append path */
	int ctrl_fd;
	/* observed fsync cost: the pump writes, stats reads - plain longs, as a
	 * torn read only reports a slightly stale mean */
	unsigned long long fsync_n, fsync_us_sum, fsync_us_max;
	unsigned long long fsync_ewma_us;   /* recent cost, 1/8 weight */
	struct pc_winmax fsync_wmax;        /* worst of the last 5 min */
	int probe_warned;
	unsigned long long overruns;
	int free_segs;
	int pressure_warned;
	/* fill rate: an EWMA of the seconds one segment takes, rotation to
	 * rotation (0 = unmeasured), and whether the rate trigger said so */
	long long rot_last_us;
	double seg_ewma_s;
	int rate_warned;
	long long sizing_warned_us;        /* last "cannot keep up" warning */
	int dropped_any;               /* has this WAL ever dropped? */
	int shed;                      /* already taken out of service */
	/* healing - see wal_heal_step() */
	int healing, heal_gave_up;
	long long heal_t0_us, heal_req_us, last_heal_ms;
	unsigned long long heal_mark, heal_err0, heals;
	/* when the last HEAL_RING heals ended (monotonic ms; a ring, so
	 * heals_recent counts to 64), and when routine healing was said */
	long long heal_end_ms[64];
	long long heal_routine_said_ms;
	int failed;                    /* the device refused a write or sync:
	                                * sticky; no append or sync after it */
	int failed_errno;
	unsigned long long ctrl_errors;
	/* ring drops are logged by the pump, not at the drop site: that is the
	 * worker's hot path, and logging per drop would deepen the overload */
	unsigned long long dropped_reported;
	long long drop_log_us;
	/* the pump's stage buffer, made at init with the rings so both are
	 * resident (and, with the arena, locked) from the start */
	unsigned char *stage;
	size_t stage_cap;
	int pin;                          /* lock the buffers (arena is) */
	unsigned long long locked_bytes;  /* 0 = not locked */
	int lock_errno;
	/* fsync = always waits on a full ring instead of dropping */
	int pump_w, pump_gone;            /* the pump's ring; drain over */
	unsigned long long ring_waits, ring_wait_us;
	/* on_full = heal drops, then heals; workers' slots (0 = every thread)
	 * wait unbounded; the pump's wait for a segment: start, last ask */
	int on_full_heal, work_lo, work_hi, full_gave_up, full_hot;
	long long full_t0_us, full_req_us;
	unsigned long long full_waits, full_wait_us;
	/* the ring grows nseg_min..nseg_max: grow_main provisions a slot off
	 * the pump, adopted at its next pass (grow_req/grow_done = slot + 1) */
	int nseg_min, nseg_max, cur;
	int grow_req, grow_done, grow_err, grow_stop, grow_thr_on;
	long long grow_after_us, low_since_us, shrink_us;
	unsigned long long grown, retired;
	pthread_t grow_thr;
} W;

#define WAL_SEG_CAP 256                   /* CONTROL and the reader stop here */

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

/* provision one segment with real zero writes: fallocate does not
 * provision thin/sparse/COW backends, a write does */
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
			/* leave nothing behind: on ENOSPC a partial segment
			 * would keep its space after the refused start */
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
static void ctrl_write(void);

static void wal_shed_once(int why);
static void wal_storage_failed(const char *what, int err);
static int wal_can_heal(void);

/* every fdatasync goes through here: it stamps the marker the hang
 * watchdog reads and records the cost under real load, errno kept */
static int wal_fdatasync_timed(long long now)
{
	long long dur;
	int rc, err;

	pc_wal_fsync_start_us = now;
	rc = fdatasync(W.fd);
	err = errno;
	dur = wall_now_us() - now;
	pc_wal_fsync_start_us = 0;
	if (dur < 0)
		dur = 0;
	W.fsync_n++;
	W.fsync_us_sum += (unsigned long long)dur;
	if ((unsigned long long)dur > W.fsync_us_max)
		W.fsync_us_max = (unsigned long long)dur;
	pc_winmax_note(&W.fsync_wmax, get_ticks(), (unsigned long long)dur);
	/* EWMA, 1/8 weight: what fsync costs now - the lifetime mean cannot
	 * notice a device degrading */
	W.fsync_ewma_us = W.fsync_ewma_us ?
		(W.fsync_ewma_us * 7 + (unsigned long long)dur) / 8 :
		(unsigned long long)dur;
	errno = err;
	return rc;
}

/* the lowest seq not yet written - held by a producer or still in a ring; every number below it
 * is written or dropped */
static unsigned long long seq_low(void)
{
	unsigned long long lo = __atomic_load_n(&W.seq, __ATOMIC_SEQ_CST) + 1;
	int i;

	for (i = 0; i < W.nrings; i++) {
		struct ring *r = &W.rings[i];
		unsigned long long b = __atomic_load_n(&r->busy, __ATOMIC_SEQ_CST);
		uint32_t head = r->head;

		if (b && b < lo)
			lo = b;
		if (head != __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE)) {
			unsigned char sq[8];
			unsigned long long s;

			walring_copy(r->buf, r->size, (walring_pos(head, r->size) +
				PC_WAL_RECHDR + 1 + 1 + 2 + 4 + 8) % r->size, sq, 8);
			s = r64(sq);
			if (s < lo)
				lo = s;
		}
	}
	return lo;
}

/* the only place the durable watermark moves, on the device's yes (not
 * for @claim 0, the rotation's); a refusal fails the node, never retried */
static int wal_sync_commit(long long now, int claim)
{
	unsigned long long low;

	if (W.failed)
		return -1;
	/* before the sync: what it covers is what was written by now */
	low = claim ? seq_low() - 1 : 0;
	if (wal_fdatasync_timed(now) != 0) {
		wal_storage_failed("fdatasync", errno);
		return -1;
	}
	if (claim) {
		W.last_fsync_us = now;
		/* the highest written, but not past a lower number still unwritten */
		W.synced_seq = low < W.pending_hi ? low : W.pending_hi;
		__atomic_store_n(&W.synced_low, low, __ATOMIC_RELEASE);
	}
	return 0;
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
	W.cur = idx;
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
/* single pwrite, CRC-checked: a torn or absent CONTROL reads as unknown,
 * so every segment is judged hot - the conservative direction */
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
		unsigned char *e = b + CTRL_HDR + (size_t)CTRL_ENT * (size_t)i;

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

	if (W.ctrl_fd < 0 || W.nseg > WAL_SEG_CAP)
		return;
	len = ctrl_build(b, W.nseg, __atomic_load_n(&W.seq, __ATOMIC_ACQUIRE),
		W.seg_gen, W.seg_first, W.seg_last);
	if (pwrite(W.ctrl_fd, b, (size_t)len, 0) != (ssize_t)len) {
		W.ctrl_errors++;
		LM_ERR("wal: CONTROL write failed (%s) - the recycling rule "
			"will not be enforced after the next restart\n",
			strerror(errno));
		return;
	}
	/* not a node failure: CONTROL is rewritten whole, so the next sync
	 * covers all of it, and a torn one only sends recovery to the scan */
	if (fdatasync(W.ctrl_fd) != 0) {
		W.ctrl_errors++;
		LM_ERR("wal: CONTROL sync failed (%s) - the recycling rule "
			"may not be enforced after the next restart; the "
			"next rotation writes it again\n", strerror(errno));
	}
}

/* @gen/@first/@last hold WAL_SEG_CAP entries, *@n gets the recorded count;
 * returns the recorded sequence, or 0 for absent/torn */
static unsigned long long ctrl_read(const char *dir, int *n, uint32_t *gen,
		unsigned long long *first, unsigned long long *last)
{
	unsigned char b[CTRL_MAX];
	char path[512];
	unsigned long long seq;
	int i, fd, len, want, nseg;

	*n = 0;
	ctrl_path(path, sizeof path, dir);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	len = (int)read(fd, b, sizeof b);
	close(fd);
	if (len < CTRL_HDR || r32(b) != PC_WAL_CTRL_MAGIC ||
	        r32(b + 4) != PC_WAL_CTRL_FMT)
		return 0;
	nseg = (int)r32(b + 12);
	want = CTRL_HDR + CTRL_ENT * nseg;
	if (nseg < 1 || nseg > WAL_SEG_CAP || len < want)
		return 0;
	if (pc_crc32c(0, b + 12, (size_t)(want - 12)) != r32(b + 8)) {
		LM_WARN("wal: CONTROL is torn - treating every segment as "
			"un-snapshotted (the safe direction)\n");
		return 0;
	}
	seq = r64(b + 16);
	for (i = 0; i < nseg; i++) {
		const unsigned char *e = b + CTRL_HDR + (size_t)CTRL_ENT * (size_t)i;

		if (gen)   gen[i]   = r32(e);
		if (first) first[i] = r64(e + 8);
		if (last)  last[i]  = r64(e + 16);
	}
	*n = nseg;
	return seq;
}

void pc_wal_fsync_observed(struct pc_wal_fsync_obs *out)
{
	const struct pc_wal_probe *pp = pc_wal_probe_result();

	memset(out, 0, sizeof *out);
	out->fsync_n = W.fsync_n;
	out->max_us = W.fsync_us_max;
	out->sum_us = W.fsync_us_sum;
	out->win_max_us = pc_winmax_get(&W.fsync_wmax, get_ticks());
	out->avg_us = W.fsync_n ? W.fsync_us_sum / W.fsync_n : 0;
	out->recent_us = W.fsync_ewma_us;
	out->probe_p50_us = pp && pp->valid ?
		(unsigned long long)pp->fsync_p50_us : 0;
	out->probe_underestimated = out->probe_p50_us && out->fsync_n >= 20 &&
		out->recent_us >= out->probe_p50_us * 4;
}

/* the post-recovery checkpoint runs before pc_wal_init() and stamps the
 * WAL sequence as its marker, so the sequence is read from CONTROL first */
void pc_wal_preload_seq(const char *dir, int segments)
{
	int n;
	unsigned long long s = ctrl_read(dir, &n, NULL, NULL, NULL);

	(void)segments;                    /* CONTROL's count rules */
	if (s > W.seq)
		W.seq = s;
}

/* CONTROL's sequence is a lower bound between rotations; the replay moves
 * it forward - call with the highest seq recovery saw, before the checkpoint */
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

/* the slot to activate next: the oldest generation other than the active
 * slot, a never-used one (gen 0) first - in a fixed ring, (gen + 1) % nseg */
static int next_slot(void)
{
	int i, best = -1;

	for (i = 0; i < W.nseg; i++) {
		if (i == W.cur && W.fd >= 0)
			continue;
		if (best < 0 || W.seg_gen[i] < W.seg_gen[best])
			best = i;
	}
	return best < 0 ? 0 : best;
}

/* a slot is provisioned here, off the pump - zero-writing one takes
 * seconds on a disk, and the pump must keep draining */
static void *grow_main(void *a)
{
	struct timespec ts = { 0, 50000000 };

	(void)a;
	while (!__atomic_load_n(&W.grow_stop, __ATOMIC_ACQUIRE)) {
		int want = __atomic_load_n(&W.grow_req, __ATOMIC_ACQUIRE);

		if (want && __atomic_load_n(&W.grow_done, __ATOMIC_ACQUIRE) != want) {
			int rc = seg_provision(want - 1);

			__atomic_store_n(&W.grow_err, rc ? (errno ? errno : EIO) : 0,
				__ATOMIC_RELAXED);
			__atomic_store_n(&W.grow_done, want, __ATOMIC_RELEASE);
			continue;
		}
		nanosleep(&ts, NULL);
	}
	return NULL;
}

/* at a rotation, ask for a slot when the log is half full or its free
 * segments last under three snapshots; never past the cap or the disk's room */
static void grow_check(void)
{
	struct pc_rdb_stats rs;
	struct statvfs vfs;
	long long now = wall_now_us();
	int short_ = W.free_segs * 2 <= W.nseg;

	if (!W.grow_thr_on || W.nseg >= W.nseg_max || now < W.grow_after_us ||
	    __atomic_load_n(&W.grow_req, __ATOMIC_ACQUIRE))
		return;
	if (!short_ && W.seg_ewma_s > 0) {
		pc_rdb_get_stats(&rs);
		short_ = rs.last_dur_ms > 0 && (double)W.free_segs *
			W.seg_ewma_s * 1000 < 3.0 * (double)rs.last_dur_ms;
	}
	if (!short_)
		return;
	if (statvfs(W.dir, &vfs) == 0 &&
	    (long long)vfs.f_bavail * (long long)vfs.f_frsize <
	        W.seg_bytes + (long long)(W.nseg + 1) * W.seg_bytes / 10) {
		LM_WARN("wal: the log is running short (%d of %d segments "
			"free) but %s has %lld MB free - not growing it; free "
			"space, or raise segments, segment_mb or the snapshot "
			"rate\n", W.free_segs, W.nseg, W.dir,
			((long long)vfs.f_bavail * (long long)vfs.f_frsize) >> 20);
		W.grow_after_us = now + 60000000LL;
		return;
	}
	__atomic_store_n(&W.grow_req, W.nseg + 1, __ATOMIC_RELEASE);
}

/* each pump pass: a slot grow_main has provisioned joins the ring (a new
 * slot's generation 0 makes it the next one used) */
static void grow_adopt(void)
{
	int want = __atomic_load_n(&W.grow_req, __ATOMIC_ACQUIRE), idx, err;

	if (!want || __atomic_load_n(&W.grow_done, __ATOMIC_ACQUIRE) != want)
		return;
	idx = want - 1;
	err = __atomic_load_n(&W.grow_err, __ATOMIC_RELAXED);
	if (err || idx != W.nseg) {
		LM_ERR("wal: could not add segment %d (%s) - the log stays at "
			"%d segment(s); trying again in a minute\n", idx,
			strerror(err ? err : EINVAL), W.nseg);
		W.grow_after_us = wall_now_us() + 60000000LL;
	} else {
		W.seg_first[idx] = W.seg_last[idx] = 0;
		W.seg_gen[idx] = 0;
		W.nseg = idx + 1;
		W.free_segs++;
		W.grown++;
		ctrl_write();
		LM_NOTICE("wal: grew to %d segments of %lld MB (%lld MB) - %d "
			"were free; the cap is %d\n", W.nseg, W.seg_bytes >> 20,
			((long long)W.nseg * W.seg_bytes) >> 20, W.free_segs - 1,
			W.nseg_max);
	}
	__atomic_store_n(&W.grow_req, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&W.grow_done, 0, __ATOMIC_RELEASE);
}

/* after shrink_us of quiet a cold slot goes, then one per 10 s, the top file
 * renamed onto it; a crash between is safe - recovery goes by headers */
static void shrink_check(long long now)
{
	unsigned long long marker;
	char path[512], tpath[512];
	int i, hot = 0, top, k = -1;

	if (W.nseg <= W.nseg_min || pc_rdb_dir()[0] == 0 ||
	    __atomic_load_n(&W.grow_req, __ATOMIC_ACQUIRE))
		return;
	marker = pc_rdb_safe_marker();
	for (i = 0; i < W.nseg; i++)
		if (seg_hot(i, marker))
			hot++;
	if (W.nseg <= W.nseg_max) {
		if (hot * 2 > W.nseg_min) {
			W.low_since_us = 0;
			return;
		}
		if (!W.low_since_us) {
			W.low_since_us = now;
			return;
		}
		if (now - W.low_since_us < W.shrink_us)
			return;
	}
	top = W.nseg - 1;
	for (i = top; i >= 0 && k < 0; i--)
		if (i != W.cur && !seg_hot(i, marker))
			k = i;
	if (k < 0)
		return;
	seg_path(path, sizeof path, k);
	if (unlink(path) != 0 && errno != ENOENT) {
		LM_WARN("wal: cannot retire %s (%s)\n", path, strerror(errno));
		W.low_since_us = now;
		return;
	}
	if (k != top) {
		seg_path(tpath, sizeof tpath, top);
		if (rename(tpath, path) != 0) {
			/* k is gone, the top stays: provision k again (it was
			 * cold), or a rotation onto it would find no file */
			LM_WARN("wal: cannot move %s onto %s (%s) - provisioning "
				"%s again\n", tpath, path, strerror(errno), path);
			if (seg_provision(k) != 0)
				LM_CRIT("wal: cannot provision %s again (%s) - a "
					"rotation onto it will fail\n", path,
					strerror(errno));
			W.seg_first[k] = W.seg_last[k] = 0;
			W.seg_gen[k] = 0;
			W.low_since_us = now;
			return;
		}
		W.seg_first[k] = W.seg_first[top];
		W.seg_last[k] = W.seg_last[top];
		W.seg_gen[k] = W.seg_gen[top];
		if (W.cur == top)
			W.cur = k;
	}
	W.seg_first[top] = W.seg_last[top] = 0;
	W.seg_gen[top] = 0;
	W.nseg--;
	W.free_segs = W.nseg - hot;
	W.retired++;
	W.low_since_us = now - W.shrink_us +
		(W.shrink_us < 10000000LL ? W.shrink_us : 10000000LL);
	ctrl_write();
	LM_NOTICE("wal: retired a segment - the log is back to %d segment(s) "
		"(the floor is %d) after a quiet spell\n", W.nseg, W.nseg_min);
}

static int seg_rotate(void)
{
	int idx;

	/* sync the old generation first; a refusal stops the rotation, as
	 * recycling behind it would lose what the device did not take */
	if (wal_sync_commit(wall_now_us(), 0) != 0)
		return -1;

	{
		/* one rotation = one segment filled, so the interval is
		 * the fill rate - smoothed, so a burst is not a trend */
		long long now = wall_now_us();

		if (W.rot_last_us) {
			double dt = (double)(now - W.rot_last_us) / 1e6;

			W.seg_ewma_s = W.seg_ewma_s > 0
				? 0.7 * W.seg_ewma_s + 0.3 * dt : dt;
		}
		W.rot_last_us = now;
	}
	W.seg_last[W.cur] = W.pending_hi;
	idx = next_slot();
	if (W.seg_gen[idx]) {                  /* a slot that held a generation */
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
				"node %s.  Increase segments/segment_mb, or "
				"snapshot more often (a snapshot was just "
				"requested)\n",
				idx, W.seg_first[idx], W.seg_last[idx], marker,
				wal_can_heal() ? "HEALS: it refuses writes and "
				"serves reads until a snapshot that began after "
				"this overwrite has landed, then returns to "
				"service by itself" : "is FAILED: it stays a "
				"member and goes on answering reads, but refuses "
				"writes and no client will select it for new work "
				"until restarted");
			/* an already-running save returns 1 - that IS the
			 * goal, so the return is honestly ignorable */
			pc_rdb_request_save();
			/* the same loss as a ring drop: acknowledged
			 * writes that a restart will not bring back */
			wal_shed_once(PC_WAL_SHED_LOSS);
		} else if (!have_rdb) {
			LM_INFO("wal: recycling segment %d for generation %u "
				"(wal-only mode: the replay window IS the "
				"ring)\n", idx, W.gen + 1);
		}
	}
	/* counted at every rotation, not only after a wrap, or the first lap
	 * overruns unwarned; a never-written segment is not hot */
	if (pc_rdb_dir()[0] != 0) {
		unsigned long long marker = pc_rdb_safe_marker();
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
		/* rate trigger: a snapshot frees only what predates its
		 * start; begin it while free space outlasts twice the last */
		if (W.seg_ewma_s > 0) {
			struct pc_rdb_stats rs;
			double full_in_ms = W.free_segs * W.seg_ewma_s * 1000;

			pc_rdb_get_stats(&rs);
			if (rs.last_dur_ms > 0 &&
			        full_in_ms < 2.0 * (double)rs.last_dur_ms) {
				if (!W.rate_warned) {
					W.rate_warned = 1;
					LM_WARN("wal: filling faster than a "
						"snapshot can keep up - %d free "
						"segment(s) last about %.1f s at "
						"the current rate, and the last "
						"snapshot took %.1f s; requesting "
						"one now, and again at every "
						"rotation while this holds\n",
						W.free_segs, full_in_ms / 1000,
						(double)rs.last_dur_ms / 1000);
				}
				pc_rdb_request_save();
			} else if (full_in_ms >= 4.0 * (double)rs.last_dur_ms) {
				W.rate_warned = 0;
			}
			/* sizing, not a trigger: a snapshot slower than the
			 * whole ring's fill time can never land in time */
			{
				double ring_ms = W.nseg * W.seg_ewma_s * 1000;
				long long now_us = wall_now_us();

				/* the first always: the clock counts from boot,
				 * so a 10 min gap from 0 hid it on new hosts */
				if (rs.last_dur_ms > 0 && (double)rs.last_dur_ms > ring_ms &&
				        (!W.sizing_warned_us ||
				         now_us - W.sizing_warned_us >= 600LL * 1000000)) {
					W.sizing_warned_us = now_us;
					LM_WARN("wal: snapshots cannot keep ahead of the "
						"WAL - the last took %.1f s, and the whole "
						"ring (%d segments) fills in %.1f s at the "
						"current rate, so the ring fills before any "
						"snapshot can land: %s until the WAL holds more "
						"([wal] segments or segment_mb), snapshots write "
						"faster (rdb_mb_s), or the offered write load "
						"falls\n",
						(double)rs.last_dur_ms / 1000, W.nseg,
						ring_ms / 1000, W.on_full_heal
						? "expect overruns and heals"
						: "writes will wait for snapshots (on_full = wait)");
				}
			}
		}
	}
	if (pc_rdb_dir()[0] != 0)
		grow_check();
	return seg_activate(idx, W.gen + 1);
}

/* ---- init -------------------------------------------------------------- */

int pc_wal_init(const char *dir, enum pc_wal_fsync mode, int segment_mb,
		int segments, int ring_kb, int nthreads, const int *ring_kb_of)
{
	struct stat st;
	char path[512];
	int i, start_idx = 0;
	uint32_t best_gen = 0;
	const char *crc_be = pc_crc32c_select();   /* before any thread hashes */

	LM_INFO("wal: crc32c %s\n", crc_be);

	memset(&W.rings, 0, sizeof W.rings);
	snprintf(W.dir, sizeof W.dir, "%s", dir);
	W.mode = mode;
	W.seg_bytes = (long long)segment_mb << 20;
	/* the slots for a ring that may grow to the cap */
	W.nseg_min = segments;
	if (W.nseg_max < segments)
		W.nseg_max = segments;
	if (W.nseg_max > WAL_SEG_CAP)
		W.nseg_max = WAL_SEG_CAP;
	W.seg_first = calloc(WAL_SEG_CAP, sizeof *W.seg_first);
	W.seg_last = calloc(WAL_SEG_CAP, sizeof *W.seg_last);
	W.seg_gen = calloc(WAL_SEG_CAP, sizeof *W.seg_gen);
	if (!W.seg_first || !W.seg_last || !W.seg_gen)
		return -1;
	W.cur = -1;
	W.fd = -1;
	W.ctrl_fd = -1;
	/* resume the sequence: every marker and span is in it, so numbering
	 * from 1 after a restart would make them incomparable */
	{
		int nrec;
		unsigned long long c = ctrl_read(dir, &nrec, W.seg_gen,
			W.seg_first, W.seg_last);

		if (c > W.seq)          /* never below what the replay saw */
			W.seq = c;
		/* a file past the recorded count with a header is a slot (stale
		 * CONTROL); one without is a cut-short provisioning, removed */
		W.nseg = segments > nrec ? segments : nrec;
		for (i = W.nseg; i < WAL_SEG_CAP; i++) {
			unsigned char h[PC_WAL_HDR];
			int fd, ok;

			seg_path(path, sizeof path, i);
			fd = open(path, O_RDONLY);
			if (fd < 0)
				continue;
			ok = read(fd, h, sizeof h) == (ssize_t)sizeof h &&
				r32(h) == PC_WAL_MAGIC;
			close(fd);
			if (ok) {
				W.nseg = i + 1;
			} else if (unlink(path) == 0) {
				LM_NOTICE("wal: removed %s - a segment whose "
					"provisioning was cut short\n", path);
			}
		}
		W.free_segs = W.nseg;
	}
	W.pending_hi = W.synced_seq = W.synced_low = W.seq;
	/* a span left OPEN (last 0) was active at a stop without a shutdown and
	 * nothing else closes it; the resumed seq bounds its records */
	{
		int open_n = 0;

		for (i = 0; i < W.nseg; i++) {
			if (!W.seg_first[i] || W.seg_last[i])
				continue;
			if (W.seq >= W.seg_first[i])
				W.seg_last[i] = W.seq;
			else
				W.seg_first[i] = 0;    /* opened, nothing in it */
			open_n++;
		}
		if (open_n)
			LM_NOTICE("wal: %d segment span(s) left open by a stop "
				"without a shutdown - closed at seq %llu, the last "
				"the replay read\n", open_n, W.seq);
	}

	if (nthreads > MAX_THREADS)
		return -1;
	for (i = 0; i < nthreads; i++) {
		/* a ring per thread, sized by what the thread writes - the
		 * caller knows which slot is which; NULL = all ring_kb */
		W.rings[i].size = walring_size(ring_kb_of && ring_kb_of[i] > 0
			? ring_kb_of[i] : ring_kb);
		W.rings[i].buf = malloc(W.rings[i].size);
		if (!W.rings[i].buf)
			return -1;
		/* touched now, so RSS shows the ring's full cost from
		 * the start rather than a slow 'leak' until it wraps */
		pc_touch_pages(W.rings[i].buf, W.rings[i].size);
	}
	W.stage_cap = 4u << 20;
	W.stage = malloc(2 * W.stage_cap + 4);    /* and the write buffer after it, + its end mark */
	if (!W.stage)
		return -1;
	pc_touch_pages(W.stage, 2 * W.stage_cap);
	/* with the arena locked, the WAL's buffers are too - all or none;
	 * what a failure means is the daemon's call, this only reports it */
	if (W.pin) {
		unsigned long long want = 2 * W.stage_cap;
		int j, ok = mlock(W.stage, 2 * W.stage_cap) == 0;

		for (j = 0; ok && j < nthreads; j++) {
			ok = mlock(W.rings[j].buf, W.rings[j].size) == 0;
			want += W.rings[j].size;
		}
		if (ok) {
			W.locked_bytes = want;
		} else {
			W.lock_errno = errno;
			(void)munlock(W.stage, 2 * W.stage_cap);
			for (j = 0; j < nthreads; j++)
				(void)munlock(W.rings[j].buf, W.rings[j].size);
		}
	}
	/* a record larger than the ring is dropped after its client got OK;
	 * small rings are deliberate, so this warns rather than refuses */
	if ((long)ring_kb * 1024 <= MAX_REC)
		LM_WARN("wal: ring_kb = %d holds %ld KB, less than one maximum "
			"record (%ld KB) - a write larger than about %ld KB is "
			"acknowledged and then dropped from the log\n",
			ring_kb, (long)ring_kb, MAX_REC / 1024,
			((long)ring_kb * 1024 - PC_WAL_RECHDR - 300) / 1024);
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

	/* check for room before writing a byte: provisioning is real writes,
	 * so without this the daemon fills the filesystem, then fails */
	{
		struct statvfs vfs;
		long long need = 0;

		for (i = 0; i < W.nseg; i++) {
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
	 * restart continues the generation sequence */
	for (i = 0; i < W.nseg; i++) {
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
				        r32(h) == PC_WAL_MAGIC) {
					/* next_slot() reads the generations;
					 * headers fill in what CONTROL lacks */
					if (!W.seg_gen[i])
						W.seg_gen[i] = r32(h + 4);
					if (r32(h + 4) >= best_gen) {
						best_gen = r32(h + 4);
						start_idx = i;
					}
				}
				close(fd);
			}
		}
	}
	(void)start_idx;
	{
		int idx = next_slot();

		/* enforced here by the rule, not only by daemon.c
		 * checkpointing before calling us */
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
			wal_shed_once(PC_WAL_SHED_LOSS);
		}
		if (seg_activate(idx, best_gen + 1) != 0) {
			LM_ERR("wal: cannot activate a segment\n");
			return -1;
		}
	}
	W.enabled = 1;
	W.last_fsync_us = wall_now_us();
	{
		const char *e = getenv("PERFCACHED_WAL_SHRINK_S");

		W.shrink_us = (e && atoi(e) > 0 ? atoi(e) : 300) * 1000000LL;
		if (W.nseg_max > W.nseg_min) {
			pthread_attr_t at;

			pthread_attr_init(&at);
			pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
			W.grow_thr_on = pthread_create(&W.grow_thr, &at, grow_main,
				NULL) == 0;
			pthread_attr_destroy(&at);
		}
	}
	LM_NOTICE("wal: active in %s - %d x %d MB segments, generation %u, "
		"fsync %s; the ring follows the load from %d to %d segment(s)\n",
		dir, W.nseg, segment_mb, W.gen,
		mode == PC_WFSYNC_ALWAYS ? "always" :
		mode == PC_WFSYNC_NO ? "no" : "everysec", W.nseg_min, W.nseg_max);
	return 0;
}

void pc_wal_set_wakeup(int efd)
{
	W.pump_w = pc_worker_id();         /* the WAL thread calls this */
	W.wake_efd = efd;
}

void pc_wal_mark_sleeping(int s)
{
	__atomic_store_n(&W.sleeping, s, __ATOMIC_RELEASE);
}

void pc_wal_set_on_full(int heal)
{
	W.on_full_heal = heal;
}

void pc_wal_set_segments_max(int max)
{
	W.nseg_max = max;
}

void pc_wal_set_workers(int lo, int hi)
{
	W.work_lo = lo;
	W.work_hi = hi;
}

__thread int *pc_wal_wrote;

int pc_wal_tight(int resume)
{
	const struct ring *r;
	uint32_t free_;
	int w;

	if (!W.enabled || W.on_full_heal || W.work_hi <= 0 ||
	    __atomic_load_n(&W.failed, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&W.pump_gone, __ATOMIC_ACQUIRE))
		return 0;
	w = pc_worker_id();
	if (w < W.work_lo || w > W.work_hi || w >= W.nrings)
		return 0;
	r = &W.rings[w];
	free_ = r->size - (r->tail - __atomic_load_n(&r->head, __ATOMIC_ACQUIRE));
	return resume ? free_ < r->size / 2 : free_ < r->size / 4;
}

/* ---- producers --------------------------------------------------------- */

static __thread unsigned char *scratch;

/* a full ring waits instead of dropping (on_full = heal drops unless fsync =
 * always); non-workers at most PC_WAL_OTHER_WAIT_MS, not to stall peers */
static int ring_wait(struct ring *r, uint32_t need)
{
	struct timespec ts = { 0, 50000 };
	uint64_t one = 1;
	long long t0, bound = 0;
	int w;

	if ((W.on_full_heal && W.mode != PC_WFSYNC_ALWAYS) || W.wake_efd <= 0 ||
	    need > r->size || r == &W.rings[W.pump_w])
		return 0;
	w = pc_worker_id();
	if (W.work_hi > 0 && (w < W.work_lo || w > W.work_hi))
		bound = PC_WAL_OTHER_WAIT_MS * 1000LL;
	t0 = wall_now_us();
	while (r->size - (r->tail - __atomic_load_n(&r->head,
	        __ATOMIC_ACQUIRE)) < need) {
		if (__atomic_load_n(&W.failed, __ATOMIC_ACQUIRE) ||
		    __atomic_load_n(&W.pump_gone, __ATOMIC_ACQUIRE))
			return 0;
		if (bound && wall_now_us() - t0 >= bound)
			return 0;
		if (__atomic_load_n(&W.sleeping, __ATOMIC_ACQUIRE))
			if (write(W.wake_efd, &one, sizeof one) < 0) { /* best effort */ }
		nanosleep(&ts, NULL);
	}
	__atomic_add_fetch(&W.ring_waits, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&W.ring_wait_us,
		(unsigned long long)(wall_now_us() - t0), __ATOMIC_RELAXED);
	return 1;
}

/* a seq a full ring dropped - it holds a number, and the
 * synced watermark passes over it */
static void note_drop(unsigned long long seq)
{
	unsigned long long cur = __atomic_load_n(&W.drop_lo, __ATOMIC_RELAXED);

	while ((cur == 0 || seq < cur) &&
	       !__atomic_compare_exchange_n(&W.drop_lo, &cur, seq, 0,
	           __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
	cur = __atomic_load_n(&W.drop_hi, __ATOMIC_RELAXED);
	while (seq > cur &&
	       !__atomic_compare_exchange_n(&W.drop_hi, &cur, seq, 0,
	           __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
}

/* a test holds a "stall*" key's writer between its number and its ring (PERFCACHED_TEST_WAL_STALL_MS) */
static long wal_stall_ms(void)
{
	static long m = -1;

	if (m < 0) {
		const char *e = getenv("PERFCACHED_TEST_WAL_STALL_MS");
		long v = e ? atol(e) : 0;

		m = v > 0 ? v : 0;
	}
	return m;
}

/* a test drops a "drop*" key's record as a full ring drops one (PERFCACHED_TEST_WAL_DROP) */
static int wal_drop_test(void)
{
	static int m = -1;

	if (m < 0) {
		const char *e = getenv("PERFCACHED_TEST_WAL_DROP");

		m = e && atoi(e) > 0;
	}
	return m;
}

/* the record's seq once in a ring (or dropped by a full one); 0 when
 * nothing is logged by design, PC_WAL_SEQ_NEVER when it could not be */
static unsigned long long produce(int type, const char *col, const char *key,
		int klen, const char *val, int vlen, unsigned int expires_ticks,
		unsigned long long ver, unsigned char flags)
{
	const int hasver = PC_WAL_T_HASVER(type);
	const uint32_t hdr = PC_WAL_HDRLEN(type);
	struct ring *r;
	uint64_t expires_wall = 0, one = 1;
	uint32_t need, pl, pos;
	int collen = (int)strlen(col);
	unsigned long long seq = 0;

	/* after the device refused nothing goes to the rings: the pump has
	 * stopped, and a filling ring would report the refusal as drops */
	if (!W.enabled)
		return 0;
	if (__atomic_load_n(&W.failed, __ATOMIC_ACQUIRE))
		return PC_WAL_SEQ_NEVER;
	/* a cache collection's records skip the WAL, not its creates, drops,
	 * renames or numbered deletes - the replay needs those */
	if (type != PC_WAL_T_COL_CREATE && type != PC_WAL_T_COL_DROP &&
	    type != PC_WAL_T_COL_RENAME && type != PC_WAL_T_DEL_N &&
	    !pc_store_durable_name(col, (size_t)collen))
		return 0;
	{
		int w = pc_worker_id();

		if (w < 0 || w >= W.nrings)
			return type == PC_WAL_T_DEL_N ? PC_WAL_SEQ_NEVER : 0;
		r = &W.rings[w];
	}

	if (expires_ticks) {
		unsigned int nowt = get_ticks();

		expires_wall = (uint64_t)time(NULL) +
			(expires_ticks > nowt ? expires_ticks - nowt : 0);
	}

	pl = hdr + (uint32_t)collen + (uint32_t)klen +
		(uint32_t)(vlen > 0 ? vlen : 0);
	need = PC_WAL_RECHDR + pl;
	if (need > MAX_REC) {
		r->dropped++;
		return PC_WAL_SEQ_NEVER;
	}
	if (!scratch) {
		scratch = malloc(MAX_REC);
		if (!scratch)
			return PC_WAL_SEQ_NEVER;
	}

	{
		unsigned char *p = scratch;

		/* a bound below the number about to be taken, published first: the pump's
		 * watermark stops under it until the record is in the ring */
		__atomic_store_n(&r->busy, __atomic_load_n(&W.seq, __ATOMIC_SEQ_CST) + 1,
			__ATOMIC_SEQ_CST);
		seq = __atomic_fetch_add(&W.seq, 1, __ATOMIC_SEQ_CST) + 1;

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
		if (hdr == 33u)
			*p++ = flags;              /* the record's type */
		memcpy(p, col, (size_t)collen);
		p += collen;
		memcpy(p, key, (size_t)klen);
		p += klen;
		if (vlen > 0)
			memcpy(p, val, (size_t)vlen);
	}
	if (wal_stall_ms() && klen >= 5 && !memcmp(key, "stall", 5)) {
		struct timespec st = { wal_stall_ms() / 1000,
			(wal_stall_ms() % 1000) * 1000000L };

		nanosleep(&st, NULL);
	}

	/* SPSC publish: fits? copy (with wrap), then release the tail */
	{
		uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
		uint32_t tail = r->tail;
		uint32_t free_ = r->size - (tail - head);

		if (free_ < need && ring_wait(r, need)) {
			head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
			free_ = r->size - (tail - head);
		}
		if (wal_drop_test() && klen >= 4 && !memcmp(key, "drop", 4))
			free_ = 0;
		if (free_ < need) {
			r->dropped++;              /* on_full = heal, or a wait gave up */
			note_drop(seq);
			__atomic_store_n(&r->busy, 0, __ATOMIC_RELEASE);
			/* wake the pump here too: with every ring full
			 * nothing else wakes it, one batch per nap */
			if (__atomic_load_n(&W.sleeping, __ATOMIC_ACQUIRE) &&
			        W.wake_efd > 0)
				if (write(W.wake_efd, &one, sizeof one) < 0) { /* best effort */ }
			return seq;
		}
		pos = walring_pos(tail, r->size);
		if (pos + need <= r->size) {
			memcpy(r->buf + pos, scratch, need);
		} else {
			uint32_t first = r->size - pos;

			memcpy(r->buf + pos, scratch, first);
			memcpy(r->buf, scratch + first, need - first);
		}
		__atomic_store_n(&r->tail, tail + need, __ATOMIC_RELEASE);
		__atomic_store_n(&r->busy, 0, __ATOMIC_RELEASE);
	}
	if (pc_wal_wrote)
		*pc_wal_wrote = 1;                 /* its connection writes */
	if (__atomic_load_n(&W.sleeping, __ATOMIC_ACQUIRE) && W.wake_efd > 0)
		if (write(W.wake_efd, &one, sizeof one) < 0) { /* best effort */ }
	return seq;
}

void pc_wal_upsert(const char *col, const char *key, int klen,
		const char *val, int vlen, unsigned int expires_ticks,
		unsigned long long ver)
{
	produce(PC_WAL_T_UPSERT_V, col, key, klen, val, vlen, expires_ticks,
		ver, 0);
}

void pc_wal_upsert_fl(const char *col, const char *key, int klen,
		const char *val, int vlen, unsigned int expires_ticks,
		unsigned long long ver, unsigned char flags)
{
	produce(PC_WAL_T_UPSERT_V, col, key, klen, val, vlen, expires_ticks,
		ver, (unsigned char)(flags & PCACHE_F_CARRY));
}

void pc_wal_del(const char *col, const char *key, int klen)
{
	/* with the version the remove just stamped under the lock: every
	 * caller logs right after its remove, in the same thread */
	{
		unsigned char v[PC_WAL_DEL_VERLEN];

		w64(v, pcache_last_del_ver);
		produce(PC_WAL_T_DEL, col, key, klen, (const char *)v,
			PC_WAL_DEL_VERLEN, 0, 0, 0);
	}
}

unsigned long long pc_wal_del_n(const char *col, const char *key, int klen,
		unsigned long long ver, uint64_t oid, uint32_t incarn,
		uint64_t seq)
{
	unsigned char v[PC_WAL_DELN_VLEN];

	w64(v, ver);                       /* as a DEL's version */
	w64(v + 8, oid);
	w32(v + 16, incarn);
	w64(v + 20, seq);
	return produce(PC_WAL_T_DEL_N, col, key, klen, (const char *)v,
		PC_WAL_DELN_VLEN, 0, 0, 0);
}

unsigned long long pc_wal_durable_seq(unsigned long long covered)
{
	unsigned long long s = __atomic_load_n(&W.synced_seq, __ATOMIC_ACQUIRE);
	unsigned long long lo = __atomic_load_n(&W.drop_lo, __ATOMIC_RELAXED);
	unsigned long long hi = __atomic_load_n(&W.drop_hi, __ATOMIC_RELAXED);

	if (!W.enabled)
		return 0;                      /* not up yet: nothing is */
	if (hi > covered) {                /* a drop no snapshot covers yet */
		unsigned long long from = lo > covered ? lo : covered + 1;

		if (from <= s)
			s = from - 1;
	}
	return s > covered ? s : covered;
}

/* a create carries buckets_log2 as its one value byte; both carry a
 * placeholder key - a record with no key has no shape in this format */
void pc_wal_col_create(const char *col, int buckets_log2)
{
	char bl = (char)buckets_log2;

	produce(PC_WAL_T_COL_CREATE, col, ".", 1, &bl, 1, 0, 0, 0);
}

void pc_wal_col_drop(const char *col)
{
	produce(PC_WAL_T_COL_DROP, col, ".", 1, NULL, 0, 0, 0, 0);
}

void pc_wal_set_pin(int pin)
{
	W.pin = pin;
}

unsigned long long pc_wal_locked_bytes(int *err)
{
	if (err)
		*err = W.lock_errno;
	return W.locked_bytes;
}

unsigned long long pc_wal_buffer_bytes(void)
{
	unsigned long long n = 2 * W.stage_cap;
	int i;

	for (i = 0; i < W.nrings; i++)
		n += W.rings[i].size;
	return n;
}

int pc_wal_enabled(void)
{
	return W.enabled;
}

/* records before this are logged under @from and after it under @to; synced before the
 * caller persists @to, as a load maps a snapshot's @from by this record */
void pc_wal_col_rename(const char *from, const char *to)
{
	produce(PC_WAL_T_COL_RENAME, from, ".", 1, to, (int)strlen(to), 0, 0, 0);
	if (W.enabled && pc_wal_sync(0) < 0)
		LM_WARN("wal: the rename of '%s' to '%s' is not synced - a crash before "
			"it is may lose what the last snapshot holds under '%s'\n", from, to,
			from);
}

void pc_wal_touch(const char *col, const char *key, int klen,
		unsigned int expires_ticks, unsigned long long ver)
{
	produce(PC_WAL_T_TOUCH_V, col, key, klen, NULL, 0, expires_ticks,
		ver, 0);
}

/* ---- the WAL thread pump ----------------------------------------------- */

struct batch_rec {
	uint32_t pos;                     /* the record's head in its ring */
	uint32_t len;                     /* full record incl. header */
	uint64_t seq;
	int ring;
};

static int cmp_seq(const void *a, const void *b)
{
	const struct batch_rec *x = a, *y = b;

	return x->seq < y->seq ? -1 : x->seq > y->seq;
}


/* the first dropped acknowledged write sheds the node: a run of N seconds
 * misses a ring that oscillates, and a window still spends acknowledged data */

/* set by the cluster layer so wal.c need not know node states; without a
 * cluster the no-op stays and the CRIT is still logged */
static void wal_shed_noop(int why) { (void)why; }
static void (*W_shed_cb)(int) = wal_shed_noop;
static void pc_wal_shed_cb(int why) { W_shed_cb(why); }

static void wal_shed_noop_heal(int phase) { (void)phase; }
static void (*W_heal_cb)(int) = wal_shed_noop_heal;
static int W_heal_installed;

void pc_wal_on_heal(void (*fn)(int phase))
{
	if (!fn)
		return;
	W_heal_cb = fn;
	W_heal_installed = 1;
}

static unsigned long long wal_lost_total(void)
{
	unsigned long long d = W.overruns;
	int i;

	for (i = 0; i < W.nrings; i++)
		d += W.rings[i].dropped;
	return d;
}

/* a loss heals: the table still holds what the log lost, so the node refuses
 * writes until a snapshot begun after it lands; no rdb dir or IO = FAILED */
static int wal_can_heal(void)
{
	return W_heal_installed && !W.heal_gave_up && pc_rdb_dir()[0] != 0 &&
		!__atomic_load_n(&W.failed, __ATOMIC_ACQUIRE);
}

static void wal_shed_once(int why)
{
	if (why == PC_WAL_SHED_LOSS && wal_can_heal()) {
		if (!W.healing) {
			W.healing = 1;
			W.heal_t0_us = pc_rdb_mono_us();
			W.heal_req_us = 0;
			LM_WARN("wal: acknowledged writes were lost from the log "
				"(%llu so far) - HEALING: refusing writes, serving "
				"reads, and taking a snapshot that makes them "
				"durable again; the node returns to service by "
				"itself when it lands\n", wal_lost_total());
			W_heal_cb(1);
		}
		return;
	}
	if (W.shed)
		return;
	W.shed = why;
	pc_wal_shed_cb(why);
}

#define HEAL_RING ((int)(sizeof W.heal_end_ms / sizeof W.heal_end_ms[0]))

/* how many heals ended within the window (0..HEAL_RING) */
static int heals_recent(long long now_ms)
{
	int i, n = 0;

	for (i = 0; i < HEAL_RING; i++)
		if (W.heal_end_ms[i] &&
		        now_ms - W.heal_end_ms[i] < PC_WAL_HEAL_WIN_S * 1000LL)
			n++;
	return n;
}

/* each heal is a full snapshot and a spell of refused writes, so routine
 * healing means an undersized node - said once per window, with the fix */
static void heal_routine_note(void)
{
	long long now = pc_rdb_mono_us() / 1000;
	uint32_t big = 0;
	int n, i;

	W.heal_end_ms[W.heals % HEAL_RING] = now;
	n = heals_recent(now);
	if (n < PC_WAL_HEAL_ROUTINE || (W.heal_routine_said_ms &&
	        now - W.heal_routine_said_ms < PC_WAL_HEAL_WIN_S * 1000LL))
		return;
	W.heal_routine_said_ms = now;
	for (i = 0; i < W.nrings; i++)             /* rings differ by role */
		if (W.rings[i].size > big)
			big = W.rings[i].size;
	LM_WARN("wal: %d heals in the last %d minutes - healing has become "
		"routine: the write rate outruns this node's log (largest ring "
		"%u KB, fsync %s), and every heal costs a full snapshot "
		"and a spell of refused writes.  Raise [wal] ring_kb, give the "
		"log a faster disk, or spread the writes over more nodes\n",
		n, PC_WAL_HEAL_WIN_S / 60,
		big / 1024,
		W.mode == PC_WFSYNC_ALWAYS ? "always" :
		W.mode == PC_WFSYNC_NO ? "no" : "everysec");
}

/* the heal's steps, one per pump pass (WAL thread) */
static void wal_heal_step(void)
{
	struct pc_rdb_stats rs;
	unsigned long long lost;
	int i;

	if (!W.healing) {
		/* every pass: each moment before the refusal adds
		 * writes to cover; heal_mark = what the last heal covered */
		if (!wal_can_heal() || wal_lost_total() <= W.heal_mark)
			return;
		wal_shed_once(PC_WAL_SHED_LOSS);
		if (!W.healing)
			return;
	}
	if (__atomic_load_n(&W.failed, __ATOMIC_ACQUIRE)) {
		W.healing = 0;                 /* the IO shed has already run */
		return;
	}
	lost = wal_lost_total();
	pc_rdb_get_stats(&rs);
	if (!W.heal_req_us) {
		/* writes are refused now; what was in flight drains first, so
		 * the snapshot starts after the last record that could be lost */
		for (i = 0; i < W.nrings; i++)
			if (W.rings[i].head !=
			        __atomic_load_n(&W.rings[i].tail, __ATOMIC_ACQUIRE))
				return;
		W.heal_mark = lost;
		W.heal_err0 = rs.save_errors;
		W.heal_req_us = pc_rdb_mono_us();
		pc_rdb_request_urgent();
		return;
	}
	if (rs.save_errors > W.heal_err0) {
		W.healing = 0;
		W.heal_gave_up = 1;
		LM_CRIT("wal: the heal's snapshot failed - the lost writes "
			"cannot be made durable; the node goes FAILED\n");
		wal_shed_once(PC_WAL_SHED_LOSS);
		return;
	}
	if (rs.last_start_us < W.heal_req_us)
		return;                        /* the covering snapshot is not done */
	if (lost != W.heal_mark) {
		W.heal_req_us = 0;             /* lost more meanwhile: once more */
		return;
	}
	W.healing = 0;
	W.dropped_any = 0;
	W.heals++;
	W.last_heal_ms = (pc_rdb_mono_us() - W.heal_t0_us) / 1000;
	LM_NOTICE("wal: healed after %lld ms - the snapshot that began after "
		"the last loss is published, every acknowledged write is "
		"durable again; back in service (%llu heal(s) so far)\n",
		W.last_heal_ms, W.heals);
	heal_routine_note();
	W_heal_cb(0);
}

/* the device refused an append, a rotation or a sync: shed like a loss,
 * and stop the WAL (W.failed) - that descriptor cannot be trusted again */
static void wal_storage_failed(const char *what, int err)
{
	if (__atomic_load_n(&W.failed, __ATOMIC_ACQUIRE))
		return;
	W.failed_errno = err;
	__atomic_store_n(&W.failed, 1, __ATOMIC_RELEASE);
	LM_CRIT("wal: %s failed (%s) - marking this node FAILED.  The "
		"durable watermark stays at seq %llu: %llu record(s) after "
		"it were acknowledged and may not be on the device.  The "
		"sync is NOT retried - after a refusal the kernel drops the "
		"dirty pages, so a retry that succeeds proves nothing.  The "
		"node stays a member and goes on answering reads, refuses "
		"writes, and stays FAILED until the storage is fixed and it "
		"is restarted.\n", what, err ? strerror(err) : "short write",
		W.synced_seq, W.pending_hi > W.synced_seq
			? W.pending_hi - W.synced_seq : 0ULL);
	wal_shed_once(PC_WAL_SHED_IO);
}

void pc_wal_on_shed(void (*fn)(int why))
{
	if (!fn)
		return;
	W_shed_cb = fn;
	/* a startup overrun sheds inside pc_wal_init(), before this callback
	 * is installed - deliver it now */
	if (W.shed)
		fn(W.shed);
}

/* the batch, in seq order, copied into the write buffer (the stage's second
 * half) for one pwrite - an iovec per record was mostly kernel fault-in */
static size_t wal_pend;

static int append_flush(void)
{
	unsigned char *b = W.stage + W.stage_cap;
	size_t n = wal_pend, mark = 0;
	long long at = W.off;

	wal_pend = 0;
	if (!n)
		return 0;
	/* 4 zero bytes after the batch, overwritten by the next: the frontier reads len 0, not
	 * whatever a recycled segment held there */
	if (W.off + (long long)n + 4 <= W.seg_bytes) {
		memset(b + n, 0, 4);
		mark = 4;
	}
	n += mark;
	while (n > 0) {
		ssize_t w = pwrite(W.fd, b, n, at);

		if (w <= 0) {
			if (w == 0)
				errno = 0;             /* a short write, no errno */
			return -1;
		}
		at += w;
		b += w;
		n -= (size_t)w;
	}
	W.off = at - (long long)mark;
	return 0;
}

/* a record copied from its ring straight into the write buffer, and its
 * ring's room given back - a ring's records go in its own order */
static unsigned char *take_rec(const struct batch_rec *br)
{
	struct ring *r = &W.rings[br->ring];
	unsigned char *d = W.stage + W.stage_cap + wal_pend;

	walring_copy(r->buf, r->size, walring_pos(br->pos, r->size), d,
		br->len);
	__atomic_store_n(&r->head, br->pos + br->len, __ATOMIC_RELEASE);
	wal_pend += br->len;
	return d;
}

static int rings_empty(void);

static void seq_note(uint64_t seq)
{
	if (seq <= W.pending_hi && W.pending_hi)
		W.late++;                      /* the documented inversion */
	else
		W.pending_hi = seq;
}

/* would the next rotation overwrite a segment no completed snapshot
 * covers?  Only with snapshots to wait for */
static int next_hot(void)
{
	if (pc_rdb_dir()[0] == 0)
		return 0;
	return seg_hot(next_slot(), pc_rdb_safe_marker());
}

#define WAL_FULL_WAIT_MAX_MS 30000

static void full_end(void)
{
	long long t;

	if (!W.full_t0_us)
		return;
	t = wall_now_us();
	W.full_wait_us += (unsigned long long)(t - W.full_t0_us);
	LM_NOTICE("wal: a snapshot freed a segment - writes flow again after "
		"%lld ms of waiting\n", (t - W.full_t0_us) / 1000);
	W.full_t0_us = 0;
	W.full_gave_up = 0;
}

/* on_full = wait: a pass rotates at most once, and while the next segment
 * is uncovered it takes only the active one's room (full_note) */
static size_t full_cap(size_t stage_cap)
{
	size_t one = (size_t)(W.seg_bytes - PC_WAL_HDR);
	long long t, room;

	W.full_hot = 0;
	if (W.on_full_heal || !next_hot()) {
		full_end();
		return W.on_full_heal || one >= stage_cap ? stage_cap : one;
	}
	if (W.full_gave_up)
		return stage_cap;
	W.full_hot = 1;
	t = wall_now_us();
	if (t - W.full_req_us >= 1000000) {
		W.full_req_us = t;
		pc_rdb_request_urgent();
	}
	room = W.seg_bytes - W.off;
	return room <= 0 ? 0 : (size_t)room < stage_cap ? (size_t)room
		: stage_cap;
}

/* writers wait only when that cap left records in the rings; after
 * WAL_FULL_WAIT_MAX_MS the overrun and its heal */
static void full_note(int left)
{
	long long t;

	if (!W.full_hot || !left)
		return;
	t = wall_now_us();
	if (!W.full_t0_us) {
		W.full_t0_us = t;
		W.full_waits++;
		LM_WARN("wal: no free segment - every one holds records no "
			"snapshot has covered yet, so writes wait for the "
			"snapshot now being taken instead of overwriting "
			"(on_full = wait).  Increase segments/segment_mb if "
			"this repeats\n");
	}
	if (t - W.full_t0_us >= WAL_FULL_WAIT_MAX_MS * 1000LL &&
	    !W.full_gave_up) {
		W.full_gave_up = 1;
		LM_CRIT("wal: waited %d s for a snapshot to free a segment and "
			"none did - overwriting, as on_full = heal would\n",
			WAL_FULL_WAIT_MAX_MS / 1000);
	}
}

int pc_wal_pump(void)
{
	static struct batch_rec recs[BATCH_MAX];
	unsigned char *stage = W.stage;
	size_t stage_len = 0, stage_cap = W.stage_cap;    /* the batch's bytes, and their cap */
	int i, nrec = 0, left = 0;
	long long now;

	if (!W.enabled || __atomic_load_n(&W.failed, __ATOMIC_ACQUIRE))
		return 1000;
	if (!stage)
		return 1000;
	grow_adopt();
	stage_cap = full_cap(stage_cap);

	/* every ring's records, by place: copied once, in seq order, below */
	for (i = 0; i < W.nrings && nrec < BATCH_MAX; i++) {
		struct ring *r = &W.rings[i];
		uint32_t head = r->head;
		uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);

		while (head != tail && nrec < BATCH_MAX) {
			unsigned char hdr[PC_WAL_RECHDR], sq[8];
			uint32_t pos = walring_pos(head, r->size), len, total;

			walring_copy(r->buf, r->size, pos, hdr,
				PC_WAL_RECHDR);
			len = r32(hdr);
			total = PC_WAL_RECHDR + len;
			if (stage_len + total > stage_cap) {
				left = 1;
				break;
			}
			walring_copy(r->buf, r->size, (pos + PC_WAL_RECHDR +
				1 + 1 + 2 + 4 + 8) % r->size, sq, 8);
			recs[nrec].pos = head;
			recs[nrec].len = total;
			recs[nrec].seq = r64(sq);
			recs[nrec].ring = i;
			stage_len += total;
			nrec++;
			head += total;
		}
	}
	full_note(left);

	if (nrec) {
		qsort(recs, (size_t)nrec, sizeof recs[0], cmp_seq);
		for (i = 0; i < nrec; i++) {
			struct batch_rec *br = &recs[i];
			unsigned char *d1;
			uint32_t crc;

			/* three that fit this segment: checksummed at once */
			if (i + 3 <= nrec && W.off + (long long)wal_pend + br[0].len +
			        br[1].len + br[2].len <= W.seg_bytes) {
				const unsigned char *p[3];
				unsigned char *d[3];
				size_t n[3];
				uint32_t c[3];
				int k;

				if (wal_pend + br[0].len + br[1].len + br[2].len >
				        W.stage_cap && append_flush() != 0) {
					wal_storage_failed("append", errno);
					return 1000;
				}
				for (k = 0; k < 3; k++) {
					seq_note(br[k].seq);
					d[k] = take_rec(&br[k]);
					w32(d[k] + 8, W.gen);
					p[k] = d[k] + 8;
					n[k] = br[k].len - 8;
				}
				pc_crc32c_x3(p, n, c);
				for (k = 0; k < 3; k++) {
					w32(d[k] + 4, c[k]);
					W.appended++;
					W.bytes += br[k].len;
				}
				i += 2;
				continue;
			}
			seq_note(br->seq);

			/* rotate first, stamp after: the gen echo must be the
			 * generation of the segment the record lands in */
			if (W.off + (long long)wal_pend + br->len > W.seg_bytes) {
				/* what is queued belongs to THIS segment */
				if (append_flush() != 0) {
					wal_storage_failed("append", errno);
					return 1000;
				}
				if (seg_rotate() != 0) {
					/* a refused sync has said so itself */
					if (!W.failed)
						wal_storage_failed("segment "
							"rotation", errno);
					return 1000;
				}
			}
			if (wal_pend + br->len > W.stage_cap && append_flush() != 0) {
				wal_storage_failed("append", errno);
				return 1000;
			}
			d1 = take_rec(br);
			w32(d1 + 8, W.gen);
			crc = pc_crc32c(0, d1 + 8, br->len - 8);
			w32(d1 + 4, crc);
			W.appended++;
			W.bytes += br->len;
		}
		/* before any sync below: a sync covers what was written */
		if (append_flush() != 0) {
			wal_storage_failed("append", errno);
			return 1000;
		}
	}

	now = wall_now_us();
	/* the probe promised one latency and the device delivers another: say
	 * so once, with both numbers - the startup probe measured a cache */
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
	/* ~1 Hz: has the ring dropped anything since we last said so? */
	if (now - W.drop_log_us >= 1000000) {
		unsigned long long d = 0;

		shrink_check(now);
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

		/* without a heal, out of service: leaving the map sheds the
		 * load, where refusing each write invites retries */
		if (W.dropped_any && !W.shed && !W.healing && !wal_can_heal()) {
			LM_CRIT("wal: dropped %llu acknowledged write(s) - "
				"marking this node FAILED.  It stays a member "
				"and goes on answering reads, but refuses "
				"writes and no client will select it for new "
				"work, and it stays FAILED until restarted.  "
				"Raise [wal] ring_kb, or move to "
				"fsync = everysec.\n", d);
			wal_shed_once(PC_WAL_SHED_LOSS);
		}
		/* where it can heal, only wal_heal_step() starts heals: drops
		 * learnt here after a heal completed were covered by it */
	}
	if (W.mode == PC_WFSYNC_ALWAYS && nrec) { /* NOLINT(bugprone-branch-clone) */
		wal_sync_commit(now, 1);
	/* the arms differ by MODE, kept apart so each mode's comment
	 * sits on its own branch */
	/* NOLINTNEXTLINE(bugprone-branch-clone) */
	} else if (W.mode == PC_WFSYNC_EVERYSEC &&
	           now - W.last_fsync_us >= 1000000) {
		wal_sync_commit(now, 1);
	}

	/* a sync barrier: everything pumped so far goes to the platter
	 * NOW, whatever the fsync policy says */
	if (__atomic_load_n(&W.sync_req, __ATOMIC_ACQUIRE)) {
		wal_sync_commit(now, 1);
		__atomic_store_n(&W.sync_req, 0, __ATOMIC_RELEASE);
	}

	wal_heal_step();
	/* a round that stopped at BATCH_MAX or the stage's size left records
	 * behind - drain again now, not after a 20 ms nap */
	if (W.full_t0_us && !W.full_gave_up)
		return 2;                      /* a snapshot frees room soon */
	if (nrec && !rings_empty())
		return 0;
	if (W.mode == PC_WFSYNC_ALWAYS)
		return nrec ? 0 : 20;          /* drain again promptly */
	return nrec || W.healing ? 20 : 200;
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
	__atomic_store_n(&W.pump_gone, 1, __ATOMIC_RELEASE);
	/* nobody is left to refuse a write to, so a refusal here is a line in
	 * the log - but the line is owed, and the watermark stays */
	if (!W.failed && wal_sync_commit(wall_now_us(), 1) != 0)
		LM_CRIT("wal: the shutdown sync failed - records after seq "
			"%llu may not be on the device\n", W.synced_seq);
	/* after the drain: here CONTROL can be exact, not a lower bound; the
	 * active segment's span is closed first for the same reason */
	__atomic_store_n(&W.grow_stop, 1, __ATOMIC_RELEASE);
	if (W.seg_last && W.nseg > 0 && W.cur >= 0)
		W.seg_last[W.cur] = W.pending_hi;
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

/* the sync verb (worker-callable): block, bounded, until all appended is
 * synced; 0 = synced, 1 = WAL off, -1 = timeout, -2 = the device refused */
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
	/* a dropped number is never written: the wait is for a sync that left none below it */
	while (waited < timeout_ms) {
		if (__atomic_load_n(&W.synced_low, __ATOMIC_ACQUIRE) >= target)
			return 0;
		/* the watermark will never reach it - say so now, not after the
		 * caller's timeout */
		if (__atomic_load_n(&W.failed, __ATOMIC_ACQUIRE))
			return -2;
		__atomic_store_n(&W.sync_req, 1, __ATOMIC_RELEASE);
		if (__atomic_load_n(&W.sleeping, __ATOMIC_ACQUIRE) &&
		        W.wake_efd > 0)
			if (write(W.wake_efd, &one, sizeof one) < 0) { }
		usleep(2000);
		waited += 2;
	}
	return __atomic_load_n(&W.synced_low, __ATOMIC_ACQUIRE) >= target ? 0 : -1;
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
	out->ring_waits = __atomic_load_n(&W.ring_waits, __ATOMIC_RELAXED);
	out->ring_wait_ms = __atomic_load_n(&W.ring_wait_us, __ATOMIC_RELAXED) / 1000;
	{
		long long t0 = W.full_t0_us;

		out->full_waits = W.full_waits;
		out->full_wait_ms = (W.full_wait_us + (unsigned long long)(t0 ?
			wall_now_us() - t0 : 0)) / 1000;
		out->full_waiting = t0 != 0;
		out->on_full_heal = W.on_full_heal;
	}
	out->late = W.late;
	out->overruns = W.overruns;
	out->free_segments = W.seg_first ? W.free_segs : 0;
	out->segments = W.seg_first ? W.nseg : 0;
	out->segments_min = W.seg_first ? W.nseg_min : 0;
	out->segments_max = W.seg_first ? W.nseg_max : 0;
	out->grown = W.grown;
	out->retired = W.retired;
	out->segment_bytes = W.seg_first ? (unsigned long long)W.seg_bytes : 0;
	/* fill rate and how long the free segments last at it; the time since
	 * the last rotation is a floor, so an idle ring's figure decays */
	{
		double s = W.seg_ewma_s;
		long long now = wall_now_us();

		if (s > 0 && W.rot_last_us) {
			double idle = (double)(now - W.rot_last_us) / 1e6;

			if (idle > s)
				s = idle;
			out->fill_mb_s = (double)W.seg_bytes / 1048576.0 / s;
			out->full_in_s = W.free_segs * s;
			out->ring_fill_s = W.nseg * s;
		} else {
			out->fill_mb_s = 0;
			out->ring_fill_s = -1;
			out->full_in_s = -1;       /* not measured yet */
		}
	}
	out->recycles = W.recycles;
	out->failed = __atomic_load_n(&W.failed, __ATOMIC_ACQUIRE);
	out->failed_errno = W.failed_errno;
	out->ctrl_errors = W.ctrl_errors;
	out->last_seq = W.seq;
	out->synced_seq = W.synced_seq;
	out->pending_hi = W.pending_hi;
	out->heals = W.heals;
	out->healing = W.healing;
	out->last_heal_ms = W.last_heal_ms;
	out->heals_recent = heals_recent(pc_rdb_mono_us() / 1000);
	out->heal_routine = out->heals_recent >= PC_WAL_HEAL_ROUTINE;
	{
		unsigned long long lost = wal_lost_total();

		out->unhealed = lost > W.heal_mark ? lost - W.heal_mark : 0;
	}
}
