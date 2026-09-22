/*
 * psindextest.c - the pub/sub name index (src/psindex.[ch]).  The pattern
 * index must find EXACTLY the patterns a brute-force glob over every
 * pattern finds, for literal prefixes shorter than, equal to and longer
 * than PSI_K, for patterns that start with a metacharacter, before and
 * after removals, in a private table that grows and a shared one that
 * does not.  Exact-name lookups must find what was inserted and nothing
 * that was removed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psindex.h"

static int pass, fail;
#define CHK(c, m) do { if (c) pass++; else { fail++; printf("  FAIL %s\n", m); } } while (0)

struct pat {
	struct psi_node node;
	char name[80];
	int linked;
	int hit;
};

static unsigned long long rs = 0x243F6A8885A308D3ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 29); }

static void mark(struct psi_node *n, void *arg)
{
	(void)arg;
	((struct pat *)n)->hit++;
}

/* a pattern: a literal prefix of 0..48 bytes over a small alphabet (so
 * prefixes collide and share buckets), then a tail with metacharacters */
static void make_pattern(char *out, size_t cap)
{
	static const char lit[] = "ab.:_";
	static const char *tails[] = { "*", "?*", "[ab]*", "\\**", "", "*b", "?" };
	size_t lp = rnd() % 49, i;

	if (lp > cap - 12)
		lp = cap - 12;
	for (i = 0; i < lp; i++)
		out[i] = lit[rnd() % 5];
	strcpy(out + lp, tails[rnd() % 7]);
}

static void make_channel(char *out, size_t cap, size_t *len)
{
	static const char al[] = "ab.:_*";
	size_t n = 1 + rnd() % 56, i;

	if (n > cap)
		n = cap;
	for (i = 0; i < n; i++)
		out[i] = al[rnd() % 6];
	*len = n;
}

static void run(unsigned flags, unsigned nb, const char *what)
{
	enum { NP = 3000 };
	struct psi_table t;
	static struct pat p[NP];
	int i, trial, bad = 0, removed = 0;
	long matches = 0;

	psi_init(&t, nb, flags | PSI_PATTERNS);
	for (i = 0; i < NP; i++) {
		make_pattern(p[i].name, sizeof p[i].name);
		p[i].node.name = p[i].name;
		p[i].node.nlen = (uint32_t)strlen(p[i].name);
		p[i].linked = 1;
		psi_insert(&t, &p[i].node);
	}
	for (trial = 0; trial < 40000; trial++) {
		char ch[64];
		size_t cl;

		if (trial == 20000)
			for (i = 0; i < NP; i += 3) {
				psi_remove(&t, &p[i].node);
				p[i].linked = 0;
				removed++;
			}
		make_channel(ch, sizeof ch, &cl);
		for (i = 0; i < NP; i++)
			p[i].hit = 0;
		psi_match(&t, ch, cl, mark, NULL);
		for (i = 0; i < NP; i++) {
			int want = p[i].linked && psi_glob(p[i].name, strlen(p[i].name), ch, cl);

			matches += want;
			if (p[i].hit != want) {
				if (bad++ < 5)
					printf("  %s: pattern \"%s\" vs \"%.*s\": index %d, brute force %d\n",
						what, p[i].name, (int)cl, ch, p[i].hit, want);
			}
		}
	}
	printf("  %s: 40000 channels x %d patterns (%d removed half way), %ld matches, %d disagreements, %u buckets at the end\n",
		what, NP, removed, matches, bad, t.nb);
	CHK(bad == 0 && matches > 1000, what);
	bad = 0;
	for (i = 0; i < NP; i++) {
		struct psi_node *f = psi_find(&t, p[i].name, strlen(p[i].name));

		if (p[i].linked && !f)
			bad++;
	}
	CHK(bad == 0, "every linked pattern is found by its full name");
	psi_destroy(&t);
}

int main(void)
{
	struct psi_table t;
	struct pat e[5000];
	int i, bad = 0;

	/* ---- exact names ---- */
	psi_init(&t, 64, PSI_PRIVATE);
	for (i = 0; i < 5000; i++) {
		snprintf(e[i].name, sizeof e[i].name, "chan:%d", i * 7919);
		e[i].node.name = e[i].name;
		e[i].node.nlen = (uint32_t)strlen(e[i].name);
		psi_insert(&t, &e[i].node);
	}
	CHK(t.nb > 64, "a private exact table grows as it fills");
	for (i = 0; i < 5000; i += 2)
		psi_remove(&t, &e[i].node);
	for (i = 0; i < 5000; i++) {
		struct psi_node *f = psi_find(&t, e[i].name, strlen(e[i].name));

		if ((i % 2 == 0) != (f == NULL) || (f && f != &e[i].node))
			bad++;
	}
	CHK(bad == 0 && t.n == 2500, "exact names: found while linked, gone once removed");
	CHK(psi_find(&t, "chan:", 5) == NULL && psi_find(&t, "chan:7919x", 10) == NULL,
		"a prefix or an extension of a name is not the name");
	psi_destroy(&t);

	/* ---- literal prefix ---- */
	CHK(psi_literal_prefix("news.*", 6) == 5 && psi_literal_prefix("*x", 2) == 0 &&
		psi_literal_prefix("a\\*b", 4) == 1 && psi_literal_prefix("abc", 3) == 3 &&
		psi_literal_prefix("a?c", 3) == 1 && psi_literal_prefix("a[b]", 4) == 1,
		"the literal prefix stops at * ? [ and \\");

	/* ---- the pattern index against brute force ---- */
	run(PSI_PRIVATE, 16, "private table, grows from 16 buckets");
	run(0, 4096, "shared table, fixed 4096 buckets");

	printf("psindextest: %d passed, %d failed\n", pass, fail);
	return fail != 0;
}
