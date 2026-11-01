/*
 * clorder.h - S461: a replicated set and a replicated delete of one key,
 * applied on two threads in any interleaving, end as one thread would.
 */
#ifndef PC_CLORDER_H
#define PC_CLORDER_H

#include "compat/str.h"
#include "core/pcache_htable.h"

#define CLORDER_REFUSED 2              /* refused before the store */
#define CLORDER_TAKEN   3              /* stored, then taken back by a delete */

struct clorder_tomb {
	/* nonzero refuses a copy at @ver (a tombstone or a dropped copy covers it) */
	int (*refuse)(void *arg, unsigned long long ver);
	unsigned long long (*ver)(void *arg);      /* the newest tombstone, 0 none */
	void *arg;
};

/* 0 = stored, CLORDER_*, or pcache_ht_store_ver's error */
int clorder_set(pcache_htable_t *ht, const str *k, const str *v,
		unsigned int exp, unsigned char fl, unsigned long long ver,
		const struct clorder_tomb *t);
/* after a store at @ver: 1 = the tombstone at @tv (0 none) covers it, taken back.
 * Read @tv after the store - the bucket lock orders it after a racing delete's plant */
int clorder_take_back(pcache_htable_t *ht, const str *k, unsigned long long ver,
		unsigned long long tv);
/* its tombstone already planted; @shadow is a resize filling (S69), or NULL.
 * 1 = removed from @ht */
int clorder_del(pcache_htable_t *ht, pcache_htable_t *shadow, const str *k,
		unsigned long long ver);

enum { CLORDER_SET, CLORDER_DEL };
/* tests: called in each side's gap - after the set's check, before the remove */
extern void (*clorder_test_hook)(int side, const str *k);

#endif
