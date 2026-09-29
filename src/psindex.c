/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* psindex.c - see psindex.h */
#include <stdlib.h>
#include <string.h>

#include "psindex.h"

#define FNV_BASIS 2166136261u
#define FNV_PRIME 16777619u

#define LOAD(x)      __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v)  __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

/* ---- glob ------------------------------------------------------------------
 * Written for this engine, not ported.  Iterative with ONE backtrack point:
 * every token but '*' consumes exactly one byte, so when a later token
 * fails only the most recent '*' needs to take one more byte - an earlier
 * star taking more could only hand the rest a later start, which the most
 * recent star already covers.  Work is at most pattern x string, with no
 * recursion.  (The first engine's recursive matcher was exponential in '*'
 * and read past an open set; see CHANGELOG, SECURITY.) */

/* the set at p[i] == '[': is byte c in it, and the index past it (past its
 * ']', or the pattern's end when it is left open) */
static size_t glob_set(const char *p, size_t n, size_t i, unsigned char c,
		int *in_set)
{
	int negate = 0, found = 0;

	i++;
	if (i < n && p[i] == '^') {
		negate = 1;
		i++;
	}
	while (i < n && p[i] != ']') {
		unsigned char a = (unsigned char)p[i];

		if (a == '\\' && i + 1 < n) {          /* an escaped member */
			if ((unsigned char)p[i + 1] == c)
				found = 1;
			i += 2;
		} else if (i + 2 < n && p[i + 1] == '-') {   /* a range */
			unsigned char b = (unsigned char)p[i + 2];
			unsigned char lo = a < b ? a : b, hi = a < b ? b : a;

			if (c >= lo && c <= hi)
				found = 1;
			i += 3;
		} else {                               /* a plain member */
			if (a == c)
				found = 1;
			i++;
		}
	}
	*in_set = negate ? !found : found;
	return i < n ? i + 1 : n;
}

int psi_glob(const char *p, size_t n, const char *s, size_t len)
{
	size_t i = 0, j = 0;                   /* pattern, string */
	size_t star = (size_t)-1, star_j = 0;  /* the retry point */

	while (j < len) {
		if (i < n) {
			unsigned char c = (unsigned char)s[j];
			int in_set;

			if (p[i] == '*') {
				while (i < n && p[i] == '*')
					i++;
				if (i == n)
					return 1;          /* a trailing star takes the rest */
				star = i;
				star_j = j;
				continue;
			}
			if (p[i] == '?') {
				i++;
				j++;
				continue;
			}
			if (p[i] == '[') {
				size_t next = glob_set(p, n, i, c, &in_set);

				if (in_set) {
					i = next;
					j++;
					continue;
				}
			} else if (p[i] == '\\' && i + 1 < n) {
				if ((unsigned char)p[i + 1] == c) {
					i += 2;
					j++;
					continue;
				}
			} else if ((unsigned char)p[i] == c) {
				/* a literal - a trailing '\' is one too */
				i++;
				j++;
				continue;
			}
		}
		/* this token failed, or the pattern ran out first: the most
		 * recent star takes one more byte, or there is no match */
		if (star == (size_t)-1)
			return 0;
		i = star;
		j = ++star_j;
	}
	while (i < n && p[i] == '*')
		i++;
	return i == n;
}

size_t psi_literal_prefix(const char *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (p[i] == '*' || p[i] == '?' || p[i] == '[' || p[i] == '\\')
			break;
	return i;
}

/* ---- hashing ------------------------------------------------------------ */

static uint32_t fnv(const char *s, size_t n, uint32_t h)
{
	size_t i;

	for (i = 0; i < n; i++)
		h = (h ^ (unsigned char)s[i]) * FNV_PRIME;
	return h;
}

/* a pattern's bucket key: its first @level bytes, and the level itself, so
 * "ab" filed at level 2 and "ab" read at level 2 agree and nothing else does */
static uint32_t key_hash(uint32_t prefix_hash, unsigned level)
{
	return (prefix_hash ^ ((uint32_t)level * 0x9E3779B1u)) * FNV_PRIME;
}

static void node_key(const struct psi_table *t, const char *name, size_t nlen,
		uint32_t *hash, uint16_t *level)
{
	if (t->flags & PSI_PATTERNS) {
		size_t lp = psi_literal_prefix(name, nlen);
		unsigned l = lp < PSI_K ? (unsigned)lp : PSI_K;

		*level = (uint16_t)l;
		*hash = key_hash(fnv(name, l, FNV_BASIS), l);
	} else {
		*level = 0;
		*hash = fnv(name, nlen, FNV_BASIS);
	}
}

/* ---- the table ---------------------------------------------------------- */

int psi_init(struct psi_table *t, unsigned nbuckets, unsigned flags)
{
	unsigned nb = 16;

	while (nb < nbuckets && nb < (1u << 30))
		nb <<= 1;
	memset(t, 0, sizeof *t);
	t->b = calloc(nb, sizeof *t->b);
	if (!t->b)
		return -1;
	t->nb = nb;
	t->flags = flags;
	return 0;
}

void psi_destroy(struct psi_table *t)
{
	free(t->b);
	t->b = NULL;
	t->nb = 0;
}

/* private tables only: no reader can be on a chain while it is rebuilt */
static int grow(struct psi_table *t)
{
	unsigned nb = t->nb << 1, i;
	struct psi_node **nbk = calloc(nb, sizeof *nbk);

	if (!nbk)
		return -1;
	for (i = 0; i < t->nb; i++) {
		struct psi_node *x = t->b[i], *next;

		for (; x; x = next) {
			next = x->next;
			x->next = nbk[x->hash & (nb - 1)];
			nbk[x->hash & (nb - 1)] = x;
		}
	}
	free(t->b);
	t->b = nbk;
	t->nb = nb;
	return 0;
}

int psi_insert(struct psi_table *t, struct psi_node *node)
{
	int rc = 0;
	unsigned i;

	node_key(t, node->name, node->nlen, &node->hash, &node->level);
	if ((t->flags & PSI_PRIVATE) && t->n + 1 > 2 * t->nb && grow(t) != 0)
		rc = -1;
	i = node->hash & (t->nb - 1);
	/* fully built before it becomes visible */
	node->next = t->b[i];
	STORE(t->b[i], node);
	__atomic_add_fetch(&t->n, 1, __ATOMIC_RELAXED);
	if (t->flags & PSI_PATTERNS)
		__atomic_add_fetch(&t->level_n[node->level], 1, __ATOMIC_RELAXED);
	return rc;
}

void psi_remove(struct psi_table *t, struct psi_node *node)
{
	struct psi_node **pp = &t->b[node->hash & (t->nb - 1)];

	for (; *pp; pp = &(*pp)->next)
		if (*pp == node) {
			/* node->next is left as it is: a reader standing on
			 * the node walks on into the live chain */
			STORE(*pp, node->next);
			__atomic_sub_fetch(&t->n, 1, __ATOMIC_RELAXED);
			if (t->flags & PSI_PATTERNS)
				__atomic_sub_fetch(&t->level_n[node->level], 1,
					__ATOMIC_RELAXED);
			return;
		}
}

struct psi_node *psi_find(const struct psi_table *t, const char *name,
		size_t nlen)
{
	uint32_t h;
	uint16_t level;
	struct psi_node *x;

	node_key(t, name, nlen, &h, &level);
	for (x = LOAD(t->b[h & (t->nb - 1)]); x; x = LOAD(x->next))
		if (x->hash == h && x->nlen == nlen && !memcmp(x->name, name, nlen))
			return x;
	return NULL;
}

int psi_match(const struct psi_table *t, const char *chan, size_t clen,
		psi_match_f cb, void *arg)
{
	uint32_t ph = FNV_BASIS;
	size_t maxl = clen < PSI_K ? clen : PSI_K, l;
	int found = 0;

	if (!LOAD(t->n))
		return 0;
	for (l = 0; l <= maxl; l++) {
		uint32_t kh;
		struct psi_node *x;

		if (l)
			ph = (ph ^ (unsigned char)chan[l - 1]) * FNV_PRIME;
		if (!LOAD(t->level_n[l]))
			continue;
		kh = key_hash(ph, (unsigned)l);
		for (x = LOAD(t->b[kh & (t->nb - 1)]); x; x = LOAD(x->next)) {
			if (x->hash != kh || x->level != l ||
			        memcmp(x->name, chan, l))
				continue;
			if (psi_glob(x->name, x->nlen, chan, clen)) {
				cb(x, arg);
				found++;
			}
		}
	}
	return found;
}

int psi_each(const struct psi_table *t, psi_each_f cb, void *arg)
{
	unsigned i;
	int n = 0;

	for (i = 0; i < t->nb; i++) {
		struct psi_node *x;

		for (x = LOAD(t->b[i]); x; x = LOAD(x->next)) {
			if (cb(x, arg) != 0)
				return n;
			n++;
		}
	}
	return n;
}
