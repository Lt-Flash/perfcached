/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * clterm.c — the mastership term: persistence and the three rules.
 *
 * Kept free of cluster.c so the rules can be exercised without a fleet;
 * the decisions are pure functions and the state is one small file.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "clterm.h"

#define TERM_MAGIC 0x4D544350u             /* 'PCTM' LE */

static struct {
	uint32_t term;
	int durable;
	char dir[512];
} T;

unsigned long long pc_term_rejected;

static void w32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}
static uint32_t r32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Written through a temp file and a rename, not in place.  The term is
 * rewritten every mastership change, so unlike the write-once identity
 * it can be torn by a crash mid-write - and a half-written term is not
 * a smaller term, it is an arbitrary one.  The directory fsync is what
 * makes the rename itself durable; without it the old name can survive
 * a power loss and the new content be lost.
 */
static int term_store(uint32_t t)
{
	char tmp[600], fin[600];
	unsigned char buf[8];
	int fd, dirfd, ok = 0;

	if (!T.dir[0])
		return 0;
	snprintf(tmp, sizeof tmp, "%s/" PC_TERM_FILE ".tmp.%d", T.dir, (int)getpid());
	snprintf(fin, sizeof fin, "%s/" PC_TERM_FILE, T.dir);

	w32(buf, TERM_MAGIC);
	w32(buf + 4, t);

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return 0;
	if (write(fd, buf, sizeof buf) == (ssize_t)sizeof buf && fsync(fd) == 0)
		ok = 1;
	close(fd);
	if (!ok) {
		unlink(tmp);
		return 0;
	}
	if (rename(tmp, fin) != 0) {
		unlink(tmp);
		return 0;
	}
	dirfd = open(T.dir, O_RDONLY);
	if (dirfd >= 0) {
		fsync(dirfd);
		close(dirfd);
	}
	return 1;
}

int pc_term_init(const char *state_dir)
{
	char path[600];
	unsigned char buf[8];
	int fd;
	ssize_t got;

	memset(&T, 0, sizeof T);
	if (!state_dir || !*state_dir)
		return 0;                  /* nothing persisted, nothing to
		                            * contradict - start at 0 */
	snprintf(T.dir, sizeof T.dir, "%s", state_dir);
	snprintf(path, sizeof path, "%s/" PC_TERM_FILE, state_dir);

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		/* absent is the normal first boot; prove we CAN write before
		 * claiming durability, so a read-only state dir is caught
		 * here rather than at the moment of a promotion */
		T.durable = term_store(0);
		return 0;
	}
	got = read(fd, buf, sizeof buf);
	close(fd);
	if (got != (ssize_t)sizeof buf || r32(buf) != TERM_MAGIC)
		return -1;                 /* present but unreadable: refuse */
	T.term = r32(buf + 4);
	T.durable = 1;
	return 0;
}

uint32_t pc_term_current(void) { return T.term; }
int pc_term_durable(void) { return T.durable; }

int pc_term_observe(uint32_t seen)
{
	if (seen <= T.term)
		return 0;
	if (seen > T.term + PC_TERM_MAX_JUMP) {
		pc_term_rejected++;
		return -1;
	}
	/* persist BEFORE adopting: a term we have acknowledged but cannot
	 * remember is one we could contradict after a restart */
	if (T.dir[0] && !term_store(seen))
		return -1;
	T.term = seen;
	return 1;
}

uint32_t pc_term_claim(void)
{
	uint32_t next = T.term + 1;

	if (next < T.term)
		return 0;                  /* wrap: unreachable, never silent */
	if (T.dir[0]) {
		if (!term_store(next))
			return 0;          /* cannot remember it: must not use it */
	} else if (T.durable) {
		return 0;                  /* claimed durable with nowhere to
		                            * write - a contradiction, refuse */
	}
	T.term = next;
	return next;
}

int pc_term_cmp(uint32_t mine, uint32_t theirs)
{
	if (theirs < mine)
		return PC_TERM_STALE;
	if (theirs > mine)
		return PC_TERM_AHEAD;
	return PC_TERM_SAME;
}

int pc_term_must_stepdown(int is_master, uint32_t mine, uint32_t theirs)
{
	return is_master && theirs > mine;
}

