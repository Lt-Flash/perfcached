/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * ackstate.c - 12ny AT-3b: the node's watermarks and their file.  See
 * ackstate.h.
 *
 * The file, explicit LE:
 *   u32 magic 'PCAK' | u32 ver 1 | u64 snapshot marker | u32 n
 *   n x [u64 oid][u32 incarn][u64 applied mark]
 *   u32 crc32c of everything before it
 * Written to a temp name, synced, renamed, the directory synced - the
 * snapshot's own discipline.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "ackstate.h"
#include "crc32c.h"
#include "walfmt.h"

#define ACK_MAGIC 0x4B414350u              /* 'PCAK' LE */
#define ACK_VER   1u
#define ACK_HDR   20u
#define ACK_ENT   20u

struct cap { uint64_t oid; uint32_t incarn; uint64_t mark; };

static struct clack *A;
static struct clackf *F;                   /* AT-4: under A_mx too */
static pthread_mutex_t A_mx = PTHREAD_MUTEX_INITIALIZER;
static struct cap capv[CLACK_SPACES];
static unsigned int capn;
static int cap_valid;
/* AT-4: the keepalive's slice - the cluster thread's alone */
static struct cap slv[CLACK_SPACES];
static unsigned int sln, slcur;

int pc_ack_init(int durable_on_apply)
{
	pthread_mutex_lock(&A_mx);
	if (!A)
		A = clack_new(durable_on_apply);
	if (!F)
		F = clackf_new();
	pthread_mutex_unlock(&A_mx);
	return A && F ? 0 : -1;
}

int pc_ack_enabled(void)
{
	return A != NULL;
}

int pc_ack_applied(uint64_t oid, uint32_t incarn, uint64_t seq,
		uint64_t walseq)
{
	int rc;

	if (!A)
		return CLACK_FULL;
	pthread_mutex_lock(&A_mx);
	rc = clack_applied(A, oid, incarn, seq, walseq);
	pthread_mutex_unlock(&A_mx);
	return rc;
}

void pc_ack_synced(uint64_t durable)
{
	if (!A)
		return;
	pthread_mutex_lock(&A_mx);
	clack_synced(A, durable);
	pthread_mutex_unlock(&A_mx);
}

static int cap_cb(uint64_t oid, uint32_t incarn, uint64_t mark,
		uint64_t applied, uint64_t hi, void *ctx)
{
	(void)mark; (void)hi; (void)ctx;
	if (capn < CLACK_SPACES) {
		capv[capn].oid = oid;
		capv[capn].incarn = incarn;
		capv[capn].mark = applied;
		capn++;
	}
	return 0;
}

void pc_ack_capture(void)
{
	if (!A)
		return;
	pthread_mutex_lock(&A_mx);
	capn = 0;
	clack_each(A, cap_cb, NULL);
	cap_valid = 1;
	pthread_mutex_unlock(&A_mx);
}

int pc_ack_save(const char *dir, uint64_t marker)
{
	char tmp[512], fin[512];
	unsigned char *buf;
	size_t len, off;
	unsigned int i, n;
	int fd, dfd, rc = -1;

	if (!A || !cap_valid)
		return 0;
	n = capn;                          /* the snapshot thread's own */
	len = ACK_HDR + (size_t)n * ACK_ENT + 4;
	if (!(buf = malloc(len)))
		return -1;
	w32(buf, ACK_MAGIC);
	w32(buf + 4, ACK_VER);
	w64(buf + 8, marker);
	w32(buf + 16, n);
	for (i = 0, off = ACK_HDR; i < n; i++, off += ACK_ENT) {
		w64(buf + off, capv[i].oid);
		w32(buf + off + 8, capv[i].incarn);
		w64(buf + off + 12, capv[i].mark);
	}
	w32(buf + off, pc_crc32c(0, buf, off));
	cap_valid = 0;
	snprintf(tmp, sizeof tmp, "%s/" PC_ACK_FILE ".tmp.%d", dir, (int)getpid());
	snprintf(fin, sizeof fin, "%s/" PC_ACK_FILE, dir);
	fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	if (fd >= 0) {
		if (write(fd, buf, len) == (ssize_t)len && fdatasync(fd) == 0 &&
		    close(fd) == 0) {
			fd = -1;
			if (rename(tmp, fin) == 0) {
				dfd = open(dir, O_RDONLY | O_DIRECTORY);
				if (dfd >= 0 && fsync(dfd) == 0)
					rc = 0;
				if (dfd >= 0)
					close(dfd);
			}
		}
		if (fd >= 0)
			close(fd);
		if (rc)
			unlink(tmp);
	}
	free(buf);
	return rc;
}

int pc_ack_load(const char *dir, uint64_t marker)
{
	char path[512];
	unsigned char *buf;
	struct stat st;
	unsigned int i, n, done = 0;
	size_t len, off;
	int fd;

	if (!A)
		return 0;
	snprintf(path, sizeof path, "%s/" PC_ACK_FILE, dir);
	if ((fd = open(path, O_RDONLY)) < 0)
		return 0;
	if (fstat(fd, &st) != 0 || st.st_size < (off_t)(ACK_HDR + 4) ||
	    !(buf = malloc((size_t)st.st_size))) {
		close(fd);
		return -1;
	}
	len = (size_t)st.st_size;
	if (read(fd, buf, len) != (ssize_t)len) {
		close(fd);
		free(buf);
		return -1;
	}
	close(fd);
	n = r32(buf + 16);
	if (r32(buf) != ACK_MAGIC || r32(buf + 4) != ACK_VER ||
	    n > CLACK_SPACES || len != ACK_HDR + (size_t)n * ACK_ENT + 4 ||
	    pc_crc32c(0, buf, len - 4) != r32(buf + len - 4)) {
		free(buf);
		return -1;
	}
	if (r64(buf + 8) != marker) {      /* another snapshot's */
		free(buf);
		return 0;
	}
	pthread_mutex_lock(&A_mx);
	for (i = 0, off = ACK_HDR; i < n; i++, off += ACK_ENT)
		if (clack_restore(A, r64(buf + off), r32(buf + off + 8),
		        r64(buf + off + 12)) == CLACK_NEW)
			done++;
	pthread_mutex_unlock(&A_mx);
	free(buf);
	return (int)done;
}

static int slice_cb(uint64_t oid, uint32_t incarn, uint64_t mark,
		uint64_t applied, uint64_t hi, void *ctx)
{
	(void)applied; (void)hi; (void)ctx;
	if (sln < CLACK_SPACES) {
		slv[sln].oid = oid;
		slv[sln].incarn = incarn;
		slv[sln].mark = mark;
		sln++;
	}
	return 0;
}

struct holes_ctx { struct pc_ack_hole *out; int max, n; };

static int holes_cb(uint64_t oid, uint32_t incarn, uint64_t mark,
		uint64_t applied, uint64_t hi, void *ctx)
{
	struct holes_ctx *h = ctx;
	uint64_t f, t;

	(void)mark;
	if (hi > applied && h->n < h->max && clack_gap(A, oid, incarn, &f, &t)) {
		h->out[h->n].oid = oid;
		h->out[h->n].incarn = incarn;
		h->out[h->n].from = f;
		h->out[h->n++].to = t;
	}
	return 0;
}

int pc_ack_holes(struct pc_ack_hole *out, int max)
{
	struct holes_ctx h = { out, max, 0 };

	if (!A)
		return 0;
	pthread_mutex_lock(&A_mx);
	clack_each(A, holes_cb, &h);
	pthread_mutex_unlock(&A_mx);
	return h.n;
}

int pc_ack_floor(uint64_t oid, uint32_t incarn, uint64_t floor)
{
	int rc;

	if (!A)
		return CLACK_FULL;
	pthread_mutex_lock(&A_mx);
	rc = clack_floor(A, oid, incarn, floor);
	pthread_mutex_unlock(&A_mx);
	return rc;
}

struct known_ctx { uint64_t oid; uint32_t incarn; uint64_t applied, hi; int found; };

static int known_cb(uint64_t oid, uint32_t incarn, uint64_t mark,
		uint64_t applied, uint64_t hi, void *ctx)
{
	struct known_ctx *k = ctx;

	(void)mark;
	if (oid != k->oid || incarn != k->incarn)
		return 0;
	k->applied = applied;
	k->hi = hi;
	k->found = 1;
	return 1;
}

int pc_ack_known(uint64_t oid, uint32_t incarn, uint64_t *applied,
		uint64_t *hi)
{
	struct known_ctx k = { oid, incarn, 0, 0, 0 };

	if (!A)
		return 0;
	pthread_mutex_lock(&A_mx);
	clack_each(A, known_cb, &k);
	pthread_mutex_unlock(&A_mx);
	*applied = k.applied;
	*hi = k.hi;
	return k.found;
}

int pc_ack_slice(uint64_t *oid, uint32_t *incarn, uint64_t *mark, int max,
		int *nspaces)
{
	int i, n;

	*nspaces = 0;
	if (!A || max <= 0)
		return 0;
	pthread_mutex_lock(&A_mx);
	sln = 0;
	clack_each(A, slice_cb, NULL);
	pthread_mutex_unlock(&A_mx);
	*nspaces = (int)sln;
	if (!sln)
		return 0;
	n = (int)sln < max ? (int)sln : max;
	if (slcur >= sln)
		slcur = 0;
	for (i = 0; i < n; i++) {
		const struct cap *c = &slv[(slcur + (unsigned int)i) % sln];

		oid[i] = c->oid;
		incarn[i] = c->incarn;
		mark[i] = c->mark;
	}
	slcur = (slcur + (unsigned int)n) % sln;
	return n;
}

int pc_ack_fleet_member(uint64_t member, uint32_t incarn)
{
	int rc;

	if (!F)
		return -1;
	pthread_mutex_lock(&A_mx);
	rc = clackf_member(F, member, incarn);
	pthread_mutex_unlock(&A_mx);
	return rc;
}

int pc_ack_fleet_report(uint64_t member, uint64_t oid, uint32_t sincarn,
		uint64_t mark, long long now)
{
	int rc;

	if (!F)
		return -1;
	pthread_mutex_lock(&A_mx);
	rc = clackf_report(F, member, oid, sincarn, mark, now);
	pthread_mutex_unlock(&A_mx);
	return rc;
}

void pc_ack_fleet_clear(uint64_t member)
{
	if (!F)
		return;
	pthread_mutex_lock(&A_mx);
	clackf_clear(F, member);
	pthread_mutex_unlock(&A_mx);
}

void pc_ack_fleet_forget(uint64_t member)
{
	if (!F)
		return;
	pthread_mutex_lock(&A_mx);
	clackf_forget(F, member);
	pthread_mutex_unlock(&A_mx);
}

int pc_ack_fleet_moved(uint64_t member, uint64_t oid, uint32_t sincarn,
		long long *moved)
{
	int rc;

	if (!F)
		return 0;
	pthread_mutex_lock(&A_mx);
	rc = clackf_moved(F, member, oid, sincarn, moved);
	pthread_mutex_unlock(&A_mx);
	return rc;
}

uint64_t pc_ack_fleet_max(uint64_t oid, uint32_t sincarn,
		const uint64_t *members, int n)
{
	uint64_t v;

	if (!F)
		return 0;
	pthread_mutex_lock(&A_mx);
	v = clackf_max(F, oid, sincarn, members, n);
	pthread_mutex_unlock(&A_mx);
	return v;
}

int pc_ack_fleet_stuck(uint64_t oid, uint32_t sincarn, const uint64_t *members,
		int n, uint64_t head, long long also, long long now,
		long long still_ms, struct clackf_stuck *out, int max)
{
	int k;

	if (!F)
		return 0;
	pthread_mutex_lock(&A_mx);
	k = clackf_stuck(F, oid, sincarn, members, n, head, also, now,
		still_ms, out, max);
	pthread_mutex_unlock(&A_mx);
	return k;
}

struct rows_ctx { uint64_t member; struct pc_ack_row *out; int n, max; };

static int rows_own_cb(uint64_t oid, uint32_t incarn, uint64_t mark,
		uint64_t applied, uint64_t hi, void *ctx)
{
	struct rows_ctx *r = ctx;

	(void)applied;
	if (r->n >= r->max)
		return 1;
	r->out[r->n].oid = oid;
	r->out[r->n].incarn = incarn;
	r->out[r->n].mark = mark;
	r->out[r->n].head = hi;            /* raised by the members' below */
	r->out[r->n++].moved = 0;
	return 0;
}

static int rows_fleet_cb(uint64_t member, uint32_t incarn, uint64_t oid,
		uint32_t sincarn, uint64_t mark, void *ctx)
{
	struct rows_ctx *r = ctx;

	(void)incarn;
	if (member != r->member)
		return 0;
	if (r->n >= r->max)
		return 1;
	r->out[r->n].oid = oid;
	r->out[r->n].incarn = sincarn;
	r->out[r->n].mark = mark;
	r->out[r->n].head = 0;
	r->out[r->n++].moved = 0;
	return 0;
}

struct hi_ctx { uint64_t oid; uint32_t incarn; uint64_t hi; };

static int hi_cb(uint64_t oid, uint32_t incarn, uint64_t mark,
		uint64_t applied, uint64_t hi, void *ctx)
{
	struct hi_ctx *h = ctx;

	(void)mark; (void)applied;
	if (oid == h->oid && incarn == h->incarn) {
		h->hi = hi;
		return 1;
	}
	return 0;
}

int pc_ack_member_rows(uint64_t member, int self, struct pc_ack_row *out,
		int max)
{
	struct rows_ctx r = { member, out, 0, max };
	int i;

	if (!A || !F || max <= 0)
		return 0;
	pthread_mutex_lock(&A_mx);
	if (self)
		clack_each(A, rows_own_cb, &r);
	else
		clackf_each(F, rows_fleet_cb, &r);
	for (i = 0; i < r.n; i++) {
		struct hi_ctx h = { out[i].oid, out[i].incarn, 0 };
		uint64_t f = clackf_max(F, out[i].oid, out[i].incarn, NULL, 0);

		if (!self) {
			clack_each(A, hi_cb, &h);
			if (h.hi > out[i].head)
				out[i].head = h.hi;
			(void)clackf_moved(F, member, out[i].oid, out[i].incarn,
				&out[i].moved);
		}
		if (f > out[i].head)
			out[i].head = f;
		if (out[i].mark > out[i].head)
			out[i].head = out[i].mark;
	}
	pthread_mutex_unlock(&A_mx);
	return r.n;
}

long long pc_ack_fleet_heard(uint64_t member, long long now)
{
	long long was;

	if (!F)
		return 0;
	pthread_mutex_lock(&A_mx);
	was = clackf_heard(F, member, now);
	pthread_mutex_unlock(&A_mx);
	return was;
}

int pc_ack_fleet_mark(uint64_t member, uint64_t oid, uint32_t sincarn,
		uint64_t *mark)
{
	int rc;

	if (!F)
		return 0;
	pthread_mutex_lock(&A_mx);
	rc = clackf_mark(F, member, oid, sincarn, mark);
	pthread_mutex_unlock(&A_mx);
	return rc;
}

uint64_t pc_ack_fleet_min(uint64_t oid, uint32_t sincarn,
		const uint64_t *members, int n)
{
	uint64_t v;

	if (!F)
		return 0;
	pthread_mutex_lock(&A_mx);
	v = clackf_min(F, oid, sincarn, members, n);
	pthread_mutex_unlock(&A_mx);
	return v;
}

int pc_ack_fleet_each(clackf_each_cb cb, void *ctx)
{
	int rc;

	if (!F)
		return 0;
	pthread_mutex_lock(&A_mx);
	rc = clackf_each(F, cb, ctx);
	pthread_mutex_unlock(&A_mx);
	return rc;
}

void pc_ack_fleet_stats(struct clackf_stats *st)
{
	memset(st, 0, sizeof *st);
	if (!F)
		return;
	pthread_mutex_lock(&A_mx);
	clackf_get_stats(F, st);
	pthread_mutex_unlock(&A_mx);
}

int pc_ack_each(clack_each_cb cb, void *ctx)
{
	int rc;

	if (!A)
		return 0;
	pthread_mutex_lock(&A_mx);
	rc = clack_each(A, cb, ctx);
	pthread_mutex_unlock(&A_mx);
	return rc;
}

void pc_ack_get_stats(struct clack_stats *st)
{
	memset(st, 0, sizeof *st);
	if (!A)
		return;
	pthread_mutex_lock(&A_mx);
	clack_get_stats(A, st);
	pthread_mutex_unlock(&A_mx);
}
