/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* psindex.h - an intrusive pub/sub name index: exact names, or patterns filed by literal
 * prefix.  One writer at a time; readers take no lock, so unlinked nodes retire via quiesce.h. */
#ifndef PC_PSINDEX_H
#define PC_PSINDEX_H

#include <stddef.h>
#include <stdint.h>

#define PSI_K           32             /* literal-prefix levels indexed */
#define PSI_PRIVATE     0x1            /* one thread: may grow; shared ones never rehash */
#define PSI_PATTERNS    0x2            /* a pattern table, not exact names */

struct psi_node {
	struct psi_node *next;             /* the bucket chain (atomic) */
	const char *name;                  /* caller-owned, stable while linked */
	uint32_t nlen;
	uint32_t hash;                     /* exact: of the name; pattern: of (level, prefix) */
	uint16_t level;                    /* patterns: min(prefix, PSI_K); 0 = always tried */
};

struct psi_table {
	struct psi_node **b;               /* heads (atomic) */
	unsigned nb;                       /* power of two */
	unsigned n;                        /* nodes linked (atomic) */
	unsigned flags;
	unsigned level_n[PSI_K + 1];       /* patterns per level (atomic) */
};

int  psi_init(struct psi_table *t, unsigned nbuckets, unsigned flags);
void psi_destroy(struct psi_table *t); /* frees the buckets, not the nodes */

/* writer side: psi_insert sets node->hash and node->level (the caller set name and nlen);
 * -1 when a private table could not grow - the node is linked regardless */
int  psi_insert(struct psi_table *t, struct psi_node *node);
void psi_remove(struct psi_table *t, struct psi_node *node);

/* reader side */
struct psi_node *psi_find(const struct psi_table *t, const char *name,
		size_t nlen);
typedef void (*psi_match_f)(struct psi_node *node, void *arg);
/* every pattern in @t matching @chan; returns how many */
int  psi_match(const struct psi_table *t, const char *chan, size_t clen,
		psi_match_f cb, void *arg);
/* every node, in bucket order (writer-side iteration) */
typedef int (*psi_each_f)(struct psi_node *node, void *arg);
int  psi_each(const struct psi_table *t, psi_each_f cb, void *arg);

/* the glob itself: * ? [set] [^set] [a-z] \x, one byte per token but '*',
 * an open set closed at the pattern's end; O(pattern x string) */
int  psi_glob(const char *p, size_t n, const char *s, size_t len);
/* bytes before the first metacharacter */
size_t psi_literal_prefix(const char *p, size_t n);

#endif /* PC_PSINDEX_H */
