/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/*
 * psindex.h - a name index for pub/sub: exact channel names, or glob
 * patterns indexed by their literal prefix, intrusive (the caller embeds a
 * struct psi_node in its own record and owns the memory).
 *
 * CONCURRENCY.  One writer at a time - the caller's lock, or a table only
 * one thread ever touches.  Readers take no lock: heads and links are
 * loaded and stored atomically, a node is fully built before it is linked,
 * and an unlinked node still points into the live chain, so a reader
 * standing on it walks on.  What the index does NOT do is free: a caller
 * with concurrent readers must retire an unlinked node through quiesce.h
 * and free it once no reader can hold it.
 *
 * THE PATTERN INDEX.  Every byte of a pattern before its first '*', '?',
 * '[' or '\' is literal, and a literal byte consumes exactly one channel
 * byte, so a pattern whose literal prefix is P can only match a channel
 * that starts with P.  A pattern is filed under (min(|P|, PSI_K), the
 * first min(|P|, PSI_K) bytes of P); a lookup probes, for each level l up
 * to min(PSI_K, channel length), the one bucket for (l, the channel's
 * first l bytes), skipping levels that hold nothing, and runs the glob
 * only on the patterns filed there.  Patterns that start with a
 * metacharacter live at level 0 and are always tried - nothing can be
 * pruned for them.  The previous engine ran the glob on every pattern for
 * every publish.
 *
 * Private tables (PSI_PRIVATE: one thread, no concurrent readers) grow as
 * they fill; shared tables keep the size they were made with, because a
 * rehash under lock-free readers would move nodes out from under them.
 */
#ifndef PC_PSINDEX_H
#define PC_PSINDEX_H

#include <stddef.h>
#include <stdint.h>

#define PSI_K           32             /* literal-prefix levels indexed */
#define PSI_PRIVATE     0x1            /* one thread: may grow */
#define PSI_PATTERNS    0x2            /* a pattern table, not exact names */

struct psi_node {
	struct psi_node *next;             /* the bucket chain (atomic) */
	const char *name;                  /* caller-owned, stable while linked */
	uint32_t nlen;
	uint32_t hash;                     /* exact: of the name; pattern: of (level, prefix) */
	uint16_t level;                    /* patterns: min(literal prefix, PSI_K) */
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

/* writer side.  psi_insert sets node->hash and node->level; the caller
 * set name and nlen.  0, or -1 when a private table could not grow (the
 * node is linked regardless - a longer chain is not a failure). */
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
