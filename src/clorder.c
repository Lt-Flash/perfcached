/*
 * clorder.c - S461: the set and delete apply paths of a replicated key.
 */
#include "clorder.h"

void (*clorder_test_hook)(int side, const str *k);

int clorder_set(pcache_htable_t *ht, const str *k, const str *v,
		unsigned int exp, unsigned char fl, unsigned long long ver,
		const struct clorder_tomb *t)
{
	unsigned long long tv;
	int rc;

	if (t->refuse(t->arg, ver))
		return CLORDER_REFUSED;
	if (clorder_test_hook)
		clorder_test_hook(CLORDER_SET, k);
	rc = pcache_ht_store_ver(ht, k, v, exp, fl, ver);
	if (rc)
		return rc;
	/* a delete planted after the check removed nothing yet */
	tv = t->ver(t->arg);
	return clorder_take_back(ht, k, ver, tv) ? CLORDER_TAKEN : 0;
}

int clorder_take_back(pcache_htable_t *ht, const str *k, unsigned long long ver,
		unsigned long long tv)
{
	return tv && ver <= tv && pcache_ht_remove_below(ht, k, tv + 1) == 1;
}

int clorder_del(pcache_htable_t *ht, pcache_htable_t *shadow, const str *k,
		unsigned long long ver)
{
	if (clorder_test_hook)
		clorder_test_hook(CLORDER_DEL, k);
	if (!ver) {                        /* an older sender: unversioned */
		if (shadow)
			(void)pcache_ht_remove(shadow, k);
		return pcache_ht_remove(ht, k) == 1;
	}
	/* a write newer than the delete stays, checked under the bucket lock */
	if (shadow)
		(void)pcache_ht_remove_upto(shadow, k, ver);
	return pcache_ht_remove_upto(ht, k, ver) == 1;
}
