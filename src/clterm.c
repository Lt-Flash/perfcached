/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* clterm.c - the mastership term: persistence and the three rules, free of cluster.c so the
 * rules (pure functions over one small file) can be exercised without a fleet. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "clterm.h"
#include "fnv1a.h"
#include "statefile.h"

#define TERM_MAGIC 0x4D544350u             /* 'PCTM' LE - the legacy form */

/* the file is three lines: TERM_HEADER, "term <n>", "check <%016llx FNV-1a of the term's 4 LE
 * bytes>" (a torn-write detector).  The legacy 8-byte form (magic + term) is read, rewritten. */
#define TERM_HEADER "perfcached-node-term 1"

/* term = the highest adopted (claimed, or seen and followed), which a claim goes above, the cluster
 * thread's; stored = the highest on disk, pc-maint's (pc_term_persist_tick); claim = a term asked to be
 * stored for a promotion, refused = one whose store failed; unsaved = an adopted term its store
 * refused, until a later store lands (set only on a failure: stats read it) */
static struct {
	uint32_t term, stored, claim, refused, unsaved;
	int durable, err;
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
	unsigned char raw[4];

	w32(raw, t);
	return fnv1a64(raw, 4);
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

/* strict: those three lines and nothing else; 0 parsed, -1 not */
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

/* Through a temp file and a rename (statefile.c): the term is rewritten on every mastership change,
 * and a torn term is an arbitrary one.  1 stored; 0 not, the errno kept */
static int term_store(uint32_t t)
{
	char buf[96];
	int n;

	if (!T.dir[0])
		return 0;
	n = snprintf(buf, sizeof buf, TERM_HEADER "\nterm %u\ncheck %016llx\n",
		(unsigned)t, (unsigned long long)term_check(t));
	if (pc_state_write(T.dir, PC_TERM_FILE, buf, (size_t)n) != 0) {
		__atomic_store_n(&T.err, errno, __ATOMIC_RELAXED);
		return 0;
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
		if (errno != ENOENT)
			return -1;         /* there but unreadable: refuse, never overwrite */
		/* absent is the normal first boot; prove we CAN write before claiming
		 * durability, so a read-only state dir is caught now, not at a promotion */
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
		T.stored = T.term;
		return 0;
	}
	if (term_parse(buf, (size_t)got, &T.term) != 0)
		return -1;                 /* present but invalid: refuse */
	T.durable = 1;
	T.stored = T.term;
	return 0;
}

uint32_t pc_term_current(void) { return __atomic_load_n(&T.term, __ATOMIC_ACQUIRE); }
int pc_term_durable(void) { return T.durable; }

int pc_term_plausible(uint32_t seen)
{
	if (seen <= T.term || seen - T.term <= PC_TERM_MAX_JUMP)
		return 1;
	pc_term_rejected++;
	return 0;
}

int pc_term_observe(uint32_t seen)
{
	if (seen <= T.term)
		return 0;
	if (!pc_term_plausible(seen))
		return -1;
	/* adopted at once - the node follows it, and a claim must go above it; pc-maint writes it, and
	 * until it is on disk a claim waits for it (pc_term_claim_ready) */
	__atomic_store_n(&T.term, seen, __ATOMIC_RELEASE);
	return 1;
}

int pc_term_persist_tick(void)
{
	uint32_t t = __atomic_load_n(&T.term, __ATOMIC_ACQUIRE);
	uint32_t c = __atomic_load_n(&T.claim, __ATOMIC_ACQUIRE);
	uint32_t want = c > t ? c : t;

	if (!T.dir[0] || want <= __atomic_load_n(&T.stored, __ATOMIC_ACQUIRE))
		return 0;
	if (!term_store(want)) {
		if (t > __atomic_load_n(&T.stored, __ATOMIC_ACQUIRE))
			__atomic_store_n(&T.unsaved, t, __ATOMIC_RELEASE);
		if (c > t) {               /* the promotion goes on without a term, as it always did */
			__atomic_store_n(&T.refused, c, __ATOMIC_RELEASE);
			__atomic_compare_exchange_n(&T.claim, &c, 0, 0,
				__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
		}
		return -1;
	}
	__atomic_store_n(&T.stored, want, __ATOMIC_RELEASE);
	return 1;
}

uint32_t pc_term_unpersisted(void)
{
	uint32_t u = __atomic_load_n(&T.unsaved, __ATOMIC_ACQUIRE);

	return u > __atomic_load_n(&T.stored, __ATOMIC_ACQUIRE) ? u : 0;
}

int pc_term_retry(void)
{
	uint32_t u = __atomic_load_n(&T.unsaved, __ATOMIC_ACQUIRE);

	if (!u)
		return 0;
	if (u > __atomic_load_n(&T.stored, __ATOMIC_ACQUIRE))
		return -1;
	return __atomic_compare_exchange_n(&T.unsaved, &u, 0, 0, __ATOMIC_ACQ_REL,
		__ATOMIC_ACQUIRE) ? 1 : -1;
}

int pc_term_store_errno(void) { return __atomic_load_n(&T.err, __ATOMIC_RELAXED); }

int pc_term_claim_ready(void)
{
	uint32_t next = T.term + 1;

	if (!T.dir[0] || __atomic_load_n(&T.stored, __ATOMIC_ACQUIRE) >= next ||
	        __atomic_load_n(&T.refused, __ATOMIC_ACQUIRE) >= next)
		return 1;
	__atomic_store_n(&T.claim, next, __ATOMIC_RELEASE);
	return 0;
}

uint32_t pc_term_claim(void)
{
	uint32_t next = T.term + 1;

	if (next < T.term)
		return 0;                  /* wrap: unreachable, never silent */
	if (T.dir[0]) {
		if (__atomic_load_n(&T.stored, __ATOMIC_ACQUIRE) < next) {
			/* not on disk: must not use it - pc-maint is asked again for a later call */
			__atomic_store_n(&T.refused, 0, __ATOMIC_RELEASE);
			__atomic_store_n(&T.claim, next, __ATOMIC_RELEASE);
			return 0;
		}
	} else if (T.durable) {
		return 0;                  /* claimed durable with nowhere to
		                            * write - a contradiction, refuse */
	}
	__atomic_store_n(&T.term, next, __ATOMIC_RELEASE);
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

