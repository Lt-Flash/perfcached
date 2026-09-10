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

#define TERM_MAGIC 0x4D544350u             /* 'PCTM' LE - the legacy form */

/* S106: the file is text, in the identity file's style:
 *
 *   perfcached-node-term 1
 *   term 11
 *   check 9f0c2b7e4d1a6358
 *
 * The check is FNV-1a over the term's four little-endian bytes - a
 * torn-write detector, not a guard.  The legacy eight-byte form (magic
 * + term) is read once and rewritten.  Anything else refuses. */
#define TERM_HEADER "perfcached-node-term 1"

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

static uint64_t term_check(uint32_t t)
{
	uint64_t h = 1469598103934665603ULL;
	unsigned char raw[4];
	int i;

	w32(raw, t);
	for (i = 0; i < 4; i++) {
		h ^= raw[i];
		h *= 1099511628211ULL;
	}
	return h;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* strict: the three lines above and nothing else; 0 parsed, -1 not */
static int term_parse(const unsigned char *buf, size_t len, uint32_t *out)
{
	const char *s = (const char *)buf;
	size_t hl = strlen(TERM_HEADER), i, d;
	uint64_t v = 0, want = 0;

	if (len < hl + 1 || memcmp(s, TERM_HEADER, hl) != 0 || s[hl] != '\n')
		return -1;
	s += hl + 1;
	len -= hl + 1;
	if (len < 5 + 1 + 1 || memcmp(s, "term ", 5) != 0)
		return -1;
	s += 5;
	len -= 5;
	for (d = 0; d < len && s[d] != '\n'; d++) {
		if (s[d] < '0' || s[d] > '9' || d >= 10)
			return -1;
		v = v * 10 + (uint64_t)(s[d] - '0');
	}
	if (d == 0 || d >= len || (d > 1 && s[0] == '0') || v > 0xFFFFFFFFu)
		return -1;
	s += d + 1;
	len -= d + 1;
	if (len != 6 + 16 + 1 || memcmp(s, "check ", 6) != 0 || s[6 + 16] != '\n')
		return -1;
	for (i = 0; i < 16; i++) {
		int x = hexval((unsigned char)s[6 + i]);

		if (x < 0)
			return -1;
		want = want << 4 | (uint64_t)x;
	}
	if (want != term_check((uint32_t)v))
		return -1;
	*out = (uint32_t)v;
	return 0;
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
	char tmp[600], fin[600], buf[96];
	int fd, dirfd, n, ok = 0;

	if (!T.dir[0])
		return 0;
	snprintf(tmp, sizeof tmp, "%s/" PC_TERM_FILE ".tmp.%d", T.dir, (int)getpid());
	snprintf(fin, sizeof fin, "%s/" PC_TERM_FILE, T.dir);

	n = snprintf(buf, sizeof buf, TERM_HEADER "\nterm %u\ncheck %016llx\n",
		(unsigned)t, (unsigned long long)term_check(t));

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return 0;
	if (write(fd, buf, (size_t)n) == (ssize_t)n && fsync(fd) == 0)
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
	unsigned char buf[128];
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
	if (got < 0)
		return -1;
	if (got == 8 && r32(buf) == TERM_MAGIC) {
		/* the legacy form: read once, rewritten as text - and the
		 * rewrite has to succeed, or durability is a claim */
		T.term = r32(buf + 4);
		if (!term_store(T.term))
			return -1;
		T.durable = 1;
		return 0;
	}
	if (term_parse(buf, (size_t)got, &T.term) != 0)
		return -1;                 /* present but invalid: refuse */
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

