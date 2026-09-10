/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 Yury Kirsanov - see COPYING */
/* Shim for OpenSIPS mi/item.h — mi_item_t becomes a generic key/value
 * sink so the arena's stats emitter compiles unchanged.  The daemon's
 * stats verb supplies the callbacks (S8); selftests can pass a printf
 * sink or NULL-check their way around it. */
#ifndef PC_COMPAT_MI_ITEM_H
#define PC_COMPAT_MI_ITEM_H

typedef struct mi_item {
	void (*number)(struct mi_item *it, const char *name, int nlen,
			long long val);
	void (*string)(struct mi_item *it, const char *name, int nlen,
			const char *val, int vlen);
	void *ctx;
} mi_item_t;

#define MI_SSTR(_s) (_s), sizeof(_s) - 1

static inline int add_mi_number(mi_item_t *it, const char *name, int nlen,
		long long val)
{
	if (it && it->number)
		it->number(it, name, nlen, val);
	return 0;
}

static inline int add_mi_string(mi_item_t *it, const char *name, int nlen,
		const char *val, int vlen)
{
	if (it && it->string)
		it->string(it, name, nlen, val, vlen);
	return 0;
}

#endif /* PC_COMPAT_MI_ITEM_H */
